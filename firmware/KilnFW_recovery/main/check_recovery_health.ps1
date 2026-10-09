# check_recovery_health.ps1 -- builds and runs test_recovery_health.c (host test
# of recovery_health_policy.h plus source scans of recovery_wifi.c,
# recovery_http.c and recovery_main.c: the image must not report "up" for a
# SoftAP/HTTP server that does not exist, must restart after repeated AP_STOP or
# a never-starting httpd, and must record a distinct error per Wi-Fi bring-up
# failure) under MSVC, then runs negative tests: each mutant (a scratch copy of
# one source with one rule broken) must make the same test binary FAIL, else
# the test is vacuous for that rule.
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

$work = Join-Path $env:TEMP "recovery_health_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

$realHeader = Join-Path $here "recovery_health_policy.h"
$realSrc = @{
    "recovery_wifi.c"  = Join-Path $here "recovery_wifi.c"
    "recovery_http.c"  = Join-Path $here "recovery_http.c"
    "recovery_main.c"  = Join-Path $here "recovery_main.c"
    "recovery_health.c" = Join-Path $here "recovery_health.c"
}

# $Header: header to build against; $Src: map of the three sources to scan.
function Build-And-Run {
    param([string]$Header, [hashtable]$Src, [string]$Tag)
    $dir = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Copy-Item $Header (Join-Path $dir "recovery_health_policy.h")
    $test = Join-Path $dir "test_recovery_health.c"
    Copy-Item (Join-Path $here "test_recovery_health.c") $test
    $exe = Join-Path $dir "t.exe"
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /WX /std:c11 /I`"$dir`" `"$test`" /Fe:`"$exe`" /Fo:`"$dir\\`" /Fd:`"$dir\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_health" -Lane light
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
    $ro = cmd /c "`"$exe`" `"$($Src['recovery_wifi.c'])`" `"$($Src['recovery_http.c'])`" `"$($Src['recovery_main.c'])`" `"$($Src['recovery_health.c'])`" 2>&1"
    $rx = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    return @{ Exit = $rx; Output = ($ro -join "`n") }
}

# Mutant of $File (basename in $here): $Needle -> $Replacement; expect FAIL.
function Test-Mutant {
    param([string]$File, [string]$Needle, [string]$Replacement, [string]$Tag)
    $src = (Get-Content (Join-Path $here $File) -Raw) -replace "`r`n", "`n"
    $nd = $Needle -replace "`r`n", "`n"
    $mutant = $src.Replace($nd, $Replacement)
    if ($mutant -eq $src) {
        throw "negative test ${Tag}: could not find '$Needle' in $File to mutate -- update this check."
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_" + $File)
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    if ($File -like "*.h") {
        $bad = Build-And-Run -Header $mpath -Src $realSrc -Tag $Tag
    } else {
        $m = $realSrc.Clone()
        $m[$File] = $mpath
        $bad = Build-And-Run -Header $realHeader -Src $m -Tag $Tag
    }
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED ($Tag): the mutant still passed -- the host test is vacuous for it."
    }
}

