<#
.SYNOPSIS
    Download and verify the third-party binaries bundled into SatelliteSetup.exe.

.DESCRIPTION
    The Inno Setup installer (installer.iss) ships two driver prerequisites:
    the ViGEmBus driver installer, and the HIDMaestro SDK release that
    helper/hidmaestro builds satellite-hm-helper.exe from. Neither binary is
    committed to git; this script fetches them on demand and verifies each
    SHA-256 against the pinned hash in redist/SHA256SUMS before letting the
    helper build or iscc consume them. The HIDMaestro zip is additionally
    unpacked into redist/hidmaestro/ (the helper project's reference path).

    Idempotent. If a file already exists with the expected hash, nothing
    is downloaded.

.PARAMETER Force
    Re-download even if the existing file matches the pinned hash.

.EXAMPLE
    pwsh scripts/fetch-redist.ps1
    iscc installer.iss
#>
[CmdletBinding()]
param(
    [switch]$Force
)

$ErrorActionPreference = 'Stop'

# To bump: update the URL/filename below, run this script with -Force, then
# replace the matching line in redist/SHA256SUMS with the new hash.
$Redistributables = @(
    @{
        Name     = 'ViGEmBus 1.22.0'
        Url      = 'https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe'
        Filename = 'ViGEmBus_1.22.0_x64_x86_arm64.exe'
    }
    @{
        Name     = 'HIDMaestro 1.7.0'
        Url      = 'https://github.com/hifihedgehog/HIDMaestro/releases/download/v1.7.0/HIDMaestro-v1.7.0.zip'
        Filename = 'HIDMaestro-v1.7.0.zip'
    }
)

$RepoRoot = Split-Path -Parent $PSScriptRoot
$RedistDir = Join-Path $RepoRoot 'redist'
$SumFile = Join-Path $RedistDir 'SHA256SUMS'

if (-not (Test-Path $RedistDir)) {
    New-Item -ItemType Directory -Path $RedistDir | Out-Null
}
if (-not (Test-Path $SumFile)) {
    Write-Error "Pinned hash file not found: $SumFile"
}

# Parse SHA256SUMS (sha256sum binary-mode format: <hash> *<filename>).
$ExpectedHashes = @{}
foreach ($Line in Get-Content $SumFile) {
    $Trimmed = $Line.Trim()
    if (-not $Trimmed -or $Trimmed.StartsWith('#')) { continue }
    if ($Trimmed -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
        $ExpectedHashes[$Matches[2]] = $Matches[1].ToLower()
    } else {
        Write-Warning "Unparseable line in SHA256SUMS: $Trimmed"
    }
}

$AnyDownloaded = $false
foreach ($Item in $Redistributables) {
    $Path = Join-Path $RedistDir $Item.Filename
    $Expected = $ExpectedHashes[$Item.Filename]
    if (-not $Expected) {
        Write-Error "$($Item.Filename) is not listed in redist/SHA256SUMS"
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
        # ProgressPreference = 'SilentlyContinue' makes Invoke-WebRequest ~10x
        # faster on Windows PowerShell 5.1 because it skips the progress bar.
        $OldPref = $ProgressPreference
        $ProgressPreference = 'SilentlyContinue'
        Invoke-WebRequest -Uri $Item.Url -OutFile $Path -UseBasicParsing
    } finally {
        $ProgressPreference = $OldPref
    }

    $Actual = (Get-FileHash $Path -Algorithm SHA256).Hash.ToLower()
    if ($Actual -ne $Expected) {
        Remove-Item $Path -ErrorAction SilentlyContinue
        Write-Error "SHA-256 mismatch for $($Item.Filename)`n  expected: $Expected`n  actual:   $Actual"
    }
    Write-Host "[OK]   verified $($Item.Filename)"
    $AnyDownloaded = $true
}

# Stage the HIDMaestro SDK assemblies where helper/hidmaestro's csproj
# references them. The zip carries HIDMaestro.Core.dll at its root and the
# WinRT projection assemblies inside the tool subfolders.
$HmZip = Join-Path $RedistDir 'HIDMaestro-v1.7.0.zip'
$HmSdkDir = Join-Path $RedistDir 'hidmaestro'
$HmWanted = @('HIDMaestro.Core.dll', 'Microsoft.Windows.SDK.NET.dll', 'WinRT.Runtime.dll')
$HmMissing = $HmWanted | Where-Object { -not (Test-Path (Join-Path $HmSdkDir $_)) }
if ($HmMissing -or $Force) {
    Write-Host "[INFO] Unpacking HIDMaestro SDK assemblies into redist/hidmaestro/"
    $Staging = Join-Path $RedistDir 'hidmaestro-staging'
    if (Test-Path $Staging) { Remove-Item -Recurse -Force $Staging }
    Expand-Archive -Path $HmZip -DestinationPath $Staging -Force
    if (-not (Test-Path $HmSdkDir)) {
        New-Item -ItemType Directory -Path $HmSdkDir | Out-Null
    }
    foreach ($Name in $HmWanted) {
        $Found = Get-ChildItem -Path $Staging -Recurse -Filter $Name |
            Select-Object -First 1
        if (-not $Found) {
            Write-Error "HIDMaestro zip did not contain $Name"
        }
        Copy-Item $Found.FullName (Join-Path $HmSdkDir $Name) -Force
    }
    Remove-Item -Recurse -Force $Staging
    Write-Host "[OK]   redist/hidmaestro/ staged ($($HmWanted -join ', '))"
} else {
    Write-Host "[OK]   HIDMaestro SDK assemblies already staged"
}

if ($AnyDownloaded) {
    Write-Host ''
    Write-Host '=== redist/ ready ==='
} else {
    Write-Host ''
    Write-Host '=== redist/ already up-to-date ==='
}
