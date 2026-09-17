# test_check_stop_path_never_gated.ps1 -- negative test for
# tools/check_stop_path_never_gated.ps1 (docs/WEB_AUTH_PLAN.md section 9,
# the LCD Stop-is-never-gated property).
#
# Proves Invoke-StopPathGateScan (the PRODUCTION function, the same one
# check_stop_path_never_gated.ps1's main body calls) can actually detect a
# PIN gate placed in front of the Stop branch -- and does not merely pass
# vacuously on every input. Same dot-source pattern as
# test_check_route_tier_coverage.ps1: dot-source the check (which, per its
# own guard, only defines functions when dot-sourced) and call its exported
# function directly against synthetic scratch files.
#
#   1. A synthetic ui_home_fire_btn_cb() shaped like the real one (Stop
#      branch ungated + calls the stop confirm, Start branch gated) ->
#      passes clean.
#   2. The same function with a PIN gate call ADDED to the Stop branch ->
#      caught, StopBranchGated true.
#   3. The same function with the Stop branch's call to
#      ui_home_show_stop_confirm() REMOVED entirely (so the check cannot
#      coincidentally pass by matching nothing) -> caught,
#      StopBranchCallsStop false.
#   4. A synthetic function with NO gating anywhere (both branches ungated)
#      -> caught via StartBranchGated false, proving this check cannot be
#      satisfied merely by stripping all PIN gates rather than moving one.
#   5. The REAL production file, dot-sourced and scanned directly (not a
#      copy) -> passes clean, proving today's real check is not vacuous on
#      the actual tree.
#
# This does not touch the real repo tree; steps 1-4 are entirely synthetic
# scratch files, and step 5 only READS the real tree
# (Invoke-StopPathGateScan never writes).
#
# Usage: powershell -ExecutionPolicy Bypass -File App\test\test_check_stop_path_never_gated.ps1

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path
$checkScript = Join-Path $repoRoot "tools\check_stop_path_never_gated.ps1"

if (-not (Test-Path $checkScript)) {
    throw "test_check_stop_path_never_gated: expected $checkScript not found -- has it moved?"
}

# Dot-source: per check_stop_path_never_gated.ps1's own guard, this defines
# Invoke-StopPathGateScan/Get-CodeOnlyLines and runs nothing else (no
# -SourceFile was bound, and $MyInvocation.InvocationName is '.' during
# dot-sourcing).
. $checkScript

$scratchDir = Join-Path $env:TEMP "stop_path_gate_test_$PID"
if (-not (Test-Path $scratchDir)) {
    New-Item -ItemType Directory -Force -Path $scratchDir | Out-Null
}

$failures = @()

$goodBody = @'
void ui_home_fire_btn_cb(lv_event_t *e)
{
    (void)e;
    profile_exec_status_t st;
    profile_executor_get_status(&st);
    if (st.state == PROFILE_EXEC_RUNNING || st.state == PROFILE_EXEC_PAUSED) {
        /* Stop is never gated. */
        ui_home_show_stop_confirm();
    } else {
        ui_lcd_lock_run_gated("Enter PIN to start firing", LCD_PIN_ROLE_USER,
                               ui_home_show_start_confirm_gated_cb, NULL);
    }
}
'@

try {

# --- Assertion 1: the real shape -> passes clean. ---
$goodFile = Join-Path $scratchDir "good.c"
Set-Content -Path $goodFile -Value $goodBody -Encoding utf8
$r1 = Invoke-StopPathGateScan -SourceFile $goodFile
if ($r1.StopBranchGated -or (-not $r1.StopBranchCallsStop) -or (-not $r1.StartBranchGated)) {
    $failures += "Assertion 1 FAILED: correctly-shaped synthetic file scored StopBranchGated=$($r1.StopBranchGated) StopBranchCallsStop=$($r1.StopBranchCallsStop) StartBranchGated=$($r1.StartBranchGated), expected false/true/true."
} else {
    Write-Host "Assertion 1 OK: correctly-shaped synthetic file passes (Stop ungated, Start gated)."
}

# --- Assertion 2: add a PIN gate to the Stop branch -> caught. ---
$gatedStopBody = $goodBody -replace [regex]::Escape("ui_home_show_stop_confirm();"), "ui_lcd_lock_run_gated(`"Enter PIN to stop`", LCD_PIN_ROLE_USER, ui_home_show_stop_confirm_gated_cb, NULL);"
if ($gatedStopBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 2 did not match anything in `$goodBody"
}
$gatedStopFile = Join-Path $scratchDir "gated_stop.c"
Set-Content -Path $gatedStopFile -Value $gatedStopBody -Encoding utf8
$r2 = Invoke-StopPathGateScan -SourceFile $gatedStopFile
if (-not $r2.StopBranchGated) {
    $failures += "Assertion 2 FAILED: a PIN gate literally added to the Stop branch was NOT detected (StopBranchGated=false) -- the check is blind to the exact defect it exists to catch."
} else {
    Write-Host "Assertion 2 OK: a PIN gate added to the Stop branch is detected (StopBranchGated=true)."
}

