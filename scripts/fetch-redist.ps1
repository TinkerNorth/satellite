<#
.SYNOPSIS
    Download and verify the third-party installers bundled into SatelliteSetup.exe.

.DESCRIPTION
    The Inno Setup installer (installer.iss) ships a copy of the ViGEmBus
    driver installer as a prerequisite. We do not commit that ~6 MB binary
    to git -- instead, this script fetches it on demand and verifies its
    SHA-256 against the pinned hash in redist/SHA256SUMS before letting
    iscc consume it.

    Idempotent -- safe to re-run. If the file already exists with the
    expected hash, nothing is downloaded.

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

# --- Pinned redistributables -----------------------------------------------
# To bump: update the URL/filename below, run this script with -Force, then
# replace the matching line in redist/SHA256SUMS with the new hash.
$Redistributables = @(
    @{
        Name     = 'ViGEmBus 1.22.0'
        Url      = 'https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe'
        Filename = 'ViGEmBus_1.22.0_x64_x86_arm64.exe'
    }
)

# --- Resolve paths ---------------------------------------------------------
$RepoRoot = Split-Path -Parent $PSScriptRoot
$RedistDir = Join-Path $RepoRoot 'redist'
$SumFile = Join-Path $RedistDir 'SHA256SUMS'

if (-not (Test-Path $RedistDir)) {
    New-Item -ItemType Directory -Path $RedistDir | Out-Null
}
if (-not (Test-Path $SumFile)) {
    Write-Error "Pinned hash file not found: $SumFile"
}

# --- Parse SHA256SUMS (sha256sum binary-mode format: <hash> *<filename>) ---
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

# --- Process each redistributable ------------------------------------------
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
            Write-Host "[OK]   $($Item.Name) -- already current ($($Item.Filename))"
            continue
        }
        Write-Host "[WARN] $($Item.Filename) hash mismatch -- will re-download"
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

if ($AnyDownloaded) {
    Write-Host ''
    Write-Host '=== redist/ ready ==='
} else {
    Write-Host ''
    Write-Host '=== redist/ already up-to-date ==='
}
