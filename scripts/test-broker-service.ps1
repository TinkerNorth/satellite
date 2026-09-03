<#
.SYNOPSIS
    Registers the SatelliteHmBroker service against a built helper, proves it
    hosts in session 0 and answers on its pipe, then removes it again.

.DESCRIPTION
    Exercises the parts of the elevation-free HIDMaestro path that only a
    real Service Control Manager can: the advapi32 dispatcher in
    `satellite-hm-helper.exe service`, the named-pipe trigger, the SDDL that
    grants interactive users SERVICE_START, the pipe DACL, and the broker's
    client authorization (this script is not satellite.exe, so the expected
    answer to `hello` is a refusal naming that). Cleans up after itself unless
    -Keep is passed, in which case the service stays registered so a running
    satellite.exe beside the helper can be tested with a real controller plug.

    Requires an elevated PowerShell (sc.exe create/delete need it).

.PARAMETER Helper
    Path to satellite-hm-helper.exe. Defaults to the dotnet publish output.

.PARAMETER Keep
    Leave the service registered after the checks.
#>
[CmdletBinding()]
param(
    [string]$Helper = (Join-Path $PSScriptRoot '..\helper\hidmaestro\bin\Release\net10.0-windows10.0.26100.0\win-x64\publish\satellite-hm-helper.exe'),
    [switch]$Keep
)

$ErrorActionPreference = 'Stop'
$svc = 'SatelliteHmBroker'
$pipe = 'satellite-hm-broker'
$sddl = 'D:(A;;CCLCSWRPWPDTLOCRRC;;;SY)(A;;CCDCLCSWRPWPDTLOCRSDRCWDWO;;;BA)(A;;CCLCSWRPLOCRRC;;;IU)(A;;CCLCSWLOCRRC;;;SU)S:(AU;FA;CCDCLCSWRPWPDTLOCRSDRCWDWO;;WD)'

$principal = New-Object Security.Principal.WindowsPrincipal([Security.Principal.WindowsIdentity]::GetCurrent())
if (-not $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) {
    throw 'Run from an elevated PowerShell: registering a service needs it.'
}
$Helper = (Resolve-Path $Helper).Path

$failures = [System.Collections.Generic.List[string]]::new()
function Assert([bool]$Condition, [string]$What) {
    if ($Condition) { "  ok: $What" } else { $failures.Add($What); Write-Warning "FAIL: $What" }
}

function Invoke-Hello {
    $c = New-Object System.IO.Pipes.NamedPipeClientStream('.', $pipe, [System.IO.Pipes.PipeDirection]::InOut)
    try {
        $c.Connect(15000)
        $w = New-Object System.IO.StreamWriter($c)
        $w.AutoFlush = $true
        $w.Write('{"op":"hello","protocol":1}' + [char]10)
        $r = New-Object System.IO.StreamReader($c)
        return $r.ReadLine()
    }
    finally { $c.Dispose() }
}

try {
    'step 1: register'
    if (Get-Service -Name $svc -ErrorAction SilentlyContinue) {
        & sc.exe stop $svc | Out-Null
        Start-Sleep -Seconds 2
        & sc.exe delete $svc | Out-Null
        Start-Sleep -Seconds 1
    }
    & sc.exe create $svc binPath= "`"$Helper`" service" start= demand obj= LocalSystem DisplayName= 'Satellite Controller Broker' | Out-Null
    Assert ($null -ne (Get-Service -Name $svc -ErrorAction SilentlyContinue)) 'service registered'
    & sc.exe sdset $svc $sddl | Out-Null
    & sc.exe triggerinfo $svc "start/namedpipe/$pipe" | Out-Null
    $trig = (& sc.exe qtriggerinfo $svc) -join ' '
    Assert ($trig -match [regex]::Escape($pipe)) 'named-pipe start trigger recorded'

    'step 2: explicit start hosts in session 0'
    & sc.exe start $svc | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and (Get-Service $svc).Status -ne 'Running') { Start-Sleep -Milliseconds 250 }
    Assert ((Get-Service $svc).Status -eq 'Running') 'service reaches Running'
    $proc = Get-Process -Name 'satellite-hm-helper' -ErrorAction SilentlyContinue | Where-Object { $_.SessionId -eq 0 } | Select-Object -First 1
    Assert ($null -ne $proc) 'helper process runs in session 0'

    'step 3: pipe answers, and refuses a client that is not satellite.exe'
    $reply = Invoke-Hello
    "  reply: $reply"
    Assert ($reply -like '*"ok":false*' -and $reply -like '*not satellite.exe*') 'hello refused with the image-path reason'

    'step 4: a second connection is served (pipe instance re-created)'
    $reply2 = Invoke-Hello
    Assert ($reply2 -like '*not satellite.exe*') 'second connection answered'

    'step 5: stop, then let the named-pipe trigger start it'
    & sc.exe stop $svc | Out-Null
    $deadline = [DateTime]::UtcNow.AddSeconds(15)
    while ([DateTime]::UtcNow -lt $deadline -and (Get-Service $svc).Status -ne 'Stopped') { Start-Sleep -Milliseconds 250 }
    Assert ((Get-Service $svc).Status -eq 'Stopped') 'service stops on request'
    $triggered = $null
    try { $triggered = Invoke-Hello } catch { "  trigger connect: $($_.Exception.Message)" }
    Start-Sleep -Seconds 1
    Assert ((Get-Service $svc).Status -eq 'Running') 'connecting to the pipe started the service (trigger)'
    if ($null -eq $triggered) {
        try { $triggered = Invoke-Hello } catch { "  retry connect: $($_.Exception.Message)" }
    }
    Assert ($triggered -like '*not satellite.exe*') 'triggered service answers on its pipe'

    'step 6: interactive users hold SERVICE_START (sdset)'
    $sd = (& sc.exe sdshow $svc) -join ''
    Assert ($sd -match '\(A;;[A-Z]*RP[A-Z]*;;;IU\)') 'SDDL grants IU the RP (start) right'
}
finally {
    if (-not $Keep) {
        & sc.exe stop $svc 2>$null | Out-Null
        Start-Sleep -Seconds 2
        & sc.exe delete $svc 2>$null | Out-Null
        Get-Process -Name 'satellite-hm-helper' -ErrorAction SilentlyContinue | Stop-Process -Force -ErrorAction SilentlyContinue
        'cleanup: service removed'
    }
    else {
        "kept: $svc stays registered against $Helper"
    }
}

if ($failures.Count -gt 0) {
    throw "Broker service test failed: $($failures.Count) assertion(s): $($failures -join '; ')"
}
'Broker service: all assertions passed.'
