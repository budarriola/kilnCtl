# run_checks.ps1 -- runs every firmware/SimFW/tools/check_*.ps1 (plus the
# Python check_scenarios.py, via tools/PcTools's venv) in one shot, in the
# same spirit as this repo's other per-area check scripts
# (firmware/SaftyFW/tools/check_isolation.ps1, tools/check_no_duplicate_crc.ps1)
# -- CI-style gate, non-zero exit on the first failure category, clear
# per-check pass/fail banner.
#
# Checks run, in order (cheapest/most-structural first):
#   1. check_single_owner.ps1  -- DESIGN_NOTES.md sec 4's single-owner-per-peripheral doctrine
#   2. check_sim_purity.ps1    -- src/sim/ stays FreeRTOS/pico-sdk-free (DESIGN_NOTES.md sec 4.3/9, PLAN.md sec 13 item 1)
#   3. check_event_seq_monotonic.ps1 -- event ring seq stays monotonic for the boot lifetime (PROTOCOL.md, RESET_SIM)
#   4. check_scenarios.py      -- scenarios/*.yaml schema + guard-ID + fault-type validity
#
# Usage: powershell -File firmware\SimFW\tools\run_checks.ps1
$ErrorActionPreference = "Stop"

$toolsDir = $PSScriptRoot
$repoRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $toolsDir))
$pyExe = Join-Path $repoRoot "tools\PcTools\.venv\Scripts\python.exe"

$results = @()

function Run-Check {
    param([string]$Label, [scriptblock]$Body)
    Write-Host ""
    Write-Host "=== $Label ===" -ForegroundColor Cyan
    try {
        & $Body
        $script:results += [pscustomobject]@{ Label = $Label; Pass = $true }
    } catch {
        Write-Host $_.Exception.Message -ForegroundColor Red
        $script:results += [pscustomobject]@{ Label = $Label; Pass = $false }
    }
}

Run-Check "check_single_owner.ps1" { & (Join-Path $toolsDir "check_single_owner.ps1") }
Run-Check "check_sim_purity.ps1" { & (Join-Path $toolsDir "check_sim_purity.ps1") }

Run-Check "check_event_seq_monotonic.ps1" { & (Join-Path $toolsDir "check_event_seq_monotonic.ps1") }

Run-Check "check_scenarios.py" {
    if (Test-Path $pyExe) {
        & $pyExe (Join-Path $toolsDir "check_scenarios.py")
    } else {
        & python (Join-Path $toolsDir "check_scenarios.py")
    }
    if ($LASTEXITCODE -ne 0) {
        throw "check_scenarios.py exited $LASTEXITCODE"
    }
}

Write-Host ""
Write-Host "=== Summary ===" -ForegroundColor Cyan
$failCount = 0
foreach ($r in $results) {
    if ($r.Pass) {
        Write-Host "  PASS  $($r.Label)" -ForegroundColor Green
    } else {
        Write-Host "  FAIL  $($r.Label)" -ForegroundColor Red
        $failCount++
    }
}

if ($failCount -gt 0) {
    throw "$failCount of $($results.Count) SimFW check(s) failed"
}

Write-Host ""
Write-Host "All SimFW checks passed." -ForegroundColor Green
exit 0
