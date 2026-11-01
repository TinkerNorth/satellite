[CmdletBinding()]
param(
    [string]$Exe = '',
    [int]$StaySecs = 45,
    [int]$UdpPort = 9876,
    [int]$WebPort = 9877,
    [int]$ReadySecs = 20,
    [int]$ExitSecs = 20
)

$ErrorActionPreference = 'Stop'

$Invariant = [Globalization.CultureInfo]::InvariantCulture
$ProcessName = 'satellite'
$TrayWindowClass = 'ControllerForwardTray'
$WmClose = 0x0010
$LoopbackAddress = '127.0.0.1'
$DatagramBytes = 40
$DatagramCount = 3
$DatagramSeed = 7
$PollMs = 250
$CounterSettleSecs = 5
$RefusedExit = 2
$FailedExit = 1
$MaxErrorLinesShown = 10
$LogLinesShownOnFailure = 40
$LogSubdirectory = 'TinkerNorth\Satellite\logs'
$LogFilter = 'satellite-*.log'
$LogStampPattern = '^(\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}\.\d{3}) '
$LogStampFormat = "yyyy-MM-dd'T'HH:mm:ss.fff"
$ErrorLinePattern = '^\S+ \[ERR(OR)?\s*\]'
$WarningLinePattern = '^\S+ \[WARN\s*\]'
$TicksPerMillisecond = 10000

function Initialize-WindowApi {
    if ('SatelliteSmoke.Window' -as [type]) { return }
    Add-Type -Namespace SatelliteSmoke -Name Window -MemberDefinition @'
[DllImport("user32.dll", CharSet = CharSet.Unicode)]
public static extern IntPtr FindWindowExW(IntPtr parent, IntPtr after, string windowClass, string title);
[DllImport("user32.dll")]
public static extern uint GetWindowThreadProcessId(IntPtr window, out uint processId);
[DllImport("user32.dll")]
public static extern bool PostMessageW(IntPtr window, uint message, IntPtr wParam, IntPtr lParam);
'@
}

function Get-RunningSatellites {
    return @(Get-Process -Name $ProcessName -ErrorAction SilentlyContinue)
}

function Find-TrayWindow([int]$ProcessId) {
    Initialize-WindowApi
    $window = [IntPtr]::Zero
    while ($true) {
        $window = [SatelliteSmoke.Window]::FindWindowExW([IntPtr]::Zero, $window, $TrayWindowClass, [NullString]::Value)
        if ($window -eq [IntPtr]::Zero) { return [IntPtr]::Zero }
        $owner = [uint32]0
        [void][SatelliteSmoke.Window]::GetWindowThreadProcessId($window, [ref]$owner)
        if ($owner -eq $ProcessId) { return $window }
    }
}

function Send-WindowClose([IntPtr]$Window) {
    Initialize-WindowApi
    return [SatelliteSmoke.Window]::PostMessageW($Window, $WmClose, [IntPtr]::Zero, [IntPtr]::Zero)
}

function Wait-ForListeners($Process, [int]$Udp, [int]$Web, [int]$TimeoutSecs) {
    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSecs)
    while ([DateTime]::UtcNow -lt $deadline) {
        if ($Process.HasExited) { return $false }
        $udpOwners = @(Get-NetUDPEndpoint -LocalPort $Udp -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $Process.Id })
        $tcpOwners = @(Get-NetTCPConnection -LocalPort $Web -State Listen -ErrorAction SilentlyContinue | Where-Object { $_.OwningProcess -eq $Process.Id })
        if ($udpOwners.Count -gt 0 -and $tcpOwners.Count -gt 0) { return $true }
        Start-Sleep -Milliseconds $PollMs
    }
    return $false
}

function Get-AdminJson([int]$Port, [string]$Path) {
    return Invoke-RestMethod -Uri "http://${LoopbackAddress}:$Port$Path" -TimeoutSec 5
}

function Get-UnknownTokenCount([int]$Port) {
    return [long](Get-AdminJson $Port '/api/debug').rx.unknownToken
}

function Send-UnauthenticatedDatagrams([int]$Port) {
    $bytes = New-Object byte[] $DatagramBytes
    (New-Object System.Random $DatagramSeed).NextBytes($bytes)
    $client = New-Object System.Net.Sockets.UdpClient
    try {
        for ($i = 0; $i -lt $DatagramCount; $i++) { [void]$client.Send($bytes, $bytes.Length, $LoopbackAddress, $Port) }
    } finally {
        $client.Close()
    }
}

