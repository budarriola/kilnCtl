# test_check_stop_path_requires_pin.ps1 -- negative test for
# tools/check_stop_path_requires_pin.ps1 (owner decision 2026-09-28: "stop
# needs login. there is an estop button" -- LCD Stop now requires the PIN,
# the same as Start).
#
# This test used to be test_check_stop_path_never_gated.ps1 and asserted the
# OPPOSITE shape (Stop branch ungated). It has been renamed and every
# assertion inverted to match the reversal.
#
# Proves Invoke-StopPathGateScan (the PRODUCTION function, the same one
# check_stop_path_requires_pin.ps1's main body calls) can actually detect a
# PIN gate MISSING from the Stop branch -- and does not merely pass
# vacuously on every input. Same dot-source pattern as
# test_check_route_tier_coverage.ps1: dot-source the check (which, per its
# own guard, only defines functions when dot-sourced) and call its exported
# function directly against synthetic scratch files.
#
#   1. A synthetic ui_home_fire_btn_cb() shaped like the real one (both
#      branches gated, Stop branch reaches the gated stop-confirm callback)
#      -> passes clean.
#   2. The same function with the Stop branch's PIN gate REMOVED (calling
#      ui_home_show_stop_confirm() directly again, the pre-reversal shape)
#      -> caught, StopBranchGated false.
#   3. The same function with the Stop branch's reference to the stop
#      confirm REMOVED entirely (so the check cannot coincidentally pass by
#      matching nothing) -> caught, StopBranchCallsStop false.
#   4. A synthetic function with NO gating anywhere (both branches ungated)
#      -> caught via StartBranchGated false, proving this check cannot be
#      satisfied merely by having removed all PIN gates.
#   5. The REAL production file, dot-sourced and scanned directly (not a
#      copy) -> passes clean, proving today's real check is not vacuous on
#      the actual tree.
#   6. A Stop branch that calls ui_home_show_stop_confirm() directly (no
#      PIN) next to a stray LCD_PIN_ROLE_USER token -> caught via
#      StopBranchDirectConfirm/StopBranchConfirmViaGate. The first version of
#      this check PASSED this shape (2026-09-28 review of 3e7bb20b).
#   7. A Stop branch that runs a gate with some other callback and only then
#      calls ui_home_show_stop_confirm() directly (confirm opens regardless
#      of the PIN outcome) -> caught the same way.
#
# This does not touch the real repo tree; steps 1-4 are entirely synthetic
# scratch files, and step 5 only READS the real tree
# (Invoke-StopPathGateScan never writes).
#
# Usage: powershell -ExecutionPolicy Bypass -File App\test\test_check_stop_path_requires_pin.ps1

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path
$checkScript = Join-Path $repoRoot "tools\check_stop_path_requires_pin.ps1"

if (-not (Test-Path $checkScript)) {
    throw "test_check_stop_path_requires_pin: expected $checkScript not found -- has it moved?"
}

# Dot-source: per check_stop_path_requires_pin.ps1's own guard, this defines
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
        ui_lcd_lock_run_gated("Enter PIN to stop firing", LCD_PIN_ROLE_USER,
                               ui_home_show_stop_confirm_gated_cb, NULL);
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
if ((-not $r1.StopBranchGated) -or (-not $r1.StopBranchCallsStop) -or (-not $r1.StartBranchGated) -or (-not $r1.StopBranchConfirmViaGate) -or $r1.StopBranchDirectConfirm) {
    $failures += "Assertion 1 FAILED: correctly-shaped synthetic file scored StopBranchGated=$($r1.StopBranchGated) StopBranchCallsStop=$($r1.StopBranchCallsStop) StartBranchGated=$($r1.StartBranchGated), expected true/true/true."
} else {
    Write-Host "Assertion 1 OK: correctly-shaped synthetic file passes (Stop gated, Start gated)."
}

# --- Assertion 2: remove the Stop branch's PIN gate (revert to the old,
# pre-reversal ungated shape) -> caught. ---
$ungatedStopBody = $goodBody -replace [regex]::Escape('ui_lcd_lock_run_gated("Enter PIN to stop firing", LCD_PIN_ROLE_USER,
                               ui_home_show_stop_confirm_gated_cb, NULL);'), "ui_home_show_stop_confirm();"
