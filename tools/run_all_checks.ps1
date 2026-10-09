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

    # Skip ONLY the three phase-1 full target builds (check_00_kilnfw_target_
    # build.ps1, check_00_saftyfw_target_build.ps1, check_00_kilnfw_recovery_
    # target_build.ps1) plus the separate phase-2 check_00_kilnfw_host_tests.ps1
    # -- for a caller that already ran build_kilnfw/build_saftyfw/
    # build_host_tests (or the equivalent) itself and wants the rest of the
    # suite without paying to redo that multi-minute work. Every other check,
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
    [switch]$AllowSkips,

    # Disable the machine-wide check-result cache (tools/checkcache_lib.ps1;
    # KILNCTL_CHECKCACHE=0 does the same). Every check then runs for real.
    [switch]$NoCache,

    # A check that could not get a build-gate slot within the gate timeout
    # (tools/build_gate.ps1, 3600 s) did NOT run: that is machine load, not a
    # defect in the tree. It is filed under its own BUSY heading with a
    # rerun-it-alone hint instead of an anonymous FAIL, but it still fails the
    # run (nothing was verified) unless -AllowBusy is passed.
    [switch]$AllowBusy,

    # "Known failures on main" baseline (tools/main_baseline_lib.ps1). Every run
    # prints a "vs main baseline" section splitting failures into NEW / KNOWN /
    # FIXED. By default the exit code is unchanged (any failure fails the run).
    # With -FailOnlyOnNew a run whose every failure is KNOWN (also fails on the
    # baseline of an origin/main ancestor of HEAD) exits 0, loudly. No usable
    # baseline means nothing is KNOWN, so the exit code stays as it was.
    [switch]$FailOnlyOnNew
)

# param() must be the first statement in the script, so this assignment --
# previously placed above param() -- is here instead. It was harmless on its
# own (PowerShell just silently failed to bind ANY parameter when param()
# wasn't first, running the switches as $null/$false), but it meant
# -ListOnly did nothing at all: the script always ran the full suite.
$SkipExitCode = 3

$ErrorActionPreference = "Stop"

# This process-wide env var must not outlive this run: a caller that invokes
# this script by dot-sourcing it, or from a long-lived PowerShell session
# that later runs a NON--Fast pass in the same process, must never see a
# leftover KILNCTL_CHECKS_FAST from a previous -Fast run. Set/cleared here,
# before any early `exit`, and cleaned up again at every exit point below
# (and from the trap, for an uncaught terminating error) via
# Clear-ChecksFastEnv, rather than relying on one cleanup line at the very
# bottom of the script that several paths below `exit` before reaching.
function Clear-ChecksFastEnv {
    if ($Fast) {
        Remove-Item Env:\KILNCTL_CHECKS_FAST -ErrorAction SilentlyContinue
    }
}
if ($Fast) {
    $env:KILNCTL_CHECKS_FAST = "1"
} else {
    Remove-Item Env:\KILNCTL_CHECKS_FAST -ErrorAction SilentlyContinue
}
trap {
    Clear-ChecksFastEnv
    break
}

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
    Clear-ChecksFastEnv
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
    Clear-ChecksFastEnv
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected negative test $routeTierNegativeTest not found -- proceeding" -ForegroundColor Yellow
    Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
}

# test_check_uri_handler_cap_max_routes.ps1 is a negative test -- it proves
# check_uri_handler_cap.ps1's KILN_HTTP_MAX_ROUTES rule fails when violated.
# Named test_*, so the glob above does not pick it up; wired explicitly here.
$uriCapNegativeTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_check_uri_handler_cap_max_routes.ps1"
if (Test-Path $uriCapNegativeTest) {
    $checks += Get-Item $uriCapNegativeTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected negative test $uriCapNegativeTest not found." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    Clear-ChecksFastEnv
    exit 2
} else {
    Write-Host "WARNING: expected negative test $uriCapNegativeTest not found -- proceeding" -ForegroundColor Yellow
}

# test_check_config_migration_steps.ps1 is a negative test, not a guard -- it
# proves check_config_migration_steps.ps1's scan (docs/CONFIG_MIGRATION_CHAIN.md
# section 5) can actually detect a version bump with no matching step, plus
# every other rule shape it claims to enforce. Named test_*, not check_*, so
# the glob above does not pick it up; wired explicitly here, same pattern as
# the negative tests above.
$configMigrationStepsNegativeTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_check_config_migration_steps.ps1"
if (Test-Path $configMigrationStepsNegativeTest) {
    $checks += Get-Item $configMigrationStepsNegativeTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected negative test $configMigrationStepsNegativeTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing negative test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    Clear-ChecksFastEnv
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected negative test $configMigrationStepsNegativeTest not found -- proceeding" -ForegroundColor Yellow
    Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
}

