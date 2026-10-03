# check_recovery_hold.ps1 -- builds and runs test_recovery_hold.c (host test of
# recovery_hold.c, the relay-hold watchdog's decision logic) under MSVC, then
# runs negative tests: each mutant (a scratch copy of recovery_hold.c with one
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

$work = Join-Path $env:TEMP "recovery_hold_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

function Build-And-Run {
    param([string]$Impl, [string]$Tag)
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = Join-Path $here "test_recovery_hold.c"
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /WX /std:c11 /I`"$here`" `"$test`" `"$Impl`" /Fe:`"$exe`" /Fo:`"$obj\\`" /Fd:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_hold"
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

# "<NL>" in a needle/replacement stands for the file's own line ending.
function Test-Mutant {
    param([string]$Needle, [string]$Replacement, [string]$Tag)
    $orig = Join-Path $here "recovery_hold.c"
    $src = Get-Content $orig -Raw
    $nl = if ($src.Contains("`r`n")) { "`r`n" } else { "`n" }
    $n = $Needle.Replace("<NL>", $nl)
    $r = $Replacement.Replace("<NL>", $nl)
    $mutant = $src.Replace($n, $r)
    if ($mutant -eq $src) {
        throw "negative test ${Tag}: could not find '$Needle' in recovery_hold.c to mutate -- update this check."
    }
    $mpath = Join-Path $work ("mutant_" + $Tag + "_recovery_hold.c")
    Set-Content -Path $mpath -Value $mutant -Encoding ascii
    $bad = Build-And-Run -Impl $mpath -Tag $Tag
    if ($bad.Exit -eq 0) {
        throw "NEGATIVE TEST FAILED ($Tag): the mutant still passed -- the host test is vacuous for it."
    }
}

try {
    $good = Build-And-Run -Impl (Join-Path $here "recovery_hold.c") -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_hold reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_hold never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 33) { throw "only $passCount assertions ran -- test looks gutted." }

    Test-Mutant -Needle "st->ever_ok = false;" -Replacement "(void)0;" -Tag "initok"
    Test-Mutant -Needle "bool dir_ok = (uint16_t)(dir & out_dir_mask) == 0;" -Replacement "bool dir_ok = true;" -Tag "dirignore"
    Test-Mutant -Needle "bool data_ok = (uint16_t)(data & hold_mask) == 0;" -Replacement "bool data_ok = true;" -Tag "dataignore"
    Test-Mutant -Needle "return dir_ok && data_ok;" -Replacement "return dir_ok || data_ok;" -Tag "andor"
    Test-Mutant -Needle "if (read_ok && rhold_regs_match" -Replacement "if (rhold_regs_match" -Tag "readok"
    Test-Mutant -Needle "if (!st->fault) {" -Replacement "if (1) {" -Tag "firsttime"
    Test-Mutant -Needle "st->fault_seen_s_valid = true;" -Replacement "(void)0;" -Tag "validflag"
    Test-Mutant -Needle "st->mismatch_count++;" -Replacement "(void)0;" -Tag "mismatchcount"
    Test-Mutant -Needle "return RHOLD_REASSERT;" -Replacement "return RHOLD_HEALTHY;" -Tag "action"
    Test-Mutant -Needle "return RHOLD_HEALTHY;" -Replacement "st->fault = false; return RHOLD_HEALTHY;" -Tag "healthyclears"
    Test-Mutant -Needle "st->reassert_fail_count++;" -Replacement "(void)0;" -Tag "failcount"
    Test-Mutant -Needle "// st->fault is deliberately left as-is: it latches until reboot." `
        -Replacement "st->fault = !verified;" -Tag "repairclears"
    Test-Mutant -Needle "if (verified) {<NL>        st->ever_ok = true;<NL>        st->last_ok_s = now_s;" `
        -Replacement "if (verified) {<NL>        st->ever_ok = true;" -Tag "lastok"
    Test-Mutant -Needle "return boot_fault || st->fault;" -Replacement "return boot_fault;" -Tag "effectivefault"
    Test-Mutant -Needle "return boot_verified && !st->fault;" -Replacement "return boot_verified;" -Tag "effectiveverified"

    Write-Host "check_recovery_hold: PASS ($passCount assertions; negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
