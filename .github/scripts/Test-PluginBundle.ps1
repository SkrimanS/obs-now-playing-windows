<#
.SYNOPSIS
Validates a built Windows OBS plugin bundle without starting OBS.

.EXAMPLE
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .github\scripts\Test-PluginBundle.ps1 `
    -BundlePath release\Release\obs-now-playing

.EXAMPLE
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .github\scripts\Test-PluginBundle.ps1 `
    -BundlePath dist\obs-now-playing-0.4.2-windows-x64.zip
#>
[CmdletBinding()]
param(
    [Parameter(Position = 0)]
    [string] $BundlePath,

    [string] $RepositoryRoot,

    [switch] $SkipBinaryTools
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$script:Failures = [System.Collections.Generic.List[string]]::new()
$script:Checks = 0
$script:TemporaryDirectory = $null

function Add-Pass {
    param([Parameter(Mandatory)][string] $Message)

    $script:Checks++
    Write-Host "[PASS] $Message" -ForegroundColor Green
}

function Add-Failure {
    param([Parameter(Mandatory)][string] $Message)

    $script:Checks++
    $script:Failures.Add($Message)
    Write-Host "[FAIL] $Message" -ForegroundColor Red
}

function Assert-Check {
    param(
        [Parameter(Mandatory)][bool] $Condition,
        [Parameter(Mandatory)][string] $Message
    )

    if ($Condition) {
        Add-Pass $Message
    } else {
        Add-Failure $Message
    }
}

function Get-StrictUtf8Text {
    param([Parameter(Mandatory)][string] $Path)

    $encoding = [System.Text.UTF8Encoding]::new($false, $true)
    return [System.IO.File]::ReadAllText($Path, $encoding)
}

function Read-Locale {
    param([Parameter(Mandatory)][string] $Path)

    $values = [ordered]@{}
    $lineNumber = 0

    foreach ($line in (Get-StrictUtf8Text $Path) -split "`r?`n") {
        $lineNumber++
        if ([string]::IsNullOrWhiteSpace($line) -or $line.TrimStart().StartsWith(';') -or $line.TrimStart().StartsWith('#')) {
            continue
        }

        if ($line -notmatch '^([^=\s]+)\s*=\s*"(.*)"\s*$') {
            throw "Invalid locale syntax at ${Path}:${lineNumber}: $line"
        }

        $key = $Matches[1]
        $value = $Matches[2]
        if ($values.Contains($key)) {
            throw "Duplicate locale key '$key' at ${Path}:${lineNumber}"
        }
        if ([string]::IsNullOrWhiteSpace($value)) {
            throw "Empty locale value for '$key' at ${Path}:${lineNumber}"
        }

        $values[$key] = $value
    }

    return $values
}

function Find-DumpBin {
    $command = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($null -ne $command) {
        return $command.Source
    }

    $patterns = [System.Collections.Generic.List[string]]::new()
    if (${env:ProgramFiles}) {
        $patterns.Add((Join-Path ${env:ProgramFiles} 'Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe'))
    }
    if (${env:ProgramFiles(x86)}) {
        $patterns.Add((Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe'))
    }

    $candidates = @(
        @(
            foreach ($pattern in $patterns) {
                Get-ChildItem -Path $pattern -File -ErrorAction SilentlyContinue
            }
        ) | Sort-Object FullName -Descending
    )

    if ($candidates.Count -gt 0) {
        return $candidates[0].FullName
    }

    return $null
}

function Resolve-BundleRoot {
    param(
        [Parameter(Mandatory)][string] $Candidate,
        [Parameter(Mandatory)][string] $PluginName
    )

    $resolved = (Resolve-Path -LiteralPath $Candidate).Path
    if ((Get-Item -LiteralPath $resolved) -is [System.IO.FileInfo]) {
        if ([System.IO.Path]::GetExtension($resolved) -ine '.zip') {
            throw "BundlePath must be a directory or .zip archive: $resolved"
        }

        $script:TemporaryDirectory = Join-Path ([System.IO.Path]::GetTempPath()) ("obs-plugin-validation-" + [guid]::NewGuid().ToString('N'))
        New-Item -ItemType Directory -Path $script:TemporaryDirectory | Out-Null
        Expand-Archive -LiteralPath $resolved -DestinationPath $script:TemporaryDirectory

        $topLevel = @(Get-ChildItem -LiteralPath $script:TemporaryDirectory -Force)
        Assert-Check ($topLevel.Count -eq 1 -and $topLevel[0].PSIsContainer -and $topLevel[0].Name -eq $PluginName) `
            "archive has exactly one '$PluginName' root directory"
        $resolved = $script:TemporaryDirectory
    }

    $directDll = Join-Path $resolved "bin\64bit\$PluginName.dll"
    if (Test-Path -LiteralPath $directDll -PathType Leaf) {
        return $resolved
    }

    $child = Join-Path $resolved $PluginName
    $childDll = Join-Path $child "bin\64bit\$PluginName.dll"
    if (Test-Path -LiteralPath $childDll -PathType Leaf) {
        return $child
    }

    $matches = @(
        Get-ChildItem -LiteralPath $resolved -Directory -Recurse -ErrorAction SilentlyContinue |
            Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName "bin\64bit\$PluginName.dll") -PathType Leaf }
    )
    if ($matches.Count -eq 1) {
        return $matches[0].FullName
    }

    throw "Could not identify a unique '$PluginName' plugin root below '$resolved'."
}

function Get-PeInformation {
    param([Parameter(Mandatory)][string] $Path)

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::Read)
    $reader = [System.IO.BinaryReader]::new($stream)
    try {
        if ($reader.ReadUInt16() -ne 0x5A4D) {
            throw 'Missing MZ header.'
        }

        $stream.Position = 0x3C
        $peOffset = $reader.ReadUInt32()
        if ($peOffset -gt ($stream.Length - 96)) {
            throw 'PE header offset is outside the file.'
        }

        $stream.Position = $peOffset
        if ($reader.ReadUInt32() -ne 0x00004550) {
            throw 'Missing PE signature.'
        }

        $machine = $reader.ReadUInt16()
        [void] $reader.ReadUInt16()
        $stream.Position = $peOffset + 22
        $characteristics = $reader.ReadUInt16()
        $stream.Position = $peOffset + 24
        $optionalMagic = $reader.ReadUInt16()
        $stream.Position = $peOffset + 24 + 70
        $dllCharacteristics = $reader.ReadUInt16()

        return [pscustomobject]@{
            Machine = $machine
            Characteristics = $characteristics
            OptionalMagic = $optionalMagic
            DllCharacteristics = $dllCharacteristics
        }
    } finally {
        $reader.Dispose()
        $stream.Dispose()
    }
}

try {
    if ([string]::IsNullOrWhiteSpace($RepositoryRoot)) {
        $RepositoryRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
    } else {
        $RepositoryRoot = (Resolve-Path -LiteralPath $RepositoryRoot).Path
    }

    $buildspecPath = Join-Path $RepositoryRoot 'buildspec.json'
    if (-not (Test-Path -LiteralPath $buildspecPath -PathType Leaf)) {
        throw "buildspec.json not found below '$RepositoryRoot'."
    }

    $buildspec = Get-Content -LiteralPath $buildspecPath -Raw -Encoding UTF8 | ConvertFrom-Json
    $pluginName = [string] $buildspec.name
    $pluginVersion = [string] $buildspec.version
    Assert-Check (-not [string]::IsNullOrWhiteSpace($pluginName)) 'buildspec has a plugin name'
    Assert-Check ($pluginVersion -match '^\d+\.\d+\.\d+(?:[-+][0-9A-Za-z.-]+)?$') 'buildspec version uses semantic version syntax'

    if ([string]::IsNullOrWhiteSpace($BundlePath)) {
        $candidates = @(
            (Join-Path $RepositoryRoot "release\Release\$pluginName"),
            (Join-Path $RepositoryRoot "release\$pluginName"),
            (Join-Path $RepositoryRoot "dist\$pluginName")
        )
        $BundlePath = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
        if ([string]::IsNullOrWhiteSpace($BundlePath)) {
            throw 'No default bundle was found. Pass -BundlePath explicitly.'
        }
    }

    $bundleRoot = Resolve-BundleRoot -Candidate $BundlePath -PluginName $pluginName
    Write-Host "Validating bundle: $bundleRoot" -ForegroundColor Cyan

    $dllPath = Join-Path $bundleRoot "bin\64bit\$pluginName.dll"
    $bundleData = Join-Path $bundleRoot 'data'
    $bundleLocale = Join-Path $bundleData 'locale'
    $bundleEffect = Join-Path $bundleData 'now-playing.effect'
    $bundleReadme = Join-Path $bundleRoot 'README.md'
    $bundleLicense = Join-Path $bundleRoot 'LICENSE'

    Assert-Check (Test-Path -LiteralPath $dllPath -PathType Leaf) '64-bit plugin DLL is present in the OBS bundle layout'
    Assert-Check (Test-Path -LiteralPath $bundleLocale -PathType Container) 'locale directory is present in the OBS bundle layout'
    Assert-Check (Test-Path -LiteralPath $bundleEffect -PathType Leaf) 'runtime effect file is present in the OBS bundle layout'
    Assert-Check (Test-Path -LiteralPath $bundleReadme -PathType Leaf) 'manual installation instructions are included in the bundle'
    Assert-Check (Test-Path -LiteralPath $bundleLicense -PathType Leaf) 'license is included in the bundle'
    Assert-Check (-not (Test-Path -LiteralPath (Join-Path $bundleRoot 'obs-plugins'))) 'bundle does not use the deprecated obs-plugins layout'
    Assert-Check (-not (Test-Path -LiteralPath (Join-Path $bundleRoot 'data\obs-plugins'))) 'bundle data does not use the deprecated nested layout'

    if (Test-Path -LiteralPath $bundleReadme -PathType Leaf) {
        try {
            $readmeText = Get-StrictUtf8Text $bundleReadme
            Assert-Check ($readmeText -match '(?i)ProgramData[\\/]obs-studio[\\/]plugins') `
                'manual installation instructions use the current per-machine OBS plugin directory'
        } catch {
            Add-Failure "bundled README is valid UTF-8 and readable: $($_.Exception.Message)"
        }
    }

    $sourceEffect = Join-Path $RepositoryRoot 'data\now-playing.effect'
    if ((Test-Path -LiteralPath $sourceEffect -PathType Leaf) -and (Test-Path -LiteralPath $bundleEffect -PathType Leaf)) {
        $sourceEffectHash = (Get-FileHash -LiteralPath $sourceEffect -Algorithm SHA256).Hash
        $bundleEffectHash = (Get-FileHash -LiteralPath $bundleEffect -Algorithm SHA256).Hash
        Assert-Check ($sourceEffectHash -eq $bundleEffectHash) 'packaged effect is byte-identical to the source effect'

        try {
            $effectText = Get-StrictUtf8Text $bundleEffect
            Assert-Check ($effectText -match '(?m)^\s*uniform\s+float4x4\s+ViewProj\s*;') 'effect declares the OBS ViewProj matrix'
            Assert-Check ($effectText -match '(?m)^\s*uniform\s+texture2d\s+image\s*;') 'effect declares the image texture'
            Assert-Check ($effectText -match '(?m)^\s*uniform\s+float\s+opacity\s*;') 'effect declares the opacity parameter'
            Assert-Check ($effectText -match '(?m)^\s*technique\s+Draw\b') 'effect provides the Draw technique'
        } catch {
            Add-Failure "effect is valid UTF-8 and readable: $($_.Exception.Message)"
        }
    }

    $sourceLocale = Join-Path $RepositoryRoot 'data\locale'
    $sourceLocaleFiles = @(Get-ChildItem -LiteralPath $sourceLocale -Filter '*.ini' -File | Sort-Object Name)
    $bundleLocaleFiles = if (Test-Path -LiteralPath $bundleLocale -PathType Container) {
        @(Get-ChildItem -LiteralPath $bundleLocale -Filter '*.ini' -File | Sort-Object Name)
    } else {
        @()
    }
    $sourceLocaleNames = @($sourceLocaleFiles | ForEach-Object Name)
    $bundleLocaleNames = @($bundleLocaleFiles | ForEach-Object Name)
    Assert-Check (@(Compare-Object $sourceLocaleNames $bundleLocaleNames).Count -eq 0) 'packaged locale file set matches the source locale file set'

    $referenceLocale = $null
    $referenceKeys = @()
    foreach ($sourceFile in $sourceLocaleFiles) {
        $packagedFile = Join-Path $bundleLocale $sourceFile.Name
        try {
            $locale = Read-Locale $sourceFile.FullName
            Add-Pass "source locale '$($sourceFile.Name)' has valid UTF-8 and INI syntax"

            if ($sourceFile.Name -eq 'en-US.ini') {
                $referenceLocale = $locale
                $referenceKeys = @($locale.Keys)
            }

            if (Test-Path -LiteralPath $packagedFile -PathType Leaf) {
                Assert-Check ((Get-FileHash -LiteralPath $sourceFile.FullName -Algorithm SHA256).Hash -eq
                        (Get-FileHash -LiteralPath $packagedFile -Algorithm SHA256).Hash) `
                    "packaged locale '$($sourceFile.Name)' is byte-identical to its source"
                [void] (Read-Locale $packagedFile)
            }
        } catch {
            Add-Failure "locale '$($sourceFile.Name)' is valid: $($_.Exception.Message)"
        }
    }

    if ($null -eq $referenceLocale) {
        Add-Failure "reference locale 'en-US.ini' exists"
    } else {
        foreach ($sourceFile in $sourceLocaleFiles | Where-Object Name -ne 'en-US.ini') {
            try {
                $locale = Read-Locale $sourceFile.FullName
                $difference = @(Compare-Object $referenceKeys @($locale.Keys))
                Assert-Check ($difference.Count -eq 0) "locale '$($sourceFile.Name)' has exact key parity with en-US.ini"
            } catch {
                Add-Failure "locale '$($sourceFile.Name)' key parity can be evaluated: $($_.Exception.Message)"
            }
        }
    }

    if (Test-Path -LiteralPath $dllPath -PathType Leaf) {
        Assert-Check ((Get-Item -LiteralPath $dllPath).Length -gt 4096) 'plugin DLL is non-empty'

        try {
            $pe = Get-PeInformation $dllPath
            Assert-Check ($pe.Machine -eq 0x8664) 'plugin DLL targets AMD64/x64'
            Assert-Check ($pe.OptionalMagic -eq 0x020B) 'plugin DLL uses the PE32+ format'
            Assert-Check (($pe.Characteristics -band 0x2000) -ne 0) 'PE image is marked as a DLL'
            Assert-Check (($pe.DllCharacteristics -band 0x0040) -ne 0) 'plugin DLL enables ASLR (DYNAMIC_BASE)'
            Assert-Check (($pe.DllCharacteristics -band 0x0100) -ne 0) 'plugin DLL enables DEP (NX_COMPAT)'
            Assert-Check (($pe.DllCharacteristics -band 0x0020) -ne 0) 'plugin DLL enables high-entropy ASLR'
        } catch {
            Add-Failure "plugin DLL has a readable PE header: $($_.Exception.Message)"
        }

        $versionInfo = [Diagnostics.FileVersionInfo]::GetVersionInfo($dllPath)
        Assert-Check ($versionInfo.ProductName -eq $pluginName) 'DLL ProductName matches buildspec name'
        $fileVersionParts = @(([string] $versionInfo.FileVersion -replace ', ', '.').Split('.') | ForEach-Object { [int] $_ })
        $pluginVersionParts = @($pluginVersion.Split('.') | ForEach-Object { [int] $_ })
        while ($fileVersionParts.Count -lt $pluginVersionParts.Count) { $fileVersionParts += 0 }
        while ($pluginVersionParts.Count -lt $fileVersionParts.Count) { $pluginVersionParts += 0 }
        $normalizedFileVersion = $fileVersionParts -join '.'
        $normalizedPluginVersion = $pluginVersionParts -join '.'
        Assert-Check ($normalizedFileVersion -eq $normalizedPluginVersion) "DLL file version matches buildspec version ($pluginVersion)"

        if ($SkipBinaryTools) {
            Write-Host '[SKIP] DLL exports and imports (-SkipBinaryTools)' -ForegroundColor Yellow
        } else {
            $dumpbin = Find-DumpBin
            if ($null -eq $dumpbin) {
                Add-Failure 'dumpbin.exe is available to validate OBS exports and imports (use -SkipBinaryTools only for source-only checks)'
            } else {
                $exports = @(& $dumpbin /nologo /exports $dllPath 2>&1)
                if ($LASTEXITCODE -ne 0) {
                    Add-Failure "dumpbin /exports succeeded: $($exports -join ' ')"
                } else {
                    $exportText = $exports -join "`n"
                    foreach ($requiredExport in @(
                            'obs_module_load',
                            'obs_module_unload',
                            'obs_module_set_pointer',
                            'obs_module_ver',
                            'obs_module_get_string',
                            'obs_module_set_locale',
                            'obs_module_free_locale'
                        )) {
                        Assert-Check ($exportText -match "(?m)\b$([regex]::Escape($requiredExport))\b") "DLL exports $requiredExport"
                    }
                }

                $dependents = @(& $dumpbin /nologo /dependents $dllPath 2>&1)
                if ($LASTEXITCODE -ne 0) {
                    Add-Failure "dumpbin /dependents succeeded: $($dependents -join ' ')"
                } else {
                    $dependencyText = $dependents -join "`n"
                    Assert-Check ($dependencyText -match '(?im)^\s*obs\.dll\s*$') 'DLL imports the OBS runtime (obs.dll)'
                    Assert-Check ($dependencyText -notmatch '(?im)^\s*(?:Qt6[^\s]*|MSVCP\d+D|VCRUNTIME\d+(?:_\d+)?D)\.dll\s*$') `
                        'DLL has no accidental Qt or debug-runtime dependency'
                }
            }
        }
    }

    if ($script:Failures.Count -gt 0) {
        Write-Host "`n$($script:Failures.Count) of $($script:Checks) checks failed:" -ForegroundColor Red
        foreach ($failure in $script:Failures) {
            Write-Host " - $failure" -ForegroundColor Red
        }
        throw 'Plugin bundle validation failed.'
    }

    Write-Host "`nAll $($script:Checks) plugin bundle checks passed." -ForegroundColor Green
} finally {
    if ($null -ne $script:TemporaryDirectory -and (Test-Path -LiteralPath $script:TemporaryDirectory)) {
        Remove-Item -LiteralPath $script:TemporaryDirectory -Recurse -Force
    }
}
