# check_no_handler_direct_driver_calls.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers the HTTP-handler direct-driver-call check
# (tools/check_no_handler_direct_driver_calls.py) automatically, the same
# convention every other guard script in this repo follows (see
# check_relay_authority_paths.ps1). The actual logic lives in the Python
# script -- see that file's own top comment before editing behaviour here.
#
# WHY THIS EXISTS. docs/HTTP_HANDLER_OWNERSHIP.md's Batch A found and
# fixed four HTTP-handler call sites reading hardware directly
# (MAX31856_read_all()/kiln_io_read()) instead of through their owner-task
# accessor. This check makes a reintroduced direct call a build-time failure
# instead of a silent regression.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_no_handler_direct_driver_calls.py"

if (-not (Test-Path $pyScript)) {
    throw "check_no_handler_direct_driver_calls.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "check_no_handler_direct_driver_calls.py exited $code -- see its output above for the bypass call site."
}
exit 0
