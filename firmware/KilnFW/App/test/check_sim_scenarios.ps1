# check_sim_scenarios.ps1 -- wires sim_scenarios.exe's own PASS/FAIL/SEP-pin/
# WI-4 acceptance-check assertions, AND the WI-7 sharded-determinism proof
# (docs/SCENARIO_SIMULATION_PLAN.md sec 4.3/5.3/8), into run_all_checks.ps1's
# check_*.ps1 glob.
#
# Until this file, sim_scenarios.exe's canonical (--of 1) run was gated only
# through build_host_tests.ps1's Invoke-HostTestExe (itself invoked by the
# SEPARATE tools/regression_suite.py gate, not by run_all_checks.ps1), and
# the WI-7 sharded-determinism proof (App/test/run_sim_scenarios.ps1) was
# NEVER invoked by any automated script anywhere in this tree -- confirmed by
# grep across run_all_checks.ps1, regression_suite.py, and every other
# check_*.ps1/.py. A regression in either the canonical run's own assertions
# or in the (scenario, arm) determinism guarantee could land with the
# standard 104-check baseline still fully green.
#
# SUPERSEDED IN PART, 2026-09-21: check_00_kilnfw_host_tests.ps1 (same
# directory) now builds and runs the WHOLE build_host_tests.ps1 suite --
# sim_scenarios.exe included -- as its own discovered check_*.ps1 (in
# run_all_checks.ps1's throttled phase 2, NOT phase 1 -- the check_00_
# prefix there is for sort order, not phase membership), so a broken
# BUILD of sim_scenarios.exe (or any of its 55 siblings) can no longer land
# with every discovered check_*.ps1 green the way this file's header
# originally described. This file's OWN, separate value is unchanged and
# still not covered by that check: it exercises sim_scenarios.exe with the
# additional WI-7 sharded-determinism (--of 4) argument
# build_host_tests.ps1's own canonical (--of 1) invocation never passes, and
# it wires the standalone WI-7 proof script (run_sim_scenarios.ps1) that
# build_host_tests.ps1 does not invoke at all. Keep both checks.
#
# Follows check_sim_iter_tune_bars.ps1's exact three-status convention (see
# that file's long header for the full rationale, repeated only in brief
# here): exit 0 PASS, exit 1 FAIL (a real regression against this checkout's
# own source), exit 3 SKIP (MSVC toolchain genuinely not found/invocable --
# an environment prerequisite, never counted as a clean pass).
#
# Self-contained build: own object dir (build_sim_scenarios_check_obj,
# distinct from build_host_tests.ps1's shared App/test/build and from
# check_sim_iter_tune_bars.ps1's own build_sim_iter_tune_bars_obj -- several
# agents/checks can build concurrently in this tree without clobbering each
# other's .obj files) and its own inline /I list (the same set
# build_host_tests.ps1's $hostTestsRsp and check_sim_iter_tune_bars.ps1 both
# use), so this check has no dependency on build_host_tests.ps1 having run
# first.
#
# Budget (plan sec 8): under 60s wall-clock including the build plus both the
# canonical (--of 1) and sharded (--of 4) runs. Reports the measured elapsed
# time; does not hard-fail on the budget itself since the acceptance
# criterion below is that a real regression (bar failure, SEP-pin mismatch,
# or a determinism divergence) is the only thing that turns this red.

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
$testDir = $PSScriptRoot
$driversDir = Join-Path $testDir "..\drivers"

$sw = [System.Diagnostics.Stopwatch]::StartNew()

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    Write-Host "SKIP: MSVC toolchain not found -- vcvarsall.bat missing at $vcvars."
    Write-Host "      This is a prerequisite absence (documented SKIP case in run_all_checks.ps1's"
    Write-Host "      own header: 'no toolchain installed'), not a sim_scenarios result."
    exit 3
}

$outDir = Join-Path $testDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$objDir = Join-Path $testDir "build_sim_scenarios_check_obj"
New-Item -ItemType Directory -Force -Path $objDir | Out-Null

