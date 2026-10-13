# checkcache: ok
# check_cfgfs_never_gates_nvs.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers tools/check_cfgfs_never_gates_nvs.py
# automatically, the same convention every other Python-backed guard script
# in this repo follows (see check_relay_authority_paths.ps1). The actual
# scan and its rationale live in the Python script -- read that file's own
# top comment before editing behaviour here.
#
# WHY THIS EXISTS. docs/RELEASE_HARDENING.md section 10: the `cfg`
# partition is parked (inert on every board actually running today) rather
# than finished, on the strength of NVS staying authoritative and
# unconditional. This check is the mechanical guard that keeps that true --
# it fails the build the moment a future edit makes any `*_cfg_fs_save()`
# call site skip or condition the NVS write on the file write's result.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_cfgfs_never_gates_nvs.py"

if (-not (Test-Path $pyScript)) {
    throw "check_cfgfs_never_gates_nvs.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "check_cfgfs_never_gates_nvs.py exited $code -- see its output above for the offending call site."
}
exit 0
