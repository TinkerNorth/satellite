<#
.SYNOPSIS
    Hold a built satellite-hm-helper.exe to src/platform/windows/driver_pins.h.

.DESCRIPTION
    Runs the helper's `sdk-version` command, which prints the HIDMaestro SDK
    release compiled into it and the hidmaestro.inf DriverVer that SDK embeds,
    and fails when either differs from the pins the dashboard's driver banner
    compares installed drivers against. This checks the artifact the installer
    ships, after `dotnet publish`, where fetch-redist.ps1 checks the staged SDK
    before it.

        scripts/verify-helper-sdk.ps1
        scripts/verify-helper-sdk.ps1 -Helper "C:\Program Files\Satellite\satellite-hm-helper.exe"

.PARAMETER Helper
    Path to satellite-hm-helper.exe. Defaults to the helper project's publish output.

.PARAMETER PinsHeader
    The driver pins header. Defaults to <repo>/src/platform/windows/driver_pins.h.
#>
[CmdletBinding()]
param(
    [string]$Helper = '',
    [string]$PinsHeader = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'redist-functions.ps1')

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Helper) {
    $Helper = Join-Path $RepoRoot 'helper\hidmaestro\bin\Release\net10.0-windows10.0.26100.0\win-x64\publish\satellite-hm-helper.exe'
}
if (-not $PinsHeader) { $PinsHeader = Join-Path $RepoRoot 'src\platform\windows\driver_pins.h' }
if (-not (Test-Path $Helper)) { throw "$Helper not found; run dotnet publish helper/hidmaestro first" }

$Lines = @(& $Helper sdk-version)
if ($LASTEXITCODE -ne 0) { throw "$Helper sdk-version exited $LASTEXITCODE" }
$Reported = ConvertFrom-HelperSdkVersionOutput $Lines
$Name = Split-Path -Leaf $Helper
Assert-HmSdkMatchesPins -Subject $Name -SdkVersion $Reported.Sdk -DriverVersion $Reported.Driver -Pins (Get-DriverPins $PinsHeader)
Write-Host "[OK]   $Name carries HIDMaestro SDK $($Reported.Sdk), driver INF $($Reported.Driver); driver_pins.h agrees"
