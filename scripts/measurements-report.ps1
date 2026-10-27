[CmdletBinding()]
param(
    [string]$InputDir = '',
    [string]$OutputDir = '',
    [string]$Version = '',
    [string]$PreviousDir = '',
    [string]$Commit = '',
    [double]$GapLimitMs = 50,
    [double]$TrendRatio = 2,
    [double]$TrendMinDeltaUs = 100,
    [switch]$AllowPartial
)

$ErrorActionPreference = 'Stop'

$Invariant = [Globalization.CultureInfo]::InvariantCulture
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$SchemaVersion = 1
$BenchName = 'frame_path_bench'
$LatencyTarget = 'bench_frame_path'
$LockStatsTarget = 'bench_frame_path_lockstats'
$DefaultMicHz = 50
$DefaultVariant = 'default'
$ExpectedTargets = @($LatencyTarget, $LockStatsTarget)
$ExpectedBackends = @('mock', 'vigem')
$ExpectedVariants = @('default', 'mic1000')
$StallScenarios = @('S4', 'S5', 'S6', 'S7', 'S8')
$MicScenarios = @('S6')
$StallDish = 'A'
$VersionPattern = '^[0-9A-Za-z][0-9A-Za-z._+-]*$'
$RequiredRowFields = @('p50Us', 'p99Us', 'p999Us', 'maxUs', 'longestGapMs', 'frames')
$RequiredTextFields = @('backend', 'scenario', 'dish')
$LatencyFields = [ordered]@{
    rateHz = 'count'; frames = 'count'; p50Us = 'number'; p99Us = 'number'; p999Us = 'number'; maxUs = 'number'
    overOneMs = 'count'; longestGapMs = 'number'; drops = 'count'; refused = 'count'; motion = 'count'
    mic = 'count'; heartbeats = 'count'
}
$LockFields = [ordered]@{
    acquisitions = 'count'; contended = 'count'; failedTries = 'count'
    maxWaitUs = 'number'; p99WaitUs = 'number'; maxHoldUs = 'number'
}
$TrendMetrics = [ordered]@{ p999Us = 'p99.9'; maxUs = 'max' }
$ShortCommitLength = 7

function Assert-VersionLabel([string]$Value) {
    if ($Value -notmatch $VersionPattern) {
        throw "version label '$Value' may only hold letters, digits, '.', '_', '+' and '-'"
    }
}

function Format-Fixed([double]$Value, [int]$Decimals) {
    return $Value.ToString("F$Decimals", $Invariant)
}

function Format-Compact([double]$Value) {
    return $Value.ToString('0.###', $Invariant)
}

function Format-Count($Value) {
    return ([long]$Value).ToString($Invariant)
}

function Get-ShortCommit([string]$Commit) {
    if ($Commit.Length -le $ShortCommitLength) { return $Commit }
    return $Commit.Substring(0, $ShortCommitLength)
}

function Get-VersionSortKey([string]$Text) {
    $parts = foreach ($digits in [regex]::Matches($Text, '\d+')) { $digits.Value.PadLeft(8, '0') }
    return ($parts -join '')
}

function Get-RunVariant($Settings) {
    $micHz = [int]$Settings.micHz
    if ($micHz -eq $DefaultMicHz) { return $DefaultVariant }
    return "mic$micHz"
}

function Test-ScenarioKept([string]$Variant, [string]$Scenario) {
    if ($Variant -eq $DefaultVariant) { return $true }
    return ($MicScenarios -contains $Scenario)
}

function Get-ScenarioLabel($Row) {
    if ($Row.variant -match '^mic(\d+)$') { return "$($Row.scenario) (mic $($Matches[1]) Hz)" }
    return [string]$Row.scenario
}

function Assert-BenchRow($Row, [string]$File) {
    foreach ($name in $RequiredTextFields) {
        if ([string]::IsNullOrEmpty([string]$Row.$name)) { throw "$File has a row without $name" }
    }
    foreach ($name in $RequiredRowFields) {
        if ($null -eq $Row.$name) { throw "$File has a row for $($Row.scenario) without $name" }
    }
}

