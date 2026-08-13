/*
 * Now Playing for OBS
 * Copyright (C) 2026 OBS Now Playing contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include <obs-module.h>
#include <plugin-support.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <exception>
#include <memory>
#include <cmath>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Media.Control.h>
#endif

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(PLUGIN_NAME, "en-US")

namespace {

constexpr const char *SOURCE_ID = "obs_now_playing_source";

#ifdef _WIN32

using SessionManager = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionManager;
using MediaSession = winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSession;

struct SessionOption {
	std::string id;
	std::string label;
};

std::mutex g_sessions_mutex;
std::vector<SessionOption> g_sessions;
std::vector<SessionOption> g_windows;

std::string to_utf8(const winrt::hstring &value)
{
	return winrt::to_string(value);
}

std::wstring to_wide(const std::string &value)
{
	if (value.empty())
		return {};

	const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
	if (size <= 0)
		return {};

	std::wstring result(static_cast<size_t>(size), L'\0');
	MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size);
	return result;
}

std::string window_text(HWND window)
{
	const int length = GetWindowTextLengthW(window);
	if (length <= 0)
		return {};

	std::wstring title(static_cast<size_t>(length + 1), L'\0');
	const int copied = GetWindowTextW(window, title.data(), length + 1);
	if (copied <= 0)
		return {};

	title.resize(static_cast<size_t>(copied));
	return winrt::to_string(title);
}

std::string process_name(HWND window)
{
	DWORD process_id = 0;
	GetWindowThreadProcessId(window, &process_id);
	if (!process_id)
		return {};

	std::wstring path(32768, L'\0');
	HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id);
	if (!process)
		return {};

	DWORD path_size = static_cast<DWORD>(path.size());
	const bool success = QueryFullProcessImageNameW(process, 0, path.data(), &path_size) != FALSE;
	CloseHandle(process);
	if (!success)
		return {};

	path.resize(path_size);
	const size_t separator = path.find_last_of(L"\\/");
	const std::wstring name = separator == std::wstring::npos ? path : path.substr(separator + 1);
	return winrt::to_string(name);
}

DWORD window_process_id(HWND window)
{
	DWORD process_id = 0;
	GetWindowThreadProcessId(window, &process_id);
	return process_id;
}

bool is_browser(const std::string &name)
{
	return name == "chrome.exe" || name == "msedge.exe" || name == "firefox.exe" || name == "brave.exe" ||
	       name == "opera.exe" || name == "vivaldi.exe";
}

std::string window_class(HWND window)
{
	wchar_t buffer[256]{};
	const int copied = GetClassNameW(window, buffer, static_cast<int>(std::size(buffer)));
	return copied > 0 ? winrt::to_string(std::wstring_view(buffer, static_cast<size_t>(copied))) : std::string{};
}

std::string hex_encode(const std::string &value)
{
	static constexpr char digits[] = "0123456789ABCDEF";
	std::string result;
	result.reserve(value.size() * 2);
	for (const unsigned char byte : value) {
		result.push_back(digits[byte >> 4]);
		result.push_back(digits[byte & 0x0F]);
	}
	return result;
}

std::optional<std::string> hex_decode(const std::string &value)
{
	if ((value.size() & 1U) != 0)
		return std::nullopt;
	auto nibble = [](char character) -> int {
		if (character >= '0' && character <= '9')
			return character - '0';
		if (character >= 'A' && character <= 'F')
			return character - 'A' + 10;
		if (character >= 'a' && character <= 'f')
			return character - 'a' + 10;
		return -1;
	};

	std::string result;
	result.reserve(value.size() / 2);
	for (size_t index = 0; index < value.size(); index += 2) {
		const int high = nibble(value[index]);
		const int low = nibble(value[index + 1]);
		if (high < 0 || low < 0)
			return std::nullopt;
		result.push_back(static_cast<char>((high << 4) | low));
	}
	return result;
}

struct WindowRecord {
	HWND handle = nullptr;
	DWORD process_id = 0;
	std::string application;
	std::string class_name;
	std::string title;
};

BOOL CALLBACK collect_window_records(HWND window, LPARAM parameter) noexcept
{
	try {
		if (!IsWindowVisible(window) || GetWindow(window, GW_OWNER) != nullptr)
			return TRUE;

		WindowRecord record;
		record.handle = window;
		record.process_id = window_process_id(window);
		record.title = window_text(window);
		record.application = process_name(window);
		record.class_name = window_class(window);
		if (record.title.empty() || record.application.empty() || record.class_name.empty())
			return TRUE;

		auto *records = reinterpret_cast<std::vector<WindowRecord> *>(parameter);
		records->push_back(std::move(record));
		return TRUE;
	} catch (...) {
		// Never allow a C++ exception to cross the Win32 callback boundary.
		return FALSE;
	}
}

std::vector<WindowRecord> enumerate_windows()
{
	std::vector<WindowRecord> records;
	records.reserve(32);
	EnumWindows(collect_window_records, reinterpret_cast<LPARAM>(&records));
	return records;
}

std::string make_window_id(const WindowRecord &record)
{
	const auto handle = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(record.handle));
	return "window3:" + std::to_string(handle) + ":" + std::to_string(record.process_id) + ":" +
	       hex_encode(record.application) + ":" +
	       hex_encode(record.class_name) + ":" + hex_encode(record.title);
}

struct SavedWindow {
	HWND handle = nullptr;
	DWORD process_id = 0;
	std::string application;
	std::string class_name;
	std::string title;
};

std::optional<SavedWindow> parse_window_id(const std::string &id)
{
	const bool version3 = id.rfind("window3:", 0) == 0;
	const bool version2 = id.rfind("window2:", 0) == 0;
	if (!version3 && !version2)
		return std::nullopt;

	const size_t handle_end = id.find(':', 8);
	const size_t process_end = version3 && handle_end != std::string::npos ? id.find(':', handle_end + 1) : handle_end;
	const size_t app_start = process_end == std::string::npos ? std::string::npos : process_end + 1;
	const size_t app_end = app_start == std::string::npos ? std::string::npos : id.find(':', app_start);
	const size_t class_end = app_end == std::string::npos ? std::string::npos : id.find(':', app_end + 1);
	if (handle_end == std::string::npos || process_end == std::string::npos || app_end == std::string::npos ||
	    class_end == std::string::npos)
		return std::nullopt;

	char *end = nullptr;
	const auto raw_handle = std::strtoull(id.c_str() + 8, &end, 10);
	if (!end || end != id.c_str() + handle_end)
		return std::nullopt;
	DWORD process_id = 0;
	if (version3) {
		char *process_id_end = nullptr;
		const unsigned long parsed_process_id = std::strtoul(id.c_str() + handle_end + 1, &process_id_end, 10);
		if (!process_id_end || process_id_end != id.c_str() + process_end || parsed_process_id > MAXDWORD)
			return std::nullopt;
		process_id = static_cast<DWORD>(parsed_process_id);
	}
	const auto application = hex_decode(id.substr(app_start, app_end - app_start));
	const auto class_name = hex_decode(id.substr(app_end + 1, class_end - app_end - 1));
	const auto title = hex_decode(id.substr(class_end + 1));
	if (!application || !class_name || !title || application->empty() || class_name->empty())
		return std::nullopt;

	return SavedWindow{reinterpret_cast<HWND>(static_cast<uintptr_t>(raw_handle)), process_id, *application,
		*class_name, *title};
}

bool window_matches(HWND window, const SavedWindow &saved)
{
	return IsWindow(window) && (!saved.process_id || window_process_id(window) == saved.process_id) &&
	       process_name(window) == saved.application && window_class(window) == saved.class_name;
}

HWND resolve_window(const std::string &id)
{
	if (const auto saved = parse_window_id(id)) {
		if (window_matches(saved->handle, *saved))
			return saved->handle;

		const auto records = enumerate_windows();
		for (const auto &record : records) {
			if (record.application != saved->application || record.class_name != saved->class_name)
				continue;
			if (record.title == saved->title)
				return record.handle;
		}
		// After an application restart, only an exact title match is safe.
		// If the title changed, ask the user to select the window again rather
		// than risk exposing a different window from the same application.
		return nullptr;
	}

	// Legacy IDs are accepted for migration only. New selections carry a process
	// id, executable and class signature, and never guess after an app restart.
	if (id.rfind("window:", 0) == 0) {
		const auto raw_handle = std::strtoull(id.c_str() + 7, nullptr, 10);
		const HWND window = reinterpret_cast<HWND>(static_cast<uintptr_t>(raw_handle));
		return IsWindow(window) ? window : nullptr;
	}
	return nullptr;
}

std::string describe_saved_window(const std::string &id)
{
	if (const auto saved = parse_window_id(id)) {
		if (const HWND resolved = resolve_window(id))
			return saved->application + " | " + window_text(resolved);
		return saved->application + " | " + saved->title;
	}
	return id;
}

std::string upgrade_window_id(const std::string &id)
{
	if (id.rfind("window:", 0) != 0 && id.rfind("window2:", 0) != 0)
		return id;
	const HWND window = resolve_window(id);
	if (!window)
		return id;
	WindowRecord record{window, window_process_id(window), process_name(window), window_class(window),
		window_text(window)};
	return record.application.empty() || record.class_name.empty() ? id : make_window_id(record);
}

std::string clean_browser_title(std::string title)
{
	static constexpr const char *suffixes[] = {
		" - Google Chrome", " - Microsoft Edge", " — Mozilla Firefox", " - Mozilla Firefox",
		" - Brave",         " - Opera",          " - Vivaldi",          " - YouTube",
		" - Twitch",        " | YouTube",        " | Twitch",
	};

	bool removed = true;
	while (removed) {
		removed = false;
		for (const char *suffix : suffixes) {
			const size_t length = std::strlen(suffix);
			if (title.size() >= length && title.compare(title.size() - length, length, suffix) == 0) {
				title.resize(title.size() - length);
				removed = true;
				break;
			}
		}
	}
	return title;
}

void replace_all(std::string &value, const std::string &needle, const std::string &replacement)
{
	if (needle.empty())
		return;
	for (size_t position = 0; (position = value.find(needle, position)) != std::string::npos;
	     position += replacement.size())
		value.replace(position, needle.size(), replacement);
}

std::string format_media_text(const std::string &format, const std::string &title, const std::string &artist)
{
	std::string result = format;
	if (artist.empty()) {
		replace_all(result, "{artist} — ", "");
		replace_all(result, "{artist} - ", "");
	}
	if (title.empty()) {
		replace_all(result, " — {title}", "");
		replace_all(result, " - {title}", "");
	}
	replace_all(result, "{artist}", artist);
	replace_all(result, "{title}", title);
	return result;
}

template<typename AsyncOperation>
auto await_with_timeout(const AsyncOperation &operation, std::chrono::milliseconds timeout)
{
	using winrt::Windows::Foundation::AsyncStatus;
	if (operation.wait_for(timeout) == AsyncStatus::Started) {
		operation.Cancel();
		throw winrt::hresult_error(HRESULT_FROM_WIN32(ERROR_TIMEOUT), L"Windows Media Control timed out");
	}
	return operation.GetResults();
}

struct ApartmentScope {
	ApartmentScope()
	{
		winrt::init_apartment(winrt::apartment_type::multi_threaded);
	}
	~ApartmentScope()
	{
		winrt::uninit_apartment();
	}
	ApartmentScope(const ApartmentScope &) = delete;
	ApartmentScope &operator=(const ApartmentScope &) = delete;
};

int read_clamped_int(obs_data_t *settings, const char *name, int minimum, int maximum)
{
	const long long value = obs_data_get_int(settings, name);
	if (value < minimum)
		return minimum;
	if (value > maximum)
		return maximum;
	return static_cast<int>(value);
}

bool apply_selected_preset(obs_data_t *settings);

std::optional<uint32_t> parse_style_color(const char *value)
{
	if (!value)
		return std::nullopt;
	std::string text(value);
	if (!text.empty() && text.front() == '#')
		text.erase(text.begin());
	if (text.size() != 6 && text.size() != 8)
		return std::nullopt;

	uint32_t rgba = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), rgba, 16);
	if (result.ec != std::errc{} || result.ptr != text.data() + text.size())
		return std::nullopt;
	if (text.size() == 6)
		rgba = (rgba << 8) | 0xFF;

	const uint32_t red = (rgba >> 24) & 0xFF;
	const uint32_t green = (rgba >> 16) & 0xFF;
	const uint32_t blue = (rgba >> 8) & 0xFF;
	const uint32_t alpha = rgba & 0xFF;
	return (alpha << 24) | (blue << 16) | (green << 8) | red;
}

void apply_custom_style_json(obs_data_t *settings)
{
	const char *code = obs_data_get_string(settings, "style_code");
	if (!code || !*code)
		return;

	obs_data_t *raw_style = obs_data_create_from_json(code);
	if (!raw_style)
		return;
	auto style = std::unique_ptr<obs_data_t, decltype(&obs_data_release)>(raw_style, obs_data_release);

	auto copy_string = [&](const char *json_name, const char *setting_name, size_t maximum_length) {
		if (!obs_data_has_user_value(style.get(), json_name))
			return;
		std::string value = obs_data_get_string(style.get(), json_name);
		if (value.size() <= maximum_length)
			obs_data_set_string(settings, setting_name, value.c_str());
	};
	auto copy_int = [&](const char *json_name, const char *setting_name) {
		if (obs_data_has_user_value(style.get(), json_name))
			obs_data_set_int(settings, setting_name, obs_data_get_int(style.get(), json_name));
	};
	auto copy_bool = [&](const char *json_name, const char *setting_name) {
		if (obs_data_has_user_value(style.get(), json_name))
			obs_data_set_bool(settings, setting_name, obs_data_get_bool(style.get(), json_name));
	};
	auto copy_color = [&](const char *json_name, const char *setting_name) {
		if (!obs_data_has_user_value(style.get(), json_name))
			return;
		if (const auto color_value = parse_style_color(obs_data_get_string(style.get(), json_name)))
			obs_data_set_int(settings, setting_name, *color_value);
		else
			obs_log(LOG_WARNING, "Custom style field '%s' must use #RRGGBB or #RRGGBBAA", json_name);
	};

	copy_string("font", "font", 128);
	copy_string("text_format", "text_format", 512);
	copy_int("font_size", "font_size");
	copy_int("fade_duration_ms", "fade_duration_ms");
	copy_int("scroll_speed", "scroll_speed");
	copy_int("border_width", "border_width");
	copy_int("corner_radius", "corner_radius");
	copy_int("horizontal_padding", "horizontal_padding");
	copy_int("width", "width");
	copy_int("height", "height");
	copy_bool("show_artist", "show_artist");
	copy_bool("hide_when_paused", "hide_when_paused");
	copy_bool("show_card", "show_card");
	copy_color("text_color", "color");
	copy_color("background_color", "background_color");
	copy_color("border_color", "border_color");

	if (obs_data_has_user_value(style.get(), "font_weight")) {
		const std::string value = obs_data_get_string(style.get(), "font_weight");
		if (value == "normal")
			obs_data_set_int(settings, "font_weight", FW_NORMAL);
		else if (value == "semibold")
			obs_data_set_int(settings, "font_weight", FW_SEMIBOLD);
		else if (value == "bold")
			obs_data_set_int(settings, "font_weight", FW_BOLD);
	}
	if (obs_data_has_user_value(style.get(), "alignment")) {
		const std::string value = obs_data_get_string(style.get(), "alignment");
		if (value == "left")
			obs_data_set_int(settings, "text_alignment", 0);
		else if (value == "center")
			obs_data_set_int(settings, "text_alignment", 1);
		else if (value == "right")
			obs_data_set_int(settings, "text_alignment", 2);
	}

	obs_data_set_string(settings, "preset", "custom");
	obs_data_set_string(settings, "preset_applied", "custom");
}

struct NowPlayingSource {
	obs_source_t *source = nullptr;
	std::atomic_bool running{true};
	std::thread worker;
	std::mutex mutex;
	std::mutex render_mutex;
	std::condition_variable wake_worker;
	bool wake_requested = false;

	std::string source_mode = "auto";
	std::string selected_session = "auto";
	std::string selected_window;
	std::string font = "Segoe UI";
	std::string text_format = "{artist} — {title}";
	uint32_t color = 0xFFFFFFFF;
	uint32_t background_color = 0xD0201010;
	uint32_t border_color = 0xFFFFC878;
	int font_size = 36;
	int canvas_width = 800;
	int canvas_height = 120;
	int border_width = 3;
	int corner_radius = 24;
	int horizontal_padding = 20;
	int font_weight = FW_SEMIBOLD;
	int text_alignment = 0;
	int fade_duration_ms = 250;
	int scroll_speed = 60;
	bool show_artist = true;
	bool show_card = false;
	bool hide_when_paused = true;
	std::atomic_bool showing{true};
	uint64_t settings_generation = 0;
	uint64_t rendered_settings_generation = UINT64_MAX;
	std::string rendered_title;
	std::string rendered_artist;
	float current_opacity = 0.0f;
	bool visible_target = false;
	bool marquee_required = false;
	int marquee_distance = 0;
	float marquee_elapsed = 0.0f;
	float marquee_frame_accumulator = 0.0f;
	std::chrono::steady_clock::time_point last_session_options_refresh{};
	std::chrono::steady_clock::time_point last_window_options_refresh{};

	std::vector<uint8_t> pixels;
	int bitmap_width = 0;
	int bitmap_height = 0;
	uint64_t bitmap_generation = 0;
	uint64_t texture_generation = 0;
	gs_texture_t *texture = nullptr;
	gs_effect_t *draw_effect = nullptr;
	int texture_width = 0;
	int texture_height = 0;
	bool texture_failure_logged = false;
	std::chrono::steady_clock::time_point next_texture_retry{};
	MediaSession cached_session{nullptr};
	std::string cached_session_setting;

	explicit NowPlayingSource(obs_source_t *source_) : source(source_), showing(obs_source_showing(source_))
	{
	}

	bool start()
	{
		char *effect_path = obs_module_file("now-playing.effect");
		if (!effect_path) {
			obs_log(LOG_ERROR, "Could not locate now-playing.effect");
			return false;
		}

		char *effect_error = nullptr;
		obs_enter_graphics();
		draw_effect = gs_effect_create_from_file(effect_path, &effect_error);
		obs_leave_graphics();
		bfree(effect_path);
		if (!draw_effect) {
			obs_log(LOG_ERROR, "Could not load now-playing.effect: %s", effect_error ? effect_error : "unknown error");
			bfree(effect_error);
			return false;
		}
		bfree(effect_error);

		worker = std::thread([this] { poll_media(); });
		return true;
	}

	void set_showing(bool value)
	{
		showing = value;
		{
			std::lock_guard lock(mutex);
			wake_requested = true;
		}
		wake_worker.notify_all();
	}

	~NowPlayingSource()
	{
		running = false;
		{
			std::lock_guard lock(mutex);
			wake_requested = true;
		}
		wake_worker.notify_all();
		if (worker.joinable())
			worker.join();

		if (texture || draw_effect) {
			obs_enter_graphics();
			if (texture)
				gs_texture_destroy(texture);
			if (draw_effect)
				gs_effect_destroy(draw_effect);
			obs_leave_graphics();
		}
	}

	void update(obs_data_t *settings)
	{
		apply_selected_preset(settings);
		apply_custom_style_json(settings);
		const std::string legacy_session = obs_data_get_string(settings, "session");
		const char *configured_mode = obs_data_get_string(settings, "source_mode");
		if ((!obs_data_has_user_value(settings, "source_mode") || !configured_mode || !*configured_mode) &&
		    legacy_session.rfind("window:", 0) == 0) {
			obs_data_set_string(settings, "source_mode", "window");
			obs_data_set_string(settings, "window", legacy_session.c_str());
			obs_data_set_string(settings, "session", "auto");
		}

		std::string configured_window = obs_data_get_string(settings, "window");
		const std::string upgraded_window = upgrade_window_id(configured_window);
		if (upgraded_window != configured_window) {
			configured_window = upgraded_window;
			obs_data_set_string(settings, "window", configured_window.c_str());
		}

		const int safe_width = read_clamped_int(settings, "width", 100, 3840);
		const int safe_height = read_clamped_int(settings, "height", 40, 2160);
		const int max_inset = (std::max)(0, (std::min)(safe_width, safe_height) / 2);
		const int safe_border = read_clamped_int(settings, "border_width", 0, (std::min)(30, max_inset));
		const int safe_radius = read_clamped_int(settings, "corner_radius", 0, (std::min)(200, max_inset));
		const int safe_padding = read_clamped_int(settings, "horizontal_padding", 0,
			(std::min)(300, (std::max)(0, safe_width / 2 - 1)));
		const long long requested_weight = obs_data_get_int(settings, "font_weight");
		const int safe_weight = requested_weight == FW_NORMAL || requested_weight == FW_SEMIBOLD ||
			requested_weight == FW_BOLD
				? static_cast<int>(requested_weight)
				: FW_SEMIBOLD;

		obs_data_set_int(settings, "width", safe_width);
		obs_data_set_int(settings, "height", safe_height);
		obs_data_set_int(settings, "border_width", safe_border);
		obs_data_set_int(settings, "corner_radius", safe_radius);
		obs_data_set_int(settings, "horizontal_padding", safe_padding);
		obs_data_set_int(settings, "font_size", read_clamped_int(settings, "font_size", 10, 200));
		obs_data_set_int(settings, "font_weight", safe_weight);
		obs_data_set_int(settings, "text_alignment", read_clamped_int(settings, "text_alignment", 0, 2));
		obs_data_set_int(settings, "fade_duration_ms", read_clamped_int(settings, "fade_duration_ms", 0, 3000));
		obs_data_set_int(settings, "scroll_speed", read_clamped_int(settings, "scroll_speed", 10, 300));

		{
			std::lock_guard lock(mutex);
			source_mode = obs_data_get_string(settings, "source_mode");
			selected_session = obs_data_get_string(settings, "session");
			selected_window = configured_window;
			if (source_mode != "auto" && source_mode != "smtc" && source_mode != "window")
				source_mode = "auto";
			font = obs_data_get_string(settings, "font");
			if (font.empty())
				font = "Segoe UI";
			text_format = obs_data_get_string(settings, "text_format");
			color = static_cast<uint32_t>(obs_data_get_int(settings, "color"));
			background_color = static_cast<uint32_t>(obs_data_get_int(settings, "background_color"));
			border_color = static_cast<uint32_t>(obs_data_get_int(settings, "border_color"));
			font_size = static_cast<int>(obs_data_get_int(settings, "font_size"));
			font_weight = static_cast<int>(obs_data_get_int(settings, "font_weight"));
			text_alignment = static_cast<int>(obs_data_get_int(settings, "text_alignment"));
			fade_duration_ms = static_cast<int>(obs_data_get_int(settings, "fade_duration_ms"));
			scroll_speed = static_cast<int>(obs_data_get_int(settings, "scroll_speed"));
			canvas_width = static_cast<int>(obs_data_get_int(settings, "width"));
			canvas_height = static_cast<int>(obs_data_get_int(settings, "height"));
			border_width = static_cast<int>(obs_data_get_int(settings, "border_width"));
			corner_radius = static_cast<int>(obs_data_get_int(settings, "corner_radius"));
			horizontal_padding = static_cast<int>(obs_data_get_int(settings, "horizontal_padding"));
			show_artist = obs_data_get_bool(settings, "show_artist");
			show_card = obs_data_get_bool(settings, "show_card");
			hide_when_paused = obs_data_get_bool(settings, "hide_when_paused");
			++settings_generation;
			wake_requested = true;
		}
		wake_worker.notify_all();
	}

	void refresh_window_options(std::chrono::steady_clock::time_point now)
	{
		if (now - last_window_options_refresh < std::chrono::seconds(3))
			return;

		std::vector<SessionOption> windows;
		for (const auto &record : enumerate_windows())
			windows.push_back({make_window_id(record), record.application + " | " + record.title});
		{
			std::lock_guard lock(g_sessions_mutex);
			g_windows = std::move(windows);
		}
		last_window_options_refresh = now;
	}

	std::string read_window_title(const std::string &window_id)
	{
		const HWND window = resolve_window(window_id);
		if (!window)
			return {};
		std::string title = window_text(window);
		if (is_browser(process_name(window)))
			title = clean_browser_title(std::move(title));
		return title;
	}

	void refresh_window_only()
	{
		std::string wanted_window;
		{
			std::lock_guard lock(mutex);
			wanted_window = selected_window;
		}
		refresh_window_options(std::chrono::steady_clock::now());
		apply_media(read_window_title(wanted_window), {});
	}

	void poll_media()
	{
		try {
			ApartmentScope apartment;
			SessionManager manager{nullptr};
			auto retry_delay = std::chrono::seconds(1);

			while (running) {
				auto wait_time = std::chrono::milliseconds(750);
				std::string mode;
				bool hide_paused;
				{
					std::lock_guard lock(mutex);
					mode = source_mode;
					hide_paused = hide_when_paused;
				}
				const bool window_mode = mode == "window";
				try {
					if (window_mode && !hide_paused) {
						refresh_window_only();
					} else {
						if (!manager)
							manager = await_with_timeout(SessionManager::RequestAsync(), std::chrono::seconds(3));
						if (!running)
							break;
						refresh(manager);
					}
					retry_delay = std::chrono::seconds(1);
				} catch (const winrt::hresult_error &error) {
					obs_log(LOG_WARNING, "Windows Media Control will retry after an error: %s",
						to_utf8(error.message()).c_str());
					manager = nullptr;
					if (window_mode)
						refresh_window_only();
					else
						apply_media({}, {});
					wait_time = retry_delay;
					retry_delay = (std::min)(retry_delay * 2, std::chrono::seconds(30));
				} catch (const std::exception &error) {
					obs_log(LOG_WARNING, "Now Playing worker will retry after an error: %s", error.what());
					apply_media({}, {});
					wait_time = retry_delay;
					retry_delay = (std::min)(retry_delay * 2, std::chrono::seconds(30));
				} catch (...) {
					obs_log(LOG_WARNING, "Now Playing worker will retry after an unknown error");
					apply_media({}, {});
					wait_time = retry_delay;
					retry_delay = (std::min)(retry_delay * 2, std::chrono::seconds(30));
				}
				if (!showing)
					wait_time = (std::max)(wait_time, std::chrono::milliseconds(10000));
				std::unique_lock lock(mutex);
				wake_worker.wait_for(lock, wait_time, [this] { return wake_requested || !running; });
				wake_requested = false;
			}
		} catch (const winrt::hresult_error &error) {
			obs_log(LOG_ERROR, "Windows Media Control failed: %s", to_utf8(error.message()).c_str());
		} catch (const std::exception &error) {
			obs_log(LOG_ERROR, "Windows Media Control worker could not start: %s", error.what());
		} catch (...) {
			obs_log(LOG_ERROR, "Windows Media Control worker could not start");
		}
		apply_media({}, {});
	}

	void refresh(const SessionManager &manager)
	{
		const auto sessions = manager.GetSessions();
		const auto now = std::chrono::steady_clock::now();
		const bool refresh_session_options = now - last_session_options_refresh >= std::chrono::seconds(3);
		std::vector<SessionOption> options;
		if (refresh_session_options)
			options.reserve(sessions.Size() + 8);
		std::unordered_map<std::string, size_t> app_occurrences;

		std::string wanted;
		std::string wanted_window;
		std::string mode;
		bool hide_paused;
		{
			std::lock_guard lock(mutex);
			wanted = selected_session;
			wanted_window = selected_window;
			mode = source_mode;
			hide_paused = hide_when_paused;
		}

		MediaSession chosen{nullptr};
		if (wanted != "auto" && cached_session && cached_session_setting == wanted) {
			for (const auto &session : sessions) {
				if (session == cached_session) {
					chosen = session;
					break;
				}
			}
			// Applications create a new SMTC object after they restart. If the
			// cached object disappeared, immediately resolve the saved app/slot
			// again instead of leaving the source permanently disconnected.
			if (!chosen) {
				cached_session = nullptr;
				cached_session_setting.clear();
			}
		} else {
			cached_session = nullptr;
			cached_session_setting.clear();
		}
		if (refresh_session_options || (wanted != "auto" && !chosen)) {
			for (const auto &session : sessions) {
				if (!running)
					return;
				const auto app_id = to_utf8(session.SourceAppUserModelId());
				const size_t occurrence = app_occurrences[app_id]++;
				const std::string option_id = app_id + "|" + std::to_string(occurrence);
				if (refresh_session_options) {
					std::string label = app_id + " [" + std::to_string(occurrence + 1) + "]";
					try {
						const auto properties = await_with_timeout(session.TryGetMediaPropertiesAsync(),
							std::chrono::milliseconds(750));
						const auto title = to_utf8(properties.Title());
						if (!title.empty())
							label += " - " + title;
					} catch (...) {
					}
					options.push_back({option_id, std::move(label)});
				}

				const bool legacy_match = wanted == app_id && occurrence == 0;
				if (wanted != "auto" && (wanted == option_id || legacy_match) && !chosen) {
					chosen = session;
					cached_session = session;
					cached_session_setting = wanted;
				}
			}
		}

		if (refresh_session_options) {
			{
				std::lock_guard lock(g_sessions_mutex);
				g_sessions = std::move(options);
			}
			last_session_options_refresh = now;
		}
		if (mode != "smtc")
			refresh_window_options(now);

		std::string title;
		std::string artist;
		auto read_window = [&](const std::string &window_id) {
			title = read_window_title(window_id);

			if (hide_paused && !title.empty()) {
				bool matched_media_session = false;
				bool is_playing = false;
				for (const auto &session : sessions) {
					if (!running)
						return;
					try {
						const auto properties = await_with_timeout(session.TryGetMediaPropertiesAsync(),
							std::chrono::milliseconds(750));
						const std::string media_title = to_utf8(properties.Title());
						if (media_title.empty() || (media_title != title && title.find(media_title) == std::string::npos &&
							media_title.find(title) == std::string::npos))
							continue;
						matched_media_session = true;
						is_playing = session.GetPlaybackInfo().PlaybackStatus() ==
							     winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
						break;
					} catch (...) {
					}
				}
				if (matched_media_session && !is_playing)
					title.clear();
			}
		};

		if (mode == "window") {
			read_window(wanted_window);
			apply_media(title, artist);
			return;
		}

		if (!chosen && wanted == "auto")
			chosen = manager.GetCurrentSession();

		bool smtc_has_metadata = false;
		bool smtc_paused = false;
		if (chosen) {
			try {
				bool is_playing = true;
				if (hide_paused) {
					is_playing = chosen.GetPlaybackInfo().PlaybackStatus() ==
						winrt::Windows::Media::Control::GlobalSystemMediaTransportControlsSessionPlaybackStatus::Playing;
					smtc_paused = !is_playing;
				}
				if (is_playing) {
					const auto properties = await_with_timeout(chosen.TryGetMediaPropertiesAsync(),
						std::chrono::milliseconds(1500));
					title = to_utf8(properties.Title());
					artist = to_utf8(properties.Artist());
					smtc_has_metadata = !title.empty() || !artist.empty();
				}
			} catch (...) {
			}
		}

		if (mode == "auto" && !smtc_has_metadata && !smtc_paused)
			read_window(wanted_window);

		apply_media(title, artist);
	}

	void apply_media(const std::string &title, const std::string &artist)
	{
		bool has_content;
		{
			std::lock_guard lock(mutex);
			const std::string visible_artist = show_artist ? artist : std::string{};
			has_content = (!title.empty() || !visible_artist.empty()) &&
				!format_media_text(text_format, title, visible_artist).empty();
		}
		if (has_content) {
			bool needs_render;
			{
				std::lock_guard lock(mutex);
				needs_render = title != rendered_title || artist != rendered_artist ||
					       settings_generation != rendered_settings_generation;
			}
			if (needs_render) {
				{
					std::lock_guard lock(mutex);
					marquee_elapsed = 0.0f;
					marquee_frame_accumulator = 0.0f;
				}
				render_bitmap(title, artist, 0);
			}
		}

		std::lock_guard lock(mutex);
		visible_target = has_content;
	}

	void render_bitmap(const std::string &title, const std::string &artist, int scroll_offset)
	{
		std::lock_guard render_lock(render_mutex);
		int width;
		int height;
		int size;
		uint32_t text_color;
		uint32_t card_color;
		uint32_t outline_color;
		bool include_artist;
		bool include_card;
		std::string font_name;
		int outline_width;
		int radius;
		int padding;
		int weight;
		int alignment;
		uint64_t generation;
		std::string format;
		{
			std::lock_guard lock(mutex);
			width = canvas_width;
			height = canvas_height;
			size = font_size;
			text_color = color;
			card_color = background_color;
			outline_color = border_color;
			include_artist = show_artist;
			include_card = show_card;
			font_name = font;
			outline_width = border_width;
			radius = corner_radius;
			padding = horizontal_padding;
			weight = font_weight;
			alignment = text_alignment;
			generation = settings_generation;
			format = text_format;
		}

		const std::string visible_artist = include_artist ? artist : std::string{};
		const std::wstring text = to_wide(format_media_text(format, title, visible_artist));
		const std::wstring font_w = to_wide(font_name);
		const size_t pixel_count = static_cast<size_t>(width) * static_cast<size_t>(height);
		std::vector<uint8_t> text_mask(pixel_count);
		std::vector<uint8_t> output(pixel_count * 4);
		std::vector<uint8_t> outer_mask(include_card ? pixel_count : 0);
		std::vector<uint8_t> inner_mask(include_card ? pixel_count : 0);
		std::vector<uint8_t> border_mask(include_card ? pixel_count : 0);

		BITMAPINFO info{};
		info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
		info.bmiHeader.biWidth = width;
		info.bmiHeader.biHeight = -height;
		info.bmiHeader.biPlanes = 1;
		info.bmiHeader.biBitCount = 32;
		info.bmiHeader.biCompression = BI_RGB;

		HDC dc = CreateCompatibleDC(nullptr);
		if (!dc)
			return;

		void *bits = nullptr;
		HBITMAP bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
		if (!bitmap || !bits) {
			if (bitmap)
				DeleteObject(bitmap);
			DeleteDC(dc);
			return;
		}

		HGDIOBJ old_bitmap = SelectObject(dc, bitmap);
		if (!old_bitmap || old_bitmap == HGDI_ERROR) {
			DeleteObject(bitmap);
			DeleteDC(dc);
			return;
		}
		RECT rect{0, 0, width, height};
		SetBkMode(dc, TRANSPARENT);
		SetTextColor(dc, RGB(255, 255, 255));

		auto clear_bitmap = [&] { FillRect(dc, &rect, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH))); };
		auto read_mask = [&](std::vector<uint8_t> &mask) {
			GdiFlush();
			auto *dib = static_cast<uint8_t *>(bits);
			for (size_t pixel = 0; pixel < mask.size(); ++pixel)
				mask[pixel] = dib[pixel * 4];
		};

		if (include_card) {
			const int safe_radius = (std::max)(0, (std::min)(radius, (std::min)(width, height) / 2));
			const int safe_border = (std::max)(0, (std::min)(outline_width, (std::min)(width, height) / 2));
			HBRUSH white_brush = CreateSolidBrush(RGB(255, 255, 255));
			if (white_brush) {
				HGDIOBJ old_brush = SelectObject(dc, white_brush);
				HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));

				clear_bitmap();
				RoundRect(dc, 0, 0, width, height, safe_radius * 2, safe_radius * 2);
				read_mask(outer_mask);

				clear_bitmap();
				const int inner_radius = (std::max)(0, safe_radius - safe_border);
				RoundRect(dc, safe_border, safe_border, width - safe_border, height - safe_border,
					inner_radius * 2, inner_radius * 2);
				read_mask(inner_mask);

				SelectObject(dc, old_pen);
				SelectObject(dc, old_brush);
				DeleteObject(white_brush);
			}
		}

		HFONT draw_font = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
			OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, ANTIALIASED_QUALITY, DEFAULT_PITCH | FF_DONTCARE,
			font_w.empty() ? L"Segoe UI" : font_w.c_str());
		HGDIOBJ font_to_use = draw_font ? static_cast<HGDIOBJ>(draw_font) : GetStockObject(DEFAULT_GUI_FONT);
		HGDIOBJ old_font = SelectObject(dc, font_to_use);
		clear_bitmap();
		const int safe_padding = (std::max)(0, padding);
		const int available_width = (std::max)(1, width - safe_padding * 2);
		SIZE text_extent{};
		GetTextExtentPoint32W(dc, text.c_str(), static_cast<int>(text.size()), &text_extent);
		const bool needs_marquee = text_extent.cx > available_width;
		constexpr int marquee_gap = 80;
		const int scroll_distance = needs_marquee ? text_extent.cx + marquee_gap : 0;
		RECT clip_rect{safe_padding, 0, width - safe_padding, height};
		const int saved_dc = SaveDC(dc);
		IntersectClipRect(dc, clip_rect.left, clip_rect.top, clip_rect.right, clip_rect.bottom);
		UINT draw_flags = DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX;
		if (needs_marquee) {
			RECT first{safe_padding - scroll_offset, 0, safe_padding - scroll_offset + text_extent.cx + 1, height};
			DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &first, draw_flags | DT_LEFT);
			RECT second{first.left + scroll_distance, 0, first.right + scroll_distance, height};
			DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &second, draw_flags | DT_LEFT);
		} else {
			RECT text_rect{safe_padding, 0, width - safe_padding, height};
			draw_flags |= alignment == 1 ? DT_CENTER : alignment == 2 ? DT_RIGHT : DT_LEFT;
			DrawTextW(dc, text.c_str(), static_cast<int>(text.size()), &text_rect, draw_flags);
		}
		if (saved_dc)
			RestoreDC(dc, saved_dc);
		read_mask(text_mask);

		auto blend_layer = [&](uint32_t layer_color, const std::vector<uint8_t> &mask) {
			const uint8_t red = static_cast<uint8_t>(layer_color & 0xFF);
			const uint8_t green = static_cast<uint8_t>((layer_color >> 8) & 0xFF);
			const uint8_t blue = static_cast<uint8_t>((layer_color >> 16) & 0xFF);
			const uint8_t opacity = static_cast<uint8_t>((layer_color >> 24) & 0xFF);
			for (size_t pixel = 0; pixel < mask.size(); ++pixel) {
				const uint16_t source_alpha = static_cast<uint16_t>(opacity) * mask[pixel] / 255;
				const uint16_t inverse_alpha = 255 - source_alpha;
				const size_t i = pixel * 4;
				output[i] = static_cast<uint8_t>((static_cast<uint16_t>(blue) * source_alpha / 255) +
					(static_cast<uint16_t>(output[i]) * inverse_alpha / 255));
				output[i + 1] = static_cast<uint8_t>((static_cast<uint16_t>(green) * source_alpha / 255) +
					(static_cast<uint16_t>(output[i + 1]) * inverse_alpha / 255));
				output[i + 2] = static_cast<uint8_t>((static_cast<uint16_t>(red) * source_alpha / 255) +
					(static_cast<uint16_t>(output[i + 2]) * inverse_alpha / 255));
				output[i + 3] = static_cast<uint8_t>(source_alpha +
					(static_cast<uint16_t>(output[i + 3]) * inverse_alpha / 255));
			}
		};

		if (include_card) {
			blend_layer(card_color, outer_mask);
			for (size_t pixel = 0; pixel < border_mask.size(); ++pixel)
				border_mask[pixel] = outer_mask[pixel] > inner_mask[pixel]
					? static_cast<uint8_t>(outer_mask[pixel] - inner_mask[pixel])
					: 0;
			blend_layer(outline_color, border_mask);
		}
		blend_layer(text_color, text_mask);

		if (old_font && old_font != HGDI_ERROR)
			SelectObject(dc, old_font);
		SelectObject(dc, old_bitmap);
		if (draw_font)
			DeleteObject(draw_font);
		DeleteObject(bitmap);
		DeleteDC(dc);

		{
			std::lock_guard lock(mutex);
			pixels = std::move(output);
			bitmap_width = width;
			bitmap_height = height;
			rendered_title = title;
			rendered_artist = artist;
			rendered_settings_generation = generation;
			marquee_required = needs_marquee;
			marquee_distance = scroll_distance;
			++bitmap_generation;
		}
	}

	void video_tick(float seconds)
	{
		std::string title_to_render;
		std::string artist_to_render;
		int offset_to_render = 0;
		bool update_marquee = false;
		{
			std::lock_guard lock(mutex);
			const float target = visible_target ? 1.0f : 0.0f;
			if (fade_duration_ms <= 0) {
				current_opacity = target;
			} else {
				const float step = seconds * 1000.0f / static_cast<float>(fade_duration_ms);
				if (current_opacity < target)
					current_opacity = (std::min)(target, current_opacity + step);
				else if (current_opacity > target)
					current_opacity = (std::max)(target, current_opacity - step);
			}

			if (showing && visible_target && marquee_required && marquee_distance > 0) {
				marquee_elapsed += seconds;
				marquee_frame_accumulator += seconds;
				const long long pixel_area = static_cast<long long>(canvas_width) * canvas_height;
				const float frame_rate = pixel_area <= 1000000 ? 30.0f : pixel_area <= 3000000 ? 20.0f : 10.0f;
				if (marquee_frame_accumulator >= 1.0f / frame_rate) {
					marquee_frame_accumulator = 0.0f;
					constexpr float pause_seconds = 1.25f;
					const float travel_seconds = static_cast<float>(marquee_distance) /
						static_cast<float>((std::max)(10, scroll_speed));
					const float cycle = pause_seconds + travel_seconds;
					const float phase = cycle > 0.0f ? std::fmod(marquee_elapsed, cycle) : 0.0f;
					offset_to_render = phase <= pause_seconds ? 0 : static_cast<int>(
						(phase - pause_seconds) * static_cast<float>(scroll_speed));
					title_to_render = rendered_title;
					artist_to_render = rendered_artist;
					update_marquee = true;
				}
			} else {
				marquee_elapsed = 0.0f;
				marquee_frame_accumulator = 0.0f;
			}
		}
		if (update_marquee)
			render_bitmap(title_to_render, artist_to_render, offset_to_render);
	}

	void video_render(gs_effect_t *)
	{
		std::lock_guard lock(mutex);
		if (pixels.empty() || current_opacity <= 0.0f || !draw_effect)
			return;

		if (texture && (texture_width != bitmap_width || texture_height != bitmap_height)) {
			gs_texture_destroy(texture);
			texture = nullptr;
			texture_failure_logged = false;
			next_texture_retry = {};
		}

		if (!texture) {
			const auto now = std::chrono::steady_clock::now();
			if (texture_failure_logged && texture_generation == bitmap_generation && now < next_texture_retry)
				return;
			const uint8_t *data = pixels.data();
			texture = gs_texture_create(static_cast<uint32_t>(bitmap_width), static_cast<uint32_t>(bitmap_height),
				GS_BGRA, 1, &data, GS_DYNAMIC);
			if (!texture) {
				texture_generation = bitmap_generation;
				next_texture_retry = now + std::chrono::seconds(5);
				if (!texture_failure_logged)
					obs_log(LOG_ERROR, "Could not create the Now Playing texture (%dx%d)", bitmap_width,
						bitmap_height);
				texture_failure_logged = true;
				return;
			}
			texture_failure_logged = false;
			next_texture_retry = {};
			texture_width = bitmap_width;
			texture_height = bitmap_height;
			texture_generation = bitmap_generation;
		} else if (texture_generation != bitmap_generation) {
			gs_texture_set_image(texture, pixels.data(), static_cast<uint32_t>(bitmap_width * 4), false);
			texture_generation = bitmap_generation;
		}

		gs_eparam_t *image = gs_effect_get_param_by_name(draw_effect, "image");
		gs_eparam_t *opacity = gs_effect_get_param_by_name(draw_effect, "opacity");
		if (!image || !opacity)
			return;

		const bool previous_srgb = gs_framebuffer_srgb_enabled();
		gs_enable_framebuffer_srgb(true);
		gs_blend_state_push();
		gs_blend_function(GS_BLEND_ONE, GS_BLEND_INVSRCALPHA);
		gs_effect_set_texture_srgb(image, texture);
		gs_effect_set_float(opacity, current_opacity);
		while (gs_effect_loop(draw_effect, "Draw"))
			gs_draw_sprite(texture, 0, static_cast<uint32_t>(bitmap_width), static_cast<uint32_t>(bitmap_height));
		gs_blend_state_pop();
		gs_enable_framebuffer_srgb(previous_srgb);
	}
};

const char *source_name(void *)
{
	return obs_module_text("NowPlayingSource");
}

void *source_create(obs_data_t *settings, obs_source_t *source)
{
	try {
		auto context = std::make_unique<NowPlayingSource>(source);
		context->update(settings);
		if (!context->start())
			return nullptr;
		return context.release();
	} catch (const std::exception &error) {
		obs_log(LOG_ERROR, "Could not create the Now Playing source: %s", error.what());
	} catch (...) {
		obs_log(LOG_ERROR, "Could not create the Now Playing source");
	}
	return nullptr;
}

void source_destroy(void *data)
{
	delete static_cast<NowPlayingSource *>(data);
}

void source_update(void *data, obs_data_t *settings)
{
	try {
		static_cast<NowPlayingSource *>(data)->update(settings);
	} catch (const std::exception &error) {
		obs_log(LOG_ERROR, "Could not update the Now Playing source: %s", error.what());
	} catch (...) {
		obs_log(LOG_ERROR, "Could not update the Now Playing source");
	}
}

uint32_t source_width(void *data)
{
	auto *context = static_cast<NowPlayingSource *>(data);
	std::lock_guard lock(context->mutex);
	return static_cast<uint32_t>(context->canvas_width);
}

uint32_t source_height(void *data)
{
	auto *context = static_cast<NowPlayingSource *>(data);
	std::lock_guard lock(context->mutex);
	return static_cast<uint32_t>(context->canvas_height);
}

void source_render(void *data, gs_effect_t *effect)
{
	static_cast<NowPlayingSource *>(data)->video_render(effect);
}

void source_tick(void *data, float seconds)
{
	static_cast<NowPlayingSource *>(data)->video_tick(seconds);
}

void source_show(void *data)
{
	static_cast<NowPlayingSource *>(data)->set_showing(true);
}

void source_hide(void *data)
{
	static_cast<NowPlayingSource *>(data)->set_showing(false);
}

void source_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "source_mode", "auto");
	obs_data_set_default_string(settings, "session", "auto");
	obs_data_set_default_string(settings, "window", "");
	obs_data_set_default_string(settings, "font", "Segoe UI");
	obs_data_set_default_string(settings, "text_format", "{artist} — {title}");
	obs_data_set_default_string(settings, "preset", "custom");
	obs_data_set_default_string(settings, "preset_applied", "");
	obs_data_set_default_string(settings, "style_code", "");
	obs_data_set_default_int(settings, "font_size", 36);
	obs_data_set_default_int(settings, "font_weight", FW_SEMIBOLD);
	obs_data_set_default_int(settings, "text_alignment", 0);
	obs_data_set_default_int(settings, "fade_duration_ms", 250);
	obs_data_set_default_int(settings, "scroll_speed", 60);
	obs_data_set_default_int(settings, "width", 800);
	obs_data_set_default_int(settings, "height", 120);
	obs_data_set_default_int(settings, "color", 0xFFFFFFFF);
	obs_data_set_default_int(settings, "background_color", 0xD0201010);
	obs_data_set_default_int(settings, "border_color", 0xFFFFC878);
	obs_data_set_default_bool(settings, "show_artist", true);
	obs_data_set_default_bool(settings, "show_card", false);
	obs_data_set_default_bool(settings, "hide_when_paused", true);
	obs_data_set_default_int(settings, "border_width", 3);
	obs_data_set_default_int(settings, "corner_radius", 24);
	obs_data_set_default_int(settings, "horizontal_padding", 20);
}

bool source_mode_modified(obs_properties_t *properties, obs_property_t *, obs_data_t *settings)
{
	const char *mode = obs_data_get_string(settings, "source_mode");
	const bool show_smtc = !mode || strcmp(mode, "window") != 0;
	const bool show_window = !mode || strcmp(mode, "smtc") != 0;
	obs_property_set_visible(obs_properties_get(properties, "session"), show_smtc);
	obs_property_set_visible(obs_properties_get(properties, "window"), show_window);
	return true;
}

bool apply_selected_preset(obs_data_t *settings)
{
	const std::string preset = obs_data_get_string(settings, "preset");
	const char *applied = obs_data_get_string(settings, "preset_applied");
	if (preset.empty() || (applied && preset == applied))
		return false;
	if (preset == "custom") {
		obs_data_set_string(settings, "preset_applied", "custom");
		return false;
	}

	if (preset == "minimal") {
		obs_data_set_bool(settings, "show_card", false);
		obs_data_set_int(settings, "color", 0xFFFFFFFF);
		obs_data_set_int(settings, "font_weight", FW_NORMAL);
		obs_data_set_int(settings, "text_alignment", 0);
	} else if (preset == "dark") {
		obs_data_set_bool(settings, "show_card", true);
		obs_data_set_int(settings, "color", 0xFFFFFFFF);
		obs_data_set_int(settings, "background_color", 0xD0201010);
		obs_data_set_int(settings, "border_color", 0x80FFFFFF);
		obs_data_set_int(settings, "border_width", 2);
		obs_data_set_int(settings, "corner_radius", 24);
		obs_data_set_int(settings, "font_weight", FW_SEMIBOLD);
	} else if (preset == "neon") {
		obs_data_set_bool(settings, "show_card", true);
		obs_data_set_int(settings, "color", 0xFFFFFFFF);
		obs_data_set_int(settings, "background_color", 0xD0301020);
		obs_data_set_int(settings, "border_color", 0xFFFFC878);
		obs_data_set_int(settings, "border_width", 3);
		obs_data_set_int(settings, "corner_radius", 28);
		obs_data_set_int(settings, "font_weight", FW_BOLD);
	} else if (preset == "glass") {
		obs_data_set_bool(settings, "show_card", true);
		obs_data_set_int(settings, "color", 0xFFFFFFFF);
		obs_data_set_int(settings, "background_color", 0x70202020);
		obs_data_set_int(settings, "border_color", 0x70FFFFFF);
		obs_data_set_int(settings, "border_width", 2);
		obs_data_set_int(settings, "corner_radius", 32);
		obs_data_set_int(settings, "font_weight", FW_SEMIBOLD);
	} else if (preset == "spotify") {
		obs_data_set_bool(settings, "show_card", true);
		obs_data_set_int(settings, "color", 0xFFFFFFFF);
		obs_data_set_int(settings, "background_color", 0xE0181818);
		obs_data_set_int(settings, "border_color", 0xFF60D71E);
		obs_data_set_int(settings, "border_width", 3);
		obs_data_set_int(settings, "corner_radius", 24);
		obs_data_set_int(settings, "font_weight", FW_BOLD);
	} else if (preset == "youtube") {
		obs_data_set_bool(settings, "show_card", true);
		obs_data_set_int(settings, "color", 0xFFFFFFFF);
		obs_data_set_int(settings, "background_color", 0xE0201010);
		obs_data_set_int(settings, "border_color", 0xFF3300FF);
		obs_data_set_int(settings, "border_width", 3);
		obs_data_set_int(settings, "corner_radius", 18);
		obs_data_set_int(settings, "font_weight", FW_BOLD);
	} else if (preset == "purple") {
		obs_data_set_bool(settings, "show_card", true);
		obs_data_set_int(settings, "color", 0xFFFFFFFF);
		obs_data_set_int(settings, "background_color", 0xD0301828);
		obs_data_set_int(settings, "border_color", 0xFFFF4691);
		obs_data_set_int(settings, "border_width", 3);
		obs_data_set_int(settings, "corner_radius", 30);
		obs_data_set_int(settings, "font_weight", FW_SEMIBOLD);
	} else if (preset == "outline") {
		obs_data_set_bool(settings, "show_card", true);
		obs_data_set_int(settings, "color", 0xFFFFFFFF);
		obs_data_set_int(settings, "background_color", 0x00000000);
		obs_data_set_int(settings, "border_color", 0xFFFFFFFF);
		obs_data_set_int(settings, "border_width", 2);
		obs_data_set_int(settings, "corner_radius", 22);
		obs_data_set_int(settings, "font_weight", FW_NORMAL);
	}
	obs_data_set_string(settings, "preset_applied", preset.c_str());
	return true;
}

bool preset_modified(obs_properties_t *, obs_property_t *, obs_data_t *settings)
{
	return apply_selected_preset(settings);
}

obs_properties_t *source_properties_impl(void *data)
{
	auto properties_owner = std::unique_ptr<obs_properties_t, decltype(&obs_properties_destroy)>(
		obs_properties_create(), obs_properties_destroy);
	auto *properties = properties_owner.get();
	auto *mode = obs_properties_add_list(properties, "source_mode", obs_module_text("SourceMode"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(mode, obs_module_text("ModeAutomatic"), "auto");
	obs_property_list_add_string(mode, obs_module_text("ModeSMTC"), "smtc");
	obs_property_list_add_string(mode, obs_module_text("ModeWindow"), "window");
	obs_property_set_modified_callback(mode, source_mode_modified);

	auto *sessions = obs_properties_add_list(properties, "session", obs_module_text("Session"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(sessions, obs_module_text("AutomaticSession"), "auto");

	std::vector<SessionOption> options;
	std::vector<SessionOption> windows;
	{
		std::lock_guard lock(g_sessions_mutex);
		options = g_sessions;
		windows = g_windows;
	}
	for (const auto &option : options)
		obs_property_list_add_string(sessions, option.label.c_str(), option.id.c_str());

	if (data) {
		auto *context = static_cast<NowPlayingSource *>(data);
		std::string selected;
		{
			std::lock_guard lock(context->mutex);
			selected = context->selected_session;
		}
		bool found = selected == "auto";
		for (const auto &option : options)
			found = found || option.id == selected;
		if (!found && !selected.empty())
			obs_property_list_add_string(sessions, selected.c_str(), selected.c_str());
	}

	if (data && options.empty())
		obs_property_set_long_description(sessions, obs_module_text("SessionHint"));

	auto *window = obs_properties_add_list(properties, "window", obs_module_text("Window"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(window, obs_module_text("NoWindow"), "");
	for (const auto &option : windows)
		obs_property_list_add_string(window, option.label.c_str(), option.id.c_str());
	if (windows.empty())
		obs_property_set_long_description(window, obs_module_text("WindowHint"));

	if (data) {
		auto *context = static_cast<NowPlayingSource *>(data);
		std::string selected;
		std::string current_mode;
		{
			std::lock_guard lock(context->mutex);
			selected = context->selected_window;
			current_mode = context->source_mode;
		}
		bool found = selected.empty();
		for (const auto &option : windows) {
			found = found || option.id == selected;
			if (!found) {
				const auto saved = parse_window_id(selected);
				const auto candidate = parse_window_id(option.id);
				found = saved && candidate && saved->handle == candidate->handle &&
					(!saved->process_id || saved->process_id == candidate->process_id) &&
					saved->application == candidate->application && saved->class_name == candidate->class_name;
			}
		}
		if (!found)
			obs_property_list_add_string(window, describe_saved_window(selected).c_str(), selected.c_str());
		obs_property_set_visible(sessions, current_mode != "window");
		obs_property_set_visible(window, current_mode != "smtc");
	}

	auto *preset = obs_properties_add_list(properties, "preset", obs_module_text("Preset"), OBS_COMBO_TYPE_LIST,
		OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(preset, obs_module_text("PresetCustom"), "custom");
	obs_property_list_add_string(preset, obs_module_text("PresetMinimal"), "minimal");
	obs_property_list_add_string(preset, obs_module_text("PresetDark"), "dark");
	obs_property_list_add_string(preset, obs_module_text("PresetNeon"), "neon");
	obs_property_list_add_string(preset, obs_module_text("PresetGlass"), "glass");
	obs_property_list_add_string(preset, obs_module_text("PresetSpotify"), "spotify");
	obs_property_list_add_string(preset, obs_module_text("PresetYouTube"), "youtube");
	obs_property_list_add_string(preset, obs_module_text("PresetPurple"), "purple");
	obs_property_list_add_string(preset, obs_module_text("PresetOutline"), "outline");
	obs_property_set_modified_callback(preset, preset_modified);
	auto *style_code = obs_properties_add_text(properties, "style_code", obs_module_text("StyleCode"),
		OBS_TEXT_MULTILINE);
	(void)style_code;
	obs_properties_add_text(properties, "style_code_help", obs_module_text("StyleCodeHint"), OBS_TEXT_INFO);
	obs_properties_add_text(properties, "text_format", obs_module_text("TextFormat"), OBS_TEXT_DEFAULT);
	obs_properties_add_text(properties, "font", obs_module_text("Font"), OBS_TEXT_DEFAULT);
	obs_properties_add_int(properties, "font_size", obs_module_text("FontSize"), 10, 200, 1);
	auto *weight = obs_properties_add_list(properties, "font_weight", obs_module_text("FontWeight"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(weight, obs_module_text("WeightNormal"), FW_NORMAL);
	obs_property_list_add_int(weight, obs_module_text("WeightSemiBold"), FW_SEMIBOLD);
	obs_property_list_add_int(weight, obs_module_text("WeightBold"), FW_BOLD);
	auto *alignment = obs_properties_add_list(properties, "text_alignment", obs_module_text("TextAlignment"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(alignment, obs_module_text("AlignLeft"), 0);
	obs_property_list_add_int(alignment, obs_module_text("AlignCenter"), 1);
	obs_property_list_add_int(alignment, obs_module_text("AlignRight"), 2);
	obs_properties_add_color_alpha(properties, "color", obs_module_text("Color"));
	obs_properties_add_bool(properties, "show_artist", obs_module_text("ShowArtist"));
	obs_properties_add_bool(properties, "hide_when_paused", obs_module_text("HideWhenPaused"));
	obs_properties_add_int(properties, "fade_duration_ms", obs_module_text("FadeDuration"), 0, 3000, 50);
	obs_properties_add_int(properties, "scroll_speed", obs_module_text("ScrollSpeed"), 10, 300, 5);
	obs_properties_add_bool(properties, "show_card", obs_module_text("ShowCard"));
	obs_properties_add_color_alpha(properties, "background_color", obs_module_text("BackgroundColor"));
	obs_properties_add_color_alpha(properties, "border_color", obs_module_text("BorderColor"));
	obs_properties_add_int(properties, "border_width", obs_module_text("BorderWidth"), 0, 30, 1);
	obs_properties_add_int(properties, "corner_radius", obs_module_text("CornerRadius"), 0, 200, 1);
	obs_properties_add_int(properties, "horizontal_padding", obs_module_text("HorizontalPadding"), 0, 300, 1);
	obs_properties_add_int(properties, "width", obs_module_text("Width"), 100, 3840, 10);
	obs_properties_add_int(properties, "height", obs_module_text("Height"), 40, 2160, 10);
	return properties_owner.release();
}

obs_properties_t *source_properties(void *data)
{
	try {
		return source_properties_impl(data);
	} catch (const std::exception &error) {
		obs_log(LOG_ERROR, "Could not create Now Playing properties: %s", error.what());
	} catch (...) {
		obs_log(LOG_ERROR, "Could not create Now Playing properties");
	}
	return obs_properties_create();
}

obs_source_info source_info = {
	.id = SOURCE_ID,
	.type = OBS_SOURCE_TYPE_INPUT,
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_SRGB,
	.get_name = source_name,
	.create = source_create,
	.destroy = source_destroy,
	.get_width = source_width,
	.get_height = source_height,
	.get_defaults = source_defaults,
	.get_properties = source_properties,
	.update = source_update,
	.show = source_show,
	.hide = source_hide,
	.video_tick = source_tick,
	.video_render = source_render,
};

#endif

} // namespace

bool obs_module_load(void)
{
#ifdef _WIN32
	obs_register_source(&source_info);
	obs_log(LOG_INFO, "plugin loaded successfully (version %s)", PLUGIN_VERSION);
	return true;
#else
	obs_log(LOG_ERROR, "Now Playing for OBS currently supports Windows only");
	return false;
#endif
}

void obs_module_unload(void)
{
	obs_log(LOG_INFO, "plugin unloaded");
}