if ($ungatedStopBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 2 did not match anything in `$goodBody"
}
$ungatedStopFile = Join-Path $scratchDir "ungated_stop.c"
Set-Content -Path $ungatedStopFile -Value $ungatedStopBody -Encoding utf8
$r2 = Invoke-StopPathGateScan -SourceFile $ungatedStopFile
if ($r2.StopBranchGated) {
    $failures += "Assertion 2 FAILED: removing the Stop branch's PIN gate was NOT detected (StopBranchGated=true) -- the check is blind to the exact defect it exists to catch (a regression back to the pre-2026-09-28 ungated Stop)."
} else {
    Write-Host "Assertion 2 OK: removing the Stop branch's PIN gate is detected (StopBranchGated=false)."
}

# --- Assertion 3: remove the Stop branch's reference to the stop confirm
# entirely -> caught via StopBranchCallsStop, proving the check isn't just
# matching on PRESENCE of the gate pattern to mean "fine" regardless of
# whether Stop is still reachable at all. ---
$noStopCallBody = $goodBody -replace [regex]::Escape("ui_home_show_stop_confirm_gated_cb"), "/* stop callback removed */"
if ($noStopCallBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 3 did not match anything in `$goodBody"
}
$noStopCallFile = Join-Path $scratchDir "no_stop_call.c"
Set-Content -Path $noStopCallFile -Value $noStopCallBody -Encoding utf8
$r3 = Invoke-StopPathGateScan -SourceFile $noStopCallFile
if ($r3.StopBranchCallsStop) {
    $failures += "Assertion 3 FAILED: removing the Stop branch's reference to ui_home_show_stop_confirm_gated_cb was not detected (StopBranchCallsStop=true)."
} else {
    Write-Host "Assertion 3 OK: removing the Stop branch's stop-confirm reference is detected (StopBranchCallsStop=false)."
}

# --- Assertion 4: strip gating from BOTH branches -> caught via
# StartBranchGated=false, proving the check cannot be defeated by simply
# deleting all PIN gates. ---
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
    if ((-not $r5.StopBranchGated) -or (-not $r5.StopBranchCallsStop) -or (-not $r5.StartBranchGated) -or (-not $r5.StopBranchConfirmViaGate) -or $r5.StopBranchDirectConfirm) {
        $failures += "Assertion 5 FAILED: the REAL production file scored StopBranchGated=$($r5.StopBranchGated) StopBranchCallsStop=$($r5.StopBranchCallsStop) StartBranchGated=$($r5.StartBranchGated), expected true/true/true -- today's real check is either vacuous or the real file regressed."
    } else {
        Write-Host "Assertion 5 OK: the REAL production ui_home_fire_btn_cb() passes (Stop gated, Start gated) -- not vacuous on the actual tree."
    }
}

# --- Assertion 6: direct ungated confirm plus a stray gate token -> caught.
$sneakyBody = $goodBody -replace [regex]::Escape('ui_lcd_lock_run_gated("Enter PIN to stop firing", LCD_PIN_ROLE_USER,
                               ui_home_show_stop_confirm_gated_cb, NULL);'), "lcd_pin_role_t unused_role = LCD_PIN_ROLE_USER; (void)unused_role; ui_home_show_stop_confirm();"
if ($sneakyBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 6 did not match anything in `$goodBody"
}
$sneakyFile = Join-Path $scratchDir "sneaky_stop.c"
Set-Content -Path $sneakyFile -Value $sneakyBody -Encoding utf8
$r6 = Invoke-StopPathGateScan -SourceFile $sneakyFile
if ((-not $r6.StopBranchDirectConfirm) -or $r6.StopBranchConfirmViaGate) {
    $failures += "Assertion 6 FAILED: a direct ungated ui_home_show_stop_confirm() beside a stray LCD_PIN_ROLE_USER token scored StopBranchDirectConfirm=$($r6.StopBranchDirectConfirm) StopBranchConfirmViaGate=$($r6.StopBranchConfirmViaGate), expected true/false."
} else {
    Write-Host "Assertion 6 OK: a direct confirm call beside a stray gate token is detected."
}

# --- Assertion 7: gate some other action, then confirm directly -> caught.
$gateThenDirectBody = $goodBody -replace [regex]::Escape('ui_home_show_stop_confirm_gated_cb, NULL);'), "some_other_cb, NULL); ui_home_show_stop_confirm();"
if ($gateThenDirectBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 7 did not match anything in `$goodBody"
}
$gateThenDirectFile = Join-Path $scratchDir "gate_then_direct.c"
Set-Content -Path $gateThenDirectFile -Value $gateThenDirectBody -Encoding utf8
$r7 = Invoke-StopPathGateScan -SourceFile $gateThenDirectFile
if ((-not $r7.StopBranchDirectConfirm) -or $r7.StopBranchConfirmViaGate) {
    $failures += "Assertion 7 FAILED: gate-other-callback-then-direct-confirm scored StopBranchDirectConfirm=$($r7.StopBranchDirectConfirm) StopBranchConfirmViaGate=$($r7.StopBranchConfirmViaGate), expected true/false."
} else {
    Write-Host "Assertion 7 OK: a gate on another callback followed by a direct confirm is detected."
}

} finally {
    Remove-Item -Path $scratchDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "test_check_stop_path_requires_pin FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  - $f" -ForegroundColor Red
    }
    throw "$($failures.Count) assertion(s) failed -- see above."
}

Write-Host ""
Write-Host "test_check_stop_path_requires_pin: all 7 assertions passed."
exit 0