# --- Assertion 3: remove the Stop branch's call entirely -> caught via
# StopBranchCallsStop, proving the check isn't just matching on ABSENCE of
# the gate pattern to mean "fine" regardless of what else changed. ---
$noStopCallBody = $goodBody -replace [regex]::Escape("ui_home_show_stop_confirm();"), "/* stop call removed */"
if ($noStopCallBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 3 did not match anything in `$goodBody"
}
$noStopCallFile = Join-Path $scratchDir "no_stop_call.c"
Set-Content -Path $noStopCallFile -Value $noStopCallBody -Encoding utf8
$r3 = Invoke-StopPathGateScan -SourceFile $noStopCallFile
if ($r3.StopBranchCallsStop) {
    $failures += "Assertion 3 FAILED: removing the Stop branch's call to ui_home_show_stop_confirm() was not detected (StopBranchCallsStop=true)."
} else {
    Write-Host "Assertion 3 OK: removing the Stop branch's stop-confirm call is detected (StopBranchCallsStop=false)."
}

# --- Assertion 4: strip gating from BOTH branches -> caught via
# StartBranchGated=false, proving the check cannot be defeated by simply
# deleting all PIN gates rather than moving one onto Stop. ---
$noGateAnywhereBody = $goodBody -replace [regex]::Escape('ui_lcd_lock_run_gated("Enter PIN to start firing", LCD_PIN_ROLE_USER,
                               ui_home_show_start_confirm_gated_cb, NULL);'), "ui_home_show_start_confirm();"
if ($noGateAnywhereBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 4 did not match anything in `$goodBody"
}
$noGateAnywhereFile = Join-Path $scratchDir "no_gate_anywhere.c"
Set-Content -Path $noGateAnywhereFile -Value $noGateAnywhereBody -Encoding utf8
$r4 = Invoke-StopPathGateScan -SourceFile $noGateAnywhereFile
if ($r4.StartBranchGated) {
    $failures += "Assertion 4 FAILED: removing the Start branch's PIN gate was not detected (StartBranchGated=true)."
} else {
    Write-Host "Assertion 4 OK: a file with no PIN gate anywhere is detected via StartBranchGated=false, not read as a clean pass."
}

# --- Assertion 5: the REAL production file -> passes clean. ---
$realFile = Join-Path $repoRoot "firmware\KilnFW\App\drivers\ui\ui_page_home_actions.c"
if (-not (Test-Path $realFile)) {
    $failures += "Assertion 5 FAILED: real file $realFile not found."
} else {
    $r5 = Invoke-StopPathGateScan -SourceFile $realFile
    if ($r5.StopBranchGated -or (-not $r5.StopBranchCallsStop) -or (-not $r5.StartBranchGated)) {
        $failures += "Assertion 5 FAILED: the REAL production file scored StopBranchGated=$($r5.StopBranchGated) StopBranchCallsStop=$($r5.StopBranchCallsStop) StartBranchGated=$($r5.StartBranchGated), expected false/true/true -- today's real check is either vacuous or the real file regressed."
    } else {
        Write-Host "Assertion 5 OK: the REAL production ui_home_fire_btn_cb() passes (Stop ungated, Start gated) -- not vacuous on the actual tree."
    }
}

} finally {
    Remove-Item -Path $scratchDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "test_check_stop_path_never_gated FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  - $f" -ForegroundColor Red
    }
    throw "$($failures.Count) assertion(s) failed -- see above."
}

Write-Host ""
Write-Host "test_check_stop_path_never_gated: all 5 assertions passed."
exit 0
