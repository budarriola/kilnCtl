# check_python_zero_caller_sweep.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers the Python-side zero-production-caller sweep
# (tools/check_python_zero_caller_sweep.py) automatically, the same
# convention every other guard script in this repo follows. The actual
# sweep logic, methodology and allowlists live in the Python script -- see
# that file's own top comment before editing behaviour here.
#
# WHY THIS EXISTS. docs/RELEASE_HARDENING_PLAN.md item 1's acceptance
# criteria called for a zero-production-caller ("dead code with no caller")
# sweep on both the C and Python sides. The C-side sweep found a real
# instance once (a live, wired-in HTTP handler nothing calls --
# web_auth_table_create_session). The Python-side sweep was run by hand
# twice (2026-09-17, 2026-09-20) but never made a standing check, so a
# newly introduced zero-caller function under tools/PcTools/src would not
# be caught again until someone re-ran it by hand. This makes it automatic.
$ErrorActionPreference = "Stop"

$pyScript = Join-Path $PSScriptRoot "check_python_zero_caller_sweep.py"

if (-not (Test-Path $pyScript)) {
    throw "check_python_zero_caller_sweep.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "check_python_zero_caller_sweep.py exited $code -- see its output above for which function(s) have no caller."
}
exit 0
