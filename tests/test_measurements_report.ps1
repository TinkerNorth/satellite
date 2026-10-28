[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
$reportScript = Join-Path $repoRoot 'scripts\measurements-report.ps1'
. $reportScript

$savedSummary = $env:GITHUB_STEP_SUMMARY
$savedImage = $env:ImageVersion
Remove-Item Env:GITHUB_STEP_SUMMARY -ErrorAction SilentlyContinue
Remove-Item Env:ImageVersion -ErrorAction SilentlyContinue

$script:pass = 0
$script:fail = 0
$script:current = ''
$utf8 = New-Object System.Text.UTF8Encoding($false)
$scenarioIds = @('S1', 'S2', 'S3', 'S4', 'S5', 'S6', 'S7', 'S8')
$targets = @('bench_frame_path', 'bench_frame_path_lockstats')
$backends = @('mock', 'vigem')

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
    $dir = Join-Path ([IO.Path]::GetTempPath()) ('satellite-measurements-test-' + [guid]::NewGuid().ToString('N'))
    New-Item -ItemType Directory -Path $dir | Out-Null
    return $dir
}

function WriteText([string]$Path, [string]$Text) { [IO.File]::WriteAllText($Path, $Text, $utf8) }

function ReadJson([string]$Path) { return ([IO.File]::ReadAllText($Path) | ConvertFrom-Json) }

function Quote([string]$Text) { return '"' + $Text + '"' }

function Neutral([string]$Text) { return ($Text -replace '::', ': :') }

function ChangeKey([string]$Target, [string]$Backend, [int]$MicHz, [string]$Scenario, [string]$Dish) {
    return "$Target|$Backend|$MicHz|$Scenario|$Dish"
}

function BenchRow([string]$Backend, [string]$Scenario, [string]$Dish, [hashtable]$Set) {
    $isA = $Dish -eq 'A'
    $rate = 75
    if ($isA) { $rate = if ($Scenario -eq 'S1') { 1000 } else { 8000 } }
    $row = [ordered]@{
        backend      = $Backend
        scenario     = $Scenario
        dish         = $Dish
        rateHz       = $rate
        frames       = $(if ($isA) { 24000 } else { 225 })
        p50Us        = 15.4
        p99Us        = $(if ($isA) { 15.9 } else { 31.0 })
        p999Us       = $(if ($isA) { 17.7 } else { 32.0 })
        maxUs        = $(if ($isA) { 60.5 } else { 32.0 })
        overOneMs    = 0
        longestGapMs = $(if ($isA) { 0.2 } else { 13.35 })
        drops        = 0
        refused      = 0
        motion       = 0
        mic          = 0
        heartbeats   = 0
    }
    foreach ($key in $Set.Keys) { $row[$key] = $Set[$key] }
    return $row
}

function LockRow([string]$Backend, [string]$Scenario, [string]$Lock, [string]$Role, [double]$MaxWait) {
    return [ordered]@{
        backend      = $Backend
        scenario     = $Scenario
        lock         = $Lock
        role         = $Role
        acquisitions = 150
        contended    = 2
        failedTries  = 0
        maxWaitUs    = $MaxWait
        p99WaitUs    = 0.5
        maxHoldUs    = 60.7
    }
}

function BenchJson([string]$Target, [string]$Backend, [int]$MicHz, [hashtable]$Changes) {
    $lockStats = $Target -eq 'bench_frame_path_lockstats'
    $rows = @()
    $locks = @()
    foreach ($scenario in $scenarioIds) {
        foreach ($dish in @('A', 'B')) {
            if ($dish -eq 'B' -and $scenario -eq 'S1') { continue }
            $set = $Changes[(ChangeKey $Target $Backend $MicHz $scenario $dish)]
            if ($null -eq $set) { $set = @{} }
            $rows += , (BenchRow $Backend $scenario $dish $set)
        }
        if ($lockStats -and $scenario -in @('S6', 'S7', 'S8')) {
            $locks += , (LockRow $Backend $scenario 'config' 'Receive' 4.5)
            $locks += , (LockRow $Backend $scenario 'service.state' 'Receive' 0.1)
        }
    }
    $settings = [ordered]@{
        secs = 3.0; reps = 3; rateA = 8000; rateB = 75; motionHz = 500; micHz = $MicHz
        submitUs = 15; plugMs = 200; rxCpu = 2; smoke = $false
    }
    $doc = [ordered]@{
        bench = 'frame_path_bench'; cpu = 'Test CPU 9000'; build = 'Release'
        lockStats = $lockStats; settings = $settings; rows = $rows; locks = $locks
    }
    return ($doc | ConvertTo-Json -Depth 6)
}

function WriteFullSet([string]$Dir, [hashtable]$Changes = @{}, [string[]]$Skip = @()) {
    New-Item -ItemType Directory -Force -Path $Dir | Out-Null
    foreach ($target in $targets) {
        foreach ($backend in $backends) {
            foreach ($micHz in @(50, 1000)) {
                $suffix = if ($micHz -eq 50) { '' } else { '-mic1000' }
                $name = "$target-$backend$suffix.json"
                if ($Skip -contains $name) { continue }
                WriteText (Join-Path $Dir $name) (BenchJson $target $backend $micHz $Changes)
            }
        }
    }
}

