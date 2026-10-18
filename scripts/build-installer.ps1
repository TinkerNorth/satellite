<#
.SYNOPSIS
    Build the Windows Inno Setup installer: dist\SatelliteSetup.exe (the
    signed UIAccess variant with -UiAccess)

.DESCRIPTION
    The seven steps release.yml's windows job runs, minus the CI-only signing
    path (locally, scripts/sign.ps1 gates itself on the SATELLITE_SIGN_* /
    CLOUD_SIGN_TOOL environment variables and is skipped when none are set):

      [1] scripts\fetch-redist.ps1  downloads + SHA-256 verifies the redist
                                    binaries, stages redist\hidmaestro\ from
                                    the pinned zip and holds the staged SDK
                                    to src\platform\windows\driver_pins.h
      [2] dotnet publish            builds satellite-hm-helper.exe
                                    (helper\hidmaestro, self-contained)
      [3] scripts\verify-helper-sdk.ps1
                                    holds the published helper to
                                    driver_pins.h (its sdk-version output)
      [4] scripts\sign.ps1          signs satellite.exe (skipped without creds)
      [5] iscc installer.iss        compiles the installer, passing
                                    /DMyAppVersion=<content of /VERSION>
                                    normalized exactly like release.yml
                                    normalizes the tag (strip a leading v and
                                    any -rc/+build suffix), plus /DUiAccess=1
                                    with -UiAccess
      [6] scripts\sign.ps1          signs SatelliteSetup.exe (same gating)
      [7] scripts\generate-sbom.ps1 emits dist\satellite-sbom.cdx.json

    -UiAccess builds what release.yml builds when WIN_SIGN_CERT_BASE64 is set:
    satellite.exe must request uiAccess="true" (built with the CMake option
    -DSATELLITE_UIACCESS=ON) and signing credentials are mandatory, because
    Windows refuses to start a uiAccess executable that is not signed or not
    installed under Program Files. The resulting installer accepts only a
    destination under Program Files.

    Requires: satellite.exe already built (scripts\build.ps1 first), Inno
    Setup 6, .NET SDK 10 (scripts\install-deps.ps1 installs both).

.PARAMETER Iscc
    Path to ISCC.exe. Defaults to PATH, then the standard install locations.

.PARAMETER UiAccess
    Build the signed UIAccess installer (iscc /DUiAccess=1). Fails unless one
    of SATELLITE_SIGN_THUMBPRINT, SATELLITE_SIGN_PFX or CLOUD_SIGN_TOOL is set
    and satellite.exe carries uiAccess="true".
#>
[CmdletBinding()]
param(
    [string]$Iscc = '',
    [switch]$UiAccess
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Step([string]$Text) { Write-Output ''; Write-Output "=== $Text ===" }

if (-not (Test-Path 'satellite.exe')) {
    throw "satellite.exe not found at the repo root. Run scripts\build.ps1 first."
}

if ($UiAccess) {
    if (-not ($env:SATELLITE_SIGN_THUMBPRINT -or $env:SATELLITE_SIGN_PFX -or $env:CLOUD_SIGN_TOOL)) {
        throw '-UiAccess needs signing credentials: Windows refuses to start an unsigned uiAccess satellite.exe. Set SATELLITE_SIGN_THUMBPRINT, SATELLITE_SIGN_PFX or CLOUD_SIGN_TOOL (see scripts\sign.ps1).'
    }
    $exeText = [Text.Encoding]::ASCII.GetString([IO.File]::ReadAllBytes((Join-Path $repoRoot 'satellite.exe')))
    if (-not $exeText.Contains('uiAccess="true"')) {
        throw 'satellite.exe does not request uiAccess="true". Rebuild it with the CMake option -DSATELLITE_UIACCESS=ON before running with -UiAccess.'
    }
}

# /VERSION is the authoritative version (version-consistency.yml). Normalize
# it the way release.yml normalizes the tag for iscc: strip a leading v, then
# strip any prerelease/build suffix (Inno's VersionInfoVersion wants numeric).
$version = ((Get-Content 'VERSION' -Raw).Trim() -replace '^v', '') -replace '[-+].*$', ''
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION content '$version' is not MAJOR.MINOR.PATCH"
}