try {
    $good = Build-And-Run -Header $realHeader -Src $realSrc -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_health reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_health never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 60) { throw "only $passCount assertions ran -- test looks gutted." }

    # Policy rules.
    Test-Mutant -File "recovery_health_policy.h" -Needle "return ap_stop_count >= RHEALTH_AP_STOP_RESTART_LIMIT;" -Replacement "return false;" -Tag "neverrestart"
    Test-Mutant -File "recovery_health_policy.h" -Needle "registered == expected;" -Replacement "registered > 0;" -Tag "partialroutes"
    Test-Mutant -File "recovery_health_policy.h" -Needle "return wifi_up && lcd_ok && http_ok;" -Replacement "return wifi_up && lcd_ok;" -Tag "allup"
    # M1: s_up set before the AP_START event / not cleared on AP_STOP / no restart.
    Test-Mutant -File "recovery_wifi.c" -Needle "        s_up = false;`n" -Replacement "" -Tag "stopkeepsup"
    Test-Mutant -File "recovery_wifi.c" -Needle "    // s_up is set by the AP_START handler" -Replacement "    s_up = true;`n    // s_up is set by the AP_START handler" -Tag "upearly"
    Test-Mutant -File "recovery_wifi.c" -Needle "                restart_image_for_wifi();`n            } else if" -Replacement "                (void)0;`n            } else if" -Tag "norestart"
    # M2: a distinct error per path.
    Test-Mutant -File "recovery_wifi.c" -Needle "s_error = `"netif_init_fail`";" -Replacement "(void)0;" -Tag "noerr_netif"
    Test-Mutant -File "recovery_wifi.c" -Needle "s_error = `"wifi_init_fail`";" -Replacement "(void)0;" -Tag "noerr_wifiinit"
    Test-Mutant -File "recovery_wifi.c" -Needle "s_error = `"event_register_fail`";" -Replacement "(void)0;" -Tag "noerr_evreg"
    # H2: http result checked, route results counted, main retries/restarts/gates.
    Test-Mutant -File "recovery_http.c" -Needle "esp_err_t rr = httpd_register_uri_handler(server, &routes[i]);" -Replacement "httpd_register_uri_handler(server, &routes[i]); esp_err_t rr = ESP_OK;" -Tag "ignoreregister"
    Test-Mutant -File "recovery_http.c" -Needle "        // No LCD banner here: the caller retries" -Replacement "        recovery_lcd_set_error(`"HTTP FAILED`", s_http_last_error);`n        // No LCD banner here: the caller retries" -Tag "perattemptbanner"
    Test-Mutant -File "recovery_main.c" -Needle "        recovery_lcd_clear_error();`n" -Replacement "" -Tag "nosuccessclear"
    Test-Mutant -File "recovery_health_policy.h" -Needle "return restarts_so_far < RHEALTH_HTTP_MAX_RESTARTS;" -Replacement "return true;" -Tag "unboundedrestart"
    Test-Mutant -File "recovery_wifi.c" -Needle "                recovery_lcd_set_ap_state(RLCD_AP_RESTARTING);" -Replacement "        (void)0;" -Tag "deadapcreds"
    Test-Mutant -File "recovery_main.c" -Needle "            recovery_health_restart_for_http();" -Replacement "            (void)0;" -Tag "mainnorestart"
    # Round 3: shared cap, shutdown awareness, failed re-raise, stale counts.
    Test-Mutant -File "recovery_health_policy.h" -Needle "return wifi_restarts_so_far < RHEALTH_HTTP_MAX_RESTARTS;" -Replacement "return true;" -Tag "wifiunbounded"
    Test-Mutant -File "recovery_health_policy.h" -Needle "RHEALTH_AP_RESTART_IMAGE
                                                              : RHEALTH_AP_STAY_DOWN;
}

// RTC" -Replacement "RHEALTH_AP_RESTART_IMAGE : RHEALTH_AP_RESTART_IMAGE;
}

// RTC" -Tag "apnostay"
    Test-Mutant -File "recovery_wifi.c" -Needle "        if (s_shutting_down) {`n            break;" -Replacement "        if (0) {`n            break;" -Tag "ignoreshutdown"
    Test-Mutant -File "recovery_wifi.c" -Needle "if (rhealth_ap_reraise_failed_action(recovery_health_wifi_restarts()) ==`n                        RHEALTH_AP_RESTART_IMAGE) {" -Replacement "if (0) {" -Tag "startfailignored"
    Test-Mutant -File "recovery_health.c" -Needle "    if (!s_keep) {" -Replacement "    if (0) {" -Tag "staleCounts"
    Test-Mutant -File "recovery_main.c" -Needle "        recovery_health_clear_http();`n    } else {" -Replacement "    } else {" -Tag "limitkeepcount"
    Test-Mutant -File "recovery_health.c" -Needle "void recovery_health_clear_wifi(void)`n{`n    s_wifi_restarts = 0;" -Replacement "void recovery_health_clear_wifi(void)`n{`n    s_wifi_restarts = s_wifi_restarts;" -Tag "clearwifinoop"
    Test-Mutant -File "recovery_wifi.c" -Needle "    if (s_up && s_ap_stop_count == s_stops_at_arm) {" -Replacement "    if (s_up) {" -Tag "stablenostopcheck"
    Test-Mutant -File "recovery_wifi.c" -Needle "recovery_health_clear_wifi(); // staying up for good; after the decision`n            } else {" -Replacement "`n            } else {" -Tag "staydownnoclear"
    Test-Mutant -File "recovery_wifi.c" -Needle "        arm_stable_timer();`n" -Replacement "" -Tag "nostabletimer"
    Test-Mutant -File "recovery_main.c" -Needle "if (rhealth_all_up(recovery_wifi_is_up(), recovery_lcd_is_ok(), http_ok)) {" -Replacement "if (1) {" -Tag "mainungated"

    Write-Host "check_recovery_health: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
