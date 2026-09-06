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
$hwAbsDir = Join-Path $testDir "..\..\..\hwAbstraction"
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
    (Join-Path $testDir "test_pid_fuzzy.c"),
    (Join-Path $testDir "test_sim_kiln.c"),
    (Join-Path $testDir "test_ota_auth.c"),
    (Join-Path $testDir "test_ota_interlock.c"),
    (Join-Path $testDir "test_heat_interlock.c"),
    (Join-Path $testDir "test_heat_enable.c"),
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
    (Join-Path $testDir "test_boot_guard.c"),
    (Join-Path $testDir "test_boot_button.c"),
    (Join-Path $testDir "test_backlight_pwm.c"),
    (Join-Path $testDir "test_display_power_policy.c"),
    (Join-Path $testDir "test_display_power_cfg.c"),
    (Join-Path $testDir "test_display_power_wiring.c"),
    (Join-Path $testDir "test_crash_report.c"),
    (Join-Path $testDir "test_watchdog_cfg.c"),
    (Join-Path $testDir "test_ramp_assist_cfg.c"),
    (Join-Path $testDir "test_ui_page_home_graph.c"),
    (Join-Path $testDir "test_max31856_codec.c"),
    (Join-Path $testDir "test_panel_codec.c"),
    (Join-Path $testDir "test_st7796_panel.c"),
    (Join-Path $testDir "test_panel_detect.c"),
    (Join-Path $testDir "test_owner_slot_pool.c"),
    (Join-Path $testDir "test_dram_margin.c"),
    (Join-Path $testDir "test_httpd_socket_budget.c"),
    (Join-Path $testDir "test_stack_margin.c"),
    (Join-Path $testDir "test_time_sync.c"),
    (Join-Path $testDir "test_log_store.c"),
    (Join-Path $testDir "test_esp_spi_owner.c"),
    (Join-Path $testDir "test_touch_dev.c"),
    (Join-Path $testDir "test_ramp_ident.c"),
    (Join-Path $testDir "test_bx_worker_reentrancy.c"),
    (Join-Path $testDir "test_gpio_probe.c"),
    (Join-Path $testDir "test_iter_tune.c"),
    (Join-Path $testDir "test_cone_table.c"),
    (Join-Path $testDir "test_ramp_lock_onesided.c"),
    (Join-Path $testDir "test_zone_sweep_relay_off_wiring.c"),
    (Join-Path $testDir "test_approach_rate_cap.c"),
    (Join-Path $testDir "sim_plant.c"),
    (Join-Path $driversDir "control/pid.c"),
    (Join-Path $driversDir "control/cone_table.c"),
    (Join-Path $driversDir "control/thermal_guard.c"),
    (Join-Path $driversDir "control/heater_output.c"),
    (Join-Path $driversDir "control/pid_autotune.c"),
    (Join-Path $driversDir "control/pid_fuzzy.c"),
    (Join-Path $driversDir "net/ota_auth.c"),
    (Join-Path $driversDir "net/ota_interlock.c"),
    (Join-Path $driversDir "persist/ota_record.c"),
    (Join-Path $driversDir "control/heat_interlock.c"),
    (Join-Path $driversDir "control/heat_enable.c"),
    (Join-Path $driversDir "control/thermo_combine.c"),
    (Join-Path $driversDir "control/profile_feasibility.c"),
    (Join-Path $driversDir "ui/ui_page_home_graph.c"),
    (Join-Path $driversDir "hw/max31856_codec.c"),
    (Join-Path $driversDir "hw/panel_codec.c"),
    (Join-Path $driversDir "hw/st7796_panel.c"),
    (Join-Path $driversDir "hw/panel_detect.c"),
    (Join-Path $hwAbsDir "esp/spi/owner_slot_pool.c"),
    (Join-Path $hwAbsDir "host/fake_gpio.c"),
    (Join-Path $hwAbsDir "host/fake_sysinfo.c"),
    (Join-Path $hwAbsDir "host/fake_time.c"),
    (Join-Path $hwAbsDir "host/fake_wdt.c"),
    # HW_ABSTRACTION.md Phase 3 item 3 (nvs.h -> hal_kv.h migration):
    # boot_guard.c/kiln_cfg_store.c/watchdog_cfg.c/ramp_assist_cfg.c (each
    # #included directly by its own test_*.c above) and ota_record.c (linked
    # as a real object further down this list) now call hal_kv_*() instead
    # of nvs_*() directly, so this one shared executable needs the host
    # hal_kv backend linked in once for all of them.
    (Join-Path $hwAbsDir "host/fake_kv.c"),
    (Join-Path $hwAbsDir "common/hal_status.c"),
    (Join-Path $hwAbsDir "esp/common/hal_esp_common.c"),
    (Join-Path $driversDir "common/stack_margin.c"),
    (Join-Path $driversDir "net/time_sync_tz.c"),
    (Join-Path $driversDir "persist/log_store.c"),
    (Join-Path $driversDir "hw/touch_dev.c"),
    (Join-Path $driversDir "control/ramp_ident.c"),
    (Join-Path $driversDir "control/iter_tune.c")
)

