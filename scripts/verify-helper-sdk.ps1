<#
.SYNOPSIS
    Hold a built satellite-hm-helper.exe to src/platform/windows/driver_pins.h.

.DESCRIPTION
    Runs the helper's `sdk-version` command, which prints the HIDMaestro SDK
    release compiled into it and the hidmaestro.inf DriverVer that SDK embeds,
    and fails when either differs from the pins the dashboard's driver banner
    compares installed drivers against. Runs its `check-sdk` command first,
    which holds what the helper restates of the SDK's internals to the SDK by
    reflection: the identity tag satellite.exe's version-section check expects,
    the SDK version the SDK writes into that section, that section's layout as
    src/core/hidmaestro_version.h states it, and the XUSB companion and timeout
    scale the helper reports at plug. This checks the artifact the installer
    ships, after `dotnet publish`, where fetch-redist.ps1 checks the staged SDK
    before it.

        scripts/verify-helper-sdk.ps1
        scripts/verify-helper-sdk.ps1 -Helper "C:\Program Files\Satellite\satellite-hm-helper.exe"

.PARAMETER Helper
    Path to satellite-hm-helper.exe. Defaults to the helper project's publish output.

.PARAMETER PinsHeader
    The driver pins header. Defaults to <repo>/src/platform/windows/driver_pins.h.

.PARAMETER VersionHeader
    satellite.exe's version-section header. Defaults to <repo>/src/core/hidmaestro_version.h.
#>
[CmdletBinding()]
param(
    [string]$Helper = '',
    [string]$PinsHeader = '',
    [string]$VersionHeader = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'redist-functions.ps1')

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Helper) {
    $Helper = Join-Path $RepoRoot 'helper\hidmaestro\bin\Release\net10.0-windows10.0.26100.0\win-x64\publish\satellite-hm-helper.exe'
}
if (-not $PinsHeader) { $PinsHeader = Join-Path $RepoRoot 'src\platform\windows\driver_pins.h' }
if (-not $VersionHeader) { $VersionHeader = Join-Path $RepoRoot 'src\core\hidmaestro_version.h' }
if (-not (Test-Path $Helper)) { throw "$Helper not found; run dotnet publish helper/hidmaestro first" }

$SdkPin = (Get-DriverPins $PinsHeader)[$HmSdkVersionPin]
& $Helper check-sdk --sdk-pin $SdkPin --version-header $VersionHeader
if ($LASTEXITCODE -ne 0) { throw "$Helper check-sdk exited $LASTEXITCODE" }
Write-Host "[OK]   IdentityTags is the pinned SDK's DeviceIdentity, the SDK writes SDK $SdkPin into a version section laid out as $VersionHeader says, and the helper reads companions and the timeout scale as the SDK does"

$Lines = @(& $Helper sdk-version)
if ($LASTEXITCODE -ne 0) { throw "$Helper sdk-version exited $LASTEXITCODE" }
$Reported = ConvertFrom-HelperSdkVersionOutput $Lines
$Name = Split-Path -Leaf $Helper
Assert-HmSdkMatchesPins -Subject $Name -SdkVersion $Reported.Sdk -DriverVersion $Reported.Driver -Pins (Get-DriverPins $PinsHeader)
Write-Host "[OK]   $Name carries HIDMaestro SDK $($Reported.Sdk), driver INF $($Reported.Driver); driver_pins.h agrees"

$ProfilesDir = Join-Path (Split-Path -Parent $Helper) $HmSatelliteProfilesDir
foreach ($Copy in $HmSatelliteProfileCopies) {
    $File = Join-Path $ProfilesDir "$Copy.json"
    if (-not (Test-Path $File)) { throw "$File is missing; dotnet publish exports it from the staged SDK" }
}
& $Helper check-profiles --dir $ProfilesDir
if ($LASTEXITCODE -ne 0) { throw "$Name check-profiles exited $LASTEXITCODE" }
Write-Host "[OK]   $ProfilesDir holds the stream-free copies of the Sony profiles and the SDK loads them"
