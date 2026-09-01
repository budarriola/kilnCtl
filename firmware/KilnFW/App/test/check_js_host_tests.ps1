# check_js_host_tests.ps1 -- runs every Node-only JS test harness under this
# directory automatically, closing the gap flagged during a 2026-09 audit:
# test_guided_flow.js, test_firing_chart.js, test_longpress.js and
# test_zones_inheritance.js existed only as documentation -- nothing in the
# repo invoked any of them without a human typing `node <path>` by hand.
# Between them they are the only coverage for the firing-chart colour-
# collision check, the per-zone trace sampler, the press-and-hold PID
# gesture, and settings_source inheritance's cycle guard (whose failure mode
# is an infinite loop, not a wrong answer -- see the timeout section below).
#
# Wired in as a check_*.ps1 (discovered by tools/run_all_checks.ps1's glob)
# rather than folded into build_host_tests.ps1, on purpose:
#   - build_host_tests.ps1 is scoped to the MSVC-built C suite; its
#     Invoke-HostTestExe bookkeeping (built/run/passed counts) is built
#     around compiling an .exe, which these harnesses never do. Reusing it
#     would mean forking that bookkeeping or bolting on a second unrelated
#     code path inside one script.
#   - run_all_checks.ps1 already exists for exactly this problem -- its own
#     header quotes ROADMAP.md M10: "Every one of them has been proven able
#     to fail, which is the hard part; being run automatically is the easy
#     part nobody has done." Dropping a new check_*.ps1 here needs no
#     further wiring: the next JS harness (a fifth, a sixth) is picked up by
#     the SAME discovery glob that already finds this file, with nothing to
#     remember to edit.
#
# Discovery is by glob (test_*.js in this directory), not a hardcoded list,
# for the same reason run_all_checks.ps1 globs check_*.ps1: a hardcoded list
# is this repository's most repeated defect class.
#
# node availability: checked once, up front. A MISSING node FAILS this
# check (non-zero exit) rather than warning and passing. A warn-and-pass
# would recreate precisely the bug this script exists to close -- a run
# that looks green while silently covering nothing -- just moved one layer
# down (from "nobody ran it" to "the runner declined to run it and said so
# quietly"). This check's only job is to make that state loud, so it fails.
# The message distinguishes this from an actual test failure so it is not
# misread as a JS regression.
#
# Per-file timeout: test_zones_inheritance.js's cycle-guard test is proven
# (2026-09, with the visited-set guard deliberately removed) to hang rather
# than fail when that guard regresses -- escaping it needed `timeout 8`.
# Each file here gets a hard wall-clock budget; a file that exceeds it is
# killed and counted as a FAILURE, not left to wedge this check or the
# aggregator that calls it. $TimeoutSeconds is generous relative to the
# ~1s each harness actually takes today, to leave headroom before this
# becomes the second false-failure class instead of the first missing-
# guard class.
#
# Usage: powershell -File firmware\KilnFW\App\test\check_js_host_tests.ps1
$ErrorActionPreference = "Stop"

$TimeoutSeconds = 20

$testDir = $PSScriptRoot

$node = Get-Command node -ErrorAction SilentlyContinue
if (-not $node) {
    Write-Host "JS HOST TEST CHECK: node not found on PATH." -ForegroundColor Red
    Write-Host "  SKIPPED, NOT PASSED -- this is a FAILURE, not a pass-through." -ForegroundColor Red
    Write-Host "  The JS harnesses under $testDir (test_*.js) were NOT run and their" -ForegroundColor Red
    Write-Host "  coverage (firing-chart colour collisions, per-zone trace sampling," -ForegroundColor Red
    Write-Host "  the press-and-hold PID gesture, settings_source inheritance incl." -ForegroundColor Red
    Write-Host "  its cycle guard) is UNVERIFIED for this run. Install Node.js and" -ForegroundColor Red
    Write-Host "  re-run, or this check will keep failing loudly by design." -ForegroundColor Red
    exit 1
}

