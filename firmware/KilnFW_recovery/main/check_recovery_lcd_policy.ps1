# check_recovery_lcd_policy.ps1 -- builds and runs test_recovery_lcd_policy.c
# (host test of recovery_lcd_policy.h -- the LCD init retry schedule and the
# boot_guard record classification -- plus source scans of recovery_lcd.c and
# recovery_http.c) under MSVC, then runs negative tests: each mutant (a scratch
# copy of the header, recovery_lcd.c or recovery_http.c with one rule broken)
# must make the same test binary FAIL, else the test is vacuous for that rule.
#
# Contract (tools/run_all_checks.ps1): exit 0 PASS, exit 3 SKIP (no MSVC),
# anything else FAIL. Scratch is PID-keyed under $env:TEMP, deleted in finally.
$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
. (Join-Path $here "..\..\..\tools\build_gate.ps1")

$vswhere = "C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe"
$vcvars = $null
if (Test-Path $vswhere) {
    $installPath = & $vswhere -latest -products * `
        -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 `
        -property installationPath 2>$null
    if ($installPath) {
        $candidate = Join-Path $installPath "VC\Auxiliary\Build\vcvarsall.bat"
        if (Test-Path $candidate) { $vcvars = $candidate }
    }
}
if (-not $vcvars) {
    $fallback = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
    if (Test-Path $fallback) { $vcvars = $fallback }
}
if (-not $vcvars) {
    Write-Host "SKIP: vcvarsall.bat not found -- cannot build the host test with MSVC."
    exit 3
}
# vcvarsall runs ONCE here, outside the build gate; the gate then covers only cl.
Import-KilnVcvarsEnv -Vcvars $vcvars

$work = Join-Path $env:TEMP "recovery_lcd_policy_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

$realHeader = Join-Path $here "recovery_lcd_policy.h"
$realLcd = Join-Path $here "recovery_lcd.c"
$realHttp = Join-Path $here "recovery_http.c"

