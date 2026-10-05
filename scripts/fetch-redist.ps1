<#
.SYNOPSIS
    Download and verify the third-party binaries bundled into SatelliteSetup.exe.

.DESCRIPTION
    The Inno Setup installer (installer.iss) ships two driver prerequisites:
    the ViGEmBus driver installer, and the HIDMaestro SDK release that
    helper/hidmaestro builds satellite-hm-helper.exe from. Neither binary is
    committed to git; this script fetches them on demand and verifies each
    SHA-256 against the pinned hash in redist/SHA256SUMS before letting the
    helper build or iscc consume them.

    The HIDMaestro SDK assemblies are unpacked into redist/hidmaestro/ (the
    helper project's reference path). That directory is stamped with the hash
    of the zip it came from and is re-staged whenever the pinned zip changes,
    so a bumped pin cannot leave an older SDK in the helper.

    After staging, the embedded hidmaestro.inf DriverVer and the SDK release
    are compared with src/platform/windows/driver_pins.h, the pins the
    dashboard's driver banner compares installed drivers against. A mismatch
    fails the build: the installer would otherwise ship a driver other than the
    one the banner says it ships.

    Idempotent. A file that already matches its pinned hash is not downloaded,
    and a staging that already came from the pinned zip is left alone.

.PARAMETER Force
    Re-download and re-stage even if everything matches.

.PARAMETER RedistDir
    The redist directory. Defaults to <repo>/redist.

.PARAMETER PinsHeader
    The driver pins header. Defaults to <repo>/src/platform/windows/driver_pins.h.

.EXAMPLE
    pwsh scripts/fetch-redist.ps1
    iscc installer.iss
#>
[CmdletBinding()]
param(
    [switch]$Force,
    [string]$RedistDir = '',
    [string]$PinsHeader = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'redist-functions.ps1')

$RepoRoot = Split-Path -Parent $PSScriptRoot
if (-not $RedistDir) { $RedistDir = Join-Path $RepoRoot 'redist' }
if (-not $PinsHeader) { $PinsHeader = Join-Path $RepoRoot 'src\platform\windows\driver_pins.h' }
$SumFile = Join-Path $RedistDir 'SHA256SUMS'

if (-not (Test-Path $RedistDir)) {
    New-Item -ItemType Directory -Path $RedistDir | Out-Null
}
if (-not (Test-Path $SumFile)) {
    throw "Pinned hash file not found: $SumFile"
}

$Pins = Get-DriverPins $PinsHeader
$Redistributables = Get-RedistTable
$HmEntry = Get-HmRedistEntry $Redistributables
$HmSdkVersion = Get-HmSdkVersionFromZipName $HmEntry.Filename
Assert-HmSdkVersionPinned -Subject "The redist table's $($HmEntry.Filename)" -SdkVersion $HmSdkVersion -Pins $Pins

$ExpectedHashes = Read-Sha256Sums $SumFile

$AnyDownloaded = $false
foreach ($Item in $Redistributables) {
    $Path = Join-Path $RedistDir $Item.Filename
    $Expected = $ExpectedHashes[$Item.Filename]
    if (-not $Expected) {
        throw "$($Item.Filename) is not listed in redist/SHA256SUMS"
    }

    if ((Test-Path $Path) -and -not $Force) {
        $Actual = (Get-FileHash $Path -Algorithm SHA256).Hash.ToLower()
        if ($Actual -eq $Expected) {
            Write-Host "[OK]   $($Item.Name): already current ($($Item.Filename))"
            continue
        }
        Write-Host "[WARN] $($Item.Filename) hash mismatch; will re-download"
        Remove-Item $Path
    }

    Write-Host "[INFO] Downloading $($Item.Name)..."
    Write-Host "       $($Item.Url)"
    try {
        $OldPref = $ProgressPreference
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Item.Url -OutFile $Path -UseBasicParsing
    } finally {
        $ProgressPreference = $OldPref
    }

    $Actual = (Get-FileHash $Path -Algorithm SHA256).Hash.ToLower()
    if ($Actual -ne $Expected) {
        Remove-Item $Path -ErrorAction SilentlyContinue
        throw "SHA-256 mismatch for $($Item.Filename)`n  expected: $Expected`n  actual:   $Actual"
    }
    Write-Host "[OK]   verified $($Item.Filename)"
    $AnyDownloaded = $true
}

$HmZip = Join-Path $RedistDir $HmEntry.Filename
$HmSdkDir = Join-Path $RedistDir 'hidmaestro'
$StampLine = "$($ExpectedHashes[$HmEntry.Filename]) *$($HmEntry.Filename)"
$Staged = Sync-HmSdkStaging -ZipPath $HmZip -SdkDir $HmSdkDir -Wanted $HmSdkAssemblies -StampLine $StampLine -Force:$Force
if ($Staged) {
    Write-Host "[OK]   redist/hidmaestro/ staged from $($HmEntry.Filename) ($($HmSdkAssemblies -join ', '))"
} else {
    Write-Host "[OK]   redist/hidmaestro/ already staged from $($HmEntry.Filename)"
}

$CoreDll = Join-Path $HmSdkDir 'HIDMaestro.Core.dll'
$DriverVersion = Get-HmSdkDriverVersion $CoreDll
Assert-HmSdkMatchesPins -Subject 'redist/hidmaestro/HIDMaestro.Core.dll' -SdkVersion $HmSdkVersion -DriverVersion $DriverVersion -Pins $Pins
Write-Host "[OK]   HIDMaestro SDK $HmSdkVersion embeds driver INF $DriverVersion; driver_pins.h agrees"

if ($AnyDownloaded) {
    Write-Host ''
    Write-Host '=== redist/ ready ==='
} else {
    Write-Host ''
    Write-Host '=== redist/ already up-to-date ==='
}
