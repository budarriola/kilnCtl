# check_recovery_wifi_policy.ps1 -- builds and runs test_recovery_wifi_policy.c
# (host test of recovery_wifi_policy.h plus a source-order scan of
# recovery_wifi.c: a failed esp_wifi_set_storage(RAM) must refuse AP bring-up
# before the passphrase reaches the driver) under MSVC, then runs negative
# tests: each mutant (a scratch copy of the header or recovery_wifi.c with one
# rule broken) must make the same test binary FAIL, else the test is vacuous
# for that rule.
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

$work = Join-Path $env:TEMP "recovery_wifi_policy_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

$realHeader = Join-Path $here "recovery_wifi_policy.h"
$realWifi = Join-Path $here "recovery_wifi.c"
$realHttpHeader = Join-Path $here "recovery_http_policy.h"
$realHttp = Join-Path $here "recovery_http.c"

# $Header: header to build against (copied beside a copy of the test so the
# quoted include resolves to it); $WifiSrc: the recovery_wifi.c to scan.
function Build-And-Run {
    param([string]$Header, [string]$WifiSrc, [string]$Tag, [string]$HttpHeader = $realHttpHeader, [string]$HttpSrc = $realHttp)
    $dir = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Copy-Item $Header (Join-Path $dir "recovery_wifi_policy.h")
    Copy-Item $HttpHeader (Join-Path $dir "recovery_http_policy.h")
    $test = Join-Path $dir "test_recovery_wifi_policy.c"
    Copy-Item (Join-Path $here "test_recovery_wifi_policy.c") $test
    $exe = Join-Path $dir "t.exe"
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && cl /nologo /W3 /WX /std:c11 /I`"$dir`" `"$test`" /Fe:`"$exe`" /Fo:`"$dir\\`" /Fd:`"$dir\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_wifi_policy" -Lane light
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
    $ro = cmd /c "`"$exe`" `"$WifiSrc`" `"$HttpSrc`" 2>&1"
    $rx = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    return @{ Exit = $rx; Output = ($ro -join "`n") }
}


# Mutant of $File (basename in $here): $Needle -> $Replacement; expect FAIL.
function Test-Mutant {
    param([string]$File, [string]$Needle, [string]$Replacement, [string]$Tag)
    $src = (Get-Content (Join-Path $here $File) -Raw).Replace("`r`n", "`n")
    $Needle = $Needle.Replace("`r`n", "`n")
    $Replacement = $Replacement.Replace("`r`n", "`n")
    $mutant = $src.Replace($Needle, $Replacement)
    if ($mutant -eq $src) {
        throw "negative test ${Tag}: could not find '$Needle' in $File to mutate -- update this check."
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_" + $File)
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    if ($File -eq "recovery_http_policy.h") {
        $bad = Build-And-Run -Header $realHeader -WifiSrc $realWifi -Tag $Tag -HttpHeader $mpath
    } elseif ($File -eq "recovery_http.c") {
        $bad = Build-And-Run -Header $realHeader -WifiSrc $realWifi -Tag $Tag -HttpSrc $mpath
    } elseif ($File -like "*.h") {
        $bad = Build-And-Run -Header $mpath -WifiSrc $realWifi -Tag $Tag
    } else {
        $bad = Build-And-Run -Header $realHeader -WifiSrc $mpath -Tag $Tag
    }
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED ($Tag): the mutant still passed -- the host test is vacuous for it."
    }
}

