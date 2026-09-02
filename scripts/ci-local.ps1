<#
.SYNOPSIS
    Run the gates Windows CI runs, in the same order, against the local tree.

.DESCRIPTION
    Mirrors .github/workflows/windows-ci.yml, so a green run here means a
    green run there: clang-format (pinned 22.1.4) over CI's exact file set,
    the action-pin lint from _security.yml, configure + build + ctest via the
    windows-mingw preset (MSYS2 MINGW64, the toolchain CI installs), the
    satellite.exe check, and the HIDMaestro helper publish.

        scripts/ci-local.ps1
        scripts/ci-local.ps1 -AllowMissing   # downgrade a missing tool to a notice

    Without -AllowMissing a gate whose tool is absent FAILS rather than
    printing a notice and continuing: a "green" run that silently skipped a
    gate is worse than no run at all. Exception: the .NET SDK, where CI's
    setup-dotnet has no local equivalent, skips with a warning.

    The MSVC A/B lane (windows-msvc-ci.yml) is not mirrored here; run
    scripts/build.ps1 -Msvc for that. The Linux/macOS equivalent is
    scripts/ci-local.sh.

.PARAMETER AllowMissing
    Downgrade a missing gated tool (clang-format, cmake, g++) to a notice
    instead of failing.
#>
[CmdletBinding()]
param(
    [switch]$AllowMissing
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent $PSScriptRoot
Set-Location $repoRoot

function Step([string]$Text) { Write-Output ''; Write-Output "=== $Text ===" }

# Returns $true when the caller should run the gate, $false when it was
# skipped by permission, and throws when a tool CI gates on is missing.
function Have([string]$Tool) {
    if (Get-Command $Tool -ErrorAction SilentlyContinue) { return $true }
    if ($AllowMissing) {
        # Write-Host, not Write-Output: this function's pipeline output IS its
        # return value, and a notice string would read as $true to the caller.
        Write-Host "::notice:: $Tool is not installed; CI gates this. Skipping (-AllowMissing)."
        return $false
    }
    throw "$Tool is not installed and CI gates it. Install it (scripts/install-deps.ps1), or re-run with -AllowMissing."
}

Step 'clang-format (check only)'
if (Have 'clang-format') {
    # CI pins 22.1.4; other versions disagree on braced-init lists, which is
    # why the pin exists.
    $want = '22.1.4'
    $got = ((& clang-format --version) | Select-String -Pattern '\d+\.\d+\.\d+').Matches[0].Value
    if ($got -ne $want) {
        Write-Output "::notice:: clang-format $got, CI pins $want; disagreements may be the version, not the code."
    }
    # The exact file set scripts/check-format.sh (and every CI lane) uses:
    # find src tests -type f ( -name '*.cpp' -o -name '*.h' -o -name '*.mm' )
    $files = Get-ChildItem -Path src, tests -Recurse -File -Include *.cpp, *.h, *.mm |
        ForEach-Object { $_.FullName }
    if (-not $files) { throw 'no source files found for the format gate' }
    # Batch to stay under the Windows command-line length limit.
    for ($i = 0; $i -lt $files.Count; $i += 40) {
        $chunk = $files[$i..([Math]::Min($i + 39, $files.Count - 1))]
        & clang-format --dry-run --Werror @chunk
        if ($LASTEXITCODE -ne 0) { throw 'clang-format check failed' }
    }
    Write-Output 'clang-format: OK'
}

Step 'Action pin lint (40-char SHA required)'
# PowerShell port of the awk in _security.yml's action-pin-lint job.
$pinFail = $false
Get-ChildItem -Path .github\workflows -Recurse -File -Include *.yml, *.yaml | ForEach-Object {
    $file = $_
    foreach ($raw in (Get-Content $file.FullName)) {
        if ($raw -match '^\s*#') { continue }
        $line = $raw -replace '\s+#.*$', ''
        if ($line -notmatch '^\s*-?\s*uses:\s+(\S.*)$') { continue }
        $ref = ($line -replace '^\s*-?\s*uses:\s+', '').TrimEnd()
        if ($ref -match '^\./') { continue }
        if ($ref -match '^docker://[^@]+@sha256:[0-9a-f]{64}$') { continue }
        if ($ref -notmatch '@[0-9a-f]{40}(\s|$)') {
            Write-Output "$($file.FullName): $ref"
            $script:pinFail = $true
        } elseif ($ref -match '@0{40}(\s|$)') {
            Write-Output "$($file.FullName): $ref (forbidden all-zero placeholder pin)"
            $script:pinFail = $true
        }
    }
}
if ($pinFail) { throw 'unpinned action reference' }
Write-Output 'action pins: OK'

# windows-ci.yml runs inside the MSYS2 MINGW64 shell; get the same toolchain
# by putting mingw64\bin first. Never ucrt64: different toolchain than CI's.
$mingwBin = 'C:\msys64\mingw64\bin'
if ((Test-Path $mingwBin) -and (-not (($env:PATH -split ';') -contains $mingwBin))) {
    $env:PATH = "$mingwBin;$env:PATH"
}

Step 'Configure (Release, preset windows-mingw)'
$toolchainReady = (Have 'cmake') -and (Have 'g++')
if ($toolchainReady) {
    $gxx = (Get-Command g++).Source
    if ($gxx -notlike "$mingwBin*") {
        Write-Output "::notice:: g++ resolves to $gxx, not $mingwBin; CI builds with MSYS2 MINGW64 gcc."
    }
    & cmake --preset windows-mingw
    if ($LASTEXITCODE -ne 0) { throw 'cmake configure failed' }

    Step 'Build'
    & cmake --build --preset windows-mingw -j $env:NUMBER_OF_PROCESSORS
    if ($LASTEXITCODE -ne 0) { throw 'cmake build failed' }

    Step 'Run tests'
    & ctest --preset windows-mingw
    if ($LASTEXITCODE -ne 0) { throw 'ctest failed' }

    Step 'Verify satellite.exe was produced'
    if (-not (Test-Path 'satellite.exe')) { throw 'satellite.exe not produced at repo root' }
    Get-Item satellite.exe | Format-List Name, Length, LastWriteTime
}

Step 'Build HIDMaestro helper (dotnet publish)'
if (Get-Command dotnet -ErrorAction SilentlyContinue) {
    # fetch-redist.ps1 stages the hash-pinned HIDMaestro SDK the helper
    # project references; CI runs it right before the publish.
    & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'fetch-redist.ps1')
    if ($LASTEXITCODE -ne 0) { throw 'fetch-redist.ps1 failed' }
    & dotnet publish helper/hidmaestro/satellite-hm-helper.csproj -c Release
    if ($LASTEXITCODE -ne 0) { throw 'dotnet publish failed' }
    if (-not (Test-Path 'helper/hidmaestro/bin/Release/net10.0-windows10.0.26100.0/win-x64/publish/satellite-hm-helper.exe')) {
        throw 'dotnet publish did not produce satellite-hm-helper.exe'
    }
} else {
    Write-Output '::warning:: no .NET SDK on PATH; skipping the helper publish CI runs (install-deps.ps1 installs SDK 10).'
}

Write-Output ''
Write-Output 'All local CI gates passed.'
