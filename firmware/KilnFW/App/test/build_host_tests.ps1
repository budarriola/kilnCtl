# Builds and runs the host-side unit tests for pid.c / thermal_guard.c /
# heater_output.c / thermo_combine.c / ota_auth.c / ota_interlock.c /
# pid_autotune.c / profile_feasibility.c (+ the sim_plant.c closed-loop
# check) with MSVC, entirely off-target -- no ESP-IDF, no hardware.
# TODO.md 5A.1, 6A.8, 10.8.
#
# profile_feasibility.c is pure math but its headers are real firmware
# headers that name ESP-IDF types, so App/test/stubs/ supplies type-only
# stand-ins (esp_err.h, driver/spi_master.h, esp_spi_owner.h, freertos/*)
# reached via /I. Being on the include path only, they can never shadow a
# real header in an on-target build.
#
# Usage: powershell -File App\test\build_host_tests.ps1
# -OutDir lets a caller build into a private directory instead of the shared
# App/test/build. Two concurrent runs of this script otherwise compile into
# the same .obj paths and corrupt each other -- which matters when several
# agents are working the same checkout at once. Defaults to the old path, so
# every existing invocation is unchanged.
param([string]$OutDir = "")
$ErrorActionPreference = "Stop"

$vcvars = "C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools\VC\Auxiliary\Build\vcvarsall.bat"
if (-not (Test-Path $vcvars)) {
    throw "vcvarsall.bat not found at $vcvars -- update this path if MSVC Build Tools moved/were reinstalled."
}

$testDir = $PSScriptRoot
$driversDir = Join-Path $testDir "..\drivers"
$outDir = if ($OutDir) { $OutDir } else { Join-Path $testDir "build" }
New-Item -ItemType Directory -Force -Path $outDir | Out-Null
$exe = Join-Path $outDir "kilnctl_host_tests.exe"

$sources = @(
    (Join-Path $testDir "test_main.c"),
    (Join-Path $testDir "test_pid.c"),
    (Join-Path $testDir "test_thermal_guard.c"),
    (Join-Path $testDir "test_heater_output.c"),
    (Join-Path $testDir "test_closed_loop.c"),
    (Join-Path $testDir "test_pid_autotune.c"),
    (Join-Path $testDir "test_sim_kiln.c"),
    (Join-Path $testDir "test_ota_auth.c"),
    (Join-Path $testDir "test_ota_interlock.c"),
    (Join-Path $testDir "test_heat_interlock.c"),
    (Join-Path $testDir "test_thermo_combine.c"),
    (Join-Path $testDir "test_profile_feasibility.c"),
    (Join-Path $testDir "test_profile_plan_curve.c"),
    (Join-Path $testDir "test_wifi_prov.c"),
    (Join-Path $testDir "test_backup_import.c"),
    (Join-Path $testDir "test_ota_record.c"),
    (Join-Path $testDir "test_uart_log_bridge.c"),
    (Join-Path $testDir "test_safety_watchdog.c"),
    (Join-Path $testDir "test_safety_link.c"),
    (Join-Path $testDir "test_dashboard_safety_ready.c"),
    (Join-Path $testDir "test_readiness_commissioning.c"),
    (Join-Path $testDir "test_kiln_cfg_store.c"),
    (Join-Path $testDir "test_safety_cfg_store.c"),
    (Join-Path $testDir "test_rules_eval.c"),
    (Join-Path $testDir "test_boot_guard.c"),
    (Join-Path $testDir "test_boot_button.c"),
    (Join-Path $testDir "test_crash_report.c"),
    (Join-Path $testDir "test_watchdog_cfg.c"),
    (Join-Path $testDir "test_ui_page_home_graph.c"),
    (Join-Path $testDir "test_max31856_codec.c"),
    (Join-Path $testDir "test_owner_slot_pool.c"),
    (Join-Path $testDir "test_dram_margin.c"),
    (Join-Path $testDir "test_stack_margin.c"),
    (Join-Path $testDir "sim_plant.c"),
    (Join-Path $driversDir "pid.c"),
    (Join-Path $driversDir "thermal_guard.c"),
    (Join-Path $driversDir "heater_output.c"),
    (Join-Path $driversDir "pid_autotune.c"),
    (Join-Path $driversDir "ota_auth.c"),
    (Join-Path $driversDir "ota_interlock.c"),
    (Join-Path $driversDir "ota_record.c"),
    (Join-Path $driversDir "heat_interlock.c"),
    (Join-Path $driversDir "thermo_combine.c"),
    (Join-Path $driversDir "profile_feasibility.c"),
    (Join-Path $driversDir "rules_eval.c"),
    (Join-Path $driversDir "ui_page_home_graph.c"),
    (Join-Path $driversDir "max31856_codec.c"),
    (Join-Path $driversDir "owner_slot_pool.c")
)

