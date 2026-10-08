# check_commonfw_ctest.ps1 -- runs the WHOLE CommonFW host-test suite via
# ctest, not just test_diag.
#
# WHY THIS EXISTS. check_commonfw_diag_vectors.ps1 (sibling in this
# directory) closed the gap for test_diag specifically, but its own header
# says why it stops there: firmware/CommonFW/CMakeLists.txt declares roughly
# three dozen add_test() targets (every kilnlink_*.c wire codec's own test,
# plus test_frame/test_fuzz/test_fuzz_payloads and the two benchproto tests),
# and nothing in the standing suite ever ran `ctest` itself -- only test_diag
# had a dedicated check. A change that broke, say, test_set_config.c or
# test_benchproto_link.c's codec could land with every discovered
# check_*.ps1 green, the same "orphaned CommonFW coverage" class
# check_commonfw_diag_vectors.ps1's header documents, just for every target
# except the one it happens to cover.
#
# This check is a SEPARATE file, not a rewrite of
# check_commonfw_diag_vectors.ps1, because that file's own header explains a
# real, test_diag-specific hazard (a stack-buffer overrun under MSVC's debug
# CRT throwing a modal dialog nobody is there to dismiss) that earned it its
# own bounded-timeout Start-Process dance around exactly one executable.
# Folding the rest of the suite into that file would either dilute that
# focused fix or duplicate it needlessly for tests that don't share the
# hazard. Instead this check configures and builds the same top-level
# CommonFW CMake project fresh, in its own isolated directory, and lets
# `ctest` run and time every declared test -- ctest's own --timeout flag is
# the general-purpose version of the same "a hang must not be a runner
# stall" protection, applied to all of them at once.
#
# ISOLATED, $PID-KEYED BUILD DIR -- the fixed-shared-scratch-path race class
# run_all_checks.ps1's own header warns about. A fresh configure+build here
# every run (never a shared/incremental build/ directory) also means a
# CMakeLists.txt edit that silently drops or misconfigures a target is
# caught the same way a developer's own clean-clone build would catch it.
#
# Contract (tools/run_all_checks.ps1): exit 0 PASS, exit 3 SKIP (cmake or a
# C compiler absent), anything else FAIL.

$ErrorActionPreference = "Stop"

$testDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$commonDir = Split-Path -Parent $testDir

# Build gate: HEAVY lane. The build is parallel (Ninja, -j), so it is no longer
# the single serial compile the light lane assumes. The MSVC environment is
# imported ONCE, before the gate (Import-KilnVcvarsEnv), and the slot is held
# only around the --build call, not the configure or the ctest run.
. (Join-Path $testDir "..\..\..\tools\build_gate.ps1")

$sw = [System.Diagnostics.Stopwatch]::StartNew()

# PROFILE (2026-10-08, this machine): the Visual Studio generator took ~68 s to
# configure and ~1206 s to build serially (msbuild spawns several processes per
# target and Defender scans each one); ctest itself is ~79 s serial. Ninja + cl
# with a parallel build and `ctest -j` removes most of it. The build dir stays
# fresh and $PID-keyed on purpose: a dropped or misconfigured target in
# CMakeLists.txt must still be caught the way a clean clone would catch it.
$jobs = [Math]::Max(2, [Math]::Min(8, [Environment]::ProcessorCount))

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "SKIP: no ``cmake`` on PATH -- cannot build CommonFW's host tests."
    exit 3
}
if (-not (Get-Command ctest -ErrorAction SilentlyContinue)) {
    Write-Host "SKIP: no ``ctest`` on PATH -- cannot run CommonFW's host tests."
    exit 3
}

$buildDir = Join-Path $env:TEMP ("commonfw_ctest_" + $PID)