# test_check_stop_path_requires_pin.ps1 is a negative test, not a guard --
# it proves check_stop_path_requires_pin.ps1's scan (owner decision
# 2026-09-28, reversing docs/WEB_AUTH_PLAN.md section 9's old
# Stop-is-never-gated rule: "stop needs login. there is an estop button")
# can actually detect a PIN gate removed from the Stop branch. Named test_*,
# not check_*, so the glob above does not pick it up; wired explicitly here,
# same pattern as the two negative tests just above. (This test and its
# check were renamed from *_never_gated to *_requires_pin on 2026-09-28,
# inverting every assertion to match the reversal.)
$stopPathNegativeTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_check_stop_path_requires_pin.ps1"
if (Test-Path $stopPathNegativeTest) {
    $checks += Get-Item $stopPathNegativeTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected negative test $stopPathNegativeTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing negative test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    Clear-ChecksFastEnv
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected negative test $stopPathNegativeTest not found -- proceeding" -ForegroundColor Yellow
    Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
}

# test_check_duplicate_symbols.ps1 is a scratch-dir test of
# check_duplicate_symbols.ps1's manifest selection, not a guard. Named test_*,
# not check_*, so the glob above does not pick it up; wired explicitly here,
# same pattern as the negative tests above.
$dupSymbolsTest = Join-Path $repoRoot "tools\test_check_duplicate_symbols.ps1"
if (Test-Path $dupSymbolsTest) {
    $checks += Get-Item $dupSymbolsTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected test $dupSymbolsTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    Clear-ChecksFastEnv
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected test $dupSymbolsTest not found -- proceeding" -ForegroundColor Yellow
}

# test_check_lcd_home_nav_gated.ps1 is a negative test, not a guard -- it
# proves check_lcd_home_nav_gated.ps1's scan (owner decision 2026-09-28: only
# the LCD home/dashboard view stays reachable without a PIN) can actually
# detect a missing gate on the Config-hub/profile-picker nav callbacks, and a
# wrongly-added gate on the credential-reset gesture. Named test_*, not
# check_*, so the glob above does not pick it up; wired explicitly here, same
# pattern as the negative tests above.
$lcdHomeNavNegativeTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_check_lcd_home_nav_gated.ps1"
if (Test-Path $lcdHomeNavNegativeTest) {
    $checks += Get-Item $lcdHomeNavNegativeTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected negative test $lcdHomeNavNegativeTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing negative test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    Clear-ChecksFastEnv
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected negative test $lcdHomeNavNegativeTest not found -- proceeding" -ForegroundColor Yellow
    Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
}

# test_stack_budget_symbol_bounds.py (firmware/KilnFW/App/test/) is a
# regression test for the objdump-decodes-past-a-function's-real-end
# phantom call-graph edge class: 924eeea2 fixed it for stack_budget_lib.py;
# 2026-09-20 fixed the same bug in this same directory's
# check_main_task_stack_budget.py, the older name-keyed parser
# check_executor_task_stack_budget.py/check_httpd_task_stack_budget.py/
# check_system_uart_bridge_stack_budget.py/check_uart_log_bridge_stack_
# budget.py all actually import and call. Named test_*, not check_*, so
# the glob above does not pick it up; wired explicitly here, same pattern
# as the SaftyFW regsp negative tests below. Needs no ELF and no Xtensa
# toolchain -- objdump is monkeypatched -- so it runs unconditionally.
$stackBudgetSymbolBoundsTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_stack_budget_symbol_bounds.py"
if (Test-Path $stackBudgetSymbolBoundsTest) {
    $checks += Get-Item $stackBudgetSymbolBoundsTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected test $stackBudgetSymbolBoundsTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing regression test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    Clear-ChecksFastEnv
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected test $stackBudgetSymbolBoundsTest not found -- proceeding" -ForegroundColor Yellow
    Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
}

# test_sdkconfig_sibling_pair_guard.py (firmware/KilnFW/App/test/) is a
# regression test for use_sdkconfig_for_elf()'s sibling-pair agreement guard
# in this same directory's check_all_task_stack_budgets.py -- the "reset one
# side of a pair" class where check_00_kilnfw_target_build.ps1 publishes a
# checkbuild's sdkconfig next to the ELF, but a later plain `idf.py build`
# replaces the ELF while leaving that published sibling in place, so the ELF
# could silently be graded against a stale, disagreeing config. Named
# test_*, not check_*, so the glob above does not pick it up; wired
# explicitly here, same pattern as test_stack_budget_symbol_bounds.py above.
# Needs no ELF and no toolchain, so it runs unconditionally.
$sdkconfigSiblingPairGuardTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_sdkconfig_sibling_pair_guard.py"
if (Test-Path $sdkconfigSiblingPairGuardTest) {
    $checks += Get-Item $sdkconfigSiblingPairGuardTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected test $sdkconfigSiblingPairGuardTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing regression test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected test $sdkconfigSiblingPairGuardTest not found -- proceeding" -ForegroundColor Yellow
    Write-Host "         without it because -AllowFewerChecks was passed." -ForegroundColor Yellow
}