$sourceArgs = ($sources | ForEach-Object { '"' + $_ + '"' }) -join " "
# Type-only stand-ins for the ESP-IDF headers the real firmware headers name,
# plus CommonFW's include root for kilnlink/kilnlink_version.h. Include-path
# only, so an on-target build is unaffected.
$stubDir = Join-Path $testDir "stubs"
$commonInc = Join-Path $testDir "..\..\..\CommonFW\include"
$cmd = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /I`"$stubDir`" /I`"$commonInc`" /Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

cmd.exe /c $cmd
if ($LASTEXITCODE -ne 0) {
    throw "build failed"
}

# Every executable RUNS, even after one of them fails.
#
# This script used to `exit $LASTEXITCODE` the moment an executable returned
# non-zero, so a single failing check in the first binary meant the other five
# never ran and their results were simply unknown -- reported as if the suite
# had been considered. Found 2026-08-27, when four stale wording assertions in
# the first executable were hiding whether zones_http, safety_cfg_http, the two
# prestart suites and rules_task passed at all. A test runner that stops at the
# first failure hides exactly the failures you most need to see together.
$script:failedExes = @()


& $exe
if ($LASTEXITCODE -ne 0) {
    $script:failedExes += $exe
}

# ---- test_zones_http.c: its own SEPARATE executable ------------------------
# See test_zones_http.c's header comment for why: it #includes zones_http.c
# directly (to reach parse_zone_fields(), a `static` function with no other
# seam), which DEFINES the real zones_config_get_*()/set_*() functions --
# and test_backup_import.c above already defines its OWN fake bodies for
# those same names to stub backup_import_apply()'s dependency on
# zones_http.h. Linking both into one executable would be a multiple-
# definition error, so this one is built and run as a second, independent
# binary instead of being added to $sources above.
$exe2 = Join-Path $outDir "kilnctl_host_tests_zones.exe"
$cmd2 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$outDir\\zones_`" /Fe:`"$exe2`" `"$(Join-Path $testDir 'test_zones_http.c')`""

cmd.exe /c $cmd2
if ($LASTEXITCODE -ne 0) {
    throw "zones_http build failed"
}

& $exe2
if ($LASTEXITCODE -ne 0) {
    $script:failedExes += $exe2
}

# ---- test_safety_cfg_http.c: its own THIRD, separate executable -----------
# Same reason as test_zones_http.c above: it #includes safety_cfg_http.c
# directly to reach its static parse_set_param_body()/build_commissioning_
# json()/apply_pairs() helpers, and defines its own fake bodies for
# safety_cfg_store_get_by_index() and friends -- the main executable already
# links the REAL ones via test_safety_cfg_store.c's #include of safety_cfg_
# store.c, so linking both into one binary would multiply-define every
# safety_cfg_store_* symbol.
$exe3 = Join-Path $outDir "kilnctl_host_tests_safety_cfg_http.exe"
$cmd3 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$outDir\\safety_cfg_http_`" /Fe:`"$exe3`" `"$(Join-Path $testDir 'test_safety_cfg_http.c')`""

cmd.exe /c $cmd3
if ($LASTEXITCODE -ne 0) {
    throw "safety_cfg_http build failed"
}

