# check_sim_iter_tune_bars.ps1 -- enforces sim_iter_tune.exe's A1/A2/A5/A6
# statistical acceptance bars (ITER_TUNE_REDESIGN_PLAN.md sec 6/7).
#
# Until 2026-09-10 (docs/audits/firing_score_entry_ema_review_2026-09-10.md)
# sim_iter_tune.c's main() printed PASS/FAIL per bar but always
# `return 0`, and build_host_tests.ps1 built it as a "data-generating
# harness, not run automatically" -- so no check_*.ps1 or CI path ever ran
# it, and every bar could silently fail forever. That is exactly how the A1
# false-accept rate went from 0.00% to 3.64% after d63a5591's coupling-model
# change and was only caught by a manual re-run. sim_iter_tune.c's main()
# now returns 1 if any of A1/A2/A5/A6 fails; this script builds it, runs it
# at the n=220 sample size the audit's numbers are quoted at (660 A1/A2
# comparisons), and fails on a non-zero exit -- naming which bar(s) failed
# from the captured stdout so a red run does not require re-reading the
# whole log by hand.
#
# This does NOT replace sim_iter_tune.exe's usefulness as an ad hoc
# data-generating harness for exploratory runs (any sample size, any
# argv[1]) -- run the .exe directly for that; this check only pins the one
# canonical n=220 configuration the audit's figures reference.

$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..\..\..")).Path
$testDir = $PSScriptRoot
$driversDir = Join-Path $testDir "..\drivers"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    Write-Host "FAIL: vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved."
    exit 1
}

$outDir = Join-Path $testDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$rsp = Join-Path $outDir "host_tests_common_flags.rsp"
if (-not (Test-Path $rsp)) {
    Write-Host "FAIL: $rsp not found -- run build_host_tests.ps1 at least once first (it writes the shared response file this check reuses)."
    exit 1
}

. (Join-Path $repoRoot "tools\build_lock.ps1")
$lock = Enter-BuildLock -Name "kilnfw_sim_iter_tune_bars"
try {
    $exe = Join-Path $outDir "kilnctl_sim_iter_tune_bars_check.exe"
    if (Test-Path $exe) { Remove-Item -Force $exe }

    $src = @(
        (Join-Path $testDir "sim_iter_tune.c"),
        (Join-Path $testDir "sim_plant.c"),
        (Join-Path $driversDir "control\pid.c"),
        (Join-Path $driversDir "control\heater_output.c"),
        (Join-Path $driversDir "control\zone_coupling_solve.c"),
        (Join-Path $driversDir "control\firing_score.c"),
        (Join-Path $driversDir "control\firing_compare.c"),
        (Join-Path $driversDir "control\iter_tune.c")
    )
    $srcQuoted = ($src | ForEach-Object { "`"$_`"" }) -join " "

    $bat = Join-Path $outDir "_check_sim_iter_tune_build.bat"
    @"
@echo off
call "$vcvars" x64 >nul
cl @"$rsp" /std:c11 /Fo:"$outDir\\" /Fe:"$exe" $srcQuoted
echo BUILD_EXIT=%ERRORLEVEL%
"@ | Set-Content -Encoding ascii -LiteralPath $bat

    $buildOut = cmd.exe /c "`"$bat`""
    $buildOut | ForEach-Object { Write-Host $_ }
    if (-not (Test-Path $exe)) {
        Write-Host "FAIL: sim_iter_tune.exe did not build -- see compiler output above."
        exit 1
    }

    $runOut = & $exe 220 2>&1
    $runExit = $LASTEXITCODE
    $runOut | ForEach-Object { Write-Host $_ }

    Remove-Item -Force -ErrorAction SilentlyContinue $bat
    Remove-Item -Force -ErrorAction SilentlyContinue $exe

    if ($runExit -ne 0) {
        $barLines = $runOut | Where-Object { $_ -match "bar:.*-> FAIL" }
        Write-Host "FAIL: sim_iter_tune.exe (n=220) exited $runExit -- at least one acceptance bar failed:"
        foreach ($b in $barLines) { Write-Host "  $b" }
        exit 1
    }

    Write-Host "PASS: sim_iter_tune.exe (n=220) -- A1/A2/A5/A6 all clear."
    exit 0
} finally {
    Exit-BuildLock -Lock $lock
}