# test_check_ui_responsive_sweep.ps1 is a negative test, not a guard -- it
# proves ui_responsive_sweep.mjs's isTransientHarnessError() classifier (used
# by check_ui_responsive_sweep.ps1, itself glob-discovered above) can still
# tell a transient CDP/harness error apart from a genuine layout-regression
# FAIL. Added 2026-09-17 alongside the fix for the classifier's gap that
# caused three real misfires under this script's own parallel phase that day
# (see ui_responsive_sweep.mjs's comment on isTransientHarnessError() and
# check_ui_responsive_sweep.ps1's phase-3 note below). Named test_*, not
# check_*, so the glob above does not pick it up; wired explicitly here, same
# pattern as the other negative tests above.
$uiResponsiveSweepNegativeTest = Join-Path $repoRoot "firmware\KilnFW\App\test\test_check_ui_responsive_sweep.ps1"
if (Test-Path $uiResponsiveSweepNegativeTest) {
    $checks += Get-Item $uiResponsiveSweepNegativeTest
    $checks = $checks | Sort-Object FullName
} elseif (-not $AllowFewerChecks) {
    Write-Host ""
    Write-Host "FAILED: expected negative test $uiResponsiveSweepNegativeTest not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing negative test must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    Clear-ChecksFastEnv
    exit 2
} else {
    Write-Host ""
    Write-Host "WARNING: expected negative test $uiResponsiveSweepNegativeTest not found -- proceeding" -ForegroundColor Yellow
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
        Clear-ChecksFastEnv
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
        Clear-ChecksFastEnv
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
# The worktree's own tools/PcTools/.venv is gitignored, so a fresh worktree
# never has one. Resolve the interpreter in order: the worktree venv, then
# $env:KILNCTL_PCTOOLS_PYTHON, then the main tree's venv (the parent of
# `git rev-parse --git-common-dir`). selfcheck.py is always run from the
# worktree's own tools\PcTools with PYTHONPATH=src (see Start-CheckAsync),
# so a borrowed interpreter still tests THIS tree's code, not the main tree's.
. (Join-Path $PSScriptRoot "lib_pctools_python.ps1")
$selfcheckPython = Resolve-PcToolsPython -RepoRoot $repoRoot -NoPathFallback
if (-not $selfcheckPython) { $selfcheckPython = Join-Path $pcToolsDir ".venv\Scripts\python.exe" }
if ((Test-Path $selfcheckPy) -and (Test-Path $selfcheckPython)) {
    # A synthetic entry: the main loop below special-cases .py files to run
    # under $selfcheckPython instead of `powershell -File`.
    $checks += Get-Item $selfcheckPy
    $checks = $checks | Sort-Object FullName
} elseif ((Test-Path $selfcheckPy) -and -not (Test-Path $selfcheckPython)) {
    # The script exists but no interpreter resolved anywhere: fail even under
    # -AllowFewerChecks. Silently dropping selfcheck was the old behaviour and
    # meant every fresh-worktree run skipped it.
    Write-Host ""
    Write-Host "FAILED: $selfcheckPy exists but no PcTools python was found (tried the worktree" -ForegroundColor Red
    Write-Host "        .venv, KILNCTL_PCTOOLS_PYTHON, and the main tree's tools\PcTools\.venv)." -ForegroundColor Red
    Write-Host "        Set KILNCTL_PCTOOLS_PYTHON to a python with the PcTools dependencies." -ForegroundColor Red
    Clear-ChecksFastEnv
    exit 2
} elseif (-not $AllowFewerChecks) {
    # A missing selfcheck.py used to be a silent WARNING that just
    # dropped the check from the run while the script still reported "all
    # passed" -- exactly the "glob found nothing, still green" trap this
    # file's own header warns about, just for a hand-added entry instead of
    # a glob. Hard failure instead: a live selfcheck.py that regressed is
    # supposed to show up as FAIL below, not as a check quietly missing.
    Write-Host ""
    Write-Host "FAILED: expected $selfcheckPy not found --" -ForegroundColor Red
    Write-Host "        has it moved? A missing selfcheck.py must not read as a clean run." -ForegroundColor Red
    Write-Host "        Pass -AllowFewerChecks if a partial tree is genuinely intended." -ForegroundColor Red
    Clear-ChecksFastEnv
    exit 2
} else {
    # Same restore as the hal-boundary block above.
    Write-Host ""
    Write-Host "WARNING: expected $selfcheckPy not found -- proceeding" -ForegroundColor Yellow
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
    Clear-ChecksFastEnv
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
        $_.FullName -notmatch 'check_00_saftyfw_target_build\.ps1$' -and
        $_.FullName -notmatch 'check_00_kilnfw_recovery_target_build\.ps1$' -and
        # check_00_kilnfw_host_tests.ps1 (2026-09-21): builds and runs all of
        # build_host_tests.ps1's KilnFW host-test executables from a clean,
        # isolated -OutDir -- same multi-minute cost class as the three
        # target builds above. A caller who already ran build_host_tests.ps1
        # (or an equivalent) itself gets to skip redoing it here too, same as
        # -Fast already does for the target builds.
        $_.FullName -notmatch 'check_00_kilnfw_host_tests\.ps1$'
    }
}