& $exe3
if ($LASTEXITCODE -ne 0) {
    $script:failedExes += $exe3
}

# ---- test_profile_executor_prestart.c: its own FOURTH, separate executable-
# Same reason as test_zones_http.c above: it #includes profile_executor.c
# directly (recovery mode's "called before profile_executor_start() has run"
# guard has no other seam to test through) and so defines its own fake
# bodies for zones_config_*()/profiles_http_get()/etc, which would
# multiply-define against other host tests' fakes of the same names if
# linked into the main executable. pid.c/thermal_guard.c/heater_output.c/
# thermo_combine.c are linked in for real (already host-tested elsewhere)
# rather than faked, since profile_executor.c's own logic depends on them
# compiling correctly even though these tests never reach past the guard.
$exe4 = Join-Path $outDir "kilnctl_host_tests_profile_executor.exe"
$peObjDir = Join-Path $outDir "pe"
New-Item -ItemType Directory -Force -Path $peObjDir | Out-Null
$cmd4 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$peObjDir\\`" /Fe:`"$exe4`" " +
        "`"$(Join-Path $testDir 'test_profile_executor_prestart.c')`" " +
        "`"$(Join-Path $driversDir 'pid.c')`" `"$(Join-Path $driversDir 'thermal_guard.c')`" " +
        "`"$(Join-Path $driversDir 'heater_output.c')`" `"$(Join-Path $driversDir 'thermo_combine.c')`""

cmd.exe /c $cmd4
if ($LASTEXITCODE -ne 0) {
    throw "profile_executor prestart build failed"
}

& $exe4
if ($LASTEXITCODE -ne 0) {
    $script:failedExes += $exe4
}

# ---- test_autotune_engine_prestart.c: its own FIFTH, separate executable --
# Same reasoning as test_profile_executor_prestart.c immediately above, for
# autotune_engine.c's identical pre-start guard (begin_run_locked()'s
# s_at.lock == NULL check). pid_autotune.c is linked in for real (already
# host-tested) rather than faked, same reasoning as profile_executor's pid.c.
$exe5 = Join-Path $outDir "kilnctl_host_tests_autotune_engine.exe"
$aeObjDir = Join-Path $outDir "ae"
New-Item -ItemType Directory -Force -Path $aeObjDir | Out-Null
$cmd5 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$aeObjDir\\`" /Fe:`"$exe5`" " +
        "`"$(Join-Path $testDir 'test_autotune_engine_prestart.c')`" " +
        "`"$(Join-Path $driversDir 'thermal_guard.c')`" `"$(Join-Path $driversDir 'heater_output.c')`" " +
        "`"$(Join-Path $driversDir 'thermo_combine.c')`" `"$(Join-Path $driversDir 'pid_autotune.c')`""

cmd.exe /c $cmd5
if ($LASTEXITCODE -ne 0) {
    throw "autotune_engine prestart build failed"
}

& $exe5
if ($LASTEXITCODE -ne 0) {
    $script:failedExes += $exe5
}

