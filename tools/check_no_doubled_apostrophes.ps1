# check_no_doubled_apostrophes.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers the doubled-apostrophe artifact check
# (tools/check_no_doubled_apostrophes.py) automatically, the same convention
# every other guard script in this repo follows (see
# check_no_handler_direct_driver_calls.ps1). The actual logic lives in the
# Python script -- see that file's own top comment before editing behaviour
# here.
#
# WHY THIS EXISTS. Commit ee0b8754 hand-fixed 39 doubled-apostrophe artifacts
# (`doesn''t`, `` `file.h`''s ``) in firmware/KilnFW/TODO.md, left by agents
# writing prose through PowerShell single-quoted strings. This check makes a
# reintroduced instance a build-time failure instead of a silent doc defect.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_no_doubled_apostrophes.py"

if (-not (Test-Path $pyScript)) {
    throw "check_no_doubled_apostrophes.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "check_no_doubled_apostrophes.py exited $code -- see its output above for the offending file:line."
}
exit 0
