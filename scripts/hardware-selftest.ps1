<#
.SYNOPSIS
    Run the HIDMaestro checks that need an elevated process on a machine with
    the driver, and keep their logs.

.DESCRIPTION
    CI builds and unit-tests the HIDMaestro helper and satellite.exe, but it
    never creates a virtual controller: that needs an elevated process and the
    HIDMaestro driver. This script runs, in order, and saves each one's output
    under -LogDir:

      1. satellite-hm-helper.exe selftest             (plain UMDF2 personas)
      2. satellite-hm-helper.exe selftest --composite (the USB/IP persona; installs
                                                       HIDMaestro's USB transport)
      3. test_windows_devnode_probe.exe               (Plug and Play behaviour the
                                                       device watch builds on)

    Close every program that holds a game controller open first: the script
    refuses to start while Steam or DS4Windows runs, since a program holding a
    pad open refuses its removal, which costs about 5.4 s each and fails the
    removal checks. -IgnoreHolders runs anyway.

        hardware-selftest.bat
        hardware-selftest.bat -SkipComposite
        hardware-selftest.bat -Helper "C:\Program Files\Satellite\satellite-hm-helper.exe"

    Requires an elevated prompt. The .bat at the repo root forwards here with
    -ExecutionPolicy Bypass, so it runs where the policy refuses .ps1 files.
    Exit code 0 when every step passed.

.PARAMETER Helper
    Path to satellite-hm-helper.exe. Defaults to the helper project's publish output.

.PARAMETER Probe
    Path to test_windows_devnode_probe.exe. Defaults to the one at the repo root
    (a Windows build writes it there); a MinGW build also needs C:\msys64\mingw64\bin
    on PATH, which this script prepends when present.

.PARAMETER LogDir
    Where the logs go. Defaults to <repo>\build-hardware-selftest\<timestamp>.

.PARAMETER SkipComposite
    Leave out `selftest --composite`.

.PARAMETER IgnoreHolders
    Run even while a program known to hold game controllers open is running.
#>
[CmdletBinding()]
param(
    [string]$Helper = '',
    [string]$Probe = '',
    [string]$LogDir = '',
    [switch]$SkipComposite,
    [switch]$IgnoreHolders
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
if (-not $Helper) {
    $Helper = Join-Path $repoRoot 'helper\hidmaestro\bin\Release\net10.0-windows10.0.26100.0\win-x64\publish\satellite-hm-helper.exe'
}
if (-not $Probe) { $Probe = Join-Path $repoRoot 'test_windows_devnode_probe.exe' }
if (-not $LogDir) { $LogDir = Join-Path $repoRoot ('build-hardware-selftest\' + (Get-Date -Format 'yyyyMMdd-HHmmss')) }

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run from an elevated PowerShell: creating virtual controllers and software devices needs it.'
}
if (-not (Test-Path $Helper)) { throw "$Helper not found; run dotnet publish helper/hidmaestro/satellite-hm-helper.csproj -c Release first" }
if (-not (Test-Path $Probe)) { throw "$Probe not found; build the Windows tree first (build-tests.bat)" }

$mingwBin = 'C:\msys64\mingw64\bin'
if (Test-Path $mingwBin) { $env:PATH = "$mingwBin;$env:PATH" }
New-Item -ItemType Directory -Force $LogDir | Out-Null

$holders = Get-Process -Name steam, steamwebhelper, 'DS4Windows', 'ds4windows' -ErrorAction SilentlyContinue
if ($holders) {
    $names = ($holders | Select-Object -ExpandProperty ProcessName -Unique) -join ', '
    if (-not $IgnoreHolders) {
        throw "Close these first, a program that holds a game controller open refuses its removal, which costs about 5.4 s each and fails the removal checks: $names. Pass -IgnoreHolders to run anyway."
    }
    Write-Warning "Running beside programs that may hold game controllers open: $names. A refused removal costs about 5.4 s and fails its check."
}

$results = [System.Collections.Generic.List[object]]::new()
function Invoke-Step([string]$Name, [string]$Exe, [string[]]$Arguments, [string]$Log) {
    Write-Host ''
    Write-Host "=== $Name ===" -ForegroundColor Cyan
    Write-Host "    $Exe $($Arguments -join ' ')"
    & $Exe @Arguments 2>&1 | Tee-Object -FilePath $Log | ForEach-Object { "$_" }
    $exit = $LASTEXITCODE
    $text = Get-Content $Log -ErrorAction SilentlyContinue
    $passed = @($text | Where-Object { $_ -like 'PASS *' }).Count
    $failed = @($text | Where-Object { $_ -like 'FAIL *' }).Count
    $results.Add([pscustomobject]@{ Step = $Name; Exit = $exit; Passed = $passed; Failed = $failed; Log = $Log })
}

Write-Host "helper: $Helper"
& $Helper sdk-version
Invoke-Step 'selftest' $Helper @('selftest', '--log', (Join-Path $LogDir 'selftest.helper.log')) (Join-Path $LogDir 'selftest.log')
if (-not $SkipComposite) {
    Invoke-Step 'selftest --composite' $Helper @('selftest', '--composite', '--log', (Join-Path $LogDir 'selftest-composite.helper.log')) (Join-Path $LogDir 'selftest-composite.log')
}
Invoke-Step 'devnode probe' $Probe @() (Join-Path $LogDir 'devnode-probe.log')

Write-Host ''
Write-Host '=== Results ===' -ForegroundColor Cyan
$results | Format-Table Step, Exit, Passed, Failed, Log -AutoSize | Out-String -Width 200 | Write-Host
$skipped = @($results | Where-Object { $_.Step -eq 'devnode probe' -and $_.Exit -eq 77 })
if ($skipped) { Write-Warning 'The devnode probe skipped: it ran without elevation or without the Plug and Play manager.' }
$broken = @($results | Where-Object { $_.Exit -ne 0 })
if ($broken) {
    Write-Host "FAILED: $(($broken | Select-Object -ExpandProperty Step) -join ', '). Send the files under $LogDir." -ForegroundColor Red
    exit 1
}
Write-Host "All steps passed. Logs: $LogDir" -ForegroundColor Green
exit 0