function ConvertTo-Figure($Value, [string]$Kind) {
    if ($Kind -eq 'count') {
        if ($null -eq $Value) { return [long]0 }
        return [long]$Value
    }
    if ($null -eq $Value) { return [double]0 }
    return [double]$Value
}

function New-LatencyRow($Row, [string]$Target, [string]$Variant) {
    $out = [ordered]@{
        target   = $Target
        variant  = $Variant
        backend  = [string]$Row.backend
        scenario = [string]$Row.scenario
        dish     = [string]$Row.dish
    }
    foreach ($name in $LatencyFields.Keys) { $out[$name] = ConvertTo-Figure $Row.$name $LatencyFields[$name] }
    return $out
}

function New-LockRow($Row, [string]$Target, [string]$Variant) {
    $out = [ordered]@{
        target   = $Target
        variant  = $Variant
        backend  = [string]$Row.backend
        scenario = [string]$Row.scenario
        lock     = [string]$Row.lock
        role     = [string]$Row.role
    }
    foreach ($name in $LockFields.Keys) { $out[$name] = ConvertTo-Figure $Row.$name $LockFields[$name] }
    return $out
}

function Read-BenchFile([string]$Path) {
    $file = Split-Path -Leaf $Path
    $text = [IO.File]::ReadAllText($Path)
    try { $json = $text | ConvertFrom-Json } catch { throw "$file is not valid JSON: $($_.Exception.Message)" }
    if ($null -eq $json -or $json.bench -ne $BenchName) { throw "$file is not $BenchName output" }
    $sourceRows = @($json.rows)
    if ($null -eq $json.rows -or $sourceRows.Count -eq 0) { throw "$file has no rows" }
    if ($null -eq $json.settings -or $null -eq $json.settings.micHz) { throw "$file has no settings.micHz, so its run cannot be named" }
    $target = if ([bool]$json.lockStats) { $LockStatsTarget } else { $LatencyTarget }
    $variant = Get-RunVariant $json.settings
    $rows = New-Object 'System.Collections.Generic.List[object]'
    foreach ($row in $sourceRows) {
        Assert-BenchRow $row $file
        if (Test-ScenarioKept $variant ([string]$row.scenario)) { $rows.Add((New-LatencyRow $row $target $variant)) }
    }
    $locks = New-Object 'System.Collections.Generic.List[object]'
    foreach ($lock in @($json.locks)) {
        if ($null -eq $lock) { continue }
        if (Test-ScenarioKept $variant ([string]$lock.scenario)) { $locks.Add((New-LockRow $lock $target $variant)) }
    }
    return [pscustomobject]@{
        File     = $file
        Target   = $target
        Variant  = $variant
        Cpu      = [string]$json.cpu
        Build    = [string]$json.build
        Settings = $json.settings
        Rows     = $rows.ToArray()
        Locks    = $locks.ToArray()
    }
}

function Read-BenchDirectory([string]$Dir) {
    if (-not (Test-Path -LiteralPath $Dir -PathType Container)) { throw "input directory $Dir does not exist" }
    $files = @(Get-ChildItem -LiteralPath $Dir -Filter '*.json' -File | Sort-Object Name)
    if ($files.Count -eq 0) { throw "no bench JSON files in $Dir" }
    foreach ($file in $files) { Read-BenchFile $file.FullName }
}

function Get-RowKey($Row) {
    return "$($Row.target)|$($Row.variant)|$($Row.backend)|$($Row.scenario)|$($Row.dish)"
}

function Get-RunLabel($Row) {
    return "$($Row.target) $($Row.backend) $($Row.variant)"
}

function Assert-NoDuplicateRows($Rows) {
    $seen = @{}
    foreach ($row in $Rows) {
        $key = Get-RowKey $row
        if ($seen.ContainsKey($key)) { throw "two bench files cover the same row: $key" }
        $seen[$key] = $true
    }
}

