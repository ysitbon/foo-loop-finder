[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release", "RelWithDebInfo", "MinSizeRel")]
    [string]$Configuration = "Release",

    [switch]$Clean,

    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = $PSScriptRoot
$buildDirectory = Join-Path $repoRoot "build"
$vswhere = Join-Path ${env:ProgramFiles(x86)} `
    "Microsoft Visual Studio\Installer\vswhere.exe"

if (-not (Test-Path $vswhere)) {
    throw "vswhere.exe was not found. Install or repair Visual Studio Build Tools 2022."
}

$vsPath = & $vswhere `
    -latest `
    -products * `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
    -property installationPath

if (-not $vsPath) {
    throw @"
No Visual Studio installation with the MSVC x64/x86 tools was found.
Open Visual Studio Installer and add:
  - Desktop development with C++
  - MSVC v143 C++ x64/x86 build tools
  - Windows 10 or 11 SDK
  - C++ CMake tools for Windows
"@
}

$vsPath = @($vsPath)[0].Trim()
$devShell = Join-Path $vsPath "Common7\Tools\Launch-VsDevShell.ps1"

if (-not (Test-Path $devShell)) {
    throw "Visual Studio developer shell was not found at: $devShell"
}

Write-Host "Initializing MSVC from $vsPath"
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake was not found after initializing Build Tools. Add C++ CMake tools for Windows."
}

Push-Location $repoRoot
try {
    Write-Host "Configuring $Configuration x64 build"
    & cmake -S . -B $buildDirectory -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed with exit code $LASTEXITCODE."
    }

    if ($Clean) {
        Write-Host "Cleaning existing $Configuration outputs"
        & cmake --build $buildDirectory --config $Configuration --target clean
        if ($LASTEXITCODE -ne 0) {
            throw "Clean failed with exit code $LASTEXITCODE."
        }
    }

    Write-Host "Building $Configuration"
    & cmake --build $buildDirectory --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "Build failed with exit code $LASTEXITCODE."
    }

    if (-not $SkipTests) {
        Write-Host "Running tests"
        & ctest --test-dir $buildDirectory `
            -C $Configuration `
            --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            throw "Tests failed with exit code $LASTEXITCODE."
        }
    }

    Write-Host ""
    Write-Host "Build succeeded." -ForegroundColor Green
    Write-Host "Core library: build\$Configuration\loop_finder_core.lib"
    Write-Host "Tests:        build\$Configuration\loop_finder_tests.exe"
    Write-Host ""
    Write-Host "Note: this iteration does not produce foo_loop_finder.dll yet."
}
finally {
    Pop-Location
}
