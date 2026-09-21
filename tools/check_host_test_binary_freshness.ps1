# check_host_test_binary_freshness.ps1 -- mechanical guard against measuring
# a host-test executable that is older than the sources it was built from.
#
# Background: docs/audits/review_sim_fuzzy_commits_2026-09-13.md.
# build_host_tests.ps1 rebuilt kilnctl_sim_fuzzy_closedloop.exe mid a
# negative test for ed854ac5 (source deliberately sabotaged), the source was
# hand-restored, and `git diff` came back empty -- but the poisoned .exe
# sitting in firmware/KilnFW/App/test/build/ was never rebuilt. ba230bca then
# read that prebuilt binary directly instead of rebuilding, and measured six
# numbers from sabotaged code that reached the project owner as a verdict
# before the audit caught it. An empty `git diff` proves the source is
# restored; it says nothing about build artifacts already on disk.
#
# This check does NOT rebuild anything and does NOT re-run any test -- it
# only compares mtimes: every host-test .exe already sitting in the KilnFW
# and SaftyFW host-test build directories against every .c/.h source under
# that build's known source roots. A source newer than its exe is exactly
# the shape of the incident above (an edit or a hand-restore landing after
# the last build) and fails the check loudly, naming both files.
#
# Exit-code contract (run_all_checks.ps1's SKIP/PASS/FAIL contract):
#   0 -- PASS. No .exe found older than its own sources (or no .exe files
#        exist yet at all in a given build dir -- see SKIP below for the
#        "build dir absent entirely" case, which is different).
#   1 -- FAIL. At least one stale .exe found; the Python helper's own output
#        names the exe and the newer source file.
#   3 -- SKIP. Neither the KilnFW nor the SaftyFW host-test build directory
#        exists yet (nothing has been built on this machine/worktree). The
#        script prints a line containing "SKIP" and the specific reason.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_host_test_binary_freshness.py"

if (-not (Test-Path $pyScript)) {
    throw "check_host_test_binary_freshness.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript $repoRoot
$code = $LASTEXITCODE
if ($code -eq 3) {
    # SKIP: the .py already printed the "SKIP: ..." line to stdout above.
    exit 3
}
if ($code -ne 0) {
    throw "check_host_test_binary_freshness.py exited $code -- see its output above for which binary is stale and why."
}
exit 0