# ---- test_rules_task_prestart.c: its own SIXTH, separate executable -------
# rules_task.c turns out to need no pre-start guard at all -- it holds no
# FreeRTOS mutex, and rules_task_get_status() only ever copies the
# zero-initialized static status struct (see this file's header comment).
# This #includes rules_task.c directly to prove that claim against the real
# code rather than a hand-rolled copy, same convention as the two prestart
# executables above; own executable for the same fake-body collision reason.
$exe6 = Join-Path $outDir "kilnctl_host_tests_rules_task.exe"
$rtObjDir = Join-Path $outDir "rt"
New-Item -ItemType Directory -Force -Path $rtObjDir | Out-Null
$cmd6 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$rtObjDir\\`" /Fe:`"$exe6`" " +
        "`"$(Join-Path $testDir 'test_rules_task_prestart.c')`" " +
        "`"$(Join-Path $driversDir 'rules_eval.c')`" `"$(Join-Path $driversDir 'thermo_combine.c')`" " +
        "`"$(Join-Path $driversDir 'stack_margin.c')`""

cmd.exe /c $cmd6
if ($LASTEXITCODE -ne 0) {
    throw "rules_task prestart build failed"
}

& $exe6
if ($LASTEXITCODE -ne 0) { $script:failedExes += "kilnctl_host_tests_rules_task.exe" }

# ---- test_profiles_http.c: its own SEVENTH, separate executable -----------
# Same reason as test_zones_http.c above: it #includes profiles_http.c
# directly to reach decode_profile_blob()/nvs_load_all_from(), both `static`,
# with no other seam. It also defines its own multi-key NVS stub (a real
# profiles_http.c load touches a "prof_used" bitmap key plus up to 8 separate
# "profN" blob keys on one open handle, which stubs/nvs.h's shared
# single-blob-slot stub cannot model) -- see test_profiles_http.c's own
# header comment. Own executable so that stub, and this file's fake bodies
# for zones_http.h/profile_feasibility.h/profiles_builtin.h, never collide
# with any other test file's definitions of those same symbols.
$exe7 = Join-Path $outDir "kilnctl_host_tests_profiles_http.exe"
$cmd7 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$outDir\\profiles_`" /Fe:`"$exe7`" `"$(Join-Path $testDir 'test_profiles_http.c')`""

cmd.exe /c $cmd7
if ($LASTEXITCODE -ne 0) {
    throw "profiles_http build failed"
}

& $exe7
if ($LASTEXITCODE -ne 0) { $script:failedExes += $exe7 }

# ---- test_ota_http.c: its own EIGHTH, separate executable -----------------
# ota_http.c/factory_reset.c shipped their security fixes (empty-AP-password
# refusal, per-context HMAC/lockout separation, auth-before-interlock on
# POST /api/factory_reset) with no host-test coverage at all -- neither file
# could compile on the host tree until this pass added the ESP-IDF/PSA-Crypto
# stub headers below. Same reason as test_zones_http.c/test_profiles_http.c
# above: this file #includes BOTH ota_http.c and factory_reset.c directly (to
# reach ota_http.c's file-scope nonce state and factory_reset.c's static
# reset_post_handler(), neither of which has any other seam), so it must be
# its own executable -- other host tests already define their OWN fakes for
# several of the same ESP-IDF/driver symbols this file needs, and both
# `static const char *TAG` file-scope statics would collide if this were
# merged into any of them. ota_auth.c/ota_interlock.c/ota_record.c are linked
# in for REAL (already host-tested elsewhere) rather than faked, since
# decision 2 (per-context lockout separation) is genuinely exercised through
# ota_auth.c's real nonce/lockout state machine, not a stand-in for it.
$exe8 = Join-Path $outDir "kilnctl_host_tests_ota_http.exe"
$otaObjDir = Join-Path $outDir "ota"
New-Item -ItemType Directory -Force -Path $otaObjDir | Out-Null
$cmd8 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$otaObjDir\\`" /Fe:`"$exe8`" " +
        "`"$(Join-Path $testDir 'test_ota_http.c')`" " +
        "`"$(Join-Path $driversDir 'ota_auth.c')`" `"$(Join-Path $driversDir 'ota_interlock.c')`" " +
        "`"$(Join-Path $driversDir 'ota_record.c')`""

cmd.exe /c $cmd8
if ($LASTEXITCODE -ne 0) {
    throw "ota_http build failed"
}

& $exe8
if ($LASTEXITCODE -ne 0) { $script:failedExes += $exe8 }

# ---- test_uart_protocol_link_delegate.c: its own NINTH, separate executable
# SaftyFW/TODO.md Phase 1's "KilnFW's uart_protocol.c delegating framing/CRC,
# proven byte-identical" item -- see that file's own header comment. Needs
# CommonFW's kilnlink_frame.c/kilnlink_crc.c linked in directly (the only host
# test that does), which no other executable here needs, so it gets its own
# build command rather than joining $sources above.
$exe9 = Join-Path $outDir "kilnctl_host_tests_uart_protocol_link_delegate.exe"
$commonSrc = Join-Path $testDir "..\..\..\CommonFW\src"
$linkObjDir = Join-Path $outDir "link"
New-Item -ItemType Directory -Force -Path $linkObjDir | Out-Null
$cmd9 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$linkObjDir\\`" /Fe:`"$exe9`" " +
        "`"$(Join-Path $testDir 'test_uart_protocol_link_delegate.c')`" " +
        "`"$(Join-Path $commonSrc 'kilnlink_frame.c')`" `"$(Join-Path $commonSrc 'kilnlink_crc.c')`""

