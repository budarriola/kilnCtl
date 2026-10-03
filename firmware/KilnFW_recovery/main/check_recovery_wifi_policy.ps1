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

$work = Join-Path $env:TEMP "recovery_wifi_policy_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

# $Header: header to build against (copied beside a copy of the test so the
# quoted include resolves to it); $WifiSrc: the recovery_wifi.c to scan.
function Build-And-Run {
    param([string]$Header, [string]$WifiSrc, [string]$Tag)
    $dir = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $dir -Force | Out-Null
    Copy-Item $Header (Join-Path $dir "recovery_wifi_policy.h")
    $test = Join-Path $dir "test_recovery_wifi_policy.c"
    Copy-Item (Join-Path $here "test_recovery_wifi_policy.c") $test
    $exe = Join-Path $dir "t.exe"
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /WX /std:c11 /I`"$dir`" `"$test`" /Fe:`"$exe`" /Fo:`"$dir\\`" /Fd:`"$dir\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_wifi_policy"
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
    $ro = cmd /c "`"$exe`" `"$WifiSrc`" 2>&1"
    $rx = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    return @{ Exit = $rx; Output = ($ro -join "`n") }
}

$realHeader = Join-Path $here "recovery_wifi_policy.h"
$realWifi = Join-Path $here "recovery_wifi.c"

# Mutant of $File (basename in $here): $Needle -> $Replacement; expect FAIL.
function Test-Mutant {
    param([string]$File, [string]$Needle, [string]$Replacement, [string]$Tag)
    $src = Get-Content (Join-Path $here $File) -Raw
    $mutant = $src.Replace($Needle, $Replacement)
    if ($mutant -eq $src) {
        throw "negative test ${Tag}: could not find '$Needle' in $File to mutate -- update this check."
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_" + $File)
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    if ($File -like "*.h") {
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
    if ($passCount -lt 25) { throw "only $passCount assertions ran -- test looks gutted." }

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

    Write-Host "check_recovery_wifi_policy: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
