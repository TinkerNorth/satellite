<#
.SYNOPSIS
    Silent install / launch / repair / uninstall assertions for
    SatelliteSetup.exe.

.DESCRIPTION
    Ported from dish-windows scripts/test-installer-roundtrip.ps1 and adapted
    to Satellite's installer contract ({autopf} + admin context, HKLM ARP,
    bundled ViGEmBus driver). Asserts the CONTRACT the app and the docs rely
    on, against a real disk and a real registry:

      1. A fresh /VERYSILENT install into a directory WITH A SPACE lands
         satellite.exe and the web UI assets. The bundled ViGEmBus driver is
         explicitly skipped (/VIGEM=skip): a kernel driver install has no
         business succeeding-or-hanging inside a CI assertion, and the
         installer documents the switch for exactly this.
      2. The ARP entry exists (native or WOW6432Node view) and points at the
         install.
      3. The installed exe actually starts and stays up: satellite runs
         headless-fallback without a tray host, so "stays up 10 s" is the
         portable liveness check.
      4. A silent re-run over the same version (Inno's repair/upgrade path)
         exits 0 and leaves the install intact.
      5. A silent uninstall removes the files, the directory and the ARP
         entry. The uninstaller's tail is asynchronous; removal is polled for
         up to 30 s.

    Requires an elevated context (the installer is PrivilegesRequired=admin);
    GitHub's windows runners provide one.

.PARAMETER Setup
    Path to the compiled dist\SatelliteSetup.exe under test.

.PARAMETER WorkRoot
    Where the sandbox lives. Defaults to RUNNER_TEMP (CI) or TEMP.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Setup,
    [string]$WorkRoot = ''
)

$ErrorActionPreference = 'Stop'

# Inno derives the ARP key from the AppId in installer.iss. The installer is
# admin-context, so the entry lands in HKLM, in the native view or the
# WOW6432Node view depending on ArchitecturesInstallIn64BitMode; probe both.
$appId = '{B8F3A2E1-7D4C-4E5F-9A1B-3C6D8E0F2A4B}'
$arpCandidates = @(
    "HKLM:\Software\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1",
    "HKLM:\Software\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\${appId}_is1"
)
function Get-ArpKey {
    foreach ($k in $arpCandidates) { if (Test-Path $k) { return $k } }
    return $null
}

$Setup = (Resolve-Path $Setup).Path
if (-not $WorkRoot) { $WorkRoot = if ($env:RUNNER_TEMP) { $env:RUNNER_TEMP } else { $env:TEMP } }
# The space is on purpose: quoting bugs in the handoff or the uninstaller
# show up here or in the field.
$sandbox = Join-Path $WorkRoot ("satellite roundtrip " + [IO.Path]::GetRandomFileName().Split('.')[0])
$installDir = Join-Path $sandbox 'Satellite App'
New-Item -ItemType Directory -Force -Path $sandbox | Out-Null

$failures = [System.Collections.Generic.List[string]]::new()
function Assert([bool]$Condition, [string]$What) {
    if ($Condition) { "  ok: $What" } else { $failures.Add($What); Write-Warning "FAIL: $What" }
}

function Invoke-Setup([string[]]$Arguments) {
    $p = Start-Process -FilePath $Setup -ArgumentList $Arguments -Wait -PassThru
    return $p.ExitCode
}

