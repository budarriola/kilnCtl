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
# vcvarsall runs ONCE here, outside the build gate; the gate then covers only cl.
Import-KilnVcvarsEnv -Vcvars $vcvars

$work = Join-Path $env:TEMP "recovery_pico_proto_$PID"
if (Test-Path $work) { Remove-Item -Recurse -Force $work }
New-Item -ItemType Directory -Path $work | Out-Null

function Build-And-Run {
    param([string]$ImplC, [string]$Tag, [string]$TestC = "")
    $exe = Join-Path $work "t_$Tag.exe"
    $obj = Join-Path $work $Tag
    New-Item -ItemType Directory -Path $obj -Force | Out-Null
    $test = if ($TestC) { $TestC } else { Join-Path $here "test_recovery_pico_proto.c" }
    $crc = Join-Path $common "src\kilnlink_crc.c"
    $frm = Join-Path $common "src\kilnlink_frame.c"
    $cmd = "set `"PATH=%PATH%;C:\Program Files (x86)\Microsoft Visual Studio\Installer`" && cl /nologo /W3 /WX /std:c11 /I`"$here`" /I`"$common\include`" `"$test`" `"$ImplC`" `"$crc`" `"$frm`" /Fe:`"$exe`" /Fo:`"$obj\\`" /Fd:`"$obj\\`""
    $gate = Enter-KilnBuildGate -Label "recovery_pico_proto" -Lane light
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
        # A lost COMPLETE reported as a plain failure (which makes the relay ABORT).
        @{ Name = "fin_unknown_as_fail"; Needle = 'return RPP_FIN_UNKNOWN;'; Repl = 'return fin_fail(f, "mutant");' },
        # A stop after END reported as an ordinary abort (which sends ABORT).
        @{ Name = "fin_stop_after_end_aborts"; Needle = 'return f->end_sent_once && !pico_terminal;'; Repl = 'return 0;' },
        @{ Name = "discover_any_status_is_boot"; Needle = 'return st->state == RPP_STATE_IDLE;'; Repl = 'return 1;' },
        @{ Name = "stop_unknown_ignores_pico_terminal"; Needle = 'return f->end_sent_once && !pico_terminal;'; Repl = 'return f->end_sent_once;' },
        @{ Name = "stop_unknown_ignores_end_sent"; Needle = 'return f->end_sent_once && !pico_terminal;'; Repl = 'return !pico_terminal;' },
        @{ Name = "stop_beats_complete"; Needle = 'return fresh->state == RPP_STATE_COMPLETE;'; Repl = 'return 0;' },
        @{ Name = "pace_floor"; Needle = '+ tick_us - 1u'; Repl = '+ 0u' },
        # A bootloader-reported slot labelled as the application's.
        @{ Name = "bootloader_labelled_app"; Needle = 'reported_by_bootloader ? RPP_TARGET_FROM_BOOTLOADER : RPP_TARGET_FROM_APP'; Repl = 'RPP_TARGET_FROM_APP' },
        @{ Name = "target_assumed"; Needle = 't.target_slot = RPP_SLOT_UNKNOWN;'; Repl = 't.target_slot = RPP_SLOT_B;' },
        # Bootloader slot trailer ignored (the ESP falls back to the operator's guess).
        @{ Name = "trailer_ignored"; Needle = 'if (p[15] == gap_count) {'; Repl = 'if (0) {' },
        # Trailer read even after a gap-count clamp (gap bytes mistaken for slots).
        @{ Name = "trailer_after_clamp"; Needle = 'if (p[15] == gap_count) {'; Repl = 'if (1) {' },
        # A slot byte outside 0/1 coerced into a slot.
        @{ Name = "trailer_coerces_slot"; Needle = 'out->active_slot = (p[trailer] == 0u || p[trailer] == 1u) ? (int)p[trailer]'; Repl = 'out->active_slot = (p[trailer] != 0u) ? 1' },
        # Operator slot beats the Pico-reported active slot.
        @{ Name = "operator_beats_reported"; Needle = 'if (app_active_slot == RPP_SLOT_A || app_active_slot == RPP_SLOT_B) {'; Repl = 'if (operator_slot == RPP_SLOT_UNKNOWN && (app_active_slot == RPP_SLOT_A || app_active_slot == RPP_SLOT_B)) {' },
        # FAILED after END no longer ends the transfer as a failure.
        @{ Name = "failed_not_fatal"; Needle = 'return fin_fail(f, "Pico ended the update");'; Repl = 'return RPP_FIN_WAIT;' },
        # The VERIFYING wall-clock cap made unreachable / mis-transitioned.
        @{ Name = "verify_cap_unreachable"; Needle = 'f->verify_elapsed_ms >= RPP_VERIFY_TIMEOUT_MS &&'; Repl = 'f->verify_elapsed_ms >= 0xFFFFFFFFu &&' },
        @{ Name = "verify_cap_as_done"; Needle = 'return RPP_FIN_UNKNOWN;
    }
    if (ev == RPP_EV_QUIET || !st) {'; Repl = 'return RPP_FIN_DONE;
    }
    if (ev == RPP_EV_QUIET || !st) {' },
        # TRIP_PENDING no longer maps to power_cycle.
        @{ Name = "trip_no_power_cycle"; Needle = 'return (st->err & RPP_ERR_TRIP_PENDING) != 0u;'; Repl = 'return 0;' },
        # power_cycle raised for any refusal.
        @{ Name = "power_cycle_any_refusal"; Needle = 'return (st->err & RPP_ERR_TRIP_PENDING) != 0u;'; Repl = 'return 1;' }
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
    # Mutants of the TEST's fake receiver: each reintroduces a behaviour the
    # receivers do not have (or removes one they do), and the same assertions
    # must catch it, else the fake is not what pins the relay to the real thing.
    $tsrc = Get-Content (Join-Path $here "test_recovery_pico_proto.c") -Raw
    $tmutants = @(
        # Early END answered with the gap list (the old, wrong model).
        @{ Name = "fake_end_lists_gaps"; Needle = 'fr_emit(r, RPP_STATE_RECEIVING, 0, NULL, 0); // early-END reply';
           Repl = '{ uint16_t g_[RPP_STATUS_MAX_GAPS]; uint32_t n_ = fr_find_gaps(r, 0, g_, RPP_STATUS_MAX_GAPS); fr_emit(r, RPP_STATE_RECEIVING, 0, g_, n_); }' },
        # The app's 4-deep, once-per-wake queue dropped from the model.
        @{ Name = "fake_no_app_queue"; Needle = '#ifndef FR_NO_APP_QUEUE'; Repl = '#if 0' },
        # The app beaconing IDLE (it never does).
        @{ Name = "fake_app_idles"; Needle = '#define FR_IDLE_ONLY_BOOTLOADER (r->bootloader)'; Repl = '#define FR_IDLE_ONLY_BOOTLOADER (1)' },
        # The bootloader going silent after COMPLETE (it beacons IDLE).
        @{ Name = "fake_boot_silent"; Needle = '#define FR_IDLE_ONLY_BOOTLOADER (r->bootloader)'; Repl = '#define FR_IDLE_ONLY_BOOTLOADER (0)' },
        # The bootloader omitting ERASING (it does send it).
        @{ Name = "fake_boot_no_erasing"; Needle = 'fr_emit(r, RPP_STATE_ERASING, 0, NULL, 0); // once per BEGIN';
           Repl = 'if (!r->bootloader) fr_emit(r, RPP_STATE_ERASING, 0, NULL, 0); // once per BEGIN' },
        # A BEGIN while active ignored (both receivers restart).
        @{ Name = "fake_begin_ignored_active"; Needle = 'r->begins_restarted++;'; Repl = 'r->begins_restarted++; return;' }
    )
    foreach ($m in $tmutants) {
        $mutant = $tsrc.Replace($m.Needle, $m.Repl)
        if ($mutant -eq $tsrc) {
            throw "negative test: could not find '$($m.Needle)' in test_recovery_pico_proto.c to mutate -- update this check."
        }
        $mpath = Join-Path $work "tmutant_$($m.Name).c"
        Set-Content -Path $mpath -Value $mutant -Encoding ascii
        $bad = Build-And-Run -ImplC $impl -Tag $m.Name -TestC $mpath
        if ($bad.Exit -eq 0) {
            throw "NEGATIVE TEST FAILED: the $($m.Name) test-source mutant still passed -- the fake receiver is not pinned."
        }
    }
    Write-Host "check_recovery_pico_proto: PASS ($passCount assertions; $($mutants.Count) impl + $($tmutants.Count) fake-receiver negative-test mutants failed as required)"
    exit 0
}
finally {
    if (Test-Path $work) { Remove-Item -Recurse -Force $work -ErrorAction SilentlyContinue }
}
