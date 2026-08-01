[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [string]$FoobarSdkPath,

    [switch]$Clean,

    [switch]$SkipTests
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

$repoRoot = $PSScriptRoot
$buildRoot = Join-Path $repoRoot "build"
$cmakeBuildDirectory = Join-Path $buildRoot "vs2022-x64"
$nativeProject = Join-Path $repoRoot "native\foo_loop_finder.vcxproj"
$nativeOutputDirectory = Join-Path $buildRoot "native\$Configuration"
$nativeDll = Join-Path $nativeOutputDirectory "foo_loop_finder.dll"
$packagePath = Join-Path $buildRoot "foo_loop_finder.fb2k-component"
$packageZipPath = Join-Path $buildRoot "foo_loop_finder.zip"
$sdkWasExplicit = $PSBoundParameters.ContainsKey("FoobarSdkPath")

if ([string]::IsNullOrWhiteSpace($FoobarSdkPath)) {
    $FoobarSdkPath = Join-Path $repoRoot "external\foobar2000-sdk"
}

$sdkRoot = $ExecutionContext.SessionState.Path.GetUnresolvedProviderPathFromPSPath(
    $FoobarSdkPath)
$requiredSdkFiles = @(
    "foobar2000\SDK\foobar2000_SDK.vcxproj",
    "foobar2000\foobar2000_component_client\foobar2000_component_client.vcxproj",
    "pfc\pfc.vcxproj",
    "foobar2000\shared\shared-x64.lib"
)
$missingSdkFiles = @($requiredSdkFiles | Where-Object {
        -not (Test-Path -LiteralPath (Join-Path $sdkRoot $_) -PathType Leaf)
    })

if ($missingSdkFiles.Count -gt 0) {
    $missingList = ($missingSdkFiles | ForEach-Object { "  - $_" }) -join "`n"
    if ($sdkWasExplicit) {
        throw @"
The path supplied with -FoobarSdkPath is not a valid foobar2000 SDK 2025-03-07 root:
  $sdkRoot
Missing required files:
$missingList
"@
    }

    throw @"
The default foobar2000 SDK directory is missing or incomplete:
  $sdkRoot

Download the official foobar2000 SDK 2025-03-07 and extract the contents so that
this exact directory contains the foobar2000 and pfc folders:
  $repoRoot\external\foobar2000-sdk

Missing required files:
$missingList
"@
}

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
$msbuild = Join-Path $vsPath "MSBuild\Current\Bin\MSBuild.exe"

if (-not (Test-Path $devShell)) {
    throw "Visual Studio developer shell was not found at: $devShell"
}
if (-not (Test-Path $msbuild)) {
    throw "MSBuild was not found at: $msbuild"
}

Write-Host "Initializing MSVC from $vsPath"
& $devShell -Arch amd64 -HostArch amd64 -SkipAutomaticLocation

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    throw "cmake was not found after initializing Build Tools. Add C++ CMake tools for Windows."
}

Push-Location $repoRoot
try {
    if ($Clean) {
        $expectedBuildRoot = [System.IO.Path]::GetFullPath(
            (Join-Path $repoRoot "build"))
        $resolvedBuildRoot = [System.IO.Path]::GetFullPath($buildRoot)
        if ($resolvedBuildRoot -ne $expectedBuildRoot) {
            throw "Refusing to clean unexpected build directory: $resolvedBuildRoot"
        }
        if (Test-Path -LiteralPath $resolvedBuildRoot) {
            Write-Host "Cleaning generated outputs in $resolvedBuildRoot"
            Remove-Item -LiteralPath $resolvedBuildRoot -Recurse -Force
        }
    }

    Write-Host "Configuring $Configuration x64 build"
    & cmake -S . -B $cmakeBuildDirectory -G "Visual Studio 17 2022" -A x64
    if ($LASTEXITCODE -ne 0) {
        throw "CMake configuration failed with exit code $LASTEXITCODE."
    }

    Write-Host "Building core and tests ($Configuration|x64)"
    & cmake --build $cmakeBuildDirectory --config $Configuration --parallel
    if ($LASTEXITCODE -ne 0) {
        throw "CMake build failed with exit code $LASTEXITCODE."
    }

    if (-not $SkipTests) {
        Write-Host "Running core tests"
        & ctest --test-dir $cmakeBuildDirectory `
            -C $Configuration `
            --output-on-failure
        if ($LASTEXITCODE -ne 0) {
            throw "Tests failed with exit code $LASTEXITCODE."
        }
    }

    Write-Host "Building native foobar2000 component ($Configuration|x64)"
    & $msbuild $nativeProject `
        /m `
        /t:Build `
        "/p:Configuration=$Configuration" `
        /p:Platform=x64 `
        /p:PlatformToolset=v143 `
        "/p:FoobarSdkRoot=$sdkRoot"
    if ($LASTEXITCODE -ne 0) {
        throw "Native component build failed with exit code $LASTEXITCODE."
    }

    if (-not (Test-Path -LiteralPath $nativeDll -PathType Leaf)) {
        throw @"
MSBuild reported success, but the native component DLL was not produced:
  $nativeDll
"@
    }

    Write-Host "Packaging foo_loop_finder.fb2k-component"
    New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
    Remove-Item -LiteralPath $packageZipPath -Force -ErrorAction SilentlyContinue
    Remove-Item -LiteralPath $packagePath -Force -ErrorAction SilentlyContinue
    Compress-Archive -LiteralPath $nativeDll -DestinationPath $packageZipPath -Force
    Move-Item -LiteralPath $packageZipPath -Destination $packagePath

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($packagePath)
    try {
        $packageEntries = @($archive.Entries | ForEach-Object { $_.FullName })
    }
    finally {
        $archive.Dispose()
    }
    if ($packageEntries.Count -ne 1 -or
        $packageEntries[0] -ne "foo_loop_finder.dll") {
        throw "Package contents are invalid: $($packageEntries -join ', ')"
    }

    $coreLibrary = Join-Path $cmakeBuildDirectory `
        "$Configuration\loop_finder_core.lib"
    $testExecutable = Join-Path $cmakeBuildDirectory `
        "$Configuration\loop_finder_tests.exe"

    Write-Host ""
    Write-Host "Build succeeded." -ForegroundColor Green
    Write-Host "Generated artifacts:"
    Write-Host "  Core library:       $coreLibrary"
    Write-Host "  Core tests:         $testExecutable"
    Write-Host "  Component DLL:      $nativeDll"
    Write-Host "  Component package:  $packagePath"
}
finally {
    Pop-Location
}