function Wait-ForUnknownTokenRise([int]$Port, [long]$Before, [int]$Rise) {
    $deadline = [DateTime]::UtcNow.AddSeconds($CounterSettleSecs)
    $after = Get-UnknownTokenCount $Port
    while ($after - $Before -lt $Rise -and [DateTime]::UtcNow -lt $deadline) {
        Start-Sleep -Milliseconds $PollMs
        $after = Get-UnknownTokenCount $Port
    }
    return $after
}

function Get-LogDirectory {
    return (Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) $LogSubdirectory)
}

function Read-SharedLines([string]$Path) {
    $stream = New-Object IO.FileStream($Path, [IO.FileMode]::Open, [IO.FileAccess]::Read, [IO.FileShare]::ReadWrite)
    try {
        $reader = New-Object IO.StreamReader($stream, [Text.Encoding]::UTF8)
        try { return @($reader.ReadToEnd() -split "`r?`n") } finally { $reader.Dispose() }
    } finally {
        $stream.Dispose()
    }
}

function Select-RunLines([string[]]$Lines, [datetime]$Since) {
    $inRun = $false
    foreach ($line in $Lines) {
        if ($line -match $LogStampPattern) {
            $stamp = [datetime]::ParseExact($Matches[1], $LogStampFormat, $Invariant)
            $inRun = $stamp -ge $Since
        }
        if ($inRun -and $line) { $line }
    }
}

function Get-RunLogLines([string]$Directory, [datetime]$Since) {
    if (-not (Test-Path -LiteralPath $Directory -PathType Container)) { return @() }
    $floor = $Since.AddTicks(-($Since.Ticks % $TicksPerMillisecond))
    $files = @(Get-ChildItem -LiteralPath $Directory -Filter $LogFilter -File |
        Where-Object { $_.LastWriteTime -ge $floor } | Sort-Object LastWriteTime)
    foreach ($file in $files) { Select-RunLines (Read-SharedLines $file.FullName) $floor }
}

function Find-ErrorLines([string[]]$Lines) {
    return @($Lines | Where-Object { $_ -match $ErrorLinePattern })
}

function Find-WarningLines([string[]]$Lines) {
    return @($Lines | Where-Object { $_ -match $WarningLinePattern })
}