function Get-MissingRuns($Rows) {
    $present = @{}
    foreach ($row in $Rows) { $present[(Get-RunLabel $row)] = $true }
    foreach ($target in $ExpectedTargets) {
        foreach ($backend in $ExpectedBackends) {
            foreach ($variant in $ExpectedVariants) {
                $label = "$target $backend $variant"
                if (-not $present.ContainsKey($label)) { $label }
            }
        }
    }
}

function Get-RequiredStallScenarios([string]$Variant) {
    if ($Variant -eq $DefaultVariant) { return $StallScenarios }
    return @($MicScenarios | Where-Object { $StallScenarios -contains $_ })
}

function Get-StallGate($Rows, [double]$LimitMs, [bool]$RequireAllRuns) {
    $results = New-Object 'System.Collections.Generic.List[object]'
    $failures = New-Object 'System.Collections.Generic.List[string]'
    if ($RequireAllRuns) {
        foreach ($label in @(Get-MissingRuns $Rows)) { $failures.Add("missing run: $label") }
    }
    $runs = @{}
    foreach ($row in $Rows) { $runs[(Get-RunLabel $row)] = $row }
    foreach ($label in @($runs.Keys | Sort-Object)) {
        $sample = $runs[$label]
        foreach ($scenario in (Get-RequiredStallScenarios $sample.variant)) {
            $match = @($Rows | Where-Object {
                (Get-RunLabel $_) -eq $label -and $_.scenario -eq $scenario -and $_.dish -eq $StallDish
            })
            if ($match.Count -eq 0) {
                $failures.Add("$label has no dish $StallDish row for $scenario, so its plug stall cannot be judged")
                continue
            }
            $gap = [double]$match[0].longestGapMs
            $passed = $gap -lt $LimitMs
            $results.Add([ordered]@{
                target       = $sample.target
                variant      = $sample.variant
                backend      = $sample.backend
                scenario     = $scenario
                longestGapMs = $gap
                limitMs      = $LimitMs
                passed       = $passed
            })
            if (-not $passed) {
                $failures.Add("$label $scenario dish ${StallDish}: longest gap $(Format-Fixed $gap 1) ms is not under $(Format-Compact $LimitMs) ms")
            }
        }
    }
    return [ordered]@{
        name     = 'plugStall'
        limitMs  = $LimitMs
        passed   = ($failures.Count -eq 0)
        results  = $results.ToArray()
        failures = $failures.ToArray()
    }
}

function Read-PreviousMeasurements([string]$Dir, [string]$ExcludeVersion) {
    if (-not $Dir -or -not (Test-Path -LiteralPath $Dir -PathType Container)) { return $null }
    $files = @(Get-ChildItem -LiteralPath $Dir -Filter 'measurements-*.json' -File |
        Sort-Object { Get-VersionSortKey $_.Name })
    $found = $null
    foreach ($file in $files) {
        try { $parsed = [IO.File]::ReadAllText($file.FullName) | ConvertFrom-Json } catch {
            Write-Host "::notice::skipping $($file.Name), which is not valid JSON"
            continue
        }
        $usable = $null -ne $parsed -and $parsed.schema -eq $SchemaVersion -and $null -ne $parsed.latency
        if (-not $usable) {
            Write-Host "::notice::skipping $($file.Name), which is not a schema $SchemaVersion measurements file"
            continue
        }
        if ([string]$parsed.version -eq $ExcludeVersion) { continue }
        $found = $parsed
    }
    return $found
}

function Test-TrendRegression([double]$Current, [double]$Previous, [double]$Ratio, [double]$MinDeltaUs) {
    $muchHigher = $Current -gt ($Previous * $Ratio)
    $farHigher = ($Current - $Previous) -gt $MinDeltaUs
    return ($muchHigher -and $farHigher)
}

