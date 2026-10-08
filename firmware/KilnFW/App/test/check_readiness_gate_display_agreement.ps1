# checkcache: ok
# check_readiness_gate_display_agreement.ps1 -- thin wrapper so
# run_all_checks.ps1's check_*.ps1 glob picks up
# check_readiness_gate_display_agreement.py.
#
# The rationale, the bug class it guards against and its stated limits all
# live in the Python script's module docstring. Short version: since
# 2026-09-09 four /api/readiness items are a real firing interlock
# (App/drivers/safety/readiness_gate.h), so the gate and the page must decide
# each of those items with the SAME shared predicate -- otherwise the operator
# sees one thing and the board does another.
#
# Never skips: both files it reads are checked into the tree, so an absent one
# is itself the failure (the interlock lost its decision source, or the page
# lost the item an operator was just refused on).
#
# Usage:
#   powershell -ExecutionPolicy Bypass -File firmware\KilnFW\App\test\check_readiness_gate_display_agreement.ps1
#   ... -NegativeTestDropPredicate readiness_estop_verification_status
#       # proves the check can fail: pretends the gate stopped consulting that
#       # predicate, which must report FAIL, not OK

param(
    [string]$NegativeTestDropPredicate
)

$ErrorActionPreference = "Stop"
$script = Join-Path $PSScriptRoot "check_readiness_gate_display_agreement.py"
if (-not (Test-Path $script)) {
    Write-Host "check_readiness_gate_display_agreement: FAIL -- $script is missing"
    exit 1
}

$argv = @($script)
if ($NegativeTestDropPredicate) {
    $argv += @("--negative-test-drop-predicate", $NegativeTestDropPredicate)
}

& python @argv
exit $LASTEXITCODE
