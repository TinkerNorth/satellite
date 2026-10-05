<#
.SYNOPSIS
    Tests for scripts/redist-functions.ps1 and the staging flow of
    scripts/fetch-redist.ps1, run against fake zips in a temp directory.

.DESCRIPTION
    Same shape as tests/test_util.h: a Test label, Expect / ExpectEq /
    ExpectThrows, one process exit code. Runs under Windows PowerShell or
    pwsh; the end-to-end cases launch fetch-redist.ps1 under the same host.

        powershell -NoProfile -ExecutionPolicy Bypass -File tests\test_fetch_redist.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
. (Join-Path $repoRoot 'scripts\redist-functions.ps1')

$script:pass = 0
$script:fail = 0
$script:current = ''
$latin1 = [Text.Encoding]::GetEncoding(28591)

function Test([string]$Name) { $script:current = $Name }

function Expect([bool]$Condition, [string]$What) {
    if ($Condition) { $script:pass++; return }
    $script:fail++
    Write-Host "  FAIL [$($script:current)] $What"
}

function ExpectEq($Actual, $Expected, [string]$What) {
    if ("$Actual" -eq "$Expected") { $script:pass++; return }
    $script:fail++
    Write-Host "  FAIL [$($script:current)] $What (got '$Actual' vs '$Expected')"
}

function ExpectThrows([scriptblock]$Block, [string]$Needle) {
    $message = $null
    try { & $Block | Out-Null } catch { $message = $_.Exception.Message }
    if ($null -eq $message) {
        $script:fail++
        Write-Host "  FAIL [$($script:current)] expected an error containing '$Needle'"
        return
    }
    if ($message -like "*$Needle*") { $script:pass++; return }
    $script:fail++
    Write-Host "  FAIL [$($script:current)] error was '$message', expected it to contain '$Needle'"
}

