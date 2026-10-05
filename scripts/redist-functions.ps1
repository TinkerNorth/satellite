<#
.SYNOPSIS
    Functions shared by scripts/fetch-redist.ps1, scripts/verify-helper-sdk.ps1
    and tests/test_fetch_redist.ps1: the redistributable table, SHA256SUMS
    parsing, HIDMaestro SDK staging keyed to the pinned zip, and the gate that
    holds src/platform/windows/driver_pins.h to what the staged SDK embeds.

.DESCRIPTION
    Dot-source this file; it defines functions only and runs nothing.
#>

$HmSdkAssemblies = @('HIDMaestro.Core.dll', 'Microsoft.Windows.SDK.NET.dll', 'WinRT.Runtime.dll')
$HmInfCatalog = 'hidmaestro.cat'
$HmStagingStampName = 'staged-from.sha256'
$HmSdkVersionPin = 'SATELLITE_HIDMAESTRO_SDK_VERSION'
$HmDriverVersionPin = 'SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION'

function Get-RedistTable {
    return @(
        @{
            Name     = 'ViGEmBus 1.22.0'
            Url      = 'https://github.com/nefarius/ViGEmBus/releases/download/v1.22.0/ViGEmBus_1.22.0_x64_x86_arm64.exe'
            Filename = 'ViGEmBus_1.22.0_x64_x86_arm64.exe'
        }
        @{
            Name     = 'HIDMaestro 1.9.2'
            Url      = 'https://github.com/hifihedgehog/HIDMaestro/releases/download/v1.9.2/HIDMaestro-v1.9.2.zip'
            Filename = 'HIDMaestro-v1.9.2.zip'
        }
    )
}

function Get-HmRedistEntry([array]$Table) {
    return ($Table | Where-Object { $_.Name -like 'HIDMaestro *' } | Select-Object -First 1)
}

function Read-Sha256Sums {
    [CmdletBinding()]
    param([string]$Path)
    $hashes = @{}
    foreach ($line in Get-Content $Path) {
        $trimmed = $line.Trim()
        if (-not $trimmed -or $trimmed.StartsWith('#')) { continue }
        if ($trimmed -match '^([0-9a-fA-F]{64})\s+\*?(.+)$') {
            $hashes[$Matches[2]] = $Matches[1].ToLower()
        } else {
            Write-Warning "Unparseable line in ${Path}: $trimmed"
        }
    }
    return $hashes
}

function Get-DriverPins([string]$Path) {
    $pins = @{}
    foreach ($line in Get-Content $Path) {
        if ($line -match '^\s*#define\s+(SATELLITE_\w+)\s+"([^"]*)"') {
            $pins[$Matches[1]] = $Matches[2]
        }
    }
    foreach ($name in @($HmSdkVersionPin, $HmDriverVersionPin)) {
        if (-not $pins.ContainsKey($name)) { throw "$Path defines no $name" }
    }
    return $pins
}

function Get-InfDriverVersions([string]$Text) {
    $versions = @{}
    $sectionPattern = '(?im)\[Version\][ \t]*\r?\n([\s\S]*?)(?=^[ \t]*\[[^\]\r\n]+\][ \t]*\r?$|\z)'
    foreach ($section in [regex]::Matches($Text, $sectionPattern)) {
        $catalog = ''
        $driverVer = ''
        foreach ($rawLine in ($section.Groups[1].Value -split "`r?`n")) {
            $line = $rawLine -replace ';.*$', ''
            if ($line -match '^\s*CatalogFile(?:\.\w+)?\s*=\s*(\S+)') { $catalog = $Matches[1].ToLower() }
            if ($line -match '^\s*DriverVer\s*=\s*[0-9/]+\s*,\s*([0-9]+(?:\.[0-9]+)*)') { $driverVer = $Matches[1] }
        }
        if (-not $catalog -or -not $driverVer) { continue }
        if (-not $versions.ContainsKey($catalog)) { $versions[$catalog] = @() }
        if ($versions[$catalog] -notcontains $driverVer) { $versions[$catalog] += $driverVer }
    }
    return $versions
}

