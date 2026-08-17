# Сборка из исходного кода

## Требования

- Windows 10 или Windows 11 x64;
- Visual Studio 2022 с рабочей нагрузкой «Разработка классических приложений на C++»;
- Windows SDK 10.0.22621.0;
- CMake 3.28 или новее;
- Git.

## Visual Studio 2022

```powershell
cmake --preset windows-x64
cmake --build --preset windows-x64
cmake --install build_x64 --config RelWithDebInfo --prefix release/RelWithDebInfo
```

## Visual Studio 2026

Команды необходимо запускать из Developer PowerShell:

```powershell
cmake --preset windows-x64-vs2026
cmake --build --preset windows-x64-vs2026
cmake --install build_nmake_vs2026 --prefix release/RelWithDebInfo
```

Готовый пакет появится в `release/RelWithDebInfo/obs-now-playing`.