try {
    if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }

    $cfgArgs = @("-S", $commonDir, "-B", $buildDir)
    $vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    if ((Get-Command ninja -ErrorAction SilentlyContinue) -and (Test-Path $vcvars)) {
        Import-KilnVcvarsEnv -Vcvars $vcvars
        $cfgArgs += @("-G", "Ninja", "-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_C_COMPILER=cl")
    }
    $cfg = & cmake @cfgArgs 2>&1
    if ($LASTEXITCODE -ne 0) {
        # No usable C toolchain is a legitimate SKIP (a machine without MSVC
        # or gcc), but any other configure failure is a real defect -- same
        # split check_commonfw_diag_vectors.ps1 uses.
        if ($cfg -match "No CMAKE_C_COMPILER could be found|CMAKE_C_COMPILER not set") {
            Write-Host "SKIP: no C compiler available to CMake -- cannot build CommonFW's host tests."
            exit 3
        }
        $cfg | ForEach-Object { Write-Host $_ }
        throw "cmake configure of firmware/CommonFW failed (exit $LASTEXITCODE)."
    }

    # No -target: build everything CMakeLists.txt declares (both libraries
    # plus every host-test executable), not just one.
    $gate = Enter-KilnBuildGate -Label "commonfw_ctest" -Lane heavy
    try {
        $bld = & cmake --build $buildDir --config Debug --parallel $jobs 2>&1
        $bldExit = $LASTEXITCODE
    } finally {
        Exit-KilnBuildGate -Gate $gate
    }
    if ($bldExit -ne 0) {
        $bld | ForEach-Object { Write-Host $_ }
        throw "cmake --build of firmware/CommonFW failed (exit $bldExit)."
    }

    # --timeout is ctest's own per-test wall-clock bound -- the general form
    # of the bounded-wait check_commonfw_diag_vectors.ps1 hand-rolled around
    # test_diag.exe alone (a debug-CRT stack-check failure pops a modal
    # dialog nobody is there to dismiss, which otherwise hangs the runner
    # forever instead of failing). 60s is generously above every one of
    # these host tests' observed runtime (each is a small, self-contained
    # in-memory codec test with no I/O or sleeps).
    # $ErrorActionPreference = "Continue" for exactly this call: ctest writes
    # to stderr when a test fails, and under "Stop" (this script's default),
    # capturing a native command's stderr via 2>&1 in-process promotes that
    # into a terminating NativeCommandError even though ctest's own exit code
    # is the real, correct signal -- confirmed by hand while writing this
    # check's own negative test (a genuinely failing test_frame produced
    # exactly this false "NativeCommandError" instead of a clean FAIL). Same
    # class of gotcha check_00_kilnfw_host_tests.ps1's header documents for
    # Start-Process; here the fix is simpler because ctest's own output
    # doesn't need file redirection, just a non-terminating error action.
    $prevEap = $ErrorActionPreference
    $ErrorActionPreference = "Continue"
    $ctestOut = & ctest --test-dir $buildDir --output-on-failure --timeout 60 -C Debug -j $jobs 2>&1
    $ctestExit = $LASTEXITCODE
    $ErrorActionPreference = $prevEap
    $ctestOut | ForEach-Object { Write-Host $_ }

    $sw.Stop()
    $elapsedS = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    Write-Host ""
    Write-Host "check_commonfw_ctest.ps1: wall time ${elapsedS}s"

    $summaryLine = ($ctestOut -split "`r?`n" | Where-Object { $_ -match '^\d+% tests passed' } | Select-Object -First 1)
    if ($summaryLine) {
        Write-Host "check_commonfw_ctest.ps1: $($summaryLine.Trim())"
    } else {
        Write-Host "check_commonfw_ctest.ps1: WARNING -- no 'N% tests passed' summary line found in ctest output"
    }

    if ($ctestExit -ne 0) {
        throw "ctest reported failures in firmware/CommonFW (exit $ctestExit) -- see --output-on-failure detail above."
    }

    Write-Host ("check_commonfw_ctest.ps1: PASS -- every add_test() target in " +
                "firmware/CommonFW/CMakeLists.txt built and passed under ctest.")
    exit 0
}
finally {
    if (Test-Path $buildDir) {
        Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    }
}