# SKIP-FAST (2026-09-23). Skipping the three phase-1 target builds above
# necessarily starves a handful of downstream checks of the one artifact
# they exist to grade (check_recovery_image_size.ps1 needs recovery.bin from
# check_00_kilnfw_recovery_target_build.ps1; check_embedded_pico_image_fresh.ps1
# needs the SaftyFW slot bins from check_00_saftyfw_target_build.ps1;
# check_web_gzip_parity.ps1 needs the KilnFW build's embedded .gz output from
# check_00_kilnfw_target_build.ps1) -- their own missing-prerequisite SKIP is
# a direct, expected consequence of -Fast itself, not a sign anything is
# broken (see CLAUDE.md's "-Fast red-run caveat"). That used to make a
# perfectly healthy -Fast run come back red with unexplained SKIPs unless the
# caller also passed -AllowSkips, which then ALSO silences a genuine SKIP
# from an unrelated cause -- too blunt.
#
# This env var is this runner's side of the contract: it is set only when
# -Fast is actually in effect, and only a check whose SKIP's sole cause is a
# missing phase-1 artifact should test it and, if set, print "SKIP-FAST: "
# instead of "SKIP: " for that specific reason (never for any other SKIP
# reason it has). Complete-CheckResult below keys off that literal string,
# not the exit code -- exit 3 stays the one reserved SKIP code for both. A
# SKIP-FAST is filed into its own bucket and never fails the run; a plain
# SKIP still does by default, exactly as before. A check must never print
# SKIP-FAST on its own initiative without checking this var, since a plain
# `-AllowFewerChecks`/no -Fast run must still see a real SKIP as fatal.
# ($env:KILNCTL_CHECKS_FAST and Clear-ChecksFastEnv are set up right after
# $ErrorActionPreference above, before any early `exit`, and cleared here and
# at every later exit point.)

if ($ListOnly) {
    Write-Host "$($checks.Count) check scripts discovered:"
    foreach ($c in $checks) {
        Write-Host "  $($c.FullName.Substring($repoRoot.Length + 1))"
    }
    Clear-ChecksFastEnv
    exit 0
}

# Check-result cache (tools/checkcache_lib.ps1): a PASS of an opt-in
# (`# checkcache: ok`) check is reused when the tree is clean and identical.
. (Join-Path $PSScriptRoot "checkcache_lib.ps1")
. (Join-Path $PSScriptRoot "main_baseline_lib.ps1")
$script:MainBaselineStart = $null
try { $script:MainBaselineStart = Get-MainBaselineStartState -RepoRoot $repoRoot } catch { }
$script:CheckCacheCtx = Initialize-CheckCache -RepoRoot $repoRoot -Fast:$Fast -NoCache:$NoCache -PcToolsPython $selfcheckPython

Write-Host ""
Write-Host "Running $($checks.Count) guard scripts from $repoRoot (parallel, throttle $MaxParallel)"
Write-Host "Run mode: $(if ($Fast) { 'fast' } else { 'full' })"
if ($script:CheckCacheCtx.Enabled) {
    Write-Host "Check cache: on (tree $($script:CheckCacheCtx.Tree.Substring(0,12)), $($script:CheckCacheCtx.Mode))" -ForegroundColor Cyan
} else {
    Write-Host "Check cache: off ($($script:CheckCacheCtx.Reason))" -ForegroundColor Yellow
}
Write-Host ""

$failed = @()
$passed = @()
$skipped = @()
$skippedFast = @()
$busy = @()

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
    # selfcheck.py needs the worktree's own src on the path (a borrowed
    # interpreter's venv may point at the main tree's). Set only around this
    # launch; the child inherits it at creation.
    $savedPyPath = $env:PYTHONPATH
    if ($Check.FullName -eq $SelfcheckPy) { $env:PYTHONPATH = (Join-Path $checkDir "src") }
    try {
        $proc = Start-Process -FilePath $exe -ArgumentList $procArgs -WorkingDirectory $checkDir `
            -RedirectStandardOutput $outFile -RedirectStandardError $errFile -PassThru -NoNewWindow
    } finally {
        $env:PYTHONPATH = $savedPyPath
    }

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
        Started = [DateTime]::UtcNow
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
        [void](Add-CheckCacheResult -Ctx $script:CheckCacheCtx -Rel $Running.Rel -Bucket "pass" `
            -DurationSec ([DateTime]::UtcNow - $Running.Started).TotalSeconds -OutputText $outText)
        return [pscustomobject]@{ Bucket = "pass"; Path = $Running.Rel }
    } elseif ($code -eq $SkipExitCode) {
        # SKIP-FAST is checked first: a check that prints it is asserting its
        # ONE stated reason is a direct, expected consequence of -Fast (see
        # the block above that sets KILNCTL_CHECKS_FAST) -- filed into its
        # own bucket rather than the ordinary "skip" one so it never counts
        # as an unexplained/fatal SKIP below.
        $reasonLine = ($outText -split "`r?`n" | Where-Object { $_ -match 'SKIP-FAST' } | Select-Object -First 1)
        if ($reasonLine) {
            Write-Host "  SKIP-FAST  $($Running.Rel)" -ForegroundColor Yellow
            return [pscustomobject]@{ Bucket = "skipfast"; Path = $Running.Rel; Reason = $reasonLine.Trim() }
        }
        $reasonLine = ($outText -split "`r?`n" | Where-Object { $_ -match 'SKIP' } | Select-Object -First 1)
        if (-not $reasonLine) {
            $reasonLine = "(no SKIP reason line found in output -- check violates the SKIP contract, see header)"
        }
        Write-Host "  SKIP  $($Running.Rel)" -ForegroundColor Yellow
        return [pscustomobject]@{ Bucket = "skip"; Path = $Running.Rel; Reason = $reasonLine.Trim() }
    } elseif (($script:gateWaitingRels -contains $Running.Rel) -and
              ($outText -match 'build gate: timed out after \d+s waiting for a (heavy|light)-lane build slot')) {
        # Load artifact, not a defect: the check never got to build. Own bucket.
        # Only a check that itself takes the gate ($gateWaitingPaths) can be
        # BUSY: a pytest/unit-test check whose FAILING output merely quotes the
        # message (a gate unit test, mcpkit/buildgate.py's own tests) stays a FAIL.
        Write-Host "  BUSY  $($Running.Rel) (build gate timeout -- not run)" -ForegroundColor Yellow
        return [pscustomobject]@{ Bucket = "busy"; Path = $Running.Rel; Code = $code; Output = $outText }
    } else {
        Write-Host "  FAIL  $($Running.Rel) (exit $code)" -ForegroundColor Red
        return [pscustomobject]@{ Bucket = "fail"; Path = $Running.Rel; Code = $code; Output = $outText }
    }
}

