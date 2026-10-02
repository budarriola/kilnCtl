# check_recovery_pico_proto.ps1 -- builds and runs test_recovery_pico_proto.c
# (host test of recovery_pico_proto.c: kilnlink framing/stuffing/CRC via the
# shared CommonFW sources, UPDATE_* packers, deframer, UPDATE_STATUS gap parsing,
# image vector/slot/CRC validation, target resolution, gap-round and pacing
# logic) under MSVC, then runs negative tests: three scratch mutants of
# recovery_pico_proto.c (SRAM-end vector bound, gap completion rule, Frame A
# slot bit) must each make the same test binary FAIL, else the test is vacuous.
#
# Contract (tools/run_all_checks.ps1): exit 0 PASS, exit 3 SKIP (no MSVC),
# anything else FAIL. Scratch is PID-keyed under $env:TEMP, deleted in finally.
$ErrorActionPreference = "Stop"

$here = $PSScriptRoot
. (Join-Path $here "..\..\..\tools\build_gate.ps1")
$common = (Resolve-Path (Join-Path $here "..\..\CommonFW")).Path

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

$work = Join-Path $env:TEMP "recovery_pico_proto_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

function Build-And-Run {
    param([string]$ImplC, [string]$Tag)
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = Join-Path $here "test_recovery_pico_proto.c"
    $crc = Join-Path $common "src\kilnlink_crc.c"
    $frm = Join-Path $common "src\kilnlink_frame.c"
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && call `"$vcvars`" x64 >nul && cl /nologo /W3 /WX /std:c11 /I`"$here`" /I`"$common\include`" `"$test`" `"$ImplC`" `"$crc`" `"$frm`" /Fe:`"$exe`" /Fo:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_pico_proto"
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

try {
    $impl = Join-Path $here "recovery_pico_proto.c"
    $good = Build-And-Run -ImplC $impl -Tag "real"
    Write-Host $good.Output
    if ($good.Exit -ne 0) { throw "test_recovery_pico_proto reported failures (exit $($good.Exit))." }
    if ($good.Output -notmatch "RESULT pass=(\d+) fail=0") {
        throw "test_recovery_pico_proto never printed a passing RESULT line."
    }
    $passCount = [int]$Matches[1]
    if ($passCount -lt 100) { throw "only $passCount assertions ran -- test looks gutted." }

    $src = Get-Content $impl -Raw
    $mutants = @(
        @{ Name = "sram_end"; Needle = 'sp > RPP_SRAM_END'; Repl = '0' },
        @{ Name = "slot_bit"; Needle = '(flags2 & 0x10u) ? RPP_SLOT_B : RPP_SLOT_A'; Repl = 'RPP_SLOT_A' },
        # Silence before END is never sent: the old relay's deadlock.
        @{ Name = "fin_quiet_no_end"; Needle = 'if (!f->end_outstanding) {'; Repl = 'if (0) {' },
        @{ Name = "fin_never_done"; Needle = 'return RPP_FIN_DONE;'; Repl = 'return RPP_FIN_WAIT;' },
        @{ Name = "fin_gaps_ignored"; Needle = 'return RPP_FIN_RETRANSMIT;'; Repl = 'return RPP_FIN_SEND_END;' },
        @{ Name = "begin_resend_early"; Needle = 'b->idle_beacons >= RPP_BEGIN_IDLE_BEACONS &&'; Repl = 'b->idle_beacons >= 2u &&' },
        @{ Name = "begin_erasing_ignored"; Needle = 'if (st->state == RPP_STATE_ERASING) {'; Repl = 'if (0) {' },
        @{ Name = "fin_beacon_stall"; Needle = 'return rpp_fin_step(f, RPP_EV_QUIET, NULL, since_end_ms);'; Repl = 'return RPP_FIN_WAIT;' },
        @{ Name = "pace_floor"; Needle = '+ tick_us - 1u'; Repl = '+ 0u' },
        @{ Name = "target_assumed"; Needle = 't.target_slot = RPP_SLOT_UNKNOWN;'; Repl = 't.target_slot = RPP_SLOT_B;' }
    )
    foreach ($m in $mutants) {
        $mutant = $src.Replace($m.Needle, $m.Repl)
        if ($mutant -eq $src) {
            throw "negative test: could not find '$($m.Needle)' in recovery_pico_proto.c to mutate -- update this check."
        }
        $mpath = Join-Path $work "mutant_$($m.Name).c"
        Set-Content -Path $mpath -Value $mutant -Encoding ascii
        $bad = Build-And-Run -ImplC $mpath -Tag $m.Name
        if ($bad.Exit -eq 0) {
            throw "NEGATIVE TEST FAILED: the $($m.Name) mutant still passed -- the host test is vacuous there."
        }
    }
    Write-Host "check_recovery_pico_proto: PASS ($passCount assertions; $($mutants.Count) negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
