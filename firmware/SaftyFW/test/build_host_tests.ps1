# Builds and runs the host-side unit tests for safety_guards.c with MSVC,
# entirely off-target -- no pico-sdk, no FreeRTOS, no hardware. Mirrors
# firmware/KilnFW/App/test/build_host_tests.ps1's pattern. TODO.md Phase 4.
#
# Usage: powershell -File test\build_host_tests.ps1 [-OutDir <path>]
#
# -OutDir exists because concurrent runs of this script share one build
# directory: the .obj files carry fixed names, so two agents compiling at once
# corrupt each other's objects and produce a link error that looks like a code
# defect. Give each concurrent run its own directory. Mirrors the same
# parameter on KilnFW/App/test/build_host_tests.ps1.
param([string]$OutDir = "")
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
# HAL Phase 1a (docs/HW_ABSTRACTION.md) moved uart_owner_tx_policy.c/h
# out of src/tasks/ into firmware/hwAbstraction/pico/uart/ (colocated with
# the rest of the pico UART owner); it is still host-testable on its own
# (no pico-sdk/FreeRTOS dependency), so this script now reaches it there.
$hwAbstractionPicoUartDir = Join-Path $testDir "..\..\hwAbstraction\pico\uart"
# HAL Phase 1b (docs/HW_ABSTRACTION.md "hal_gpio" section): relay_owner.c
# is now a hal_gpio client. hal_gpio.h/hal_status.h live in interface/; the
# host backend under test is fake_gpio.c (host/); relay_owner.c's own host
# build additionally needs a minimal FreeRTOS stub (stubs\freertos_min\) --
# see that directory's FreeRTOS.h for why it exists and its narrow scope.
$hwAbstractionInterfaceDir = Join-Path $testDir "..\..\hwAbstraction\interface"
$hwAbstractionHostDir = Join-Path $testDir "..\..\hwAbstraction\host"
$hwAbstractionCommonDir = Join-Path $testDir "..\..\hwAbstraction\common"
$freertosMinStubDir = Join-Path $testDir "stubs\freertos_min"
# HAL Phase 1b -- max31856.c host build, now a hal_spi client
# (docs/HW_ABSTRACTION.md pico spi_owner adapter). Its only pico-sdk
# dependency left is the raw hardware/gpio.h CS/~FAULT bring-up in
# max31856_init() (outside this migration's scope), stubbed to inert no-ops
# by this dir's hardware/gpio.h -- see that file's own comment.
$hardwareGpioMinStubDir = Join-Path $testDir "stubs\hardware_gpio_min"
# hal_spi_pico.c's OWN adapter-logic host test (test_hal_spi_pico.c) links
# the real hal_spi_pico.c against a small spi_owner stub (spi_owner_stub.c),
# NOT fake_spi.c -- see stubs/spi_owner_stub/spi_owner_stub.h's own comment
# for why this is a different layer than max31856.c's fake_spi-backed test.
$hwAbstractionPicoSpiDir = Join-Path $testDir "..\..\hwAbstraction\pico\spi"
$spiOwnerStubDir = Join-Path $testDir "stubs\spi_owner_stub"

$outDir = if ($OutDir) { $OutDir } else { Join-Path $testDir "build" }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null

