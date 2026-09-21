#Requires -Version 7.0
<#
.SYNOPSIS
    Windows equivalent of setup.sh: installs mousiki's runtime dependencies,
    then configures and builds it.

.DESCRIPTION
    mousiki itself is a C++ binary, but it shells out to three external tools
    at runtime:

      ffmpeg  - decodes Opus (miniaudio's built-in decoders don't cover it,
                and Opus is exactly what the yt-dlp cache stores), and ffprobe
                supplies track metadata.
      yt-dlp  - online search, playlist listing and streaming.
      python  - runs scripts/fetch_lyrics.py for synced lyrics.

    None of those are optional if you want the corresponding feature, but all
    three are independent: mousiki runs local files fine with none installed.

.PARAMETER SkipDeps
    Configure and build only; don't touch winget/pip.

.PARAMETER BuildType
    Release (default) or Debug.
#>
[CmdletBinding()]
param(
    [switch]$SkipDeps,
    [ValidateSet('Release','Debug')]
    [string]$BuildType = 'Release'
)

$ErrorActionPreference = 'Stop'
$root = $PSScriptRoot

function Test-Command([string]$Name) {
    return [bool](Get-Command $Name -ErrorAction SilentlyContinue)
}

function Write-Step([string]$Message) {
    Write-Host "==> $Message" -ForegroundColor Cyan
}

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
if (-not $SkipDeps) {
    if (-not (Test-Command winget)) {
        Write-Warning "winget not found. Install the dependencies manually, then re-run with -SkipDeps."
    } else {
        # cmake and a compiler come first; without them there is nothing to build.
        if (-not (Test-Command cmake)) {
            Write-Step "Installing CMake"
            winget install --id Kitware.CMake --accept-source-agreements --accept-package-agreements --silent
        }
        if (-not (Test-Command ffmpeg)) {
            Write-Step "Installing FFmpeg"
            winget install --id Gyan.FFmpeg --accept-source-agreements --accept-package-agreements --silent
        }
        if (-not (Test-Command yt-dlp)) {
            Write-Step "Installing yt-dlp"
            winget install --id yt-dlp.yt-dlp --accept-source-agreements --accept-package-agreements --silent
        }
        if (-not (Test-Command py) -and -not (Test-Command python3)) {
            Write-Step "Installing Python 3"
            winget install --id Python.Python.3.12 --accept-source-agreements --accept-package-agreements --silent
        }
    }

    # winget puts new tools on the machine PATH, but this already-running
    # session inherited the old one. Refresh it in-process so the checks below
    # and the build itself can see them without a new shell.
    $env:PATH = [Environment]::GetEnvironmentVariable('Path','Machine') + ';' +
                [Environment]::GetEnvironmentVariable('Path','User')

    $python = if (Test-Command py) { 'py -3' } elseif (Test-Command python3) { 'python3' } else { $null }
    if ($python) {
        Write-Step "Installing the requests package (used by scripts/lrc.py)"
        Invoke-Expression "$python -m pip install --quiet --upgrade requests"
    } else {
        Write-Warning "No Python 3 found. Lyrics will be unavailable; everything else still works."
    }
}

# ---------------------------------------------------------------------------
# Toolchain check
# ---------------------------------------------------------------------------
if (-not (Test-Command cmake)) {
    throw "cmake is not on PATH. Open a new PowerShell window (winget updates PATH only for new sessions) or install CMake manually."
}

# CMake needs a C++17 compiler. On a clean Windows box there isn't one: Visual
# Studio Build Tools has to be installed interactively, because the C++
# workload is a separate component that winget's silent install won't select.
$hasCompiler = (Test-Command cl) -or (Test-Command g++) -or
               (Test-Path 'C:\Program Files\Microsoft Visual Studio') -or
               (Test-Path 'C:\Program Files (x86)\Microsoft Visual Studio')
if (-not $hasCompiler) {
    Write-Warning @"
No C++ compiler detected.

Install Visual Studio Build Tools and tick the workload
"Desktop development with C++":

    winget install --id Microsoft.VisualStudio.2022.BuildTools

Then re-run this script. (MSYS2 / MinGW-w64 also works if you prefer it --
pass -G "MinGW Makefiles" to cmake below.)
"@
    return
}

# ---------------------------------------------------------------------------
# Build
# ---------------------------------------------------------------------------
$build = Join-Path $root 'build'

Write-Step "Configuring ($BuildType)"
# miniaudio and kissfft are vendored in third_party/, so configuring
# needs no network access.
cmake -S $root -B $build -DCMAKE_BUILD_TYPE=$BuildType
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }

Write-Step "Building"
cmake --build $build --config $BuildType --parallel
if ($LASTEXITCODE -ne 0) { throw "cmake build failed" }

# Single-config generators put the exe in build\, multi-config (Visual Studio)
# in build\<Config>\.
$exe = Get-ChildItem -Path $build -Filter 'mousiki.exe' -Recurse |
       Select-Object -First 1

if (-not $exe) { throw "Build reported success but mousiki.exe was not found under $build" }

Write-Host ""
Write-Host "Built: $($exe.FullName)" -ForegroundColor Green
Write-Host "Run it with:  & '$($exe.FullName)'"
Write-Host ""
Write-Host "Config will be generated at: $env:USERPROFILE\.config\mousiki\config.txt"
Write-Host "Use Windows Terminal, not the legacy console window."
