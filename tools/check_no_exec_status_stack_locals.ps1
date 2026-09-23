# check_no_exec_status_stack_locals.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers the profile_exec_status_t stack-local guard
# (tools/check_no_exec_status_stack_locals.py) automatically, the same
# convention every other guard script in this repo follows. The actual
# logic (and the "what counts as a violation" definition) lives in the
# Python script -- see that file's own top comment before editing behaviour
# here.
#
# WHY THIS EXISTS. profile_exec_status_t is 1384 bytes
# (firmware/KilnFW/App/drivers/control/profile_executor.h) and its own doc
# comment warns against putting a copy on a tight-stack task's stack. A
# 2026-09-22 audit found a new stack local of this type in a persist-layer
# function reachable from the httpd task, plus several more under
# drivers/http -- all converted to heap-alloc (matching safety_cfg_http.c's
# precedent) or to the narrower profile_executor_get_active_id() accessor.
# This check makes a reintroduction of that class of stack local a
# build-time failure under drivers/http and drivers/persist.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_no_exec_status_stack_locals.py"

if (-not (Test-Path $pyScript)) {
    throw "check_no_exec_status_stack_locals.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "check_no_exec_status_stack_locals.py exited $code -- see its output above for the stack-local site."
}
exit 0