function RunReport([string]$In, [string]$Out, [string]$Version, [string]$Prev = '', [bool]$Partial = $false) {
    $result = @(Invoke-MeasurementsReport -InputDir $In -OutputDir $Out -Version $Version -PreviousDir $Prev `
        -Commit 'abcdef0123456789' -GapLimitMs 50 -TrendRatio 2 -TrendMinDeltaUs 100 -AllowPartial $Partial 6>$null)
    return $result[-1]
}

function InvokeReportScript([string[]]$Arguments, [hashtable]$Env = @{}) {
    $hostExe = (Get-Process -Id $PID).Path
    $out = [IO.Path]::GetTempFileName()
    $err = [IO.Path]::GetTempFileName()
    $argv = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', (Quote $reportScript)) + $Arguments
    $saved = @{}
    foreach ($name in $Env.Keys) {
        $saved[$name] = [Environment]::GetEnvironmentVariable($name)
        [Environment]::SetEnvironmentVariable($name, $Env[$name])
    }
    try {
        $proc = Start-Process -FilePath $hostExe -ArgumentList $argv -PassThru -NoNewWindow `
            -RedirectStandardOutput $out -RedirectStandardError $err
        $null = $proc.Handle
        $proc.WaitForExit()
        $code = $proc.ExitCode
    } finally {
        foreach ($name in $saved.Keys) { [Environment]::SetEnvironmentVariable($name, $saved[$name]) }
    }
    $text = [string][IO.File]::ReadAllText($out) + [string][IO.File]::ReadAllText($err)
    Remove-Item $out, $err -Force
    return @{ Code = $code; Output = $text }
}

function TestVersionAndFormatting {
    Test 'Assert-VersionLabel: file-name safe labels pass and anything else is refused'
    Assert-VersionLabel '2.1.9'
    Assert-VersionLabel '2.1.9-rc.1'
    Assert-VersionLabel 'dev-abc1234'
    $script:pass += 3
    ExpectThrows { Assert-VersionLabel '' } 'version label'
    ExpectThrows { Assert-VersionLabel '..\evil' } 'version label'
    ExpectThrows { Assert-VersionLabel '2.1.9 beta' } 'version label'

    Test 'Format-Fixed and Format-Compact: invariant digits whatever the culture'
    ExpectEq (Format-Fixed 15.449 1) '15.4' 'one decimal'
    ExpectEq (Format-Fixed 1.0023 3) '1.002' 'three decimals'
    ExpectEq (Format-Compact 50) '50' 'whole number'
    ExpectEq (Format-Compact 2.5) '2.5' 'fraction'

    Test 'Get-VersionSortKey: 2.1.10 sorts after 2.1.9'
    Expect ((Get-VersionSortKey 'measurements-2.1.10.json') -gt (Get-VersionSortKey 'measurements-2.1.9.json')) 'numeric, not text, order'
}

