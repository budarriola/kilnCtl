# check_mcp_facade_coverage.ps1 -- thin wrapper so run_all_checks.ps1's
# check_*.ps1 glob discovers the MCP facade coverage check
# (tools/check_mcp_facade_coverage.py) automatically, the same convention
# every other guard script in this repo follows. The actual logic (and its
# documented "what counts as covered" definition) lives in the Python
# script -- see that file's own top comment before editing behaviour here.
#
# WHY THIS EXISTS. safety_get_diag was registered as a kilnctrl MCP tool but
# absent from mcp_facade.py's search taxonomy entirely, so kiln_find could
# never surface it -- nothing caught this until it was found by hand. This
# check makes that class of gap (and its mirror, a taxonomy entry naming a
# tool that no longer exists) a build-time failure for both kilnctrl and
# kicad, for every future tool addition/removal/rename.
$ErrorActionPreference = "Stop"

$repoRoot = Split-Path -Parent $PSScriptRoot
$pyScript = Join-Path $PSScriptRoot "check_mcp_facade_coverage.py"

if (-not (Test-Path $pyScript)) {
    throw "check_mcp_facade_coverage.ps1: expected $pyScript not found -- has it moved?"
}

$python = "python"
if (Get-Command python3 -ErrorAction SilentlyContinue) {
    $python = "python3"
}

& $python $pyScript
$code = $LASTEXITCODE
if ($code -ne 0) {
    throw "check_mcp_facade_coverage.py exited $code -- see its output above for which tool/taxonomy entry is the gap."
}
exit 0