function Invoke-AppSmoke {
    param([string]$Exe, [int]$StaySecs, [int]$UdpPort, [int]$WebPort, [int]$ReadySecs, [int]$ExitSecs)

    $running = @(Get-RunningSatellites)
    if ($running.Count -gt 0) {
        $ids = ($running | ForEach-Object { $_.Id }) -join ', '
        Write-Host "Refusing to run: $ProcessName.exe is already running (process $ids). A second instance would fight the first one over its single-instance mutex, its ports and its tray icon, so close it and run this again."
        return $RefusedExit
    }
    if (-not $Exe) { $Exe = Join-Path (Split-Path -Parent $PSScriptRoot) 'satellite.exe' }
    if (-not (Test-Path -LiteralPath $Exe -PathType Leaf)) { throw "$Exe does not exist; build the satellite target first" }
    $Exe = (Resolve-Path -LiteralPath $Exe).Path

    $problems = New-Object 'System.Collections.Generic.List[string]'
    $configDir = Join-Path ([IO.Path]::GetTempPath()) ('satellite-smoke-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $configDir | Out-Null
    $savedConfigDir = [Environment]::GetEnvironmentVariable('SATELLITE_CONFIG_DIR')
    $process = $null
    try {
        [Environment]::SetEnvironmentVariable('SATELLITE_CONFIG_DIR', $configDir)
        $startedAt = Get-Date
        try {
            $process = Start-Process -FilePath $Exe -WorkingDirectory (Split-Path -Parent $Exe) -PassThru
        } finally {
            [Environment]::SetEnvironmentVariable('SATELLITE_CONFIG_DIR', $savedConfigDir)
        }
        $null = $process.Handle
        Write-Host "launched $Exe as process $($process.Id) with SATELLITE_CONFIG_DIR=$configDir"

        $ready = Wait-ForListeners $process $UdpPort $WebPort $ReadySecs
        if ($ready) {
            Write-Host "  ok: UDP $UdpPort and TCP $WebPort are owned by process $($process.Id)"
        } else {
            $state = if ($process.HasExited) { "it exited with code $($process.ExitCode)" } else { 'it was still running' }
            $problems.Add("UDP $UdpPort and TCP $WebPort were not both owned by the launched process within $ReadySecs s; $state")
        }

        if ($ready) {
            try {
                $status = Get-AdminJson $WebPort '/api/status'
                if ($status.listening -eq $true) { Write-Host '  ok: /api/status reports the receiver listening' } else { $problems.Add("/api/status reports listening = $($status.listening)") }
                if ($status.udpPort -ne $UdpPort -or $status.webPort -ne $WebPort) {
                    $problems.Add("/api/status reports ports $($status.udpPort) and $($status.webPort), expected $UdpPort and $WebPort")
                }

                $before = Get-UnknownTokenCount $WebPort
                Send-UnauthenticatedDatagrams $UdpPort
                $after = Wait-ForUnknownTokenRise $WebPort $before $DatagramCount
                if ($after - $before -ge $DatagramCount) {
                    Write-Host "  ok: /api/debug rx.unknownToken rose from $before to $after after $DatagramCount unauthenticated datagrams of $DatagramBytes bytes"
                } else {
                    $problems.Add("/api/debug rx.unknownToken went from $before to $after after $DatagramCount unauthenticated datagrams, expected a rise of at least $DatagramCount")
                }
            } catch {
                $problems.Add("the admin API on port $WebPort could not be read: $($_.Exception.Message)")
            }
        }

        $stayUntil = $startedAt.AddSeconds($StaySecs)
        while ((Get-Date) -lt $stayUntil -and -not $process.HasExited) { Start-Sleep -Milliseconds $PollMs }
        if ($process.HasExited) {
            $problems.Add("the process exited by itself with code $($process.ExitCode) before $StaySecs s were up")
        } else {
            Write-Host "  ok: still running after $StaySecs s"
            $window = Find-TrayWindow $process.Id
            if ($window -eq [IntPtr]::Zero) {
                $problems.Add("no $TrayWindowClass window belongs to process $($process.Id)")
            } elseif (-not (Send-WindowClose $window)) {
                $problems.Add("posting WM_CLOSE to the $TrayWindowClass window failed")
            } elseif ($process.WaitForExit($ExitSecs * 1000)) {
                if ($process.ExitCode -eq 0) { Write-Host '  ok: WM_CLOSE ended the process with exit code 0' } else { $problems.Add("the process exited with code $($process.ExitCode) after WM_CLOSE, expected 0") }
            } else {
                $problems.Add("the process was still running $ExitSecs s after WM_CLOSE")
            }
        }
    } finally {
        if ($null -ne $process -and -not $process.HasExited) {
            Stop-Process -Id $process.Id -Force
            [void]$process.WaitForExit(15000)
        }
        Remove-Item -LiteralPath $configDir -Recurse -Force -ErrorAction SilentlyContinue
    }

    $lines = @(Get-RunLogLines (Get-LogDirectory) $startedAt)
    if ($lines.Count -eq 0) {
        $problems.Add("no line of this run reached a satellite-*.log under $(Get-LogDirectory)")
    } else {
        $errors = @(Find-ErrorLines $lines)
        $warnings = @(Find-WarningLines $lines)
        foreach ($line in $warnings) { Write-Host "  warn: $line" }
        if ($errors.Count -eq 0) {
            Write-Host "  ok: $($lines.Count) log lines from this run, none at ERROR"
        } else {
            $problems.Add("$($errors.Count) ERROR line(s) in this run's log")
            foreach ($line in ($errors | Select-Object -First $MaxErrorLinesShown)) { Write-Host "  error: $line" }
        }
    }

    if ($problems.Count -eq 0) {
        Write-Host 'Smoke test passed.'
        return 0
    }
    foreach ($line in ($lines | Select-Object -Last $LogLinesShownOnFailure)) { Write-Host "  log: $line" }
    foreach ($problem in $problems) { Write-Host "FAIL: $problem" }
    Write-Host "Smoke test failed: $($problems.Count) problem(s)."
    return $FailedExit
}

if ($MyInvocation.InvocationName -ne '.') {
    $exitCode = $FailedExit
    try {
        $result = @(Invoke-AppSmoke -Exe $Exe -StaySecs $StaySecs -UdpPort $UdpPort -WebPort $WebPort -ReadySecs $ReadySecs -ExitSecs $ExitSecs)
        $exitCode = [int]$result[-1]
    } catch {
        Write-Host "Smoke test could not run: $($_.Exception.Message)"
    }
    exit $exitCode
}