function Get-TrendComparison($Rows, $Previous, [double]$Ratio, [double]$MinDeltaUs) {
    $warnings = New-Object 'System.Collections.Generic.List[string]'
    $previousValues = @{}
    if ($null -eq $Previous) {
        return [ordered]@{ available = $false; previousVersion = ''; warnings = @(); previousValues = $previousValues }
    }
    $previousByKey = @{}
    foreach ($row in @($Previous.latency)) { $previousByKey[(Get-RowKey $row)] = $row }
    foreach ($row in $Rows) {
        $key = Get-RowKey $row
        if (-not $previousByKey.ContainsKey($key)) { continue }
        $before = $previousByKey[$key]
        foreach ($metric in $TrendMetrics.Keys) {
            $now = [double]$row.$metric
            $was = [double]$before.$metric
            $previousValues["$key|$metric"] = $was
            if (-not (Test-TrendRegression $now $was $Ratio $MinDeltaUs)) { continue }
            $label = $TrendMetrics[$metric]
            $subject = "$($row.target) $($row.backend) $(Get-ScenarioLabel $row) dish $($row.dish) $label"
            $change = "$(Format-Fixed $now 1) us, was $(Format-Fixed $was 1) us in $($Previous.version), +$(Format-Fixed ($now - $was) 1) us"
            $warnings.Add("$subject is $change")
        }
    }
    return [ordered]@{
        available       = $true
        previousVersion = [string]$Previous.version
        warnings        = $warnings.ToArray()
        previousValues  = $previousValues
    }
}

function New-MarkdownTable([string[]]$Headers, $Rows, [int]$TextColumns) {
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('| ' + ($Headers -join ' | ') + ' |')
    $rule = for ($i = 0; $i -lt $Headers.Count; $i++) { if ($i -lt $TextColumns) { '---' } else { '---:' } }
    $lines.Add('| ' + ($rule -join ' | ') + ' |')
    foreach ($cells in $Rows) { $lines.Add('| ' + ($cells -join ' | ') + ' |') }
    return ($lines -join "`n")
}

function Get-RowOrder($Row, [int]$Position) {
    $micHz = 0
    if ($Row.variant -match '^mic(\d+)$') { $micHz = 1 + [int]$Matches[1] }
    $scenarioNumber = [int]([string]$Row.scenario).Substring(1)
    $backendIndex = [array]::IndexOf($ExpectedBackends, [string]$Row.backend)
    if ($backendIndex -lt 0) { $backendIndex = $ExpectedBackends.Count }
    return ((([long]$micHz * 1000 + $scenarioNumber) * 100 + $backendIndex) * 1000000 + $Position)
}

function Sort-MeasurementRows($Rows) {
    $ordered = for ($i = 0; $i -lt $Rows.Count; $i++) {
        [pscustomobject]@{ Order = (Get-RowOrder $Rows[$i] $i); Row = $Rows[$i] }
    }
    return @($ordered | Sort-Object Order | ForEach-Object { $_.Row })
}

function Format-LatencyTable($Rows, $Trend) {
    $headers = @('scenario', 'mode', 'rate Hz', 'frames', 'p50 us', 'p99 us', 'p99.9 us', 'max us', 'over 1 ms', 'longest gap ms', 'drops')
    if ($Trend.available) {
        $headers += @("p99.9 us in $($Trend.previousVersion)", "max us in $($Trend.previousVersion)")
    }
    $table = New-Object 'System.Collections.Generic.List[string[]]'
    foreach ($row in (Sort-MeasurementRows $Rows)) {
        $cells = @(
            (Get-ScenarioLabel $row), $row.backend, (Format-Count $row.rateHz), (Format-Count $row.frames),
            (Format-Fixed $row.p50Us 1), (Format-Fixed $row.p99Us 1), (Format-Fixed $row.p999Us 1),
            (Format-Fixed $row.maxUs 1), (Format-Count $row.overOneMs), (Format-Fixed $row.longestGapMs 3),
            (Format-Count $row.drops)
        )
        if ($Trend.available) {
            $key = Get-RowKey $row
            foreach ($metric in $TrendMetrics.Keys) {
                $cells += $(if ($Trend.previousValues.ContainsKey("$key|$metric")) { Format-Fixed $Trend.previousValues["$key|$metric"] 1 } else { '-' })
            }
        }
        $table.Add([string[]]$cells)
    }
    return (New-MarkdownTable $headers $table 2)
}

