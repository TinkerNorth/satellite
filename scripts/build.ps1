<#
.SYNOPSIS
    Build Satellite on Windows: scripts/build.ps1 [debug|release] [test] [-Msvc]

.DESCRIPTION
    Thin wrapper over the CMake presets in CMakePresets.json, which are the
    single source of configure truth (the same presets windows-ci.yml and
    windows-msvc-ci.yml run). Same argument shape as the dish repos'
    scripts/build.ps1 / scripts/build.sh.

        scripts/build.ps1                  # Release (windows-mingw preset)
        scripts/build.ps1 debug            # Debug (windows-mingw-debug preset)
        scripts/build.ps1 release test     # Release build, then ctest
        scripts/build.ps1 -Msvc            # hardened MSVC + vcpkg (windows-msvc preset)

    The default lane is MSYS2 MINGW64 (MinGW Makefiles), exactly what
    windows-ci.yml builds; run scripts/install-deps.ps1 once to install it.
    Output: satellite.exe at the repo root (CMake RUNTIME_OUTPUT_DIRECTORY).

.PARAMETER BuildType
    debug or release (default release). -Msvc supports release only.

.PARAMETER Action
    'test' to run ctest after building; empty to just build.

.PARAMETER Msvc
    Use the hardened MSVC + vcpkg lane instead (CFG + CET + Spectre).
    Needs Visual Studio (or Build Tools) with the C++ x64 toolset and a vcpkg
    checkout in VCPKG_ROOT (VCPKG_INSTALLATION_ROOT is accepted as fallback).
    The Visual Studio generator is resolved per-machine via
    scripts/windows-vs-generator.ps1 and passed as a -G override, the same
    mechanism windows-msvc-ci.yml and release.yml use.
#>
[CmdletBinding()]
param(
    [ValidateSet('release', 'debug')]
    [string]$BuildType = 'release',
    [ValidateSet('', 'test')]
    [string]$Action = '',
    [switch]$Msvc
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

$jobs = $env:NUMBER_OF_PROCESSORS

if ($Msvc) {
    if ($BuildType -eq 'debug') {
        throw "The windows-msvc preset is the hardened Release lane; there is no MSVC debug preset. Use the MinGW lane for debug builds."
    }
    if (-not $env:VCPKG_ROOT) {
        if ($env:VCPKG_INSTALLATION_ROOT) {
            $env:VCPKG_ROOT = $env:VCPKG_INSTALLATION_ROOT
        } else {
            throw "VCPKG_ROOT is not set. Run scripts/install-deps.ps1 -Msvc, or point VCPKG_ROOT at a vcpkg checkout."
        }
    }
    $generator = & (Join-Path $PSScriptRoot 'windows-vs-generator.ps1')
    $preset = 'windows-msvc'
    & cmake --preset $preset -G $generator
} else {
    # windows-ci.yml runs inside the MSYS2 MINGW64 shell; a plain PowerShell
    # session gets the same toolchain by putting mingw64\bin first. Never
    # ucrt64: that is a different toolchain than CI's.
    $mingwBin = 'C:\msys64\mingw64\bin'
    if ((Test-Path $mingwBin) -and (-not (($env:PATH -split ';') -contains $mingwBin))) {
        $env:PATH = "$mingwBin;$env:PATH"
    }
    if (-not (Get-Command g++ -ErrorAction SilentlyContinue)) {
        throw "No g++ on PATH and no MSYS2 MINGW64 at $mingwBin. Run scripts/install-deps.ps1 first."
    }
    $preset = if ($BuildType -eq 'debug') { 'windows-mingw-debug' } else { 'windows-mingw' }
    & cmake --preset $preset
}
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed (preset $preset)" }

& cmake --build --preset $preset -j $jobs
if ($LASTEXITCODE -ne 0) { throw "cmake build failed (preset $preset)" }

if ($Action -eq 'test') {
    & ctest --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "ctest failed (preset $preset)" }
}

Write-Output ''
Write-Output "Build complete (preset $preset): satellite.exe at the repo root"
