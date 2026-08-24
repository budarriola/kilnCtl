# Builds and runs the host-side unit tests for safety_guards.c with MSVC,
# entirely off-target -- no pico-sdk, no FreeRTOS, no hardware. Mirrors
# firmware/KilnFW/App/test/build_host_tests.ps1's pattern. TODO.md Phase 4.
#
# Usage: powershell -File test\build_host_tests.ps1
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$testDir = $PSScriptRoot
$srcDir = Join-Path $testDir "..\src"
$bootDir = Join-Path $testDir "..\bootloader"
$updateDir = Join-Path $testDir "..\src\update"
$commonSrcDir = Join-Path $testDir "..\..\CommonFW\src"
$commonIncDir = Join-Path $testDir "..\..\CommonFW\include"
$outDir = Join-Path $testDir "build"
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "saftyfw_host_tests.exe"

$sources = @(
    (Join-Path $testDir "test_main.c"),
    (Join-Path $testDir "test_safety_guards.c"),
    (Join-Path $testDir "test_link_frame.c"),
    (Join-Path $testDir "test_link_frame_wire.c"),
    (Join-Path $testDir "test_bootloader_metadata.c"),
    (Join-Path $testDir "test_update.c"),
    (Join-Path $testDir "test_kilnlink_power.c"),
    (Join-Path $testDir "test_tx_watermark.c"),
    (Join-Path $testDir "test_relay_grace.c"),
    (Join-Path $testDir "test_config_store.c"),
    (Join-Path $testDir "test_ct_amps_cal.c"),
    (Join-Path $testDir "test_snapshots.c"),
    (Join-Path $testDir "test_boot_checkin_coverage.c"),
    (Join-Path $testDir "test_watchdog_budget_coverage.c"),
    (Join-Path $testDir "test_watchdog_gate.c"),
    (Join-Path $srcDir "safety_guards.c"),
    (Join-Path $srcDir "config_store.c"),
    (Join-Path $srcDir "config_params.c"),
    (Join-Path $srcDir "ct_amps_cal.c"),
    (Join-Path $srcDir "tasks\link_frame.c"),
    (Join-Path $srcDir "tasks\tx_watermark.c"),
    (Join-Path $srcDir "tasks\relay_grace.c"),
    (Join-Path $srcDir "tasks\watchdog_gate.c"),
    (Join-Path $bootDir "crc32.c"),
    (Join-Path $bootDir "metadata.c"),
    (Join-Path $updateDir "image_header.c"),
    (Join-Path $updateDir "received_ranges.c"),
    (Join-Path $updateDir "update_receiver.c"),
    (Join-Path $updateDir "confirm.c"),
    (Join-Path $commonSrcDir "kilnlink_power.c"),
    (Join-Path $commonSrcDir "kilnlink_frame.c"),
    (Join-Path $commonSrcDir "kilnlink_crc.c"),
    # docs/COMMISSIONING.md section 2 wire family -- config_params.c calls
    # into kilnlink_param_value.c's shared type-tag codec; the frame codecs
    # themselves (set_param/commit_config/get_param/param/get_config_page/
    # config_page/set_log_level) are exercised directly by config_params.c's
    # host tests and by test_link_frame.c's dispatch-id checks.
    (Join-Path $commonSrcDir "kilnlink_param_value.c"),
    (Join-Path $commonSrcDir "kilnlink_set_param.c"),
    (Join-Path $commonSrcDir "kilnlink_commit_config.c"),
    (Join-Path $commonSrcDir "kilnlink_get_param.c"),
    (Join-Path $commonSrcDir "kilnlink_param.c"),
    (Join-Path $commonSrcDir "kilnlink_get_config_page.c"),
    (Join-Path $commonSrcDir "kilnlink_config_page.c"),
    (Join-Path $commonSrcDir "kilnlink_set_log_level.c"),
    (Join-Path $commonSrcDir "kilnlink_commit_config_rejected.c"),
    (Join-Path $commonSrcDir "kilnlink_inject_tc.c"),
    (Join-Path $testDir "test_kilnlink_inject_tc.c"),
    (Join-Path $srcDir "tasks\uart_owner_tx_policy.c"),
    (Join-Path $testDir "test_uart_owner_tx_policy.c"),
    (Join-Path $srcDir "tasks\clock_health.c"),
    (Join-Path $testDir "test_clock_health.c"),
    (Join-Path $testDir "test_safety_core_stack_budget.c"),
    (Join-Path $srcDir "clear_trip_diag_codec.c"),
    (Join-Path $testDir "test_clear_trip_diag_codec.c"),
    (Join-Path $srcDir "watchdog_overdue_diag_codec.c"),
    (Join-Path $testDir "test_watchdog_overdue_diag_codec.c"),
    (Join-Path $testDir "test_log_task_stack_budget.c"),
    (Join-Path $srcDir "max31856_tc_type_policy.c"),
    (Join-Path $testDir "test_max31856_tc_type_policy.c"),
    (Join-Path $srcDir "max31856_decode.c"),
    (Join-Path $testDir "test_max31856_decode.c"),
    (Join-Path $srcDir "max31856_tc_range_policy.c"),
    (Join-Path $testDir "test_max31856_tc_range_policy.c"),
    (Join-Path $srcDir "link_diag_flags.c"),
    (Join-Path $testDir "test_link_diag_flags.c"),
    (Join-Path $srcDir "current_presence_policy.c"),
    (Join-Path $testDir "test_current_presence_policy.c"),
    (Join-Path $srcDir "discrete_pin_policy.c"),
    (Join-Path $testDir "test_discrete_pin_policy.c")
)

$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /W4 /WX /EHsc /I `"$srcDir`" /I `"$bootDir`" /I `"$updateDir`" /I `"$commonIncDir`" /Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "build failed"
}

& $exe
exit $LASTEXITCODE
