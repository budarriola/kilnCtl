# check_profiles_capacity.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers the profile-store capacity gate
# (tools/check_profiles_capacity.py) automatically, the same convention
# every other guard script in this repo follows (see
# check_recovery_image_size.ps1, its direct precedent). The actual logic
# (why this exists, where the bounds and per-item costs come from) lives in
# the Python script's own module docstring -- read that before editing
# behaviour here.
#
# docs/PROFILE_SLOTS_100.md section 5, task 4: lands while
# PROFILES_MAX_COUNT is still 8 and `cfg` is still 0x80000, so this gate is
# already live and enforcing before task 6 raises the slot count.
#
# Exit-code contract (run_all_checks.ps1's PASS/FAIL contract; this check
# has no legitimate SKIP state -- partitions.csv and the source files it
# reads always exist in this tree):
#   0 -- PASS. Both profiles_nvs and cfg worst-case totals fit.
#   anything else -- FAIL. See stderr for which partition overflowed, or
#        which input file could not be found/parsed.
$ErrorActionPreference = "Stop"

$pyScript = Join-Path $PSScriptRoot "check_profiles_capacity.py"

if (-not (Test-Path $pyScript)) {
    throw "check_profiles_capacity.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -eq 0) {
    exit 0
}
throw "check_profiles_capacity.py exited $code -- see its output above for which partition overflows."
