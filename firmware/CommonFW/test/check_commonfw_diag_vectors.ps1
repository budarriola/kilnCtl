# check_commonfw_diag_vectors.ps1 -- builds and runs CommonFW's own
# test_diag host test, the byte-exact codec test for SAFETY_CMD_DIAG
# (0x08, Frame B, kilnlink_diag.{c,h} / test/vectors/diag_vectors.json).
#
# WHY THIS EXISTS. firmware/CommonFW/CMakeLists.txt declares ~40 host-test
# executables (test_diag among them), but NOTHING in the standing suite ever
# configured or built that CMake project: KilnFW's and SaftyFW's own
# build_host_tests.ps1 each compile their own test sets and reach into
# CommonFW only for src/ + include/ (SaftyFW additionally builds exactly one
# CommonFW test file, test_fuzz_payloads.c). So the entire CommonFW/test/
# directory was orphaned coverage -- test_diag could be broken by a wire
# layout change and run_all_checks.ps1 would still report a clean sweep.
# Found 2026-09-20 during the review of the DIAG log_frames_dropped field
# (KILNLINK_PROTOCOL_VERSION 15 -> 16), which changed KILNLINK_DIAG_LEN
# 26 -> 30 and would have been graded entirely by tests nobody ran.
#
# WHY A TIMEOUT, NOT A BARE CALL. The failure mode this check most needs to
# catch -- a KILNLINK_DIAG_LEN that disagrees with the offsets
# kilnlink_diag_encode() actually writes -- overruns test_diag.c's
# `uint8_t buf[KILNLINK_DIAG_LEN]`. Under MSVC's Debug runtime checks that
# raises a Run-Time Check Failure #2 ("stack around the variable was
# corrupted"), which the debug CRT reports with _CRTDBG_MODE_WNDW: a MODAL
# DIALOG. In a non-interactive run nobody dismisses it, so the sabotaged
# build HANGS FOREVER instead of failing -- confirmed by hand, 2026-09-20,
# 20s+ with not one byte of stdout. A hang that a runner waits on is worse
# than a failure, so this check enforces its own wall-clock bound and grades
# a timeout as a FAIL, naming it. (test_diag.c's own main() additionally
# routes the debug CRT's reports to stderr so this particular case now aborts
# loudly and fast; the timeout stays as the general backstop, since that fix
# is per-file and the other ~39 CommonFW tests do not have it.)
#
# Contract (tools/run_all_checks.ps1): exit 0 PASS, exit 3 SKIP (cmake or a
# C compiler absent), anything else FAIL.
#
# Scratch is keyed on $PID and deleted in a finally block -- this runner
# dispatches checks in parallel and a fixed build directory races a second
# copy of this same check (see run_all_checks.ps1's
# FIXED-SHARED-SCRATCH-PATH RACE CLASS comment).

$ErrorActionPreference = "Stop"

$testDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$commonDir = Split-Path -Parent $testDir

# Build gate: gates the cmake --build call below only (opus review advisory
# d) -- same heavy-build class as check_commonfw_ctest.ps1's, just scoped to
# one target.
. (Join-Path $testDir "..\..\..\tools\build_gate.ps1")

if (-not (Get-Command cmake -ErrorAction SilentlyContinue)) {
    Write-Host "SKIP: no ``cmake`` on PATH -- cannot build CommonFW's host tests."
    exit 3
}

$buildDir = Join-Path $env:TEMP ("commonfw_diag_vectors_" + $PID)
$timeoutSec = 180

