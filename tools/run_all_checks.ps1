# Runs every standing guard script in the repository and returns a single
# verdict.
#
# ROADMAP.md M10 has carried this item for a while: "All of tools/check_*.ps1
# and firmware/*/tools/check_*.ps1 are standalone and manual today. Every one of
# them has been proven able to fail, which is the hard part; being run
# automatically is the easy part nobody has done."
#
# DISCOVERY, NOT A LIST. The scripts are found by globbing, deliberately. A
# hardcoded list is this repository's single most repeated defect -- the HTTP
# route cap fell behind the real route count four separate times, each time
# surfacing as "one page is broken", and a comment saying "keep this ahead of
# the count" had already failed three times before the count was made to
# recompute itself. A new check_*.ps1 must be run by this script the moment it
# is written, without anyone remembering to come here.
#
# The trap that discovery introduces instead is the opposite one: a glob that
# matches nothing reports "all passed" in a cheerful green, which is worse than
# a failure because it looks like evidence. $MinimumChecks below is the floor
# that makes that case loud. It is not a target to keep bumping -- it is a
# tripwire for a broken glob, a moved directory, or a wrong working directory.
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File tools\run_all_checks.ps1
#   powershell -ExecutionPolicy Bypass -File tools\run_all_checks.ps1 -ListOnly
#
# Exit code is 0 only if every discovered check passed (or was legitimately
# skipped) and at least $MinimumChecks of them were found.
#
# SKIP STATUS. A check script may find its own prerequisite missing (no
# `node` on PATH, no build directory yet, a toolchain not installed) without
# that being a defect -- these are documented, deliberate non-failures
# (docs/audits/check_independence_2026-09-07.md category 4). Before this
# revision, such a check reported that by printing a yellow warning and
# calling `exit 0`, which is indistinguishable, to this runner and to its
# exit code, from an actual PASS -- a check silently loses all its coverage
# on a machine missing its prerequisite while run_all_checks.ps1 keeps
# reporting a clean sweep. That is exactly the "guard reports green with
# zero coverage" shape this repo has been burned by before (see
# check_source_path_drift.ps1's `if not path.is_file(): skipTest(...)`
# history) and the audit above found three fresh instances of it.
#
# THE CONTRACT (applies to both .ps1 and .py checks -- this is a reserved
# EXIT CODE, not a PowerShell-only mechanism):
#   exit 0  -- PASS. The check ran its real assertion and it held.
#   exit 3  -- SKIP. The check's own prerequisite (toolchain, build output,
#              external tool) was absent. The check MUST still print, to
#              stdout/stderr, a line containing the word "SKIP" followed by
#              the specific reason (e.g. "SKIP: no `node` on PATH") -- this
#              runner greps that line back out for the skip summary below,
#              so a skip with no stated reason is itself a bug in the check.
#   anything else -- FAIL. Includes PowerShell `throw`, a non-zero/non-3 exit
#              from a Python check, or any other exit code.
# A check that can never legitimately skip should simply never exit 3; there
# is no opt-in required beyond using this exit code correctly.
param(
    # Print what would run, run nothing. For confirming the glob sees what you
    # expect after moving a directory.
    [switch]$ListOnly,

    # Skip the discovery floor. Only for a deliberate partial tree.
    [switch]$AllowFewerChecks
)

# param() must be the first statement in the script, so this assignment --
# previously placed above param() -- is here instead. It was harmless on its
# own (PowerShell just silently failed to bind ANY parameter when param()
# wasn't first, running the switches as $null/$false), but it meant
# -ListOnly did nothing at all: the script always ran the full suite.
$SkipExitCode = 3

$ErrorActionPreference = "Stop"

# The repository root is this script's parent's parent -- derived, never
# assumed from the caller's working directory, so the script gives the same
# answer whether it is run from the root, from an editor, or from a hook.
$repoRoot = Split-Path -Parent $PSScriptRoot

# Every check_*.ps1 anywhere in the tree, excluding build output and any
# vendored/third-party directory that might ship its own.
$checks = Get-ChildItem -Path $repoRoot -Filter "check_*.ps1" -Recurse -File |
    Where-Object {
        $_.FullName -notmatch '\\build\\' -and
        $_.FullName -notmatch '\\node_modules\\' -and
        # Any dotted directory: .venv, .git, and -- the one that actually bit
        # here -- .claude\worktrees\, which holds leftover per-agent copies of
        # the whole tree. Without this the first run of this script found 24
        # scripts where the repository has 11, and would have been reporting
        # the pass/fail state of an abandoned worktree alongside the real one.
        $_.FullName -notmatch '\\\.[^\\]+\\'
    } |
    Sort-Object FullName