function Format-LockTable($Locks) {
    $headers = @('mode', 'scenario', 'lock', 'role', 'acquisitions', 'contended', 'failed tries', 'max wait us', 'p99 wait us', 'max hold us')
    $table = New-Object 'System.Collections.Generic.List[string[]]'
    foreach ($row in (Sort-MeasurementRows $Locks)) {
        $table.Add([string[]]@(
            $row.backend, (Get-ScenarioLabel $row), $row.lock, $row.role, (Format-Count $row.acquisitions),
            (Format-Count $row.contended), (Format-Count $row.failedTries), (Format-Fixed $row.maxWaitUs 1),
            (Format-Fixed $row.p99WaitUs 1), (Format-Fixed $row.maxHoldUs 1)
        ))
    }
    return (New-MarkdownTable $headers $table 4)
}

function Format-Settings($Settings) {
    $pace = "$(Format-Compact ([double]$Settings.secs)) s a rep, $($Settings.reps) reps interleaved"
    $rates = "dish A $($Settings.rateA) Hz, dish B $($Settings.rateB) Hz, motion $($Settings.motionHz) Hz, mic $($Settings.micHz) Hz"
    $costs = "submit $($Settings.submitUs) us, plug $($Settings.plugMs) ms, receiver on CPU $($Settings.rxCpu)"
    return "$pace, $rates, $costs"
}

function Format-MachineLine($Measurements) {
    $parts = New-Object 'System.Collections.Generic.List[string]'
    if ($Measurements.commit) { $parts.Add("commit ``$(Get-ShortCommit ([string]$Measurements.commit))``") }
    $parts.Add([string]$Measurements.machine.cpu)
    $parts.Add("$($Measurements.machine.build) build")
    if ($Measurements.machine.image) { $parts.Add("runner image $($Measurements.machine.image)") }
    return ('Measured at ' + ($parts -join ', ') + '.')
}

function Format-GateMarkdown($Gate) {
    $limit = Format-Compact $Gate.limitMs
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('## Plug stall gate')
    $lines.Add('')
    if ($Gate.passed) {
        $worst = @($Gate.results | Sort-Object { $_.longestGapMs } -Descending | Select-Object -First 1)
        $detail = ''
        if ($worst.Count -eq 1) {
            $where = "$($worst[0].backend) $(Get-ScenarioLabel $worst[0]) in $($worst[0].target)"
            $detail = " (the longest was $(Format-Fixed $worst[0].longestGapMs 3) ms, $where)"
        }
        $lines.Add("PASS: dish A's longest gap stays under $limit ms in every scenario with a PUT$detail.")
        return ($lines -join "`n")
    }
    $lines.Add("FAIL: dish A's longest gap must stay under $limit ms in every scenario with a PUT.")
    $lines.Add('')
    foreach ($failure in $Gate.failures) { $lines.Add("- $failure") }
    return ($lines -join "`n")
}

function Format-TrendMarkdown($Trend, [double]$Ratio, [double]$MinDeltaUs) {
    $lines = New-Object 'System.Collections.Generic.List[string]'
    if (-not $Trend.available) {
        $lines.Add('## Trend')
        $lines.Add('')
        $lines.Add('No earlier release carries measurements, so there is nothing to compare with.')
        return ($lines -join "`n")
    }
    $lines.Add("## Trend against $($Trend.previousVersion)")
    $lines.Add('')
    $rule = "more than $(Format-Compact $Ratio) times and more than $(Format-Compact $MinDeltaUs) us above"
    if ($Trend.warnings.Count -eq 0) {
        $lines.Add("No p99.9 or max is $rule the previous release.")
        return ($lines -join "`n")
    }
    $lines.Add("Warnings only, because shared runners are noisy. Each p99.9 or max below is $rule the previous release.")
    $lines.Add('')
    foreach ($warning in $Trend.warnings) { $lines.Add("- $warning") }
    return ($lines -join "`n")
}

