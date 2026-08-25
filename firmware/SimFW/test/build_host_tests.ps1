# Builds and runs the host-side unit tests for firmware/SimFW/src/sim/ with
# MSVC, entirely off-target -- no pico-sdk, no FreeRTOS, no hardware. Mirrors
# firmware/SaftyFW/test/build_host_tests.ps1's pattern exactly (docs/PLAN.md
# section 13.1: "everything in src/sim/ is pure and runs on the PC ... same
# MSVC/CMake harness pattern SaftyFW/test uses").
#
# Usage: powershell -File test\build_host_tests.ps1
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$testDir = $PSScriptRoot
$simDir = Join-Path $testDir "..\src\sim"
$tasksDir = Join-Path $testDir "..\src\tasks"
$outDir = Join-Path $testDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "simfw_host_tests.exe"

# CommonFW's `benchproto` library (firmware/CommonFW/include/benchproto/,
# src/benchproto_link.c) -- freestanding C11, zero FreeRTOS/TinyUSB
# dependencies, so it links into this host-test binary for real (unlike
# usb_owner.c/cmd_task.c, which stay out per each test file's own header
# comment). Only test_usb_owner_session_reset_logic.c uses it today.
$commonfwDir = Join-Path $testDir "..\..\CommonFW"
$commonfwIncludeDir = Join-Path $commonfwDir "include"

$sources = @(
    (Join-Path $testDir "test_main.c"),
    (Join-Path $testDir "test_thermal_model.c"),
    (Join-Path $testDir "test_max31856_regs.c"),
    (Join-Path $testDir "test_max31856_resp_image.c"),
    (Join-Path $testDir "test_sine_synth.c"),
    (Join-Path $testDir "test_fault_engine.c"),
    (Join-Path $testDir "test_tc_fault_state.c"),
    (Join-Path $testDir "test_gap_closure_logic.c"),
    (Join-Path $testDir "test_cmd_task_gap_closure.c"),
    (Join-Path $testDir "test_i2c_owner_io_read.c"),
    (Join-Path $testDir "test_i2c_owner_io_set.c"),
    (Join-Path $testDir "test_i2c_owner_apply_cmd_logging.c"),
    (Join-Path $testDir "test_i2c_owner_bus_scan.c"),
    (Join-Path $testDir "test_i2c_owner_bus_scan_probe_len_coverage.c"),
    (Join-Path $testDir "test_k4_gating_logic.c"),
    (Join-Path $testDir "test_ct_calibration.c"),
    (Join-Path $testDir "test_dut_power_domains.c"),
    (Join-Path $testDir "test_ct_i2s_gen.c"),
    (Join-Path $testDir "test_safe_reboot_logic.c"),
    (Join-Path $testDir "test_usb_dual_cdc_logic.c"),
    (Join-Path $testDir "test_cmd_payload_vectors.c"),
    (Join-Path $testDir "test_usb_owner_session_reset_logic.c"),
    (Join-Path $testDir "test_task_stats_encoding.c"),
    (Join-Path $testDir "test_sim_engine_ring_drain_reset.c"),
    (Join-Path $testDir "test_sim_engine_event_seq_monotonic.c"),
    (Join-Path $testDir "test_virtual_simfw_port_drift_coverage.c"),
    (Join-Path $simDir "thermal_model.c"),
    (Join-Path $simDir "max31856_regs.c"),
    (Join-Path $simDir "max31856_resp_image.c"),
    (Join-Path $simDir "sine_synth.c"),
    (Join-Path $simDir "fault_engine.c"),
    (Join-Path $simDir "tc_fault_state.c"),
    (Join-Path $simDir "ct_calibration.c"),
    (Join-Path $simDir "ct_i2s_gen.c"),
    (Join-Path $commonfwDir "src\benchproto_link.c")
)

$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /W4 /WX /EHsc /I `"$simDir`" /I `"$tasksDir`" /I `"$commonfwIncludeDir`" /Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "build failed"
}

& $exe
exit $LASTEXITCODE
