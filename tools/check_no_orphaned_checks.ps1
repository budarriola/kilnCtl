# check_no_orphaned_checks.ps1 -- closes the "orphaned negative test" class
# mechanically instead of by inspection.
#
# 2026-09-10 (opus review, round 2, defect A): run_all_checks.ps1's
# check_*.ps1 glob is discovery, so a new check_*.ps1 is always picked up
# automatically -- but test_*.ps1/test_*.py and check_*.py files are NOT
# matched by that glob at all; they only run if someone remembers to add an
# explicit entry (the hal-boundary negative test, the hwAbstraction scripts,
# selfcheck.py, and now the two SaftyFW regsp negative tests all needed one).
# Two of those were found completely unwired this round
# (test_regsp_margin_against_declared.py, test_regsp_stale_literal.py) --
# following check_saftyfw_task_count.py, found the same way in an earlier
# pass. "Someone remembers" has now failed at least three times.
#
# WHAT THIS CHECKS. Every check_*.ps1, check_*.py, test_*.ps1 and test_*.py
# under tools/ and firmware/*/test/ (the two places this repo's standalone
# guards and negative tests live -- NOT firmware/*/App/test, PcTools/tests or
# mykicadMcp/tests, which are pytest-collected suites reached by a completely
# different, already-mechanical mechanism: pytest's own test_*.py discovery
# via run_pctools_tests / the mykicadMcp test runner) must appear somewhere
# in run_all_checks.ps1's own source as a literal filename -- either because
# the check_*.ps1 glob finds it automatically, or because it is named in an
# explicit Join-Path/array entry the way the hal-boundary and SaftyFW
# negative tests are. A file that appears nowhere in that source text cannot
# be reachable by any mechanism run_all_checks.ps1 has, discovery or
# hand-wired.
#
# EXCLUSIONS, and why: vendored third-party trees (components/lvgl -- not
# this project's code, has its own upstream test story) and build output.
#
# This is intentionally a text-presence check against run_all_checks.ps1's
# own source, not a re-implementation of its discovery logic -- simple
# enough that a false negative here is easy to see is wrong. A file named in
# a comment counts as "wired" too; that is a smaller failure mode (someone
# writes about wiring it in and never does) which check_saftyfw_task_count.py
# demonstrated does eventually get caught by inspection, at real cost.

$ErrorActionPreference = "Stop"
$repoRoot = Split-Path -Parent $PSScriptRoot
$runAllChecks = Join-Path $repoRoot "tools\run_all_checks.ps1"

if (-not (Test-Path $runAllChecks)) {
    Write-Host "check_no_orphaned_checks: FAIL -- $runAllChecks not found"
    exit 1
}
# 2026-09-18 (release-gate vacuity audit, tenth pass): the "is it named in
# run_all_checks.ps1?" fallback below must consult run_all_checks.ps1's CODE,
# not its prose. That file's header narrates past orphan incidents by
# filename -- e.g. line 311's "the same 'orphaned negative test' shape
# check_saftyfw_task_count.py was found in" -- and a bare -match over the
# raw text treats such a mention as WIRING. A file that nothing executes is
# then reported as covered precisely BECAUSE this repo documented that it
# once went uncovered. Confirmed live: check_saftyfw_task_count.py and
# check_saftyfw_task_stack_budgets.py are each named in run_all_checks.ps1
# only from comment lines. Both are genuinely reached today (via their
# code-referencing wrappers, handled above), so stripping comments here
# flags nothing correct -- it removes a false absolution, not real coverage.
$runAllChecksCode = @(Get-Content -Path $runAllChecks | Where-Object { $_ -notmatch '^\s*#' }) -join "`n"

$searchRoots = @(
    (Join-Path $repoRoot "tools"),
    (Join-Path $repoRoot "firmware")
)