# test_check_hal_include_boundary.ps1 is a negative test, not a guard --
# it proves check_hal_include_boundary.ps1's scan can actually detect a
# violation (HW_ABSTRACTION.md Phase 4's "negative test -- a new
# precedent, none of the existing checks has one"). It is named test_*, not
# check_*, so the glob above does not pick it up on its own; it is added
# here explicitly rather than renamed, since firmware/KilnFW/App/test/ is
# where every other test_*.ps1/.c in this repo lives and it belongs there,
# not under tools/.
$halBoundaryNegativeTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_check_hal_include_boundary.ps1"
if (Test-Path $halBoundaryNegativeTest) {
    $checks += Get-Item $halBoundaryNegativeTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    # 2026-09-10 (opus review, round 2): this used to be a silent WARNING
    # that dropped the check from $checks while the script still reported
    # every remaining check passed -- exactly the "wired but silently
    # skipped" trap selfcheck.py's block below was already hardened
    # against (a check that goes missing must not read as a clean run).
    # Hard failure instead, same pattern.
    Write-Host ""
    Write-Host "FAILED: expected negative test $halBoundaryNegativeTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing negative test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    exit 2
} elseif (-not (Test-Path $halBoundaryNegativeTest)) {
    # 2026-09-10 (opus review, round 3): under -AllowFewerChecks this branch
    # used to fall through with NO output at all -- strictly worse than the
    # original silent WARNING it replaced, since -AllowFewerChecks is the one
    # mode meant to tolerate a missing expected file, and it now does so with
    # zero signal. Restore a visible warning.
    Write-Host ""
    Write-Host "WARNING: expected negative test $halBoundaryNegativeTest not found -- proceeding" -ForegroundColor Yellow
    Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
}

# firmware/hwAbstraction/test/{compile_esp_backends,compile_pico_backends,
# test_host_fakes}.ps1 are named compile_*/test_* rather than check_*, so
# the glob above does not pick them up on its own -- added explicitly here,
# same pattern as the hal boundary negative test just above. compile_esp_
# backends.ps1 and compile_pico_backends.ps1 legitimately exit 1 when the
# matching toolchain/build dir is missing (IDF's compile_commands.json /
# SaftyFW's build.ninja) -- that is a correct, non-vacuous failure on a
# machine without that toolchain configured, not a bug in the script.
# test_regsp_margin_against_declared.py and test_regsp_stale_literal.py
# (firmware/SaftyFW/test/) are negative tests for the 2026-09-10 (round 2)
# opus-review defects A -- they prove check_saftyfw_task_stack_budgets.py's
# regsp_margin_fail() and stack_budget_lib_arm.py's parse() can actually
# detect the bugs they were written against. Neither check_*.ps1 nor
# check_saftyfw_task_stack_budgets.ps1 invoked them, and a repo-wide grep
# found no reference to either filename anywhere but the file itself -- the
# same "orphaned negative test" shape check_saftyfw_task_count.py was found
# in earlier. Added explicitly, same pattern as the hal boundary negative
# test above; run with plain `python` (no special venv needed, same as the
# checker they import).
$saftyfwTestDir = Join-Path $repoRoot "firmware\SaftyFW\test"
$saftyfwOrphanTests = @("test_regsp_margin_against_declared.py", "test_regsp_stale_literal.py")
foreach ($name in $saftyfwOrphanTests) {
    $scriptPath = Join-Path $saftyfwTestDir $name
    if (Test-Path $scriptPath) {
        $checks += Get-Item $scriptPath
    } elseif (-not $AllowFewerChecks) {
        # See the hal-boundary block above: a silent WARNING here used to
        # drop the check from $checks with no effect on the final pass/fail
        # count -- "wired but silently skipped" is invisible, which is the
        # recurring class this whole file exists to close.
        Write-Host ""
        Write-Host "FAILED: expected SaftyFW negative test $scriptPath not found --" -ForegroundColor Red
        Write-Host "        has it moved? A missing negative test must not read as a clean run." -ForegroundColor Red
        Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
        exit 2
    } elseif (-not (Test-Path $scriptPath)) {
        # Same restore as the hal-boundary block above: -AllowFewerChecks
        # must not be completely silent about what it is tolerating.
        Write-Host ""
        Write-Host "WARNING: expected SaftyFW negative test $scriptPath not found -- proceeding" -ForegroundColor Yellow
        Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
    }
}
$checks = $checks | Sort-Object FullName

