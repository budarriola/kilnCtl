# checkcache: ok
# check_lcd_admin_gates.ps1 -- LCD UI audit 2026-10-09 (L1-L3, L6, L9, L11, L22).
# Every LCD action whose web equivalent is ROUTE_TIER_ADMIN must run through the
# admin gate (ui_lcd_lock_run_gated(..., LCD_PIN_ROLE_ADMIN, ...) or a
# ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN) check), and kiln_ui.c must initialize
# the LCD lock BEFORE the touch-calibration early return (L9).
# Mechanical, string-level: each rule names a file and a regex that must match.
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_lcd_admin_gates.ps1 [-UiDir <path>]
param([string]$UiDir)
$ErrorActionPreference = "Stop"
if (-not $UiDir) { $UiDir = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\drivers\ui" }
$UiDir = (Resolve-Path $UiDir).Path

$admin = 'LCD_PIN_ROLE_ADMIN'
$rules = @(
    @('ui_page_network_manage.c', 'run_gated\("Admin PIN to [a-z ]+",\s*LCD_PIN_ROLE_ADMIN, connect_submit_apply', 'L1 connect/add'),
    @('ui_page_network_manage.c', ('run_gated\("Admin PIN to forget network",\s*' + $admin), 'L1 forget'),
    @('ui_page_network.c', ('run_gated\("Admin PIN to change Wi-Fi mode",\s*' + $admin), 'L1 mode'),
    @('ui_page_network.c', ('run_gated\("Admin PIN to change AP identity",\s*' + $admin), 'L1 AP identity'),
    @('ui_page_network.c', ('has_role\(' + $admin + '\)'), 'L6 AP password/QR mask'),
    @('ui_page_profile_builder_review.c', ('run_gated\("Admin PIN to save profile",\s*' + $admin), 'L2 save'),
    @('ui_page_live_decide.c', ('run_gated\("Admin PIN to keep/discard edit",\s*' + $admin), 'L2 decide'),
    @('ui_page_temperature.c', ('run_gated\("Admin PIN to switch relay",\s*' + $admin), 'L3 relay'),
    @('ui_page_config.c', ('run_gated\("Admin PIN to change units",\s*' + $admin), 'L11 units'),
    @('ui_page_config.c', ('run_gated\("Admin PIN to recalibrate touch",\s*' + $admin), 'L11 touch cal nav'),
    @('ui_page_touch_cal.c', ('has_role\(' + $admin + '\)'), 'L11 touch cal save'),
    @('ui_page_profile_picker.c', ('has_role\(' + $admin + '\)'), 'L11 profile delete'),
    @('ui_page_edit_firing.c', ('run_gated\("Enter admin PIN to apply edit",\s*' + $admin), 'L11 live-edit apply'),
    @('ui_page_diagnostics.c', ('has_role\(' + $admin + '\)'), 'L11 crash ack'),
    @('ui_page_config.c', 'run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*temperature_open_apply', 'L3 hub temperature USER gate'),
    @('ui_page_config.c', 'run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*network_open_apply', 'L3 hub network USER gate'),
    @('ui_page_config.c', 'run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*diagnostics_open_apply', 'L3 hub diagnostics USER gate'),
    @('ui_page_config.c', 'run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*safety_open_apply', 'L3 hub safety USER gate'),
    @('ui_page_config.c', 'run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*profiles_open_apply', 'L3 hub profiles USER gate')
)
$fail = 0
foreach ($r in $rules) {
    $path = Join-Path $UiDir $r[0]
    if (-not (Test-Path $path)) { Write-Host "FAIL: $($r[2]): missing $($r[0])"; $fail++; continue }
    $txt = Get-Content -Raw -Path $path
    if ($txt -notmatch $r[1]) { Write-Host "FAIL: $($r[2]): $($r[0]) lacks an admin gate matching /$($r[1])/"; $fail++ }
}
# L9: lock init must precede the touch-cal early return.
$kui = Get-Content -Raw -Path (Join-Path $UiDir "kiln_ui.c")
$m = [regex]::Match($kui, "(?m)^\s*ui_lcd_lock_init\(\);")
$i = if ($m.Success) { $m.Index } else { -1 }
$j = $kui.IndexOf("lvgl_port_touch_cal_support()")
if ($i -lt 0 -or $j -lt 0 -or $i -gt $j) {
    Write-Host "FAIL: L9: kiln_ui.c must call ui_lcd_lock_init() before lvgl_port_touch_cal_support()"; $fail++
}
if ($fail) { Write-Host "LCD admin gate check FAILED ($fail)"; exit 1 }
Write-Host "LCD admin gate check passed: $($rules.Count) gates plus L9 init order."
exit 0