$candidates = Get-ChildItem -Path $searchRoots -Recurse -File -Include "check_*.ps1", "check_*.py", "test_*.ps1", "test_*.py" |
    Where-Object {
        $_.FullName -notmatch '\\build\\' -and
        $_.FullName -notmatch '\\node_modules\\' -and
        $_.FullName -notmatch '\\\.[^\\]+\\' -and
        # Pytest-collected suites: reached by pytest's own discovery, not by
        # run_all_checks.ps1 at all -- a different, already-mechanical path.
        $_.FullName -notmatch '\\PcTools\\tests\\' -and
        $_.FullName -notmatch '\\mykicadMcp\\tests\\' -and
        # Vendored third-party code -- not this project's, has its own
        # upstream test/CI story we do not own.
        $_.FullName -notmatch '\\components\\lvgl\\' -and
        # tools/PcTools/scripts/check_chip_partition_table.py is a live-board
        # diagnostic (docs/audits/check_independence_2026-09-07.md: "live
        # board read via debug interface"), not a standalone repo-state
        # guard -- it needs a connected, powered board and cannot run as
        # part of an unattended run_all_checks.ps1 pass. Documented
        # exclusion, not an orphan. 2026-09-10: narrowed from excluding the
        # WHOLE \PcTools\scripts\ directory to excluding just this one
        # file -- the wholesale directory exclusion silently orphaned any
        # FUTURE standalone check_*/test_* dropped anywhere else in that
        # directory (confirmed: as of this pass it is still the only
        # check_*/test_*-shaped file there, so narrowing changes nothing
        # today, but it stops being a blind spot for the next one).
        $_.FullName -notmatch '\\PcTools\\scripts\\check_chip_partition_table\.py$'
    } |
    Sort-Object FullName

$orphans = @()
foreach ($f in $candidates) {
    if ($f.Extension -eq ".ps1" -and $f.Name -like "check_*.ps1") {
        # Covered unconditionally by run_all_checks.ps1's own
        # `Get-ChildItem -Filter "check_*.ps1" -Recurse` glob.
        continue
    }
    if ($f.Extension -eq ".py" -and $f.Name -like "check_*.py") {
        # This repo's standing pattern for a Python check (selfcheck.py
        # aside) is a thin same-directory check_*.ps1 wrapper -- e.g.
        # check_saftyfw_task_stack_budgets.ps1 wrapping the .py of the same
        # base name -- which the glob above already reaches. Treat that
        # wrapper's presence as coverage rather than demanding the .py
        # filename appear in run_all_checks.ps1's own source too.
        #
        # 2026-09-18 (release-gate vacuity audit, tenth pass): the wrapper
        # must actually REFERENCE the .py in non-comment code, not merely
        # exist. Accepting mere existence meant a same-named wrapper that
        # never invokes its .py would leave that .py orphaned -- executed by
        # nothing -- while this check reported it covered. That is precisely
        # the defect check_saftyfw_task_count.py shipped in (see that
        # wrapper's own header: the .py existed, a sibling's comment claimed
        # it "fails loud", and it never ran), reproduced one level up in the
        # guard meant to catch it. Comment lines are stripped first, so a
        # wrapper that only NAMES its .py in prose does not count as
        # invoking it. All 14 wrapper/.py pairs in the tree satisfy this
        # today, so this tightens the rule without flagging correct code.
        $wrapper = Join-Path $f.DirectoryName ($f.BaseName + ".ps1")
        if (Test-Path $wrapper) {
            $wrapperCode = @(Get-Content -Path $wrapper | Where-Object { $_ -notmatch '^\s*#' })
            if (($wrapperCode -join "`n") -match [regex]::Escape($f.Name)) {
                continue
            }
        }
    }
    if ($runAllChecksCode -notmatch [regex]::Escape($f.Name)) {
        $orphans += $f.FullName.Substring($repoRoot.Length + 1)
    }
}

if ($orphans.Count -gt 0) {
    Write-Host ""
    Write-Host "FAILED: found $($orphans.Count) check/test file(s) under tools/ or" -ForegroundColor Red
    Write-Host "        firmware/*/test/ that run_all_checks.ps1 never mentions by name --" -ForegroundColor Red
    Write-Host "        neither its check_*.ps1 glob nor any explicit wiring reaches them," -ForegroundColor Red
    Write-Host "        so they never run as part of the suite:" -ForegroundColor Red
    foreach ($o in $orphans) {
        Write-Host "  $o" -ForegroundColor Red
    }
    Write-Host ""
    Write-Host "        Wire each one into run_all_checks.ps1 (see the hal-boundary or" -ForegroundColor Red
    Write-Host "        SaftyFW-regsp-tests blocks there for the pattern), or delete it if" -ForegroundColor Red
    Write-Host "        it is genuinely dead." -ForegroundColor Red
    exit 1
}

Write-Host "check_no_orphaned_checks: PASS ($($candidates.Count) check/test files under tools/ and firmware/*/test/ all named in run_all_checks.ps1)"
exit 0
