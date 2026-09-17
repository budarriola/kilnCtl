# check_recovery_image_size.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers the recovery-image size gate
# (tools/check_recovery_image_size.py) automatically, the same convention
# every other guard script in this repo follows. The actual logic (why this
# exists, what it does and does not check yet, and where the byte bound and
# the image path come from) lives in the Python script's own module
# docstring -- read that before editing behaviour here.
#
# Closes the gap measured into existence in docs/OTA_SINGLE_SLOT_PLAN.md:
# ESP-IDF's own check_sizes.py only hard-fails a build when a binary is too
# large for EVERY matching `app`-type partition, so an oversized `recovery`
# image builds clean (exit 0, just a warning) as long as it still fits the
# much larger `app` slot. This script is the independent, no-effort-required
# backstop against exactly that.
#
# Exit-code contract (run_all_checks.ps1's SKIP/PASS/FAIL contract):
#   0 -- PASS. Recovery partition and image both exist; image fits.
#   3 -- SKIP. The recovery partition row and/or the image do not exist yet
#        (both are still-planned work as of 2026-09-16 -- see the .py
#        docstring). The .py always prints a line containing "SKIP" and the
#        specific reason.
#   anything else -- FAIL. The image exists and overflows its partition, or
#        the table itself could not be parsed.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_recovery_image_size.py"

if (-not (Test-Path $pyScript)) {
    throw "check_recovery_image_size.ps1: expected $pyScript not found -- has it moved?"
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
if ($code -eq 3) {
    # SKIP: the .py already printed the "SKIP: ..." line to stdout above --
    # just propagate the reserved skip exit code to run_all_checks.ps1.
    exit 3
}
throw "check_recovery_image_size.py exited $code -- see its output above for why the recovery image failed to fit its partition."
