# check_stack_margin_baseline.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers the stack-depth tripwire
# (tools/check_stack_margin_baseline.py) automatically, the same convention
# every other guard script in this repo follows. The actual logic (and its
# documented "what this can/cannot catch without hardware" limitations)
# lives in the Python script -- see that file's own top comment before
# editing behaviour here.
#
# This is scan #3 of the three the opus review of the display path
# specified (UI_PLAN.md, "Context rules for the display path" -- the
# stack-depth tripwire for e7b8efc's stack overflow). Unlike the other two
# scans (test_display_power_wiring.c sections 7-9, pure source-text scans
# that need no hardware at all), this one grades a CHECKED-IN capture of a
# real uxTaskGetStackHighWaterMark() reading -- it cannot itself take a new
# reading, so it is a gate on a stale number, not a live sensor.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_stack_margin_baseline.py"

if (-not (Test-Path $pyScript)) {
    throw "check_stack_margin_baseline.ps1: expected $pyScript not found -- has it moved?"
}

# Prefer the KilnFW host-test venv's python if one exists (same one
# build_host_tests.ps1 / run_pctools_tests use), falling back to whatever
# 'python' resolves to on PATH -- this script has no C to compile and no
# dependency beyond the standard library, so any Python 3 works.
$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "check_stack_margin_baseline.py exited $code -- see its output above for which baseline entry failed and why."
}
exit 0