function Format-MeasurementsMarkdown($Measurements, $Gate, $Trend, [double]$Ratio, [double]$MinDeltaUs) {
    $latencyRows = @($Measurements.latency | Where-Object { $_.target -eq $LatencyTarget -and $_.dish -eq $StallDish })
    $lockRows = @($Measurements.locks | Where-Object { $_.target -eq $LockStatsTarget })
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add("# Satellite $($Measurements.version) measurements")
    $lines.Add('')
    $lines.Add((Format-MachineLine $Measurements))
    $lines.Add('')
    $lines.Add("$(Format-Settings $Measurements.settings). A row is the rep whose dish A p99.9 is the median of its reps.")
    $lines.Add('')
    $lines.Add((Format-GateMarkdown $Gate))
    $lines.Add('')
    $lines.Add((Format-TrendMarkdown $Trend $Ratio $MinDeltaUs))
    $lines.Add('')
    $lines.Add("## Dish A latency ($LatencyTarget)")
    $lines.Add('')
    $lines.Add('Latencies are in microseconds and the longest gap is in milliseconds. "over 1 ms" counts frames slower than 1 ms and "drops" counts ring overflows.')
    $lines.Add('')
    $lines.Add((Format-LatencyTable $latencyRows $Trend))
    $lines.Add('')
    $lines.Add("## Lock waits by thread role ($LockStatsTarget)")
    $lines.Add('')
    if ($lockRows.Count -eq 0) {
        $lines.Add('No lock-stats run was supplied.')
    } else {
        $lines.Add('Waits and holds are in microseconds, and a p99 wait is the upper edge of its log2 bucket.')
        $lines.Add('')
        $lines.Add((Format-LockTable $lockRows))
    }
    return (($lines -join "`n") + "`n")
}

function Write-Annotation([string]$Level, [string]$Title, [string]$Message) {
    $escaped = $Message -replace '%', '%25' -replace "`r", '%0D' -replace "`n", '%0A'
    Write-Host "::$Level title=$Title::$escaped"
}

function New-Measurements($Runs, [string]$Version, [string]$Commit, [string]$GeneratedAt) {
    $latency = New-Object 'System.Collections.Generic.List[object]'
    $locks = New-Object 'System.Collections.Generic.List[object]'
    $sources = New-Object 'System.Collections.Generic.List[object]'
    foreach ($run in $Runs) {
        foreach ($row in $run.Rows) { $latency.Add($row) }
        foreach ($row in $run.Locks) { $locks.Add($row) }
        $sources.Add([ordered]@{ file = $run.File; target = $run.Target; variant = $run.Variant; settings = $run.Settings })
    }
    Assert-NoDuplicateRows $latency
    $first = @($Runs | Where-Object { $_.Variant -eq $DefaultVariant } | Select-Object -First 1)
    if ($first.Count -eq 0) { $first = @($Runs[0]) }
    return [ordered]@{
        schema      = $SchemaVersion
        version     = $Version
        commit      = $Commit
        generatedAt = $GeneratedAt
        machine     = [ordered]@{
            cpu   = $first[0].Cpu
            build = $first[0].Build
            os    = [Environment]::OSVersion.VersionString
            image = [string]$env:ImageVersion
        }
        settings    = $first[0].Settings
        sources     = $sources.ToArray()
        latency     = $latency.ToArray()
        locks       = $locks.ToArray()
    }
}

function Write-TextFile([string]$Path, [string]$Text) {
    [IO.File]::WriteAllText($Path, $Text, $Utf8NoBom)
}

