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
# REVIEW_LCDFX5 LOW-5: every rule is matched against ONE named function's body (brace-matched), never the
# whole file, so a gate moved into a dead function no longer satisfies it. Fields: file, function, regex
# that must match the body, label, and optionally an apply identifier whose non-definition occurrences in
# the file must equal the expected count (a direct, ungated call of the apply adds an occurrence).
# To change a gated call site: edit its row here (function / apply name / count) in the same change.
# LCDFX8 LOW-1: a case body is straight-line statements then an UNCONDITIONAL return (no if/?:/loops/jumps before it).
$uncond = '(?:(?!\bif\b|\bwhile\b|\bfor\b|\bswitch\b|\bgoto\b|\breturn\b|\?)[^{}])*?\breturn\s*;'
# Bare-gate callbacks allowed to have code after the gate statement (none today; add with a reason).
# Keyed by row label. apply_cb carries two gates (apply, then decide), so each row sees the other gate.
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
    @('ui_page_touch_cal.c', 'finish_calibration', ('if\s*\(touch_cal_store_is_calibrated\(\)\s*&&\s*!ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{[^{}]*\breturn\s*;\s*\}[\s\S]*touch_cal_store_save\('), 'L11 touch cal save', $null, 0),
    @('ui_page_profile_picker.c', 'delete_btn_clicked_cb', ('if\s*\(!ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{\s*' + (GateRx 'Admin PIN to delete profile' $admin 'delete_unlocked_cb') + '[^;]*;\s*return\s*;\s*\}\s*delete_do\(ud\)\s*;\s*$'), 'L11 profile delete', $null, 0),
    @('ui_page_profile_picker.c', 'delete_unlocked_cb', ('^\s*if\s*\(ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{\s*delete_do\('), 'L11 profile delete unlocked', $null, 0),
    @('ui_page_edit_firing.c', 'apply_cb', (GateRx 'Enter admin PIN to apply edit' $admin 'apply_live_gated'), 'L11 live-edit apply', 'apply_live_gated', 1),
    @('ui_page_edit_firing.c', 'apply_cb', (GateRx 'Enter admin PIN to keep/discard edit' $admin 'open_decide_gated_cb'), 'L11 live-edit decide', 'open_decide_gated_cb', 1),
    @('ui_page_edit_firing.c', 'apply_live_gated', ('!ui_lcd_lock_has_role\(' + $admin + '\)\)\s*return\s*;'), 'L11 live-edit apply re-check', $null, 0),
    @('ui_page_diagnostics.c', 'crash_ack_btn_clicked_cb', ('if\s*\(!ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{\s*' + (GateRx 'Admin PIN to acknowledge crash' $admin 'crash_ack_unlocked_cb') + '[^;]*;\s*return\s*;\s*\}\s*crash_ack_do\(\)\s*;\s*$'), 'L11 crash ack', $null, 0),
    @('ui_page_diagnostics.c', 'crash_ack_unlocked_cb', ('if\s*\(ui_lcd_lock_has_role\(' + $admin + '\)\)\s*\{\s*crash_ack_do\(\)'), 'L11 crash ack unlocked', $null, 0),
    # REVIEW_LCDFX6 HIGH-1 (owner decision D2): clearing a trip from the LCD needs an ADMIN PIN, and the action re-checks the
    # role itself. btn_clicked_cb is EXACTLY the ADMIN gate on clear_action (whose only reference is that gate);
    # clear_action's verdict is fed the live ADMIN role, NEEDS_ADMIN/NOT_TRIPPED return before the send, and the send
    # (dashboard_safety_clear_trip) appears once, after the switch. Other files in drivers/ui may not call it (below).
    @('ui_page_safety.c', 'btn_clicked_cb', ('^\s*\(void\)\s*e\s*;\s*' + (GateRx 'Enter admin PIN to clear trip' $admin 'clear_action') + '\s*NULL\)\s*;\s*$'), 'D2 trip clear gate', 'clear_action', 1),
    @('ui_page_safety.c', 'clear_action', ('^\s*\(void\)\s*user\s*;\s*switch\s*\(ui_safety_clear_verdict\(&s_view,\s*ui_lcd_lock_has_role\(' + $admin + '\)\)\)\s*\{\s*case\s+UI_SAFETY_CLEAR_NOT_TRIPPED\s*:' + $uncond + '\s*case\s+UI_SAFETY_CLEAR_NEEDS_ADMIN\s*:' + $uncond + '\s*case\s+UI_SAFETY_CLEAR_SEND\s*:\s*default\s*:\s*break\s*;\s*\}[\s\S]*dashboard_safety_clear_trip\('), 'D2 trip clear re-check', 'dashboard_safety_clear_trip', 1)
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
$logTok = '\b(kiln_ui_show|ui_page_safety_open|ui_lcd_lock_run_gated|ui_lcd_lock_has_role|dashboard_safety_clear_trip|clear_action|\w+_open_apply|\w+_gated_\w+|\w+_unlocked_cb)\b'
$script:logFails = New-Object System.Collections.ArrayList
# REVIEW_LCDFX5 INFO-3: #warning/#error/#pragma/#line text may hold an unbalanced apostrophe that
# would desynchronise the stripper, so those directive lines are blanked first.
# REVIEW_LCDFX7 P1/P7: ESP_LOG* statements are removed from the code (they cannot gate or open anything), unless
# their arguments contain a trusted call/gate token, which fails the check (X15).
function Get-CodeFromText([string]$raw, [string]$name) {
    $t = $raw -replace "`r", ''
    $t = $t.Replace("\`n", '')
    $t = [regex]::Replace($t, '(?m)^[ \t]*(#|%:)[ \t]*(warning|error|pragma|line)\b[^\n]*', '')
    $t = Remove-CComments $t
    # LCDFX8 HIGH-1: attributes are removed here, so Count-Uses needs no `))` lookbehind (which hid every use after `if (f(x))`).
    $t = [regex]::Replace($t, '__attribute__\s*\(\((?:[^()]|\((?:[^()]|\([^()]*\))*\))*\)\)', ' ')
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
$trusted = 'ui_lcd_lock_run_gated|ui_lcd_lock_has_role|LCD_PIN_ROLE_NONE|LCD_PIN_ROLE_USER|LCD_PIN_ROLE_ADMIN|kiln_ui_show|ui_page_safety_open|ui_page_safety_build|lcd_safety_strip_needs_pin|touch_cal_store_is_calibrated|lcd_touch_cal_\w*exit_target|\w+_open_apply|trip_strip_gated_open_cb|trip_strip_clicked_cb|' + (($rules | Where-Object { $_[4] } | ForEach-Object { [regex]::Escape($_[4]) } | Select-Object -Unique) -join '|')
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

# REVIEW_LCDFX7 MED-1: sink inventory. The mutators behind the ADMIN gates may be called from exactly one place each
# under drivers/ui (file : sink -> function whose body holds the single call), so a new helper, callback or second call
# in another function cannot reach them without passing a gate. To add a legitimate caller, edit this table.
$sinkAllow = @(
    @('ui_page_temperature.c',     'dashboard_set_relay',      'relay_toggle_apply'),
    @('ui_page_network_manage.c',  'wifi_prov_forget_network', 'forget_worker_task'),
    @('ui_page_config.c',          'unit_pref_set\w*',         'units_toggle_apply'),
    @('ui_page_touch_cal.c',       'touch_cal_store_save',     'finish_calibration'),
    @('ui_page_safety.c',          'dashboard_safety_clear_trip', 'clear_action')
)
foreach ($uf in @(Get-ChildItem -Path $UiDir -Filter *.c -File)) {
    $ucode = Get-CachedCode $uf.Name
    $ublank = Blank-Strings $ucode
    foreach ($sinkName in 'dashboard_set_relay', 'wifi_prov_forget_network', 'unit_pref_set\w*', 'touch_cal_store_save', 'dashboard_safety_clear_trip', 'safety_link_send_clear_trip') {
        $calls = @([regex]::Matches($ublank, '\b' + $sinkName + '\s*\('))
        if ($calls.Count -eq 0) { continue }
        $allow = @($sinkAllow | Where-Object { $_[0] -eq $uf.Name -and $_[1] -eq $sinkName })
        if ($allow.Count -ne 1 -or $calls.Count -ne 1) { Write-Host "FAIL: L-sink: $($uf.Name) calls $sinkName( $($calls.Count) time(s) (allowed: $(if ($allow.Count) { 'exactly 1 in ' + $allow[0][2] } else { 'none' }); a new caller of a gated mutator needs an ADMIN gate and a row in `$sinkAllow in tools/check_lcd_admin_gates.ps1)"; $fail++; continue }
        $sfb = Get-FuncBody $ucode $allow[0][2]
        if (-not $sfb.ok -or (Blank-Strings $sfb.body) -notmatch ('\b' + $sinkName + '\s*\(')) { Write-Host "FAIL: L-sink: the single $sinkName( call in $($uf.Name) is not inside $($allow[0][2])()"; $fail++ }
    }
}
foreach ($sk in $sinkAllow) {
    if ($sk[1] -match '\w') { continue }
    if (-not ((Blank-Strings (Get-CachedCode $sk[0])) -match ('\b' + $sk[1] + '\s*\('))) { Write-Host "FAIL: L-sink: allowlisted $($sk[0]):$($sk[1]) no longer present (update `$sinkAllow)"; $fail++ }
}
# Use counts of the private appliers the gates reach (definition excluded): delete_do/crash_ack_do are reached from the
# ungated-role branch of the click handler and from the PIN-success callback, forget_worker_task only from the task create.
foreach ($uc in @(@('ui_page_profile_picker.c', 'delete_do', 2), @('ui_page_diagnostics.c', 'crash_ack_do', 2), @('ui_page_network_manage.c', 'forget_worker_task', 1))) {
    $un = Count-Uses (Get-CachedCode $uc[0]) $uc[1]
    if ($un -ne $uc[2]) { Write-Host "FAIL: L-sink: $($uc[1]) is referenced $un time(s) in $($uc[0]) outside its definition (expected $($uc[2]); a third call/reference bypasses the gate rows above)"; $fail++ }
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
