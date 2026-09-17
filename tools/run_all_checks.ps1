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
# THE FIXED-SHARED-SCRATCH-PATH RACE CLASS (swept 2026-09-16; read this
# before writing a check that compiles, links, or generates a file).
# Because this runner dispatches checks in PARALLEL, a check that writes a
# FIXED path races a second copy of itself and any sibling writing the same
# path. Four separate fixes now exist for this (143ff6af's
# build_heater_output_pwm_drift/run_<pid>/ and the sweep that followed it),
# and the sweep found instances in two shapes:
#   WRITER vs WRITER -- a fixed build directory, a fixed .obj/.exe name, a
#     fixed generated .bat, or a fixed $env:TEMP response file. A PRIVATE
#     directory is NOT sufficient: two copies of the same check still
#     collide inside it. Key the path on $PID (PowerShell) or os.getpid()
#     (Python) and delete it in a finally block.
#   WRITER vs SCANNER -- several checks recursively enumerate a source tree
#     and then read each file. A scratch file another check creates and
#     deletes in that tree can vanish between the enumeration and the read,
#     which is fatal under $ErrorActionPreference = "Stop". Two rules:
#     put scratch OUTSIDE any scanned source tree ($env:TEMP, $PID-keyed),
#     and enumerate git-tracked files rather than walking the working tree
#     (check_no_duplicate_crc.ps1 and check_stack_margin_registration.ps1
#     both do this).
# NO MECHANICAL CHECK ENFORCES THIS, deliberately. The instances have no
# unifying syntactic shape -- a Path join, a Join-Path, a response-file
# name, a Get-ChildItem -Recurse with no write of its own -- and a rule
# general enough to catch all of them would flag the large majority of
# correct code, since most fixed paths in this repo are read-only inputs or
# are already serialized by tools/build_lock.ps1's named mutex. (That mutex
# is keyed by NAME, not by directory: two scripts writing one directory
# under different lock names are NOT serialized -- check before relying on
# it.) This mirrors the reasoning CLAUDE.md records for the "reset one side
# of a pair" class, which was rejected for a mechanical check for the same
# reason. Review by hand instead, using the two rules above.
param(
    # Print what would run, run nothing. For confirming the glob sees what you
    # expect after moving a directory.
    [switch]$ListOnly,

    # Skip the discovery floor. Only for a deliberate partial tree.
    [switch]$AllowFewerChecks,

    # Run only checks whose repo-relative path matches this regex (-match).
    # Combines with -Skip (Only is applied first, then Skip removes from
    # what's left). For iterating on a single check without paying for the
    # whole suite.
    [string]$Only,

    # Exclude checks whose repo-relative path matches this regex (-match).
    [string]$Skip,

    # Skip ONLY the two full target builds (check_00_kilnfw_target_build.ps1,
    # check_00_saftyfw_target_build.ps1) -- for a caller that just ran
    # build_kilnfw/build_saftyfw (or equivalent) itself and wants the rest of
    # the suite without paying to redo the target build. Every other check,
    # including the two checks that read the target ELFs, still runs. This is
    # NOT a general "skip slow checks" switch -- everything else in the
    # default run still runs, because weakening any of it is exactly the
    # failure mode this whole exercise is trying to avoid.
    [switch]$Fast,

    # How many checks to run at once in the (large) parallel phase. Default
    # is a conservative fraction of the core count: most of these checks are
    # short-lived powershell/python processes, not itself CPU-bound work, but
    # a handful DO shell out to ninja/cmake with their own -j, and running too
    # many of those at once thrashes rather than helps. Checks that touch a
    # shared build directory already serialize themselves via
    # tools/build_lock.ps1's named mutex, so raising this is safe from a
    # correctness standpoint -- it only trades wall clock for CPU contention.
    [int]$MaxParallel = 8,

    # 2026-09-15: commit 9507918e made six KilnFW stack-budget checkers print
    # SKIP when the target ELF is 0 bytes -- i.e. a build is still writing it.
    # Parallelizing this runner makes that race MORE likely to actually
    # happen (a checker starting while check_00_*_target_build.ps1's publish
    # step is mid-write), so a SKIP is no longer safely ignorable the way an
    # honest "no toolchain on this machine" SKIP is: it can now mean "this
    # check's coverage silently didn't run, on THIS machine, because of how
    # this very script scheduled it." Default posture is therefore: any SKIP
    # fails the overall run (exit 1), loudly listing which checks and why,
    # same as a FAIL. Pass -AllowSkips to opt back into the old "skips don't
    # fail the suite" behavior for a machine that genuinely lacks a
    # prerequisite (no node/toolchain installed) and is not expected to ever
    # pass those checks.
    [switch]$AllowSkips
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

# test_check_route_tier_coverage.ps1 is a negative test, not a guard -- it
# proves check_route_tier_coverage.ps1's scan (docs/WEB_AUTH_PLAN.md section
# 1) can actually detect a route registered with no tier. Named test_*, not
# check_*, so the glob above does not pick it up; wired explicitly here,
# same pattern as the hal boundary negative test just above.
$routeTierNegativeTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_check_route_tier_coverage.ps1"
if (Test-Path $routeTierNegativeTest) {
    $checks += Get-Item $routeTierNegativeTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected negative test $routeTierNegativeTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing negative test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected negative test $routeTierNegativeTest not found -- proceeding" -ForegroundColor Yellow
    Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
}

# test_check_stop_path_never_gated.ps1 is a negative test, not a guard -- it
# proves check_stop_path_never_gated.ps1's scan (docs/WEB_AUTH_PLAN.md
# section 9, the LCD Stop-is-never-gated property) can actually detect a PIN
# gate moved onto the Stop branch. Named test_*, not check_*, so the glob
# above does not pick it up; wired explicitly here, same pattern as the two
# negative tests just above.
$stopPathNegativeTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_check_stop_path_never_gated.ps1"
if (Test-Path $stopPathNegativeTest) {
    $checks += Get-Item $stopPathNegativeTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected negative test $stopPathNegativeTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing negative test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected negative test $stopPathNegativeTest not found -- proceeding" -ForegroundColor Yellow
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

# -Only / -Skip / -Fast filtering happens AFTER the discovery floor check
# above, deliberately -- a broken glob must still be caught even when the
# caller is filtering down to one check, rather than a typo'd -Only silently
# hiding a discovery regression too.
if ($Only) {
    $checks = $checks | Where-Object { $_.FullName.Substring($repoRoot.Length + 1) -match $Only }
}
if ($Skip) {
    $checks = $checks | Where-Object { $_.FullName.Substring($repoRoot.Length + 1) -notmatch $Skip }
}
if ($Fast) {
    $checks = $checks | Where-Object {
        $_.FullName -notmatch 'check_00_kilnfw_target_build\.ps1$' -and
        $_.FullName -notmatch 'check_00_saftyfw_target_build\.ps1$'
    }
}

if ($ListOnly) {
    Write-Host "$($checks.Count) check scripts discovered:"
    foreach ($c in $checks) {
        Write-Host "  $($c.FullName.Substring($repoRoot.Length + 1))"
    }
    exit 0
}

Write-Host ""
Write-Host "Running $($checks.Count) guard scripts from $repoRoot (parallel, throttle $MaxParallel)"
Write-Host ""

$failed = @()
$passed = @()
$skipped = @()

# Scratch dir for redirected stdout/stderr of each parallel check process.
# Keyed by PID so two concurrent run_all_checks.ps1 invocations (different
# sessions/agents, same machine) never collide on the same files.
$scratchDir = Join-Path $env:TEMP "kilnctl_run_all_checks_$PID"
New-Item -ItemType Directory -Path $scratchDir -Force | Out-Null

function Start-CheckAsync {
    param($Check, [string]$RepoRoot, [string]$SelfcheckPy, [string]$SelfcheckPython, [string]$ScratchDir)

    $rel = $Check.FullName.Substring($RepoRoot.Length + 1)
    # Each check is run from ITS OWN directory's parent project, because
    # several resolve paths relative to $PSScriptRoot and at least one
    # (check_uri_handler_cap.ps1) recounts from source trees it locates that
    # way. Running them all from the repository root would have worked today
    # and broken silently the first time one of them changed how it resolves.
    $checkDir = Split-Path -Parent $Check.FullName

    if ($Check.FullName -eq $SelfcheckPy) {
        # selfcheck.py -- run under the PcTools venv's own interpreter, not
        # `powershell -File`, which cannot execute it.
        $exe = $SelfcheckPython
        $procArgs = @($Check.FullName)
    } elseif ($Check.Extension -eq ".py") {
        # The SaftyFW orphan negative tests -- no special venv needed, same
        # interpreter unittest is invoked with directly during development.
        $exe = "python"
        $procArgs = @($Check.FullName)
    } else {
        $exe = "powershell"
        $procArgs = @("-NoProfile", "-ExecutionPolicy", "Bypass", "-File", $Check.FullName)
    }

    $tag = ($rel -replace '[\\/:]', '_')
    $outFile = Join-Path $ScratchDir "$tag.out.txt"
    $errFile = Join-Path $ScratchDir "$tag.err.txt"

    # Start-Process launches a genuinely separate process (same isolation the
    # original in-process `&` call to a child powershell already relied on --
    # a check calling `exit` cannot terminate this aggregator either way), and
    # -- unlike the original `& ... 2>&1` under $ErrorActionPreference =
    # "Stop" -- a child writing to stderr can never be promoted into a
    # terminating NativeCommandError here, since Start-Process is not a
    # PowerShell-native invocation at all. So the Continue/Stop dance the
    # serial version needed is simply not a hazard for this path.
    $proc = Start-Process -FilePath $exe -ArgumentList $procArgs -WorkingDirectory $checkDir `
        -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -NoNewWindow

    # Well-known Start-Process/-PassThru gotcha: with output redirected, the
    # returned Process object's ExitCode reads back $null forever -- even
    # after WaitForExit() -- unless something first touches .Handle, which
    # forces .NET to reopen the process with a full-access handle instead of
    # the limited one Start-Process obtains by default. Confirmed by hand
    # while writing this: without this line, ExitCode was $null on every
    # single check, which silently misfiled every PASS as a FAIL with a
    # blank "(exit )" -- caught only because the negative-test pass for this
    # change ran the happy path first and it was already all-red.
    $null = $proc.Handle

    return [pscustomobject]@{
        Rel     = $rel
        Proc    = $proc
        OutFile = $outFile
        ErrFile = $errFile
    }
}

function Complete-CheckResult {
    param($Running, [int]$SkipExitCode)

    # .NET's Process.ExitCode has a well-known gotcha with Start-Process
    # -PassThru: HasExited can read true before ExitCode is reliably
    # populated on the same object. An explicit WaitForExit() (a no-op if it
    # already exited) forces the property to settle before we read it --
    # without this, ExitCode intermittently came back $null here, and $null
    # -eq 0 is false, so a genuine PASS was misfiled as a FAIL with a blank
    # "(exit )" in testing during this change.
    $Running.Proc.WaitForExit()
    $code = $Running.Proc.ExitCode
    $outText = ""
    foreach ($f in @($Running.OutFile, $Running.ErrFile)) {
        if (Test-Path $f) { $outText += (Get-Content -Raw -ErrorAction SilentlyContinue $f) }
    }
    Remove-Item -ErrorAction SilentlyContinue $Running.OutFile, $Running.ErrFile

    if ($code -eq 0) {
        Write-Host "  PASS  $($Running.Rel)" -ForegroundColor Green
        return [pscustomobject]@{ Bucket = "pass"; Path = $Running.Rel }
    } elseif ($code -eq $SkipExitCode) {
        $reasonLine = ($outText -split "`r?`n" | Where-Object { $_ -match 'SKIP' } | Select-Object -First 1)
        if (-not $reasonLine) {
            $reasonLine = "(no SKIP reason line found in output -- check violates the SKIP contract, see header)"
        }
        Write-Host "  SKIP  $($Running.Rel)" -ForegroundColor Yellow
        return [pscustomobject]@{ Bucket = "skip"; Path = $Running.Rel; Reason = $reasonLine.Trim() }
    } else {
        Write-Host "  FAIL  $($Running.Rel) (exit $code)" -ForegroundColor Red
        return [pscustomobject]@{ Bucket = "fail"; Path = $Running.Rel; Code = $code; Output = $outText }
    }
}

function Invoke-ChecksParallel {
    param($ChecksToRun, [int]$MaxParallel, [string]$RepoRoot, [string]$SelfcheckPy, [string]$SelfcheckPython, [string]$ScratchDir, [int]$SkipExitCode)

    $pending = New-Object System.Collections.Generic.Queue[object]
    foreach ($c in $ChecksToRun) { $pending.Enqueue($c) }
    $running = @()
    $results = @()

    while ($pending.Count -gt 0 -or $running.Count -gt 0) {
        while ($running.Count -lt $MaxParallel -and $pending.Count -gt 0) {
            $c = $pending.Dequeue()
            $running += Start-CheckAsync -Check $c -RepoRoot $RepoRoot -SelfcheckPy $SelfcheckPy -SelfcheckPython $SelfcheckPython -ScratchDir $ScratchDir
        }
        Start-Sleep -Milliseconds 200
        $stillRunning = @()
        foreach ($r in $running) {
            if ($r.Proc.HasExited) {
                $results += Complete-CheckResult -Running $r -SkipExitCode $SkipExitCode
            } else {
                $stillRunning += $r
            }
        }
        $running = $stillRunning
    }
    return $results
}

# TWO PHASES, not one flat parallel batch, to preserve a guarantee the
# original strict-alphabetical serial order gave for free: check_00_kilnfw_
# target_build.ps1 and check_00_saftyfw_target_build.ps1 must both FINISH
# (and publish their ELFs into the shared firmware/*/build/ directories)
# before anything that reads those artifacts runs (the stack-budget checks,
# compile_esp_backends.ps1/compile_pico_backends.ps1, check_saftyfw_task_
# count.ps1, etc.) -- those consumers only SKIP on a MISSING elf, not a
# STALE one, so if they ran concurrently with a build in flight they could
# silently grade a leftover artifact from a previous run instead of this
# one, same failure shape check_00_kilnfw_target_build.ps1's own header
# documents. The two target builds are independent of each other (separate
# toolchains, separate build dirs, separate build_lock.ps1 mutex names) so
# they still run concurrently with each other in phase 1; everything else
# (all lint/drift/mirror/host-test checks, which don't touch either target
# build's output) runs throttled in phase 2. Checks that DO share a build
# directory among themselves (e.g. two build_lock.ps1 users) still serialize
# correctly within phase 2 via that same named mutex -- they just queue
# instead of racing, exactly as build_lock.ps1's own header describes for
# two concurrent manual runs.
$buildChecks = $checks | Where-Object {
    $_.FullName -match 'check_00_kilnfw_target_build\.ps1$' -or
    $_.FullName -match 'check_00_saftyfw_target_build\.ps1$'
}
$restChecks = $checks | Where-Object {
    $_.FullName -notmatch 'check_00_kilnfw_target_build\.ps1$' -and
    $_.FullName -notmatch 'check_00_saftyfw_target_build\.ps1$'
}

$results = @()
if ($buildChecks.Count -gt 0) {
    Write-Host "Phase 1/2: target builds ($($buildChecks.Count))" -ForegroundColor Cyan
    $results += Invoke-ChecksParallel -ChecksToRun $buildChecks -MaxParallel ([Math]::Max(1, $buildChecks.Count)) `
        -RepoRoot $repoRoot -SelfcheckPy $selfcheckPy -SelfcheckPython $selfcheckPython -ScratchDir $scratchDir -SkipExitCode $SkipExitCode
}
if ($restChecks.Count -gt 0) {
    Write-Host "Phase 2/2: remaining checks ($($restChecks.Count))" -ForegroundColor Cyan
    $results += Invoke-ChecksParallel -ChecksToRun $restChecks -MaxParallel $MaxParallel `
        -RepoRoot $repoRoot -SelfcheckPy $selfcheckPy -SelfcheckPython $selfcheckPython -ScratchDir $scratchDir -SkipExitCode $SkipExitCode
}

Remove-Item -ErrorAction SilentlyContinue -Recurse -Force $scratchDir

foreach ($r in $results) {
    if ($r.Bucket -eq "pass") {
        $passed += $r.Path
    } elseif ($r.Bucket -eq "skip") {
        $skipped += [pscustomobject]@{ Path = $r.Path; Reason = $r.Reason }
    } else {
        $failed += [pscustomobject]@{ Path = $r.Path; Code = $r.Code; Output = $r.Output }
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

# A skip is never silently folded into "passed" -- the summary line always
# states the skip count explicitly and the per-check SKIP lines above always
# print, even on an otherwise-green run. Whether a skip fails the SUITE is
# now controlled by -AllowSkips (default: it does).
#
# 2026-09-15: this used to unconditionally exit 0 here on the theory that a
# skip is always a documented, environment-dependent non-failure (missing
# `node`, no build directory yet) -- forcing every developer machine without
# the full toolchain to show red would make the signal noisier, not clearer.
# That reasoning stopped holding once commit 9507918e made six KilnFW
# stack-budget checkers SKIP on a genuinely bad signal (a 0-byte ELF, i.e. a
# build still in flight) rather than only on a missing prerequisite -- and
# parallelizing this very runner made that race more likely, not less, since
# a checker and a build can now genuinely be scheduled close together. A
# runner that still called that combination a clean pass would be exactly
# the "green with zero coverage" trap this file's own header has warned
# about since its first revision. Default posture: any SKIP fails the run;
# -AllowSkips opts back into the old behavior for a machine that genuinely,
# permanently lacks a prerequisite.
if ($skipped.Count -gt 0 -and -not $AllowSkips) {
    Write-Host "$($passed.Count) passed, $($skipped.Count) skipped, $($failed.Count) failed." -ForegroundColor Red
    Write-Host "FAILED: $($skipped.Count) check(s) skipped and -AllowSkips was not passed -- a skip is not a pass." -ForegroundColor Red
    exit 1
}

Write-Host "$($passed.Count) passed, $($skipped.Count) skipped, $($failed.Count) failed." -ForegroundColor Green
exit 0
