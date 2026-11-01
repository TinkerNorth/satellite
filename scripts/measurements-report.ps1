[CmdletBinding()]
param(
    [string]$InputDir = '',
    [string]$OutputDir = '',
    [string]$Version = '',
    [string]$PreviousDir = '',
    [string]$Commit = '',
    [double]$GapPlugFraction = 0.5,
    [double]$TrendRatio = 2,
    [double]$TrendMinDeltaUs = 100,
    [switch]$AllowPartial
)

$ErrorActionPreference = 'Stop'

$Invariant = [Globalization.CultureInfo]::InvariantCulture
$Utf8NoBom = New-Object System.Text.UTF8Encoding($false)
$SchemaVersion = 2
$BenchName = 'frame_path_bench'
$LatencyTarget = 'bench_frame_path'
$LockStatsTarget = 'bench_frame_path_lockstats'
$DefaultMicHz = 50
$DefaultVariant = 'default'
$ExpectedTargets = @($LatencyTarget, $LockStatsTarget)
$ExpectedBackends = @('mock', 'vigem')
$ExpectedVariants = @('default', 'mic1000')
$StallScenarios = @('S4', 'S5', 'S6', 'S7', 'S8')
$QuietScenarios = @('S2', 'S3')
$MicScenarios = @('S6')
$LockFreeRoles = @('Receive', 'Pointer', 'Mic')
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
$HeadlineScenario = 'S2'
$HeadlineBackends = @('vigem', 'mock')
$FrameRatesHz = @(240, 60)
$BestFields = @('p50Us', 'p99Us', 'p999Us', 'maxUs')
$WorstFields = @('longestGapMs', 'overOneMs', 'drops')
$TransactionLocks = @('service.topology', 'hidmaestro.helper')

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