$hwAbstractionTestDir = Join-Path $repoRoot "firmware\hwAbstraction\test"
$hwAbstractionScripts = @("compile_esp_backends.ps1", "compile_pico_backends.ps1", "test_host_fakes.ps1")
foreach ($name in $hwAbstractionScripts) {
    $scriptPath = Join-Path $hwAbstractionTestDir $name
    if (Test-Path $scriptPath) {
        $checks += Get-Item $scriptPath
    } elseif (-not $AllowFewerChecks) {
        # Same hardening as the two blocks above.
        Write-Host ""
        Write-Host "FAILED: expected hwAbstraction test $scriptPath not found --" -ForegroundColor Red
        Write-Host "        has it moved? A missing expected check must not read as a clean run." -ForegroundColor Red
        Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
        exit 2
    } elseif (-not (Test-Path $scriptPath)) {
        # Same restore as the hal-boundary block above.
        Write-Host ""
        Write-Host "WARNING: expected hwAbstraction test $scriptPath not found -- proceeding" -ForegroundColor Yellow
        Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
    }
}
$checks = $checks | Sort-Object FullName

# tools/PcTools/selfcheck.py is not a check_*.ps1 -- it's a standalone Python
# script -- so the glob above never finds it, and run_all_checks.ps1 had been
# reporting a clean sweep of every *.ps1 guard while selfcheck.py itself was
# hard-failing (a stale hardcoded path left by a directory reorg -- see
# tools/PcTools/TODO.md's 2026-09-05 GUI-vs-MCP audit entry). Added
# explicitly, same pattern as the hal boundary negative test and the
# hwAbstraction scripts above. Run via the project venv's own interpreter,
# never `python`/`uv run` from PATH -- this also sidesteps `uv run`'s sync
# step colliding with a live kilnctrl MCP server holding its own venv's
# console-script .exe open.
$pcToolsDir = Join-Path $repoRoot "tools\PcTools"
$selfcheckPy = Join-Path $pcToolsDir "selfcheck.py"
$selfcheckPython = Join-Path $pcToolsDir ".venv\Scripts\python.exe"
if ((Test-Path $selfcheckPy) -and (Test-Path $selfcheckPython)) {
    # A synthetic entry: the main loop below special-cases .py files to run
    # under $selfcheckPython instead of `powershell -File`.
    $checks += Get-Item $selfcheckPy
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    # A missing selfcheck.py/venv used to be a silent WARNING that just
    # dropped the check from the run while the script still reported "all
    # passed" -- exactly the "glob found nothing, still green" trap this
    # file's own header warns about, just for a hand-added entry instead of
    # a glob. Hard failure instead: a live selfcheck.py that regressed is
    # supposed to show up as FAIL below, not as a check quietly missing.
    Write-Host ""
    Write-Host "FAILED: expected $selfcheckPy (or its venv $selfcheckPython) not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing selfcheck.py must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    exit 2
} elseif (-not ((Test-Path $selfcheckPy) -and (Test-Path $selfcheckPython))) {
    # Same restore as the hal-boundary block above.
    Write-Host ""
    Write-Host "WARNING: expected $selfcheckPy (or its venv $selfcheckPython) not found -- proceeding" -ForegroundColor Yellow
    Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
}

# As of 2026-08-28 there are several: some under tools/, two under
# firmware/SaftyFW/tools/, and the floor is set below the real count on
# purpose. It exists to catch "the glob found nothing", not to assert an
# exact inventory; setting it equal to the count would turn every legitimate
# deletion into a failure and teach people to edit this number, which is how
# the route cap got into trouble.
$MinimumChecks = 5

if ($checks.Count -lt $MinimumChecks -and -not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: found only $($checks.Count) check scripts under $repoRoot," -ForegroundColor Red
    Write-Host "        which is below the floor of $MinimumChecks. This almost certainly means" -ForegroundColor Red
    Write-Host "        the glob is broken or a directory moved -- NOT that the repository is" -ForegroundColor Red
    Write-Host "        clean. Investigate before trusting any green result from this script." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    exit 2
}

if ($ListOnly) {
    Write-Host "$($checks.Count) check scripts discovered:"
    foreach ($c in $checks) {
        Write-Host "  $($c.FullName.Substring($repoRoot.Length + 1))"
    }
    exit 0
}

Write-Host ""
Write-Host "Running $($checks.Count) guard scripts from $repoRoot"
Write-Host ""

$failed = @()
$passed = @()
$skipped = @()

