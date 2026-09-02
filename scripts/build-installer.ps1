<#
.SYNOPSIS
    Build the Windows Inno Setup installer: dist\SatelliteSetup.exe

.DESCRIPTION
    The six steps release.yml's windows job runs, minus the CI-only signing
    path (locally, scripts/sign.ps1 gates itself on the SATELLITE_SIGN_* /
    CLOUD_SIGN_TOOL environment variables and is skipped when none are set):

      [1] scripts\fetch-redist.ps1  downloads + SHA-256 verifies the redist
                                    binaries, stages redist\hidmaestro\
      [2] dotnet publish            builds satellite-hm-helper.exe
                                    (helper\hidmaestro, self-contained)
      [3] scripts\sign.ps1          signs satellite.exe (skipped without creds)
      [4] iscc installer.iss        compiles the installer, passing
                                    /DMyAppVersion=<content of /VERSION>
                                    normalized exactly like release.yml
                                    normalizes the tag (strip a leading v and
                                    any -rc/+build suffix)
      [5] scripts\sign.ps1          signs SatelliteSetup.exe (same gating)
      [6] scripts\generate-sbom.ps1 emits dist\satellite-sbom.cdx.json

    Requires: satellite.exe already built (scripts\build.ps1 first), Inno
    Setup 6, .NET SDK 10 (scripts\install-deps.ps1 installs both).

.PARAMETER Iscc
    Path to ISCC.exe. Defaults to PATH, then the standard install locations.
#>
[CmdletBinding()]
param(
    [string]$Iscc = ''
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Step([string]$Text) { Write-Output ''; Write-Output "=== $Text ===" }

if (-not (Test-Path 'satellite.exe')) {
    throw "satellite.exe not found at the repo root. Run scripts\build.ps1 first."
}

# /VERSION is the authoritative version (version-consistency.yml). Normalize
# it the way release.yml normalizes the tag for iscc: strip a leading v, then
# strip any prerelease/build suffix (Inno's VersionInfoVersion wants numeric).
$version = ((Get-Content 'VERSION' -Raw).Trim() -replace '^v', '') -replace '[-+].*$', ''
if ($version -notmatch '^\d+\.\d+\.\d+$') {
    throw "VERSION content '$version' is not MAJOR.MINOR.PATCH"
}

Step "[1/6] Fetching redistributables"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'fetch-redist.ps1')
if ($LASTEXITCODE -ne 0) { throw 'fetch-redist.ps1 failed' }

Step "[2/6] Building HIDMaestro helper (dotnet publish)"
if (-not (Get-Command dotnet -ErrorAction SilentlyContinue)) {
    throw 'dotnet not found on PATH. Install the .NET SDK: winget install Microsoft.DotNet.SDK.10 (or run scripts\install-deps.ps1).'
}
& dotnet publish helper/hidmaestro/satellite-hm-helper.csproj -c Release
if ($LASTEXITCODE -ne 0) { throw 'dotnet publish helper\hidmaestro failed' }
if (-not (Test-Path 'helper/hidmaestro/bin/Release/net10.0-windows10.0.26100.0/win-x64/publish/satellite-hm-helper.exe')) {
    throw 'dotnet publish did not produce satellite-hm-helper.exe'
}

Step "[3/6] Signing satellite.exe"
if (-not ($env:SATELLITE_SIGN_THUMBPRINT -or $env:SATELLITE_SIGN_PFX -or $env:CLOUD_SIGN_TOOL)) {
    Write-Output '[SKIP] No signing credentials in env; satellite.exe will be unsigned.'
    Write-Output '       SmartScreen will warn first-time users. See scripts\sign.ps1 for the env vars.'
} else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sign.ps1') -Files 'satellite.exe'
    if ($LASTEXITCODE -ne 0) { throw 'signing satellite.exe failed' }
}

Step "[4/6] Compiling installer (iscc /DMyAppVersion=$version installer.iss)"
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
& $Iscc "/DMyAppVersion=$version" installer.iss
if ($LASTEXITCODE -ne 0) { throw 'iscc failed' }
if (-not (Test-Path 'dist/SatelliteSetup.exe')) { throw 'iscc did not produce dist/SatelliteSetup.exe' }

Step "[5/6] Signing SatelliteSetup.exe"
if (-not ($env:SATELLITE_SIGN_THUMBPRINT -or $env:SATELLITE_SIGN_PFX -or $env:CLOUD_SIGN_TOOL)) {
    Write-Output '[SKIP] No signing credentials in env; SatelliteSetup.exe will be unsigned.'
} else {
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'sign.ps1') -Files 'dist\SatelliteSetup.exe'
    if ($LASTEXITCODE -ne 0) { throw 'signing SatelliteSetup.exe failed' }
}

Step "[6/6] Generating SBOM"
& powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'generate-sbom.ps1')
if ($LASTEXITCODE -ne 0) {
    Write-Output '[WARN] SBOM generation failed; continuing anyway'
}

Write-Output ''
Write-Output "=== Installer built: dist\SatelliteSetup.exe (version $version) ==="