try {
    # --- 1. Fresh silent install (driver skipped) ----------------------------
    'step 1: fresh silent install (/VIGEM=skip)'
    $log1 = Join-Path $sandbox 'install-1.log'
    $code = Invoke-Setup @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/VIGEM=skip', "/DIR=`"$installDir`"", "/LOG=`"$log1`"")
    Assert ($code -eq 0) "fresh install exits 0 (got $code)"
    Assert (Test-Path (Join-Path $installDir 'satellite.exe')) 'satellite.exe installed'
    Assert ($null -ne (Get-ChildItem -Path $installDir -Recurse -Filter 'dashboard.js' -ErrorAction SilentlyContinue | Select-Object -First 1)) 'web UI assets installed'
    $unins = Get-ChildItem -Path $installDir -Filter 'unins*.exe' -ErrorAction SilentlyContinue
    Assert ($null -ne $unins -and $unins.Count -ge 1) 'uninstaller present'

    # --- 2. ARP entry --------------------------------------------------------
    'step 2: Add/Remove Programs entry'
    $arpKey = Get-ArpKey
    Assert ($null -ne $arpKey) 'ARP key exists (HKLM native or WOW6432Node)'
    if ($arpKey) {
        $arp = Get-ItemProperty $arpKey
        Assert ($arp.DisplayName -like 'Satellite*') "DisplayName starts with 'Satellite' (got '$($arp.DisplayName)')"
        Assert ($arp.InstallLocation.TrimEnd('\') -eq $installDir) 'InstallLocation points at the sandbox install'
        Assert (-not [string]::IsNullOrWhiteSpace($arp.DisplayVersion)) 'DisplayVersion recorded'
    }

    # --- 3. Launch smoke: the installed exe must start and stay up -----------
    # No tray host and no ViGEmBus on a runner: satellite is built to run
    # anyway (headless fallback; backend probe reports the driver missing).
    # A bundle that dies inside 10 s is exactly the class of failure the
    # build tree hides.
    'step 3: launch smoke (10 s liveness)'
    $exe = Join-Path $installDir 'satellite.exe'
    $app = Start-Process -FilePath $exe -PassThru -WorkingDirectory $installDir
    Start-Sleep -Seconds 10
    Assert (-not $app.HasExited) 'satellite.exe still running after 10 s'
    if (-not $app.HasExited) {
        Stop-Process -Id $app.Id -Force
        $app.WaitForExit(15000) | Out-Null
    }

    # --- 4. Silent re-run over the same version ------------------------------
    'step 4: silent re-run (repair / upgrade shape)'
    $log2 = Join-Path $sandbox 'install-2.log'
    $code = Invoke-Setup @('/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART', '/VIGEM=skip', "/LOG=`"$log2`"")
    Assert ($code -eq 0) "re-run exits 0 (got $code)"
    Assert (Test-Path (Join-Path $installDir 'satellite.exe')) 'satellite.exe still present after re-run'
    Assert ($null -ne (Get-ArpKey)) 'ARP key survives the re-run'

    # --- 5. Silent uninstall -------------------------------------------------
    'step 5: silent uninstall'
    $uninsExe = (Get-ChildItem -Path $installDir -Filter 'unins*.exe' | Select-Object -First 1).FullName
    $p = Start-Process -FilePath $uninsExe -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait -PassThru
    Assert ($p.ExitCode -eq 0) "uninstall exits 0 (got $($p.ExitCode))"
    # The uninstaller deletes itself and the directory through a helper after
    # exiting; poll rather than assert immediately.
    $deadline = [DateTime]::UtcNow.AddSeconds(30)
    while ([DateTime]::UtcNow -lt $deadline -and ((Test-Path $installDir) -or ($null -ne (Get-ArpKey)))) {
        Start-Sleep -Milliseconds 500
    }
    Assert (-not (Test-Path (Join-Path $installDir 'satellite.exe'))) 'satellite.exe removed'
    Assert (-not (Test-Path $installDir)) 'install directory removed'
    Assert ($null -eq (Get-ArpKey)) 'ARP key removed'
}
finally {
    if (Test-Path $installDir) {
        # A failed run must not strand an ARP entry pointing into the sandbox.
        $uninsExe = (Get-ChildItem -Path $installDir -Filter 'unins*.exe' -ErrorAction SilentlyContinue | Select-Object -First 1)
        if ($uninsExe) {
            Start-Process -FilePath $uninsExe.FullName -ArgumentList '/VERYSILENT', '/SUPPRESSMSGBOXES', '/NORESTART' -Wait | Out-Null
            Start-Sleep -Seconds 3
        }
    }
    Remove-Item -Recurse -Force $sandbox -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    throw "Installer round-trip failed: $($failures.Count) assertion(s): $($failures -join '; ')"
}
'Installer round-trip: all assertions passed.'
