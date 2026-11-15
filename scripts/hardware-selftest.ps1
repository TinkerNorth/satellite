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

    Close every program that holds a game controller open first (Steam with a
    PlayStation pad refuses removals and fails the removal checks).

        scripts/hardware-selftest.ps1
        scripts/hardware-selftest.ps1 -SkipComposite
        scripts/hardware-selftest.ps1 -Helper "C:\Program Files\Satellite\satellite-hm-helper.exe"

    Requires an elevated PowerShell. Exit code 0 when every step passed.

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
#>
[CmdletBinding()]
param(
    [string]$Helper = '',
    [string]$Probe = '',
    [string]$LogDir = '',
    [switch]$SkipComposite
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
    Write-Warning ("These programs may hold a game controller open and refuse removals: " +
        (($holders | Select-Object -ExpandProperty ProcessName -Unique) -join ', '))
}

$results = [System.Collections.Generic.List[object]]::new()
function Run-Step([string]$Name, [string]$Exe, [string[]]$Arguments, [string]$Log) {
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
Run-Step 'selftest' $Helper @('selftest', '--log', (Join-Path $LogDir 'selftest.helper.log')) (Join-Path $LogDir 'selftest.log')
if (-not $SkipComposite) {
    Run-Step 'selftest --composite' $Helper @('selftest', '--composite', '--log', (Join-Path $LogDir 'selftest-composite.helper.log')) (Join-Path $LogDir 'selftest-composite.log')
}
Run-Step 'devnode probe' $Probe @() (Join-Path $LogDir 'devnode-probe.log')

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
