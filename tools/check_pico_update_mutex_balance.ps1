# checkcache: ok
# check_pico_update_mutex_balance.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers tools/check_pico_update_mutex_balance.py
# automatically, the same convention every other Python-backed guard script
# in this repo follows (see check_cfgfs_never_gates_nvs.ps1). The actual
# scan and its rationale live in the Python script -- read that file's own
# top comment before editing behaviour here.
#
# WHY THIS EXISTS. Opus review, 2026-09-20: pico_auto_update_boot.c's
# attempt_update_embedded()/attempt_update_staged_locked() claim the single
# cross-processor update mutex and must release it on every early failure
# return before the relay task takes ownership -- pico_img_stage.h's own
# contract says staging is "not reentrant and not locked internally" (review
# finding F1). This check keeps that balance honest mechanically instead of
# by re-reading the file on every future edit.

$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_pico_update_mutex_balance.py"

if (-not (Test-Path $pyScript)) {
    throw "check_pico_update_mutex_balance.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "check_pico_update_mutex_balance.py exited $code -- see its output above for the offending line."
}
exit 0