# hal_time migration (HW_ABSTRACTION.md item 5) pushed the "main"
# executable's inline source-file list back over cmd.exe's ~8191-char
# command-line limit (same failure mode this file's own flags-.rsp comment
# below already describes for the /I flags, just on the $sources side this
# time) -- "The command line is too long." with no other diagnostic. Fixed
# the same way: the source list goes through a response file too, so the
# actual `cl` invocation only ever sees `cl @flags.rsp @sources.rsp ...`.
$sourcesRsp = Join-Path $outDir "host_tests_main_sources.rsp"
[System.IO.File]::WriteAllText($sourcesRsp, (($sources | ForEach-Object { '"' + $_ + '"' }) -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))
$sourceArgs = "@`"$sourcesRsp`""
# Type-only stand-ins for the ESP-IDF headers the real firmware headers name,
# plus CommonFW's include root for kilnlink/kilnlink_version.h. Include-path
# only, so an on-target build is unaffected.
$stubDir = Join-Path $testDir "stubs"
$commonInc = Join-Path $testDir "..\..\..\CommonFW\include"

# Response file for the common /nologo /W3 /EHsc + /I include flags shared by
# every `cl` invocation below (some also add /std:c11 on the command line
# itself, since that flag is NOT common to every executable -- e.g. exe5/
# exe20 build without it). Previously these flags were inlined into all 22
# `cl` command-line strings; once the drivers/ layering reorg
# (tools/drivers_reorg/) lands, each moved file's new layer subdir needs its
# own /I entry, and adding 11 more `/I` flags to 22 already-long inline
# command lines pushed the "main" executable's line from 7745 to 8924 chars
# -- past cmd.exe's ~8191-char command-line limit, so the build failed with
# no useful error. Routing the include flags through @file instead means
# cmd.exe only ever sees the short `cl @"$hostTestsRsp" ...` form, and the
# reorg's rewrite only has to append lines to this ONE file, not touch any of
# the 22 `cl` invocations. /I`"$driversDir`" is included here even though a
# few invocations (e.g. the main one) never referenced it directly before --
# harmless: an extra include directory that resolves nothing today, and it is
# exactly the directory tools/drivers_reorg/plan_moves.ps1 splits into layer
# subdirs, whose /I entries land in this same file.
$hostTestsRsp = Join-Path $outDir "host_tests_common_flags.rsp"
$hostTestsRspLines = @(
    "/nologo"
    "/W3"
    "/EHsc"
    "/I`"$stubDir`""
    "/I`"$commonInc`""
    "/I`"$driversDir`""
    "/I`"$driversDir\bridge`""
    "/I`"$driversDir\common`""
    "/I`"$driversDir\control`""
    "/I`"$driversDir\http`""
    "/I`"$driversDir\hw`""
    "/I`"$driversDir\net`""
    "/I`"$driversDir\owners`""
    "/I`"$driversDir\persist`""
    "/I`"$driversDir\safety`""
    "/I`"$driversDir\sim`""
    "/I`"$driversDir\ui`""
    "/I`"$hwAbsDir\esp\spi`""
    "/I`"$hwAbsDir\esp\i2c`""
    "/I`"$hwAbsDir\esp\uart`""
    "/I`"$hwAbsDir\interface`""
    "/I`"$hwAbsDir\host`""
    "/I`"$hwAbsDir\esp\common`""
)
[System.IO.File]::WriteAllText($hostTestsRsp, ($hostTestsRspLines -join "`r`n"), (New-Object System.Text.UTF8Encoding($false)))

$cmd = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 /Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

# ---- build/run bookkeeping -------------------------------------------------
#
# Every executable is BUILT, even after an earlier one fails to build or run,
# and a build that fails can never be mistaken for a stale pass:
#
#   1. Each executable's binary is deleted (if present) immediately before
#      its own build command runs. If the build then fails, there is no
#      leftover .exe on disk for anything downstream to accidentally pick up
#      or re-report -- the file simply does not exist.
#   2. A failed build is recorded and the function returns WITHOUT running
#      the (now-absent) binary. It never `throw`s, so one broken executable
#      can no longer prevent every later executable from even being built.
#   3. The final summary distinguishes three counts: how many executables
#      were even BUILT, how many of those were RUN, and how many of the ones
#      run actually PASSED -- so "156/156 passed" can never silently mean
#      "the other N never got that far".
#
# This replaces an earlier version that `throw`d the instant any one
# executable's build failed, which aborted the whole script before later
# executables were built at all. If a caller then re-ran (or separately
# invoked) an executable built in a previous, successful pass, its result
# described stale code, not the code the caller was trying to test --
# exactly the failure class this rewrite exists to make impossible. Found
# 2026-08-31 during the opus review that deliberately broke the coupling-
# index orientation check and watched it read as a pass.
$script:builtExes = @()
$script:buildFailures = @()
$script:failedExes = @()

function Invoke-HostTestExe {
    param(
        [Parameter(Mandatory)][string]$Name,
        [Parameter(Mandatory)][string]$ExePath,
        [Parameter(Mandatory)][string]$BuildCmd
    )
    if (Test-Path $ExePath) {
        Remove-Item -Force $ExePath
    }
    cmd.exe /c $BuildCmd
    $buildExit = $LASTEXITCODE
    if ($buildExit -ne 0 -or -not (Test-Path $ExePath)) {
        Write-Host "BUILD FAILED: $Name"
        $script:buildFailures += $Name
        return
    }
    $script:builtExes += $Name
    & $ExePath
    if ($LASTEXITCODE -ne 0) {
        $script:failedExes += $Name
    }
}

Invoke-HostTestExe -Name "main" -ExePath $exe -BuildCmd $cmd

