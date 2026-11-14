<#
.SYNOPSIS
    Functions shared by scripts/fetch-redist.ps1, scripts/verify-helper-sdk.ps1
    and tests/test_fetch_redist.ps1: the redistributable table, SHA256SUMS
    parsing, HIDMaestro SDK staging keyed to the pinned zip, and the gate that
    holds src/platform/windows/driver_pins.h to what the staged SDK embeds.

.DESCRIPTION
    Dot-source this file; it defines functions only and runs nothing.
#>

$HmSdkStagedFiles = @('HIDMaestro.Core.dll', 'Microsoft.Windows.SDK.NET.dll', 'WinRT.Runtime.dll', 'LICENSE', 'THIRD-PARTY-NOTICES.txt')
$HmSatelliteProfilesDir = 'hidmaestro-profiles'
$HmSatelliteProfileCopies = @('satellite-dualsense', 'satellite-dualsense-composite', 'satellite-dualsense-edge', 'satellite-dualsense-edge-composite', 'satellite-dualshock-4-v2', 'satellite-dualshock-4-v2-composite', 'satellite-switch-pro')
$HmCatalogProfilesDir = 'profiles'
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
            Name     = 'HIDMaestro 2.1.0'
            Url      = 'https://github.com/hifihedgehog/HIDMaestro/releases/download/v2.1.0/HIDMaestro-v2.1.0.zip'
            Filename = 'HIDMaestro-v2.1.0.zip'
        }
        @{
            Name           = 'HIDMaestro 2.1.0 catalog profile dualsense'
            Url            = 'https://raw.githubusercontent.com/hifihedgehog/HIDMaestro/v2.1.0/profiles/sony/dualsense.json'
            Filename       = 'HIDMaestro-v2.1.0-dualsense.json'
            CatalogProfile = 'dualsense'
        }
        @{
            Name           = 'HIDMaestro 2.1.0 catalog profile dualsense-composite'
            Url            = 'https://raw.githubusercontent.com/hifihedgehog/HIDMaestro/v2.1.0/profiles/sony/dualsense-composite.json'
            Filename       = 'HIDMaestro-v2.1.0-dualsense-composite.json'
            CatalogProfile = 'dualsense-composite'
        }
        @{
            Name           = 'HIDMaestro 2.1.0 catalog profile dualsense-edge'
            Url            = 'https://raw.githubusercontent.com/hifihedgehog/HIDMaestro/v2.1.0/profiles/sony/dualsense-edge.json'
            Filename       = 'HIDMaestro-v2.1.0-dualsense-edge.json'
            CatalogProfile = 'dualsense-edge'
        }
        @{
            Name           = 'HIDMaestro 2.1.0 catalog profile dualsense-edge-composite'
            Url            = 'https://raw.githubusercontent.com/hifihedgehog/HIDMaestro/v2.1.0/profiles/sony/dualsense-edge-composite.json'
            Filename       = 'HIDMaestro-v2.1.0-dualsense-edge-composite.json'
            CatalogProfile = 'dualsense-edge-composite'
        }
        @{
            Name           = 'HIDMaestro 2.1.0 catalog profile dualshock-4-v2'
            Url            = 'https://raw.githubusercontent.com/hifihedgehog/HIDMaestro/v2.1.0/profiles/sony/dualshock-4-v2.json'
            Filename       = 'HIDMaestro-v2.1.0-dualshock-4-v2.json'
            CatalogProfile = 'dualshock-4-v2'
        }
        @{
            Name           = 'HIDMaestro 2.1.0 catalog profile dualshock-4-v2-composite'
            Url            = 'https://raw.githubusercontent.com/hifihedgehog/HIDMaestro/v2.1.0/profiles/sony/dualshock-4-v2-composite.json'
            Filename       = 'HIDMaestro-v2.1.0-dualshock-4-v2-composite.json'
            CatalogProfile = 'dualshock-4-v2-composite'
        }
        @{
            Name           = 'HIDMaestro 2.1.0 catalog profile switch-pro'
            Url            = 'https://raw.githubusercontent.com/hifihedgehog/HIDMaestro/v2.1.0/profiles/nintendo/switch-pro.json'
            Filename       = 'HIDMaestro-v2.1.0-switch-pro.json'
            CatalogProfile = 'switch-pro'
        }
    )
}

function Get-HmRedistEntry([array]$Table) {
    return ($Table | Where-Object { $_.Filename -match '^HIDMaestro-v\d+\.\d+\.\d+\.zip$' } | Select-Object -First 1)
}

function Get-HmCatalogProfileEntries([array]$Table) {
    return ,@($Table | Where-Object { $_.ContainsKey('CatalogProfile') })
}

function Get-HmCatalogProfileVersion([string]$Filename) {
    if ($Filename -notmatch '^HIDMaestro-v(\d+\.\d+\.\d+)-([a-z0-9-]+)\.json$') {
        throw "$Filename is not named HIDMaestro-v<MAJOR.MINOR.PATCH>-<profile>.json"
    }
    return @{ Version = $Matches[1]; Profile = $Matches[2] }
}

function Assert-HmCatalogProfileEntry([hashtable]$Entry, [hashtable]$Pins) {
    $named = Get-HmCatalogProfileVersion $Entry.Filename
    Assert-HmSdkVersionPinned -Subject "The redist table's $($Entry.Filename)" -SdkVersion $named.Version -Pins $Pins
    if ($named.Profile -ne $Entry.CatalogProfile) {
        throw "$($Entry.Filename) is named for profile '$($named.Profile)' but stages '$($Entry.CatalogProfile)'"
    }
    if ($Entry.Url -notlike "*/v$($named.Version)/profiles/*/$($Entry.CatalogProfile).json") {
        throw "$($Entry.Filename) does not download profiles/*/$($Entry.CatalogProfile).json at the v$($named.Version) tag: $($Entry.Url)"
    }
}

function Sync-HmCatalogProfiles([string]$RedistDir, [string]$SdkDir, [array]$Entries) {
    $dir = Join-Path $SdkDir $HmCatalogProfilesDir
    if (-not (Test-Path $dir)) { New-Item -ItemType Directory -Path $dir | Out-Null }
    $staged = @()
    foreach ($entry in $Entries) {
        $source = Join-Path $RedistDir $entry.Filename
        if (-not (Test-Path $source)) { throw "$source is missing; fetch it first" }
        $target = Join-Path $dir "$($entry.CatalogProfile).json"
        Copy-Item $source $target -Force
        $staged += $target
    }
    return $staged
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