function New-LatencyRow($Row, [string]$Target, [string]$Variant, [double]$PlugMs) {
    $out = [ordered]@{
        target   = $Target
        variant  = $Variant
        backend  = [string]$Row.backend
        scenario = [string]$Row.scenario
        dish     = [string]$Row.dish
        plugMs   = $PlugMs
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
    if ($null -eq $json.settings.plugMs) { throw "$file has no settings.plugMs, so its plug stall cannot be judged" }
    $target = if ([bool]$json.lockStats) { $LockStatsTarget } else { $LatencyTarget }
    $variant = Get-RunVariant $json.settings
    $plugMs = [double]$json.settings.plugMs
    $rows = New-Object 'System.Collections.Generic.List[object]'
    foreach ($row in $sourceRows) {
        Assert-BenchRow $row $file
        if (Test-ScenarioKept $variant ([string]$row.scenario)) { $rows.Add((New-LatencyRow $row $target $variant $plugMs)) }
    }
    $locks = New-Object 'System.Collections.Generic.List[object]'
    foreach ($lock in @($json.locks)) {
        if ($null -eq $lock) { continue }
        if (Test-ScenarioKept $variant ([string]$lock.scenario)) { $locks.Add((New-LockRow $lock $target $variant)) }
    }
    $reps = New-Object 'System.Collections.Generic.List[object]'
    foreach ($row in @($json.reps)) {
        if ($null -eq $row) { continue }
        Assert-BenchRow $row $file
        if ($null -eq $row.rep) { throw "$file has a rep row without its rep index" }
        if (-not (Test-ScenarioKept $variant ([string]$row.scenario))) { continue }
        $repRow = New-LatencyRow $row $target $variant $plugMs
        $repRow['rep'] = [int]$row.rep
        $reps.Add($repRow)
    }
    return [pscustomobject]@{
        File     = $file
        Target   = $target
        Variant  = $variant
        Cpu      = [string]$json.cpu
        Build    = [string]$json.build
        Settings = $json.settings
        Rows     = $rows.ToArray()
        Reps     = $reps.ToArray()
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

function Get-NoiseFloor($Rows, [string]$Label) {
    $worst = $null
    foreach ($scenario in $QuietScenarios) {
        $match = @($Rows | Where-Object {
            (Get-RunLabel $_) -eq $Label -and $_.scenario -eq $scenario -and $_.dish -eq $StallDish
        })
        if ($match.Count -eq 0) { continue }
        $gap = [double]$match[0].longestGapMs
        if ($null -eq $worst -or $gap -gt $worst.longestGapMs) {
            $worst = [ordered]@{ scenario = $scenario; longestGapMs = $gap }
        }
    }
    return $worst
}

function Get-FractionPhrase([double]$Fraction) {
    if ($Fraction -eq 0.5) { return 'half' }
    return "$(Format-Compact ($Fraction * 100))%"
}

function Get-StallGate($Rows, [double]$PlugFraction, [bool]$RequireAllRuns) {
    $results = New-Object 'System.Collections.Generic.List[object]'
    $summaries = New-Object 'System.Collections.Generic.List[object]'
    $failures = New-Object 'System.Collections.Generic.List[string]'
    $share = Get-FractionPhrase $PlugFraction
    if ($RequireAllRuns) {
        foreach ($label in @(Get-MissingRuns $Rows)) { $failures.Add("missing run: $label") }
    }
    $runs = @{}
    foreach ($row in $Rows) { $runs[(Get-RunLabel $row)] = $row }
    foreach ($label in @($runs.Keys | Sort-Object)) {
        $sample = $runs[$label]
        $plugMs = [double]$sample.plugMs
        $limitMs = $plugMs * $PlugFraction
        if ($plugMs -le 0) {
            $failures.Add("$label ran with a plug of $(Format-Compact $plugMs) ms, so its plug stall cannot be judged")
            continue
        }
        $noise = Get-NoiseFloor $Rows $label
        $noiseText = ''
        if ($null -ne $noise) { $noiseText = "; the runner noise floor of this run is $(Format-Fixed $noise.longestGapMs 1) ms" }
        $worst = $null
        foreach ($scenario in (Get-RequiredStallScenarios $sample.variant)) {
            $match = @($Rows | Where-Object {
                (Get-RunLabel $_) -eq $label -and $_.scenario -eq $scenario -and $_.dish -eq $StallDish
            })
            if ($match.Count -eq 0) {
                $failures.Add("$label has no dish $StallDish row for $scenario, so its plug stall cannot be judged")
                continue
            }
            $gap = [double]$match[0].longestGapMs
            $passed = $gap -lt $limitMs
            $result = [ordered]@{
                target       = $sample.target
                variant      = $sample.variant
                backend      = $sample.backend
                scenario     = $scenario
                plugMs       = $plugMs
                limitMs      = $limitMs
                longestGapMs = $gap
                passed       = $passed
            }
            $results.Add($result)
            if ($null -eq $worst -or $gap -gt $worst.longestGapMs) { $worst = $result }
            if (-not $passed) {
                $failures.Add("$label $scenario dish ${StallDish}: longest gap $(Format-Fixed $gap 1) ms is not under $(Format-Compact $limitMs) ms, $share of the $(Format-Compact $plugMs) ms plug$noiseText")
            }
        }
        $summaries.Add([ordered]@{
            target             = $sample.target
            variant            = $sample.variant
            backend            = $sample.backend
            plugMs             = $plugMs
            limitMs            = $limitMs
            scenario           = $(if ($null -eq $worst) { '' } else { $worst.scenario })
            longestGapMs       = $(if ($null -eq $worst) { $null } else { $worst.longestGapMs })
            noiseFloorScenario = $(if ($null -eq $noise) { '' } else { $noise.scenario })
            noiseFloorMs       = $(if ($null -eq $noise) { $null } else { $noise.longestGapMs })
            passed             = ($null -ne $worst -and $worst.passed)
        })
    }
    return [ordered]@{
        name         = 'plugStall'
        plugFraction = $PlugFraction
        passed       = ($failures.Count -eq 0)
        runs         = $summaries.ToArray()
        results      = $results.ToArray()
        failures     = $failures.ToArray()
    }
}

function Get-LockLawGate($Rows, $Locks) {
    $failures = New-Object 'System.Collections.Generic.List[string]'
    $recorded = @{}
    foreach ($lock in $Locks) { $recorded[(Get-RunLabel $lock)] = $true }
    $statsRuns = @{}
    foreach ($row in $Rows) {
        if ($row.target -eq $LockStatsTarget) { $statsRuns[(Get-RunLabel $row)] = $true }
    }
    foreach ($label in @($statsRuns.Keys | Sort-Object)) {
        if (-not $recorded.ContainsKey($label)) { $failures.Add("$label recorded no lock at all, so its lock laws cannot be judged") }
    }
    foreach ($lock in $Locks) {
        if ($LockFreeRoles -notcontains [string]$lock.role) { continue }
        $taken = [long]$lock.acquisitions
        if ($taken -le 0) { continue }
        $times = if ($taken -eq 1) { 'time' } else { 'times' }
        $failures.Add("$(Get-RunLabel $lock) $($lock.scenario): the $($lock.role) role acquired $($lock.lock) $(Format-Count $taken) $times")
    }
    return [ordered]@{
        name     = 'lockLaws'
        roles    = $LockFreeRoles
        checked  = @($Locks).Count
        passed   = ($failures.Count -eq 0)
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

function Format-Span($Values) {
    $sorted = @($Values | Sort-Object)
    if ($sorted.Count -eq 0) { return '' }
    $low = Format-Compact ([double]$sorted[0])
    $high = Format-Compact ([double]$sorted[-1])
    if ($low -eq $high) { return "$low ms" }
    return "$low to $high ms"
}

function Get-RunTitle($Run) {
    if ($Run.variant -match '^mic(\d+)$') { return "$($Run.target) (mic $($Matches[1]) Hz)" }
    return [string]$Run.target
}

function Sort-GateRuns($Runs) {
    $ordered = for ($i = 0; $i -lt $Runs.Count; $i++) {
        $run = $Runs[$i]
        $targetIndex = [array]::IndexOf($ExpectedTargets, [string]$run.target) + 1
        $backendIndex = [array]::IndexOf($ExpectedBackends, [string]$run.backend) + 1
        $variantRank = $(if ($run.variant -eq $DefaultVariant) { 0 } else { 1 })
        [pscustomobject]@{ Order = ((($targetIndex * 100 + $backendIndex) * 10 + $variantRank) * 1000 + $i); Run = $run }
    }
    return @($ordered | Sort-Object Order | ForEach-Object { $_.Run })
}

function Format-GateRunsTable($Gate) {
    $headers = @('run', 'mode', 'longest gap with a PUT ms', 'limit ms', 'runner noise floor ms')
    $table = New-Object 'System.Collections.Generic.List[string[]]'
    foreach ($run in (Sort-GateRuns @($Gate.runs))) {
        $worst = '-'
        if ($null -ne $run.longestGapMs) { $worst = "$(Format-Fixed $run.longestGapMs 3) ($($run.scenario))" }
        $floor = '-'
        if ($null -ne $run.noiseFloorMs) { $floor = "$(Format-Fixed $run.noiseFloorMs 3) ($($run.noiseFloorScenario))" }
        $table.Add([string[]]@((Get-RunTitle $run), [string]$run.backend, $worst, (Format-Compact $run.limitMs), $floor))
    }
    return (New-MarkdownTable $headers $table 2)
}

function Format-GateMarkdown($Gate) {
    $share = Get-FractionPhrase $Gate.plugFraction
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('## Plug stall gate')
    $lines.Add('')
    if ($Gate.passed) {
        $worst = $null
        foreach ($result in $Gate.results) {
            if ($null -eq $worst -or $result.longestGapMs -gt $worst.longestGapMs) { $worst = $result }
        }
        $detail = ''
        if ($null -ne $worst) {
            $where = "$($worst.backend) $(Get-ScenarioLabel $worst) in $($worst.target)"
            $detail = "; the longest was $(Format-Fixed $worst.longestGapMs 3) ms, $where"
        }
        $limits = Format-Span @($Gate.results | ForEach-Object { $_.limitMs })
        $plugs = Format-Span @($Gate.results | ForEach-Object { $_.plugMs })
        $lines.Add("PASS: dish A's longest gap stays under $share of the plug in every scenario with a PUT (limit $limits for a plug of $plugs$detail).")
    } else {
        $lines.Add("FAIL: dish A's longest gap must stay under $share of the plug in every scenario with a PUT.")
        $lines.Add('')
        foreach ($failure in $Gate.failures) { $lines.Add("- $failure") }
    }
    if (@($Gate.runs).Count -gt 0) {
        $lines.Add('')
        $lines.Add('The runner noise floor is the longest dish A gap in S2 and S3 of the same run, which stream at the same rate as the stall scenarios and plug nothing.')
        $lines.Add('')
        $lines.Add((Format-GateRunsTable $Gate))
    }
    return ($lines -join "`n")
}

function Format-LockGateMarkdown($LockGate) {
    $roles = @($LockGate.roles)
    $named = ($roles[0..($roles.Count - 2)] -join ', ') + ' and ' + $roles[-1]
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('## Lock law gate')
    $lines.Add('')
    if (-not $LockGate.passed) {
        $lines.Add("FAIL: the $named roles must take no lock in any scenario.")
        $lines.Add('')
        foreach ($failure in $LockGate.failures) { $lines.Add("- $failure") }
    } elseif ($LockGate.checked -eq 0) {
        $lines.Add('No lock-stats run was supplied, so the lock laws were not checked.')
    } else {
        $lines.Add("PASS: the $named roles took no lock in any scenario ($(Format-Count $LockGate.checked) lock rows checked).")
    }
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

function Get-RepSpread($Reps) {
    $spread = @{}
    foreach ($row in $Reps) {
        $key = Get-RowKey $row
        if (-not $spread.ContainsKey($key)) {
            $entry = [ordered]@{ reps = 0 }
            foreach ($field in $BestFields) { $entry["best$field"] = $null }
            foreach ($field in $WorstFields) { $entry["worst$field"] = $null }
            $spread[$key] = $entry
        }
        $entry = $spread[$key]
        $entry.reps = $entry.reps + 1
        foreach ($field in $BestFields) {
            $value = [double]$row.$field
            if ($null -eq $entry["best$field"] -or $value -lt $entry["best$field"]) { $entry["best$field"] = $value }
        }
        foreach ($field in $WorstFields) {
            $value = [double]$row.$field
            if ($null -eq $entry["worst$field"] -or $value -gt $entry["worst$field"]) { $entry["worst$field"] = $value }
        }
    }
    return $spread
}

function Format-FrameShare([double]$Ms, [int]$Hz) {
    $frames = $Ms * $Hz / 1000.0
    if ($frames -ge 1) { return "$(Format-Fixed $frames 1) frames at $Hz Hz" }
    return "$(Format-Fixed ($frames * 100.0) 1)% of a $Hz Hz frame"
}

function Format-FrameShares([double]$Ms) {
    return (($FrameRatesHz | ForEach-Object { Format-FrameShare $Ms $_ }) -join ' and ')
}

function Select-HeadlineRow($Rows) {
    foreach ($backend in $HeadlineBackends) {
        $match = @($Rows | Where-Object {
            $_.target -eq $LatencyTarget -and $_.variant -eq $DefaultVariant -and $_.backend -eq $backend -and
            $_.scenario -eq $HeadlineScenario -and $_.dish -eq $StallDish
        })
        if ($match.Count -gt 0) { return $match[0] }
    }
    return $null
}

function Get-WorstBy($Rows, [string]$Field) {
    $worst = $null
    foreach ($row in $Rows) {
        if ($null -eq $worst -or [double]$row.$Field -gt [double]$worst.$Field) { $worst = $row }
    }
    return $worst
}

function Get-Headline($Measurements, $Gate, $LockGate, $Spread) {
    $rows = @($Measurements.latency)
    $settings = $Measurements.settings
    $secs = [double]$settings.secs
    $headline = [ordered]@{ inputCost = $null; connectPause = $null; noiseFloor = $null; locks = $null }
    $cost = Select-HeadlineRow $rows
    if ($null -ne $cost) {
        $entry = $Spread[(Get-RowKey $cost)]
        $bestP999 = $null
        $reps = 1
        if ($null -ne $entry) { $bestP999 = $entry.bestp999Us; $reps = $entry.reps }
        $updates = 0
        if ($secs -gt 0) { $updates = [math]::Round([double]$cost.frames / $secs) }
        $headline.inputCost = [ordered]@{
            backend       = $cost.backend
            scenario      = $cost.scenario
            rateHz        = [long]$cost.rateHz
            p50Us         = [double]$cost.p50Us
            p99Us         = [double]$cost.p99Us
            p999Us        = [double]$cost.p999Us
            bestP999Us    = $bestP999
            reps          = $reps
            updatesPerSec = $updates
            frames        = [long]$cost.frames
            drops         = [long]$cost.drops
        }
    }
    $stallRows = @($rows | Where-Object { $_.target -eq $LatencyTarget -and $_.dish -eq $StallDish -and $StallScenarios -contains $_.scenario })
    $worst = Get-WorstBy (Sort-MeasurementRows $stallRows) 'longestGapMs'
    if ($null -ne $worst) {
        $worstAcrossReps = $null
        foreach ($row in $stallRows) {
            $entry = $Spread[(Get-RowKey $row)]
            if ($null -eq $entry) { continue }
            if ($null -eq $worstAcrossReps -or $entry.worstlongestGapMs -gt $worstAcrossReps) { $worstAcrossReps = $entry.worstlongestGapMs }
        }
        $headline.connectPause = [ordered]@{
            longestGapMs      = [double]$worst.longestGapMs
            scenario          = $worst.scenario
            backend           = $worst.backend
            variant           = $worst.variant
            worstAcrossRepsMs = $worstAcrossReps
            plugMs            = [double]$worst.plugMs
            scenarios         = @($StallScenarios)
        }
    }
    $floors = @($Gate.runs | Where-Object { $null -ne $_.noiseFloorMs })
    $floor = Get-WorstBy (Sort-GateRuns $floors) 'noiseFloorMs'
    if ($null -ne $floor) {
        $headline.noiseFloor = [ordered]@{ ms = [double]$floor.noiseFloorMs; scenario = $floor.noiseFloorScenario; backend = $floor.backend; target = $floor.target }
    }
    $locks = @($Measurements.locks)
    $lockFree = @($locks | Where-Object { $LockFreeRoles -contains $_.role })
    $taken = [long]0
    foreach ($lock in $lockFree) { $taken += [long]$lock.acquisitions }
    $shortLocks = @($locks | Where-Object { $TransactionLocks -notcontains $_.lock })
    $transactionLocks = @($locks | Where-Object { $TransactionLocks -contains $_.lock })
    $hold = Get-WorstBy $shortLocks 'maxHoldUs'
    $transaction = Get-WorstBy $transactionLocks 'maxHoldUs'
    $wait = Get-WorstBy $locks 'maxWaitUs'
    $longestHold = $null
    if ($null -ne $hold) { $longestHold = [ordered]@{ us = [double]$hold.maxHoldUs; lock = $hold.lock; role = $hold.role; scenario = $hold.scenario; backend = $hold.backend } }
    $longestTransaction = $null
    if ($null -ne $transaction) { $longestTransaction = [ordered]@{ us = [double]$transaction.maxHoldUs; lock = $transaction.lock; role = $transaction.role; scenario = $transaction.scenario; backend = $transaction.backend } }
    $longestWait = $null
    if ($null -ne $wait) { $longestWait = [ordered]@{ us = [double]$wait.maxWaitUs; lock = $wait.lock; role = $wait.role; scenario = $wait.scenario; backend = $wait.backend } }
    $headline.locks = [ordered]@{
        inputPathAcquisitions = $taken
        rowsChecked           = [long]$LockGate.checked
        lawsPassed            = [bool]$LockGate.passed
        longestHold           = $longestHold
        longestTransaction    = $longestTransaction
        longestWait           = $longestWait
        plugMs                = [double]$settings.plugMs
        stallGatePassed       = [bool]$Gate.passed
    }
    return $headline
}

function Format-HeadlineMarkdown($Headline, $Settings) {
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add('## What a game sees')
    $lines.Add('')
    $lines.Add("Measured on the machine named above with a simulated $($Settings.submitUs) us driver submit and a $(Format-Compact ([double]$Settings.plugMs)) ms plug. Reference numbers from a dedicated machine are in tools/bench/README.md; a shared runner inflates the tails and the gaps, never the median.")
    $lines.Add('')
    $cost = $Headline.inputCost
    if ($null -ne $cost) {
        $repsText = ''
        if ($null -ne $cost.bestP999Us -and $cost.reps -gt 1) { $repsText = ", and the best of $($cost.reps) reps puts the 99.9th percentile at $(Format-Fixed $cost.bestP999Us 1) us" }
        $lines.Add("- **Added latency per input** ($($cost.backend) adapter path, $($cost.scenario) at $(Format-Count $cost.rateHz) Hz): $(Format-Fixed $cost.p50Us 1) us at the median and $(Format-Fixed $cost.p99Us 1) us at the 99th percentile$repsText. The median is $(Format-FrameShares ($cost.p50Us / 1000.0)).")
        $lines.Add("- **Input cadence:** $(Format-Count $cost.updatesPerSec) updates a second sustained for $(Format-Compact ([double]$Settings.secs)) s with $(Format-Count $cost.drops) drops.")
    } else {
        $lines.Add("- **Added latency per input:** no $HeadlineScenario row for dish A in the latency run, so it cannot be stated.")
    }
    $pause = $Headline.connectPause
    if ($null -ne $pause) {
        $acrossReps = ''
        if ($null -ne $pause.worstAcrossRepsMs) { $acrossReps = "; the worst single rep across those scenarios was $(Format-Fixed $pause.worstAcrossRepsMs 3) ms" }
        $lines.Add("- **While another player connects** ($($pause.scenarios[0]) to $($pause.scenarios[-1]), both adapters): the first player's input paused at most $(Format-Fixed $pause.longestGapMs 3) ms ($($pause.backend) $(Get-ScenarioLabel $pause)), $(Format-FrameShares $pause.longestGapMs)$acrossReps. A receive thread that waited out the plug would pause $(Format-Compact $pause.plugMs) ms, $(Format-FrameShare $pause.plugMs 60).")
    }
    $floor = $Headline.noiseFloor
    if ($null -ne $floor) {
        $lines.Add("- **Runner noise floor:** with no plug in play the runner itself paused the harness up to $(Format-Fixed $floor.ms 3) ms ($($floor.backend) $($floor.scenario)); a gap under that line is the machine, not Satellite.")
    }
    $lines.Add('')
    $lines.Add('## What we hold ourselves to')
    $lines.Add('')
    $locks = $Headline.locks
    $lawText = 'FAIL'
    if ($locks.lawsPassed) { $lawText = 'PASS' }
    $named = ($LockFreeRoles[0..($LockFreeRoles.Count - 2)] -join ', ') + ' and ' + $LockFreeRoles[-1]
    $lines.Add("- **Locks on the input path:** $(Format-Count $locks.inputPathAcquisitions). The $named threads acquired that many locks across $(Format-Count $locks.rowsChecked) lock rows (lock law gate $lawText).")
    if ($null -ne $locks.longestHold) {
        $h = $locks.longestHold
        $lines.Add("- **Longest hold of a short lock:** $(Format-Fixed $h.us 1) us, $($h.lock) by $($h.role) in $($h.backend) $($h.scenario).")
    }
    if ($null -ne $locks.longestTransaction) {
        $t = $locks.longestTransaction
        $lines.Add("- **Longest hold of a transaction lock, held across the plug by design:** $(Format-Fixed ($t.us / 1000.0) 1) ms, $($t.lock) by $($t.role) in $($t.backend) $($t.scenario), for a $(Format-Compact $locks.plugMs) ms plug.")
    }
    if ($null -ne $locks.longestWait) {
        $w = $locks.longestWait
        $lines.Add("- **Longest wait for a lock by any thread:** $(Format-Fixed $w.us 1) us, $($w.role) on $($w.lock) in $($w.backend) $($w.scenario).")
    }
    $stallText = 'FAIL'
    if ($locks.stallGatePassed) { $stallText = 'PASS' }
    $lines.Add("- **Plug stall gate:** $stallText.")
    return ($lines -join "`n")
}

function Format-RepsTable($Rows, $Spread) {
    $headers = @('scenario', 'mode', 'reps', 'p50 us best', 'p99 us best', 'p99.9 us best', 'max us best', 'longest gap ms worst', 'over 1 ms worst', 'drops worst')
    $table = New-Object 'System.Collections.Generic.List[string[]]'
    foreach ($row in (Sort-MeasurementRows $Rows)) {
        $entry = $Spread[(Get-RowKey $row)]
        if ($null -eq $entry) { continue }
        $table.Add([string[]]@(
            (Get-ScenarioLabel $row), $row.backend, (Format-Count $entry.reps),
            (Format-Fixed $entry.bestp50Us 1), (Format-Fixed $entry.bestp99Us 1), (Format-Fixed $entry.bestp999Us 1),
            (Format-Fixed $entry.bestmaxUs 1), (Format-Fixed $entry.worstlongestGapMs 3),
            (Format-Count $entry.worstoverOneMs), (Format-Count $entry.worstdrops)
        ))
    }
    if ($table.Count -eq 0) { return 'The bench files carry no per-rep rows, so the reported rep stands alone.' }
    return (New-MarkdownTable $headers $table 2)
}

function Format-MeasurementsMarkdown($Measurements, $Gate, $LockGate, $Trend, [double]$Ratio, [double]$MinDeltaUs, $Headline, $Spread) {
    $latencyRows = @($Measurements.latency | Where-Object { $_.target -eq $LatencyTarget -and $_.dish -eq $StallDish })
    $lockRows = @($Measurements.locks | Where-Object { $_.target -eq $LockStatsTarget })
    $lines = New-Object 'System.Collections.Generic.List[string]'
    $lines.Add("# Satellite $($Measurements.version) measurements")
    $lines.Add('')
    $lines.Add((Format-MachineLine $Measurements))
    $lines.Add('')
    $lines.Add("$(Format-Settings $Measurements.settings). A row is the rep whose dish A p99.9 is the median of its reps.")
    $lines.Add('')
    $lines.Add((Format-HeadlineMarkdown $Headline $Measurements.settings))
    $lines.Add('')
    $lines.Add((Format-GateMarkdown $Gate))
    $lines.Add('')
    $lines.Add((Format-LockGateMarkdown $LockGate))
    $lines.Add('')
    $lines.Add((Format-TrendMarkdown $Trend $Ratio $MinDeltaUs))
    $lines.Add('')
    $lines.Add("## Dish A latency ($LatencyTarget)")
    $lines.Add('')
    $lines.Add('Latencies are in microseconds and the longest gap is in milliseconds. "over 1 ms" counts frames slower than 1 ms and "drops" counts ring overflows.')
    $lines.Add('')
    $lines.Add((Format-LatencyTable $latencyRows $Trend))
    $lines.Add('')
    $lines.Add('## Across reps')
    $lines.Add('')
    $lines.Add('Per scenario, the best rep for each latency figure and the worst rep for each stall figure, so one noisy rep can neither hide a stall nor pass for the machine.')
    $lines.Add('')
    $lines.Add((Format-RepsTable $latencyRows $Spread))
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
    $reps = New-Object 'System.Collections.Generic.List[object]'
    $locks = New-Object 'System.Collections.Generic.List[object]'
    $sources = New-Object 'System.Collections.Generic.List[object]'
    foreach ($run in $Runs) {
        foreach ($row in $run.Rows) { $latency.Add($row) }
        foreach ($row in $run.Reps) { $reps.Add($row) }
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
        reps        = $reps.ToArray()
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
        [double]$GapPlugFraction,
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
    $gate = Get-StallGate $rows $GapPlugFraction (-not $AllowPartial)
    $lockGate = Get-LockLawGate $rows @($measurements.locks)
    $previous = Read-PreviousMeasurements $PreviousDir $Version
    $trend = Get-TrendComparison $rows $previous $TrendRatio $TrendMinDeltaUs
    $spread = Get-RepSpread @($measurements.reps)
    $headline = Get-Headline $measurements $gate $lockGate $spread
    $measurements['gate'] = $gate
    $measurements['lockGate'] = $lockGate
    $measurements['headline'] = $headline
    $measurements['trend'] = [ordered]@{
        previousVersion = $trend.previousVersion
        ratio           = $TrendRatio
        minDeltaUs      = $TrendMinDeltaUs
        warnings        = $trend.warnings
    }
    $markdown = Format-MeasurementsMarkdown $measurements $gate $lockGate $trend $TrendRatio $TrendMinDeltaUs $headline $spread
    New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null
    $outDir = (Resolve-Path -LiteralPath $OutputDir).Path
    $jsonPath = Join-Path $outDir "measurements-$Version.json"
    $markdownPath = Join-Path $outDir "measurements-$Version.md"
    Write-TextFile $jsonPath (($measurements | ConvertTo-Json -Depth 10) + "`n")
    Write-TextFile $markdownPath $markdown
    Add-StepSummary $markdown
    foreach ($warning in $trend.warnings) { Write-Annotation 'warning' 'Measurement trend' $warning }
    foreach ($failure in $gate.failures) { Write-Annotation 'error' 'Plug stall gate' $failure }
    foreach ($failure in $lockGate.failures) { Write-Annotation 'error' 'Lock law gate' $failure }
    $verdict = if ($gate.passed) { 'PASS' } else { 'FAIL' }
    $lockVerdict = if ($lockGate.passed) { 'PASS' } else { 'FAIL' }
    $limitText = if (@($gate.results).Count -gt 0) { "limit $(Format-Span @($gate.results | ForEach-Object { $_.limitMs }))" } else { 'nothing to judge' }
    $comparison = if ($trend.available) { "compared with $($trend.previousVersion), $($trend.warnings.Count) warning(s)" } else { 'no earlier release to compare with' }
    Write-Host "measurements: $($runs.Count) bench files, $($rows.Count) latency rows, $(@($measurements.locks).Count) lock rows"
    Write-Host "plug stall gate: $verdict ($limitText, $(Get-FractionPhrase $gate.plugFraction) of the plug)"
    foreach ($run in (Sort-GateRuns @($gate.runs))) {
        if ($null -eq $run.noiseFloorMs) { continue }
        Write-Host "runner noise floor, $(Get-RunTitle $run) $($run.backend): $(Format-Fixed $run.noiseFloorMs 3) ms ($($run.noiseFloorScenario))"
    }
    Write-Host "lock law gate: $lockVerdict ($(Format-Count $lockGate.checked) lock rows checked)"
    if ($null -ne $headline.inputCost) {
        Write-Host "headline: $(Format-Fixed $headline.inputCost.p50Us 1) us median, $(Format-Fixed $headline.inputCost.p99Us 1) us p99 per input on $($headline.inputCost.backend); $(Format-Count $headline.locks.inputPathAcquisitions) locks on the input path"
    }
    Write-Host "trend: $comparison"
    Write-Host "wrote $jsonPath"
    Write-Host "wrote $markdownPath"
    if ($gate.passed -and $lockGate.passed) { return 0 }
    return 1
}

if ($MyInvocation.InvocationName -ne '.') {
    $exitCode = 1
    $commitText = if ($Commit) { $Commit } else { [string]$env:GITHUB_SHA }
    try {
        $result = @(Invoke-MeasurementsReport -InputDir $InputDir -OutputDir $OutputDir -Version $Version `
            -PreviousDir $PreviousDir -Commit $commitText -GapPlugFraction $GapPlugFraction `
            -TrendRatio $TrendRatio -TrendMinDeltaUs $TrendMinDeltaUs -AllowPartial ([bool]$AllowPartial))
        $exitCode = [int]$result[-1]
    } catch {
        Write-Host "::error title=Measurements report::$($_.Exception.Message)"
        Write-Host "measurements-report failed: $($_.Exception.Message)"
    }
    exit $exitCode
}