# ---- test_zones_http.c: its own SEPARATE executable ------------------------
# See test_zones_http.c's header comment for why: it #includes zones_http.c
# directly (to reach parse_zone_fields(), a `static` function with no other
# seam), which DEFINES the real zones_config_get_*()/set_*() functions --
# and test_backup_import.c above already defines its OWN fake bodies for
# those same names to stub backup_import_apply()'s dependency on
# zones_http.h. Linking both into one executable would be a multiple-
# definition error, so this one is built and run as a second, independent
# binary instead of being added to $sources above.
#
# zones_http.c now calls into zones_config_json.c (the HTTP-free split of its
# own validation/decode/field-parsing core -- see that file's header comment)
# instead of defining those functions itself, so this executable's link needs
# zones_config_json.c compiled in alongside it, the same way $sources lists
# every other driver .c a test executable actually calls into.
#
# zones_config_json.c split 2026-09-04 (ROADMAP.md M15, the 1500-line rule)
# into three files -- see zones_config_json_internal.h's own doc comment for
# the map. All three compile in here, same convention as test_profiles_http.c
# picking up profiles_catalog_http.c/profiles_edit_http.c alongside
# profiles_http.c.
$exe2 = Join-Path $outDir "kilnctl_host_tests_zones.exe"
$exe2ObjDir = Join-Path $outDir "zones_obj\"
if (-not (Test-Path $exe2ObjDir)) { New-Item -ItemType Directory -Path $exe2ObjDir | Out-Null }
$cmd2 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$exe2ObjDir\`" /Fe:`"$exe2`" `"$(Join-Path $testDir 'test_zones_http.c')`" " +
        "`"$(Join-Path $driversDir 'persist/zones_config_json.c')`" " +
        "`"$(Join-Path $driversDir 'persist/zones_config_convert.c')`" " +
        "`"$(Join-Path $driversDir 'persist/zones_config_migrate.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
# fake_kv.c/hal_status.c/hal_esp_common.c added HW_ABSTRACTION.md Phase 3
# item 3 (nvs.h -> hal_kv.h migration): zones_http.c/zones_config_store.c now
# call hal_kv_*()/hal_status_to_esp_err() instead of nvs_*() directly, and
# test_zones_http.c's own nvs_test_enable()/nvs_test_clear() shims (see that
# file's header comment) drive the real fake_kv backend now.

Invoke-HostTestExe -Name "zones_http" -ExePath $exe2 -BuildCmd $cmd2

# ---- test_safety_cfg_http.c: its own THIRD, separate executable -----------
# Same reason as test_zones_http.c above: it #includes safety_cfg_http.c
# directly to reach its static parse_set_param_body()/build_commissioning_
# json()/apply_pairs() helpers, and defines its own fake bodies for
# safety_cfg_store_get_by_index() and friends -- the main executable already
# links the REAL ones via test_safety_cfg_store.c's #include of safety_cfg_
# store.c, so linking both into one binary would multiply-define every
# safety_cfg_store_* symbol.
$exe3 = Join-Path $outDir "kilnctl_host_tests_safety_cfg_http.exe"
$cmd3 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$outDir\\safety_cfg_http_`" /Fe:`"$exe3`" `"$(Join-Path $testDir 'test_safety_cfg_http.c')`""

Invoke-HostTestExe -Name "safety_cfg_http" -ExePath $exe3 -BuildCmd $cmd3

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
# PID_EXPANSION_PLAN.md sec 7.3: profile_executor_ramp_assist.c's
# ramp_assist_dwell_credit_tick() now calls cone_table_band_bottom_c()/
# cone_table_heat_work_weight() -- link the real module (already host-tested
# by test_cone_table.c, own executable) into $cmd4 below rather than faking
# it, same reasoning as pid.c/thermal_guard.c already linked there.
$cmd4 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$peObjDir\\`" /Fe:`"$exe4`" " +
        "`"$(Join-Path $testDir 'test_profile_executor_prestart.c')`" " +
        "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/thermal_guard.c')`" " +
        "`"$(Join-Path $driversDir 'control/heater_output.c')`" `"$(Join-Path $driversDir 'control/thermo_combine.c')`" " +
        "`"$(Join-Path $driversDir 'control/heat_enable.c')`" `"$(Join-Path $driversDir 'control/pid_fuzzy.c')`" " +
        "`"$(Join-Path $driversDir 'common/stack_margin.c')`" `"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`" " +
        "`"$(Join-Path $driversDir 'control/adaptive_tune.c')`" `"$(Join-Path $driversDir 'control/adaptive_tune_model.c')`" " +
        "`"$(Join-Path $driversDir 'control/adaptive_tune_ki.c')`" `"$(Join-Path $driversDir 'control/pid_autotune.c')`" " +
        "`"$(Join-Path $driversDir 'control/cone_table.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
# HW_ABSTRACTION.md Phase 3 item 3 (nvs.h -> hal_kv.h migration, batch
# 4: control/, safety/, profiles_http.c, ui_page_diagnostics.c): this
# executable links adaptive_tune.c/adaptive_tune_model.c/adaptive_tune_ki.c
# for real (see above), and adaptive_tune.c now calls hal_kv_*() and
# hal_status_to_esp_err() instead of nvs_*() directly, so the host hal_kv
# backend (fake_kv.c) and the shared esp_err_t<->hal_status_t mapper
# (hal_status.c/hal_esp_common.c) both need linking in.
# profile_executor.c split 2026-09-01 ("files over 1500 lines should be
# broken up where it makes sense") -- the new profile_executor_*.c pieces
# are NOT added as separate compile units above; test_profile_executor_
# prestart.c #includes all of them directly, same convention as
# test_zones_http.c's own multi-#include block, so this file keeps reaching
# every split-out module's `static` internals (the coupling solve caches
# among them) the same way it did when this was all one translation unit.

Invoke-HostTestExe -Name "profile_executor_prestart" -ExePath $exe4 -BuildCmd $cmd4

# ---- test_autotune_engine_prestart.c: its own FIFTH, separate executable --
# Same reasoning as test_profile_executor_prestart.c immediately above, for
# autotune_engine.c's identical pre-start guard (begin_run_locked()'s
# s_at.lock == NULL check). pid_autotune.c is linked in for real (already
# host-tested) rather than faked, same reasoning as profile_executor's pid.c.
$exe5 = Join-Path $outDir "kilnctl_host_tests_autotune_engine.exe"
$aeObjDir = Join-Path $outDir "ae"
New-Item -ItemType Directory -Force -Path $aeObjDir | Out-Null
$cmd5 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$aeObjDir\\`" /Fe:`"$exe5`" " +
        "`"$(Join-Path $testDir 'test_autotune_engine_prestart.c')`" " +
        "`"$(Join-Path $driversDir 'control/thermal_guard.c')`" `"$(Join-Path $driversDir 'control/heater_output.c')`" " +
        "`"$(Join-Path $driversDir 'control/thermo_combine.c')`" `"$(Join-Path $driversDir 'control/pid_autotune.c')`" " +
        "`"$(Join-Path $driversDir 'control/heat_enable.c')`" `"$(Join-Path $driversDir 'common/stack_margin.c')`""
# stack_margin.c added DRAM_PSRAM_PLAN.md Phase 0 (4.2): autotune_engine.c's
# autotune_engine_start() now calls stack_margin_register() (registration
# only, no size change), and this executable #includes autotune_engine.c
# directly (same as test_profile_executor_prestart.c's cmd4 above, which
# already linked stack_margin.c for the same reason).
#
# autotune_engine.c split 2026-09-04 (ROADMAP.md M15 A3, same "files over
# 1500 lines" reasoning as profile_executor.c's split above) into
# autotune_engine.c/_guard.c/_step_identify.c/_relay.c/_coupling.c -- the four
# new pieces are NOT added as separate compile units here either, same
# reasoning as profile_executor's own split just above: test_autotune_engine_
# prestart.c #includes all five directly.

Invoke-HostTestExe -Name "autotune_engine_prestart" -ExePath $exe5 -BuildCmd $cmd5

# rules_task.c/test_rules_task_prestart.c (the SIXTH executable this script
# used to build) were deleted 2026-08-27 along with the rest of the rule
# engine -- see relay_authority.h's RELAY_OWNER_RULE comment. Executable
# numbering below (SEVENTH, EIGHTH, ...) is kept as-is rather than renumbered,
# to avoid an unrelated diff on every comment in this file.

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
$phObjDir = Join-Path $outDir "profiles_http_obj"
New-Item -ItemType Directory -Force -Path $phObjDir | Out-Null
$cmd7 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$phObjDir\\`" /Fe:`"$exe7`" `"$(Join-Path $testDir 'test_profiles_http.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
# HW_ABSTRACTION.md Phase 3 item 3 (nvs.h -> hal_kv.h migration, batch
# 4): profiles_http.c (#included directly above) now calls hal_kv_*() and
# hal_status_to_esp_err() instead of nvs_*() directly, so this executable
# needs the host hal_kv backend and the shared esp_err_t<->hal_status_t
# mapper linked in. /Fo: switched from a single-file name prefix
# ("$outDir\profiles_") to a real object directory now that this cl
# invocation compiles more than one source -- a name-prefix /Fo: only works
# for a single source file; MSVC needs a directory (trailing backslash) once
# there is more than one .obj to place.

Invoke-HostTestExe -Name "profiles_http" -ExePath $exe7 -BuildCmd $cmd7

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
$cmd8 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$otaObjDir\\`" /Fe:`"$exe8`" " +
        "`"$(Join-Path $testDir 'test_ota_http.c')`" " +
        "`"$(Join-Path $driversDir 'net/ota_auth.c')`" `"$(Join-Path $driversDir 'net/ota_interlock.c')`" " +
        "`"$(Join-Path $driversDir 'persist/ota_record.c')`" `"$(Join-Path $driversDir 'http/ota_http_util.c')`" " +
        "`"$(Join-Path $driversDir 'common/stack_margin.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" " +
        "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
# hal_time migration (HW_ABSTRACTION.md item 5): ota_http_pico.c, one of
# the files #included directly into test_ota_http.c above, now calls
# hal_time_now_us() instead of esp_timer_get_time(); fake_time.c supplies it.
# stack_margin.c added DRAM_PSRAM_PLAN.md Phase 0 (4.2): ota_http.c's
# recovery-exit/rollback/pico-rollback reboot task starts now call
# stack_margin_register() (registration only, no size change), and this
# executable #includes ota_http.c directly.
# fake_kv.c added HW_ABSTRACTION.md Phase 3 item 3 (nvs.h -> hal_kv.h
# migration): ota_record.c, linked in for real above, now calls hal_kv_*()
# instead of nvs_*() directly. hal_esp_common.c added in the same pass's
# flash-safety review follow-up: ota_record.c now also calls
# hal_status_to_esp_err() to preserve its ESP_FAIL/mapped-error return
# contract instead of collapsing every failure to plain ESP_FAIL.

Invoke-HostTestExe -Name "ota_http" -ExePath $exe8 -BuildCmd $cmd8

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
$cmd9 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$linkObjDir\\`" /Fe:`"$exe9`" " +
        "`"$(Join-Path $testDir 'test_uart_protocol_link_delegate.c')`" " +
        "`"$(Join-Path $commonSrc 'kilnlink_frame.c')`" `"$(Join-Path $commonSrc 'kilnlink_crc.c')`""

Invoke-HostTestExe -Name "uart_protocol_link_delegate" -ExePath $exe9 -BuildCmd $cmd9

# ---- test_board_temps.c: its own TENTH, separate executable --------------
# Same reason as test_zones_http.c above: it #includes board_temps.c
# directly to reach board_temps_get() (the pure half of that file, no other
# seam). 2026-09-06 migration: board_temps.c now goes through
# hal_sysinfo_temp_*() instead of driver/temperature_sensor.h directly, so
# this executable links the real host fake_sysinfo.c + hal_status.c (same
# pair exe21/test_partition_info_http.c links) instead of defining its own
# temperature_sensor_*() stub bodies -- own executable so fake_sysinfo.c's
# hal_sysinfo_* symbols never collide with any other test file's own fakes
# of the same names (test_partition_info_http.c already claims fake_sysinfo.c
# for itself the same way).
$exe10 = Join-Path $outDir "kilnctl_host_tests_board_temps.exe"
$btObjDir = Join-Path $outDir "bt"
New-Item -ItemType Directory -Force -Path $btObjDir | Out-Null
$cmd10 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$btObjDir\\`" /Fe:`"$exe10`" `"$(Join-Path $testDir 'test_board_temps.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

Invoke-HostTestExe -Name "board_temps" -ExePath $exe10 -BuildCmd $cmd10

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
$cmd11 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$kioObjDir\\`" /Fe:`"$exe11`" " +
        "`"$(Join-Path $testDir 'test_kiln_io_owner.c')`" `"$(Join-Path $driversDir 'owners/kiln_io.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/spi/owner_slot_pool.c')`" `"$(Join-Path $driversDir 'common/stack_margin.c')`""
# stack_margin.c added DRAM_PSRAM_PLAN.md Phase 0 (4.2): kiln_io_owner.c's
# kiln_io_owner_start() now calls stack_margin_register() (registration
# only, no size change), and this executable #includes kiln_io_owner.c
# directly.

Invoke-HostTestExe -Name "kiln_io_owner" -ExePath $exe11 -BuildCmd $cmd11

# ---- test_safety_trip_words.c: its own TWELFTH, separate executable ------
# Header-only (safety_trip_words.h is static inline, no .c) -- see the test
# file's own header comment. No shared-symbol collision risk, but every
# other single-purpose test here gets its own exe, so this follows suit.
$exe12 = Join-Path $outDir "kilnctl_host_tests_safety_trip_words.exe"
$stwObjDir = Join-Path $outDir "stw"
New-Item -ItemType Directory -Force -Path $stwObjDir | Out-Null
$cmd12 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$stwObjDir\\`" /Fe:`"$exe12`" `"$(Join-Path $testDir 'test_safety_trip_words.c')`""

Invoke-HostTestExe -Name "safety_trip_words" -ExePath $exe12 -BuildCmd $cmd12

# ---- test_safety_trip_decision.c: its own THIRTEENTH, separate executable
# 2026-08-28 opus review: safety_apply_trip_event()'s trip_fault_sources_
# valid logic (safety_link.c) had no automated test because at the time no
# host harness linked safety_link.c -- it is ~3900 lines pulling in
# driver/gpio.h, driver/uart.h, esp_heap_caps.h, a dozen kilnlink/* codecs,
# and safety_cfg_store.h. The decision itself was factored into
# safety_trip_decision.c, a pure, dependency-free (stdbool/stdint only)
# translation unit safety_link.c calls into -- links the real .c (not a
# stub) since there is nothing in it to stub. Own executable per this file's
# usual per-purpose convention, though the true reason here is simpler: it
# needs no stub headers, unlike every test file above it.
#
# ROADMAP.md M13 TASK 2 (below, exe14) is what actually gets safety_link.c
# ITSELF compiling and linking off-target -- this executable stays as the
# smaller, dependency-free harness for the three-line decision alone.
$exe13 = Join-Path $outDir "kilnctl_host_tests_safety_trip_decision.exe"
$stdObjDir = Join-Path $outDir "std"
New-Item -ItemType Directory -Force -Path $stdObjDir | Out-Null
$cmd13 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$stdObjDir\\`" /Fe:`"$exe13`" " +
        "`"$(Join-Path $testDir 'test_safety_trip_decision.c')`" `"$(Join-Path $driversDir 'safety/safety_trip_decision.c')`""

Invoke-HostTestExe -Name "safety_trip_decision" -ExePath $exe13 -BuildCmd $cmd13

# ---- test_safety_link_compile.c: its own FOURTEENTH, separate executable --
# ROADMAP.md M13 TASK 2: safety_link.c itself now compiles and links off-
# target, following the same precedent as ota_http.c/factory_reset.c
# (commit da4918c) and zones_http.c/profile_executor.c/profiles_http.c
# before it -- #includes the real safety_link.c directly (its own header
# comment lists which static functions have no other seam) rather than
# restate its logic somewhere host-friendly.
#
# The stub surface needed (App/test/stubs/{driver/gpio.h, driver/uart.h,
# sdkconfig.h, esp_err.h, esp_random.h, uart_owner.h, uart_protocol.h}) was
# smaller than expected -- safety_link.c's own real-firmware dependencies
# (MAX31856.h/kiln_io.h/profile_executor.h/thermo_owner.h/zones_http.h/
# safety_cfg_store.h) already compile off-target cleanly, since other host
# tests link them for real. Only ~10 external functions from those headers
# and the uart_owner_*/uart_protocol_* hardware-driving calls needed FAKE
# bodies (this test file's own header comment lists them and what they
# don't cover: safety_link_start()'s real hardware init path, safety_poll_
# task()'s scheduling/timing, and safety_exchange()'s blocking request/
# reply cycle are all still untested off-target -- only the two static wire-
# decode decisions this file targets are exercised). Needs the same
# kilnlink_*.c set as test_safety_cfg_http.c/test_uart_protocol_link_
# delegate.c above, plus stack_margin.c and safety_trip_decision.c (both
# real, already host-tested elsewhere) -- own executable to keep its
# xSemaphoreTake() redirect (same precedent as test_ota_http.c/test_safety_
# cfg_store.c) and its uart_owner_*/uart_protocol_* fakes from colliding
# with any other test file's definitions of those same symbols.
$exe14 = Join-Path $outDir "kilnctl_host_tests_safety_link.exe"
$slObjDir = Join-Path $outDir "sl"
New-Item -ItemType Directory -Force -Path $slObjDir | Out-Null
$slExtra = @("kilnlink_config_page.c", "kilnlink_announce.c", "kilnlink_announce_reboot.c",
             "kilnlink_clear_trip.c", "kilnlink_commit_config.c", "kilnlink_commit_config_rejected.c",
             "kilnlink_context.c", "kilnlink_get_config_page.c", "kilnlink_get_ct_cal.c",
             "kilnlink_rollback.c", "kilnlink_rollback_result.c", "kilnlink_set_config.c", "kilnlink_set_ct_cal.c",
             "kilnlink_set_log_level.c", "kilnlink_set_param.c", "kilnlink_frame.c", "kilnlink_crc.c",
             "kilnlink_param.c", "kilnlink_param_value.c") | ForEach-Object { "`"$(Join-Path $commonSrc $_)`"" }
$cmd14 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$slObjDir\\`" /Fe:`"$exe14`" " +
        "`"$(Join-Path $testDir 'test_safety_link_compile.c')`" " +
        "`"$(Join-Path $driversDir 'common/stack_margin.c')`" `"$(Join-Path $driversDir 'safety/safety_trip_decision.c')`" " +
        "`"$(Join-Path $driversDir 'safety/safety_link_frame.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_gpio.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" " +
        "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`" " +
        "$($slExtra -join ' ')"
# hal_time migration (HW_ABSTRACTION.md item 5): safety_link_commands.c,
# one of the files #included by test_safety_link_compile.c above, now calls
# hal_time_now_us() instead of esp_timer_get_time(); fake_time.c supplies it.
# esp_random.h migration (HW_ABSTRACTION.md, 2026-09-06): safety_link.c
# now calls hal_sysinfo_random_u32() instead of esp_random() for esp_boot_id
# -- fake_sysinfo.c supplies it (unscripted, so this returns the fixed
# 0xA5A5A5A5 fallback; the value itself is diagnostic-only, never asserted
# on by this file's tests).

Invoke-HostTestExe -Name "safety_link" -ExePath $exe14 -BuildCmd $cmd14

# ---- test_dashboard_json.c: its own FIFTEENTH, separate executable --------
# Opus review round 3, blocker 1: dashboard_http.c's /api/control handler was
# silently emitting truncated (invalid) JSON at 3 zones once ff_hold_used_
# matrix/ff_hold_infeasible pushed the per-zone object past the old buffer
# budget -- see dashboard_json.h's own doc comment for why json_escape()/
# append_zone_status_json() were split out of dashboard_http.c into their
# own file rather than stubbed in place: dashboard_http.c #includes
# lvgl_port.h at file scope, which pulls in the LCD/touch driver stack
# (ILI9488.h's __attribute__((format(printf,...))), a GCC extension MSVC's
# host toolchain rejects outright), so that whole translation unit cannot
# compile on this host toolchain no matter how many ESP-IDF stub headers are
# added -- confirmed by attempting it. dashboard_json.c has no such
# dependency (profile_executor.h + libc only, the same header chain
# profile_executor.c's own test already proves compiles off-target), so it
# gets its own minimal executable rather than joining the main one.
$exe15 = Join-Path $outDir "kilnctl_host_tests_dashboard_json.exe"
$djObjDir = Join-Path $outDir "dj"
New-Item -ItemType Directory -Force -Path $djObjDir | Out-Null
$cmd15 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$djObjDir\\`" /Fe:`"$exe15`" `"$(Join-Path $testDir 'test_dashboard_json.c')`""

Invoke-HostTestExe -Name "dashboard_json" -ExePath $exe15 -BuildCmd $cmd15

# ---- test_telemetry_format.c: its own SIXTEENTH, separate executable ------
# telemetry_format.c (firing/autotune telemetry line formatters,
# telemetry_log.c's debug-UART feature) has the same host-testability shape
# as dashboard_json.c: profile_executor.h + autotune_engine.h + libc only,
# no FreeRTOS/ESP_LOGI dependency (those live in telemetry_log.c, which is
# NOT built here -- it needs the real xTaskCreatePinnedToCoreWithCaps/
# ESP_LOGI machinery this host toolchain doesn't have a meaningful stub
# story for). Own executable rather than joining exe15 so a future edit to
# either file's stub surface can't collide.
$exe16 = Join-Path $outDir "kilnctl_host_tests_telemetry_format.exe"
$tfObjDir = Join-Path $outDir "tf"
New-Item -ItemType Directory -Force -Path $tfObjDir | Out-Null
$cmd16 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$tfObjDir\\`" /Fe:`"$exe16`" `"$(Join-Path $testDir 'test_telemetry_format.c')`""

Invoke-HostTestExe -Name "telemetry_format" -ExePath $exe16 -BuildCmd $cmd16

# ---- test_adaptive_tune.c: its own SEVENTEENTH, separate executable -------
# PID_EXPANSION_PLAN.md Phase 7d. #includes adaptive_tune.c directly (same
# convention as test_autotune_engine_prestart.c/test_profile_executor_
# prestart.c above) to reach its guard constants with no other seam, and
# defines its own tiny fake zones_config_get_model()/set_model()/get_pid()/
# set_pid() table plus minimal httpd_*()/wifi_provision_http_get_server()/
# uart_bridge_ext_run_on_flash_worker() stubs -- own executable so none of
# those collide with any other test file's fakes of the same names.
# pid_autotune.c is linked in for real (already host-tested elsewhere) so
# the SIMC recompute this file drives is the actual production math, not a
# stand-in.
$exe17 = Join-Path $outDir "kilnctl_host_tests_adaptive_tune.exe"
$atObjDir = Join-Path $outDir "at"
New-Item -ItemType Directory -Force -Path $atObjDir | Out-Null
# zone_coupling_solve.c is now linked in too (real code, not a fake) --
# adaptive_tune.c's full coupled identification (PID_EXPANSION_PLAN.md 3.3)
# calls zone_coupling_gauss_solve_partial_pivot_vec() for the actual linear
# solve/conditioning check; this test file supplies the one symbol that
# module's OTHER (unused-by-this-file) functions still need at link time,
# zones_config_get_coupling(), as a tiny fake table alongside its existing
# zones_config_get/set_model/pid fakes.
$cmd17 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$atObjDir\\`" /Fe:`"$exe17`" " +
        "`"$(Join-Path $testDir 'test_adaptive_tune.c')`" `"$(Join-Path $driversDir 'control/pid_autotune.c')`" " +
        "`"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
# HW_ABSTRACTION.md Phase 3 item 3 (nvs.h -> hal_kv.h migration, batch
# 4): adaptive_tune.c (#included directly by test_adaptive_tune.c) now calls
# hal_kv_*() and hal_status_to_esp_err() instead of nvs_*() directly, so this
# executable needs the host hal_kv backend and the shared
# esp_err_t<->hal_status_t mapper linked in.

Invoke-HostTestExe -Name "adaptive_tune" -ExePath $exe17 -BuildCmd $cmd17

# ---- test_event_log.c: its own EIGHTEENTH, separate executable ------------
# event_log.c (2026-09-02 flash-logging change, event_log.h's file banner)
# is pure encode/decode -- no ESP-IDF/FreeRTOS, same host-testability shape
# as telemetry_format.c/dashboard_json.c above. event_log_emit() (the
# esp_timer_get_time()/flash-worker device glue) lives in the SEPARATE
# event_log_emit.c on purpose and is NOT linked here, matching how
# telemetry_log.c itself is excluded from exe16.
$exe18 = Join-Path $outDir "kilnctl_host_tests_event_log.exe"
$elObjDir = Join-Path $outDir "el"
New-Item -ItemType Directory -Force -Path $elObjDir | Out-Null
$cmd18 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$elObjDir\\`" /Fe:`"$exe18`" `"$(Join-Path $testDir 'test_event_log.c')`" " +
        "`"$(Join-Path $driversDir 'persist/event_log.c')`""

Invoke-HostTestExe -Name "event_log" -ExePath $exe18 -BuildCmd $cmd18

# ---- test_run_state.c / test_relay_cycles.c: their own NINETEENTH executable
# DRAM_PSRAM_PLAN.md section 7 safety-net pass: both modules' persist_locked()
# had no caller_stack_is_external() PSRAM-stack guard until this pass, despite
# being reached directly from profile_executor's tick/halt path -- the same
# task that plan section names as its highest-care relocation candidate. Own
# executable, /std:c11, because run_state.c uses _Static_assert (line ~154),
# which cl.exe only accepts under /std:c11 -- the main executable's $sources
# are built without that flag (same reason test_zones_http.c/
# test_profiles_http.c get their own /std:c11 executables). Both files
# #include their driver .c directly (same convention as test_crash_report.c),
# so neither run_state.c nor relay_cycles.c is added to $sources above.
$exe19 = Join-Path $outDir "kilnctl_host_tests_run_state_relay_cycles.exe"
$rsObjDir = Join-Path $outDir "rs"
New-Item -ItemType Directory -Force -Path $rsObjDir | Out-Null
$cmd19 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$rsObjDir\\`" /Fe:`"$exe19`" `"$(Join-Path $testDir 'test_run_state.c')`" " +
        "`"$(Join-Path $testDir 'test_relay_cycles.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
        "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
# hal_time migration (HW_ABSTRACTION.md item 5): both run_state.c and
# relay_cycles.c now call hal_time_now_us() instead of esp_timer_get_time();
# fake_time.c supplies it.
# fake_kv.c/hal_status.c/hal_esp_common.c added HW_ABSTRACTION.md Phase 3
# item 3 (nvs.h -> hal_kv.h migration): BOTH run_state.c and relay_cycles.c
# now call hal_kv_*() instead of nvs_*() directly (test_run_state.c/
# test_relay_cycles.c no longer use stubs/nvs.h at all), and both also call
# hal_status_to_esp_err() to preserve their ESP_FAIL/mapped-error return
# contract instead of collapsing every failure to plain ESP_FAIL -- so this
# executable needs the full host hal_kv backend (fake_kv.c + hal_status.c +
# hal_esp_common.c) linked in, not just fake_time.c. hal_status.c/
# hal_esp_common.c are not otherwise in this executable's link (the main
# $sources list only pulls them in for the main "exe" build).

Invoke-HostTestExe -Name "run_state_relay_cycles" -ExePath $exe19 -BuildCmd $cmd19

# ---- test_zone_coupling_solve.c: its own TWENTIETH, separate executable --
# PID_EXPANSION_PLAN.md sec 3.2's "CORRECTION 2026-09-02d" seam-sizing pass.
# Own executable, same reason test_adaptive_tune.c's exe17 is: it defines
# its own tiny fake zones_config_get_coupling() (zone_coupling_solve.c's
# only zones_http.h dependency), which would multiply-define against exe4's
# or exe17's own fakes of the same name if linked alongside either.
$exe20 = Join-Path $outDir "kilnctl_host_tests_zone_coupling_solve.exe"
$zcsObjDir = Join-Path $outDir "zcs"
New-Item -ItemType Directory -Force -Path $zcsObjDir | Out-Null
$cmd20 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$zcsObjDir\\`" /Fe:`"$exe20`" `"$(Join-Path $testDir 'test_zone_coupling_solve.c')`" " +
        "`"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`""

Invoke-HostTestExe -Name "zone_coupling_solve" -ExePath $exe20 -BuildCmd $cmd20

# ---- test_partition_info_http.c: its own TWENTY-FIRST, separate executable
# FLASH_BUDGET_PLAN.md sec 8 item 3's replacement for the broken JTAG-based
# partition-table read: GET /api/partitions (partition_info_http.c). Own
# executable, same "no other seam" reason as test_board_temps.c above: it
# #includes partition_info_http.c directly to reach the static
# api_partitions_get_handler(), and supplies its own fake
# esp_partition_find()/esp_partition_next()/esp_ota_get_running_partition()
# bodies, which would multiply-define against any other test file's own
# fakes of those symbols (test_ota_http.c has its own, trivial, different
# ones) if linked together.
$exe21 = Join-Path $outDir "kilnctl_host_tests_partition_info_http.exe"
$pihObjDir = Join-Path $outDir "pih"
New-Item -ItemType Directory -Force -Path $pihObjDir | Out-Null
$cmd21 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$pihObjDir\\`" /Fe:`"$exe21`" `"$(Join-Path $testDir 'test_partition_info_http.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`""

Invoke-HostTestExe -Name "partition_info_http" -ExePath $exe21 -BuildCmd $cmd21

# ---- test_adaptive_tune_http.c: its own TWENTY-SECOND, separate executable
# Live bug: GET /api/adaptive_tune (adaptive_tune_http.c's status_get_
# handler()) was serving exactly 1023 bytes of TRUNCATED JSON -- the
# 320*MAX31856_CHANNEL_COUNT+64 buffer budget was a guess never measured
# against the real per-zone snprintf(), and the old code CLAMPED on
# overflow instead of failing, so the truncation was silent (200 OK, hung
# the zones page's "Continuous Tuning" panel on "Loading..." instead of
# erroring visibly). Same dashboard_json.c/test_dashboard_json.c
# mirror-render pattern as exe15 above -- adaptive_tune_http.c's status_
# get_handler() is `static` with no other seam, so this file hand-mirrors
# its exact snprintf() format string rather than pulling in the whole
# translation unit's stub surface (wifi_provision_http_get_server()/
# adaptive_tune_get_status()/set_enabled()/revert(), none of which this
# test exercises). Same "stubDir + commonInc only" include story as exe15:
# adaptive_tune.h's own #include chain (profile_executor.h -> MAX31856.h)
# is already proven host-compilable by dashboard_json.c/exe15's identical
# chain, resolved via the including file's own directory, not driversDir.
$exe22 = Join-Path $outDir "kilnctl_host_tests_adaptive_tune_http.exe"
$athObjDir = Join-Path $outDir "ath"
New-Item -ItemType Directory -Force -Path $athObjDir | Out-Null
$cmd22 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$athObjDir\\`" /Fe:`"$exe22`" `"$(Join-Path $testDir 'test_adaptive_tune_http.c')`""

Invoke-HostTestExe -Name "adaptive_tune_http" -ExePath $exe22 -BuildCmd $cmd22

# ---- test_profiles_builtin.c: its own 23rd, separate executable -----------
# Added 2026-09-05 for the PROFILES_BUILTIN_CONE_UNRATED sentinel (owner
# decision: the 10 catalogue entries whose source page states no cone get
# "Unrated" instead of an invented number, and sort last). #includes
# profiles_builtin.c directly to get the REAL generated
# profiles_builtin_table.inc, unlike test_profiles_http.c's own builtin
# stand-in a few executables up (that one deliberately fakes an EMPTY
# catalogue -- it doesn't exercise this table at all). Own executable so its
# real profiles_builtin_get()/profiles_builtin_entry() bodies never collide
# with test_profiles_http.c's fakes of the same names.
$exe23 = Join-Path $outDir "kilnctl_host_tests_profiles_builtin.exe"
$pbObjDir = Join-Path $outDir "pb"
New-Item -ItemType Directory -Force -Path $pbObjDir | Out-Null
$cmd23 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$pbObjDir\\`" /Fe:`"$exe23`" `"$(Join-Path $testDir 'test_profiles_builtin.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
        "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
# fake_kv.c/hal_status.c added HW_ABSTRACTION.md Phase 3 item 3 (nvs.h ->
# hal_kv.h migration): profiles_builtin.c's hidden-mask persistence now calls
# hal_kv_get_u32()/hal_kv_set_u32()/hal_kv_init_partition() instead of
# nvs_get_u32()/nvs_set_u32()/nvs_flash_init_partition() directly, so this
# executable needs the host hal_kv backend linked in (this file's own local
# nvs_get_u32()/nvs_set_u32() stand-ins were removed accordingly).
# hal_esp_common.c added in the same pass's flash-safety review follow-up:
# profiles_builtin.c now also calls hal_status_to_esp_err() to preserve its
# ESP_FAIL/mapped-error return contract instead of collapsing every failure
# to plain ESP_FAIL.

Invoke-HostTestExe -Name "profiles_builtin" -ExePath $exe23 -BuildCmd $cmd23

# ---- test_ft6336u.c: its own 24th, separate executable --------------------
# HAL Phase 1b (docs/HW_ABSTRACTION.md): FT6336U.c was rewritten to go
# through interface/hal_i2c.h instead of driver/i2c_master.h + i2c_owner.c
# directly -- see FT6336U.c/.h's own header comments. This links FT6336U.c
# as a real translation unit (own seam is entirely public API, no need to
# #include it) against the host hal_i2c backend
# (firmware/hwAbstraction/host/fake_i2c.c) plus hal_status.c (the
# hal_status_to_name() table, backend-independent). Own executable so
# fake_i2c.c's hal_i2c_bus_t/hal_i2c_device_t definitions never collide with
# any other test file linking a different hal_i2c backend (there is only one
# today, but every other *_http.c-direct test here follows the same
# one-executable-per-fake-surface convention).
$exe24 = Join-Path $outDir "kilnctl_host_tests_ft6336u.exe"
$ft6336uObjDir = Join-Path $outDir "ft6336u"
New-Item -ItemType Directory -Force -Path $ft6336uObjDir | Out-Null
$cmd24 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$ft6336uObjDir\\`" /Fe:`"$exe24`" " +
        "`"$(Join-Path $testDir 'test_ft6336u.c')`" " +
        "`"$(Join-Path $driversDir 'hw/FT6336U.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_i2c.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`""

Invoke-HostTestExe -Name "ft6336u" -ExePath $exe24 -BuildCmd $cmd24

# ---- test_max31856_hal_spi.c: its own 25th, separate executable -----------
# HAL Phase 1b CORRECTED (docs/HW_ABSTRACTION.md, 2026-09-05): MAX31856.c
# was rewritten to go through interface/hal_spi.h instead of driving
# esp_spi_owner.c's spi_owner_t directly -- see MAX31856.c/.h's own header
# comments (this was the fix for hal_spi_esp.c duplicating esp_spi_owner.c's
# whole queue/pool/wedge design instead of adapting it -- Phase 2 status,
# commit 26b16a6). Links MAX31856.c and max31856_codec.c (the decode
# functions MAX31856_read() calls) as real translation units against the
# host hal_spi backend (firmware/hwAbstraction/host/fake_spi.c) plus
# hal_status.c -- same one-executable-per-fake-surface convention as
# test_ft6336u.c above.
$exe25 = Join-Path $outDir "kilnctl_host_tests_max31856_hal_spi.exe"
$max31856ObjDir = Join-Path $outDir "max31856hs"
New-Item -ItemType Directory -Force -Path $max31856ObjDir | Out-Null
# fake_time.c added 2026-09-06: HW_ABSTRACTION.md "Still open"'s thermocouple
# read-cycle-latency instrumentation added a hal_time_now_us() call inside
# MAX31856_read_all() itself (same interface/hal_time.h lvgl_port.c's flush
# stats already use) -- MAX31856.c is now a real caller of that symbol, so
# this executable needs the host fake for it, same as every other target in
# this script that links a TU calling hal_time_now_us().
$cmd25 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$max31856ObjDir\\`" /Fe:`"$exe25`" " +
        "`"$(Join-Path $testDir 'test_max31856_hal_spi.c')`" " +
        "`"$(Join-Path $driversDir 'hw/MAX31856.c')`" `"$(Join-Path $driversDir 'hw/max31856_codec.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_spi.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
        "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`""

Invoke-HostTestExe -Name "max31856_hal_spi" -ExePath $exe25 -BuildCmd $cmd25

# ---- test_hal_i2c_adopt.c: its own 26th, separate executable --------------
# 2026-09-06 fix: hal_i2c_bus_init()'s old ALREADY_INIT recovery path (a
# second hal_i2c_bus_t sharing an already-open I2C port, e.g. FT6336U
# sharing SX1509's I2C_NUM_0) left the port half-released on hardware --
# see firmware/hwAbstraction/esp/i2c/hal_i2c_esp_owner.h's header comment
# and main_boot_early.c's FT6336U bring-up. The replacement,
# hal_i2c_esp_adopt(), is ESP-only and cannot link on host (real
# driver/i2c_master.h), so this test instead exercises
# fake_i2c_bus_adopt() (firmware/hwAbstraction/host/fake_i2c.c), added to
# model the same "share the underlying slot, deinit of the adopted copy is
# a no-op on it" contract at the portable hal_i2c.h level. Links only
# fake_i2c.c + hal_status.c -- no App/ driver under test here, just the
# fake backend's own adopt semantics.
$exe26 = Join-Path $outDir "kilnctl_host_tests_hal_i2c_adopt.exe"
$i2cAdoptObjDir = Join-Path $outDir "i2cadopt"
New-Item -ItemType Directory -Force -Path $i2cAdoptObjDir | Out-Null
$cmd26 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$i2cAdoptObjDir\\`" /Fe:`"$exe26`" " +
        "`"$(Join-Path $testDir 'test_hal_i2c_adopt.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_i2c.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`""

Invoke-HostTestExe -Name "hal_i2c_adopt" -ExePath $exe26 -BuildCmd $cmd26

# ---- test_hal_spi_adopt.c: its own 27th, separate executable --------------
# 2026-09-06 fix: hal_spi_bus_init()'s old ALREADY_INIT recovery path (a
# second hal_spi_bus_t sharing an already-open SPI host, e.g. MAX31856
# sharing main_boot_early.c's shared thermo/display bus) let a second caller
# create its OWN spi_owner_t/task on hardware -- see
# firmware/hwAbstraction/interface/hal_spi.h's hal_spi_bus_adopt() doc
# comment and main_boot_early.c's shared-SPI-bus bring-up. The replacement,
# hal_spi_bus_adopt(), is implemented by both the ESP backend (real
# driver/spi_master.h, cannot link on host) and the host fake
# (firmware/hwAbstraction/host/fake_spi.c), so this test exercises it
# directly at the portable hal_spi.h level. Links only fake_spi.c +
# hal_status.c -- no App/ driver under test here, just the fake backend's
# own adopt semantics (mirrors test_hal_i2c_adopt.c's own scope note).
$exe27 = Join-Path $outDir "kilnctl_host_tests_hal_spi_adopt.exe"
$spiAdoptObjDir = Join-Path $outDir "spiadopt"
New-Item -ItemType Directory -Force -Path $spiAdoptObjDir | Out-Null
$cmd27 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$spiAdoptObjDir\\`" /Fe:`"$exe27`" " +
        "`"$(Join-Path $testDir 'test_hal_spi_adopt.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_spi.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`""

Invoke-HostTestExe -Name "hal_spi_adopt" -ExePath $exe27 -BuildCmd $cmd27

# ---- test_hal_spi_async.c: its own 28th, separate executable --------------
# 2026-09-06: hal_spi_transfer_async()'s return/callback contract
# (interface/hal_spi.h), defined BEFORE CONFIG_KILNCTL_SPI_ASYNC_FLUSH is
# ever turned on -- see that header's doc comment on the function. Same
# "hal_spi_esp.c cannot link on host" scope note as exe25/exe27 above: this
# exercises the contract as fake_spi.c (the host backend) models it --
# no-synchronous-completion, FIFO completion ordering, and the queue-full/
# wedge path. Links only fake_spi.c + hal_status.c, same shape as exe27.
$exe28 = Join-Path $outDir "kilnctl_host_tests_hal_spi_async.exe"
$spiAsyncObjDir = Join-Path $outDir "spiasync"
New-Item -ItemType Directory -Force -Path $spiAsyncObjDir | Out-Null
$cmd28 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
        "/Fo:`"$spiAsyncObjDir\\`" /Fe:`"$exe28`" " +
        "`"$(Join-Path $testDir 'test_hal_spi_async.c')`" " +
        "`"$(Join-Path $hwAbsDir 'host/fake_spi.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`""

Invoke-HostTestExe -Name "hal_spi_async" -ExePath $exe28 -BuildCmd $cmd28

# ---- summary ----------------------------------------------------------
#
# 28 executables are attempted above (main + zones_http + safety_cfg_http +
# profile_executor_prestart + autotune_engine_prestart + profiles_http +
# ota_http + uart_protocol_link_delegate + board_temps + kiln_io_owner +
# safety_trip_words + safety_trip_decision + safety_link + dashboard_json +
# telemetry_format + adaptive_tune + event_log + run_state_relay_cycles +
# zone_coupling_solve + partition_info_http + adaptive_tune_http +
# profiles_builtin + ft6336u + max31856_hal_spi + hal_i2c_adopt +
# hal_spi_adopt + hal_spi_async).
# Report how many of those were even built, separately from how many of the
# built ones passed, so a partial run can never read as a full green suite.
$totalExpected = 28
Write-Host ""
Write-Host "Built: $($script:builtExes.Count)/$totalExpected executables"
if ($script:buildFailures.Count -gt 0) {
    Write-Host "BUILD FAILURES ($($script:buildFailures.Count)) -- these did not even run:"
    foreach ($f in $script:buildFailures) { Write-Host "  $f" }
}
if ($script:failedExes.Count -gt 0) {
    Write-Host "RUN FAILURES ($($script:failedExes.Count)):"
    foreach ($f in $script:failedExes) { Write-Host "  $f" }
}

if ($script:buildFailures.Count -gt 0 -or $script:failedExes.Count -gt 0) {
    exit 1
}

Write-Host "all $totalExpected host test executables built and passed"
exit 0
