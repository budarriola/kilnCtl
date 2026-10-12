# checkcache: ok
# check_lcd_admin_gates.ps1 -- LCD UI audit 2026-10-09 (L1-L3, L6, L9, L11, L22).
# Every LCD action whose web equivalent is ROUTE_TIER_ADMIN must run through the
# admin gate (ui_lcd_lock_run_gated(..., LCD_PIN_ROLE_ADMIN, ...) or a
# ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN) check), and kiln_ui.c must initialize
# the LCD lock BEFORE the touch-calibration early return (L9).
# Mechanical, string-level: each rule names a file and a regex that must match.
# Usage: powershell -ExecutionPolicy Bypass -File tools\check_lcd_admin_gates.ps1 [-UiDir <path>]
# REVIEW_LCDFX9 ACCEPTED LIMITS (threat model: accidental regressions by honest developers; regex is NOT a C parser):
#  - A gate token spelled via `##` paste or defined as a macro OUTSIDE drivers/ui is invisible here; the object-level
#    check_lcd_sink_callgraph.py proves no un-gated LCD page reaches safety_trip_clear's sinks instead.
#  - Preprocessor tricks that redefine a gate token in a header outside drivers/ui are backstopped by -Werror=shadow
#    (drivers/CMakeLists.txt) and the call-graph check, not by text rules.
#  - A gate function that is made always-true is caught by the host test test_ui_lcd_lock.c, not here.
#  - Anything needing real C parsing (macro expansion, constexpr folding) is out of scope; no further regex rounds.
param([string]$UiDir, [switch]$DumpSinks)
$ErrorActionPreference = "Stop"
if (-not $UiDir) { $UiDir = Join-Path $PSScriptRoot "..\firmware\KilnFW\App\drivers\ui" }
$UiDir = (Resolve-Path $UiDir).Path

$admin = 'LCD_PIN_ROLE_ADMIN'
# LCDFX9: every identifier that names an admin-tier mutator the LCD could reach. ANY mention (call, address-of, #define
# alias, log argument, header wrapper) under drivers/ui must be in $sinkRows below; unlisted mentions fail (deny by default).
$denyIdent = '(?:dashboard_set_\w+|dashboard_safety_clear_trip|safety_link_send_\w+|wifi_prov_(?:add|forget|set|post|clear|save(?!d)|delete|reset|start|stop|connect|disconnect|factory|remove)\w*|unit_pref_set\w*|unit_pref_(?:reset|clear|save)\w*|touch_cal_store_(?:save|clear|erase|reset|write)\w*|profiles_http_(?:save|delete|set|create|import|restore|retarget|rename|copy|write|erase)\w*|profiles_favorites_set|profiles_live_(?:decide_apply|fork|edit)\w*|crash_report_(?:ack|clear)\w*|kiln_io_set_\w+|edit_firing_apply|zones_config_(?:set|save|write|reset)\w*|aux_outputs_(?:set|save)\w*|wifi_prov_get_\w*(?:password|psk|secret)\w*|esp_restart|time_sync_set\w*|kiln_io_owner_command_(?:set|all_relays_off|sx_write)\w*|relay_cycles_(?:reset|set_type|restore)\w*|ramp_assist_cfg_(?:set|save|reset|apply)\w*|display_power_cfg_(?:set|save|reset)\w*|danger_mode_(?:request|set|stop|touch|clear)\w*|relay_names_(?:save|set|reset|clear)\w*|safety_cfg_write_\w+|autotune_engine_(?:start|abort|stop)\w*|factory_reset_(?:execute|reboot)\w*)'
# REVIEW_LCDFX5 LOW-5: every rule is matched against ONE named function's body (brace-matched), never the
# whole file, so a gate moved into a dead function no longer satisfies it. Fields: file, function, regex
# that must match the body, label, and optionally an apply identifier whose non-definition occurrences in
# the file must equal the expected count (a direct, ungated call of the apply adds an occurrence).
# To change a gated call site: edit its row here (function / apply name / count) in the same change.
# LCDFX8 LOW-1: a case body is straight-line statements then an UNCONDITIONAL return (no if/?:/loops/jumps before it).
$uncond = '(?:(?!\bif\b|\bwhile\b|\bfor\b|\bswitch\b|\bgoto\b|\breturn\b|\?)[^{}])*?\breturn\s*;'
# Bare-gate callbacks allowed to have code after the gate statement (none today; add with a reason).
# Keyed by row label. apply_cb carries two gates (apply, then decide), so each row sees the other gate.
# LCDFX9 HIGH-5: leading statements allowed before a start-anchored guard: `(void)x;` and plain declarations with an initializer.
$pfx = '(?:\s*(?:\(void\)\s*\w+|(?!\s*(?:return|goto|else)\b)[\w\s\*]+=[^;{}]*)\s*;)*\s*'
$afterGateOk = @('L11 live-edit decide')
$preCallOk = @{ 'L11 touch cal nav' = 'kiln_ui_show'; 'L11 live-edit apply' = 'ui_lcd_lock_run_gated' }   # call names allowed before this row's gate
function GateRx([string]$prompt, [string]$role, [string]$apply) { 'ui_lcd_lock_run_gated\(\s*"' + $prompt + '",\s*' + $role + ',\s*' + $apply + ',' }
$rules = @(
    @('ui_page_network_manage.c', 'connect_submit_cb', (GateRx 'Admin PIN to add network' $admin 'connect_submit_apply'), 'L1 connect/add', 'connect_submit_apply', 1),
    @('ui_page_network_manage.c', 'scan_row_clicked_cb', (GateRx 'Admin PIN to add network' $admin 'scan_row_open_apply'), 'L1 scan row', 'scan_row_open_apply', 1),
    @('ui_page_network_manage.c', 'forget_confirm_yes_cb', (GateRx 'Admin PIN to forget network' $admin 'forget_apply'), 'L1 forget', 'forget_apply', 1),
    @('ui_page_network.c', 'mode_home_btn_cb', (GateRx 'Admin PIN to change Wi-Fi mode' $admin 'mode_change_gated'), 'L1 mode home', $null, 0),
    @('ui_page_network.c', 'mode_ap_btn_cb', (GateRx 'Admin PIN to change Wi-Fi mode' $admin 'mode_change_gated'), 'L1 mode ap', 'mode_change_gated', 2),
    @('ui_page_network.c', 'ap_edit_confirm_apply_cb', (GateRx 'Admin PIN to change AP identity' $admin 'ap_edit_apply_gated'), 'L1 AP identity', 'ap_edit_apply_gated', 1),
    @('ui_page_network.c', 'ap_edit_open_cb', (GateRx 'Admin PIN to edit AP identity' $admin 'ap_edit_open_apply'), 'L6 AP editor open', 'ap_edit_open_apply', 1),
    @('ui_page_network.c', 'refresh_cb', ('bool show_secret = ui_lcd_lock_has_role\(' + $admin + '\);\s*if\s*\(!show_secret\)'), 'L6 AP password/QR mask', $null, 0),
    @('ui_page_profile_builder_review.c', 'do_save', (GateRx 'Admin PIN to save profile' $admin 'do_save_apply'), 'L2 save', 'do_save_apply', 1),
    @('ui_page_live_decide.c', 'run_decision', (GateRx 'Admin PIN to keep/discard edit' $admin 'run_decision_apply'), 'L2 decide', 'run_decision_apply', 1),
    @('ui_page_temperature.c', 'relay_toggle_cb', (GateRx 'Admin PIN to switch relay' $admin 'relay_toggle_apply'), 'L3 relay', 'relay_toggle_apply', 1),
    @('ui_page_config.c', 'units_toggle_cb', (GateRx 'Admin PIN to change units' $admin 'units_toggle_apply'), 'L11 units', 'units_toggle_apply', 1),
    @('ui_page_config.c', 'units_toggle_apply', ('^\s*\(void\)\s*\w+\s*;\s*if\s*\(!ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{\s*return;\s*\}'), 'L11 units apply re-check', $null, 0),
    @('ui_page_config.c', 'touch_cal_nav_cb', (GateRx 'Admin PIN to recalibrate touch' $admin 'touch_cal_open_apply'), 'L11 touch cal nav', 'touch_cal_open_apply', 1),
    @('ui_page_touch_cal.c', 'finish_calibration', ('[;}]\s*if\s*\(touch_cal_store_is_calibrated\(\)\s*&&\s*!ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{' + $uncond + '\s*\}\s*err\s*=\s*touch_cal_store_save\('), 'L11 touch cal save', $null, 0),
    @('ui_page_profile_picker.c', 'delete_btn_clicked_cb', ('^' + $pfx + 'if\s*\(!ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{\s*' + (GateRx 'Admin PIN to delete profile' $admin 'delete_unlocked_cb') + '[^;]*;\s*return\s*;\s*\}\s*delete_do\(ud\)\s*;\s*$'), 'L11 profile delete', $null, 0),
    @('ui_page_profile_picker.c', 'delete_unlocked_cb', ('^\s*if\s*\(ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{\s*delete_do\('), 'L11 profile delete unlocked', $null, 0),
    @('ui_page_edit_firing.c', 'apply_cb', (GateRx 'Enter admin PIN to apply edit' $admin 'apply_live_gated'), 'L11 live-edit apply', 'apply_live_gated', 1),
    @('ui_page_edit_firing.c', 'apply_cb', (GateRx 'Enter admin PIN to keep/discard edit' $admin 'open_decide_gated_cb'), 'L11 live-edit decide', 'open_decide_gated_cb', 1),
    @('ui_page_edit_firing.c', 'apply_live_gated', ('^\s*\(void\)\s*user_data\s*;\s*if\s*\(!s_pg\s*\|\|\s*!s_pg->active\s*\|\|\s*!s_pg->working\s*\|\|\s*!ui_lcd_lock_has_role\(' + $admin + '\)\)\s*return\s*;'), 'L11 live-edit apply re-check', $null, 0),
    @('ui_page_diagnostics.c', 'crash_ack_btn_clicked_cb', ('^' + $pfx + 'if\s*\(!ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{\s*' + (GateRx 'Admin PIN to acknowledge crash' $admin 'crash_ack_unlocked_cb') + '[^;]*;\s*return\s*;\s*\}\s*crash_ack_do\(\)\s*;\s*$'), 'L11 crash ack', $null, 0),
    @('ui_page_diagnostics.c', 'crash_ack_unlocked_cb', ('if\s*\(ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{\s*crash_ack_do\(\)'), 'L11 crash ack unlocked', $null, 0),
    # REVIEW_LCDFX6 HIGH-1 (owner decision D2): clearing a trip from the LCD needs an ADMIN PIN, and the action re-checks the
    # role itself. btn_clicked_cb is EXACTLY the ADMIN gate on clear_action (whose only reference is that gate);
    # clear_action's verdict is fed the live ADMIN role, NEEDS_ADMIN/NOT_TRIPPED return before the send, and the send
    # (dashboard_safety_clear_trip) appears once, after the switch. Other files in drivers/ui may not call it (below).
    @('ui_page_safety.c', 'btn_clicked_cb', ('^\s*\(void\)\s*e\s*;\s*' + (GateRx 'Enter admin PIN to clear trip' $admin 'clear_action') + '\s*NULL\)\s*;\s*$'), 'D2 trip clear gate', 'clear_action', 1),
    @('ui_page_safety.c', 'clear_action', ('^\s*\(void\)\s*user\s*;\s*(?:switch\s*\(ui_safety_clear_verdict\(&s_view,\s*ui_lcd_lock_has_role\(' + $admin + '\)\)\)|(?:const\s+)?\w+\s+(\w+)\s*=\s*ui_safety_clear_verdict\(&s_view,\s*ui_lcd_lock_has_role\(' + $admin + '\)\)\s*;\s*switch\s*\(\1\))\s*\{\s*case\s+UI_SAFETY_CLEAR_NOT_TRIPPED\s*:' + $uncond + '\s*case\s+UI_SAFETY_CLEAR_NEEDS_ADMIN\s*:' + $uncond + '\s*case\s+UI_SAFETY_CLEAR_SEND\s*:\s*default\s*:\s*break\s*;\s*\}[\s\S]*dashboard_safety_clear_trip\('), 'D2 trip clear re-check', 'dashboard_safety_clear_trip', 1)
)