try {
    $good = Build-And-Run -Header $realHeader -WifiSrc $realWifi -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_wifi_policy reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_wifi_policy never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 40) { throw "only $passCount assertions ran -- test looks gutted." }

    # Policy: a failed storage call must refuse.
    Test-Mutant -File "recovery_wifi_policy.h" -Needle "return storage_rc == 0;" -Replacement "return true;" -Tag "policytrue"
    Test-Mutant -File "recovery_wifi_policy.h" -Needle "return storage_rc == 0;" -Replacement "return storage_rc <= 0;" -Tag "policyneg"
    # Source: the gate removed (the old log-only behaviour) must be caught.
    Test-Mutant -File "recovery_wifi.c" -Needle "if (!rwifi_may_configure_ap((int)store_err)) {" -Replacement "if (0) {" -Tag "nogate"
    # Source: the refusal branch must show the LCD state and record the error.
    Test-Mutant -File "recovery_wifi.c" -Needle "recovery_lcd_set_wifi_storage_fail();" -Replacement "(void)0;" -Tag "nolcd"
    Test-Mutant -File "recovery_wifi.c" -Needle "s_error = `"wifi_storage_fail`";" -Replacement "(void)0;" -Tag "noerror"
    # Source: the refusal branch must not configure the AP (log-only old behaviour).
    Test-Mutant -File "recovery_wifi.c" -Needle "(void)esp_wifi_deinit();" -Replacement "(void)esp_wifi_set_config(0, 0);" -Tag "cfginbranch"
    # R2-L1: exit / boot_guard_reset must use the not-applicable-aware clear.
    Test-Mutant -File "recovery_http.c" -Needle "if (!boot_guard_clear_or_na(bg_msg, sizeof(bg_msg))) {
        httpd_resp_set_status(req, `"500 Internal Server Error`");
        return httpd_resp_send(req, bg_msg, HTTPD_RESP_USE_STRLEN);
    }
    esp_err_t serr" -Replacement "if (!clear_boot_guard(bg_msg, sizeof(bg_msg))) {
        httpd_resp_set_status(req, `"500 Internal Server Error`");
        return httpd_resp_send(req, bg_msg, HTTPD_RESP_USE_STRLEN);
    }
    esp_err_t serr" -Tag "exitplain"
    # R2-L2: legacy default-partition erase must stay wired and gate the result.
    Test-Mutant -File "recovery_http.c" -Needle "int legacy_rc = erase_legacy_default_wifi(&legacy_skipped);" -Replacement "int legacy_rc = 0;" -Tag "nolegacy"
    Test-Mutant -File "recovery_http_policy.h" -Needle "&& legacy_rc == 0" -Replacement "" -Tag "legacyignored"

    # Behaviour mutants (review W1-W7): each must fail the source-shape scans.
    Test-Mutant -File "recovery_http.c" -Needle "if (!rhp_wifi_reset_ok(erase_failed" -Replacement "if (rhp_wifi_reset_ok(erase_failed" -Tag "w1negate"
    Test-Mutant -File "recovery_http.c" -Needle "    bool legacy_skipped = false;
    int legacy_rc = erase_legacy_default_wifi(&legacy_skipped);" -Replacement "    bool legacy_skipped = false;
    int legacy_rc = erase_legacy_default_wifi(&legacy_skipped);
    legacy_rc = 0;" -Tag "w2overwrite"
    Test-Mutant -File "recovery_http.c" -Needle "        err = nvs_commit(h);
    }
    nvs_close(h);
    return (int)err;" -Replacement "    }
    nvs_close(h);
    return (int)err;" -Tag "w3nocommit"
    Test-Mutant -File "recovery_http.c" -Needle "    return clear_boot_guard(msg, cap);
}" -Replacement "    return true;
}" -Tag "w4alwaystrue"
    Test-Mutant -File "recovery_http.c" -Needle "if (recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_KILN) {
        snprintf(msg, cap, `"boot_guard not" -Replacement "if (recovery_io_nvs_failed_mask() & RECOVERY_NVS_FAIL_DEFAULT) {
        snprintf(msg, cap, `"boot_guard not" -Tag "w5wrongbit"
    Test-Mutant -File "recovery_http.c" -Needle "    char bg_msg[96];
    if (!boot_guard_clear_or_na(bg_msg, sizeof(bg_msg))) {
        httpd_resp_set_status(req, `"500 Internal Server Error`");
        httpd_resp_set_hdr(req, `"Connection`", `"close`");" -Replacement "    char bg_msg[96];
    if (0) {
        httpd_resp_set_status(req, `"500 Internal Server Error`");
        httpd_resp_set_hdr(req, `"Connection`", `"close`");" -Tag "w6noprecear"
    Test-Mutant -File "recovery_http.c" -Needle "    if (!boot_guard_clear_or_na(bg_msg, sizeof(bg_msg))) {
        httpd_resp_set_status(req, `"500 Internal Server Error`");
        return httpd_resp_send(req, bg_msg, HTTPD_RESP_USE_STRLEN);
    }
    return httpd_resp_sendstr(req, bg_msg);" -Replacement "    (void)boot_guard_clear_or_na(bg_msg, sizeof(bg_msg));
    return httpd_resp_sendstr(req, bg_msg);" -Tag "w7ignoreresult"

    Write-Host "check_recovery_wifi_policy: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