function NewTempDir {
    $dir = Join-Path ([IO.Path]::GetTempPath()) ('satellite-redist-test-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dir | Out-Null
    return $dir
}

function HashOf([string]$Path) { return (Get-FileHash $Path -Algorithm SHA256).Hash.ToLower() }

function HmInf([string]$DriverVer) {
    return @(
        (([string][char]0xEF) + ([string][char]0xBB) + ([string][char]0xBF) + ';'),
        '; HIDMaestro UMDF2 Virtual HID Minidriver INF',
        ';',
        '[Version]',
        'Signature   = "$WINDOWS NT$"',
        'Class       = HIDClass',
        'ClassGuid   = {745a17a0-74d3-11d0-b6fe-00a0c90f57da}',
        'Provider    = %ProviderName%',
        'CatalogFile = hidmaestro.cat',
        "DriverVer   = 09/27/2026,$DriverVer",
        'PnpLockdown = 1',
        '',
        '[DestinationDirs]',
        'DefaultDestDir  = 13',
        'UMDriverCopy    = 13',
        ''
    ) -join "`r`n"
}

function XusbInf([string]$DriverVer) {
    return @(
        '[Version]',
        'Signature   = "$WINDOWS NT$"',
        '; System class is not matched by any classifier branch.',
        'Class       = System',
        'ClassGuid   = {4D36E97D-E325-11CE-BFC1-08002BE10318}',
        'Provider    = %ProviderName%',
        'CatalogFile = hidmaestro_xusb.cat',
        "DriverVer   = 09/27/2026,$DriverVer",
        'PnpLockdown = 1',
        '',
        '[DestinationDirs]',
        'DefaultDestDir  = 13',
        ''
    ) -join "`r`n"
}

function UsbipInf([string]$Date, [string]$DriverVer) {
    return @(
        '; Copyright (c) 2022-2026 Vadym Hrynchyshyn',
        ';',
        '; usbip2_ude.inf',
        ';',
        '',
        '[Version]',
        'Signature="$WINDOWS NT$"',
        'Class=USB',
        'ClassGuid={36FC9E60-C465-11CF-8056-444553540000} ; Devguid.h, GUID_DEVCLASS_USB',
        'Provider=%Manufacturer%',
        'CatalogFile=usbip2_ude.cat',
        'PnpLockDown=1',
        "DriverVer = $Date,$DriverVer",
        '',
        '[DestinationDirs]',
        'DefaultDestDir = 13 ; Driver package isolation',
        ''
    ) -join "`n"
}

function Junk([int]$Seed) {
    return ([string][char]0) + 'MZ' + ([string][char]7) + "resource$Seed" + ([string][char]0) + ([string][char]255)
}

function SdkLayout([string]$HmDriverVer) {
    return (Junk 1) + (HmInf $HmDriverVer) + (Junk 2) + (XusbInf $HmDriverVer) + (Junk 3) +
           (HmInf $HmDriverVer) + (Junk 4) + (XusbInf $HmDriverVer) + (Junk 5) +
           (UsbipInf '09/22/2026' '12.1.30.403') + (Junk 6) + (UsbipInf '09/27/2026' '19.23.45.571') + (Junk 7)
}

function WriteLatin1([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, $latin1) }

function NewFakeSdkZip([string]$Dir, [string]$ZipName, [string]$CoreDllText, [string[]]$Omit = @()) {
    $src = Join-Path $Dir ('zip-src-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path (Join-Path $src 'HIDMaestroTest') | Out-Null
    $members = @{
        'HIDMaestro.Core.dll'                          = $CoreDllText
        'HIDMaestroTest\Microsoft.Windows.SDK.NET.dll' = 'projection assembly'
        'HIDMaestroTest\WinRT.Runtime.dll'             = 'winrt runtime'
        'README.md'                                    = 'readme'
    }
    foreach ($member in $members.Keys) {
        if ($Omit -contains (Split-Path -Leaf $member)) { continue }
        WriteLatin1 (Join-Path $src $member) $members[$member]
    }
    $zip = Join-Path $Dir $ZipName
    if (Test-Path $zip) { Remove-Item $zip }
    Compress-Archive -Path (Join-Path $src '*') -DestinationPath $zip
    Remove-Item -Recurse -Force $src
    return $zip
}

function WriteSums([string]$Dir, [string[]]$Files) {
    $lines = @('# test pins')
    foreach ($file in $Files) { $lines += ((HashOf (Join-Path $Dir $file)) + ' *' + $file) }
    Set-Content -Path (Join-Path $Dir 'SHA256SUMS') -Value $lines -Encoding ascii
}

function WritePinsHeader([string]$Path, [string]$Sdk, [string]$Driver) {
    @(
        '// SPDX-License-Identifier: LGPL-3.0-or-later',
        '#pragma once',
        '',
        '#define SATELLITE_VIGEMBUS_BUNDLED_VERSION "1.22.0"',
        '#define SATELLITE_VIGEMBUS_BUNDLED_DRIVER_VERSION "1.21.442.0"',
        "#define SATELLITE_HIDMAESTRO_SDK_VERSION `"$Sdk`"",
        "#define SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION `"$Driver`""
    ) | Set-Content -Path $Path -Encoding ascii
}

function InvokeFetchRedist([string]$RedistDir, [string]$PinsHeader, [string]$Tag) {
    $hostExe = (Get-Process -Id $PID).Path
    $script = Join-Path $repoRoot 'scripts\fetch-redist.ps1'
    $out = Join-Path $RedistDir "run-$Tag.out.txt"
    $err = Join-Path $RedistDir "run-$Tag.err.txt"
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', "`"$script`"",
              '-RedistDir', "`"$RedistDir`"", '-PinsHeader', "`"$PinsHeader`"")
    $proc = Start-Process -FilePath $hostExe -ArgumentList $argv -Wait -PassThru -NoNewWindow `
        -RedirectStandardOutput $out -RedirectStandardError $err
    $text = [string](Get-Content $out -Raw) + [string](Get-Content $err -Raw)
    return @{ Code = $proc.ExitCode; Output = $text }
}

function StagedCoreText([string]$RedistDir) {
    return [IO.File]::ReadAllText((Join-Path $RedistDir 'hidmaestro\HIDMaestro.Core.dll'), $latin1)
}

$realPinsHeader = Join-Path $repoRoot 'src\platform\windows\driver_pins.h'
$wanted = @('HIDMaestro.Core.dll', 'Microsoft.Windows.SDK.NET.dll', 'WinRT.Runtime.dll')

function TestSha256Sums {
    $dir = NewTempDir
    try {
        Test 'Read-Sha256Sums: binary-mode lines parse, comments and blanks are skipped, hashes lowercase'
        $sums = Join-Path $dir 'SHA256SUMS'
        @(
            '# pins',
            '',
            (('A' * 64) + ' *ViGEmBus_1.22.0_x64_x86_arm64.exe'),
            (('b' * 64) + '  HIDMaestro-v1.9.2.zip')
        ) | Set-Content -Path $sums -Encoding ascii
        $parsed = Read-Sha256Sums $sums
        ExpectEq $parsed.Count 2 'two entries'
        ExpectEq $parsed['ViGEmBus_1.22.0_x64_x86_arm64.exe'] ('a' * 64) 'starred entry, lowercased'
        ExpectEq $parsed['HIDMaestro-v1.9.2.zip'] ('b' * 64) 'unstarred entry'

        Test 'Read-Sha256Sums: a malformed line is a warning, not an entry'
        @(('c' * 10) + ' *short.zip') | Set-Content -Path $sums -Encoding ascii
        $parsed = Read-Sha256Sums $sums -WarningAction SilentlyContinue
        ExpectEq $parsed.Count 0 'nothing parsed'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestDriverPins {
    $dir = NewTempDir
    try {
        Test 'Get-DriverPins: reads the four defines'
        $header = Join-Path $dir 'driver_pins.h'
        WritePinsHeader $header '1.9.2' '1.8.1.2248'
        $pins = Get-DriverPins $header
        ExpectEq $pins['SATELLITE_VIGEMBUS_BUNDLED_VERSION'] '1.22.0' 'vigem release'
        ExpectEq $pins['SATELLITE_VIGEMBUS_BUNDLED_DRIVER_VERSION'] '1.21.442.0' 'vigem driver'
        ExpectEq $pins['SATELLITE_HIDMAESTRO_SDK_VERSION'] '1.9.2' 'sdk'
        ExpectEq $pins['SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION'] '1.8.1.2248' 'driver'

        Test 'Get-DriverPins: a header without the HIDMaestro pins is refused by macro name'
        @('#define SATELLITE_VIGEMBUS_BUNDLED_VERSION "1.22.0"') | Set-Content -Path $header -Encoding ascii
        ExpectThrows { Get-DriverPins $header } 'SATELLITE_HIDMAESTRO_SDK_VERSION'

        Test 'Get-DriverPins: the real header pins dotted versions'
        $real = Get-DriverPins $realPinsHeader
        Expect ($real['SATELLITE_HIDMAESTRO_SDK_VERSION'] -match '^\d+\.\d+\.\d+$') 'sdk is MAJOR.MINOR.PATCH'
        Expect ($real['SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION'] -match '^\d+(\.\d+){1,3}$') 'driver is dotted'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestInfDriverVersions {
    Test 'Get-InfDriverVersions: one INF yields its catalog and DriverVer'
    $one = Get-InfDriverVersions (HmInf '1.8.1.2248')
    ExpectEq $one.Count 1 'one catalog'
    ExpectEq ($one['hidmaestro.cat'] -join ',') '1.8.1.2248' 'version'

    Test 'Get-InfDriverVersions: the 1.9.2 SDK layout, four INFs amid binary data'
    $layout = Get-InfDriverVersions (SdkLayout '1.8.1.2248')
    ExpectEq $layout.Count 3 'three catalogs'
    ExpectEq ($layout['hidmaestro.cat'] -join ',') '1.8.1.2248' 'hidmaestro.inf, x64 and arm64 agree'
    ExpectEq ($layout['hidmaestro_xusb.cat'] -join ',') '1.8.1.2248' 'the xusb companion INF'
    ExpectEq ($layout['usbip2_ude.cat'] -join ',') '12.1.30.403,19.23.45.571' 'both usbip transports, in order'

    Test 'Get-InfDriverVersions: key order inside [Version] does not matter'
    $swapped = "[Version]`r`nDriverVer = 01/02/2026,2.0.0.1`r`nCatalogFile = other.cat`r`n`r`n[Strings]`r`nx = y`r`n"
    ExpectEq ((Get-InfDriverVersions $swapped)['other.cat'] -join ',') '2.0.0.1' 'DriverVer before CatalogFile'

    Test 'Get-InfDriverVersions: an architecture-decorated CatalogFile key counts'
    $decorated = "[Version]`nCatalogFile.NTamd64 = Arch.CAT`nDriverVer=03/04/2026,3.1.4.1`n[Manufacturer]`n"
    ExpectEq ((Get-InfDriverVersions $decorated)['arch.cat'] -join ',') '3.1.4.1' 'lowercased catalog name'

    Test 'Get-InfDriverVersions: comments, dates without versions and the last section are handled'
    $commented = "[Version]`r`n; DriverVer = 01/01/2026,9.9.9.9`r`nCatalogFile = c.cat`r`nDriverVer = 01/01/2026`r`n"
    ExpectEq (Get-InfDriverVersions $commented).Count 0 'a commented-out or versionless DriverVer records nothing'
    $tail = "[Version]`r`nCatalogFile = tail.cat`r`nDriverVer = 05/06/2026,1.2.3.4"
    ExpectEq ((Get-InfDriverVersions $tail)['tail.cat'] -join ',') '1.2.3.4' 'a section at end of text still parses'

    Test 'Get-InfDriverVersions: a [Version] without CatalogFile and text without sections yield nothing'
    ExpectEq (Get-InfDriverVersions "[Version]`nDriverVer = 05/06/2026,1.2.3.4`n[Other]`n").Count 0 'no catalog, no entry'
    ExpectEq (Get-InfDriverVersions 'DriverVer = 05/06/2026,1.2.3.4').Count 0 'no section, no entry'
    ExpectEq (Get-InfDriverVersions '').Count 0 'empty text'
}

function TestHmSdkDriverVersion {
    $dir = NewTempDir
    try {
        $dll = Join-Path $dir 'HIDMaestro.Core.dll'

        Test 'Get-HmSdkDriverVersion: reads hidmaestro.inf out of the 1.9.2 layout'
        WriteLatin1 $dll (SdkLayout '1.8.1.2248')
        ExpectEq (Get-HmSdkDriverVersion $dll) '1.8.1.2248' '1.9.2'

        Test 'Get-HmSdkDriverVersion: the 1.7.0 layout reads as its own driver'
        WriteLatin1 $dll ((Junk 1) + (HmInf '1.4.7.12') + (Junk 2) + (XusbInf '1.4.7.12'))
        ExpectEq (Get-HmSdkDriverVersion $dll) '1.4.7.12' '1.7.0'

        Test 'Get-HmSdkDriverVersion: an assembly without hidmaestro.inf is refused'
        WriteLatin1 $dll ((Junk 1) + (UsbipInf '09/22/2026' '12.1.30.403'))
        ExpectThrows { Get-HmSdkDriverVersion $dll } 'hidmaestro.inf'

        Test 'Get-HmSdkDriverVersion: two different hidmaestro.inf versions are refused, not picked between'
        WriteLatin1 $dll ((HmInf '1.8.1.2248') + (Junk 1) + (HmInf '1.8.1.958'))
        ExpectThrows { Get-HmSdkDriverVersion $dll } '1.8.1.958'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestVersionsAndAsserts {
    Test 'Get-HmSdkVersionFromZipName: the release zip name carries the SDK version'
    ExpectEq (Get-HmSdkVersionFromZipName 'HIDMaestro-v1.9.2.zip') '1.9.2' 'v-prefixed'
    ExpectThrows { Get-HmSdkVersionFromZipName 'HIDMaestro-1.9.2.zip' } 'HIDMaestro-1.9.2.zip'
    ExpectThrows { Get-HmSdkVersionFromZipName 'HIDMaestro-v1.9.zip' } 'MAJOR.MINOR.PATCH'

    $pins = @{
        'SATELLITE_HIDMAESTRO_SDK_VERSION'            = '1.9.2'
        'SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION' = '1.8.1.2248'
    }

    Test 'Assert-HmSdkMatchesPins: agreement passes'
    Assert-HmSdkMatchesPins -Subject 'x' -SdkVersion '1.9.2' -DriverVersion '1.8.1.2248' -Pins $pins
    $script:pass++

    Test 'Assert-HmSdkMatchesPins: an SDK drift names the subject, both versions and the macro'
    ExpectThrows { Assert-HmSdkMatchesPins -Subject 'HIDMaestro.Core.dll' -SdkVersion '1.7.0' -DriverVersion '1.8.1.2248' -Pins $pins } 'SATELLITE_HIDMAESTRO_SDK_VERSION'
    ExpectThrows { Assert-HmSdkMatchesPins -Subject 'HIDMaestro.Core.dll' -SdkVersion '1.7.0' -DriverVersion '1.8.1.2248' -Pins $pins } '1.7.0'
    ExpectThrows { Assert-HmSdkMatchesPins -Subject 'HIDMaestro.Core.dll' -SdkVersion '1.7.0' -DriverVersion '1.8.1.2248' -Pins $pins } '1.9.2'
    ExpectThrows { Assert-HmSdkMatchesPins -Subject 'HIDMaestro.Core.dll' -SdkVersion '1.7.0' -DriverVersion '1.8.1.2248' -Pins $pins } 'HIDMaestro.Core.dll'

    Test 'Assert-HmSdkMatchesPins: a driver drift names both versions and the macro'
    ExpectThrows { Assert-HmSdkMatchesPins -Subject 'x' -SdkVersion '1.9.2' -DriverVersion '1.4.7.12' -Pins $pins } 'SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION'
    ExpectThrows { Assert-HmSdkMatchesPins -Subject 'x' -SdkVersion '1.9.2' -DriverVersion '1.4.7.12' -Pins $pins } '1.4.7.12'
    ExpectThrows { Assert-HmSdkMatchesPins -Subject 'x' -SdkVersion '1.9.2' -DriverVersion '1.4.7.12' -Pins $pins } '1.8.1.2248'

    Test 'ConvertFrom-HelperSdkVersionOutput: sdk-version lines, build metadata stripped'
    $reported = ConvertFrom-HelperSdkVersionOutput @('sdk 1.9.2+34e5cd1890eed8d6d2ce8c0477ce19629665b94b', 'driver 1.8.1.2248')
    ExpectEq $reported.Sdk '1.9.2' 'sdk without +sha'
    ExpectEq $reported.Driver '1.8.1.2248' 'driver'
    $plain = ConvertFrom-HelperSdkVersionOutput @('noise', 'sdk 1.7.0', 'driver 1.4.7.12', '')
    ExpectEq $plain.Sdk '1.7.0' 'sdk without metadata, other lines ignored'

    Test 'ConvertFrom-HelperSdkVersionOutput: a missing line is refused'
    ExpectThrows { ConvertFrom-HelperSdkVersionOutput @('sdk 1.9.2') } 'driver'
    ExpectThrows { ConvertFrom-HelperSdkVersionOutput @() } 'sdk'
}

function TestRepoPinsAgree {
    Test 'the redist table, SHA256SUMS and the SBOM name the releases driver_pins.h pins'
    $pins = Get-DriverPins $realPinsHeader
    $table = Get-RedistTable
    $hm = Get-HmRedistEntry $table
    $vigem = $table | Where-Object { $_.Name -like 'ViGEmBus *' } | Select-Object -First 1
    ExpectEq (Get-HmSdkVersionFromZipName $hm.Filename) $pins['SATELLITE_HIDMAESTRO_SDK_VERSION'] 'HIDMaestro zip name'
    Expect ($hm.Url -like "*/v$($pins['SATELLITE_HIDMAESTRO_SDK_VERSION'])/*") 'HIDMaestro download URL tag'
    Expect ($vigem.Filename -like "ViGEmBus_$($pins['SATELLITE_VIGEMBUS_BUNDLED_VERSION'])_*") 'ViGEmBus installer name'
    Expect ($vigem.Url -like "*/v$($pins['SATELLITE_VIGEMBUS_BUNDLED_VERSION'])/*") 'ViGEmBus download URL tag'
    $sums = Read-Sha256Sums (Join-Path $repoRoot 'redist\SHA256SUMS')
    Expect $sums.ContainsKey($hm.Filename) 'SHA256SUMS lists the HIDMaestro zip'
    Expect $sums.ContainsKey($vigem.Filename) 'SHA256SUMS lists the ViGEmBus installer'
    $sbom = Get-Content (Join-Path $repoRoot 'scripts\generate-sbom.ps1') -Raw
    Expect ($sbom -match 'pkg:generic/hidmaestro@(\d+\.\d+\.\d+)') 'SBOM has a HIDMaestro purl'
    ExpectEq $Matches[1] $pins['SATELLITE_HIDMAESTRO_SDK_VERSION'] 'SBOM HIDMaestro version'
    Expect ($sbom -match 'pkg:generic/vigembus@(\d+\.\d+\.\d+)') 'SBOM has a ViGEmBus purl'
    ExpectEq $Matches[1] $pins['SATELLITE_VIGEMBUS_BUNDLED_VERSION'] 'SBOM ViGEmBus version'
}

function TestStaging {
    $dir = NewTempDir
    try {
        $sdkDir = Join-Path $dir 'hidmaestro'
        $sentinel = Join-Path $sdkDir 'left-by-an-older-zip.txt'
        $zipA = NewFakeSdkZip $dir 'HIDMaestro-vA.zip' (SdkLayout '1.8.1.2248')
        $stampA = (HashOf $zipA) + ' *HIDMaestro-vA.zip'

        Test 'Sync-HmSdkStaging: an empty directory is staged from the zip and stamped with the zip'
        ExpectEq (Sync-HmSdkStaging -ZipPath $zipA -SdkDir $sdkDir -Wanted $wanted -StampLine $stampA) $true 'staged'
        foreach ($name in $wanted) { Expect (Test-Path (Join-Path $sdkDir $name)) "$name present" }
        ExpectEq ([string](Get-Content (Get-HmStagingStampPath $sdkDir) -Raw)).Trim() $stampA 'stamp'
        Expect ((StagedCoreText $dir) -like '*1.8.1.2248*') 'the assembly is the zip''s'

        Test 'Sync-HmSdkStaging: a staging that came from this zip is left alone'
        Set-Content -Path $sentinel -Value 'untouched'
        ExpectEq (Sync-HmSdkStaging -ZipPath $zipA -SdkDir $sdkDir -Wanted $wanted -StampLine $stampA) $false 'not re-staged'
        Expect (Test-Path $sentinel) 'directory untouched'

        Test 'Sync-HmSdkStaging: a different zip replaces the staging and drops what the old one left'
        $zipB = NewFakeSdkZip $dir 'HIDMaestro-vB.zip' ((Junk 1) + (HmInf '1.4.7.12') + (XusbInf '1.4.7.12'))
        $stampB = (HashOf $zipB) + ' *HIDMaestro-vB.zip'
        ExpectEq (Sync-HmSdkStaging -ZipPath $zipB -SdkDir $sdkDir -Wanted $wanted -StampLine $stampB) $true 're-staged'
        Expect (-not (Test-Path $sentinel)) 'stale file gone'
        Expect ((StagedCoreText $dir) -like '*1.4.7.12*') 'the assembly is the new zip''s'
        ExpectEq ([string](Get-Content (Get-HmStagingStampPath $sdkDir) -Raw)).Trim() $stampB 'stamp follows'

        Test 'Sync-HmSdkStaging: staged files with no stamp are re-staged (a checkout staged before stamps existed)'
        Remove-Item (Get-HmStagingStampPath $sdkDir)
        Set-Content -Path $sentinel -Value 'legacy'
        ExpectEq (Sync-HmSdkStaging -ZipPath $zipB -SdkDir $sdkDir -Wanted $wanted -StampLine $stampB) $true 're-staged'
        Expect (-not (Test-Path $sentinel)) 'legacy file gone'

        Test 'Sync-HmSdkStaging: a missing assembly means not current'
        Remove-Item (Join-Path $sdkDir 'WinRT.Runtime.dll')
        ExpectEq (Test-HmStagingCurrent -SdkDir $sdkDir -Wanted $wanted -StampLine $stampB) $false 'not current'
        ExpectEq (Sync-HmSdkStaging -ZipPath $zipB -SdkDir $sdkDir -Wanted $wanted -StampLine $stampB) $true 're-staged'
        Expect (Test-Path (Join-Path $sdkDir 'WinRT.Runtime.dll')) 'assembly back'

        Test 'Sync-HmSdkStaging: -Force re-stages a current staging'
        Set-Content -Path $sentinel -Value 'forced'
        ExpectEq (Sync-HmSdkStaging -ZipPath $zipB -SdkDir $sdkDir -Wanted $wanted -StampLine $stampB -Force) $true 'forced'
        Expect (-not (Test-Path $sentinel)) 'forced staging is clean'

        Test 'Sync-HmSdkStaging: a zip without one of the assemblies is refused by name'
        $zipBad = NewFakeSdkZip $dir 'HIDMaestro-vBad.zip' (HmInf '1.0.0.0') -Omit @('WinRT.Runtime.dll')
        ExpectThrows { Sync-HmSdkStaging -ZipPath $zipBad -SdkDir $sdkDir -Wanted $wanted -StampLine 'bad *HIDMaestro-vBad.zip' } 'WinRT.Runtime.dll'
        ExpectEq (Test-HmStagingCurrent -SdkDir $sdkDir -Wanted $wanted -StampLine $stampB) $false 'a failed staging is not current'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestFetchRedistEndToEnd {
    $dir = NewTempDir
    try {
        $table = Get-RedistTable
        $hm = Get-HmRedistEntry $table
        $vigem = $table | Where-Object { $_.Name -like 'ViGEmBus *' } | Select-Object -First 1
        $sdkPin = Get-HmSdkVersionFromZipName $hm.Filename
        $header = Join-Path $dir 'driver_pins.h'
        Set-Content -Path (Join-Path $dir $vigem.Filename) -Value 'not an installer' -Encoding ascii
        NewFakeSdkZip $dir $hm.Filename (SdkLayout '1.8.1.2248') | Out-Null
        WriteSums $dir @($vigem.Filename, $hm.Filename)
        WritePinsHeader $header $sdkPin '1.8.1.2248'

        Test 'fetch-redist.ps1: stages the pinned zip and reports the driver INF it embeds'
        $run = InvokeFetchRedist $dir $header 'first'
        ExpectEq $run.Code 0 "exit code; output: $($run.Output)"
        Expect ($run.Output -like '*1.8.1.2248*') 'prints the embedded DriverVer'
        Expect (Test-Path (Join-Path $dir 'hidmaestro\HIDMaestro.Core.dll')) 'assembly staged'
        Expect (Test-Path (Get-HmStagingStampPath (Join-Path $dir 'hidmaestro'))) 'stamp written'

        Test 'fetch-redist.ps1: a second run with nothing changed stages nothing'
        $run = InvokeFetchRedist $dir $header 'again'
        ExpectEq $run.Code 0 "exit code; output: $($run.Output)"
        Expect ($run.Output -like '*already staged*') 'reports the staging as current'

        Test 'fetch-redist.ps1: a bumped zip is re-staged on the next run without -Force'
        NewFakeSdkZip $dir $hm.Filename (SdkLayout '1.9.9.9') | Out-Null
        WriteSums $dir @($vigem.Filename, $hm.Filename)
        WritePinsHeader $header $sdkPin '1.9.9.9'
        $run = InvokeFetchRedist $dir $header 'bump'
        ExpectEq $run.Code 0 "exit code; output: $($run.Output)"
        Expect ((StagedCoreText $dir) -like '*1.9.9.9*') 'the staged assembly is the new zip''s'
        Expect ($run.Output -like '*1.9.9.9*') 'prints the new DriverVer'

        Test 'fetch-redist.ps1: a staged SDK whose driver INF disagrees with driver_pins.h fails'
        WritePinsHeader $header $sdkPin '1.8.1.2248'
        $run = InvokeFetchRedist $dir $header 'drift'
        Expect ($run.Code -ne 0) 'non-zero exit'
        Expect ($run.Output -like '*1.9.9.9*') 'names the embedded version'
        Expect ($run.Output -like '*1.8.1.2248*') 'names the pinned version'
        Expect ($run.Output -like '*SATELLITE_HIDMAESTRO_BUNDLED_DRIVER_VERSION*') 'names the pin'

        Test 'fetch-redist.ps1: a table whose zip is not the pinned SDK release fails before fetching'
        WritePinsHeader $header '0.0.1' '1.9.9.9'
        $run = InvokeFetchRedist $dir $header 'sdkpin'
        Expect ($run.Code -ne 0) 'non-zero exit'
        Expect ($run.Output -like '*SATELLITE_HIDMAESTRO_SDK_VERSION*') 'names the pin'
        Expect ($run.Output -like '*0.0.1*') 'names the pinned version'
        Expect ($run.Output -notlike '*Downloading*') 'nothing was fetched'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

Write-Host 'Running redist staging tests...'
Write-Host ''
TestSha256Sums
TestDriverPins
TestInfDriverVersions
TestHmSdkDriverVersion
TestVersionsAndAsserts
TestRepoPinsAgree
TestStaging
TestFetchRedistEndToEnd
Write-Host ''
Write-Host '=== Test Results ==='
Write-Host "  Passed: $($script:pass)"
Write-Host "  Failed: $($script:fail)"
if ($script:fail -eq 0) { Write-Host '  STATUS: ALL PASSED' } else { Write-Host '  STATUS: FAILED' }
if ($script:fail -ne 0) { exit 1 }
exit 0
