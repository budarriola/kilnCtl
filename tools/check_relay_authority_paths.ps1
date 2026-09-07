# check_relay_authority_paths.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers the relay-authority-bypass check
# (tools/check_relay_authority_paths.py) automatically, the same convention
# every other guard script in this repo follows. The actual logic (and the
# "what counts as a bypass" definition) lives in the Python script -- see
# that file's own top comment before editing behaviour here.
#
# WHY THIS EXISTS. tools/PcTools/TODO.md's completion checklist claims "No
# relay path here bypasses relay_authority_on_blocked()" -- true of the
# authority check itself (firmware-side, cannot be overridden from the PC),
# but tools/PcTools/scripts/current_sense_commissioning.py was found calling
# io.send(devices.io_set_relay(...)) directly instead of io.set_relay(...),
# which silently discards the firmware's refusal reply (owned-by-profile /
# safety-fault-asserted / OTA-in-progress) instead of surfacing it. This
# check makes that class of gap a build-time failure for every future relay-
# write call site under tools/PcTools/src and tools/PcTools/scripts.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_relay_authority_paths.py"

if (-not (Test-Path $pyScript)) {
    throw "check_relay_authority_paths.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "check_relay_authority_paths.py exited $code -- see its output above for the bypass call site."
}
exit 0