function Invoke-ChecksParallel {
    # PerCheckTimeoutSec > 0: a check still running after that many seconds is
    # killed (whole process tree, bounded) and filed as a FAIL, so one hung
    # child can never stall the run. 0 = no cap (phases 1 and 2).
    param($ChecksToRun, [int]$MaxParallel, [string]$RepoRoot, [string]$SelfcheckPy, [string]$SelfcheckPython, [string]$ScratchDir, [int]$SkipExitCode, [int]$PerCheckTimeoutSec = 0, [string[]]$UnthrottledPaths = @())

    $pending = New-Object System.Collections.Generic.Queue[object]
    # Gate-using checks go first so they join the gate queue early.
    # The count is bounded by the discovered check list (~20 today), never
    # open-ended.
    foreach ($c in $ChecksToRun) { if ($c.FullName -in $UnthrottledPaths) { $pending.Enqueue($c) } }
    foreach ($c in $ChecksToRun) { if ($c.FullName -notin $UnthrottledPaths) { $pending.Enqueue($c) } }
    $running = @()
    $results = @()

    while ($pending.Count -gt 0 -or $running.Count -gt 0) {
        # Gate-using checks are NOT exempt from $MaxParallel: a slot is held only
        # around the compile now, so such a check spends most of its life doing
        # real work (setup, link, running test exes) outside any slot. They are
        # merely queued FIRST (see $UnthrottledPaths above) so they join the
        # gate queue early.
        while ($pending.Count -gt 0 -and @($running).Count -lt $MaxParallel) {
            $c = $pending.Dequeue()
            $cRel = $c.FullName.Substring($RepoRoot.Length + 1)
            $hit = Find-CheckCacheHit -Ctx $script:CheckCacheCtx -Rel $cRel
            if ($hit) {
                Write-Host "  PASS  $cRel (cached $($hit.When.ToLocalTime().ToString('yyyy-MM-dd HH:mm')) from $($hit.Worktree))" -ForegroundColor Green
                [void]$script:CheckCacheCtx.Hits.Add($hit)
                $results += [pscustomobject]@{ Bucket = "pass"; Path = $cRel }
                continue
            }
            $started = Start-CheckAsync -Check $c -RepoRoot $RepoRoot -SelfcheckPy $SelfcheckPy -SelfcheckPython $SelfcheckPython -ScratchDir $ScratchDir
            $running += $started
        }
        Start-Sleep -Milliseconds 200
        $stillRunning = @()
        foreach ($r in $running) {
            if ($r.Proc.HasExited) {
                $results += Complete-CheckResult -Running $r -SkipExitCode $SkipExitCode
            } elseif ($PerCheckTimeoutSec -gt 0 -and ([DateTime]::UtcNow - $r.Started).TotalSeconds -gt $PerCheckTimeoutSec) {
                try {
                    $tk = Start-Process -FilePath taskkill -ArgumentList @('/PID', "$($r.Proc.Id)", '/T', '/F') -NoNewWindow -PassThru
                    if (-not $tk.WaitForExit(15000)) { try { $tk.Kill() } catch {} }
                } catch { }
                $partial = ""
                foreach ($f in @($r.OutFile, $r.ErrFile)) {
                    if (Test-Path $f) { $partial += (Get-Content -Raw -ErrorAction SilentlyContinue $f) }
                }
                Remove-Item -ErrorAction SilentlyContinue $r.OutFile, $r.ErrFile
                Write-Host "  FAIL  $($r.Rel) (killed: exceeded ${PerCheckTimeoutSec}s wall-clock cap)" -ForegroundColor Red
                $results += [pscustomobject]@{ Bucket = "fail"; Path = $r.Rel; Code = "timeout"; Output = ("TIMEOUT: exceeded ${PerCheckTimeoutSec}s per-check wall-clock cap; process tree killed.`n" + $partial) }
            } else {
                $stillRunning += $r
            }
        }
        $running = $stillRunning
    }
    return $results
}