try {
    if (Test-Path $buildDir) { Remove-Item -Recurse -Force $buildDir }

    $cfg = & cmake -S $commonDir -B $buildDir 2>&1
    if ($LASTEXITCODE -ne 0) {
        # No usable C toolchain is a legitimate SKIP (a machine without MSVC
        # or gcc), but any other configure failure is a real defect. CMake
        # says so in its own words, so grade on that rather than guessing.
        if ($cfg -match "No CMAKE_C_COMPILER could be found|CMAKE_C_COMPILER not set") {
            Write-Host "SKIP: no C compiler available to CMake -- cannot build CommonFW's host tests."
            exit 3
        }
        $cfg | ForEach-Object { Write-Host $_ }
        throw "cmake configure of firmware/CommonFW failed (exit $LASTEXITCODE)."
    }

    $gate = Enter-KilnBuildGate -Label "commonfw_diag_vectors" -Lane light
    try {
        $bld = & cmake --build $buildDir --target test_diag --config Debug 2>&1
        $bldExit = $LASTEXITCODE
    } finally {
        Exit-KilnBuildGate -Gate $gate
    }
    if ($bldExit -ne 0) {
        $bld | ForEach-Object { Write-Host $_ }
        throw "cmake --build of CommonFW's test_diag failed (exit $bldExit)."
    }

    $exe = Get-ChildItem -Path $buildDir -Filter "test_diag.exe" -Recurse -File |
        Select-Object -First 1
    if (-not $exe) {
        $exe = Get-ChildItem -Path $buildDir -Filter "test_diag" -Recurse -File |
            Select-Object -First 1
    }
    if (-not $exe) {
        throw "CommonFW's test_diag built but no test_diag executable was found under $buildDir."
    }

    $outFile = Join-Path $buildDir "test_diag.stdout"
    $errFile = Join-Path $buildDir "test_diag.stderr"
    $proc = Start-Process -FilePath $exe.FullName -PassThru -NoNewWindow `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile
    # Touching .Handle caches the process handle NOW. Without it, a
    # -PassThru process object's .ExitCode reads $null after the process
    # ends (the handle is closed before we ask), and `$null -ne 0` would
    # then grade every run -- pass or fail -- as a FAIL. Confirmed by hand
    # against a healthy tree, 2026-09-20.
    $null = $proc.Handle
    if (-not $proc.WaitForExit($timeoutSec * 1000)) {
        try { $proc.Kill() } catch {}
        throw ("CommonFW's test_diag did not exit within $timeoutSec s -- graded a FAILURE, " +
               "not a flake. A hang here is the documented signature of a " +
               "KILNLINK_DIAG_LEN that disagrees with kilnlink_diag_encode()'s own " +
               "offsets: the overrun trips MSVC's Run-Time Check Failure #2, which the " +
               "debug CRT reports as a modal dialog nobody is there to dismiss.")
    }
    # The bounded WaitForExit(ms) overload can return before .ExitCode is
    # settled -- it came back $null here on a genuinely-failing run, and
    # `$null -ne 0` grading that as a FAIL was luck, not a check. The
    # parameterless overload settles it.
    $proc.WaitForExit()
    $exitCode = $proc.ExitCode
    if ($null -eq $exitCode) {
        throw "CommonFW's test_diag exited but reported no exit code -- refusing to grade it."
    }

    $stdout = if (Test-Path $outFile) { Get-Content $outFile -Raw } else { "" }
    $stderr = if (Test-Path $errFile) { Get-Content $errFile -Raw } else { "" }
    if ($stdout) { Write-Host $stdout.TrimEnd() }
    if ($stderr) { Write-Host $stderr.TrimEnd() }

    if ($exitCode -ne 0) {
        throw "CommonFW's test_diag reported failures (exit $exitCode)."
    }
    if ($stdout -notmatch "ALL PASS") {
        throw ("CommonFW's test_diag exited 0 but never printed ``ALL PASS`` -- refusing " +
               "to grade a silent run as a pass.")
    }

    Write-Host ("check_commonfw_diag_vectors: PASS -- CommonFW test_diag built and ran clean " +
                "(SAFETY_CMD_DIAG codec, KILNLINK_DIAG_LEN byte-exact vectors).")
    exit 0
}
finally {
    if (Test-Path $buildDir) {
        Remove-Item -Recurse -Force $buildDir -ErrorAction SilentlyContinue
    }
}
