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
    @('ui_page_diagnostics.c', ('has_role\(' + $admin + '\)'), 'L11 crash ack')
)
# Review LCDFX2/DEVBREAK INFO: rules match code only. A gate call left inside a comment must not
# satisfy a rule, so // and /* */ comments are blanked (newlines kept) before matching. String and
# char literals are skipped so a "//" inside a string is not taken for a comment.
function Remove-CComments([string]$s) {
    $sb = New-Object System.Text.StringBuilder
    $i = 0; $n = $s.Length
    while ($i -lt $n) {
        $c = $s[$i]
        $d = if ($i + 1 -lt $n) { $s[$i + 1] } else { [char]0 }
        if ($c -eq '/' -and $d -eq '/') {
            while ($i -lt $n -and $s[$i] -ne "`n") { $i++ }
        } elseif ($c -eq '/' -and $d -eq '*') {
            $i += 2
            while ($i -lt $n -and -not ($s[$i] -eq '*' -and $i + 1 -lt $n -and $s[$i + 1] -eq '/')) {
                if ($s[$i] -eq "`n") { [void]$sb.Append("`n") }
                $i++
            }
            $i += 2; [void]$sb.Append(' ')
        } elseif ($c -eq '"' -or $c -eq "'") {
            [void]$sb.Append($c); $i++
            while ($i -lt $n -and $s[$i] -ne $c) {
                if ($s[$i] -eq '\' -and $i + 1 -lt $n) { [void]$sb.Append($s[$i]); $i++ }
                [void]$sb.Append($s[$i]); $i++
            }
            if ($i -lt $n) { [void]$sb.Append($c); $i++ }
        } else { [void]$sb.Append($c); $i++ }
    }
    return $sb.ToString()
}
function Get-Code([string]$path) { Remove-CComments ((Get-Content -Raw -Path $path) -replace "`r", '') }

$fail = 0
foreach ($r in $rules) {
    $path = Join-Path $UiDir $r[0]
    if (-not (Test-Path $path)) { Write-Host "FAIL: $($r[2]): missing $($r[0])"; $fail++; continue }
    $txt = Get-Code $path
    $need = if ($r[1] -match 'ROLE_USER') { 'a USER' } else { 'an admin' }
    if ($txt -notmatch $r[1]) { Write-Host "FAIL: $($r[2]): $($r[0]) lacks $need gate matching /$($r[1])/"; $fail++ }
}
# C3 (REVIEW_LCDFX3 LOW-3, REVIEW_LCDFX4 LOW-1..4, INFO-1): every callback registered through
# build_nav_item(grid, "...", cb) in ui_page_config.c is enumerated. Each USER callback's WHOLE body
# must be exactly one `ui_lcd_lock_run_gated("...", LCD_PIN_ROLE_USER, <page>_open_apply, NULL);`
# (optionally preceded by (void)e;) -- no comma operator, no arithmetic on the role, no code around it.
# The per-page rule is checked against that callback's own body. The ADMIN exemption is only
# touch_cal_nav_cb by name with its body pinned. Every preprocessor conditional is refused in the file,
# a callback defined twice fails, and any registration form not recognised (cast, non-literal label,
# array entry, direct lv_obj_add_event_cb) fails loud.
$cfg = Get-Code (Join-Path $UiDir "ui_page_config.c")
if ($cfg -match '(?m)^\s*#\s*(if|ifdef|ifndef|elif|else)\b') { Write-Host "FAIL: C3: ui_page_config.c contains a preprocessor conditional (dead code can hide an ungated twin)"; $fail++ }
$navCalls = @([regex]::Matches($cfg, '\bbuild_nav_item\s*\(')).Count - 1   # minus the definition
$navRe = @([regex]::Matches($cfg, '(?<!void )build_nav_item\(\s*\w+\s*,\s*"[^"]*"\s*,\s*(\w+)\s*\)'))
if ($navRe.Count -ne $navCalls) { Write-Host "FAIL: C3: $navCalls build_nav_item call(s) but only $($navRe.Count) in a recognised form (cast, non-literal label or array entry?)"; $fail++ }
foreach ($m in [regex]::Matches($cfg, 'lv_obj_add_event_cb\(\s*\w+\s*,\s*([^,]+),')) {
    $v = $m.Groups[1].Value.Trim()
    if ($v -notin 'cb', 'units_toggle_cb') { Write-Host "FAIL: C3: unrecognised lv_obj_add_event_cb registration of '$v' in ui_page_config.c"; $fail++ }
}
$navN = 0; $applies = @{}
$cbs = @($navRe | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
foreach ($cb in $cbs) {
    $defs = @([regex]::Matches($cfg, '\b' + [regex]::Escape($cb) + '\s*\(\s*lv_event_t')).Count
    if ($defs -ne 1) { Write-Host "FAIL: C3: ui_page_config.c nav callback $cb has $defs definitions (expected exactly 1)"; $fail++; continue }
    $m = [regex]::Match($cfg, '(?s)static void ' + [regex]::Escape($cb) + '\(lv_event_t \*e\)\s*\{(.*?)\n\}')
    if (-not $m.Success) { Write-Host "FAIL: C3: ui_page_config.c nav callback $cb not found"; $fail++; continue }
    $body = $m.Groups[1].Value
    $um = [regex]::Match($body, '^\s*(\(void\)\s*e\s*;\s*)?ui_lcd_lock_run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*(\w+_open_apply),\s*NULL\)\s*;\s*$')
    if ($um.Success) { $navN++; $applies[$um.Groups[2].Value] = $cb; continue }
    if ($cb -eq 'touch_cal_nav_cb') {
        $tc = '^\s*\(void\)\s*e\s*;\s*if\s*\(!touch_cal_store_is_calibrated\(\)\)\s*\{\s*kiln_ui_show\("touch_cal"\)\s*;\s*return\s*;\s*\}\s*ui_lcd_lock_run_gated\("[^"]*",\s*LCD_PIN_ROLE_ADMIN,\s*touch_cal_open_apply,\s*NULL\)\s*;\s*$'
        if ($body -notmatch $tc) { Write-Host "FAIL: C3: touch_cal_nav_cb body is not the pinned shape (ADMIN gate with nothing else)"; $fail++ }
        continue
    }
    Write-Host "FAIL: C3: ui_page_config.c nav callback $cb is not exactly one USER gate statement (only touch_cal_nav_cb may differ, as pinned)"; $fail++
}
if ($navN -ne 5) { Write-Host "FAIL: C3: expected exactly 5 USER-gated nav callbacks in ui_page_config.c, found $navN"; $fail++ }
foreach ($ap in 'temperature_open_apply', 'network_open_apply', 'diagnostics_open_apply', 'safety_open_apply', 'profiles_open_apply') {
    if (-not $applies.ContainsKey($ap)) { Write-Host "FAIL: C3: no USER-gated nav callback body calls $ap"; $fail++ }
}
# LCDFX4 LOW-4: the Home trip strip is the other way into Safety; pin its whole handler.
$homeSrc = Get-Code (Join-Path $UiDir "ui_page_home.c")
$hm = [regex]::Match($homeSrc, '(?s)static void trip_strip_clicked_cb\(lv_event_t \*e\)\s*\{(.*?)\n\}')
$hpin = '^\s*\(void\)\s*e\s*;\s*if\s*\(s_ui_home_trip_strip_is_safety\)\s*\{\s*if\s*\(lcd_safety_strip_needs_pin\(ui_lcd_lock_has_role\(LCD_PIN_ROLE_USER\)\)\)\s*\{\s*ui_lcd_lock_run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*trip_strip_gated_open_cb,\s*NULL\)\s*;\s*\}\s*else\s*\{\s*trip_strip_gated_open_cb\(NULL\)\s*;\s*\}\s*\}\s*$'
if (-not $hm.Success -or $hm.Groups[1].Value -notmatch $hpin) { Write-Host "FAIL: C4: ui_page_home.c trip_strip_clicked_cb is not the pinned PIN-predicate shape"; $fail++ }
if (@([regex]::Matches($homeSrc, '\bui_page_safety_open\s*\(')).Count -ne 1) { Write-Host "FAIL: C4: ui_page_home.c must call ui_page_safety_open( exactly once (inside trip_strip_gated_open_cb)"; $fail++ }

# L9: lock init must precede the touch-cal early return.
$kui = Get-Code (Join-Path $UiDir "kiln_ui.c")
$m = [regex]::Match($kui, "(?m)^\s*ui_lcd_lock_init\(\);")
$i = if ($m.Success) { $m.Index } else { -1 }
$j = $kui.IndexOf("lvgl_port_touch_cal_support()")
if ($i -lt 0 -or $j -lt 0 -or $i -gt $j) {
    Write-Host "FAIL: L9: kiln_ui.c must call ui_lcd_lock_init() before lvgl_port_touch_cal_support()"; $fail++
}
# L-relock: handle_lcd_relock_to_home must close both network-page modals (REVIEW_LCDFX2 LOW-2).
$rm = [regex]::Match($kui, '(?s)static void handle_lcd_relock_to_home\(void\)\s*\{(.*?)\n\}')
if (-not $rm.Success) { Write-Host "FAIL: relock: handle_lcd_relock_to_home not found in kiln_ui.c"; $fail++ }
else {
    foreach ($fn in 'ui_page_network_relock_close', 'ui_page_network_manage_relock_close') {
        if ($rm.Groups[1].Value -notmatch ('(?m)\b' + $fn + '\(\)\s*;')) {
            Write-Host "FAIL: relock: handle_lcd_relock_to_home does not call $fn()"; $fail++
        }
    }
}
if ($fail) { Write-Host "LCD admin gate check FAILED ($fail)"; exit 1 }
Write-Host "LCD admin gate check passed: $($rules.Count) gates plus C3/C4 nav-callback and trip-strip shape, L9 init order and relock."
exit 0