# Phase 1 runs all three full target builds concurrently, on the guarantee
# the original strict-alphabetical serial order gave for free: every target
# build must FINISH (and publish its artifacts into the shared firmware/*/
# build/ directories) before anything that reads those artifacts runs (the
# stack-budget checks, compile_esp_backends.ps1/compile_pico_backends.ps1,
# check_saftyfw_task_count.ps1, check_recovery_image_size.ps1, etc.) -- those
# consumers only SKIP on a MISSING artifact, not a STALE one, so if they ran
# concurrently with a build in flight they could silently grade a leftover
# artifact from a previous run instead of this one, same failure shape
# check_00_kilnfw_target_build.ps1's own header documents.
#
# 2026-09-20 (docs/PICO_AUTO_UPDATE_PLAN.md): the KilnFW application build
# EMBED_FILES SaftyFW's slot images (SaftyFW_slotA.bin/slotB.bin) and fails
# at CMake configure time if either is missing or stale. This briefly split
# phase 1 into a serialized 1a (SaftyFW alone) / 1b (the two KilnFW builds)
# because check_00_kilnfw_target_build.ps1 read those slot bins out of the
# shared firmware/SaftyFW/build/ directory, the same "one build depending on
# another's published output" hazard the paragraph above describes for
# checkers reading a build's output. That split is no longer needed:
# check_00_kilnfw_target_build.ps1 now builds the SaftyFW slot images it
# embeds inside its OWN isolated checkbuild worktree rather than reading the
# shared firmware/SaftyFW/build/ directory, so it no longer depends on
# check_00_saftyfw_target_build.ps1 having run first or at all. All three
# target builds are independent again (separate build dirs, separate
# build_lock.ps1 mutex names) and run concurrently in one flat phase 1, same
# as before the 2026-09-20 pass; everything else (all lint/drift/mirror/
# host-test checks, which don't touch any target build's output) runs
# throttled in phase 2. Checks that DO share a build directory among
# themselves (e.g. two build_lock.ps1 users) still serialize correctly
# within phase 2 via that same named mutex -- they just queue instead of
# racing, exactly as build_lock.ps1's own header describes for two
# concurrent manual runs.
#
# The main tree's own `build_kilnfw()` MCP tool (tools/PcTools/src/mcpkit/
# workbench.py) is a SEPARATE path from this standing check and keeps its
# own SaftyFW-then-KilnFW build ordering: it reads bins out of the shared,
# non-isolated firmware/SaftyFW/build/ directory (there is no per-call
# checkbuild worktree for that tool), so the same real dependency applies
# there and that ordering was deliberately NOT reverted.
$buildChecks = $checks | Where-Object {
    $_.FullName -match 'check_00_kilnfw_target_build\.ps1$' -or
    $_.FullName -match 'check_00_saftyfw_target_build\.ps1$' -or
    $_.FullName -match 'check_00_kilnfw_recovery_target_build\.ps1$'
}
# check_ui_responsive_sweep.ps1 (2026-09-17, diagnosed): drives real headless
# Chrome over CDP with its own internal wall-clock timeouts
# (Page.loadEventFired waits, a 20s per-CDP-call cap). Confirmed via
# `tasklist` at the time of the 2026-09-17 misfires that this machine was
# running a dozen-plus concurrent chrome.exe from sibling agents/checks
# during Phase 2's default 8-way parallel throttle -- CPU/IO contention that
# a standalone rerun never sees, which is exactly why every misfire that day
# passed clean immediately afterward in isolation. The classifier gap that
# turned one such contention-caused timeout into a hard, unretried FAIL is
# fixed above (isTransientHarnessError() now matches
# CdpSession.waitForEvent()'s timeout string), but contention itself can
# still stretch this check's own internal timeouts even when every result is
# classified correctly, so it is pulled into its own phase here, run alone
# (-MaxParallel 1), never sharing a scheduling slot with up to 7 other
# concurrently-running checks the way Phase 2 would otherwise give it.
$restChecks = $checks | Where-Object {
    $_.FullName -notmatch 'check_00_kilnfw_target_build\.ps1$' -and
    $_.FullName -notmatch 'check_00_saftyfw_target_build\.ps1$' -and
    $_.FullName -notmatch 'check_00_kilnfw_recovery_target_build\.ps1$'
}
# check_web_commission_cdp_driver.ps1 (2026-09-21) joins it: same reason,
# same mechanism -- it runs four real headless-Chrome driver invocations
# over CDP, each with its own internal wall-clock timeouts, so it belongs
# on the serial side of this split rather than competing with up to seven
# other checks for the machine. Matched by an alternation here, not by two
# separate Where-Object passes, so a third Chrome-driving check is one
# pattern to add rather than another copy of this block.
$serialOnlyPattern = 'check_ui_responsive_sweep\.ps1$|check_web_commission_cdp_driver\.ps1$'
$uiSweepChecks = $restChecks | Where-Object {
    $_.FullName -match $serialOnlyPattern
}
$restChecks = $restChecks | Where-Object {
    $_.FullName -notmatch $serialOnlyPattern
}