Step "[1/7] Fetching redistributables"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'fetch-redist.ps1')
if ($LASTEXITCODE -ne 0) { throw 'fetch-redist.ps1 failed' }

Step "[2/7] Building HIDMaestro helper (dotnet publish)"
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw 'dotnet not found on PATH. Install the .NET SDK: winget install Microsoft.DotNet.SDK.10 (or run scripts\install-deps.ps1).'
}
foreach ($stale in @('helper/hidmaestro/bin', 'helper/hidmaestro/obj')) {
    if (Test-Path $stale) { Remove-Item -Recurse -Force $stale }
}
& dotnet publish helper/hidmaestro/satellite-hm-helper.csproj -c Release
if ($LASTEXITCODE -ne 0) { throw 'dotnet publish helper\hidmaestro failed' }
if (-not (Test-Path 'helper/hidmaestro/bin/Release/net10.0-windows10.0.26100.0/win-x64/publish/satellite-hm-helper.exe')) {
    throw 'dotnet publish did not produce satellite-hm-helper.exe'
}

Step "[3/7] Checking the published helper against driver_pins.h"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'verify-helper-sdk.ps1')
if ($LASTEXITCODE -ne 0) { throw 'verify-helper-sdk.ps1 failed' }

Step "[4/7] Signing satellite.exe"
if (-not ($env:SATELLITE_SIGN_THUMBPRINT -or $env:SATELLITE_SIGN_PFX -or $env:CLOUD_SIGN_TOOL)) {
    Write-Output '[SKIP] No signing credentials in env; satellite.exe will be unsigned.'
    Write-Output '       SmartScreen will warn first-time users. See scripts\sign.ps1 for the env vars.'
} else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sign.ps1') -Files 'satellite.exe'
    if ($LASTEXITCODE -ne 0) { throw 'signing satellite.exe failed' }
}

$isccDefines = @("/DMyAppVersion=$version")
if ($UiAccess) { $isccDefines += '/DUiAccess=1' }
Step "[5/7] Compiling installer (iscc $($isccDefines -join ' ') installer.iss)"
if (-not $Iscc) {
    $onPath = Get-Command iscc.exe -ErrorAction SilentlyContinue
    if ($onPath) { $Iscc = $onPath.Source }
}
if (-not $Iscc) {
    $Iscc = @(
        (Join-Path $env:LOCALAPPDATA 'Programs\Inno Setup 6\ISCC.exe'),
        (Join-Path ${env:ProgramFiles(x86)} 'Inno Setup 6\ISCC.exe'),
        (Join-Path $env:ProgramFiles 'Inno Setup 6\ISCC.exe')
    ) | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $Iscc) {
    throw 'ISCC.exe not found on PATH or in standard install locations. Install Inno Setup 6: winget install JRSoftware.InnoSetup (or run scripts\install-deps.ps1).'
}
Write-Output "[INFO] Using ISCC: $Iscc"
& $Iscc @isccDefines installer.iss
if ($LASTEXITCODE -ne 0) { throw 'iscc failed' }
if (-not (Test-Path 'dist/SatelliteSetup.exe')) { throw 'iscc did not produce dist/SatelliteSetup.exe' }

Step "[6/7] Signing SatelliteSetup.exe"
if (-not ($env:SATELLITE_SIGN_THUMBPRINT -or $env:SATELLITE_SIGN_PFX -or $env:CLOUD_SIGN_TOOL)) {
    Write-Output '[SKIP] No signing credentials in env; SatelliteSetup.exe will be unsigned.'
} else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sign.ps1') -Files 'dist\SatelliteSetup.exe'
    if ($LASTEXITCODE -ne 0) { throw 'signing SatelliteSetup.exe failed' }
}

Step "[7/7] Generating SBOM"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'generate-sbom.ps1')
if ($LASTEXITCODE -ne 0) {
    Write-Output '[WARN] SBOM generation failed; continuing anyway'
}

Write-Output ''
Write-Output "=== Installer built: dist\SatelliteSetup.exe (version $version) ==="
