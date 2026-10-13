# check_recovery_wifi_qr.ps1 -- builds and runs test_recovery_wifi_qr.c
# (host test of the recovery Wi-Fi QR payload builder/encoder wrapper, recovery_wifi_qr.c)
# under MSVC, then runs negative tests: each mutant (a scratch copy of the source
# with one rule broken) must make the same test binary FAIL, else the test is
# vacuous for that rule.
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

$work = Join-Path $env:TEMP "recovery_wifi_qr_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

# $Impls: full paths of the .c files linked with the test.
function Build-And-Run {
    param([string[]]$Impls, [string]$Tag)
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = Join-Path $here "test_recovery_wifi_qr.c"
    $srcs = ($Impls | ForEach-Object { "`"$_`"" }) -join " "
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && cl /nologo /W3 /WX /std:c11 /I`"$here`" `"$test`" $srcs /Fe:`"$exe`" /Fo:`"$obj\\`" /Fd:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_wifi_qr" -Lane light
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
    $ro = cmd /c "`"$exe`" 2>&1"
    $rx = $LASTEXITCODE
    $ErrorActionPreference = "Stop"
    return @{ Exit = $rx; Output = ($ro -join "`n") }
}

# Mutant: replace $Needle with $Replacement in $File (basename in $here), link
# with the real versions of $Others, expect the test to FAIL.
function Test-Mutant {
    param([string]$File, [string]$Needle, [string]$Replacement, [string[]]$Others, [string]$Tag)
    $orig = Join-Path $here $File
    $src = Get-Content $orig -Raw
    $mutant = $src.Replace($Needle, $Replacement)
    if ($mutant -eq $src) {
        throw "negative test ${Tag}: could not find '$Needle' in $File to mutate -- update this check."
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_" + $File)
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    $impls = @($mpath) + ($Others | ForEach-Object { Join-Path $here $_ })
    $bad = Build-And-Run -Impls $impls -Tag $Tag
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED ($Tag): the mutant still passed -- the host test is vacuous for it."
    }
}

try {
    $others = @("third_party\qrcodegen\qrcodegen.c")
    $impl = Join-Path $here "recovery_wifi_qr.c"
    $good = Build-And-Run -Impls @($impl, (Join-Path $here $others[0])) -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_wifi_qr reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_wifi_qr never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 30) { throw "only $passCount assertions ran -- test looks gutted." }

    $qrc = "recovery_wifi_qr.c"
    # Escaping: each special character must be escaped.
    Test-Mutant -File $qrc -Needle "|| c == ':'" -Replacement "" -Others $others -Tag "esc_colon"
    Test-Mutant -File $qrc -Needle "c == '\\' ||" -Replacement "" -Others $others -Tag "esc_backslash"
    Test-Mutant -File $qrc -Needle "|| c == ','" -Replacement "" -Others $others -Tag "esc_comma"
    Test-Mutant -File $qrc -Needle "|| c == ';'" -Replacement "" -Others $others -Tag "esc_semicolon"
    Test-Mutant -File $qrc -Needle '|| c == ''"''' -Replacement "" -Others $others -Tag "esc_quote"
    # Payload grammar.
    Test-Mutant -File $qrc -Needle '"WIFI:T:WPA;S:"' -Replacement '"WIFI:T:WEP;S:"' -Others $others -Tag "auth"
    Test-Mutant -File $qrc -Needle 'put_str(out, cap, &n, ";;", false)' -Replacement 'put_str(out, cap, &n, ";", false)' -Others $others -Tag "tail"
    # Buffer bounds: an overflow must be rejected, not truncated.
    Test-Mutant -File $qrc -Needle '*n + 1 >= cap' -Replacement '*n + 1 > cap' -Others $others -Tag "bound"
    Test-Mutant -File $qrc -Needle 'memset(out, 0, cap);' -Replacement ';' -Others $others -Tag "nowipe"
    # Encoder parameters and scratch wipe.
    Test-Mutant -File $qrc -Needle "qrcodegen_Mask_AUTO, true" -Replacement "qrcodegen_Mask_AUTO, false" -Others $others -Tag "boost"
    Test-Mutant -File $qrc -Needle "qrcodegen_Ecc_LOW" -Replacement "qrcodegen_Ecc_HIGH" -Others $others -Tag "ecc"
    Test-Mutant -File $qrc -Needle "memset(tmp, 0, RQR_BUF_LEN);" -Replacement ";" -Others $others -Tag "tmpwipe"
    # Geometry.
    Test-Mutant -File $qrc -Needle "int x0 = RQR_AREA_PX - ext;" -Replacement "int x0 = 0;" -Others $others -Tag "align"
    Test-Mutant -File $qrc -Needle "my = y / RQR_MODULE_PX - RQR_QUIET;" -Replacement "my = y / RQR_MODULE_PX - RQR_QUIET + 1;" -Others $others -Tag "rowoff"
    Test-Mutant -File $qrc -Needle "out[2 * x] = (uint8_t)(c >> 8);" -Replacement "out[2 * x] = (uint8_t)(c & 0xFF);" -Others $others -Tag "endian"

    Write-Host "check_recovery_wifi_qr: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