# Each of the three phase-1 target-build checks (and the two
# build_host_tests.ps1 scripts) now enters tools/build_gate.ps1's
# machine-wide heavy-build gate (KILNCTL_BUILD_GATE_SLOTS, default 4) before
# its actual idf.py/ninja/cmake step, so "run concurrently" above is now
# "launch concurrently" -- the three builds may still serialize (or run two
# at a time) against that gate rather than all three hitting ninja's default
# job count simultaneously. This is deliberate: uncoordinated concurrent
# target builds across multiple agent sessions hard-froze this machine
# (Kernel-Power 41, no dump) five times in one week. No change to this
# script's own scheduling was needed -- the gate is inside the checks.
# Small single-exe compiles (check_recovery_*.ps1, check_commonfw_*.ps1) use the
# separate "light" lane (-Lane light, KILNCTL_LIGHT_GATE_SLOTS, default 4) so they
# never queue behind a 7-18 minute heavy holder.
# Checks that queue on the build gate (heavy or light lane) or call a
# build_host_tests.ps1: detected from their own source, not a hand list, so a
# new gated check is covered the moment it exists.
$gateWaitingPaths = @($checks | Where-Object {
    $_.Extension -eq ".ps1" -and
    (Select-String -LiteralPath $_.FullName -Pattern 'Enter-KilnBuildGate|Join-Path \$testDir [\x22]build_host_tests\.ps1' -Quiet)
} | ForEach-Object { $_.FullName })
# Same set as repo-relative paths, for Complete-CheckResult's BUSY rule.
$script:gateWaitingRels = @($gateWaitingPaths | ForEach-Object { $_.Substring($repoRoot.Length + 1) })
$results = @()
if ($buildChecks.Count -gt 0) {
    Write-Host "Phase 1/3: full target builds ($($buildChecks.Count))" -ForegroundColor Cyan
    $results += Invoke-ChecksParallel -ChecksToRun $buildChecks -MaxParallel ([Math]::Max(1, $buildChecks.Count)) `
        -RepoRoot $repoRoot -SelfcheckPy $selfcheckPy -SelfcheckPython $selfcheckPython -ScratchDir $scratchDir -SkipExitCode $SkipExitCode
}
if ($restChecks.Count -gt 0) {
    Write-Host "Phase 2/3: remaining checks ($($restChecks.Count))" -ForegroundColor Cyan
    $results += Invoke-ChecksParallel -ChecksToRun $restChecks -MaxParallel $MaxParallel -UnthrottledPaths $gateWaitingPaths `
        -RepoRoot $repoRoot -SelfcheckPy $selfcheckPy -SelfcheckPython $selfcheckPython -ScratchDir $scratchDir -SkipExitCode $SkipExitCode
}
if ($uiSweepChecks.Count -gt 0) {
    Write-Host "Phase 3/3: serial-only checks ($($uiSweepChecks.Count))" -ForegroundColor Cyan
    # 480s per check: above the checks' own wrapper caps (420s sweep, 180s
    # driver test) so their own, clearer FAIL messages normally win; this is
    # the backstop for a wrapper that itself hangs.
    $results += Invoke-ChecksParallel -ChecksToRun $uiSweepChecks -MaxParallel 1 -PerCheckTimeoutSec 480 `
        -RepoRoot $repoRoot -SelfcheckPy $selfcheckPy -SelfcheckPython $selfcheckPython -ScratchDir $scratchDir -SkipExitCode $SkipExitCode
}

Remove-Item -ErrorAction SilentlyContinue -Recurse -Force $scratchDir

foreach ($r in $results) {
    if ($r.Bucket -eq "pass") {
        $passed += $r.Path
    } elseif ($r.Bucket -eq "skipfast") {
        $skippedFast += [pscustomobject]@{ Path = $r.Path; Reason = $r.Reason }
    } elseif ($r.Bucket -eq "busy") {
        $busy += $r
    } elseif ($r.Bucket -eq "skip") {
        $skipped += [pscustomobject]@{ Path = $r.Path; Reason = $r.Reason }
    } else {
        $failed += [pscustomobject]@{ Path = $r.Path; Code = $r.Code; Output = $r.Output }
    }
}

$cacheStored = Save-CheckCache -Ctx $script:CheckCacheCtx

# Main-baseline report, recording and exit (tools/main_baseline_lib.ps1). Every
# exit after the results are known goes through here.
function Exit-WithBaseline {
    param([int]$Code)
    $mode = if ($Fast) { "fast" } else { "full" }
    $cur = @()
    foreach ($p in $passed) { $cur += [pscustomobject]@{ Path = $p; Status = "PASS" } }
    foreach ($s in $skippedFast) { $cur += [pscustomobject]@{ Path = $s.Path; Status = "SKIP-FAST" } }
    foreach ($s in $skipped) { $cur += [pscustomobject]@{ Path = $s.Path; Status = "SKIP"; Signature = (Get-MainFailureSignature -Reason ([string]$s.Reason)) } }
    foreach ($b in $busy) { $cur += [pscustomobject]@{ Path = $b.Path; Status = "BUSY" } }
    foreach ($f in $failed) { $cur += [pscustomobject]@{ Path = $f.Path; Status = "FAIL"; Signature = (Get-MainFailureSignature -Output ([string]$f.Output) -ExitCode $f.Code) } }
    # Only the failures that actually failed the run count against -FailOnlyOnNew.
    $cur2 = @($cur | Where-Object {
        ($_.Status -ne "SKIP" -or -not $AllowSkips) -and ($_.Status -ne "BUSY" -or -not $AllowBusy) })
    $rep = $null
    try {
        $rep = Show-MainBaselineSection -Current $cur2 -Mode $mode -RepoRoot $repoRoot
        if (-not $Only -and -not $Skip) {
            $rec = Test-MainBaselineRecordable -RepoRoot $repoRoot -Start $script:MainBaselineStart
            if ($rec.Ok) {
                $fp = Get-CheckCacheFingerprint -PcToolsPython $selfcheckPython
                $file = Write-MainBaseline -Dir (Get-MainBaselineDir) -Mode $mode -Commit $rec.Commit -Tree $rec.Tree -Fingerprint $fp -Results $cur -RepoRoot $repoRoot
                Write-Host "Recorded main baseline ($mode) for origin/main $($rec.Commit.Substring(0,10)): $file" -ForegroundColor Cyan
            } else {
                Write-Host "Main baseline not recorded: $($rec.Reason)" -ForegroundColor DarkGray
            }
        } else {
            Write-Host "Main baseline not recorded: -Only/-Skip run is partial" -ForegroundColor DarkGray
        }
    } catch {
        Write-Host "main baseline: skipped ($($_.Exception.Message))" -ForegroundColor Yellow
    }
    Clear-ChecksFastEnv
    if ($Code -ne 0 -and $FailOnlyOnNew) {
        if ($null -ne $rep -and $rep.Found -and $rep.Cmp.New.Count -eq 0) {
            Write-Host "" 
            Write-Host "!!! -FailOnlyOnNew: exiting 0 although $($rep.Cmp.Known.Count) check(s) FAILED -- every failure is KNOWN on origin/main. This is NOT a clean run. !!!" -ForegroundColor Yellow
            exit 0
        }
        Write-Host "-FailOnlyOnNew: exit code stays $Code (NEW failures present or no usable baseline)." -ForegroundColor Red
    }
    exit $Code
}
Write-Host ""
if ($script:CheckCacheCtx.Hits.Count -gt 0 -or $cacheStored -gt 0) {
    $savedSec = [int](($script:CheckCacheCtx.Hits | Measure-Object DurationSec -Sum).Sum)
    Write-Host "Check cache: $($script:CheckCacheCtx.Hits.Count) of the passed checks were cached hits (about ${savedSec}s of check time saved), $cacheStored new entr$(if ($cacheStored -eq 1) {'y'} else {'ies'}) stored." -ForegroundColor Cyan
    Write-Host ""
}

if ($skippedFast.Count -gt 0) {
    # Never fatal, regardless of -AllowSkips: each of these named its SKIP as
    # a direct, expected consequence of -Fast skipping a phase-1 build. Listed
    # separately so it reads as "expected", not folded into the ordinary
    # skip count nor silently absorbed into "passed".
    Write-Host "$($skippedFast.Count) check(s) skipped due to -Fast (non-fatal):" -ForegroundColor Yellow
    foreach ($s in $skippedFast) {
        Write-Host "  SKIP-FAST  $($s.Path)" -ForegroundColor Yellow
        Write-Host "             $($s.Reason)" -ForegroundColor Yellow
    }
    Write-Host ""
}

if ($skipped.Count -gt 0) {
    Write-Host "$($skipped.Count) check(s) SKIPPED (prerequisite absent -- not counted as passed):" -ForegroundColor Yellow
    foreach ($s in $skipped) {
        Write-Host "  SKIP  $($s.Path)" -ForegroundColor Yellow
        Write-Host "        $($s.Reason)" -ForegroundColor Yellow
    }
    Write-Host ""
}

if ($busy.Count -gt 0) {
    Write-Host "$($busy.Count) check(s) did NOT RUN -- build gate busy (load, not a tree defect; nothing was verified):" -ForegroundColor Yellow
    foreach ($b in $busy) {
        Write-Host "  BUSY  $($b.Path)" -ForegroundColor Yellow
        Write-Host "        rerun alone: tools/run_all_checks.ps1 -Fast -Only '$([regex]::Escape((Split-Path -Leaf $b.Path)))'" -ForegroundColor Yellow
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
    Write-Host "$($passed.Count) passed, $($skipped.Count) skipped ($($skippedFast.Count) due to -Fast), $($failed.Count) failed." -ForegroundColor Red
    Exit-WithBaseline 1
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
if ($busy.Count -gt 0 -and -not $AllowBusy) {
    Write-Host "$($passed.Count) passed, $($skipped.Count) skipped, $($busy.Count) BUSY (not run), $($failed.Count) failed." -ForegroundColor Red
    Write-Host "FAILED: $($busy.Count) check(s) never got a build-gate slot -- rerun them alone (see above); -AllowBusy only for a deliberately partial run." -ForegroundColor Red
    Exit-WithBaseline 1
}
if ($skipped.Count -gt 0 -and -not $AllowSkips) {
    Write-Host "$($passed.Count) passed, $($skipped.Count) skipped ($($skippedFast.Count) due to -Fast), $($failed.Count) failed." -ForegroundColor Red
    Write-Host "FAILED: $($skipped.Count) check(s) skipped and -AllowSkips was not passed -- a skip is not a pass." -ForegroundColor Red
    Exit-WithBaseline 1
}

Write-Host "$($passed.Count) passed, $($skipped.Count) skipped ($($skippedFast.Count) due to -Fast), $($failed.Count) failed." -ForegroundColor Green
Exit-WithBaseline 0