. (Join-Path $repoRoot "tools\build_lock.ps1")
$lock = Enter-BuildLock -Name "kilnfw_sim_scenarios_check"
try {
    $exe = Join-Path $outDir "kilnctl_sim_scenarios_check.exe"
    if (Test-Path $exe) { Remove-Item -Force $exe }

    $src = @(
        (Join-Path $testDir "sim_scenarios.c"),
        (Join-Path $testDir "sim_scenario_table.c"),
        (Join-Path $testDir "sim_plant.c"),
        (Join-Path $testDir "sim_high_temp.c"),
        (Join-Path $testDir "sim_mistune.c"),
        (Join-Path $driversDir "control\pid.c"),
        (Join-Path $driversDir "control\pid_fuzzy.c"),
        (Join-Path $driversDir "control\pid_autotune.c"),
        (Join-Path $driversDir "control\firing_score.c")
    )
    $srcQuoted = ($src | ForEach-Object { "`"$_`"" }) -join " "

    # Same /I set build_host_tests.ps1's shared host_tests_common_flags.rsp
    # and check_sim_iter_tune_bars.ps1 both use -- inlined here so this check
    # has no dependency on either having run first.
    $includeDirs = @(
        (Join-Path $testDir "stubs"),
        (Join-Path $testDir "..\..\..\CommonFW\include"),
        (Join-Path $testDir "..\drivers"),
        (Join-Path $testDir "..\drivers\bridge"),
        (Join-Path $testDir "..\drivers\common"),
        (Join-Path $testDir "..\drivers\control"),
        (Join-Path $testDir "..\drivers\http"),
        (Join-Path $testDir "..\drivers\hw"),
        (Join-Path $testDir "..\drivers\net"),
        (Join-Path $testDir "..\drivers\owners"),
        (Join-Path $testDir "..\drivers\persist"),
        (Join-Path $testDir "..\drivers\safety"),
        (Join-Path $testDir "..\drivers\sim"),
        (Join-Path $testDir "..\drivers\ui"),
        (Join-Path $testDir "..\..\..\hwAbstraction\esp\spi"),
        (Join-Path $testDir "..\..\..\hwAbstraction\esp\i2c"),
        (Join-Path $testDir "..\..\..\hwAbstraction\esp\uart"),
        (Join-Path $testDir "..\..\..\hwAbstraction\interface"),
        (Join-Path $testDir "..\..\..\hwAbstraction\host"),
        (Join-Path $testDir "..\..\..\hwAbstraction\esp\common")
    )
    $includeArgs = ($includeDirs | ForEach-Object { "/I`"$_`"" }) -join " "

    $bat = Join-Path $outDir "_check_sim_scenarios_build.bat"
    @"
@echo off
call "$vcvars" x64 >nul
cl /nologo /W3 /EHsc /std:c11 $includeArgs /Fo:"$objDir\\" /Fe:"$exe" $srcQuoted
echo BUILD_EXIT=%ERRORLEVEL%
"@ | Set-Content -Encoding ascii -LiteralPath $bat

    $buildOut = cmd.exe /c "`"$bat`""
    $buildOut | ForEach-Object { Write-Host $_ }
    Remove-Item -Force -ErrorAction SilentlyContinue $bat

    if (-not (Test-Path $exe)) {
        $hasCompilerError = $buildOut | Where-Object { $_ -match 'error C\d{4}' -or $_ -match 'error LNK\d+' -or $_ -match 'fatal error' }
        if ($hasCompilerError) {
            Write-Host "FAIL: sim_scenarios.exe did not build -- compiler/linker reported real diagnostics above."
            exit 1
        }
        Write-Host "SKIP: sim_scenarios.exe did not build and the compiler reported no diagnostics against"
        Write-Host "      this checkout's own source -- the toolchain itself could not be invoked in this"
        Write-Host "      environment. Not a sim_scenarios result."
        exit 3
    }

    # ---- Canonical run: --shard 0 --of 1. Exercises every per-scenario
    # SEP-pin check plus the WI-4 acceptance checks (S0 all-arms-agree,
    # S1-vs-S3 not-a-confound) that only run when the whole table is
    # evaluated in one process (sim_scenarios.c's `if (of > 1)` early return).
    Write-Host "=== canonical run: --shard 0 --of 1 ==="
    $of1Raw = & $exe --shard 0 --of 1 2>&1
    $of1Exit = $LASTEXITCODE
    $of1Raw | ForEach-Object { Write-Host $_ }
    if ($of1Exit -ne 0) {
        Write-Host "FAIL: sim_scenarios.exe --shard 0 --of 1 exited $of1Exit -- a SEP-pin check, WI-4"
        Write-Host "      acceptance check, or structural invariant failed. See FAIL/MISMATCH lines above."
        exit 1
    }

    # ---- WI-7 determinism proof: --of 1 vs --of 4, byte-identical (scenario,
    # arm) data rows after canonical sort. Adapted from run_sim_scenarios.ps1
    # (the standalone script this check absorbs into an automated gate).
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
    $of1Rows = Get-DataRows $of1Raw
    Write-Host "  --of 1: $($of1Rows.Count) data rows"

    Write-Host "=== sharded run: --of 4 (Start-Process, one per shard) ==="
    $shards = 4
    $shardOutFiles = @()
    $procs = @()
    for ($i = 0; $i -lt $shards; $i++) {
        $outFile = Join-Path $env:TEMP "check_sim_scenarios_shard_$i.out.txt"
        $errFile = Join-Path $env:TEMP "check_sim_scenarios_shard_$i.err.txt"
        $shardOutFiles += $outFile
        $p = Start-Process -FilePath $exe -ArgumentList @("--shard", "$i", "--of", "$shards") `
                -NoNewWindow -PassThru -RedirectStandardOutput $outFile -RedirectStandardError $errFile
        $handle = $p.Handle
        $procs += $p
    }
    $procs | ForEach-Object { $_.WaitForExit() }
    $shardExitCodes = $procs | ForEach-Object { $_.ExitCode }
    for ($i = 0; $i -lt $shards; $i++) {
        if ($shardExitCodes[$i] -ne 0) {
            Write-Host "FAIL: sim_scenarios shard $i/$shards exited $($shardExitCodes[$i]):"
            Get-Content $shardOutFiles[$i] | ForEach-Object { Write-Host "  $_" }
            exit 1
        }
    }
    $ofNRaw = @()
    foreach ($f in $shardOutFiles) { $ofNRaw += Get-Content $f }
    $ofNRows = Get-DataRows $ofNRaw
    Write-Host "  --of $shards (aggregated): $($ofNRows.Count) data rows"

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
        Write-Host "FAIL: $diffCount of $($of1Rows.Count) rows differ between --of 1 and --of $shards --"
        Write-Host "      this is a real finding about hidden shared state, not a tolerance to raise."
        exit 1
    }

    Remove-Item -Force -ErrorAction SilentlyContinue $exe
    $sw.Stop()
    Write-Host "PASS: sim_scenarios.exe canonical run clear (SEP-pins + WI-4 acceptance checks); --of 1"
    Write-Host "      and --of $shards data rows byte-identical ($($of1Rows.Count) rows compared). Elapsed:"
    Write-Host "      $($sw.Elapsed.TotalSeconds.ToString('F1'))s (plan sec 8 budget: 60s)."
    exit 0
} finally {
    Exit-BuildLock -Lock $lock
}