# $Header: header to build against (copied beside a copy of the test so the
# quoted include resolves to it); $LcdSrc/$HttpSrc: the sources to scan.
function Build-And-Run {
    param([string]$Header, [string]$LcdSrc, [string]$HttpSrc, [string]$Tag, [string]$ExtraName = "", [string]$ExtraPath = "")
    $dir = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Copy-Item $Header (Join-Path $dir "recovery_lcd_policy.h")
    $test = Join-Path $dir "test_recovery_lcd_policy.c"
    Copy-Item (Join-Path $here "test_recovery_lcd_policy.c") $test
    $exe = Join-Path $dir "t.exe"
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && cl /nologo /W3 /WX /std:c11 /I`"$dir`" `"$test`" /Fe:`"$exe`" /Fo:`"$dir\\`" /Fd:`"$dir\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_lcd_policy" -Lane light
    try {
        $ErrorActionPreference = "Continue"
        $bo = cmd /c $cmd 2>&1
        $bx = $LASTEXITCODE
        $ErrorActionPreference = "Stop"
    } finally {
        Exit-KilnBuildGate -Gate $gate
    }
    if ($bx -ne 0) {
        $bo | ForEach-Object { Write-Host $_ }
        throw "cl failed building the $Tag variant (exit $bx)."
    }
    $ErrorActionPreference = "Continue"
    # Every other recovery source is scanned too (no log/printf may carry the passphrase).
    $extra = (Get-ChildItem $here -Filter "recovery_*.c" | Where-Object { $_.Name -ne "recovery_lcd.c" -and $_.Name -ne "recovery_http.c" } | ForEach-Object { $p = $_.FullName; if ($ExtraName -ne "" -and $_.Name -eq $ExtraName) { $p = $ExtraPath }; "`"" + $p + "`"" }) -join " "
    $ro = cmd /c "`"$exe`" `"$LcdSrc`" `"$HttpSrc`" $extra 2>&1"
    $rx = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    return @{ Exit = $rx; Output = ($ro -join "`n") }
}

# Mutant of $File (basename in $here): $Needle -> $Replacement; expect FAIL.
function Test-Mutant {
    param([string]$File, [string]$Needle, [string]$Replacement, [string]$Tag)
    $src = (Get-Content (Join-Path $here $File) -Raw).Replace("`r`n", "`n")
    $mutant = $src.Replace($Needle, $Replacement)
    if ($mutant -eq $src) {
        throw "negative test ${Tag}: could not find '$Needle' in $File to mutate -- update this check."
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_" + $File)
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    if ($File -like "*.h") {
        $bad = Build-And-Run -Header $mpath -LcdSrc $realLcd -HttpSrc $realHttp -Tag $Tag
    } elseif ($File -eq "recovery_lcd.c") {
        $bad = Build-And-Run -Header $realHeader -LcdSrc $mpath -HttpSrc $realHttp -Tag $Tag
    } elseif ($File -eq "recovery_http.c") {
        $bad = Build-And-Run -Header $realHeader -LcdSrc $realLcd -HttpSrc $mpath -Tag $Tag
    } else {
        # any other recovery_*.c is a scanned extra source: swap it in by name
        $bad = Build-And-Run -Header $realHeader -LcdSrc $realLcd -HttpSrc $realHttp -Tag $Tag -ExtraName $File -ExtraPath $mpath
    }
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED ($Tag): the mutant still passed -- the host test is vacuous for it."
    }
}

try {
    $good = Build-And-Run -Header $realHeader -LcdSrc $realLcd -HttpSrc $realHttp -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_lcd_policy reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_lcd_policy never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 60) { throw "only $passCount assertions ran -- test looks gutted." }

    # Policy rules.
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "return expander_err && rereset_done < RLCD_MAX_RERESETS;" -Replacement "return expander_err;" -Tag "uncapped"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "return expander_err && rereset_done < RLCD_MAX_RERESETS;" -Replacement "return rereset_done < RLCD_MAX_RERESETS;" -Tag "spi_rereset"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "return expander_err && rereset_done < RLCD_MAX_RERESETS;" -Replacement "return false;" -Tag "neverrereset"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "return !ok && attempts_done < RLCD_BURST_ATTEMPTS;" -Replacement "return !ok && attempts_done < 1;" -Tag "oneattempt"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "return ready ? RLCD_TICK_NONE : RLCD_TICK_WARN_AND_RETRY;" -Replacement "return RLCD_TICK_NONE;" -Tag "noretry"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "case RLCD_GET_BAD_LENGTH: return RLCD_BG_INVALID;" -Replacement "case RLCD_GET_BAD_LENGTH: return RLCD_BG_NONE;" -Tag "badlen_none"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "return failed_lines == 0;" -Replacement "return true;" -Tag "drawalways"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "LCD NOT READY, passphrase not displayed" -Replacement "LCD ok" -Tag "msg"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "           qr_size > 0;" -Replacement "           true;" -Tag "qr_ignores_netset"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "if (err_head || ap_down) {" -Replacement "if (err_head) {" -Tag "mode_ignores_apdown"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "if (net_none || storage_fail) {" -Replacement "if (net_none) {" -Tag "mode_ignores_storage"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "== RLCD_TEXT_NET_SET &&" -Replacement "!= RLCD_TEXT_ERROR &&" -Tag "qr_visible_in_starting"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "if (phase == RLCD_QR_BEFORE_TEXT && !visible) {" -Replacement "if (phase == RLCD_QR_BEFORE_TEXT && visible) {" -Tag "paint_before_inverted"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "return paint(false) != 0;" -Replacement "return paint(true) != 0;" -Tag "paint_hidden_shows_qr"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "if (phase == RLCD_QR_AFTER_TEXT && visible) {" -Replacement "if (phase == RLCD_QR_AFTER_TEXT) {" -Tag "paint_after_unconditional"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "return paint(true) != 0;" -Replacement "return paint(true) == 0;" -Tag "paint_failure_lost"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "return (qr_visible && y < qr_px && y + h > 0) ? panel_w - qr_px : panel_w;" -Replacement "return panel_w;" -Tag "band_never_clipped"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "qr_visible && y < qr_px && y + h > 0" -Replacement "y < qr_px && y + h > 0" -Tag "band_clipped_when_hidden"
    Test-Mutant -File "recovery_lcd_policy.h" -Needle "y < qr_px && y + h > 0" -Replacement "y < 100 && y + h > 0" -Tag "band_y_bound_short"
    # Source wiring (the draw_status call sites; the helpers above are compiled and run).
    Test-Mutant -File "recovery_lcd.c" -Needle "s_line_errs += rlcd_qr_paint(show_qr, RLCD_QR_BEFORE_TEXT, draw_qr_area);" -Replacement "s_line_errs += rlcd_qr_paint(show_qr, RLCD_QR_AFTER_TEXT, draw_qr_area);" -Tag "lcd_pre_phase_wrong"
    Test-Mutant -File "recovery_lcd.c" -Needle "s_line_errs += rlcd_qr_paint(show_qr, RLCD_QR_BEFORE_TEXT, draw_qr_area);" -Replacement "" -Tag "lcd_no_prepaint_hidden"
    Test-Mutant -File "recovery_lcd.c" -Needle "s_line_errs += rlcd_qr_paint(show_qr, RLCD_QR_AFTER_TEXT, draw_qr_area);" -Replacement "s_line_errs += rlcd_qr_paint(show_qr, RLCD_QR_BEFORE_TEXT, draw_qr_area);" -Tag "lcd_post_phase_wrong"
    Test-Mutant -File "recovery_lcd.c" -Needle "bool show_qr = qr_visible_now();" -Replacement "bool show_qr = true;" -Tag "lcd_qr_shown_while_hidden"
    Test-Mutant -File "recovery_lcd.c" -Needle "    } else if (mode == RLCD_TEXT_NET_SET) {" -Replacement "    } else if (s_net_set) {" -Tag "lcd_text_handcopy_netset"
    Test-Mutant -File "recovery_lcd.c" -Needle "    if (mode == RLCD_TEXT_ERROR) {" -Replacement "    if (s_err_head[0] || mode == RLCD_TEXT_ERROR) {" -Tag "lcd_text_handcopy_err"
    Test-Mutant -File "recovery_lcd.c" -Needle "return rlcd_text_mode(s_err_head[0] != 0, s_ap_down != 0," -Replacement "return rlcd_text_mode(s_err_head[0] != 0, 0," -Tag "lcd_text_ignores_apdown"
    Test-Mutant -File "recovery_lcd.c" -Needle "return rlcd_qr_visible(s_err_head[0] != 0, s_ap_down != 0," -Replacement "return rlcd_qr_visible(s_err_head[0] != 0, 0," -Tag "lcd_qr_ignores_apdown"
    Test-Mutant -File "recovery_lcd.c" -Needle "rlcd_band_width(y, h, qr_visible_now(), PANEL_W, RQR_AREA_PX)" -Replacement "PANEL_W" -Tag "lcd_bands_not_clipped"
    Test-Mutant -File "recovery_lcd.c" -Needle "send_data(s_line, (size_t)w * 2)" -Replacement "send_data(s_line, sizeof(s_line))" -Tag "lcd_band_send_full"
    Test-Mutant -File "recovery_lcd.c" -Needle "        s_ready = false;`n        ESP_LOGE(TAG, `"status draw failed" -Replacement "        ESP_LOGE(TAG, `"status draw failed" -Tag "draw_keeps_ready"
    Test-Mutant -File "recovery_lcd.c" -Needle "ESP_LOGE(TAG, RLCD_NOT_READY_MSG);" -Replacement "(void)0;" -Tag "nowarn"
    Test-Mutant -File "recovery_lcd.c" -Needle "ESP_LOGE(TAG, RLCD_NOT_READY_MSG);" -Replacement "ESP_LOGE(TAG, RLCD_NOT_READY_MSG); ESP_LOGI(TAG, `"pw %s`", s_net_pass);" -Tag "passleak"
    Test-Mutant -File "recovery_lcd.c" -Needle "if (a > 0 && rlcd_rereset_allowed(s_expander_err, s_rereset_count)) {" -Replacement "if (a > 0) {" -Tag "burst_ungated"
    Test-Mutant -File "recovery_lcd.c" -Needle "s_bg_state = rlcd_classify_boot_guard(rc, decoded);" -Replacement "s_bg_state = RLCD_BG_NONE;" -Tag "noclassify"
    # RE-01 restart fence.
    Test-Mutant -File "recovery_http.c" -Needle "    __atomic_store_n(&s_restart_pending, true, __ATOMIC_SEQ_CST);" -Replacement "" -Tag "re01_flag_never_set"
    Test-Mutant -File "recovery_http.c" -Needle "return recovery_apply_busy() || recovery_http_restart_pending();" -Replacement "return recovery_apply_busy();" -Tag "re01_route_ignores_flag"
    Test-Mutant -File "recovery_http.c" -Needle "if (recovery_http_restart_pending() || !recovery_app_write_claim()) {" -Replacement "if (!recovery_app_write_claim()) {" -Tag "re01_ota_esp_unfenced"
    Test-Mutant -File "recovery_apply_esp.c" -Needle "if (recovery_http_restart_pending()) { // RE-01" -Replacement "if (0) { // RE-01" -Tag "re01_apply_start_unfenced"
    Test-Mutant -File "recovery_http.c" -Needle "\`"keepalive_sent\`":%u," -Replacement "\`"ka_x\`":%u," -Tag "re02_status_missing"
    Test-Mutant -File "recovery_http.c" -Needle "static esp_err_t wifi_reset_post(httpd_req_t *req)`n{`n    if (route_blocked()) {`n        return refuse_while_applying(req);`n    }`n    if (recovery_pico_busy()) {" -Replacement "static esp_err_t wifi_reset_post(httpd_req_t *req)`n{`n    if (route_blocked()) {`n        return refuse_while_applying(req);`n    }`n    if (false) {" -Tag "wifi_reset_no_pico_busy"
    Test-Mutant -File "recovery_http.c" -Needle "\`"lcd_ready\`"" -Replacement "\`"lcd_rdy\`"" -Tag "nolcdready"

    Test-Mutant -File "recovery_http.c" -Needle "gen == s_verify_gen" -Replacement "true" -Tag "verify_publish_ungated"
    Test-Mutant -File "recovery_http.c" -Needle "portENTER_CRITICAL(&s_verify_mux)" -Replacement "(void)s_verify_mux" -Tag "verify_unlocked"
    Test-Mutant -File "recovery_install.c" -Needle "recovery_http_app_verify_invalidate();" -Replacement "(void)0;" -Tag "apply_no_invalidate"
    Test-Mutant -File "recovery_install.c" -Needle "if (guard_clear() != 0) {" -Replacement "if (0) {" -Tag "apply_guard_unchecked"

    Write-Host "check_recovery_lcd_policy: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
