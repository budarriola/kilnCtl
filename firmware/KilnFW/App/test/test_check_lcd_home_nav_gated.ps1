# test_check_lcd_home_nav_gated.ps1 -- negative test for
# tools/check_lcd_home_nav_gated.ps1 (owner decision 2026-09-28: only the
# LCD home/dashboard VIEW stays reachable without a PIN).
#
# Proves Invoke-HomeNavGateScan (the PRODUCTION function, the same one
# check_lcd_home_nav_gated.ps1's main body calls) can actually detect a
# missing gate on the Config-hub/profile-picker nav callbacks, and a
# wrongly-added gate on the credential-reset gesture -- not merely pass
# vacuously. Same dot-source pattern as test_check_stop_path_requires_pin.ps1.
#
#   1. A synthetic file shaped correctly (menu nav gated, profile btn gated,
#      auth-reset callback NOT gated) -> passes clean.
#   2. The same file with the menu nav's PIN gate REMOVED -> caught,
#      MenuNavGated false.
#   3. The same file with the profile button's PIN gate REMOVED -> caught,
#      ProfileBtnGated false.
#   4. The same file with a PIN gate ADDED to the credential-reset callback
#      -> caught, AuthResetGated true (this must never happen -- the gesture
#      is the recovery path for a lost PIN).
#   5. The REAL production file, dot-sourced and scanned directly -> passes
#      clean, proving today's real check is not vacuous on the actual tree.
#   6. Pause/Resume pausing directly (no gate) -> caught, PauseResumeGated
#      false.
#   7. The menu nav calling kiln_ui_show() directly beside a stray
#      LCD_PIN_ROLE_USER token -> caught, MenuNavGated false (the first
#      version of this check passed that shape).
#
# This does not touch the real repo tree; steps 1-4 are entirely synthetic
# scratch files, and step 5 only READS the real tree.
#
# Usage: powershell -ExecutionPolicy Bypass -File App\test\test_check_lcd_home_nav_gated.ps1

$ErrorActionPreference = "Stop"

$testDir = $PSScriptRoot
$repoRoot = (Resolve-Path (Join-Path $testDir "..\..\..\..")).Path
$checkScript = Join-Path $repoRoot "tools\check_lcd_home_nav_gated.ps1"

if (-not (Test-Path $checkScript)) {
    throw "test_check_lcd_home_nav_gated: expected $checkScript not found -- has it moved?"
}

. $checkScript

$scratchDir = Join-Path $env:TEMP "lcd_home_nav_gate_test_$PID"
if (-not (Test-Path $scratchDir)) {
    New-Item -ItemType Directory -Force -Path $scratchDir | Out-Null
}

$failures = @()

$goodBody = @'
void ui_home_menu_nav_cb(lv_event_t *e)
{
    (void)e;
    ui_lcd_lock_run_gated("Enter PIN to open Menu", LCD_PIN_ROLE_USER,
                           ui_home_menu_nav_gated_cb, NULL);
}

void ui_home_profile_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_lcd_lock_run_gated("Enter PIN to pick a profile", LCD_PIN_ROLE_USER,
                           ui_home_profile_btn_gated_cb, NULL);
}

void ui_home_pause_resume_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_lcd_lock_run_gated("Enter PIN to pause/resume", LCD_PIN_ROLE_USER,
                           ui_home_pause_resume_gated_cb, NULL);
}

void ui_home_edit_btn_cb(lv_event_t *e)
{
    (void)e;
    ui_lcd_lock_run_gated("Enter PIN to edit firing", LCD_PIN_ROLE_USER,
                           ui_home_edit_btn_gated_cb, NULL);
}

void ui_home_auth_reset_corner_tap_cb(lv_event_t *e)
{
    auth_reset_gesture_corner_t corner = (auth_reset_gesture_corner_t)(intptr_t)lv_event_get_user_data(e);
    auth_reset_gesture_state_t *gesture = auth_reset_gesture_singleton();
    auth_reset_gesture_on_corner_tap(gesture, corner, 0, false, false, false);
}
'@

