<#
.SYNOPSIS
    Run the gates Windows CI runs, in the same order, against the local tree.

.DESCRIPTION
    Mirrors .github/workflows/windows-ci.yml, so a green run here means a
    green run there: scripts/check-format.sh (the pinned clang-format, CI's file set),
    scripts/check-action-pins.sh, scripts/check-version-consistency.sh,
    scripts/check-vendored-freshness.sh, configure + build + clang-tidy
    (scripts/check-tidy.sh, the pinned clang-tidy) + ctest via the
    windows-mingw preset (MSYS2 MINGW64, the toolchain CI installs), the
    satellite.exe check, the redist script tests (tests/test_fetch_redist.ps1),
    the HIDMaestro helper publish and the helper's SDK check
    (scripts/verify-helper-sdk.ps1).

        scripts/ci-local.ps1                 every gate windows-ci.yml runs
        scripts/ci-local.ps1 -Quick          format, pins, version consistency,
                                             vendored freshness: no build
        scripts/ci-local.ps1 -WithMsvc       also windows-msvc-ci.yml: the MSVC
                                             build, its tests, the installer and
                                             its silent round-trip (elevated)
        scripts/ci-local.ps1 -AllowMissing   downgrade a missing tool to a notice

    Without -AllowMissing a gate whose tool is absent FAILS rather than
    printing a notice and continuing: a green run that silently skipped a
    gate is worse than no run at all.

    CI-only, with the reason: the MSVC lane's CFG/CET dumpbin read-back and
    the UIAccess compile check (they need the runner's Visual Studio layout;
    -WithMsvc runs the rest of that lane). The Linux/macOS equivalent is
    scripts/ci-local.sh.

.PARAMETER AllowMissing
    Downgrade a missing gated tool (bash, cmake, g++) to a notice
    instead of failing.
#>
[CmdletBinding()]
param(
    [switch]$AllowMissing,
    [switch]$Quick,
    [switch]$WithMsvc
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Step([string]$Text) { Write-Output ''; Write-Output "=== $Text ===" }

# Returns $true when the caller should run the gate, $false when it was
# skipped by permission, and throws when a tool CI gates on is missing.
function Have([string]$Tool) {
    if (Get-Command $Tool -ErrorAction SilentlyContinue) { return $true }
    if ($AllowMissing) {
        # Write-Host, not Write-Output: this function's pipeline output IS its
        # return value, and a notice string would read as $true to the caller.
        Write-Host "::notice:: $Tool is not installed; CI gates this. Skipping (-AllowMissing)."
        return $false
    }
    throw "$Tool is not installed and CI gates it. Install it (scripts/install-deps.ps1), or re-run with -AllowMissing."
}

Step 'clang-format (check only)'
# The same script every CI lane runs, under the bash Git for Windows ships,
# so the file set, the invocation and the pinned clang-format (scripts/tools.sh)
# are the ones CI uses.
if (Have 'bash') {
    & bash scripts/check-format.sh
    if ($LASTEXITCODE -ne 0) { throw 'clang-format check failed' }
}

Step 'Action pin lint (40-char SHA required)'
if (Have 'bash') {
    & bash scripts/check-action-pins.sh
    if ($LASTEXITCODE -ne 0) { throw 'action pin lint failed' }
}

Step 'Version consistency (version-consistency.yml)'
if (Have 'bash') {
    & bash scripts/check-version-consistency.sh
    if ($LASTEXITCODE -ne 0) { throw 'version consistency failed' }
}

Step 'Vendored components <= 90 days old (security.yml)'
if (Have 'bash') {
    & bash scripts/check-vendored-freshness.sh
    if ($LASTEXITCODE -ne 0) { throw 'vendored freshness failed' }
}

if ($Quick) {
    Write-Output ''
    Write-Output 'Quick gates passed (no build: drop -Quick for the full run).'
    exit 0
}

Step 'Script tests (tests/test_fetch_redist.ps1)'
& (Get-Process -Id $PID).Path -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'tests\test_fetch_redist.ps1')
if ($LASTEXITCODE -ne 0) { throw 'test_fetch_redist.ps1 failed' }

Step 'Script tests (tests/test_measurements_report.ps1)'
& (Get-Process -Id $PID).Path -NoProfile -ExecutionPolicy Bypass -File (Join-Path $repoRoot 'tests\test_measurements_report.ps1')
if ($LASTEXITCODE -ne 0) { throw 'test_measurements_report.ps1 failed' }

# windows-ci.yml runs inside the MSYS2 MINGW64 shell; get the same toolchain
# by putting mingw64\bin first. Never ucrt64: different toolchain than CI's.
$mingwBin = 'C:\msys64\mingw64\bin'
if (Test-Path $mingwBin) {
    $others = ($env:PATH -split ';') | Where-Object { $_ -and $_.TrimEnd('\') -ne $mingwBin }
    $env:PATH = (@($mingwBin) + $others) -join ';'
}

Step 'Configure (Release, preset windows-mingw)'
$toolchainReady = (Have 'cmake') -and (Have 'g++')
if ($toolchainReady) {
    $gxx = (Get-Command g++).Source
    if ($gxx -notlike "$mingwBin*") {
        Write-Output "::notice:: g++ resolves to $gxx, not $mingwBin; CI builds with MSYS2 MINGW64 gcc."
    }
    & cmake --preset windows-mingw
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

    Step 'Build'
    & cmake --build --preset windows-mingw -j $env:NUMBER_OF_PROCESSORS
    if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }

    Step 'clang-tidy (pinned, shared .clang-tidy)'
    & bash scripts/check-tidy.sh build
    if ($LASTEXITCODE -ne 0) { throw 'clang-tidy found something' }

    Step 'Run tests'
    & ctest --preset windows-mingw
    if ($LASTEXITCODE -ne 0) { throw 'ctest failed' }

    Step 'Verify satellite.exe was produced'
    if (-not (Test-Path 'satellite.exe')) { throw 'satellite.exe not produced at repo root' }
    Get-Item satellite.exe | Format-List Name, Length, LastWriteTime
}

Step 'Build HIDMaestro helper (dotnet publish)'
if (Have 'dotnet') {
    # fetch-redist.ps1 stages the hash-pinned HIDMaestro SDK the helper
    # project references and the catalog files the profile copies are cut
    # from, which the helper's tests read; CI runs it before both.
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'fetch-redist.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'fetch-redist.ps1 failed' }
    & dotnet test helper/hidmaestro-core.tests/satellite-hm-helper-core.tests.csproj -c Release
    if ($LASTEXITCODE -ne 0) { throw 'dotnet test helper/hidmaestro-core.tests failed' }
    foreach ($stale in @('helper/hidmaestro/bin', 'helper/hidmaestro/obj')) {
        if (Test-Path $stale) { Remove-Item -Recurse -Force $stale }
    }
    & dotnet publish helper/hidmaestro/satellite-hm-helper.csproj -c Release
    if ($LASTEXITCODE -ne 0) { throw 'dotnet publish failed' }
    if (-not (Test-Path 'helper/hidmaestro/bin/Release/net10.0-windows10.0.26100.0/win-x64/publish/satellite-hm-helper.exe')) {
        throw 'dotnet publish did not produce satellite-hm-helper.exe'
    }
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'verify-helper-sdk.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'verify-helper-sdk.ps1 failed' }
}

if ($WithMsvc) {
    Step 'MSVC lane: build and test (preset windows-msvc)'
    & (Join-Path $PSScriptRoot 'build.ps1') release test -Msvc
    if (-not $?) { throw 'the MSVC build failed' }

    Step 'MSVC lane: smoke the built app'
    & (Join-Path $PSScriptRoot 'smoke-windows-app.ps1')
    if (-not $?) { throw 'smoke-windows-app.ps1 failed' }

    Step 'MSVC lane: the installer, unsigned, and its silent round-trip'
    $iscc = & (Join-Path $PSScriptRoot 'install-inno-setup.ps1')
    & (Join-Path $PSScriptRoot 'build-installer.ps1') -Iscc $iscc
    if (-not $?) { throw 'build-installer.ps1 failed' }
    & (Join-Path $PSScriptRoot 'test-installer-roundtrip.ps1') -Setup dist\SatelliteSetup.exe
    if (-not $?) { throw 'test-installer-roundtrip.ps1 failed' }
}

Write-Output ''
Write-Output 'All local CI gates passed.'