# Review LCDFX2/DEVBREAK INFO: rules match code only. A gate call left inside a comment must not
# satisfy a rule, so // and /* */ comments are blanked (newlines kept) before matching. String and
# char literals are skipped so a "//" inside a string is not taken for a comment.
# REVIEW_LCDFX7: translation phases are mimicked in compiler order: (1) backslash-newline is spliced out FIRST
# (X1/X11/X12), (2) comments become one space, (3) adjacent string literals are concatenated (X6/X7/X15) so a
# page name split as "saf" "ety" is seen as "safety". One regex pass each, fast enough for all of App.
$script:litRe = '"(?:[^"\\\n]|\\.)*"'
$script:chrRe = "'(?:[^'\\\n]|\\.)*'"
$script:cmtEval = [System.Text.RegularExpressions.MatchEvaluator]{
    param($m)
    $v = $m.Value
    if ($v[0] -eq '"' -or $v[0] -eq "'") { return $v }
    $nl = $v.Split("`n").Count - 1
    return ' ' + ("`n" * $nl)
}
$script:catEval = [System.Text.RegularExpressions.MatchEvaluator]{
    param($m)
    $v = $m.Value
    if ($v[0] -eq "'") { return $v }
    $sb = New-Object System.Text.StringBuilder
    foreach ($x in [regex]::Matches($v, $script:litRe)) { [void]$sb.Append($x.Value.Substring(1, $x.Value.Length - 2)) }
    return '"' + $sb.ToString() + '"'
}
$script:blankEval = [System.Text.RegularExpressions.MatchEvaluator]{ param($m) '"' + ('_' * ($m.Value.Length - 2)) + '"' }
function Remove-CComments([string]$s) {
    $s = $s.Replace("\`n", '')
    $s = [regex]::Replace($s, '//[^\n]*|/\*[\s\S]*?\*/|' + $script:litRe + '|' + $script:chrRe, $script:cmtEval)
    [regex]::Replace($s, $script:chrRe + '|' + $script:litRe + '(?:\s*' + $script:litRe + ')*', $script:catEval)
}
# String literal contents replaced by '_' (same length), so an identifier or ';' inside a string never counts.
function Blank-Strings([string]$s) { [regex]::Replace($s, $script:litRe, $script:blankEval) }
# Tokens an ESP_LOG* statement must never contain (a call hidden in log arguments still executes: X15).
# LCDFX9 HIGH-3: built from the sink deny list and every row's apply name, so a new row cannot forget to be listed here.
$logApplies = @(($rules | Where-Object { $_[4] } | ForEach-Object { [regex]::Escape($_[4]) }) + 'delete_do', 'crash_ack_do', 'forget_worker_task', 'delete_unlocked_cb', 'crash_ack_unlocked_cb' | Select-Object -Unique) -join '|'
$logTok = '\b(kiln_ui_show|ui_page_safety_open|ui_lcd_lock_run_gated|ui_lcd_lock_has_role|clear_action|\w+_open_apply|\w+_gated_\w+|\w+_unlocked_cb|' + $denyIdent + '|' + $logApplies + ')\b'
$script:logFails = New-Object System.Collections.ArrayList
# REVIEW_LCDFX5 INFO-3: #warning/#error/#pragma/#line text may hold an unbalanced apostrophe that
# would desynchronise the stripper, so those directive lines are blanked first.
# REVIEW_LCDFX7 P1/P7: ESP_LOG* statements are removed from the code (they cannot gate or open anything), unless
# their arguments contain a trusted call/gate token, which fails the check (X15).
function Get-CodeFromText([string]$raw, [string]$name, [switch]$KeepLogs) {
    $t = $raw -replace "`r", ''
    $t = $t.Replace("\`n", '')
    $t = [regex]::Replace($t, '(?m)^[ \t]*(#|%:)[ \t]*(warning|error|pragma|line)\b[^\n]*', '')
    $t = Remove-CComments $t
    # LCDFX8 HIGH-1: attributes are removed here, so Count-Uses needs no `))` lookbehind (which hid every use after `if (f(x))`).
    $t = [regex]::Replace($t, '__attribute__\s*\(\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)\)', ' ')
    if ($KeepLogs) { return $t }
    $bl = Blank-Strings $t
    $ms = @([regex]::Matches($bl, '\bESP_LOG[IWEDVN]\s*\([^;]*\)\s*;'))
    for ($k = $ms.Count - 1; $k -ge 0; $k--) {
        $x = $ms[$k]
        if ($x.Value -match $logTok) { [void]$script:logFails.Add("$name has '$($Matches[1])' inside an ESP_LOG* statement (a call hidden in log arguments still executes; make it a real statement so the rules see it)") }
        $t = $t.Remove($x.Index, $x.Length)
    }
    return $t
}
function Get-Code([string]$path) { Get-CodeFromText (Get-Content -Raw -Path $path) (Split-Path $path -Leaf) }
# Body of the function whose definition ends at the '{' at $open: brace-matched, string/char aware.
function Get-BraceBody([string]$code, [int]$open) {
    $depth = 1; $i = $open + 1; $n = $code.Length
    while ($i -lt $n) {
        $c = $code[$i]
        if ($c -eq '"' -or $c -eq "'") {
            $i++
            while ($i -lt $n -and $code[$i] -ne $c) { if ($code[$i] -eq '\') { $i++ }; $i++ }
        } elseif ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') { $depth--; if ($depth -eq 0) { return $code.Substring($open + 1, $i - $open - 1) } }
        $i++
    }
    return $null
}
# Definitions only (a type token at line start, then NAME(...) {), so prototypes and calls never count.
function Get-FuncDefs([string]$code, [string]$name) {
    $at = '(?:__attribute__\s*\(\([^;{}]*?\)\)\s*)*'   # P6: attributes before/after the return type
    @([regex]::Matches($code, '(?m)^static\s+(?:inline\s+)?' + $at + '\w+[\s\*]+' + $at + [regex]::Escape($name) + '\s*\([^;{}]*\)\s*\{'))
}
function Get-FuncBody([string]$code, [string]$name) {
    $defs = Get-FuncDefs $code $name
    if ($defs.Count -ne 1) { return @{ ok = $false; n = $defs.Count } }
    $b = Get-BraceBody $code ($defs[0].Index + $defs[0].Length - 1)
    if ($null -eq $b) { return @{ ok = $false; n = 1 } }
    return @{ ok = $true; body = $b; n = 1 }
}
# Occurrences of an identifier that are not a definition/prototype (not preceded by the word void).
# P2: string literal contents are blanked first, so a name inside a log/format string is not a use.
$script:blankCache = @{}
function Count-Uses([string]$code, [string]$name) {
    if (-not $script:blankCache.ContainsKey($code)) { $script:blankCache[$code] = Blank-Strings $code }
    @([regex]::Matches($script:blankCache[$code], '(?<!void\s+)\b' + [regex]::Escape($name) + '\b')).Count }

$fail = 0
$codeCache = @{}
function Get-CachedCode([string]$file) {
    if (-not $codeCache.ContainsKey($file)) { $codeCache[$file] = Get-Code (Join-Path $UiDir $file) }
    $codeCache[$file]
}

# LOW-1: no macro (#define/#undef) anywhere under the App tree may rename a token the regexes trust.
# REVIEW_LCDFX7: P4 only the three real role enumerators are trusted (LCD_PIN_ROLE_COUNT etc. are fine); P3 App/test
# is host-test code (stubs are normal there, it is never linked into the firmware) and is excluded; X1-X4 the scan
# runs on line-spliced text and tolerates comments between '#', 'define' and the name and the '%:' digraph; X8 a
# macro whose replacement list mentions an opener/gate function (an alias) is refused too.
$trusted = 'ui_lcd_lock_\w+|ui_lcd_lock_run_gated|ui_lcd_lock_has_role|LCD_PIN_ROLE_NONE|LCD_PIN_ROLE_USER|LCD_PIN_ROLE_ADMIN|kiln_ui_show|ui_page_safety_open|ui_page_safety_build|lcd_safety_strip_needs_pin|touch_cal_store_is_calibrated|lcd_touch_cal_\w*exit_target|\w+_open_apply|trip_strip_gated_open_cb|trip_strip_clicked_cb|' + (($rules | Where-Object { $_[4] } | ForEach-Object { [regex]::Escape($_[4]) } | Select-Object -Unique) -join '|')
$openerTok = 'kiln_ui_show|ui_page_safety_open|ui_lcd_lock_run_gated'
$appRoot = Split-Path (Split-Path $UiDir)
if (-not (Test-Path (Join-Path $appRoot 'drivers'))) { $appRoot = $UiDir }
$appFiles = @(Get-ChildItem -Path $appRoot -Recurse -Include *.c, *.h -File | Where-Object { $_.FullName -notmatch '[\\/]test[\\/]' })
$ws = '(?:[ \t]|/\*[^\n]*?\*/)*'
$hash = '[ \t]*(?:#|%:)'
foreach ($f in $appFiles) {
    $raw = ((Get-Content -Raw -Path $f.FullName) -replace "`r", '').Replace("\`n", '')
    foreach ($mm in [regex]::Matches($raw, '(?m)^' + $hash + $ws + '(define|undef)' + $ws + '(' + $trusted + ')\b')) {
        Write-Host "FAIL: L1-macro: $($f.Name) $($mm.Groups[1].Value)s '$($mm.Groups[2].Value)' (a macro can rename the gate tokens this check trusts; rename your macro)"; $fail++
    }
    foreach ($mm in [regex]::Matches($raw, '(?m)^' + $hash + $ws + 'define' + $ws + '\w+(?:\([^)\n]*\))?[^\n]*\b(' + $openerTok + ')\b')) {
        Write-Host "FAIL: L1-macro: $($f.Name) defines a macro mentioning '$($mm.Groups[1].Value)' (an alias hides the opener/gate call from this check)"; $fail++
    }
}
# X5: a build-system compile definition can rename a gate token just like a #define. No CMake file may mention one.
$cmakeFiles = @(Get-ChildItem -Path $appRoot -Recurse -Include CMakeLists.txt, *.cmake -File)
$kilnRoot = Split-Path $appRoot
if ($kilnRoot -and (Test-Path $kilnRoot)) { $cmakeFiles += @(Get-ChildItem -Path $kilnRoot -File -Include CMakeLists.txt, *.cmake -ErrorAction SilentlyContinue) }
foreach ($f in $cmakeFiles) {
    $ct = Get-Content -Raw -Path $f.FullName
    foreach ($mm in [regex]::Matches($ct, '\b(' + $openerTok + '|ui_lcd_lock_has_role|LCD_PIN_ROLE_\w+|\w+_open_apply)\b')) {
        Write-Host "FAIL: L1-cmake: $($f.FullName.Substring($kilnRoot.Length)) mentions '$($mm.Groups[1].Value)' (a compile definition can rename a gate token; this check only reads C sources)"; $fail++
    }
}
# X19: the role enum is pinned: NONE must stay 0 and USER/ADMIN must follow, in order, with no explicit values.
$authH = Join-Path $UiDir 'lcd_auth_state.h'
if (Test-Path $authH) {
    $ah = Get-Code $authH
    if ($ah -notmatch 'typedef\s+enum\s*\{\s*LCD_PIN_ROLE_NONE\s*=\s*0\s*,\s*LCD_PIN_ROLE_USER\s*,\s*LCD_PIN_ROLE_ADMIN\s*,?\s*\}\s*lcd_pin_role_t\s*;') { Write-Host "FAIL: L1-enum: lcd_auth_state.h lcd_pin_role_t is not exactly { NONE = 0, USER, ADMIN } (an explicit value can make USER equal NONE)"; $fail++ }
} else { Write-Host "FAIL: L1-enum: lcd_auth_state.h missing"; $fail++ }

$regChecked = @{}
foreach ($r in $rules) {
    $path = Join-Path $UiDir $r[0]
    if (-not (Test-Path $path)) { Write-Host "FAIL: $($r[3]): missing $($r[0])"; $fail++; continue }
    $txt = Get-CachedCode $r[0]
    $fb = Get-FuncBody $txt $r[1]
    if (-not $fb.ok) { Write-Host "FAIL: $($r[3]): $($r[0]) has $($fb.n) static definitions of $($r[1])() (expected exactly 1, and it must be `static` so no other file can call it; if you renamed it, edit its row in tools/check_lcd_admin_gates.ps1)"; $fail++; continue }
    if ($fb.body -match '(?m)^[ \t]*(?:#|%:)') { Write-Host "FAIL: $($r[3]): $($r[1])() in $($r[0]) contains a preprocessor directive (#if 0 / #define can disable or rename the gate; none is allowed inside a gated function)"; $fail++ }
    if ($fb.body -notmatch $r[2]) { Write-Host "FAIL: $($r[3]): $($r[1])() in $($r[0]) lacks the gate matching /$($r[2])/ (if you only reformatted the code, update this row's regex in tools/check_lcd_admin_gates.ps1)"; $fail++ }
    elseif ($r[2].StartsWith('ui_lcd_lock_run_gated')) {
        # MED-1 (X13): a bare gate row is anchored to the function body: nothing before the gate may return/jump
        # (a call plus `return;` ahead of an intact gate leaves the gate as dead code) or call anything.
        $gm = [regex]::Match($fb.body, $r[2])
        $pre = $fb.body.Substring(0, $gm.Index)
        # LCDFX8 MED-2: the call allow-list runs over ALL pre-gate text, including guard conditions and braced
        # blocks, BEFORE they are stripped for the return/goto test.
        $callRx = '\b(?!if\b|while\b|for\b|switch\b|sizeof\b|snprintf\b|lv_event_get_\w+|touch_cal_store_is_calibrated\b)\w+\s*\('
        $preChk = if ($preCallOk.ContainsKey($r[3])) { [regex]::Replace($pre, '\b' + $preCallOk[$r[3]] + '\s*\(', '(') } else { $pre }
        if ($preChk -match $callRx) { Write-Host "FAIL: $($r[3]): $($r[1])() in $($r[0]) calls '$($Matches[0])' before the gate (also inside guards/blocks); the gate must be the first call (edit the row if this is intended)"; $fail++ }
        # LCDFX8 MED-1: the gate statement ends the function body unless the row is a known exception.
        if ($r[3] -notin $afterGateOk -and $fb.body.Substring($gm.Index + $gm.Length) -notmatch '^[^;]*;\s*$') { Write-Host "FAIL: $($r[3]): $($r[1])() in $($r[0]) has code after the gate statement (the gate must be the last statement; if intended add the function to `$afterGateOk in tools/check_lcd_admin_gates.ps1)"; $fail++ }
        $pre = [regex]::Replace($pre, '\bif\s*\([^;{}]*\)\s*return\s*;', ' ')   # single-statement refusal guards are fine
        while ($pre -match '\{[^{}]*\}') { $pre = [regex]::Replace($pre, '\{[^{}]*\}', ' ') }   # nested blocks (early-out branches) are fine
        if ($pre -match '\b(return|goto)\b') { Write-Host "FAIL: $($r[3]): $($r[1])() in $($r[0]) runs code before the gate at top level that returns or calls something ('$($pre.Trim())'); the gate must be the first call (edit the row if this is intended)"; $fail++ }
    }
    # MED-1 (X14): a gated event callback must be registered, and ONLY through lv_obj_add_event_cb (no ungated twin
    # registered instead, no `(void)cb;` dummy reference keeping the dead gated copy alive).
    if ($r[1] -match '_cb$' -and $r[1] -notmatch '_unlocked_cb$|^refresh_cb$|^open_decide_gated_cb$|^touch_cal_nav_cb$' -and -not $regChecked.ContainsKey($r[0] + ':' + $r[1])) {
        $regChecked[$r[0] + ':' + $r[1]] = 1
        $nu = Count-Uses $txt $r[1]
        $nr = @([regex]::Matches($txt, '(?:\blv_obj_add_event_cb\s*\(\s*(?:[^,;()]|\([^()]*\))+,|\.on_confirm\s*=)\s*' + [regex]::Escape($r[1]) + '\s*,')).Count
        if ($nr -lt 1 -or $nr -ne $nu) { Write-Host "FAIL: $($r[3]): $($r[1])() in $($r[0]) has $nu use(s) outside its definition but $nr lv_obj_add_event_cb registration(s); the gated callback must be what the widget registers, and nothing else may reference it"; $fail++ }
    }
    if ($r[4]) {
        $u = Count-Uses $txt $r[4]
        if ($u -ne $r[5]) { Write-Host "FAIL: $($r[3]): $($r[4]) is referenced $u time(s) in $($r[0]) outside its definition (expected $($r[5]): only the gated call; an ungated call or registration bypasses the gate)"; $fail++ }
    }
}
# C3 (REVIEW_LCDFX3 LOW-3, REVIEW_LCDFX4 LOW-1..4, INFO-1, REVIEW_LCDFX5 LOW-2/INFO-1/2): every callback registered through
# build_nav_item(grid, "...", cb) in ui_page_config.c is enumerated. Each USER callback's WHOLE body
# must be exactly one `ui_lcd_lock_run_gated("...", LCD_PIN_ROLE_USER, <page>_open_apply, NULL);`
# (optionally preceded by (void)e;) -- no comma operator, no arithmetic on the role, no code around it.
# The per-page rule is checked against that callback's own body. The ADMIN exemption is only
# touch_cal_nav_cb by name with its body pinned. Every preprocessor conditional is refused in the file
# (to add a #if, add a pinned exemption here), a callback defined twice fails, and any registration form
# not recognised fails loud. The openers are inventoried: each *_open_apply body is pinned, each page
# opener (kiln_ui_show / ui_page_safety_open) appears only in its own *_open_apply, and each *_open_apply
# identifier appears exactly twice (definition + its one gated call).
$cfg = Get-CachedCode "ui_page_config.c"
if ($cfg -match '(?m)^\s*(?:#|%:)\s*(if|ifdef|ifndef|elif|else)\b') { Write-Host "FAIL: C3: ui_page_config.c contains a preprocessor conditional (dead code can hide an ungated twin; if one is needed, extend this check)"; $fail++ }
$navCalls = @([regex]::Matches($cfg, '\bbuild_nav_item\s*\(')).Count - @([regex]::Matches($cfg, '\bbuild_nav_item\s*\(\s*(const\s+)?lv_obj_t')).Count   # minus definition and prototypes
$navRe = @([regex]::Matches($cfg, '\bbuild_nav_item\(\s*\w+\s*,\s*"[^"]*"\s*,\s*(\w+)\s*\)'))
if ($navRe.Count -ne $navCalls) { Write-Host "FAIL: C3: $navCalls build_nav_item call(s) but only $($navRe.Count) in a recognised form (cast, non-literal label or array entry?)"; $fail++ }
# Registrars: every lv_obj_add_event_cb must be the recognised row form; other registrars are refused.
$regTotal = @([regex]::Matches($cfg, '\blv_obj_add_event_cb\s*\(')).Count
$regOk = 0
foreach ($m in [regex]::Matches($cfg, '\blv_obj_add_event_cb\(\s*\w+\s*,\s*(\w+)\s*,')) {
    $v = $m.Groups[1].Value
    if ($v -in 'cb', 'units_toggle_cb') { $regOk++ } else { Write-Host "FAIL: C3: unrecognised lv_obj_add_event_cb registration of '$v' in ui_page_config.c"; $fail++ }
}
if ($regTotal -ne $regOk) { Write-Host "FAIL: C3: $regTotal lv_obj_add_event_cb call(s) in ui_page_config.c but only $regOk in the recognised 'cb'/'units_toggle_cb' form"; $fail++ }
if ($cfg -match '\b(lv_obj_add_event|lv_timer_create|lv_async_call|lv_anim_set_\w*cb|lv_timer_set_cb)\b') { Write-Host "FAIL: C3: ui_page_config.c uses a callback registrar ($($Matches[1])) other than the pinned lv_obj_add_event_cb forms"; $fail++ }
$navN = 0; $applies = @{}
$cbs = @($navRe | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
foreach ($cb in $cbs) {
    $fb = Get-FuncBody $cfg $cb
    if (-not $fb.ok) { Write-Host "FAIL: C3: ui_page_config.c nav callback $cb has $($fb.n) definitions (expected exactly 1)"; $fail++; continue }
    $body = $fb.body
    $um = [regex]::Match($body, '^\s*(\(void\)\s*\w+\s*;\s*)?ui_lcd_lock_run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*(\w+_open_apply),\s*NULL\)\s*;\s*$')
    if ($um.Success) { $navN++; $applies[$um.Groups[2].Value] = $cb; continue }
    if ($cb -eq 'touch_cal_nav_cb') {
        $tc = '^\s*\(void\)\s*\w+\s*;\s*if\s*\(!touch_cal_store_is_calibrated\(\)\)\s*\{\s*kiln_ui_show\("touch_cal"\)\s*;\s*return\s*;\s*\}\s*ui_lcd_lock_run_gated\("[^"]*",\s*LCD_PIN_ROLE_ADMIN,\s*touch_cal_open_apply,\s*NULL\)\s*;\s*$'
        if ($body -notmatch $tc) { Write-Host "FAIL: C3: touch_cal_nav_cb body is not the pinned shape (ADMIN gate with nothing else)"; $fail++ }
        continue
    }
    Write-Host "FAIL: C3: ui_page_config.c nav callback $cb is not exactly one USER gate statement (only touch_cal_nav_cb may differ, as pinned; for a new cell edit this check)"; $fail++
}
if ($navN -ne 5) { Write-Host "FAIL: C3: expected exactly 5 USER-gated nav callbacks in ui_page_config.c, found $navN (adding a USER cell? bump this count and the page table below)"; $fail++ }
$pageOf = @{ temperature_open_apply = 'temperature'; network_open_apply = 'network'; diagnostics_open_apply = 'diagnostics'; safety_open_apply = $null; profiles_open_apply = 'profiles'; touch_cal_open_apply = 'touch_cal' }
foreach ($ap in $pageOf.Keys) {
    if ($ap -ne 'touch_cal_open_apply' -and -not $applies.ContainsKey($ap)) { Write-Host "FAIL: C3: no USER-gated nav callback body calls $ap"; $fail++ }
    $fb = Get-FuncBody $cfg $ap
    if (-not $fb.ok) { Write-Host "FAIL: C3: $ap has $($fb.n) definitions in ui_page_config.c (expected 1)"; $fail++; continue }
    $page = $pageOf[$ap]
    $shape = if ($null -eq $page) { '^\s*\(void\)\s*user_data\s*;\s*ui_page_safety_open\(true\)\s*;\s*$' }
             elseif ($ap -eq 'profiles_open_apply') { '^\s*\(void\)\s*user_data\s*;\s*ui_page_profiles_refresh\(\)\s*;\s*kiln_ui_show\("profiles"\)\s*;\s*$' }
             else { '^\s*\(void\)\s*user_data\s*;\s*kiln_ui_show\("' + $page + '"\)\s*;\s*$' }
    if ($fb.body -notmatch $shape) { Write-Host "FAIL: C3: $ap body is not the pinned opener shape /$shape/ (INFO-2: an apply body must only open its page)"; $fail++ }
    if ((Count-Uses $cfg $ap) -ne 1) { Write-Host "FAIL: C3: $ap is referenced $(Count-Uses $cfg $ap) time(s) outside its definition (expected 1: the gated call; an ungated call or cast registration bypasses the gate)"; $fail++ }
}
foreach ($pg in 'temperature', 'network', 'diagnostics', 'profiles') {
    $c = @([regex]::Matches($cfg, 'kiln_ui_show\s*\(\s*"' + $pg + '"')).Count
    if ($c -ne 1) { Write-Host "FAIL: C3: kiln_ui_show(`"$pg`") appears $c times in ui_page_config.c (expected 1, inside ${pg}_open_apply)"; $fail++ }
}
if (@([regex]::Matches($cfg, '\bui_page_safety_open\s*\(')).Count -ne 1) { Write-Host "FAIL: C3: ui_page_safety_open( must appear exactly once in ui_page_config.c (inside safety_open_apply)"; $fail++ }
if (@([regex]::Matches($cfg, 'kiln_ui_show\s*\(\s*"touch_cal"')).Count -ne 2) { Write-Host "FAIL: C3: kiln_ui_show(`"touch_cal`") must appear exactly twice in ui_page_config.c (touch_cal_open_apply and the pinned uncalibrated branch)"; $fail++ }
if (@([regex]::Matches($cfg, '\bkiln_ui_show\s*\(')).Count -ne 6) { Write-Host "FAIL: C3: ui_page_config.c has an unexpected kiln_ui_show( call (expected exactly the 6 pinned ones)"; $fail++ }

# LCDFX4 LOW-4 / LCDFX5 LOW-3: the Home trip strip is the other way into Safety; pin its whole handler and the
# opener's references.
$homeSrc = Get-CachedCode "ui_page_home.c"
if ($homeSrc -match '(?m)^\s*(?:#|%:)\s*(if|ifdef|ifndef|elif|else)\b') { Write-Host "FAIL: C4: ui_page_home.c contains a preprocessor conditional (dead code can hide an ungated twin of trip_strip_clicked_cb)"; $fail++ }
$hf = Get-FuncBody $homeSrc 'trip_strip_clicked_cb'
$hpin = '^\s*\(void\)\s*\w+\s*;\s*if\s*\(s_ui_home_trip_strip_is_safety\)\s*\{\s*if\s*\(lcd_safety_strip_needs_pin\(ui_lcd_lock_has_role\(LCD_PIN_ROLE_USER\)\)\)\s*\{\s*ui_lcd_lock_run_gated\("[^"]*",\s*LCD_PIN_ROLE_USER,\s*trip_strip_gated_open_cb,\s*NULL\)\s*;\s*\}\s*else\s*\{\s*trip_strip_gated_open_cb\(NULL\)\s*;\s*\}\s*\}\s*$'
# Log statements cannot open a page, so they may be added without editing this check (LCDFX5 INFO-1).
$hbody = if ($hf.ok) { [regex]::Replace($hf.body, '\bESP_LOG[IWEDV]\s*\([^;]*\)\s*;', '') } else { '' }
if (-not $hf.ok -or $hbody -notmatch $hpin) { Write-Host "FAIL: C4: ui_page_home.c trip_strip_clicked_cb is not the pinned PIN-predicate shape (exactly one definition; only ESP_LOG* lines may be added)"; $fail++ }
$gf = Get-FuncBody $homeSrc 'trip_strip_gated_open_cb'
if (-not $gf.ok -or $gf.body -notmatch '^\s*\(void\)\s*user_data\s*;\s*ui_page_safety_open\(false\)\s*;\s*$') { Write-Host "FAIL: C4: ui_page_home.c trip_strip_gated_open_cb is not the pinned opener shape"; $fail++ }
if (@([regex]::Matches($homeSrc, '\bui_page_safety_open\s*\(')).Count -ne 1) { Write-Host "FAIL: C4: ui_page_home.c must call ui_page_safety_open( exactly once (inside trip_strip_gated_open_cb)"; $fail++ }
if ((Count-Uses $homeSrc 'trip_strip_gated_open_cb') -ne 2) { Write-Host "FAIL: C4: trip_strip_gated_open_cb must be referenced exactly twice outside its definition (gated argument + else-branch call), found $(Count-Uses $homeSrc 'trip_strip_gated_open_cb')"; $fail++ }
if ((Count-Uses $homeSrc 'trip_strip_clicked_cb') -ne 1) { Write-Host "FAIL: C4: trip_strip_clicked_cb must be referenced exactly once outside its definition (the strip registration), found $(Count-Uses $homeSrc 'trip_strip_clicked_cb')"; $fail++ }
if ($homeSrc -match 'kiln_ui_show\s*\(\s*"(safety|diagnostics|temperature|network|profiles)"') { Write-Host "FAIL: C4: ui_page_home.c must not open $($Matches[1]) via kiln_ui_show (only the pinned PIN-gated trip strip may reach Safety)"; $fail++ }

# LCDFX5 LOW-4: repo-wide inventory of the ways into USER pages across drivers/ui. Allowlist = file:page -> count.
$userPages = 'safety|diagnostics|temperature|network|profiles'
$showAllow = @{ 'ui_page_config.c:temperature' = 1; 'ui_page_config.c:network' = 1; 'ui_page_config.c:diagnostics' = 1; 'ui_page_config.c:profiles' = 1
                'ui_page_profile_builder_review.c:profiles' = 1; 'ui_page_safety.c:safety' = 1 }   # after an ADMIN-gated save
$safetyOpenAllow = @{ 'ui_page_config.c' = 1; 'ui_page_home.c' = 1; 'ui_page_safety.c' = 1; 'ui_page_safety.h' = 1 }   # safety.c/.h: definition + prototype
$showNonLiteralFiles = 'ui_topbar.c', 'ui_page_touch_cal.c', 'kiln_ui.c'                       # back-nav / host-tested exit targets / the definition
$backAllow = @('ui_page_network_manage.c:network', 'ui_page_profile_builder_zones.c:profiles')   # in-flow Back from ADMIN-gated pages
$backNonLiteralFiles = 'ui_page_profile_detail.c', 'ui_page_profile_picker.c', 'ui_page_safety.c'   # in-flow back targets
$seen = @{}
# REVIEW_LCDFX7 X9: the whole App tree (minus host-test code) is inventoried, not only drivers/ui. A file that
# mentions none of the opener tokens is skipped before the (slower) comment/concatenation pass.
$openerPre = 'kiln_ui_show|ui_page_safety_open|back_page|ui_page_safety_build|clear_trip|##'
$safetyBuildRefs = 0
foreach ($f in $appFiles) {
    if ((Get-Content -Raw -Path $f.FullName) -notmatch $openerPre) { continue }
    $code = Get-Code $f.FullName; $nm = $f.Name
    if ($nm -notin 'ui_page_safety.c', 'ui_page_safety.h') {
        $sb = @([regex]::Matches($code, '\bui_page_safety_build\b')).Count
        if ($sb -gt 0 -and $nm -ne 'kiln_ui.c') { Write-Host "FAIL: L4: $nm references ui_page_safety_build (only kiln_ui.c's single kiln_ui_register_page(`"safety`", ...) may; a second registration is a second name for the Safety page)"; $fail++ }
        if ($nm -eq 'kiln_ui.c') { $safetyBuildRefs = $sb; if ($sb -ne 1 -or $code -notmatch 'kiln_ui_register_page\(\s*"safety"\s*,\s*ui_page_safety_build\s*\)') { Write-Host "FAIL: L4: kiln_ui.c must reference ui_page_safety_build exactly once, as kiln_ui_register_page(`"safety`", ui_page_safety_build) (found $sb)"; $fail++ } }
        if ($nm -ne 'ui_page_safety.c' -and $f.FullName.StartsWith($UiDir) -and $code -match '\b(dashboard_safety_clear_trip|safety_link_send_clear_trip)\s*\(') { Write-Host "FAIL: L4: $nm calls a clear_trip function '$($Matches[0])' (the LCD trip clear is only ui_page_safety.c's gated clear_action; non-UI callers belong in http/ or bridge code, not under App/drivers/ui)"; $fail++ }
    }
    if ($f.FullName.StartsWith($UiDir) -and $code -match '##') { Write-Host "FAIL: L4: $nm uses the ## token-paste operator (it can build an opener name this check cannot see)"; $fail++ }
    foreach ($mm in [regex]::Matches($code, 'kiln_ui_show\s*\(\s*"(' + $userPages + ')"')) { $k = $nm + ':' + $mm.Groups[1].Value; $seen[$k] = 1 + [int]$seen[$k] }
    if ($nm -ne 'ui_page_safety.c') {
        foreach ($mm in [regex]::Matches($code, '\bkiln_ui_show\s*\(\s*(?!")(?!const\b)([^\s)])')) { if ($nm -notin $showNonLiteralFiles) { Write-Host "FAIL: L4: $nm calls kiln_ui_show with a non-literal page (only $($showNonLiteralFiles -join ', ') may)"; $fail++ } }
    }
    $so = @([regex]::Matches($code, '\bui_page_safety_open\s*\(')).Count
    if ($so -ne [int]$safetyOpenAllow[$nm]) { Write-Host "FAIL: L4: $nm has $so ui_page_safety_open( reference(s), allowlist says $([int]$safetyOpenAllow[$nm]) (a new way into Safety needs a gate and an allowlist entry here)"; $fail++ }
    foreach ($mm in [regex]::Matches($code, '\bback_page\s*=\s*([^;]*?)\s*[,;\n]')) {
        $v = $mm.Groups[1].Value
        if ($v -match '"(' + $userPages + ')"' -and (($nm + ':' + $Matches[1]) -notin $backAllow)) { Write-Host "FAIL: L4: $nm sets back_page to USER page $($Matches[1]) (the topbar Back button navigates with no PIN)"; $fail++ }
        elseif ($v -notmatch '"' -and $v -ne 'NULL' -and $nm -notin $backNonLiteralFiles) { Write-Host "FAIL: L4: $nm sets back_page to non-literal '$v' (only $($backNonLiteralFiles -join ', ') may)"; $fail++ }
    }
}
foreach ($k in $seen.Keys) { if ($seen[$k] -ne [int]$showAllow[$k]) { Write-Host "FAIL: L4: $k opens a USER page $($seen[$k]) time(s), allowlist says $([int]$showAllow[$k]) (add the gate, then an allowlist entry in this check)"; $fail++ } }
foreach ($k in $showAllow.Keys) { if (-not $seen.ContainsKey($k)) { Write-Host "FAIL: L4: allowlisted opener $k no longer present (update the allowlist)"; $fail++ } }

foreach ($lf in ($script:logFails | Select-Object -Unique)) { Write-Host "FAIL: L-log: $lf"; $fail++ }

# REVIEW_LCDFX8 (LCDFX9 fixes) HIGH-1/2/3, MED-1/2, LOW-1: sink inventory by IDENTIFIER MENTION, deny by default.
# Every mention (call, function pointer, #define alias, ESP_LOG argument, header wrapper) of an admin-mutator name ($denyIdent)
# in any drivers/ui .c or .h file is counted per (file, identifier, enclosing function). The count must equal the row below
# exactly, so an unlisted mutator, a second mention, an address-of or a wrapper fails; a row whose mention vanished fails too
# (no stale rows). Each listed function is a gated apply/worker (see the per-function gate rows above); add a caller here ONLY
# with an ADMIN gate. '<global>' = a mention outside any function (definition/prototype/#define).
$sinkRows = @(
    @('ui_edit_firing_apply.c', 'edit_firing_apply', '<global>', 1),
    @('ui_edit_firing_apply.h', 'edit_firing_apply', '<global>', 1),
    @('ui_page_config.c', 'unit_pref_set_ex', 'units_toggle_apply', 1),
    @('ui_page_diagnostics.c', 'crash_report_acknowledge_timeout', 'crash_ack_do', 1),
    @('ui_page_edit_firing.c', 'edit_firing_apply', 'apply_live_gated', 1),
    @('ui_page_live_decide.c', 'profiles_live_decide_apply', 'run_decision_apply', 1),
    @('ui_page_network.c', 'wifi_prov_get_ap_password', 'ap_edit_open_apply', 1),   # LCDFX9 MED-2: ADMIN-gated editor prefill
    @('ui_page_network.c', 'wifi_prov_get_ap_password', 'refresh_cb', 1),            # pinned below: revealed only in the unmasked ADMIN branch
    @('ui_page_network.c', 'wifi_prov_set_ap_password', 'ap_identity_worker_task', 1),
    @('ui_page_network.c', 'wifi_prov_set_ap_ssid', 'ap_identity_worker_task', 1),
    @('ui_page_network.c', 'wifi_prov_set_mode', 'mode_worker_task', 1),
    @('ui_page_network_manage.c', 'wifi_prov_add_network', 'connect_worker_task', 1),
    @('ui_page_network_manage.c', 'wifi_prov_forget_network', 'forget_worker_task', 1),
    @('ui_page_profile_builder_review.c', 'profiles_http_save_expect_rev', 'do_save_apply', 1),
    @('ui_page_profile_picker.c', 'profiles_favorites_set', 'delete_do', 1),
    @('ui_page_profile_picker.c', 'profiles_http_delete', 'delete_do', 1),
    @('ui_page_safety.c', 'dashboard_safety_clear_trip', 'clear_action', 1),
    @('ui_page_temperature.c', 'dashboard_set_relay', 'relay_toggle_apply', 1),
    @('ui_page_touch_cal.c', 'touch_cal_store_save', 'finish_calibration', 1)
)
function Get-TopFuncRanges([string]$code) {
    $out = New-Object System.Collections.ArrayList
    foreach ($m in [regex]::Matches($code, '(?m)^[A-Za-z_][^;{}()#]*?\b(\w+)\s*\([^;{}]*\)\s*\{')) {
        $open = $m.Index + $m.Length - 1
        $b = Get-BraceBody $code $open
        if ($null -eq $b) { continue }
        [void]$out.Add(@{ name = $m.Groups[1].Value; start = $m.Index; end = $open + 1 + $b.Length + 1; open = $open })
    }
    $out
}
$keepCache = @{}
function Get-KeepLogsCode([string]$path) {
    if (-not $keepCache.ContainsKey($path)) { $keepCache[$path] = Get-CodeFromText (Get-Content -Raw -Path $path) (Split-Path $path -Leaf) -KeepLogs }
    $keepCache[$path]
}
$uiAll = @(Get-ChildItem -Path $UiDir -File | Where-Object { $_.Extension -in '.c', '.h' })
$sinkSeen = @{}
foreach ($uf in $uiAll) {
    $kc = Blank-Strings (Get-KeepLogsCode $uf.FullName)
    $mm = @([regex]::Matches($kc, '\b' + $denyIdent + '\b'))
    if ($mm.Count -eq 0) { continue }
    $ranges = @(Get-TopFuncRanges $kc)
    foreach ($x in $mm) {
        $fnName = '<global>'
        foreach ($rg in $ranges) { if ($x.Index -gt $rg.open -and $x.Index -lt $rg.end) { $fnName = $rg.name; $dirBody = $kc.Substring($rg.open, $rg.end - $rg.open); break } }
        if ($fnName -ne '<global>' -and $dirBody -match '(?m)^[ \t]*(?:#|%:)') { Write-Host "FAIL: L-sink: $($uf.Name) function $fnName() reaches $($x.Value) but contains a preprocessor directive (#if 0 / #define can disable its gate)"; $fail++ }
        $k = "$($uf.Name)|$($x.Value)|$fnName"
        $sinkSeen[$k] = 1 + [int]$sinkSeen[$k]
    }
}
$sinkWant = @{}
foreach ($sr in $sinkRows) { $sinkWant["$($sr[0])|$($sr[1])|$($sr[2])"] = [int]$sr[3] }
if ($DumpSinks) { foreach ($k in ($sinkSeen.Keys | Sort-Object)) { Write-Host "SINK $k $($sinkSeen[$k])" } }
foreach ($k in $sinkSeen.Keys) {
    if (-not $sinkWant.ContainsKey($k)) { Write-Host "FAIL: L-sink: '$k' ($($sinkSeen[$k]) mention(s)) is an admin mutator mentioned in drivers/ui outside the inventory (any mention counts: call, function pointer, #define alias, log argument, header wrapper). A new use needs an ADMIN gate and a row in `$sinkRows in tools/check_lcd_admin_gates.ps1"; $fail++ }
    elseif ($sinkWant[$k] -ne $sinkSeen[$k]) { Write-Host "FAIL: L-sink: '$k' is mentioned $($sinkSeen[$k]) time(s), inventory says $($sinkWant[$k]) (a second mention is a second path to the mutator)"; $fail++ }
}
foreach ($k in $sinkWant.Keys) { if (-not $sinkSeen.ContainsKey($k)) { Write-Host "FAIL: L-sink: inventory row '$k' no longer present (sink moved or removed; update `$sinkRows)"; $fail++ } }

# HIGH-4: gate impostors. Only ui_lcd_lock.c/.h may define or declare a ui_lcd_lock_* function; every other drivers/ui file
# must include "ui_lcd_lock.h" if it uses the gate, and may mention the gate functions only as plain calls.
$kwEx = '(?!(?:return|else|case|goto|do)\b)'
foreach ($uf in $uiAll) {
    if ($uf.Name -in 'ui_lcd_lock.c', 'ui_lcd_lock.h') { continue }
    $gc = Blank-Strings (Get-KeepLogsCode $uf.FullName)
    if ($gc -match ('(?m)^[ \t]*' + $kwEx + '(?:static\s+|extern\s+|inline\s+)*[A-Za-z_][\w \t\*]*?\bui_lcd_lock_\w+\s*\(')) { Write-Host "FAIL: L-gate-impostor: $($uf.Name) defines/declares a ui_lcd_lock_* function (only ui_lcd_lock.c may; a local copy of the gate would not gate)"; $fail++ }
    foreach ($gm in [regex]::Matches($gc, '\b(?:ui_lcd_lock_run_gated|ui_lcd_lock_has_role)\b(?!\s*\()')) { Write-Host "FAIL: L-gate-impostor: $($uf.Name) mentions $($gm.Value) other than as a direct call (alias/function pointer/shadow)"; $fail++ }
    $hasInc = [regex]::IsMatch((Get-KeepLogsCode $uf.FullName), '(?m)^[ \t]*#[ \t]*include[ \t]+"ui_lcd_lock\.h"')
    if ($gc -match '\b(?:ui_lcd_lock_run_gated|ui_lcd_lock_has_role)\s*\(' -and -not $hasInc) { Write-Host "FAIL: L-gate-impostor: $($uf.Name) uses the LCD gate but does not include `"ui_lcd_lock.h`" (the gate symbols must come from the real header)"; $fail++ }
}

# MED-2: a wrapper OUTSIDE drivers/ui (drivers/http, persist, ...) that reaches an admin mutator must not be called from the
# UI. Every function the UI calls that is defined elsewhere in App is parsed; if its body mentions $denyIdent (and it is not
# itself a listed mutator), it is refused. (Direct, one-level; a deeper chain ends in a $denyIdent name at some level.)
$uiCalled = @{}
foreach ($uf in $uiAll) {
    foreach ($cm in [regex]::Matches((Blank-Strings (Get-KeepLogsCode $uf.FullName)), '\b([A-Za-z_]\w*)\s*\(')) { $uiCalled[$cm.Groups[1].Value] = $uf.Name }
}
$wrapAllow = @('wifi_prov_scan', 'profile_executor_run')   # read-only scan request: posts a scan command through wifi_prov_post_and_wait, changes no setting; profile_executor_run is the LCD profile-detail Start button (a USER action by design, the executor applies its own refusals)
foreach ($of in $appFiles) {
    if ($of.FullName.StartsWith($UiDir)) { continue }
    $rawo = Get-Content -Raw -Path $of.FullName
    if ($rawo -notmatch $denyIdent) { continue }
    $co = Get-CodeFromText $rawo $of.Name -KeepLogs
    $cb = Blank-Strings $co
    foreach ($fr in (Get-TopFuncRanges $cb)) {
        if (-not $uiCalled.ContainsKey($fr.name)) { continue }
        if ($fr.name -match ('^' + $denyIdent + '$') -or $fr.name -in $wrapAllow) { continue }
        $bodyo = $cb.Substring($fr.open, $fr.end - $fr.open)
        $hit = [regex]::Match($bodyo, '\b' + $denyIdent + '\b')
        if ($hit.Success) { Write-Host "FAIL: L-wrapper: $($uiCalled[$fr.name]) calls $($fr.name)() (defined in $($of.Name)), whose body reaches admin mutator $($hit.Value); call the gated path directly or add the wrapper to a reviewed allowlist"; $fail++ }
    }
}
# Use counts of the private appliers the gates reach (definition excluded): delete_do/crash_ack_do are reached from the
# ungated-role branch of the click handler and from the PIN-success callback, forget_worker_task only from the task create.
foreach ($uc in @(@('ui_page_profile_picker.c', 'delete_do', 2), @('ui_page_diagnostics.c', 'crash_ack_do', 2), @('ui_page_network_manage.c', 'forget_worker_task', 1))) {
    $un = Count-Uses (Get-CachedCode $uc[0]) $uc[1]
    if ($un -ne $uc[2]) { Write-Host "FAIL: L-sink: $($uc[1]) is referenced $un time(s) in $($uc[0]) outside its definition (expected $($uc[2]); a third call/reference bypasses the gate rows above)"; $fail++ }
}


# ---- REVIEW_LCDFX9 structural additions -------------------------------------------------------------------------------
# HIGH-3: a lone CR is a line terminator for GCC but is deleted by this script's normalisation, so `// note<CR>call();`
# hides a live call inside what the check reads as one comment. No legitimate source has one.
foreach ($f in $appFiles) {
    if ([regex]::IsMatch((Get-Content -Raw -Path $f.FullName), "`r(?!`n)")) { Write-Host "FAIL: L-cr: $($f.Name) contains a lone CR (GCC ends a // comment at it, this check does not; convert to LF or CRLF)"; $fail++ }
}
# HIGH-1: no declaration or definition of a gate token inside drivers/ui outside its home file (a block-scope
# `const lcd_pin_role_t LCD_PIN_ROLE_ADMIN = LCD_PIN_ROLE_USER;` or a local function pointer named like a predicate compiles
# silently and turns an ADMIN gate into a USER gate). Backstop for what regex cannot see: -Werror=shadow, asserted below.
$gateTok = 'LCD_PIN_ROLE_\w+|ui_lcd_lock_\w+|kiln_ui_show|ui_page_safety_open|touch_cal_store_is_calibrated|ui_safety_clear_verdict|UI_SAFETY_CLEAR_\w+|lcd_safety_strip_needs_pin|lcd_touch_cal_\w*exit_target'
$gateHome = @(
    @('^LCD_PIN_ROLE_', @('lcd_auth_state.h')), @('^ui_lcd_lock_', @('ui_lcd_lock.c', 'ui_lcd_lock.h')),
    @('^kiln_ui_show$', @('kiln_ui.c', 'kiln_ui.h')), @('^ui_page_safety_open$', @('ui_page_safety.c', 'ui_page_safety.h')),
    @('^(ui_safety_clear_verdict|UI_SAFETY_CLEAR_)', @('ui_page_safety_logic.c', 'ui_page_safety_logic.h')),
    @('^lcd_safety_strip_needs_pin$', @('ui_page_safety_logic.c', 'ui_page_safety_logic.h', 'lcd_auth_state.c', 'lcd_auth_state.h')),
    @('^lcd_touch_cal_\w*exit_target$', @('ui_page_touch_cal.c', 'ui_page_touch_cal.h', 'ui_page_touch_cal_logic.c', 'ui_page_touch_cal_logic.h', 'lcd_auth_state.c', 'lcd_auth_state.h'))
)
$declRx = @(
    ('\b(?!return\b|case\b|else\b|goto\b|do\b|sizeof\b|typeof\b|defined\b)[A-Za-z_]\w*[\s\*]+(' + $gateTok + ')\b\s*(?:=(?!=)|;|\[|,|\)|\()'),   # `type T = / ; / , / ) / (`
    ('\(\s*\*\s*(' + $gateTok + ')\s*\)'),   # function-pointer declarator
    ('[{,]\s*(' + $gateTok + ')\s*=(?!=)'),   # enumerator
    ('\btypedef\b[^;]*\b(' + $gateTok + ')\s*;')
)
foreach ($uf in $uiAll) {
    $dc = Blank-Strings (Get-KeepLogsCode $uf.FullName)
    foreach ($rx in $declRx) {
        foreach ($dm in [regex]::Matches($dc, $rx)) {
            $tok = $dm.Groups[1].Value
            $isHome = $false
            foreach ($h in $gateHome) { if ($tok -match $h[0] -and $uf.Name -in $h[1]) { $isHome = $true } }
            if (-not $isHome) { Write-Host "FAIL: L-declarator: $($uf.Name) declares/defines gate token '$tok' (`"$($dm.Value.Trim() -replace '\s+', ' ')`"); a local, parameter, enumerator, typedef or function pointer with a gate name shadows the real one and defeats the gate rows"; $fail++ }
        }
    }
}
# HIGH-1 backstop: drivers/ui must compile with -Werror=shadow (the compiler sees what the text rules cannot).
$drvCmake = Join-Path $appRoot 'drivers\CMakeLists.txt'
if (Test-Path $drvCmake) {
    $dct = Get-Content -Raw -Path $drvCmake
    if ($dct -notmatch '(?m)^\s*file\(GLOB\s+(\w+)\s+"\$\{CMAKE_CURRENT_LIST_DIR\}/ui/\*\.c"\)\s*\r?\n\s*set_source_files_properties\(\s*\$\{\1\}\s+PROPERTIES\s+COMPILE_OPTIONS\s+"-Werror=shadow"\s*\)') {
        Write-Host "FAIL: L-shadow: App/drivers/CMakeLists.txt no longer applies -Werror=shadow to every drivers/ui/*.c (file(GLOB ... ui/*.c) + set_source_files_properties(... COMPILE_OPTIONS `"-Werror=shadow`")); the shadow flag is the compiler backstop for gate-token shadowing"; $fail++
    }
    if ($dct -match '(?i)-Wno-shadow|-Wno-error=shadow') { Write-Host "FAIL: L-shadow: App/drivers/CMakeLists.txt turns shadow warnings off"; $fail++ }
}
# LOW-1 / REVIEW_LCDFX10 LOW-5: nothing in drivers/ui may be weak (a strong definition elsewhere would silently replace it at link time).
foreach ($wf in $uiAll) {
    if ((Remove-CComments ((Get-Content -Raw -Path $wf.FullName) -replace "`r", '')) -match '\bweak\b|__weak__|__attribute__\s*\(\(\s*(?:__)?weak') { Write-Host "FAIL: L-weak: $($wf.Name) mentions weak (a weak gate/page function can be overridden by a strong definition in another file at link time)"; $fail++ }
}
# REVIEW_LCDFX10 LOW-2 / REVIEW_LCDFX11 LOW-3: a diagnostic pragma naming shadow (or any _Pragma) can switch -Werror=shadow off; the code
# scan strips pragmas, so look at the raw text, after joining backslash-newline line splices. Any _Pragma at all is refused (no App
# source uses one; a stringizing macro such as _Pragma(#x) hides its argument). The scan also covers every App header a drivers/ui
# source #includes by quoted name (a header outside drivers/ui is compiled into the same -Werror=shadow translation unit).
$hdrIndex = @{}
# REVIEW_LCDFX12 INFO-3: App/test stub headers repeat real header names (lvgl_port.h, nvs.h); never index them.
foreach ($hf in (Get-ChildItem -Path $appRoot -Recurse -File -Filter *.h -ErrorAction SilentlyContinue | Where-Object { $_.FullName -notmatch '[\/]test[\/]' })) { if (-not $hdrIndex.ContainsKey($hf.Name)) { $hdrIndex[$hf.Name] = $hf.FullName } }
$pragmaScan = @{}
$pragmaText = @{}
function Get-PragmaText([string]$path) {
    if (-not $pragmaText.ContainsKey($path)) { $pragmaText[$path] = Remove-CComments ((Get-Content -Raw -Path $path) -replace "`r", '') }
    $pragmaText[$path]
}
# REVIEW_LCDFX12 LOW-2: follow #include "..." transitively (bounded depth, visited set), not just the direct includes.
$pragQ = New-Object System.Collections.Queue
foreach ($pf in $uiAll) { $pragmaScan[$pf.FullName] = $pf.Name; $pragQ.Enqueue(@($pf.FullName, $pf.Name, 0)) }
while ($pragQ.Count -gt 0) {
    $cur = $pragQ.Dequeue()
    if ($cur[2] -ge 12) { continue }
    foreach ($im in [regex]::Matches((Get-PragmaText $cur[0]), '(?m)^\s*#\s*include\s*"([^"]+)"')) {
        $hn = [IO.Path]::GetFileName($im.Groups[1].Value)
        if ($hdrIndex.ContainsKey($hn) -and -not $pragmaScan.ContainsKey($hdrIndex[$hn])) {
            $pragmaScan[$hdrIndex[$hn]] = "$hn (included via $($cur[1]))"
            $pragQ.Enqueue(@($hdrIndex[$hn], $hn, ($cur[2] + 1)))
        }
    }
}
foreach ($pk in $pragmaScan.Keys) {
    $praw = (Get-Content -Raw -Path $pk) -replace '\\\r?\n', ''
    if ($praw -match '(?is)#\s*pragma\s+(GCC|clang)\s+diagnostic[^\r\n]*shadow|_Pragma\s*\(|__pragma|#\s*pragma\s+GCC\s+(?:system_header|poison|optimize)') { Write-Host "FAIL: L-shadow: $($pragmaScan[$pk]) has a diagnostic pragma naming shadow, a _Pragma, or a GCC system_header/poison/optimize pragma; it can switch -Werror=shadow off"; $fail++ }
}
# MED-2: the AP password is revealed only in refresh_cb's unmasked ADMIN branch (and the ADMIN-gated editor prefill; both
# are sink rows above, so Home or any other page reading it fails the inventory). The masked branch (all a USER sees)
# must show string literals only.
# REVIEW_LCDFX11 MED-1: the rules below track whatever local(s) receive wifi_prov_get_ap_password() in refresh_cb, not a literal name.
$nw = Get-CachedCode 'ui_page_network.c'
$rcb = Get-FuncBody $nw 'refresh_cb'
if (-not $rcb.ok) { Write-Host "FAIL: L-secret: ui_page_network.c refresh_cb not found exactly once"; $fail++ }
else {
    $fetchRx = 'const\s+char\s*\*\s*(\w+)\s*=\s*wifi_prov_get_ap_password\(\)\s*;'
    $pwNames = @([regex]::Matches($rcb.body, $fetchRx) | ForEach-Object { $_.Groups[1].Value } | Select-Object -Unique)
    $nFetch = @([regex]::Matches($rcb.body, 'wifi_prov_get_ap_password')).Count
    if ($pwNames.Count -ne 1 -or $nFetch -ne 1) { Write-Host "FAIL: L-secret: refresh_cb must contain exactly one wifi_prov_get_ap_password() call, assigned to a 'const char *<local>' declaration (found $nFetch call(s), $($pwNames.Count) tracked local(s)); the L-secret rules track that local by name"; $fail++ }
    else {
    $pw = [regex]::Escape($pwNames[0])
    # Only the emptiness test of the local may be exempt: !X[0], X[0] ? .., X[0] ==/!= 0 or '\0', if/while (X[0]), X[0] &&/||.
    # Never &X[0] (the full password), and never X[0] passed as a value.
    $exRx = "(?<![&\w])(?:!\s*$pw\[0\]|$pw\[0\](?=\s*(?:\?|&&|\|\||[=!]=\s*(?:0|'[^']*')))|(?<=\b(?:if|while)\s*\(\s*)$pw\[0\](?=\s*\)))"
    # REVIEW_LCDFX12 LOW-1: string literals are blanked (same length) before brace matching; exactly one gate declaration and one
    # `if (!show_secret)` must exist in refresh_cb.
    $rb = Blank-Strings $rcb.body
    $nDecl = @([regex]::Matches($rb, '\bbool\s+show_secret\b')).Count
    $ifMs = @([regex]::Matches($rb, 'if\s*\(\s*!\s*show_secret\s*\)\s*\{'))
    if ($nDecl -ne 1 -or $ifMs.Count -ne 1) { Write-Host "FAIL: L-secret: refresh_cb must declare 'bool show_secret' exactly once and contain exactly one 'if (!show_secret) {' block (found $nDecl declaration(s), $($ifMs.Count) block(s))"; $fail++ }
    else {
        $ob = $ifMs[0].Index + $ifMs[0].Length - 1
        $depth = 0; $cb = -1
        for ($ci = $ob; $ci -lt $rb.Length; $ci++) { if ($rb[$ci] -eq '{') { $depth++ } elseif ($rb[$ci] -eq '}') { $depth--; if ($depth -eq 0) { $cb = $ci; break } } }
        if ($cb -lt 0) { Write-Host "FAIL: L-secret: refresh_cb's masked '!show_secret' block has no closing brace"; $fail++ }
        else {
        $mbRaw = $rcb.body.Substring($ob + 1, $cb - $ob - 1)
        $mbBlank = $rb.Substring($ob + 1, $cb - $ob - 1)
        $mb = [regex]::Replace($mbRaw, $exRx, '')
        if ($mb -match "\b$pw\b|\bap_password\b|\bap_ssid\b|\bs_ap_qr_uri\b|wifi_prov_get_\w*|build_wifi_qr_uri") { Write-Host "FAIL: L-secret: refresh_cb's masked (non-ADMIN) branch references '$($Matches[0])'; it may show string literals only (the AP password and QR are ADMIN-only)"; $fail++ }
        # REVIEW_LCDFX12 MED-1: allowlist. The masked block is exactly these four statements, in order (labels keep the previous
        # refresh's text, so the masked-label write and the QR hide are required; nothing else may be read or shown).
        $stmtsRaw = New-Object System.Collections.ArrayList
        $st = 0
        for ($ci = 0; $ci -lt $mbBlank.Length; $ci++) { if ($mbBlank[$ci] -eq ';') { [void]$stmtsRaw.Add(($mbRaw.Substring($st, $ci - $st) -replace '\s+', ' ').Trim()); $st = $ci + 1 } }
        $tail = ($mbRaw.Substring($st) -replace '\s+', ' ').Trim()
        $wantStmts = @(
            ('lv_label_set_text(s_ap_password_label, ' + $pwNames[0] + '[0] ? "******** (admin)" : "(open network)")'),
            'lv_obj_add_flag(s_ap_qr, LV_OBJ_FLAG_HIDDEN)',
            "s_ap_qr_last[0] = '\0'",
            'return')
        $okShape = ($tail -eq '' -and $stmtsRaw.Count -eq $wantStmts.Count)
        if ($okShape) { for ($wi = 0; $wi -lt $wantStmts.Count; $wi++) { if ($stmtsRaw[$wi] -ne $wantStmts[$wi]) { $okShape = $false } } }
        if (-not $okShape) { Write-Host "FAIL: L-secret: refresh_cb's masked '!show_secret' block must be exactly: the masked password-label write with the two literals keyed on $($pwNames[0])[0], lv_obj_add_flag(s_ap_qr, LV_OBJ_FLAG_HIDDEN), s_ap_qr_last[0] = '\0', return; (found: $(($stmtsRaw -join '; ')))"; $fail++ }
        # REVIEW_LCDFX10 MED-4 (P2) / REVIEW_LCDFX11 MED-2: the last statement must be a bare, unconditional return;.
        $stmts = @($mbRaw -split ';')
        $lastIdx = -1
        for ($si = $stmts.Count - 1; $si -ge 0; $si--) { if ($stmts[$si].Trim() -ne '') { $lastIdx = $si; break } }
        if ($lastIdx -lt 0 -or $stmts[$lastIdx].Trim() -ne 'return' -or $mbRaw -notmatch '\breturn\s*;\s*$') { Write-Host "FAIL: L-secret: refresh_cb's masked '!show_secret' block does not end with an unconditional 'return;' (it would fall through into the ADMIN reveal)"; $fail++ }
        }
    }
    # REVIEW_LCDFX10 MED-4 (P1): before the gate, the password may only be fetched into its local and emptiness-tested.
    $gi = $rb.IndexOf('bool show_secret')
    if ($gi -ge 0) {
        $pre = $rb.Substring(0, $gi)
        $pre = [regex]::Replace($pre, $fetchRx, '')
        $pre = [regex]::Replace($pre, $exRx, '')
        if ($pre -match "\b$pw\b|\bap_password\b|\bs_ap_qr_uri\b|build_wifi_qr_uri") { Write-Host "FAIL: L-secret: refresh_cb uses '$($Matches[0])' before the show_secret gate (only the single fetch declaration is allowed there)"; $fail++ }
    }
    }
}

# L9: lock init must precede the touch-cal early return.
$kui = Get-CachedCode "kiln_ui.c"
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
Write-Host "LCD admin gate check passed: $($rules.Count) per-function gates plus C3/C4 nav-callback and trip-strip shape, L1 macro scan, L4 opener inventory, L9 init order and relock."
exit 0