function Get-HmSdkDriverVersion([string]$CoreDllPath) {
    $bytes = [IO.File]::ReadAllBytes($CoreDllPath)
    $text = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $versions = Get-InfDriverVersions $text
    if (-not $versions.ContainsKey($HmInfCatalog)) {
        throw "$CoreDllPath embeds no hidmaestro.inf with a DriverVer line"
    }
    $found = @($versions[$HmInfCatalog])
    if ($found.Count -ne 1) {
        throw "$CoreDllPath embeds $($found.Count) different hidmaestro.inf DriverVer values ($($found -join ', ')); expected one"
    }
    return $found[0]
}

function Get-HmSdkVersionFromZipName([string]$ZipName) {
    if ($ZipName -notmatch '^HIDMaestro-v(\d+\.\d+\.\d+)\.zip$') {
        throw "$ZipName is not named HIDMaestro-v<MAJOR.MINOR.PATCH>.zip"
    }
    return $Matches[1]
}

function Assert-HmSdkVersionPinned([string]$Subject, [string]$SdkVersion, [hashtable]$Pins) {
    $pinned = $Pins[$HmSdkVersionPin]
    if ($SdkVersion -ne $pinned) {
        throw "$Subject is HIDMaestro SDK $SdkVersion, but driver_pins.h pins $HmSdkVersionPin `"$pinned`""
    }
}

function Assert-HmDriverVersionPinned([string]$Subject, [string]$DriverVersion, [hashtable]$Pins) {
    $pinned = $Pins[$HmDriverVersionPin]
    if ($DriverVersion -ne $pinned) {
        throw "$Subject embeds hidmaestro.inf DriverVer $DriverVersion, but driver_pins.h pins $HmDriverVersionPin `"$pinned`""
    }
}

function Assert-HmSdkMatchesPins([string]$Subject, [string]$SdkVersion, [string]$DriverVersion, [hashtable]$Pins) {
    Assert-HmSdkVersionPinned -Subject $Subject -SdkVersion $SdkVersion -Pins $Pins
    Assert-HmDriverVersionPinned -Subject $Subject -DriverVersion $DriverVersion -Pins $Pins
}

function ConvertFrom-HelperSdkVersionOutput([string[]]$Lines) {
    $sdk = ''
    $driver = ''
    foreach ($line in $Lines) {
        if ($line -match '^sdk\s+(\S+)') { $sdk = ($Matches[1] -split '\+')[0] }
        if ($line -match '^driver\s+(\S+)') { $driver = $Matches[1] }
    }
    if (-not $sdk -or -not $driver) {
        throw "satellite-hm-helper sdk-version printed no sdk and driver lines: $($Lines -join ' | ')"
    }
    return @{ Sdk = $sdk; Driver = $driver }
}

function Get-HmStagingStampPath([string]$SdkDir) {
    return (Join-Path $SdkDir $HmStagingStampName)
}

function Test-HmStagingCurrent([string]$SdkDir, [string[]]$Wanted, [string]$StampLine) {
    foreach ($name in $Wanted) {
        if (-not (Test-Path (Join-Path $SdkDir $name))) { return $false }
    }
    $stamp = Get-HmStagingStampPath $SdkDir
    if (-not (Test-Path $stamp)) { return $false }
    return (([string](Get-Content $stamp -Raw)).Trim() -eq $StampLine)
}

function Sync-HmSdkStaging([string]$ZipPath, [string]$SdkDir, [string[]]$Wanted, [string]$StampLine, [switch]$Force) {
    if (-not $Force -and (Test-HmStagingCurrent -SdkDir $SdkDir -Wanted $Wanted -StampLine $StampLine)) {
        return $false
    }
    $staging = "$SdkDir-staging"
    if (Test-Path $staging) { Remove-Item -Recurse -Force $staging }
    Expand-Archive -Path $ZipPath -DestinationPath $staging -Force
    if (Test-Path $SdkDir) { Remove-Item -Recurse -Force $SdkDir }
    New-Item -ItemType Directory -Path $SdkDir | Out-Null
    foreach ($name in $Wanted) {
        $found = Get-ChildItem -Path $staging -Recurse -File -Filter $name | Select-Object -First 1
        if (-not $found) { throw "$ZipPath does not contain $name" }
        Copy-Item $found.FullName (Join-Path $SdkDir $name)
    }
    Set-Content -Path (Get-HmStagingStampPath $SdkDir) -Value $StampLine -Encoding ascii
    Remove-Item -Recurse -Force $staging
    return $true
}