cmd.exe /c $cmd9
if ($LASTEXITCODE -ne 0) {
    throw "uart_protocol_link_delegate build failed"
}

& $exe9
if ($LASTEXITCODE -ne 0) { $script:failedExes += $exe9 }

# ---- test_board_temps.c: its own TENTH, separate executable --------------
# Same reason as test_zones_http.c above: it #includes board_temps.c
# directly to reach board_temps_get() (the pure half of that file, no other
# seam), which needs driver/temperature_sensor.h's stub -- own executable so
# that stub's temperature_sensor_*() bodies, defined in test_board_temps.c
# itself, never collide with any other test file's definitions of those
# same symbols.
$exe10 = Join-Path $outDir "kilnctl_host_tests_board_temps.exe"
$btObjDir = Join-Path $outDir "bt"
New-Item -ItemType Directory -Force -Path $btObjDir | Out-Null
$cmd10 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$btObjDir\\`" /Fe:`"$exe10`" `"$(Join-Path $testDir 'test_board_temps.c')`""

cmd.exe /c $cmd10
if ($LASTEXITCODE -ne 0) {
    throw "board_temps build failed"
}

& $exe10
if ($LASTEXITCODE -ne 0) { $script:failedExes += $exe10 }

# ---- test_kiln_io_owner.c: its own ELEVENTH, separate executable ----------
# Audit item, TODO.md "Audit 2026-08-27 -- open items" (IO_CMD_SX_LED_DRIVER's
# missing relay-pin gate). Same reason as test_board_temps.c above: it
# #includes kiln_io_owner.c directly to reach its `static`
# sx_mask_touches_relay()/sx_led_driver_touches_relay() predicates, the only
# seam onto the actual decision the fix hinges on. Links the real kiln_io.c
# (for the genuine kiln_io_relay_pin_mask() these predicates call) and
# provides its own SX1509/danger_mode/ota_http/relay_authority stub bodies,
# so it must be its own executable to avoid colliding with any other test
# file's fakes of those same symbols.
$exe11 = Join-Path $outDir "kilnctl_host_tests_kiln_io_owner.exe"
$kioObjDir = Join-Path $outDir "kio"
New-Item -ItemType Directory -Force -Path $kioObjDir | Out-Null
$cmd11 = "call `"$vcvars`" x64 >nul && cl /nologo /W3 /EHsc /std:c11 /I`"$stubDir`" /I`"$commonInc`" " +
        "/Fo:`"$kioObjDir\\`" /Fe:`"$exe11`" " +
        "`"$(Join-Path $testDir 'test_kiln_io_owner.c')`" `"$(Join-Path $driversDir 'kiln_io.c')`" " +
        "`"$(Join-Path $driversDir 'owner_slot_pool.c')`""

cmd.exe /c $cmd11
if ($LASTEXITCODE -ne 0) {
    throw "kiln_io_owner build failed"
}

& $exe11
if ($LASTEXITCODE -ne 0) { $script:failedExes += $exe11 }

if ($script:failedExes.Count -gt 0) {
    Write-Host ""
    Write-Host "FAILED executables ($($script:failedExes.Count)):"
    foreach ($f in $script:failedExes) { Write-Host "  $f" }
    exit 1
}
Write-Host ""
Write-Host "all host test executables passed"
exit 0
