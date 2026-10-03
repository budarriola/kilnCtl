# run_sim_scenarios.ps1 -- WI-7 (docs/SCENARIO_SIMULATION.md sec 4.3,
# WI-7's own acceptance criterion).
#
# Runs kilnctl_sim_scenarios.exe at --of 1 (the canonical, single-process
# pass -- also what build_host_tests.ps1 runs automatically) and again
# sharded --of 4 (Start-Process per shard, PROCESS-level parallelism only --
# no threads, no OpenMP, nothing that could reorder a floating-point
# accumulation, per sec 4.3 point 5), then proves the two runs' (scenario,
# arm) result rows are BYTE-IDENTICAL after being brought to the same
# canonical order (sorted by scenario index then arm index, never by
# completion order, sec 4.3 point 4).
#
# HONEST NOTE ON WHAT THIS BUYS: the whole suite finishes in a couple of
# seconds of single-process compute (sec 4.3's own "Honest note on the value
# of parallelism here"). Sharding it saves no wall-clock time today. It is
# built because writing the runner to be shardable FORCES the no-shared-
# -state discipline that makes results reproducible at all -- that
# discipline is the deliverable this script proves, not a speedup.
#
# What is compared: only the per-(scenario,arm) DATA rows (lines starting
# "S<digits>_...\t") from stdout -- not the whole transcript. The --of 1 run
# also prints CLASSIFICATION/SEP_CHECK/acceptance-check lines the --of N
# shards do not (those need every scenario's result in one process, see
# sim_scenarios.c's `if (of > 1)` early return), and each shard's own banner
# names its own shard/of, so a raw whole-stdout diff would fail for reasons
# having nothing to do with determinism. The DATA ROWS are the thing sec
# 4.3's determinism requirement is actually about.
#
# Usage: powershell -File App\test\run_sim_scenarios.ps1 [-ExePath <path>] [-Shards 4]
param(
    [string]$ExePath = "",
    [int]$Shards = 4
)
$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$exe = if ($ExePath) { $ExePath } else { Join-Path $testDir "build\kilnctl_sim_scenarios.exe" }
if (-not (Test-Path $exe)) {
    throw "sim_scenarios exe not found at $exe -- run build_host_tests.ps1 first."
}

# Arm index lookup, same order as SIM_ARM_NAMES in sim_scenario_table.c --
# needed to sort rows by (scenario index, arm index) rather than completion
# order or raw text order.
$armOrder = @{
    "A_PID" = 0; "A_PID_AT" = 1; "A_FUZZY25" = 2; "A_FUZZY50" = 3;
    "A_FUZZY_AT" = 4; "A_STATIC_MATCHED" = 5
}

function Get-DataRows($rawLines) {
    $rows = @()
    foreach ($line in $rawLines) {
        if ($line -match '^S(\d+)_\S*\t(\S+)\t') {
            $scenNum = [int]$Matches[1]
            $armName = $Matches[2]
            $armIdx = if ($armOrder.ContainsKey($armName)) { $armOrder[$armName] } else { 99 }
            $rows += [PSCustomObject]@{ ScenNum = $scenNum; ArmIdx = $armIdx; Line = $line }
        }
    }
    return $rows | Sort-Object ScenNum, ArmIdx | ForEach-Object { $_.Line }
}

Write-Host "=== WI-7: --of 1 canonical run ==="
$of1Raw = & $exe --shard 0 --of 1
$of1ExitCode = $LASTEXITCODE
if ($of1ExitCode -ne 0) {
    throw "sim_scenarios --shard 0 --of 1 exited $of1ExitCode -- fix before running the determinism check."
}
$of1Rows = Get-DataRows $of1Raw
Write-Host "  --of 1: $($of1Rows.Count) data rows"

Write-Host "=== WI-7: --of $Shards sharded run (Start-Process, one per shard) ==="
$shardOutFiles = @()
$procs = @()
for ($i = 0; $i -lt $Shards; $i++) {
    $outFile = Join-Path $env:TEMP "sim_scenarios_shard_$i.out.txt"
    $errFile = Join-Path $env:TEMP "sim_scenarios_shard_$i.err.txt"
    $shardOutFiles += $outFile
    $p = Start-Process -FilePath $exe -ArgumentList @("--shard", "$i", "--of", "$Shards") `
            -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    $handle = $p.Handle # forces .NET to cache the process handle now -- without this, .ExitCode
                         # reads empty after WaitForExit() on some PowerShell/.NET versions (a
                         # known Start-Process -PassThru quirk, not this script's bug).
    $procs += $p
}
$procs | ForEach-Object { $_.WaitForExit() }
$shardExitCodes = $procs | ForEach-Object { $_.ExitCode }
for ($i = 0; $i -lt $Shards; $i++) {
    if ($shardExitCodes[$i] -ne 0) {
        Write-Host "shard $i exited $($shardExitCodes[$i]):"
        Get-Content $shardOutFiles[$i]
        throw "sim_scenarios shard $i/$Shards failed -- see output above."
    }
}

$ofNRaw = @()
foreach ($f in $shardOutFiles) { $ofNRaw += Get-Content $f }
$ofNRows = Get-DataRows $ofNRaw
Write-Host "  --of $Shards (aggregated): $($ofNRows.Count) data rows"

Write-Host "=== WI-7: byte-identical comparison ==="
if ($of1Rows.Count -ne $ofNRows.Count) {
    Write-Host "FAIL: row count differs -- of1=$($of1Rows.Count) ofN=$($ofNRows.Count)"
    exit 1
}
$diffCount = 0
for ($i = 0; $i -lt $of1Rows.Count; $i++) {
    if ($of1Rows[$i] -cne $ofNRows[$i]) {
        $diffCount++
        if ($diffCount -le 10) {
            Write-Host "DIFF at row $i :"
            Write-Host "  of1: $($of1Rows[$i])"
            Write-Host "  ofN: $($ofNRows[$i])"
        }
    }
}
if ($diffCount -gt 0) {
    Write-Host "FAIL: $diffCount of $($of1Rows.Count) rows differ between --of 1 and --of $Shards -- this is a real"
    Write-Host "finding about hidden shared state, not a tolerance to raise. Do not patch over it."
    exit 1
}

Write-Host "PASS: --of 1 and --of $Shards data rows are byte-identical ($($of1Rows.Count) rows compared)."
exit 0
