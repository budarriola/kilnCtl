# check_recovery_passphrase.ps1 -- builds and runs test_recovery_passphrase.c
# (host test of the recovery SoftAP passphrase formatter, recovery_passphrase.c)
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

$work = Join-Path $env:TEMP "recovery_passphrase_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

# $Impls: full paths of the .c files linked with the test.
function Build-And-Run {
    param([string[]]$Impls, [string]$Tag)
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = Join-Path $here "test_recovery_passphrase.c"
    $srcs = ($Impls | ForEach-Object { "`"$_`"" }) -join " "
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /WX /std:c11 /I`"$here`" `"$test`" $srcs /Fe:`"$exe`" /Fo:`"$obj\\`" /Fd:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_passphrase"
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
    $impl = Join-Path $here "recovery_passphrase.c"
    $good = Build-And-Run -Impls @($impl) -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_passphrase reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_passphrase never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 300) { throw "only $passCount assertions ran -- test looks gutted." }

    # Modulo bias: a % 31 mapping must be caught by the exact-8-hits check.
    Test-Mutant -File "recovery_passphrase.c" -Needle "rnd[i] & (RPASS_ALPHABET_LEN - 1u)" `
        -Replacement "rnd[i] % 31u" -Others @() -Tag "bias"
    # Reduced reach: only 16 symbols reachable.
    Test-Mutant -File "recovery_passphrase.c" -Needle "rnd[i] & (RPASS_ALPHABET_LEN - 1u)" `
        -Replacement "rnd[i] & 15u" -Others @() -Tag "reach"
    # Every position must use its own random byte.
    Test-Mutant -File "recovery_passphrase.c" -Needle "alphabet[rnd[i]" -Replacement "alphabet[rnd[0]" `
        -Others @() -Tag "position"
    # Ambiguous symbols (0, O, 1, I) must stay out of the alphabet actually used.
    Test-Mutant -File "recovery_passphrase.c" -Needle "static const char alphabet[] = RPASS_ALPHABET;" `
        -Replacement 'static const char alphabet[] = "0123456789ABCDEFGHIJKLMNOPQRSTUV";' -Others @() -Tag "alphabet"
    # Validator: length must be enforced.
    Test-Mutant -File "recovery_passphrase.c" -Needle "strlen(p) != RPASS_LEN" -Replacement "strlen(p) < 1" `
        -Others @() -Tag "vlen"
    # Validator: symbols must be checked.
    Test-Mutant -File "recovery_passphrase.c" -Needle "if (!memchr(RPASS_ALPHABET, p[i], RPASS_ALPHABET_LEN)) {" `
        -Replacement "if (0) {" -Others @() -Tag "vsym"

    Write-Host "check_recovery_passphrase: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