$jsTests = Get-ChildItem -Path $testDir -Filter "test_*.js" -File | Sort-Object Name
if ($jsTests.Count -eq 0) {
    Write-Host "JS HOST TEST CHECK FAILED: no test_*.js files found under $testDir." -ForegroundColor Red
    Write-Host "  This almost certainly means the glob or directory is wrong, not that" -ForegroundColor Red
    Write-Host "  there is genuinely nothing to run -- investigate before trusting a" -ForegroundColor Red
    Write-Host "  green result elsewhere." -ForegroundColor Red
    exit 2
}

Write-Host ""
Write-Host "Running $($jsTests.Count) JS host test harness(es) from $testDir (node $($node.Version))"
Write-Host ""

$passed = @()
$failed = @()

foreach ($f in $jsTests) {
    $rel = $f.Name
    $proc = Start-Process -FilePath $node.Source -ArgumentList @("`"$($f.FullName)`"") `
        -WorkingDirectory $testDir -NoNewWindow -PassThru `
        -RedirectStandardOutput "$env:TEMP\jshosttest_$($f.BaseName)_out.log" `
        -RedirectStandardError "$env:TEMP\jshosttest_$($f.BaseName)_err.log"

    # Touching .Handle immediately after Start-Process is required for
    # .ExitCode to populate correctly after WaitForExit() below -- a known
    # .NET/PowerShell quirk (the Process object does not cache exit info
    # unless a handle was obtained while the process was still running).
    # Without this, $proc.ExitCode reads back empty even for a process that
    # exited 0, which would make every pass on this line look like a
    # failure -- caught by this script's own dry run before it was trusted.
    $procHandle = $proc.Handle
    $finished = $proc.WaitForExit($TimeoutSeconds * 1000)

    if (-not $finished) {
        try { Stop-Process -Id $proc.Id -Force -ErrorAction Stop } catch {}
        $failed += [pscustomobject]@{
            Name   = $rel
            Reason = "TIMEOUT after ${TimeoutSeconds}s -- killed. This is the failure mode " +
                     "test_zones_inheritance.js's cycle guard is proven to hit when its " +
                     "visited-set guard regresses; treat a timeout here as that guard failing, " +
                     "not as an infrastructure fluke."
        }
        Write-Host "  TIMEOUT  $rel (killed after ${TimeoutSeconds}s)" -ForegroundColor Red
        continue
    }

    $stdout = if (Test-Path "$env:TEMP\jshosttest_$($f.BaseName)_out.log") {
        Get-Content "$env:TEMP\jshosttest_$($f.BaseName)_out.log" -Raw
    } else { "" }
    $stderr = if (Test-Path "$env:TEMP\jshosttest_$($f.BaseName)_err.log") {
        Get-Content "$env:TEMP\jshosttest_$($f.BaseName)_err.log" -Raw
    } else { "" }

    if ($proc.ExitCode -eq 0) {
        $passed += $rel
        Write-Host "  PASS  $rel" -ForegroundColor Green
    } else {
        $failed += [pscustomobject]@{
            Name   = $rel
            Reason = "exit code $($proc.ExitCode)`n$stdout$stderr"
        }
        Write-Host "  FAIL  $rel (exit $($proc.ExitCode))" -ForegroundColor Red
    }
}

Write-Host ""
Write-Host "JS host tests: $($passed.Count)/$($jsTests.Count) passed"

if ($failed.Count -gt 0) {
    Write-Host ""
    Write-Host "$($failed.Count) of $($jsTests.Count) JS host test file(s) FAILED:" -ForegroundColor Red
    foreach ($fail in $failed) {
        Write-Host ""
        Write-Host "--- $($fail.Name) ---" -ForegroundColor Red
        Write-Host $fail.Reason
    }
    Write-Host ""
    exit 1
}

Write-Host "all $($jsTests.Count) JS host test files passed"
exit 0