foreach ($c in $checks) {
    $rel = $c.FullName.Substring($repoRoot.Length + 1)

    # Each check is run from ITS OWN directory's parent project, because
    # several resolve paths relative to $PSScriptRoot and at least one
    # (check_uri_handler_cap.ps1) recounts from source trees it locates that
    # way. Running them all from the repository root would have worked today
    # and broken silently the first time one of them changed how it resolves.
    $checkDir = Split-Path -Parent $c.FullName

    # Run in a child powershell so that a check calling `exit` cannot terminate
    # this aggregator, and so $ErrorActionPreference = "Stop" inside one check
    # cannot leak out. The exit code is the whole contract.
    Push-Location $checkDir
    try {
        # $ErrorActionPreference is dropped to Continue for exactly this call.
        # Under "Stop", ANY line a child process writes to stderr is promoted to
        # a terminating NativeCommandError -- so the first failing check would
        # abort this aggregator and the remaining checks would never run, while
        # the output still looked like a report. A runner that stops at the
        # first failure is a runner that hides every failure after it.
        $prev = $ErrorActionPreference
        $ErrorActionPreference = "Continue"
        if ($c.FullName -eq $selfcheckPy) {
            # selfcheck.py (see above) -- run under the PcTools venv's own
            # interpreter, not `powershell -File`, which cannot execute it.
            $output = & $selfcheckPython $c.FullName 2>&1
        } elseif ($c.Extension -eq ".py") {
            # The SaftyFW orphan negative tests (see above) -- no special
            # venv needed, same interpreter unittest is invoked with
            # directly during development.
            $output = & python $c.FullName 2>&1
        } else {
            $output = & powershell -NoProfile -ExecutionPolicy Bypass -File $c.FullName 2>&1
        }
        $code = $LASTEXITCODE
        $ErrorActionPreference = $prev
    } finally {
        Pop-Location
    }

    if ($code -eq 0) {
        $passed += $rel
        Write-Host "  PASS  $rel" -ForegroundColor Green
    } elseif ($code -eq $SkipExitCode) {
        # Pull the check's own stated reason back out of its output (see the
        # SKIP contract in this file's header) rather than inventing one --
        # the check is the authority on why it couldn't run.
        $outText = ($output | Out-String)
        $reasonLine = ($outText -split "`r?`n" | Where-Object { $_ -match 'SKIP' } | Select-Object -First 1)
        if (-not $reasonLine) {
            $reasonLine = "(no SKIP reason line found in output -- check violates the SKIP contract, see header)"
        }
        $skipped += [pscustomobject]@{ Path = $rel; Reason = $reasonLine.Trim() }
        Write-Host "  SKIP  $rel" -ForegroundColor Yellow
    } else {
        $failed += [pscustomobject]@{ Path = $rel; Code = $code; Output = ($output | Out-String) }
        Write-Host "  FAIL  $rel (exit $code)" -ForegroundColor Red
    }
}

Write-Host ""

if ($skipped.Count -gt 0) {
    Write-Host "$($skipped.Count) check(s) SKIPPED (prerequisite absent -- not counted as passed):" -ForegroundColor Yellow
    foreach ($s in $skipped) {
        Write-Host "  SKIP  $($s.Path)" -ForegroundColor Yellow
        Write-Host "        $($s.Reason)" -ForegroundColor Yellow
    }
    Write-Host ""
}

if ($failed.Count -gt 0) {
    Write-Host "$($failed.Count) of $($checks.Count) checks FAILED:" -ForegroundColor Red
    foreach ($f in $failed) {
        Write-Host ""
        Write-Host "--- $($f.Path) (exit $($f.Code)) ---" -ForegroundColor Red
        Write-Host $f.Output.TrimEnd()
    }
    Write-Host ""
    Write-Host "$($passed.Count) passed, $($skipped.Count) skipped, $($failed.Count) failed." -ForegroundColor Red
    exit 1
}

# A skip is deliberately NOT a suite failure -- these are documented,
# environment-dependent non-failures (missing `node`, no build directory
# yet), and forcing every developer machine without the full toolchain
# installed to show a red run_all_checks.ps1 would make the failure signal
# noisier, not clearer. But it must never be silently indistinguishable from
# a full pass either (that was exactly this mechanism's reason for existing)
# -- so the summary line always states the skip count explicitly, never
# folds it into "passed", and the per-check SKIP lines above always print
# even on an otherwise-green run.
Write-Host "$($passed.Count) passed, $($skipped.Count) skipped, $($failed.Count) failed." -ForegroundColor Green
exit 0