function TestReadBenchFile {
    $dir = NewTempDir
    try {
        WriteFullSet $dir
        Test 'Read-BenchFile: the latency build reads as bench_frame_path, default variant'
        $run = Read-BenchFile (Join-Path $dir 'bench_frame_path-mock.json')
        ExpectEq $run.Target 'bench_frame_path' 'target'
        ExpectEq $run.Variant 'default' 'variant'
        ExpectEq $run.Rows.Count 15 'S1 has dish A alone, S2 to S8 have both dishes'
        ExpectEq $run.Locks.Count 0 'no lock rows without the stats build'
        ExpectEq $run.Cpu 'Test CPU 9000' 'cpu'

        Test 'Read-BenchFile: the stats build carries its lock rows'
        $run = Read-BenchFile (Join-Path $dir 'bench_frame_path_lockstats-vigem.json')
        ExpectEq $run.Target 'bench_frame_path_lockstats' 'target'
        ExpectEq $run.Locks.Count 6 'two locks in each of S6 to S8'
        ExpectEq $run.Locks[0].role 'Receive' 'role'

        Test 'Read-BenchFile: a --mic-hz 1000 run keeps S6 only, rows and locks'
        $run = Read-BenchFile (Join-Path $dir 'bench_frame_path_lockstats-mock-mic1000.json')
        ExpectEq $run.Variant 'mic1000' 'variant'
        ExpectEq $run.Rows.Count 2 'S6 dish A and B'
        ExpectEq $run.Rows[0].scenario 'S6' 'scenario'
        ExpectEq $run.Locks.Count 2 'S6 locks'

        Test 'Read-BenchFile: numbers come out as numbers whichever PowerShell parsed them'
        $row = (Read-BenchFile (Join-Path $dir 'bench_frame_path-mock.json')).Rows[0]
        Expect ($row.p999Us -is [double]) 'p99.9 is a double'
        Expect ($row.frames -is [long]) 'frames is a long'
        ExpectEq $row.p999Us 17.7 'value kept'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestReadBenchFileRefusals {
    $dir = NewTempDir
    try {
        $good = BenchJson 'bench_frame_path' 'mock' 50 @{}
        $path = Join-Path $dir 'x.json'

        Test 'Read-BenchFile: text that is not JSON is refused by file name'
        WriteText $path '{ not json'
        ExpectThrows { Read-BenchFile $path } 'x.json is not valid JSON'

        Test 'Read-BenchFile: JSON from another tool is refused'
        WriteText $path '{"bench":"dish_bench","rows":[]}'
        ExpectThrows { Read-BenchFile $path } 'is not frame_path_bench output'

        Test 'Read-BenchFile: a run without rows is refused'
        WriteText $path '{"bench":"frame_path_bench","lockStats":false,"settings":{"micHz":50},"rows":[]}'
        ExpectThrows { Read-BenchFile $path } 'has no rows'

        Test 'Read-BenchFile: a run that does not say its mic rate cannot be told from the default and is refused'
        WriteText $path ($good -replace '"micHz"', '"mic"')
        ExpectThrows { Read-BenchFile $path } 'settings.micHz'

        Test 'Read-BenchFile: a row missing a figure the gate or the tables need is refused by name'
        WriteText $path ($good -replace '"longestGapMs"', '"longestGap"')
        ExpectThrows { Read-BenchFile $path } 'without longestGapMs'

        Test 'Read-BenchDirectory: a missing or empty directory is refused'
        ExpectThrows { Read-BenchDirectory (Join-Path $dir 'nope') } 'does not exist'
        $empty = Join-Path $dir 'empty'
        New-Item -ItemType Directory -Path $empty | Out-Null
        ExpectThrows { Read-BenchDirectory $empty } 'no bench JSON files'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestMerge {
    $dir = NewTempDir
    try {
        WriteFullSet (Join-Path $dir 'in')
        $runs = @(Read-BenchDirectory (Join-Path $dir 'in'))

        Test 'New-Measurements: every file lands in one flat document'
        $merged = New-Measurements $runs '9.9.9' 'abc' '2026-01-01T00:00:00Z'
        ExpectEq $merged.schema 1 'schema'
        ExpectEq $merged.version '9.9.9' 'version'
        ExpectEq $merged.sources.Count 8 'eight sources'
        ExpectEq $merged.latency.Count 68 '4 default runs of 15 rows plus 4 mic runs of 2 rows'
        ExpectEq $merged.locks.Count 16 '2 stats runs of 6 default lock rows plus 2 of 2 mic lock rows'
        ExpectEq $merged.machine.cpu 'Test CPU 9000' 'machine'
        ExpectEq $merged.settings.micHz 50 'settings come from a default run'

        Test 'New-Measurements: the mic variant never repeats a scenario the default run already measured'
        $micRows = @($merged.latency | Where-Object { $_.variant -eq 'mic1000' })
        ExpectEq $micRows.Count 8 'eight mic rows'
        Expect (@($micRows | Where-Object { $_.scenario -ne 'S6' }).Count -eq 0) 'S6 only'

        Test 'New-Measurements: two files covering the same run are refused'
        $twice = @($runs) + @($runs[0])
        ExpectThrows { New-Measurements $twice '9.9.9' 'abc' 'now' } 'two bench files cover the same row'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestGate {
    $dir = NewTempDir
    try {
        $in = Join-Path $dir 'in'
        WriteFullSet $in
        $rows = @((New-Measurements @(Read-BenchDirectory $in) '9.9.9' '' 'now').latency)

        Test 'Get-StallGate: quiet runs pass, one result for every PUT scenario of every run'
        $gate = Get-StallGate $rows 50 $true
        ExpectEq $gate.passed $true 'passed'
        ExpectEq $gate.failures.Count 0 'no failures'
        ExpectEq $gate.results.Count 24 '4 runs x S4..S8 plus 4 mic runs x S6'

        Test 'Get-StallGate: a 200 ms plug stall in S5 on one backend fails that row and no other'
        $changes = @{ (ChangeKey 'bench_frame_path' 'vigem' 50 'S5' 'A') = @{ longestGapMs = 201.718 } }
        WriteFullSet (Join-Path $dir 'stall') $changes
        $stalled = @((New-Measurements @(Read-BenchDirectory (Join-Path $dir 'stall')) '9.9.9' '' 'now').latency)
        $gate = Get-StallGate $stalled 50 $true
        ExpectEq $gate.passed $false 'failed'
        ExpectEq $gate.failures.Count 1 'exactly one failure'
        Expect ($gate.failures[0] -like '*bench_frame_path vigem default S5 dish A*201.7 ms*') 'names the run, scenario and gap'

        Test 'Get-StallGate: the limit is strict, 49.999 ms passes and 50 ms fails'
        $edge = @{ (ChangeKey 'bench_frame_path' 'mock' 50 'S4' 'A') = @{ longestGapMs = 49.999 } }
        WriteFullSet (Join-Path $dir 'edge') $edge
        $rowsEdge = @((New-Measurements @(Read-BenchDirectory (Join-Path $dir 'edge')) '9.9.9' '' 'now').latency)
        ExpectEq (Get-StallGate $rowsEdge 50 $true).passed $true 'just under'
        $edge = @{ (ChangeKey 'bench_frame_path' 'mock' 50 'S4' 'A') = @{ longestGapMs = 50.0 } }
        WriteFullSet (Join-Path $dir 'edge') $edge
        $rowsEdge = @((New-Measurements @(Read-BenchDirectory (Join-Path $dir 'edge')) '9.9.9' '' 'now').latency)
        ExpectEq (Get-StallGate $rowsEdge 50 $true).passed $false 'at the limit'

        Test 'Get-StallGate: scenarios without a PUT and dish B are not gated'
        $quiet = @{
            (ChangeKey 'bench_frame_path' 'mock' 50 'S1' 'A') = @{ longestGapMs = 400.0 }
            (ChangeKey 'bench_frame_path' 'mock' 50 'S3' 'A') = @{ longestGapMs = 400.0 }
            (ChangeKey 'bench_frame_path' 'mock' 50 'S5' 'B') = @{ longestGapMs = 400.0 }
        }
        WriteFullSet (Join-Path $dir 'quiet') $quiet
        $rowsQuiet = @((New-Measurements @(Read-BenchDirectory (Join-Path $dir 'quiet')) '9.9.9' '' 'now').latency)
        ExpectEq (Get-StallGate $rowsQuiet 50 $true).passed $true 'passes'

        Test 'Get-StallGate: the lock-stats build is held to the same limit'
        $stats = @{ (ChangeKey 'bench_frame_path_lockstats' 'mock' 50 'S7' 'A') = @{ longestGapMs = 120.0 } }
        WriteFullSet (Join-Path $dir 'stats') $stats
        $rowsStats = @((New-Measurements @(Read-BenchDirectory (Join-Path $dir 'stats')) '9.9.9' '' 'now').latency)
        $gate = Get-StallGate $rowsStats 50 $true
        ExpectEq $gate.failures.Count 1 'one failure'
        Expect ($gate.failures[0] -like '*bench_frame_path_lockstats mock default S7*') 'names the stats build'

        Test 'Get-StallGate: the --mic-hz 1000 run is gated on S6, and its other scenarios are ignored'
        $mic = @{
            (ChangeKey 'bench_frame_path' 'vigem' 1000 'S6' 'A') = @{ longestGapMs = 90.0 }
            (ChangeKey 'bench_frame_path' 'vigem' 1000 'S5' 'A') = @{ longestGapMs = 500.0 }
        }
        WriteFullSet (Join-Path $dir 'mic') $mic
        $rowsMic = @((New-Measurements @(Read-BenchDirectory (Join-Path $dir 'mic')) '9.9.9' '' 'now').latency)
        $gate = Get-StallGate $rowsMic 50 $true
        ExpectEq $gate.failures.Count 1 'only S6 counts'
        Expect ($gate.failures[0] -like '*bench_frame_path vigem mic1000 S6 dish A*') 'names the mic run'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestGateCompleteness {
    $dir = NewTempDir
    try {
        Test 'Get-StallGate: a run that is missing fails the gate unless partial sets are allowed'
        WriteFullSet (Join-Path $dir 'partial') @{} @('bench_frame_path_lockstats-vigem-mic1000.json')
        $rows = @((New-Measurements @(Read-BenchDirectory (Join-Path $dir 'partial')) '9.9.9' '' 'now').latency)
        $gate = Get-StallGate $rows 50 $true
        ExpectEq $gate.passed $false 'failed'
        ExpectEq $gate.failures.Count 1 'one failure'
        Expect ($gate.failures[0] -like 'missing run: bench_frame_path_lockstats vigem mic1000') 'names the run'
        ExpectEq (Get-StallGate $rows 50 $false).passed $true 'partial set allowed'

        Test 'Get-StallGate: a PUT scenario without a dish A row cannot pass'
        $text = BenchJson 'bench_frame_path' 'mock' 50 @{}
        $doc = $text | ConvertFrom-Json
        $doc.rows = @($doc.rows | Where-Object { -not ($_.scenario -eq 'S5' -and $_.dish -eq 'A') })
        WriteFullSet (Join-Path $dir 'hole')
        WriteText (Join-Path $dir 'hole\bench_frame_path-mock.json') ($doc | ConvertTo-Json -Depth 6)
        $holes = @((New-Measurements @(Read-BenchDirectory (Join-Path $dir 'hole')) '9.9.9' '' 'now').latency)
        $gate = Get-StallGate $holes 50 $false
        ExpectEq $gate.passed $false 'failed'
        Expect ($gate.failures[0] -like '*has no dish A row for S5*') 'names the scenario'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestTrendRule {
    Test 'Test-TrendRegression: it takes both more than twice and more than 100 us higher'
    ExpectEq (Test-TrendRegression 300 20 2 100) $true 'far and much higher'
    ExpectEq (Test-TrendRegression 30 10 2 100) $false 'three times, but only 20 us'
    ExpectEq (Test-TrendRegression 120 70 2 100) $false 'a small ratio over 100 us'
    ExpectEq (Test-TrendRegression 240 120 2 100) $false 'exactly twice is not more than twice'
    ExpectEq (Test-TrendRegression 250 120 2 100) $true 'just over twice and 130 us higher'
    ExpectEq (Test-TrendRegression 220 120 2 100) $false 'exactly 100 us higher, but not twice'
    ExpectEq (Test-TrendRegression 150 50 2 100) $false 'three times, exactly 100 us higher is not more'
    ExpectEq (Test-TrendRegression 150.5 50 2 100) $true 'three times and just over 100 us higher'
    ExpectEq (Test-TrendRegression 101 0 2 100) $true 'a zero before'
    ExpectEq (Test-TrendRegression 10 50 2 100) $false 'faster is never a warning'
}

function TestTrendComparison {
    $dir = NewTempDir
    try {
        $in = Join-Path $dir 'in'
        WriteFullSet $in
        $rows = @((New-Measurements @(Read-BenchDirectory $in) '9.9.9' '' 'now').latency)

        Test 'Get-TrendComparison: without a previous release there is nothing to compare'
        $trend = Get-TrendComparison $rows $null 2 100
        ExpectEq $trend.available $false 'unavailable'
        ExpectEq $trend.warnings.Count 0 'no warnings'

        Test 'Get-TrendComparison: the same numbers warn about nothing'
        $previous = (New-Measurements @(Read-BenchDirectory $in) '9.9.8' '' 'then') | ConvertTo-Json -Depth 10 | ConvertFrom-Json
        $trend = Get-TrendComparison $rows $previous 2 100
        ExpectEq $trend.available $true 'available'
        ExpectEq $trend.previousVersion '9.9.8' 'previous version'
        ExpectEq $trend.warnings.Count 0 'no warnings'

        Test 'Get-TrendComparison: p99.9 and max are each judged, on every row that exists in both'
        $slow = @{
            (ChangeKey 'bench_frame_path' 'mock' 50 'S5' 'A') = @{ p999Us = 400.0; maxUs = 90.0 }
            (ChangeKey 'bench_frame_path' 'vigem' 50 'S2' 'B') = @{ p999Us = 90.0; maxUs = 500.0 }
            (ChangeKey 'bench_frame_path' 'mock' 50 'S3' 'A') = @{ p999Us = 60.0; maxUs = 150.0 }
        }
        WriteFullSet (Join-Path $dir 'slow') $slow
        $nowRows = @((New-Measurements @(Read-BenchDirectory (Join-Path $dir 'slow')) '9.9.9' '' 'now').latency)
        $trend = Get-TrendComparison $nowRows $previous 2 100
        ExpectEq $trend.warnings.Count 2 'S5 p99.9 and S2 B max'
        Expect (@($trend.warnings | Where-Object { $_ -like 'bench_frame_path mock S5 dish A p99.9 is 400.0 us, was 17.7 us in 9.9.8, +382.3 us' }).Count -eq 1) 'S5 p99.9 text'
        Expect (@($trend.warnings | Where-Object { $_ -like 'bench_frame_path vigem S2 dish B max is 500.0 us, was 32.0 us in 9.9.8, +468.0 us' }).Count -eq 1) 'S2 B max text'

        Test 'Get-TrendComparison: a row the previous release did not measure is skipped'
        $trimmed = $previous
        $trimmed.latency = @($previous.latency | Where-Object { $_.scenario -ne 'S5' })
        $trend = Get-TrendComparison $nowRows $trimmed 2 100
        ExpectEq $trend.warnings.Count 1 'only S2 B is left to compare'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestPreviousSelection {
    $dir = NewTempDir
    try {
        $in = Join-Path $dir 'in'
        WriteFullSet $in
        $runs = @(Read-BenchDirectory $in)
        $prev = Join-Path $dir 'prev'
        New-Item -ItemType Directory -Path $prev | Out-Null

        Test 'Read-PreviousMeasurements: a missing, empty or unnamed directory means no previous release'
        ExpectEq ($null -eq (Read-PreviousMeasurements '' '9.9.9')) $true 'no directory given'
        ExpectEq ($null -eq (Read-PreviousMeasurements (Join-Path $dir 'nope') '9.9.9')) $true 'directory missing'
        ExpectEq ($null -eq (Read-PreviousMeasurements $prev '9.9.9')) $true 'directory empty'

        Test 'Read-PreviousMeasurements: it takes the newest version, by number'
        foreach ($version in @('2.1.9', '2.1.10', '2.1.2')) {
            $doc = New-Measurements $runs $version '' 'then'
            WriteText (Join-Path $prev "measurements-$version.json") ($doc | ConvertTo-Json -Depth 10)
        }
        ExpectEq (Read-PreviousMeasurements $prev '9.9.9').version '2.1.10' 'newest'

        Test 'Read-PreviousMeasurements: the version being measured is never its own baseline'
        ExpectEq (Read-PreviousMeasurements $prev '2.1.10').version '2.1.9' 'next newest'

        Test 'Read-PreviousMeasurements: unreadable and foreign files are skipped, not fatal'
        WriteText (Join-Path $prev 'measurements-3.0.0.json') '{ broken'
        WriteText (Join-Path $prev 'measurements-3.1.0.json') '{"schema":99,"version":"3.1.0","latency":[]}'
        ExpectEq (Read-PreviousMeasurements $prev '9.9.9' 6>$null).version '2.1.10' 'still the newest usable file'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestMarkdown {
    $dir = NewTempDir
    try {
        $in = Join-Path $dir 'in'
        WriteFullSet $in
        $prev = Join-Path $dir 'prev'
        New-Item -ItemType Directory -Path $prev | Out-Null

        Test 'the markdown carries the gate, the trend note, the dish A table and the lock table'
        $code = RunReport $in (Join-Path $dir 'out') '9.9.9' $prev
        ExpectEq $code 0 'exit code'
        $md = [IO.File]::ReadAllText((Join-Path $dir 'out\measurements-9.9.9.md'))
        Expect ($md -like '# Satellite 9.9.9 measurements*') 'title'
        Expect ($md.Contains('commit `abcdef0`')) 'short commit'
        Expect ($md -like '*Test CPU 9000, Release build*') 'machine line'
        Expect ($md -like '*PASS: dish A''s longest gap stays under 50 ms*') 'gate verdict'
        Expect ($md -like '*No earlier release carries measurements*') 'no previous release'
        Expect ($md -like '*| scenario | mode | rate Hz | frames | p50 us | p99 us | p99.9 us | max us | over 1 ms | longest gap ms | drops |*') 'latency headers'
        Expect ($md -like '*| S5 | vigem | 8000 | 24000 | 15.4 | 15.9 | 17.7 | 60.5 | 0 | 0.200 | 0 |*') 'a latency row'
        Expect ($md -like '*| S6 (mic 1000 Hz) | mock | 8000 |*') 'the mic variant row'
        Expect ($md -like '*| mode | scenario | lock | role | acquisitions | contended | failed tries | max wait us | p99 wait us | max hold us |*') 'lock headers'
        Expect ($md -like '*| mock | S6 | config | Receive | 150 | 2 | 0 | 4.5 | 0.5 | 60.7 |*') 'a lock row'
        Expect ($md -notlike '*| B |*') 'dish B stays out of the table'

        Test 'the dish A table keeps scenario order, mock before vigem, the mic variant last'
        $order = [regex]::Matches($md, '(?m)^\| (S\d(?: \(mic 1000 Hz\))?) \| (mock|vigem) \| \d+ \| 24000|^\| (S1) \| (mock|vigem) \| 1000 \|') | ForEach-Object { ($_.Value -split '\|')[1].Trim() + ' ' + ($_.Value -split '\|')[2].Trim() }
        ExpectEq ($order -join ',') 'S1 mock,S1 vigem,S2 mock,S2 vigem,S3 mock,S3 vigem,S4 mock,S4 vigem,S5 mock,S5 vigem,S6 mock,S6 vigem,S7 mock,S7 vigem,S8 mock,S8 vigem,S6 (mic 1000 Hz) mock,S6 (mic 1000 Hz) vigem' 'row order'

        Test 'with a previous release the table gains its two columns and the note names it'
        $previousDoc = New-Measurements @(Read-BenchDirectory $in) '9.9.8' '' 'then'
        WriteText (Join-Path $prev 'measurements-9.9.8.json') ($previousDoc | ConvertTo-Json -Depth 10)
        $code = RunReport $in (Join-Path $dir 'out2') '9.9.9' $prev
        ExpectEq $code 0 'exit code'
        $md = [IO.File]::ReadAllText((Join-Path $dir 'out2\measurements-9.9.9.md'))
        Expect ($md -like '*## Trend against 9.9.8*') 'trend heading'
        Expect ($md -like '*No p99.9 or max is more than 2 times and more than 100 us above the previous release.*') 'quiet trend'
        Expect ($md -like '*| p99.9 us in 9.9.8 | max us in 9.9.8 |*') 'previous columns'
        Expect ($md -like '*| S5 | vigem | 8000 | 24000 | 15.4 | 15.9 | 17.7 | 60.5 | 0 | 0.200 | 0 | 17.7 | 60.5 |*') 'previous values in the row'

        Test 'a failed gate is spelled out in the markdown and the JSON'
        $changes = @{ (ChangeKey 'bench_frame_path' 'vigem' 50 'S5' 'A') = @{ longestGapMs = 201.718 } }
        WriteFullSet (Join-Path $dir 'bad') $changes
        $code = RunReport (Join-Path $dir 'bad') (Join-Path $dir 'out3') '9.9.9' ''
        ExpectEq $code 1 'exit code'
        $md = [IO.File]::ReadAllText((Join-Path $dir 'out3\measurements-9.9.9.md'))
        Expect ($md -like '*FAIL: dish A''s longest gap must stay under 50 ms*') 'verdict'
        Expect ($md -like '*- bench_frame_path vigem default S5 dish A: longest gap 201.7 ms is not under 50 ms*') 'bullet'
        $json = ReadJson (Join-Path $dir 'out3\measurements-9.9.9.json')
        ExpectEq $json.gate.passed $false 'gate.passed'
        ExpectEq $json.gate.failures.Count 1 'gate.failures'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestOutputFiles {
    $dir = NewTempDir
    try {
        $in = Join-Path $dir 'in'
        WriteFullSet $in
        $code = RunReport $in (Join-Path $dir 'nested\out') '2.1.9' ''

        Test 'the merged JSON is written beside the markdown, named for the version'
        ExpectEq $code 0 'exit code'
        Expect (Test-Path (Join-Path $dir 'nested\out\measurements-2.1.9.json')) 'json'
        Expect (Test-Path (Join-Path $dir 'nested\out\measurements-2.1.9.md')) 'markdown'
        $json = ReadJson (Join-Path $dir 'nested\out\measurements-2.1.9.json')
        ExpectEq $json.schema 1 'schema'
        ExpectEq $json.version '2.1.9' 'version'
        ExpectEq $json.commit 'abcdef0123456789' 'commit'
        ExpectEq $json.gate.passed $true 'gate'
        ExpectEq $json.gate.limitMs 50 'limit recorded'
        ExpectEq $json.latency.Count 68 'rows'
        ExpectEq $json.locks.Count 16 'lock rows'
        ExpectEq $json.trend.warnings.Count 0 'trend recorded'
        Expect (([IO.File]::ReadAllBytes((Join-Path $dir 'nested\out\measurements-2.1.9.json')))[0] -ne 0xEF) 'no byte order mark'

        Test 'a report is its own next baseline: the written JSON reads back as a previous release'
        $prev = Join-Path $dir 'prev'
        New-Item -ItemType Directory -Path $prev | Out-Null
        Copy-Item (Join-Path $dir 'nested\out\measurements-2.1.9.json') $prev
        $changes = @{ (ChangeKey 'bench_frame_path' 'mock' 50 'S8' 'A') = @{ p999Us = 900.0 } }
        WriteFullSet (Join-Path $dir 'in2') $changes
        $code = RunReport (Join-Path $dir 'in2') (Join-Path $dir 'out2') '2.1.10' $prev
        ExpectEq $code 0 'a trend warning does not fail the run'
        $json = ReadJson (Join-Path $dir 'out2\measurements-2.1.10.json')
        ExpectEq $json.trend.previousVersion '2.1.9' 'baseline'
        ExpectEq $json.trend.warnings.Count 1 'one warning'
        Expect ($json.trend.warnings[0] -like 'bench_frame_path mock S8 dish A p99.9 is 900.0 us, was 17.7 us in 2.1.9*') 'warning text'

        Test 'GITHUB_STEP_SUMMARY receives the markdown'
        $summary = Join-Path $dir 'summary.md'
        WriteText $summary "earlier step`n"
        $env:GITHUB_STEP_SUMMARY = $summary
        try { $code = RunReport $in (Join-Path $dir 'out3') '2.1.11' '' } finally { Remove-Item Env:GITHUB_STEP_SUMMARY }
        $text = [IO.File]::ReadAllText($summary)
        Expect ($text -like 'earlier step*# Satellite 2.1.11 measurements*') 'appended after what was there'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestScriptProcess {
    $dir = NewTempDir
    try {
        $in = Join-Path $dir 'in'
        $out = Join-Path $dir 'out'
        WriteFullSet $in
        $argv = @('-InputDir', (Quote $in), '-OutputDir', (Quote $out), '-Version', '9.9.9', '-Commit', 'cafe')

        Test 'measurements-report.ps1: a clean set exits 0 and writes both files'
        $run = InvokeReportScript $argv
        ExpectEq $run.Code 0 "exit code; output: $(Neutral $run.Output)"
        Expect (Test-Path (Join-Path $out 'measurements-9.9.9.json')) 'json'
        Expect ($run.Output -like '*plug stall gate: PASS*') 'verdict on the console'

        Test 'measurements-report.ps1: a plug stall exits 1 with an error annotation, files still written'
        $changes = @{ (ChangeKey 'bench_frame_path' 'mock' 50 'S4' 'A') = @{ longestGapMs = 201.7 } }
        WriteFullSet (Join-Path $dir 'bad') $changes
        $badOut = Join-Path $dir 'badout'
        $run = InvokeReportScript @('-InputDir', (Quote (Join-Path $dir 'bad')), '-OutputDir', (Quote $badOut), '-Version', '9.9.9')
        ExpectEq $run.Code 1 'exit code'
        Expect ($run.Output -like '*::error title=Plug stall gate::bench_frame_path mock default S4 dish A: longest gap 201.7 ms is not under 50 ms*') "annotation; output: $(Neutral $run.Output)"
        Expect (Test-Path (Join-Path $badOut 'measurements-9.9.9.md')) 'markdown still written'

        Test 'measurements-report.ps1: trend warnings are annotations and leave the exit code alone'
        $prev = Join-Path $dir 'prev'
        New-Item -ItemType Directory -Path $prev | Out-Null
        $previousDoc = New-Measurements @(Read-BenchDirectory $in) '9.9.8' '' 'then'
        WriteText (Join-Path $prev 'measurements-9.9.8.json') ($previousDoc | ConvertTo-Json -Depth 10)
        $slow = @{ (ChangeKey 'bench_frame_path' 'vigem' 50 'S6' 'A') = @{ maxUs = 700.0 } }
        WriteFullSet (Join-Path $dir 'slow') $slow
        $run = InvokeReportScript @('-InputDir', (Quote (Join-Path $dir 'slow')), '-OutputDir', (Quote (Join-Path $dir 'slowout')), '-Version', '9.9.9', '-PreviousDir', (Quote $prev))
        ExpectEq $run.Code 0 'exit code'
        Expect ($run.Output -like '*::warning title=Measurement trend::bench_frame_path vigem S6 dish A max is 700.0 us, was 60.5 us in 9.9.8, +639.5 us*') "annotation; output: $(Neutral $run.Output)"

        Test 'measurements-report.ps1: -AllowPartial lets a partial set pass'
        $part = Join-Path $dir 'part'
        WriteFullSet $part @{} @('bench_frame_path_lockstats-mock.json')
        $run = InvokeReportScript @('-InputDir', (Quote $part), '-OutputDir', (Quote (Join-Path $dir 'partout')), '-Version', '9.9.9')
        ExpectEq $run.Code 1 'a missing run fails the gate'
        Expect ($run.Output -like '*missing run: bench_frame_path_lockstats mock default*') 'names it'
        $run = InvokeReportScript @('-InputDir', (Quote $part), '-OutputDir', (Quote (Join-Path $dir 'partout')), '-Version', '9.9.9', '-AllowPartial')
        ExpectEq $run.Code 0 'allowed'

        Test 'measurements-report.ps1: -GapLimitMs moves the gate'
        $run = InvokeReportScript @('-InputDir', (Quote (Join-Path $dir 'bad')), '-OutputDir', (Quote (Join-Path $dir 'wide')), '-Version', '9.9.9', '-GapLimitMs', '250')
        ExpectEq $run.Code 0 'a 201.7 ms gap is under 250 ms'

        Test 'measurements-report.ps1: bad input exits 1 with the reason as an error annotation'
        $run = InvokeReportScript @('-InputDir', (Quote (Join-Path $dir 'nope')), '-OutputDir', (Quote $out), '-Version', '9.9.9')
        ExpectEq $run.Code 1 'missing input directory'
        Expect ($run.Output -like '*::error title=Measurements report::input directory * does not exist*') 'annotation'
        $run = InvokeReportScript @('-InputDir', (Quote $in), '-OutputDir', (Quote $out), '-Version', '..\x')
        ExpectEq $run.Code 1 'bad version label'
        $run = InvokeReportScript @('-InputDir', (Quote $in), '-OutputDir', (Quote $out))
        ExpectEq $run.Code 1 'no version'
        Expect ($run.Output -like '*-Version is required*') 'says so'

        Test 'measurements-report.ps1: the commit falls back to GITHUB_SHA'
        $run = InvokeReportScript @('-InputDir', (Quote $in), '-OutputDir', (Quote (Join-Path $dir 'sha')), '-Version', '9.9.9') @{ GITHUB_SHA = '0123456789abcdef' }
        ExpectEq $run.Code 0 'exit code'
        ExpectEq (ReadJson (Join-Path $dir 'sha\measurements-9.9.9.json')).commit '0123456789abcdef' 'commit recorded'
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

function TestAnnotationEscaping {
    Test 'Write-Annotation: percent signs and line breaks cannot end the command early'
    $line = (Write-Annotation 'warning' 'T' "50% done`r`nnext" 6>&1) -join ''
    ExpectEq $line '::warning title=T::50%25 done%0D%0Anext' 'escaped'
}

function FindBenchBinary([string]$Name) {
    foreach ($dir in @($repoRoot, (Join-Path $repoRoot 'Release'))) {
        $path = Join-Path $dir "$Name.exe"
        if (Test-Path $path) { return $path }
    }
    return $null
}

function TestRealBenchSchema {
    $dir = NewTempDir
    try {
        foreach ($target in $targets) {
            $exe = FindBenchBinary $target
            if ($null -eq $exe) {
                Write-Host "  SKIP $target.exe is not built; the schema check against the real bench needs it"
                continue
            }
            Test "$target --smoke --json: the report script reads what the bench really writes"
            $json = Join-Path $dir "$target.json"
            $proc = Start-Process -FilePath $exe -ArgumentList @('--smoke', '--json', (Quote $json)) -PassThru -NoNewWindow `
                -RedirectStandardOutput (Join-Path $dir "$target.out") -RedirectStandardError (Join-Path $dir "$target.err")
            $null = $proc.Handle
            $proc.WaitForExit()
            ExpectEq $proc.ExitCode 0 'the smoke run succeeds'
            $run = Read-BenchFile $json
            ExpectEq $run.Target $target 'target from the lockStats flag'
            ExpectEq $run.Variant 'default' 'variant from micHz'
            ExpectEq $run.Rows.Count 30 'S1 once and S2 to S8 for two dishes, on each of the two smoke backends'
            $mockGate = @($run.Rows | Where-Object { $_.dish -eq 'A' -and $_.backend -eq 'mock' -and $_.scenario -in $StallScenarios })
            ExpectEq $mockGate.Count 5 'every PUT scenario has a dish A row'
            Expect (@($run.Rows | Where-Object { $_.longestGapMs -le 0 -or $_.p999Us -le 0 }).Count -eq 0) 'every figure the report reads is filled in'
            $wantLocks = $target -eq 'bench_frame_path_lockstats'
            ExpectEq ($run.Locks.Count -gt 0) $wantLocks 'lock rows exist exactly in the stats build'
        }
    } finally {
        Remove-Item -Recurse -Force $dir
    }
}

Write-Host 'Running measurements report tests...'
Write-Host ''
try {
    TestVersionAndFormatting
    TestReadBenchFile
    TestReadBenchFileRefusals
    TestMerge
    TestGate
    TestGateCompleteness
    TestTrendRule
    TestTrendComparison
    TestPreviousSelection
    TestMarkdown
    TestOutputFiles
    TestScriptProcess
    TestAnnotationEscaping
    TestRealBenchSchema
} finally {
    $env:GITHUB_STEP_SUMMARY = $savedSummary
    $env:ImageVersion = $savedImage
}
Write-Host ''
Write-Host '=== Test Results ==='
Write-Host "  Passed: $($script:pass)"
Write-Host "  Failed: $($script:fail)"
if ($script:fail -eq 0) { Write-Host '  STATUS: ALL PASSED' } else { Write-Host '  STATUS: FAILED' }
if ($script:fail -ne 0) { exit 1 }
exit 0
