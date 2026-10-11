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
# C3 (REVIEW_LCDFX3 LOW-3): every callback registered through build_nav_item(grid, "...", cb) in
# ui_page_config.c is enumerated (a renamed callback cannot drop out). A USER-gated callback must
# consist of NOTHING but the unconditional gate statement (optionally preceded by (void)e;): any
# code before it (an open ahead of the gate, an if/else/?/return around it) or after it (a direct
# *_open_apply(NULL) after the gate) fails. An ADMIN-gated callback (touch cal) is allowed; any
# other callback fails. `#if 0` is refused outright in the file (a gated #if 0 copy with an ungated
# #else twin), since the comment-aware matching cannot see preprocessor dead code.
$cfg = Get-Code (Join-Path $UiDir "ui_page_config.c")
if ($cfg -match '(?m)^\s*#\s*if\s+0\b') { Write-Host "FAIL: C3: ui_page_config.c contains '#if 0' (dead preprocessor code can hide an ungated twin)"; $fail++ }
$navN = 0
$cbs = @([regex]::Matches($cfg, 'build_nav_item\(\s*\w+\s*,\s*"[^"]*"\s*,\s*(\w+)\s*\)') | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
foreach ($cb in $cbs) {
    $m = [regex]::Match($cfg, '(?s)static void ' + [regex]::Escape($cb) + '\(lv_event_t \*e\)\s*\{(.*?)\n\}')
    if (-not $m.Success) { Write-Host "FAIL: C3: ui_page_config.c nav callback $cb not found"; $fail++; continue }
    $body = $m.Groups[1].Value
    $g = [regex]::Match($body, 'ui_lcd_lock_run_gated\([^;]*;')
    if ($g.Success -and $g.Value -match 'LCD_PIN_ROLE_USER') {
        $navN++
        $pre = $body.Substring(0, $g.Index); $post = $body.Substring($g.Index + $g.Length)
        if ($pre -notmatch '^\s*(\(void\)\s*e\s*;\s*)?$') { Write-Host "FAIL: C3: ui_page_config.c $cb has code before its USER gate (open ahead of it, or the gate is conditional)"; $fail++ }
        if ($post -notmatch '^\s*$') { Write-Host "FAIL: C3: ui_page_config.c $cb has code after its USER gate (direct open bypasses the PIN)"; $fail++ }
    } elseif ($g.Success -and $g.Value -match 'LCD_PIN_ROLE_ADMIN') {
        continue
    } else {
        Write-Host "FAIL: C3: ui_page_config.c nav callback $cb has no USER/ADMIN gate"; $fail++
    }
}
if ($navN -lt 5) { Write-Host "FAIL: C3: expected >= 5 USER-gated nav callbacks in ui_page_config.c, found $navN"; $fail++ }# L9: lock init must precede the touch-cal early return.
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
Write-Host "LCD admin gate check passed: $($rules.Count) gates plus L9 init order."
exit 0
