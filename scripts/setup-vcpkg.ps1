<#
.SYNOPSIS
    Check out vcpkg at the commit vcpkg.json names as its builtin-baseline,
    bootstrap it, and point VCPKG_ROOT at it.

.DESCRIPTION
    vcpkg.json's "builtin-baseline" is the one place the MSVC lane's
    dependency versions are pinned. Without it every dependency resolved
    against whatever vcpkg checkout the runner image carried, so libsodium,
    OpenSSL, opus and the ports sentry-native pulls in moved with each image
    refresh. The image's own checkout cannot serve a baseline newer than
    itself, so windows-msvc-ci.yml, measurements.yml and release.yml call
    this script instead of using VCPKG_INSTALLATION_ROOT.

    A full clone, not a shallow one: vcpkg reads each port's version from
    the git tree the baseline names, and a shallow history does not carry
    them all.

    In GitHub Actions the script also writes VCPKG_ROOT to GITHUB_ENV so the
    steps after it configure against this checkout.

.PARAMETER Destination
    Where the checkout lives. Defaults to RUNNER_TEMP\vcpkg in CI and
    %USERPROFILE%\vcpkg-baseline elsewhere.
#>
[CmdletBinding()]
param(
    [string]$Destination = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot

$manifest = Get-Content (Join-Path $repoRoot 'vcpkg.json') -Raw | ConvertFrom-Json
$baseline = $manifest.'builtin-baseline'
if ($baseline -notmatch '^[0-9a-f]{40}$') {
    throw "vcpkg.json has no 40-character builtin-baseline (got '$baseline')"
}

if (-not $Destination) {
    if ($env:RUNNER_TEMP) { $Destination = Join-Path $env:RUNNER_TEMP 'vcpkg' }
    else { $Destination = Join-Path $env:USERPROFILE 'vcpkg-baseline' }
}

if (-not (Test-Path (Join-Path $Destination '.git'))) {
    & git clone --quiet https://github.com/microsoft/vcpkg $Destination
    if ($LASTEXITCODE -ne 0) { throw 'git clone of microsoft/vcpkg failed' }
}
& git -C $Destination fetch --quiet origin
if ($LASTEXITCODE -ne 0) { throw 'git fetch of microsoft/vcpkg failed' }
& git -C $Destination -c advice.detachedHead=false checkout --quiet $baseline
if ($LASTEXITCODE -ne 0) { throw "vcpkg has no commit $baseline" }

& (Join-Path $Destination 'bootstrap-vcpkg.bat') -disableMetrics
if ($LASTEXITCODE -ne 0) { throw 'vcpkg bootstrap failed' }

$env:VCPKG_ROOT = $Destination
if ($env:GITHUB_ENV) { Add-Content -Path $env:GITHUB_ENV -Value "VCPKG_ROOT=$Destination" }
Write-Output "vcpkg at $baseline in $Destination"