. (Join-Path $PSScriptRoot "../../../tools/build_lock.ps1")
$buildLockName = "saftyfw_host_tests_" + ([System.Text.RegularExpressions.Regex]::Replace($outDir, '[^A-Za-z0-9]+', '_'))
$buildLock = Enter-BuildLock -Name $buildLockName
try {
    $exe = Join-Path $outDir "saftyfw_host_tests.exe"

    $sources = @(
        (Join-Path $testDir "test_main.c"),
        (Join-Path $testDir "test_safety_guards.c"),
        (Join-Path $testDir "test_guard_nuisance.c"),
        (Join-Path $testDir "test_debounce_policy.c"),
        (Join-Path $testDir "test_debounce_nuisance.c"),
        (Join-Path $srcDir "debounce_policy.c"),
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
        (Join-Path $hwAbstractionPicoUartDir "uart_owner_tx_policy.c"),
        (Join-Path $testDir "test_uart_owner_tx_policy.c"),
        (Join-Path $srcDir "tasks\clock_health.c"),
        (Join-Path $testDir "test_clock_health.c"),
        (Join-Path $srcDir "tasks\tick_timing.c"),
        (Join-Path $testDir "test_tick_timing.c"),
        (Join-Path $testDir "test_safety_core_stack_budget.c"),
        (Join-Path $testDir "test_safety_core_s8_wiring.c"),
        (Join-Path $testDir "test_safety_core_polarity_wiring.c"),
        (Join-Path $testDir "test_update_task_relay_wiring.c"),
        (Join-Path $srcDir "clear_trip_diag_codec.c"),
        (Join-Path $testDir "test_clear_trip_diag_codec.c"),
        (Join-Path $srcDir "watchdog_overdue_diag_codec.c"),
        (Join-Path $testDir "test_watchdog_overdue_diag_codec.c"),
        # HAL Phase 3 item 1 (docs/HW_ABSTRACTION.md "hal_scratch -- pico
        # watchdog-scratch registry"): boot_reason.c/clear_trip_diag.c/
        # watchdog_overdue_diag.c are now hal_scratch.h clients instead of
        # poking watchdog_hw->scratch[] directly, so all three are host-
        # compilable for the first time -- backed here by fake_scratch.c
        # (hwAbstraction/host), the same host hal_scratch.h backend
        # firmware/hwAbstraction/test/test_fake_scratch.c exercises directly.
        # hal_status.c is already linked in below (relay_owner.c's block).
        (Join-Path $srcDir "boot_reason.c"),
        (Join-Path $srcDir "clear_trip_diag.c"),
        (Join-Path $srcDir "watchdog_overdue_diag.c"),
        (Join-Path $hwAbstractionHostDir "fake_scratch.c"),
        (Join-Path $testDir "test_scratch_migration.c"),
        (Join-Path $testDir "test_log_task_stack_budget.c"),
        (Join-Path $srcDir "max31856_tc_type_policy.c"),
        (Join-Path $testDir "test_max31856_tc_type_policy.c"),
        (Join-Path $srcDir "max31856_decode.c"),
        (Join-Path $testDir "test_max31856_decode.c"),
        (Join-Path $srcDir "max31856_tc_range_policy.c"),
        (Join-Path $testDir "test_max31856_tc_range_policy.c"),
        (Join-Path $srcDir "max31856_fault_pin_policy.c"),
        (Join-Path $testDir "test_max31856_fault_pin_policy.c"),
        (Join-Path $srcDir "max31856_reconfig_retry.c"),
        (Join-Path $testDir "test_max31856_reconfig_retry.c"),
        (Join-Path $srcDir "tasks\thermo_task_drdy_recovery.c"),
        (Join-Path $testDir "test_thermo_task_drdy_recovery.c"),
        (Join-Path $srcDir "link_diag_flags.c"),
        (Join-Path $testDir "test_link_diag_flags.c"),
        (Join-Path $srcDir "current_presence_policy.c"),
        (Join-Path $testDir "test_current_presence_policy.c"),
        (Join-Path $srcDir "discrete_pin_policy.c"),
        (Join-Path $testDir "test_discrete_pin_policy.c"),
        (Join-Path $srcDir "commissioning_gate.c"),
        (Join-Path $testDir "test_commissioning_gate.c"),
        # HAL Phase 1b -- relay_owner.c host build, see the hwAbstraction* dir
        # variables above for the extra /I paths this pulls in.
        (Join-Path $srcDir "tasks\relay_owner.c"),
        (Join-Path $hwAbstractionHostDir "fake_gpio.c"),
        (Join-Path $hwAbstractionCommonDir "hal_status.c"),
        (Join-Path $testDir "test_relay_owner_gpio_init_stubs.c"),
        (Join-Path $testDir "test_relay_owner_gpio_init.c"),
        # HAL Phase 1b -- current_sense.c host build, now a hal_adc client
        # (docs/HW_ABSTRACTION.md "hal_adc -- pico-only"). Only needs the
        # freertos_min stub's xTaskGetTickCount()/portTICK_PERIOD_MS (already
        # pulled in above for relay_owner.c) plus fake_adc.c as its backend --
        # no new stub file, unlike relay_owner.c's task-creation surface.
        (Join-Path $srcDir "current_sense.c"),
        (Join-Path $hwAbstractionHostDir "fake_adc.c"),
        # current_task.c's current_task_start() is now exercised too (hal_adc_
        # init()/hal_adc_gpio_enable() x3, then xTaskCreate) -- reuses the same
        # xTaskCreate/vTaskCoreAffinitySet/watchdog_task_checkin stubs already
        # defined once in test_relay_owner_gpio_init_stubs.c for relay_owner.c
        # (all TUs link into one executable), so no new stub file is needed.
        # current_task_fn()'s for(;;) loop is never invoked on host, same as
        # relay_owner_task()'s.
        (Join-Path $srcDir "tasks\current_task.c"),
        (Join-Path $testDir "test_current_sense_hal_adc.c"),
        # HAL Phase 1b -- max31856.c host build, now a hal_spi client
        # (interface/hal_spi.h, pico backend hal_spi_pico.c). fake_spi.c is the
        # host backend under test; hal_status.c is already linked in above
        # (relay_owner.c's block). max31856_decode.c/max31856_tc_type_policy.c/
        # max31856_tc_range_policy.c/max31856_fault_pin_policy.c are already
        # linked in above for their own standalone host tests.
        (Join-Path $srcDir "max31856.c"),
        (Join-Path $hwAbstractionHostDir "fake_spi.c"),
        (Join-Path $testDir "test_max31856_hal_spi.c")
        # hal_spi_pico.c's own adapter-logic test (test_hal_spi_pico.c) is built
        # as its own SEPARATE executable below, not added here -- it implements
        # the exact same hal_spi_* symbol names fake_spi.c does (both are
        # hal_spi.h backends), so linking both into one exe is an LNK2005
        # multiply-defined-symbol error, not a valid combination.
    )

    # A response file for the cl invocation itself (not just $sources) -- this
    # grew past cmd.exe's ~8191-char command-line limit once the HAL Phase 1b
    # relay_owner.c sources/includes were added ("The command line is too
    # long."), so the compiler args and source list are written to a file and
    # passed as @rsp instead of inline on the cmd.exe command line.
    #
    # /std:c17 -- added for max31856.c's HAL Phase 1b hal_spi.h migration:
    # interface/hal_spi.h is the first header this host build pulls in that uses
    # HAL_ALIGNAS8 (hal_status.h: `_Alignas(8)`, C11), and cl's default C dialect
    # here is pre-C11, so `_Alignas` was an unrecognized identifier -- a real gap
    # in this build's dialect, not something the new source files could work
    # around. hal_flash.h/hal_i2c.h/hal_kv.h/hal_uart.h use the same macro but
    # had no host-test consumer in this build until now, so this was latent
    # rather than already fixed elsewhere. Matches the fuzz build's own
    # /std:c17 below.
    $rspContent = "/nologo /W4 /WX /EHsc /std:c17 /I `"$srcDir`" /I `"$srcDir\board`" /I `"$bootDir`" /I `"$updateDir`" /I `"$commonIncDir`" " +
        "/I `"$hwAbstractionInterfaceDir`" /I `"$hwAbstractionHostDir`" /I `"$freertosMinStubDir`" /I `"$hwAbstractionPicoSpiDir`" " +
        "/I `"$hardwareGpioMinStubDir`" " +
        "/I `"$spiOwnerStubDir`" " +
        "/Fo:`"$outDir\\`" /Fe:`"$exe`" " +
        (($sources | ForEach-Object { '"' + $_ + '"' }) -join " ")
    $rspPath = Join-Path $outDir "saftyfw_host_tests_cl.rsp"
    Set-Content -Path $rspPath -Value $rspContent -Encoding ascii -NoNewline

    $cmd = "call `"$vcvars`" x64 >nul && cl @`"$rspPath`""

    cmd.exe /c $cmd
    if ($LASTEXITCODE -ne 0) {
        throw "build failed"
    }

    & $exe
    $mainExit = $LASTEXITCODE

    # GUARD_TEST_MATRIX.md "Fuzz over every decoder": firmware/CommonFW/test/
    # test_fuzz_payloads.c drives every kilnlink payload decoder (context/
    # status/power/announce/diag/trip/ceiling/clear_trip/set_config/rollback/
    # get_fw_version/set_clock/announce_reboot/set_ct_cal/get_ct_cal/ct_cal/
    # set_log_level/set_param/commit_config/commit_config_rejected/inject_tc/
    # get_param/param/get_config_page/config_page/rollback_result/fw_version) --
    # a separate standalone binary (own `main()`, own deterministic seed/exit
    # code) rather than folded into $sources above, because that file follows
    # CommonFW/test's convention (own executable per ctest target, see
    # firmware/CommonFW/CMakeLists.txt) rather than test_main.c's TEST_CHECK/
    # g_test_count convention, and it needs to link every kilnlink_*.c file --
    # several of which ($sources above only wires the subset the commissioning
    # family actually calls) have no other consumer in this build yet. Built
    # and run as its own step so a fuzz failure is a distinct, clearly-labeled
    # CI failure rather than silently folded into (or invisibly absent from)
    # the $exe check count above.
    # Fail-closed decoder-registration check. test_fuzz_payloads.c's k_cases[]
    # is a hand-maintained list: a payload decoder added to CommonFW and not
    # registered there would silently drop out of fuzz coverage with nothing
    # failing -- "27 decoders fuzzed" would stay printed and stay true, just no
    # longer complete. Enumerate every *_decode symbol declared in the kilnlink
    # headers, subtract the two that are deliberately not in k_cases[], and
    # require the rest to be registered by name.
    #
    #   kilnlink_frame_decode      -- framing layer, fuzzed by CommonFW/test/test_fuzz.c
    #   kilnlink_param_value_decode-- not a payload decoder: (type, in, off, value)
    #                                 with the caller owning the bounds check, so it
    #                                 has no (payload, len, out) contract to fuzz.
    $fuzzSrc = Join-Path $commonSrcDir "..\test\test_fuzz_payloads.c"
    $declared = Get-ChildItem -Path (Join-Path $commonIncDir "kilnlink") -Filter "*.h" |
        Select-String -Pattern "kilnlink_[a-z0-9_]*_decode" -AllMatches |
        ForEach-Object { $_.Matches } | ForEach-Object { $_.Value } | Sort-Object -Unique
    $registered = Select-String -Path $fuzzSrc -Pattern '"(kilnlink_[a-z0-9_]*_decode)"' -AllMatches |
        ForEach-Object { $_.Matches } | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
    $fuzzExempt = @("kilnlink_frame_decode", "kilnlink_param_value_decode")
    $unregistered = $declared | Where-Object { ($fuzzExempt -notcontains $_) -and ($registered -notcontains $_) }
    if ($unregistered) {
        throw ("kilnlink payload decoder(s) declared but NOT registered in test_fuzz_payloads.c's k_cases[]: " +
            ($unregistered -join ", ") +
            " -- add an adapter + k_cases[] entry (or, if it is genuinely not a (payload,len,out) decoder, " +
            "add it to `$fuzzExempt in this script with the reason).")
    }

    $fuzzExe = Join-Path $outDir "kilnlink_fuzz_payloads.exe"
    $fuzzObjDir = Join-Path $outDir "fuzz_obj"
    New-Item -ItemType Directory -Force -Path $fuzzObjDir | Out-Null
    $fuzzSources = @((Join-Path $commonSrcDir "..\test\test_fuzz_payloads.c")) +
        (Get-ChildItem -Path $commonSrcDir -Filter "kilnlink_*.c" | ForEach-Object { $_.FullName })
    $fuzzSourceArgs = ($fuzzSources | ForEach-Object { '"' + $_ + '"' }) -join " "
    $fuzzCmd = "call `"$vcvars`" x64 >nul && cl /nologo /W4 /WX /std:c17 /I `"$commonIncDir`" " +
        "/Fo:`"$fuzzObjDir\\`" /Fe:`"$fuzzExe`" $fuzzSourceArgs"
    cmd.exe /c $fuzzCmd
    if ($LASTEXITCODE -ne 0) {
        throw "kilnlink payload fuzz build failed"
    }
    & $fuzzExe
    $fuzzExit = $LASTEXITCODE

    if ($fuzzExit -ne 0) {
        throw "kilnlink payload fuzz FAILED (exit $fuzzExit) -- see output above for which decoder and case; rerun with KILNLINK_FUZZ_SEED set to the seed printed above to reproduce"
    }

    # hal_spi_pico.c's own adapter-logic test -- a SEPARATE executable (see
    # this script's earlier comment on why it cannot share test_main.c's exe:
    # it links the real hal_spi_pico.c against spi_owner_stub.c, which defines
    # the same hal_spi_* symbol names fake_spi.c already provides in the main
    # exe above). Own main() (test_hal_spi_pico_main.c), same TEST_CHECK/
    # g_test_count convention as test_main.c, own exit code folded in below.
    $halSpiPicoExe = Join-Path $outDir "hal_spi_pico_tests.exe"
    $halSpiPicoObjDir = Join-Path $outDir "hal_spi_pico_obj"
    New-Item -ItemType Directory -Force -Path $halSpiPicoObjDir | Out-Null
    $halSpiPicoSources = @(
        (Join-Path $testDir "test_hal_spi_pico_main.c"),
        (Join-Path $testDir "test_hal_spi_pico.c"),
        (Join-Path $hwAbstractionPicoSpiDir "hal_spi_pico.c"),
        (Join-Path $spiOwnerStubDir "spi_owner_stub.c")
    )
    $halSpiPicoSourceArgs = ($halSpiPicoSources | ForEach-Object { '"' + $_ + '"' }) -join " "
    $halSpiPicoCmd = "call `"$vcvars`" x64 >nul && cl /nologo /W4 /WX /std:c17 " +
        "/I `"$hwAbstractionInterfaceDir`" /I `"$hwAbstractionPicoSpiDir`" /I `"$spiOwnerStubDir`" " +
        "/I `"$srcDir/board`" /I `"$testDir`" " +
        "/Fo:`"$halSpiPicoObjDir\\`" /Fe:`"$halSpiPicoExe`" $halSpiPicoSourceArgs"
    cmd.exe /c $halSpiPicoCmd
    if ($LASTEXITCODE -ne 0) {
        throw "hal_spi_pico adapter test build failed"
    }
    & $halSpiPicoExe
    $halSpiPicoExit = $LASTEXITCODE

    # config_store_flash.c's own host tests (Phase 3 item 2 hal_flash rebase) --
    # a SEPARATE executable for the same reason hal_spi_pico_tests.exe is above:
    # config_store_flash_host_stubs.c defines relay_owner_get_state()/
    # console_uart_puts()/console_uart_write() as bare test doubles, which
    # collide (LNK2005) with the REAL relay_owner.c/console_uart-shaped symbols
    # the main exe links for its own tests. Links the real config_store.c/
    # config_store_flash.c/config_params.c against fake_flash.c (host hal_flash
    # backend) instead of a real RP2040.
    $configStoreFlashHostStubsDir = Join-Path $testDir "stubs\config_store_flash_host_stubs"
    $configStoreFlashExe = Join-Path $outDir "config_store_flash_tests.exe"
    $configStoreFlashObjDir = Join-Path $outDir "config_store_flash_obj"
    New-Item -ItemType Directory -Force -Path $configStoreFlashObjDir | Out-Null
    $configStoreFlashSources = @(
        (Join-Path $testDir "test_config_store_flash.c"),
        (Join-Path $configStoreFlashHostStubsDir "config_store_flash_host_stubs.c"),
        (Join-Path $srcDir "config_store.c"),
        (Join-Path $srcDir "config_store_flash.c"),
        (Join-Path $srcDir "config_params.c"),
        (Join-Path $bootDir "crc32.c"),
        (Join-Path $commonSrcDir "kilnlink_commit_config_rejected.c"),
        (Join-Path $hwAbstractionHostDir "fake_flash.c"),
        (Join-Path $hwAbstractionCommonDir "hal_status.c")
    )
    $configStoreFlashSourceArgs = ($configStoreFlashSources | ForEach-Object { '"' + $_ + '"' }) -join " "
    $configStoreFlashCmd = "call `"$vcvars`" x64 >nul && cl /nologo /W4 /WX /std:c17 " +
        "/I `"$srcDir`" /I `"$srcDir\board`" /I `"$bootDir`" /I `"$commonIncDir`" " +
        "/I `"$hwAbstractionInterfaceDir`" /I `"$hwAbstractionHostDir`" " +
        "/I `"$configStoreFlashHostStubsDir`" /I `"$testDir`" " +
        "/Fo:`"$configStoreFlashObjDir\\`" /Fe:`"$configStoreFlashExe`" $configStoreFlashSourceArgs"
    cmd.exe /c $configStoreFlashCmd
    if ($LASTEXITCODE -ne 0) {
        throw "config_store_flash host test build failed"
    }
    & $configStoreFlashExe
    $configStoreFlashExit = $LASTEXITCODE

    if ($mainExit -ne 0) {
        exit $mainExit
    }
    if ($fuzzExit -ne 0) {
        exit $fuzzExit
    }
    if ($halSpiPicoExit -ne 0) {
        exit $halSpiPicoExit
    }
    exit $configStoreFlashExit
} finally {
    Exit-BuildLock -Lock $buildLock
}
