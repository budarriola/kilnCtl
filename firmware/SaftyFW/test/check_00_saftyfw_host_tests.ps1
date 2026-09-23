# check_00_saftyfw_host_tests.ps1 -- closes the same standing-suite gap
# check_00_kilnfw_host_tests.ps1 closed for KilnFW: no check_*.ps1 discovered
# by tools/run_all_checks.ps1 built and ran SaftyFW's host-test suite
# (build_host_tests.ps1's four executables: saftyfw_host_tests.exe,
# kilnlink_fuzz_payloads.exe, hal_spi_pico_tests.exe,
# config_store_flash_tests.exe). Before this check, that whole suite was
# reachable only by a developer or agent invoking build_host_tests.ps1 by
# hand -- a build break in any one of the four executables could land with
# every discovered check_*.ps1 green.
#
# NAMED check_00_* BUT LEFT IN PHASE 2, same reasoning as
# check_00_kilnfw_host_tests.ps1's own header: run_all_checks.ps1 only grants
# phase-1 (unthrottled, concurrent) membership via an exact filename match in
# its $buildChecks selector, and this file's name is deliberately not in that
# list. It shares phase 2's throttle with everything else.
#
# ISOLATED, SHORT BUILD PATH. build_host_tests.ps1 accepts -OutDir so two
# concurrent copies (this check running alongside a developer's own manual
# build, or a sibling agent in another worktree) never collide on the same
# .obj/.exe paths -- the fixed-shared-scratch-path race class
# run_all_checks.ps1's own header warns every new check about. This check
# always passes a $PID-keyed -OutDir under $env:TEMP, never the shared
# firmware/SaftyFW/test/build directory, and deletes it in a finally block.
#
# SHORT, not just isolated: docs/agent_rules note (and CLAUDE.md's SaftyFW
# host-test section) that the response-file/cl invocation in
# build_host_tests.ps1 needs a short path -- the default
# .claude/worktrees/... path overflows the MSVC command line. $env:TEMP is
# short on this machine (C:\Users\<user>\AppData\Local\Temp), so no extra
# remapping is needed here the way a long worktree path would.
#
# SKIP (exit 3) posture, same contract check_00_kilnfw_host_tests.ps1 uses:
# only when the MSVC toolchain itself is not installed on this machine
# (vcvarsall.bat missing) -- never for a build or run failure, which is
# always a real FAIL.

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

# $PID-keyed, under $env:TEMP -- never the shared firmware/SaftyFW/test/build
# directory, so a concurrent manual build or a sibling agent's own run of
# build_host_tests.ps1 can never collide on the same .obj/.exe paths.
$outDir = Join-Path $env:TEMP "saftyfw_host_tests_check_$PID"

try {
    if (Test-Path $outDir) { Remove-Item -Recurse -Force $outDir }
    New-Item -ItemType Directory -Force -Path $outDir | Out-Null

    $buildScript = Join-Path $testDir "build_host_tests.ps1"
    # Redirected to FILES, not `2>&1 | Out-String` -- same NativeCommandError
    # gotcha check_00_kilnfw_host_tests.ps1's own header documents: under
    # $ErrorActionPreference = "Stop", piping a native command's stderr
    # in-process promotes any stderr line into a terminating error even when
    # the child's real exit code is 0.
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
    Write-Host "check_00_saftyfw_host_tests.ps1: wall time ${elapsedS}s"

    # build_host_tests.ps1 prints its own aggregate verdict line naming every
    # executable that failed ("SAFTYFW HOST TESTS: FAILED -- ..." or
    # "SAFTYFW HOST TESTS: all passed"); surface it here for a legible tail,
    # but the actual pass/fail decision always defers to its exit code below.
    $summaryLine = ($outText -split "`r?`n" | Where-Object { $_ -match '^SAFTYFW HOST TESTS:' } | Select-Object -First 1)
    if ($summaryLine) {
        Write-Host "check_00_saftyfw_host_tests.ps1: $($summaryLine.Trim())"
    } else {
        Write-Host "check_00_saftyfw_host_tests.ps1: WARNING -- no 'SAFTYFW HOST TESTS:' summary line found in output"
    }

    if ($exitCode -ne 0) {
        Write-Host ""
        Write-Host "FAIL: build_host_tests.ps1 exited $exitCode -- see the failing executable(s) named" -ForegroundColor Red
        Write-Host "      in the output above (build failure, or a non-zero test-run exit)." -ForegroundColor Red
        exit 1
    }

    Write-Host "PASS: build_host_tests.ps1 built and passed every expected executable."
    exit 0
} finally {
    Remove-Item -ErrorAction SilentlyContinue -Recurse -Force $outDir
}