function Add-StepSummary([string]$Text) {
    if (-not $env:GITHUB_STEP_SUMMARY) { return }
    [IO.File]::AppendAllText($env:GITHUB_STEP_SUMMARY, $Text + "`n", $Utf8NoBom)
}

function Invoke-MeasurementsReport {
    param(
        [string]$InputDir,
        [string]$OutputDir,
        [string]$Version,
        [string]$PreviousDir,
        [string]$Commit,
        [double]$GapLimitMs,
        [double]$TrendRatio,
        [double]$TrendMinDeltaUs,
        [bool]$AllowPartial
    )
    if (-not $InputDir) { throw '-InputDir is required' }
    if (-not $OutputDir) { throw '-OutputDir is required' }
    if (-not $Version) { throw '-Version is required' }
    Assert-VersionLabel $Version
    $runs = @(Read-BenchDirectory $InputDir)
    $generatedAt = [DateTime]::UtcNow.ToString("yyyy-MM-dd'T'HH:mm:ss'Z'", $Invariant)
    $measurements = New-Measurements $runs $Version $Commit $generatedAt
    $rows = @($measurements.latency)
    $gate = Get-StallGate $rows $GapLimitMs (-not $AllowPartial)
    $previous = Read-PreviousMeasurements $PreviousDir $Version
    $trend = Get-TrendComparison $rows $previous $TrendRatio $TrendMinDeltaUs
    $measurements['gate'] = $gate
    $measurements['trend'] = [ordered]@{
        previousVersion = $trend.previousVersion
        ratio           = $TrendRatio
        minDeltaUs      = $TrendMinDeltaUs
        warnings        = $trend.warnings
    }
    $markdown = Format-MeasurementsMarkdown $measurements $gate $trend $TrendRatio $TrendMinDeltaUs
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    $outDir = (Resolve-Path -LiteralPath $OutputDir).Path
    $jsonPath = Join-Path $outDir "measurements-$Version.json"
    $markdownPath = Join-Path $outDir "measurements-$Version.md"
    Write-TextFile $jsonPath (($measurements | ConvertTo-Json -Depth 10) + "`n")
    Write-TextFile $markdownPath $markdown
    Add-StepSummary $markdown
    foreach ($warning in $trend.warnings) { Write-Annotation 'warning' 'Measurement trend' $warning }
    foreach ($failure in $gate.failures) { Write-Annotation 'error' 'Plug stall gate' $failure }
    $verdict = if ($gate.passed) { 'PASS' } else { 'FAIL' }
    $comparison = if ($trend.available) { "compared with $($trend.previousVersion), $($trend.warnings.Count) warning(s)" } else { 'no earlier release to compare with' }
    Write-Host "measurements: $($runs.Count) bench files, $($rows.Count) latency rows, $(@($measurements.locks).Count) lock rows"
    Write-Host "plug stall gate: $verdict (limit $(Format-Compact $GapLimitMs) ms)"
    Write-Host "trend: $comparison"
    Write-Host "wrote $jsonPath"
    Write-Host "wrote $markdownPath"
    if ($gate.passed) { return 0 }
    return 1
}

if ($MyInvocation.InvocationName -ne '.') {
    $exitCode = 1
    $commitText = if ($Commit) { $Commit } else { [string]$env:GITHUB_SHA }
    try {
        $result = @(Invoke-MeasurementsReport -InputDir $InputDir -OutputDir $OutputDir -Version $Version `
            -PreviousDir $PreviousDir -Commit $commitText -GapLimitMs $GapLimitMs `
            -TrendRatio $TrendRatio -TrendMinDeltaUs $TrendMinDeltaUs -AllowPartial ([bool]$AllowPartial))
        $exitCode = [int]$result[-1]
    } catch {
        Write-Host "::error title=Measurements report::$($_.Exception.Message)"
        Write-Host "measurements-report failed: $($_.Exception.Message)"
    }
    exit $exitCode
}