try {

# --- Assertion 1: the real shape -> passes clean. ---
$goodFile = Join-Path $scratchDir "good.c"
Set-Content -Path $goodFile -Value $goodBody -Encoding utf8
$r1 = Invoke-HomeNavGateScan -SourceFile $goodFile
if ((-not $r1.MenuNavGated) -or (-not $r1.ProfileBtnGated) -or (-not $r1.PauseResumeGated) -or (-not $r1.EditBtnGated) -or $r1.AuthResetGated) {
    $failures += "Assertion 1 FAILED: correctly-shaped synthetic file scored MenuNavGated=$($r1.MenuNavGated) ProfileBtnGated=$($r1.ProfileBtnGated) EditBtnGated=$($r1.EditBtnGated) AuthResetGated=$($r1.AuthResetGated), expected true/true/true/false."
} else {
    Write-Host "Assertion 1 OK: correctly-shaped synthetic file passes."
}

# --- Assertion 2: remove the menu nav's PIN gate -> caught. ---
$noMenuGateBody = $goodBody -replace [regex]::Escape('ui_lcd_lock_run_gated("Enter PIN to open Menu", LCD_PIN_ROLE_USER,
                           ui_home_menu_nav_gated_cb, NULL);'), "kiln_ui_show(`"config`");"
if ($noMenuGateBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 2 did not match anything in `$goodBody"
}
$noMenuGateFile = Join-Path $scratchDir "no_menu_gate.c"
Set-Content -Path $noMenuGateFile -Value $noMenuGateBody -Encoding utf8
$r2 = Invoke-HomeNavGateScan -SourceFile $noMenuGateFile
if ($r2.MenuNavGated) {
    $failures += "Assertion 2 FAILED: removing the menu nav's PIN gate was NOT detected (MenuNavGated=true)."
} else {
    Write-Host "Assertion 2 OK: removing the menu nav's PIN gate is detected (MenuNavGated=false)."
}

# --- Assertion 3: remove the profile button's PIN gate -> caught. ---
$noProfileGateBody = $goodBody -replace [regex]::Escape('ui_lcd_lock_run_gated("Enter PIN to pick a profile", LCD_PIN_ROLE_USER,
                           ui_home_profile_btn_gated_cb, NULL);'), "kiln_ui_show(`"profile_picker`");"
if ($noProfileGateBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 3 did not match anything in `$goodBody"
}
$noProfileGateFile = Join-Path $scratchDir "no_profile_gate.c"
Set-Content -Path $noProfileGateFile -Value $noProfileGateBody -Encoding utf8
$r3 = Invoke-HomeNavGateScan -SourceFile $noProfileGateFile
if ($r3.ProfileBtnGated) {
    $failures += "Assertion 3 FAILED: removing the profile button's PIN gate was NOT detected (ProfileBtnGated=true)."
} else {
    Write-Host "Assertion 3 OK: removing the profile button's PIN gate is detected (ProfileBtnGated=false)."
}

# --- Assertion 4: add a PIN gate to the credential-reset callback -> caught.
# This is the dangerous direction: gating the PIN-recovery gesture behind
# the PIN it exists to recover from. ---
$gatedResetBody = $goodBody -replace [regex]::Escape("auth_reset_gesture_on_corner_tap(gesture, corner, 0, false, false, false);"), "ui_lcd_lock_run_gated(`"Enter PIN`", LCD_PIN_ROLE_USER, some_cb, NULL);"
if ($gatedResetBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 4 did not match anything in `$goodBody"
}
$gatedResetFile = Join-Path $scratchDir "gated_reset.c"
Set-Content -Path $gatedResetFile -Value $gatedResetBody -Encoding utf8
$r4 = Invoke-HomeNavGateScan -SourceFile $gatedResetFile
if (-not $r4.AuthResetGated) {
    $failures += "Assertion 4 FAILED: a PIN gate added to the credential-reset gesture was NOT detected (AuthResetGated=false) -- the check is blind to the exact defect it exists to catch."
} else {
    Write-Host "Assertion 4 OK: a PIN gate added to the credential-reset gesture is detected (AuthResetGated=true)."
}

# --- Assertion 5: the REAL production file -> passes clean. ---
$realFile = Join-Path $repoRoot "firmware\KilnFW\App\drivers\ui\ui_page_home_actions.c"
if (-not (Test-Path $realFile)) {
    $failures += "Assertion 5 FAILED: real file $realFile not found."
} else {
    $r5 = Invoke-HomeNavGateScan -SourceFile $realFile
    if ((-not $r5.MenuNavGated) -or (-not $r5.ProfileBtnGated) -or (-not $r5.PauseResumeGated) -or (-not $r5.EditBtnGated) -or $r5.AuthResetGated) {
        $failures += "Assertion 5 FAILED: the REAL production file scored MenuNavGated=$($r5.MenuNavGated) ProfileBtnGated=$($r5.ProfileBtnGated) EditBtnGated=$($r5.EditBtnGated) AuthResetGated=$($r5.AuthResetGated), expected true/true/true/false -- today's real check is either vacuous or the real file regressed."
    } else {
        Write-Host "Assertion 5 OK: the REAL production file passes -- not vacuous on the actual tree."
    }
}

# --- Assertion 6: Pause/Resume acting directly, ungated -> caught. ---
$noPauseGateBody = $goodBody -replace [regex]::Escape('ui_lcd_lock_run_gated("Enter PIN to pause/resume", LCD_PIN_ROLE_USER,
                           ui_home_pause_resume_gated_cb, NULL);'), "profile_executor_pause();"
if ($noPauseGateBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 6 did not match anything in `$goodBody"
}
$noPauseGateFile = Join-Path $scratchDir "no_pause_gate.c"
Set-Content -Path $noPauseGateFile -Value $noPauseGateBody -Encoding utf8
$r6 = Invoke-HomeNavGateScan -SourceFile $noPauseGateFile
if ($r6.PauseResumeGated) {
    $failures += "Assertion 6 FAILED: an ungated Pause/Resume was NOT detected (PauseResumeGated=true)."
} else {
    Write-Host "Assertion 6 OK: an ungated Pause/Resume is detected (PauseResumeGated=false)."
}

# --- Assertion 7: direct navigation beside a stray gate token -> caught. ---
$sneakyMenuBody = $goodBody -replace [regex]::Escape('ui_lcd_lock_run_gated("Enter PIN to open Menu", LCD_PIN_ROLE_USER,
                           ui_home_menu_nav_gated_cb, NULL);'), "lcd_pin_role_t r = LCD_PIN_ROLE_USER; (void)r; kiln_ui_show(`"config`");"
if ($sneakyMenuBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 7 did not match anything in `$goodBody"
}
$sneakyMenuFile = Join-Path $scratchDir "sneaky_menu.c"
Set-Content -Path $sneakyMenuFile -Value $sneakyMenuBody -Encoding utf8
$r7 = Invoke-HomeNavGateScan -SourceFile $sneakyMenuFile
if ($r7.MenuNavGated) {
    $failures += "Assertion 7 FAILED: a direct kiln_ui_show() beside a stray LCD_PIN_ROLE_USER token was NOT detected (MenuNavGated=true)."
} else {
    Write-Host "Assertion 7 OK: a direct navigation beside a stray gate token is detected (MenuNavGated=false)."
}

# --- Assertion 8: remove the Edit button's PIN gate -> caught. ---
$noEditGateBody = $goodBody -replace [regex]::Escape('ui_lcd_lock_run_gated("Enter PIN to edit firing", LCD_PIN_ROLE_USER,
                           ui_home_edit_btn_gated_cb, NULL);'), "kiln_ui_show(`"edit_firing`");"
if ($noEditGateBody -eq $goodBody) {
    throw "test setup error: the replace for assertion 8 did not match anything in `$goodBody"
}
$noEditGateFile = Join-Path $scratchDir "no_edit_gate.c"
Set-Content -Path $noEditGateFile -Value $noEditGateBody -Encoding utf8
$r8 = Invoke-HomeNavGateScan -SourceFile $noEditGateFile
if ($r8.EditBtnGated) {
    $failures += "Assertion 8 FAILED: removing the Edit button's PIN gate was NOT detected (EditBtnGated=true)."
} else {
    Write-Host "Assertion 8 OK: removing the Edit button's PIN gate is detected (EditBtnGated=false)."
}

} finally {
    Remove-Item -Path $scratchDir -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Host "test_check_lcd_home_nav_gated FAILED:" -ForegroundColor Red
    foreach ($f in $failures) {
        Write-Host "  - $f" -ForegroundColor Red
    }
    throw "$($failures.Count) assertion(s) failed -- see above."
}

Write-Host ""
Write-Host "test_check_lcd_home_nav_gated: all 8 assertions passed."
exit 0
