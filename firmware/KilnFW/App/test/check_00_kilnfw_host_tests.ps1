# check_00_kilnfw_host_tests.ps1 -- closes the standing-suite gap
# CLAUDE.md's "0edfb313 broke the ota_http executable's build" finding
# describes: no check_*.ps1 discovered by tools/run_all_checks.ps1 built and
# ran the KilnFW host-test suite (build_host_tests.ps1's N executables). That
# gate previously lived ONLY in tools/regression_suite.py's
# _gate_kilnfw_host_tests(), which an agent's `-Only`-filtered run_all_checks.ps1
# invocation never touches -- so a build break in one of the N executables
# (a stub header missing an API another file started calling) could land with
# every discovered check_*.ps1 green. check_sim_scenarios.ps1's own header
# already documented this same gap for sim_scenarios.exe specifically; this
# check covers the whole suite (sim_scenarios.exe included, since it is one of
# the executables build_host_tests.ps1 itself builds and runs).
#
# NAMED check_00_* BUT LEFT IN PHASE 2 (throttled, -MaxParallel default 8):
# measured wall time on this 24-core bench machine was 137.4s for a full
# from-clean build + run of all N executables, UNDER the ~150s guideline in
# this check's own task brief for joining the three existing
# check_00_*_target_build.ps1 scripts that run unthrottled/concurrently in
# phase 1 -- so unlike those three, this one stays in run_all_checks.ps1's
# ordinary $restChecks pool and shares phase 2's throttle with everything
# else. The check_00_ prefix is kept anyway for discoverability/sort-order
# next to the target-build checks it's closing a coverage gap for, not as a
# phase-1 signal -- run_all_checks.ps1 only grants phase-1 membership via an
# exact filename match in its $buildChecks selector, and this file's name is
# deliberately NOT in that list (only in the three checks it doesn't join).
#
# -Fast still SKIPs this check, same reasoning as the three target builds
# even though it isn't scheduled alongside them: it's in the same
# multi-minute, full-rebuild cost class, and a caller who just ran
# build_host_tests.ps1 (or an equivalent) itself should get to skip redoing
# it here too. This is wired as its own explicit -notmatch clause in
# run_all_checks.ps1's -Fast filter, independent of phase-1 membership.
#
# ISOLATED BUILD DIR (the fixed-shared-scratch-path race class this runner's
# own header warns every new check about): build_host_tests.ps1 accepts
# -OutDir precisely so two concurrent copies (this check running alongside a
# developer's own manual build, or two agents in two worktrees) never collide
# on the same .obj/.exe paths. This check always passes a $PID-keyed -OutDir
# under $env:TEMP, never the shared firmware/KilnFW/App/test/build directory,
# and deletes it in a finally block.
#
# N IS DISCOVERED, NOT HARDCODED, IN THIS CHECK: build_host_tests.ps1 already
# maintains its own internal $totalExpected and prints "Built: X/Y
# executables" as its last summary line before exiting 0 or 1. This check
# greps that line back out of the captured output for both counts rather than
# hardcoding Y here a second time -- a change to build_host_tests.ps1's own
# executable count (the usual case: a new test file added) needs no matching
# edit here. The check still PASSES only when build_host_tests.ps1 itself
# exited 0 (which already requires X == Y, i.e. every expected executable was
# both built and run and none of the BUILD FAILURES / RUN FAILURES lists is
# non-empty) -- the grep is for a legible failure message, not the actual
# pass/fail decision, which always defers to build_host_tests.ps1's own exit
# code.
#
# SKIP (exit 3) posture, same contract check_sim_scenarios.ps1 uses: only
# when the MSVC toolchain itself is not installed on this machine
# (vcvarsall.bat missing) -- never for a build or run failure, which is
# always a real FAIL.
#
# check_sim_scenarios.ps1's header note about sim_scenarios.exe being
# ungated by any check_*.ps1 is now STALE as of this file: sim_scenarios.exe
# is one of the N executables build_host_tests.ps1 builds and runs, and this
# check gates that whole set. See the update made to that file's header in
# the same commit as this one.

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot

$sw = [System.Diagnostics.Stopwatch]::StartNew()

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    Write-Host "SKIP: MSVC toolchain not found -- vcvarsall.bat missing at $vcvars."
    Write-Host "      This is a prerequisite absence (documented SKIP case in run_all_checks.ps1's"
    Write-Host "      own header: 'no toolchain installed'), not a host-test result."
    exit 3
}

# $PID-keyed, under $env:TEMP -- never the shared firmware/KilnFW/App/test/build
# directory, so a concurrent manual build or a sibling agent's own run of
# build_host_tests.ps1 can never collide on the same .obj/.exe paths (the
# fixed-shared-scratch-path race class run_all_checks.ps1's header warns
# about).
$outDir = Join-Path $env:TEMP "kilnfw_host_tests_check_$PID"

try {
    if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $buildScript = Join-Path $testDir "build_host_tests.ps1"
    # Redirected to FILES, not `2>&1 | Out-String`: under
    # $ErrorActionPreference = "Stop", piping a native command's stderr
    # in-process promotes any stderr line (vcvarsall.bat's own vswhere.exe
    # chatter included) into a terminating NativeCommandError even when the
    # child's real exit code is 0 -- the same gotcha this repo's tooling
    # notes document elsewhere. Start-Process with file redirection sidesteps
    # it entirely, same technique run_all_checks.ps1 itself uses for every
    # check it launches.
    $stdoutFile = Join-Path $outDir "build_host_tests.stdout.txt"
    $stderrFile = Join-Path $outDir "build_host_tests.stderr.txt"
    $proc = Start-Process -FilePath "powershell" `
        -ArgumentList @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $buildScript, "-OutDir", $outDir) `
        -RedirectStandardOutput $stdoutFile -RedirectStandardError $stderrFile -PassThru -NoNewWindow
    $null = $proc.Handle
    $proc.WaitForExit()
    $exitCode = $proc.ExitCode
    $outText = ""
    foreach ($f in @($stdoutFile, $stderrFile)) {
        if (Test-Path $f) { $outText += (Get-Content -Raw -ErrorAction SilentlyContinue $f) }
    }

    Write-Host $outText

    $sw.Stop()
    $elapsedS = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    Write-Host "check_00_kilnfw_host_tests.ps1: wall time ${elapsedS}s"

    $summaryLine = ($outText -split "`r?`n" | Where-Object { $_ -match '^Built:\s+\d+/\d+ executables' } | Select-Object -First 1)
    if ($summaryLine) {
        Write-Host "check_00_kilnfw_host_tests.ps1: $($summaryLine.Trim())"
    } else {
        Write-Host "check_00_kilnfw_host_tests.ps1: WARNING -- no 'Built: X/Y executables' summary line found in output"
    }

    if ($exitCode -ne 0) {
        # build_host_tests.ps1 itself already lists BUILD FAILURES / RUN
        # FAILURES by executable name in the captured output above -- no need
        # to re-derive the failing names here, only to fail loud and point at
        # them.
        Write-Host ""
        Write-Host "FAIL: build_host_tests.ps1 exited $exitCode -- see BUILD FAILURES / RUN FAILURES" -ForegroundColor Red
        Write-Host "      (or MISMATCH) named in the output above for which executable(s)." -ForegroundColor Red
        exit 1
    }

    Write-Host "PASS: build_host_tests.ps1 built and passed every expected executable."
    exit 0
} finally {
    Remove-Item -ErrorAction SilentlyContinue -Recurse -Force $outDir
}
