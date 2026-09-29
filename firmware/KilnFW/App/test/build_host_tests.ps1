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
#
# $ErrorActionPreference below is "Stop", so any stderr line is fatal --
# including a `vswhere.exe is not recognized` line, which shows up when an
# ESP-IDF export script has already rewritten PATH in this same shell. Fix:
# prepend `C:\Program Files (x86)\Microsoft Visual Studio\Installer` to PATH
# before running this script.
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

. (Join-Path $PSScriptRoot "../../../../tools/build_lock.ps1")
. (Join-Path $PSScriptRoot "../../../../tools/build_gate.ps1")
$buildLockName = "kilnfw_host_tests_" + ([System.Text.RegularExpressions.Regex]::Replace($outDir, '[^A-Za-z0-9]+', '_'))
$buildGate = Enter-KilnBuildGate -Label "kilnfw_host_tests"
try {
# Enter-BuildLock is INSIDE the gate's try (opus review A5): if it throws
# before its own try block starts, the gate is still released by the outer
# finally below -- a flat gate/lock/try/finally chain would leak the gate
# slot forever in that case.
$buildLock = Enter-BuildLock -Name $buildLockName
try {
    $exe = Join-Path $outDir "kilnctl_host_tests.exe"

    $sources = @(
        (Join-Path $testDir "test_main.c"),
        (Join-Path $testDir "test_pid.c"),
        (Join-Path $testDir "test_thermal_guard.c"),
        (Join-Path $testDir "test_on_off_trigger_decide.c"),
        (Join-Path $testDir "test_heater_output.c"),
        (Join-Path $testDir "test_closed_loop.c"),
        (Join-Path $testDir "test_pid_autotune.c"),
        (Join-Path $testDir "test_pid_fuzzy.c"),
        (Join-Path $testDir "test_pid_fuzzy_confidence.c"),
        (Join-Path $testDir "test_sim_kiln.c"),
        (Join-Path $testDir "test_sim_plant_three_node.c"),
        (Join-Path $testDir "test_ota_auth.c"),
        (Join-Path $testDir "test_login_backoff.c"),
        (Join-Path $testDir "test_ota_image_crc.c"),
        (Join-Path $testDir "test_auth_reset_gesture.c"),
        # docs/WEB_AUTH_PLAN.md section 7/8 (LCD half only) -- the LCD's
        # own two-PIN keypad and inactivity lock. #includes lcd_auth_state.c
        # directly (same convention as test_boot_guard.c above), which in
        # turn reuses ota_auth.c's lockout primitive by embedding a fresh
        # instance, not by calling into a second parallel lockout of its own.
        (Join-Path $testDir "test_lcd_auth_state.c"),
        (Join-Path $testDir "test_security_http_core.c"),
        (Join-Path $testDir "test_web_auth.c"),
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
        (Join-Path $testDir "test_iter_tune_store.c"),
        (Join-Path $testDir "test_firing_shadow.c"),
        (Join-Path $testDir "test_safety_cfg_store.c"),
        (Join-Path $testDir "test_boot_guard.c"),
        # docs/PICO_AUTO_UPDATE_PLAN.md -- pure decision logic (header-only,
        # no I/O) and the persisted per-pair attempt-budget counter
        # (#includes pico_update_attempts.c directly, same convention as
        # test_boot_guard.c immediately above; no fake-body collision, so it
        # joins this combined executable rather than needing its own).
        (Join-Path $testDir "test_pico_auto_update_decision.c"),
        # F2 (opus review, 2026-09-20): the shared blocking/warning state
        # module pico_auto_update_boot.c's ABANDONED_BUDGET_SPENT case now
        # writes into -- see that file's own doc comment for why the boot
        # switch itself is not host-tested.
        (Join-Path $testDir "test_pico_auto_update_state.c"),
        (Join-Path $testDir "test_pico_update_attempts.c"),
        # G2's shared scanner: the build-identity record a SaftyFW image
        # carries about itself. Pure, freestanding, and compiled into BOTH
        # firmwares from firmware/CommonFW -- which is exactly why it is
        # host-tested here rather than trusted to two target builds.
        (Join-Path $testDir "test_pico_image_identity.c"),
        # Owner decision 2026-09-20: the two SaftyFW slot images embedded in
        # this build and whether they carry one agreeing identity --
        # pico_image_embedded_describe_from() is the pure, buffer-based core
        # under test (see that header's own doc comment for the split from
        # the EMBED_FILES-symbol wrapper, which is not host-testable).
        (Join-Path $testDir "test_pico_image_embedded.c"),
        (Join-Path $testDir "test_boot_button.c"),
        (Join-Path $testDir "test_backlight_pwm.c"),
        (Join-Path $testDir "test_display_power_policy.c"),
        (Join-Path $testDir "test_display_power_cfg.c"),
        (Join-Path $testDir "test_setup_wizard_progress.c"),
        (Join-Path $testDir "test_display_power_wiring.c"),
        (Join-Path $testDir "test_diagnostics_safety_tc_state.c"),
        (Join-Path $testDir "test_dashboard_protocol_version.c"),
        (Join-Path $testDir "test_estop_verification.c"),
        (Join-Path $testDir "test_dualwrite_window.c"),
        (Join-Path $testDir "test_watchdog_cfg.c"),
        (Join-Path $testDir "test_ramp_assist_cfg.c"),
        (Join-Path $testDir "test_ui_page_home_graph.c"),
        (Join-Path $testDir "test_ui_page_home_rail.c"),
        (Join-Path $testDir "test_ui_profile_list_order.c"),
        # UI_PLAN.md 6.3 -- the LCD Temperature page's safety-relay label text.
        (Join-Path $testDir "test_ui_page_temperature_safety.c"),
        (Join-Path $testDir "test_ui_page_profile_picker_format.c"),
        (Join-Path $testDir "test_max31856_codec.c"),
        (Join-Path $testDir "test_panel_codec.c"),
        (Join-Path $testDir "test_st7796_panel.c"),
        (Join-Path $testDir "test_panel_detect.c"),
        (Join-Path $testDir "test_owner_slot_pool.c"),
        (Join-Path $testDir "test_dram_margin.c"),
        (Join-Path $testDir "test_httpd_socket_budget.c"),
        (Join-Path $testDir "test_stack_margin.c"),
        (Join-Path $testDir "test_stack_margin_registry.c"),
        # docs/HTTP_POST_OWNER_MIGRATION_PLAN.md slice A1: http_async_job.c's
        # own host tests. No ESP-IDF dependency beyond the stub
        # esp_http_server.h/freertos headers already used across this
        # executable, and no static-symbol conflict with anything else
        # linked here, so it joins the main combined executable rather than
        # needing its own (unlike test_safety_cfg_http.c, which #includes
        # safety_cfg_http.c and so needs its own fakes -- see that build
        # step's own comment).
        (Join-Path $testDir "test_http_async_job.c"),
        (Join-Path $testDir "test_log_store.c"),
        (Join-Path $testDir "test_cfg_fs.c"),
        (Join-Path $testDir "test_cfg_fs_format_gate.c"),
        (Join-Path $testDir "test_cfg_fs_status.c"),
        (Join-Path $testDir "test_unit_pref.c"),
        (Join-Path $testDir "test_esp_spi_owner.c"),
        (Join-Path $testDir "test_touch_dev.c"),
        (Join-Path $testDir "test_ramp_ident.c"),
        (Join-Path $testDir "test_ramp_transient_ident.c"),
        (Join-Path $testDir "test_bx_worker_reentrancy.c"),
        (Join-Path $testDir "test_gpio_probe.c"),
        (Join-Path $testDir "test_iter_tune.c"),
        (Join-Path $testDir "test_cone_table.c"),
        (Join-Path $testDir "test_ramp_lock_onesided.c"),
        (Join-Path $testDir "test_zone_sweep_relay_off_wiring.c"),
        (Join-Path $testDir "test_approach_rate_cap.c"),
        (Join-Path $testDir "test_safety_ceiling_policy.c"),
        # docs/WEB_AUTH_PLAN.md section 5 -- the fail-closed enforcement
        # decision. Pure logic, no ESP-IDF dependency beyond the
        # httpd_method_t/httpd_uri_t types route_tier_table.h names (this
        # executable already builds against App/test/stubs/esp_http_server.h
        # for zones_http.c/backup_http.c/ota_http.c above), so it joins this
        # combined executable rather than needing its own, same convention
        # as test_ota_auth.c/ota_auth.c just above it in this file's history.
        (Join-Path $testDir "test_http_auth_enforce.c"),
        (Join-Path $testDir "test_http_session_iface.c"),
        (Join-Path $testDir "test_web_auth_safety_interaction.c"),
        # docs/WEB_AUTH_PLAN.md section 6 -- the login handler's pure
        # role-selection logic (net/web_auth_login.c below), host-tested the
        # same way test_http_auth_enforce.c/http_auth_enforce.c are: no
        # ESP-IDF dependency, joins this combined executable rather than
        # needing its own.
        (Join-Path $testDir "test_web_auth_login.c"),
        # docs/TOTP_PASSWORD_RESET_PLAN.md WT-D: totp.c/totp_config.c above
        # are both linked in for real (no ESP-IDF dependency, no fake needed)
        # -- joins this combined executable, same convention as
        # test_http_auth_enforce.c/test_web_auth_login.c just above.
        (Join-Path $testDir "test_totp.c"),
        (Join-Path $testDir "test_totp_config_persist.c"),
        # docs/TOTP_PASSWORD_RESET_PLAN.md WT-A part 2: totp_http_core.c
        # below is pure (no ESP-IDF dependency, just net/totp.h's
        # TOTP_SECRET_LEN), so it links in for real, same convention as
        # totp.c/totp_config.c just above.
        (Join-Path $testDir "test_totp_http_core.c"),
        (Join-Path $testDir "test_sim_high_temp.c"),
        (Join-Path $testDir "test_sim_mistune.c"),
        (Join-Path $testDir "test_sim_factorial_design.c"),
        (Join-Path $testDir "sim_plant.c"),
        (Join-Path $testDir "sim_high_temp.c"),
        (Join-Path $testDir "sim_mistune.c"),
        (Join-Path $testDir "sim_factorial_design.c"),
        (Join-Path $driversDir "control/pid.c"),
        (Join-Path $driversDir "control/cone_table.c"),
        (Join-Path $driversDir "control/thermal_guard.c"),
        (Join-Path $driversDir "control/on_off_trigger_decide.c"),
        (Join-Path $driversDir "control/heater_output.c"),
        (Join-Path $driversDir "control/pid_autotune.c"),
        (Join-Path $driversDir "control/pid_fuzzy.c"),
        (Join-Path $driversDir "control/pid_fuzzy_confidence.c"),
        (Join-Path $driversDir "net/ota_auth.c"),
        (Join-Path $driversDir "http/ota_image_crc.c"),
        (Join-Path $driversDir "net/auth_reset_gesture.c"),
        # 2026-09-28: lcd_auth_state.c's lockout now shares login_backoff.c's
        # ladder with web_auth_login_http.c's login lockout -- see that
        # file's link line (exe45) below, which also needs this same object.
        (Join-Path $driversDir "net/login_backoff.c"),
        (Join-Path $driversDir "ui/lcd_auth_state.c"),
        (Join-Path $driversDir "net/web_auth_session.c"),
        (Join-Path $driversDir "net/web_auth_login.c"),
        (Join-Path $driversDir "net/ota_interlock.c"),
        (Join-Path $driversDir "http/security_http_core.c"),
        (Join-Path $driversDir "http/security_backend_placeholder.c"),
        (Join-Path $driversDir "http/http_auth_enforce.c"),
        (Join-Path $driversDir "persist/ota_record.c"),
        (Join-Path $driversDir "control/heat_interlock.c"),
        (Join-Path $driversDir "control/heat_enable.c"),
        (Join-Path $driversDir "control/thermo_combine.c"),
        (Join-Path $driversDir "control/profile_feasibility.c"),
        (Join-Path $driversDir "ui/ui_page_home_graph.c"),
        (Join-Path $driversDir "ui/ui_page_home_rail.c"),
        (Join-Path $driversDir "ui/ui_profile_list_order.c"),
        (Join-Path $driversDir "ui/ui_page_temperature_safety.c"),
        (Join-Path $driversDir "ui/ui_page_profile_picker_format.c"),
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
        (Join-Path $driversDir "persist/cfg_fs.c"),
        (Join-Path $driversDir "persist/cfg_fs_format_gate.c"),
        (Join-Path $driversDir "persist/cfg_fs_status.c"),
        (Join-Path $driversDir "persist/pref_cfg_fs.c"),
        # kiln config slots filesystem move (docs/FILESYSTEM_USER_DATA_PLAN.md
        # section 5) -- kiln_cfg_store.c (#included directly by
        # test_kiln_cfg_store.c above) now calls into
        # kiln_cfg_store_cfg_fs.c's whole-document read-through/dual-write
        # bridge, so this executable needs it linked in as a plain separate
        # .c file, same convention as cfg_fs.c/pref_cfg_fs.c just above.
        (Join-Path $driversDir "persist/kiln_cfg_store_cfg_fs.c"),
        # docs/KILN_PROFILES_PLAN.md items 1/2/12 -- kiln_cfg_store.c now calls
        # kiln_package_capture_pico_half()/_compute_hash() to package the
        # Pico's commissioning params alongside the ESP blob. Pure logic, no
        # store/link dependency of its own (it takes its Pico-table accessor
        # as an injected function-pointer pair, kiln_pkg_pico_source_t --
        # see kiln_package.h's own header comment), so it is linked in for
        # real here rather than faked -- same "no reason to fake pure logic"
        # convention safety_cfg_http.c's s8_rate_guard_estimate.c linkage
        # documents in this same file, further down.
        (Join-Path $driversDir "persist/kiln_package.c"),
        # docs/KILN_PROFILES_PLAN.md section 5.3 rows 2/3 (2026-09-16) --
        # kiln_cfg_store.c now calls kiln_board_identity_get() to mint/compare
        # a board id for cross-board CT-calibration invalidation on import.
        # Linked for real, same "pure logic, no reason to fake" convention as
        # kiln_package.c immediately above; test/stubs/esp_mac.h supplies the
        # host-side esp_efuse_mac_get_default() it calls.
        (Join-Path $driversDir "persist/kiln_board_identity.c"),
        # docs/WEB_AUTH_PLAN.md item 12b: linked in for REAL (public header,
        # no static internals test_backup_import.c/test_kiln_cfg_store.c need
        # to reach) so this executable can seed a real credential and prove
        # it survives a whole-board export and a per-slot config-package
        # export/import (kiln_cfg_store_export_package_json(), #included
        # above via test_kiln_cfg_store.c's #include of kiln_cfg_store.c).
        # fake_kv.c/hal_status.c/hal_esp_common.c above already cover its
        # link needs; its psa/crypto.h host stub needs exactly one
        # g_stub_psa_import_key_result definition, added in test_backup_import.c.
        (Join-Path $driversDir "persist/web_auth_store.c"),
        # docs/TOTP_PASSWORD_RESET_PLAN.md WT-A: totp.c is pure (no ESP-IDF/
        # mbedtls/PSA dependency -- see its own header comment for why it
        # hand-rolls SHA-1/HMAC rather than going through the PSA stub that
        # web_auth_store.c above needs), so it links in for real with no
        # extra plumbing. totp_config.c needs only the hal_kv backend
        # (fake_kv.c/hal_status.c, already linked above for web_auth_store.c).
        (Join-Path $driversDir "net/totp.c"),
        (Join-Path $driversDir "persist/totp_config.c"),
        # WT-A part 2: the pure token-lifecycle/pending-secret state machine
        # behind auth_totp_http.c/security_http.c's TOTP routes -- no
        # ESP-IDF dependency, see totp_http_core.h's own header comment.
        (Join-Path $driversDir "http/totp_http_core.c"),
        # Opus review of 5dd23944 finding B (2026-09-20): backup_import.c
        # (#included via test_backup_import.c above) now calls
        # live_edit_name_collides() directly, in its new pass-1 dup-name
        # pre-check, so this executable needs live_profile.c linked in for
        # REAL -- same "small, pure, already host-tested elsewhere" reasoning
        # exe7's identical link documents. Its only other dependency,
        # profiles_builtin_id_valid()/entry(), is faked directly in
        # test_backup_import.c (same convention as test_profiles_http.c's own
        # g_fake_builtin), since this executable has no other reason to link
        # profiles_builtin.c's real read-only catalogue.
        (Join-Path $driversDir "persist/live_profile.c"),
        # kiln_http_register() (http_auth_http.c) rewiring pass: backup_http.c
        # (#included via test_backup_import.c, joined into $sources per the
        # comment further down) now calls it instead of
        # httpd_register_uri_handler() directly. http_auth_enforce.c/
        # http_auth_policy_iface.c/http_session_iface.c linked in for real,
        # same "already host-tested elsewhere, needs its real symbols to
        # link" reasoning as web_auth_store.c immediately above.
        (Join-Path $driversDir "http/http_auth_http.c"),
        (Join-Path $driversDir "http/http_auth_enforce.c"),
        (Join-Path $driversDir "http/http_auth_policy_iface.c"),
        (Join-Path $driversDir "http/http_session_iface.c"),
        # NOTE: no wifi_prov_unprovisioned_stub.c here -- this executable's
        # own $sources list already includes test_wifi_prov.c, which
        # #includes the REAL wifi_prov.c and so already defines
        # wifi_prov_is_unprovisioned(); linking the stub too would collide
        # (LNK2005).
        # http_auth_http.c's resolve_role_for_request() calls
        # httpd_req_get_hdr_value_len/_str and ota_http_get_client_ip, real
        # esp_http_server.h symbols this executable has no other source for
        # (only test_ota_http.c's own #include of ota_http.c supplies them,
        # and that's a separate executable) -- link-only fakes, see the file.
        (Join-Path $testDir "stubs/http_auth_link_stub.c"),
        (Join-Path $driversDir "hw/touch_dev.c"),
        (Join-Path $driversDir "control/ramp_ident.c"),
        (Join-Path $driversDir "control/ramp_transient_ident.c"),
        (Join-Path $driversDir "control/iter_tune.c"),
        (Join-Path $driversDir "control/firing_score.c"),
        (Join-Path $driversDir "control/firing_compare.c"),
        (Join-Path $driversDir "safety/safety_ceiling_policy.c"),
        # system_mode_gate wiring (docs/SYSTEM_MODE_GATE_PLAN.md,
        # gate-slices-2/4/5, 2026-09-25): backup_import.c (#included via
        # test_backup_import.c above) now calls system_mode_gate_check()/
        # system_mode_gate_http_send_refusal() -- link both real, pure,
        # no-ESP-IDF-dependency objects in; relay_authority_heat_run_active()
        # itself is faked in test_backup_import.c, same convention as its
        # other profile_executor/autotune_engine fakes.
        (Join-Path $driversDir "safety/system_mode_gate.c"),
        (Join-Path $driversDir "http/system_mode_gate_http.c")
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
        # C4013 "undefined; assuming extern returning int" -- the MSVC spelling
        # of -Werror=implicit-function-declaration. Promoted to an ERROR
        # 2026-09-16: a missing prototype on the Pico ceiling write path passed
        # a float under default argument promotion and wrote the ceiling as 0.
        "/we4013"
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

    # /experimental:c11atomics: live_profile.c (linked in below for
    # test_backup_import.c's Opus-review pass-1 dup-name pre-check, 2026-09-20)
    # uses <stdatomic.h> -- same MSVC requirement exe7/exeLp/exePlh already
    # need for the same reason (see this file's comments on those).
    $cmd = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 /experimental:c11atomics /Fo:`"$outDir\\`" /Fe:`"$exe`" $sourceArgs"

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
    # test_zones_config_cfg_fs.c (docs/FILESYSTEM_USER_DATA_PLAN.md section 5
    # step 5, zones-config-move task): a separate TU in this same executable
    # exercising zones_config_cfg_fs.c's read-through/dual-write policy
    # through the REAL nvs_load()/nvs_save() (defined in
    # zones_config_store.c, #included by test_zones_http.c above) plus a
    # real cfg_fs.c against a temp directory -- see that test file's own
    # header comment. cfg_fs.c/zones_config_cfg_fs.c link in here as plain
    # separate .c files (like zones_config_json.c below), not textually
    # included -- neither defines anything test_zones_http.c's #includes
    # already define, so there is no multiple-definition risk.
    $cmd2 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$exe2ObjDir\`" /Fe:`"$exe2`" `"$(Join-Path $testDir 'test_zones_http.c')`" " +
            "`"$(Join-Path $testDir 'test_zones_config_cfg_fs.c')`" " +
            "`"$(Join-Path $testDir 'test_relay_names_cfg_fs.c')`" " +
            # 2026-09-24: zones_current_sweep_engine.c (#included via
            # test_zones_http.c) now reads through the shared thermo_channels_
            # read() helper (thermo_channel_read.c) instead of duplicating the
            # MAX31856 fault-bit filter inline -- link the real object in, same
            # convention as exe4/test_profile_executor_prestart.c's own link of
            # this file for the same helper.
            "`"$(Join-Path $driversDir 'control/thermo_channel_read.c')`" " +
            # 2026-09-24 review of 81d71452: zones_current_sweep_start()
            # (zones_current_sweep_task.c, #included via test_zones_http.c)
            # now calls stack_margin_register() for the zone_sweep task --
            # link the real registry, same as the other executables below
            # that link stack_margin.c for their own registration calls.
            "`"$(Join-Path $driversDir 'common/stack_margin.c')`" " +
            # config_convert.py's zones_blob golden (see that file's header):
            # real nvs_save() bytes of a sentinel-filled zones_cfg_t, compared
            # against tools/PcTools/tests/fixtures/config_convert/.
            "`"$(Join-Path $testDir 'test_zones_blob_golden.c')`" " +
            "`"$(Join-Path $driversDir 'persist/zones_config_json.c')`" " +
            "`"$(Join-Path $driversDir 'persist/zones_config_convert.c')`" " +
            "`"$(Join-Path $driversDir 'persist/zones_config_migrate.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs.c')`" `"$(Join-Path $driversDir 'persist/zones_config_cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/pref_cfg_fs.c')`" " +
            # docs/CT_ATTRIBUTION_VERIFICATION_PLAN.md: the CT attribution
            # verdict store. Linked as a plain .c (not #included) because it
            # depends only on hal_kv -- fake_kv.c below supplies that -- and
            # test_zones_http.c's fingerprint/round-trip tests drive it
            # through its public header like any other linked module.
            "`"$(Join-Path $driversDir 'persist/ct_verify_store.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`" " +
            # Owner request 2026-09-10: zones_http_post.c (#included above via
            # test_zones_http.c) now calls safety_ceiling_sync_guard_raise()/
            # _apply_lower(), and zones_http.c (also #included above, for
            # zones_json_escape()) calls safety_ceiling_policy_target_c()/
            # safety_ceiling_sync_get_current_pico_ceiling() for the GET
            # response's new safety_ceiling field -- link the real pure-logic
            # + ESP-glue objects in, same convention as every other plain .c
            # this executable already links alongside its #included sources.
            "`"$(Join-Path $driversDir 'safety/safety_ceiling_policy.c')`" " +
            "`"$(Join-Path $driversDir 'safety/safety_ceiling_sync.c')`" " +
            # 2026-09-14 owner decision: safety_ceiling_sync.c now calls
            # config_divergence_check() (the reusable format-version+hash
            # config identity comparator) for its alarm-and-disable-heat
            # enforcement -- link the real object in, same convention as
            # safety_ceiling_policy.c just above.
            "`"$(Join-Path $driversDir 'safety/config_divergence.c')`" " +
            # 2026-09-10 opus review finding A: safety_ceiling_sync.c now calls
            # hal_time_now_us() (its own reconcile backoff timer, replacing a
            # direct esp_timer_get_time() call the HAL include-boundary check
            # refuses) -- fake_time.c supplies it here, same convention as
            # every other hal_time_now_us() caller linked into this suite.
            "`"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            # kiln_http_register() rewiring: zones_http.c (#included via
            # test_zones_http.c) now calls it instead of
            # httpd_register_uri_handler() directly.
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            # http_auth_policy_iface.c/http_session_iface.c call into
            # web_auth_store.c (policy load) and web_auth_session.c (real
            # session table/role resolution) -- linked in for REAL, same
            # "already host-tested elsewhere, needs its real symbols to link"
            # reasoning used everywhere else these four files travel together.
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            "`"$(Join-Path $testDir 'stubs/http_auth_link_stub.c')`" " +
            # system_mode_gate wiring (docs/SYSTEM_MODE_GATE_PLAN.md,
            # gate-slices-2/4/5, 2026-09-25): zones_http_post.c (#included
            # above) now calls system_mode_gate_check()/
            # system_mode_gate_http_send_refusal() -- link both real, pure,
            # no-ESP-IDF-dependency objects in, same convention as exe50's
            # own dedicated executable for the module's unit tests.
            "`"$(Join-Path $driversDir 'safety/system_mode_gate.c')`" " +
            "`"$(Join-Path $driversDir 'http/system_mode_gate_http.c')`" " +
            # docs/HTTP_POST_OWNER_MIGRATION_PLAN.md A2 review fix (Opus,
            # 2026-09-25): zones_post_handler() (zones_http_post.c, #included
            # above) now calls http_async_job_busy() -- link the real object
            # in, same convention as test_safety_cfg_http.c's own link of it.
            # stack_margin.c is already linked above for zone_sweep's own
            # registration call, so http_async_job.c's stack_margin_
            # register() call needs no additional link.
            "`"$(Join-Path $driversDir 'http/http_async_job.c')`""
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
    # S8 rate-guard auto-calc write path (docs/audits/s8_auto_calc_design_
    # 2026-09-09.md "Part 3") pulled in the REAL s8_rate_guard_estimate.c/
    # s8_rate_guard_auto_decide() as a second source here -- pure, host-
    # testable logic with no store/link dependency of its own, so there is
    # no reason to fake it the way safety_cfg_store_*/zones_config_* are
    # faked below (those genuinely need real hardware/NVS on target).
    # zones_config_get_model()/_get_thermo_count()/_get_model_fit_context()
    # ARE faked in test_safety_cfg_http.c itself, same "own stub, real
    # accessor links into a different executable" reasoning as the CT
    # calibration fakes already there.
    # 2026-09-10 (opus review round 2, defect B fix): safety_cfg_http.c's
    # rate_guard_gather_and_estimate() now calls the REAL
    # zone_coupling_matrix_provenance_ok()/zone_coupling_use_measured_diag_
    # k_dc() (zone_coupling_solve.c) instead of reimplementing the rule by
    # hand, so that source must link into this executable too -- pure,
    # host-testable logic with no store/link dependency of its own, same
    # reasoning as s8_rate_guard_estimate.c just above.
    $exe3 = Join-Path $outDir "kilnctl_host_tests_safety_cfg_http.exe"
    $cmd3 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exe3`" `"$(Join-Path $testDir 'test_safety_cfg_http.c')`" " +
            "`"$(Join-Path $driversDir 'control/s8_rate_guard_estimate.c')`" " +
            "`"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`" " +
            # 2026-09-15 review (review_divergence_fixes_b2e7017f_2026-09-15.md):
            # safety_cfg_http.c (#included above) now calls hal_time_now_us()
            # (armed-refusal-recency tracking) -- fake_time.c supplies it here,
            # same convention as safety_ceiling_sync.c's own callers elsewhere
            # in this script.
            "`"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            # kiln_http_register() rewiring: safety_cfg_http.c (#included
            # above) now calls it instead of httpd_register_uri_handler()
            # directly.
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            # http_auth_policy_iface.c/http_session_iface.c call into
            # web_auth_store.c (policy load) and web_auth_session.c (real
            # session table/role resolution) -- linked in for REAL, same
            # "already host-tested elsewhere, needs its real symbols to link"
            # reasoning used everywhere else these four files travel together.
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            # web_auth_store.c needs the host hal_kv backend (fake_kv.c) --
            # not otherwise linked into this executable (its own
            # safety_cfg_store fake is separate and does not touch hal_kv).
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" " +
            # docs/HTTP_POST_OWNER_MIGRATION_PLAN.md slice A1: safety_cfg_
            # http.c (#included above) now calls http_async_job_try_start()
            # (ct_auto_zero_post_handler()'s handoff), so http_async_job.c
            # must link for real here too -- it in turn calls
            # stack_margin_register(), not otherwise linked into this
            # executable.
            "`"$(Join-Path $driversDir 'http/http_async_job.c')`" " +
            "`"$(Join-Path $driversDir 'common/stack_margin.c')`" " +
            "`"$(Join-Path $testDir 'stubs/http_auth_link_stub.c')`""

    Invoke-HostTestExe -Name "safety_cfg_http" -ExePath $exe3 -BuildCmd $cmd3

    # ---- test_kiln_cfg_swap.c: docs/KILN_PROFILES_PLAN.md item 5, the
    # two-processor apply transaction. Its own executable: it #includes
    # kiln_cfg_swap.c directly and pre-defines every header guard that file
    # transitively reaches (kiln_cfg_store.h/kiln_package.h/safety_link.h/
    # safety_cfg_store.h/safety_cfg_http.h/safety_ceiling_sync.h/ota_state.h/
    # zones_config_accessors.h) so it can supply its own small, fully
    # controllable fake bodies for all of them, rather than linking the real
    # (hardware-owning) implementations -- see that test file's own top
    # comment. hal_kv is the one dependency NOT faked: it links against the
    # real fake_kv.h/.c Phase 2 host backend (already on $hostTestsRsp's
    # include path via hwAbsDir\host) so the pending-swap record's actual
    # persistence/CRC/corruption behavior (fake_kv_script_corrupt_key(), H10)
    # is exercised for real, not mocked a second time.
    $exe33 = Join-Path $outDir "kilnctl_host_tests_kiln_cfg_swap.exe"
    $cmd33 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
             "/Fo:`"$outDir\\`" /Fe:`"$exe33`" `"$(Join-Path $testDir 'test_kiln_cfg_swap.c')`" " +
             "`"$(Join-Path $hwAbsDir 'host\fake_kv.c')`" " +
             "`"$(Join-Path $driversDir 'persist\web_auth_store.c')`" " +
             "`"$(Join-Path $hwAbsDir 'common\hal_status.c')`" `"$(Join-Path $hwAbsDir 'esp\common\hal_esp_common.c')`""
    # web_auth_store.c added for docs/WEB_AUTH_PLAN.md item 12b: linked in for
    # REAL (public header, no static internals this file needs) so this
    # executable can prove a real credential survives a slot swap
    # (kiln_cfg_swap_apply()) regardless of the swap's own outcome. Needs
    # hal_status.c/hal_esp_common.c too (not otherwise linked here) for
    # web_auth_store.c's HAL_OK/hal_status plumbing.

    Invoke-HostTestExe -Name "kiln_cfg_swap" -ExePath $exe33 -BuildCmd $cmd33

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
            "`"$(Join-Path $driversDir 'control/thermo_channel_read.c')`" " +
            "`"$(Join-Path $driversDir 'control/heat_enable.c')`" `"$(Join-Path $driversDir 'control/pid_fuzzy.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid_fuzzy_confidence.c')`" " +
            "`"$(Join-Path $driversDir 'common/stack_margin.c')`" `"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`" " +
            "`"$(Join-Path $driversDir 'control/adaptive_tune.c')`" `"$(Join-Path $driversDir 'control/adaptive_tune_model.c')`" " +
            "`"$(Join-Path $driversDir 'control/adaptive_tune_ki.c')`" `"$(Join-Path $driversDir 'control/pid_autotune.c')`" " +
            "`"$(Join-Path $driversDir 'control/cone_table.c')`" `"$(Join-Path $driversDir 'control/on_off_trigger_decide.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs.c')`" `"$(Join-Path $driversDir 'persist/pref_cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/firing_stats_cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs_status.c')`" " +
            "`"$(Join-Path $driversDir 'persist/flash_worker_wait.c')`" " +
            # docs/LIVE_PROFILE_EDIT_PLAN.md pass 1: profile_executor.c (#included
            # above) now calls profile_executor_live_pickup_check() from
            # reload_live_profile_if_changed(). Linked for REAL rather than
            # faked -- it is a small pure file (no hal_kv/FreeRTOS deps) and
            # this executable already fakes live_profile_generation()/
            # live_profile_load_working()/profiles_validate_candidate()
            # themselves (test_profile_executor_prestart.c's own fakes),
            # which is enough for this pickup function to compile and link
            # against without pulling in live_profile.c's hal_kv-backed half --
            # live_edit_check_window() (the one real dependency
            # profile_executor_live_pickup.c has) is faked directly inside
            # test_profile_executor_prestart.c instead, alongside its other
            # fakes of this same module's surface (live_profile.c itself is
            # tested for real by test_live_profile.c, its own executable).
            "`"$(Join-Path $driversDir 'control/profile_executor_live_pickup.c')`" " +
            # ITER_TUNE_REDESIGN_PLAN.md step 8: profile_executor_firing_stats.c
            # (#included above via profile_executor.c's multi-#include block)
            # now calls firing_shadow_zone_tick()/firing_shadow_finish_firing() --
            # link the real module (already host-tested by test_firing_shadow.c,
            # its own executable) in for real, same reasoning as firing_score.c/
            # firing_compare.c would need if this executable ever called them.
            "`"$(Join-Path $driversDir 'control/firing_shadow.c')`" `"$(Join-Path $driversDir 'control/firing_score.c')`" " +
            "`"$(Join-Path $driversDir 'control/firing_compare.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`" " +
            # Slice 2 (docs/SYSTEM_MODE_GATE_PLAN.md section 3.6, 2026-09-27):
            # profile_executor_run.c now calls system_mode_gate_check() ahead
            # of readiness_gate_evaluate() -- link the real, pure module in
            # (already host-tested for real by test_system_mode_gate.c, its
            # own executable) rather than faking it.
            "`"$(Join-Path $driversDir 'safety/system_mode_gate.c')`""
    # 2026-09-08: flash_worker_wait.c linked in -- adaptive_tune.c (linked
    # for real here too) now calls flash_worker_wait_default() before its
    # kibase migrate-on-load write; same fix as exe17/exe19 above.
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
            "`"$(Join-Path $driversDir 'control/thermo_channel_read.c')`" " +
            "`"$(Join-Path $driversDir 'control/heat_enable.c')`" `"$(Join-Path $driversDir 'common/stack_margin.c')`" " +
            # Slice 2 (docs/SYSTEM_MODE_GATE_PLAN.md section 3.6, 2026-09-27):
            # autotune_engine.c now calls system_mode_gate_check() ahead of
            # readiness_gate_evaluate() -- link the real, pure module in, same
            # reasoning as test_profile_executor_prestart.c's cmd4 above.
            "`"$(Join-Path $driversDir 'safety/system_mode_gate.c')`""
    # stack_margin.c added DRAM_PSRAM_STATUS.md Phase 0 (4.2): autotune_engine.c's
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
    # docs/FILESYSTEM_USER_DATA_PLAN.md section 5 step 4 (user-profiles
    # filesystem move): profiles_http.c (#included directly above) now calls
    # into profiles_cfg_fs.c's per-slot read-through/dual-write bridge, so
    # this executable needs cfg_fs.c/profiles_cfg_fs.c linked in as plain
    # separate .c files -- same convention as cmd2's cfg_fs.c/
    # zones_config_cfg_fs.c. The new dual-write/divergence/migration tests
    # for this live inside test_profiles_http.c itself (new test functions,
    # same file) rather than a separate TU, because nvs_load_all_from() is
    # `static` -- exactly the reason this whole executable already exists as
    # its own binary (see this section's header comment).
    $exe7 = Join-Path $outDir "kilnctl_host_tests_profiles_http.exe"
    $phObjDir = Join-Path $outDir "profiles_http_obj"
    New-Item -ItemType Directory -Force -Path $phObjDir | Out-Null
    # /experimental:c11atomics: live_profile.c (linked in below) uses
    # <stdatomic.h> -- same MSVC requirement test_live_profile.c/
    # test_profile_executor_live_pickup.c already needed for the same reason
    # (see this file's own comment above them).
    $cmd7 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 /experimental:c11atomics " +
            "/Fo:`"$phObjDir\\`" /Fe:`"$exe7`" `"$(Join-Path $testDir 'test_profiles_http.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs.c')`" `"$(Join-Path $driversDir 'persist/profiles_cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs_status.c')`" " +
            # Owner request 2026-09-19 (dup-name save refusal): profiles_http.c
            # (#included above) now calls live_edit_name_collides(), which
            # lives in live_profile.c. Linked in for REAL rather than faked --
            # it is a small, pure, already host-tested function (its own
            # executable below) over profiles_builtin_id_valid()/entry(),
            # which this executable already fakes (test_profiles_http.c's own
            # g_fake_builtin, same as every other symbol from profiles_builtin.h
            # this file resolves). live_profile.c's OTHER entry points
            # (live_edit_decide()/live_profile_load_record() etc, which pull
            # in profile_encode_current_blob()/hal_kv persistence) are never
            # reached from this executable's tests, so nothing else needs
            # faking here.
            "`"$(Join-Path $driversDir 'persist/live_profile.c')`" " +
            # docs/PROFILE_SLOTS_100_PLAN.md section 7 task 6's bench-slot
            # exclusion test needs the REAL ui_page_profile_picker_is_deletable()
            # (not a hand-rolled stand-in) -- this file is small and pure
            # (no LVGL/ESP-IDF beyond the type-only stub headers already on
            # this executable's include path), same reasoning as
            # profiles_favorites.c above.
            "`"$(Join-Path $driversDir 'ui/ui_page_profile_picker_format.c')`" " +
            # profiles_edit_http.c/profiles_catalog_http.c (both reached from
            # profiles_http.c, #included above) now call profiles_favorites_*(),
            # so this executable needs that module linked in as a plain
            # separate .c file -- same convention as the cfg_fs.c/
            # profiles_cfg_fs.c entries on the line above. Linked for REAL
            # rather than faked: it is a small pure-mask module over hal_kv,
            # the host hal_kv backend (fake_kv.c) is already linked below, and
            # a fake would not exercise the delete-clears-the-favorite path
            # that profile_delete_post_handler now depends on. It resolves
            # profiles_builtin_id_valid() against this test file's own fake
            # (test_profiles_http.c:444), the same fake the rest of the
            # executable already uses.
            "`"$(Join-Path $driversDir 'persist/profiles_favorites.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`" " +
            # kiln_http_register() rewiring: profiles_http.c (#included
            # above) now calls it instead of httpd_register_uri_handler()
            # directly.
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            # http_auth_policy_iface.c/http_session_iface.c call into
            # web_auth_store.c (policy load) and web_auth_session.c (real
            # session table/role resolution) -- linked in for REAL, same
            # "already host-tested elsewhere, needs its real symbols to link"
            # reasoning used everywhere else these four files travel together.
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            # http_session_iface.c needs hal_time_now_ms() -- not otherwise
            # linked into this executable.
            "`"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $testDir 'stubs/http_auth_link_stub.c')`""
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

    # ---- test_live_profile.c: its own separate executable ---------------------
    # docs/LIVE_PROFILE_EDIT_PLAN.md pass 1. Same "#includes the .c directly"
    # reason as test_zones_http.c/test_profiles_http.c above -- it reaches
    # live_profile.c's persistence functions and profile_executor_live_pickup.c's
    # pure pickup check by #including both files directly, and needs the REAL
    # host hal_kv backend (fake_kv.c) so live_profile.c's read-back-verified
    # writes are exercised for real. Fakes profile_encode_current_blob()/
    # profile_decode_blob() locally (a tiny memcpy stand-in, not the real wire
    # format -- that format's own correctness is test_profiles_http.c's job)
    # since profiles_http.c (the real implementation) is not linked into this
    # executable; live_profile.c itself forward-declares the shared
    # profile_decode_result_t/PROFILE_DECODE_OK type behind a
    # PROFILE_DECODE_RESULT_SHIM_DECLARED guard rather than pulling in
    # profiles_http_internal.h (esp_http_server.h, not host-safe -- same
    # reasoning as profile_executor.c's own local forward declaration of
    # profiles_validate_candidate()), and this test file pre-declares an
    # identical shim ahead of live_profile.c's #include so both agree on the
    # type without a duplicate enum-tag definition error.
    $exeLp = Join-Path $outDir "kilnctl_host_tests_live_profile.exe"
    $lpObjDir = Join-Path $outDir "live_profile_obj"
    New-Item -ItemType Directory -Force -Path $lpObjDir | Out-Null
    # LOW (review): live_profile.c now uses <stdatomic.h> for
    # s_live_profile_generation (same idiom as wifi_provision_http.c's
    # s_httpd_open_sockets, which is never compiled on the host -- this is
    # the FIRST host test to pull stdatomic.h in). MSVC's own
    # vcruntime_c11_stdatomic.h refuses outright ("C atomic support is not
    # enabled") under plain /std:c11 -- /experimental:c11atomics is the
    # documented MSVC switch that turns it on.
    $cmdLp = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 /experimental:c11atomics " +
            "/Fo:`"$lpObjDir\\`" /Fe:`"$exeLp`" `"$(Join-Path $testDir 'test_live_profile.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "live_profile" -ExePath $exeLp -BuildCmd $cmdLp

    # ---- test_profiles_live_http.c: its own separate executable ---------------
    # docs/LIVE_PROFILE_EDIT_PLAN.md pass 2 (section 10/11 HTTP surface). Same
    # "#includes the .c directly" reason as test_live_profile.c above -- it
    # reaches profiles_live_http.c's `static` handlers directly, and links the
    # REAL live_profile.c (in turn needing the real host hal_kv backend,
    # fake_kv.c) so the fork/save/decide storage paths are exercised for real;
    # the httpd-tier neighbours (profiles_http.c/profiles_edit_http.c) are not
    # linked here and are faked locally instead (real coverage for those
    # bodies is test_profiles_http.c's job). Needs the same
    # /experimental:c11atomics switch as test_live_profile.c since it also
    # pulls in live_profile.c's <stdatomic.h> use.
    $exePlh = Join-Path $outDir "kilnctl_host_tests_profiles_live_http.exe"
    $plhObjDir = Join-Path $outDir "profiles_live_http_obj"
    New-Item -ItemType Directory -Force -Path $plhObjDir | Out-Null
    $cmdPlh = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 /experimental:c11atomics " +
            "/Fo:`"$plhObjDir\\`" /Fe:`"$exePlh`" `"$(Join-Path $testDir 'test_profiles_live_http.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "profiles_live_http" -ExePath $exePlh -BuildCmd $cmdPlh

    # ---- test_ui_edit_firing_apply.c: its own separate executable ------------
    # The LCD "Edit firing" page's LVGL-free half (ui_edit_firing_apply.c):
    # step clamps, relay/IO read-only, and the Apply sequence (range ->
    # validate HARD -> window -> fork -> save) plus its stale-copy guards.
    # Same shape as test_profiles_live_http.c above: #includes the REAL
    # live_profile.c and ui_edit_firing_apply.c, links fake_kv.c, fakes the
    # httpd-tier neighbours; same /experimental:c11atomics need.
    $exeEfa = Join-Path $outDir "kilnctl_host_tests_ui_edit_firing_apply.exe"
    $efaObjDir = Join-Path $outDir "ui_edit_firing_apply_obj"
    New-Item -ItemType Directory -Force -Path $efaObjDir | Out-Null
    $cmdEfa = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 /experimental:c11atomics " +
            "/Fo:`"$efaObjDir\\`" /Fe:`"$exeEfa`" `"$(Join-Path $testDir 'test_ui_edit_firing_apply.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "ui_edit_firing_apply" -ExePath $exeEfa -BuildCmd $cmdEfa

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
            # PICO_AUTO_UPDATE_PLAN.md G1: ota_http_pico.c (#included into
            # test_ota_http.c above) now records what it staged, so a later
            # boot can re-use the image. Linked in for REAL rather than faked,
            # same rationale as ota_record.c beside it -- it is a plain
            # hal_kv_* record store and fake_kv.c below already supplies its
            # backing store.
            "`"$(Join-Path $driversDir 'persist/pico_image_manifest.c')`" " +
            "`"$(Join-Path $driversDir 'http/ota_image_crc.c')`" " +
            # Owner decision 2026-09-20: ota_http_pico.c's ota_pico_do_stage()
            # now calls pico_img_stage_begin/write_chunk/finish() (shared with
            # net/pico_auto_update_boot.c's embedded-image writer) instead of
            # its own inline erase/write/manifest logic -- linked in for real,
            # same rationale as pico_image_manifest.c/ota_image_crc.c beside it.
            "`"$(Join-Path $driversDir 'net/pico_img_stage.c')`" " +
            # KilnFW TODO.md 9.4: ota_pico_do_stage() now re-scans the image it
            # just staged via pico_image_source_describe(), to learn the image's
            # OWN declared link protocol version. Linked in for real, same
            # rationale as pico_img_stage.c/ota_image_crc.c above: it is a pure
            # scanner over the fake pico_img partition test_ota_http.c already
            # supplies, and a stub would hide the real symbol this executable
            # has to keep linkable.
            "`"$(Join-Path $driversDir 'net/pico_image_source.c')`" " +
            "`"$(Join-Path $testDir '..\..\..\CommonFW\src\saftyfw_image_identity.c')`" " +
            "`"$(Join-Path $driversDir 'common/stack_margin.c')`" " +
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" " +
            "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`" " +
            # system_mode_gate wiring (docs/SYSTEM_MODE_GATE_PLAN.md,
            # gate-slices-2/4/5, 2026-09-25): factory_reset.c (#included
            # above) now calls system_mode_gate_check()/
            # system_mode_gate_http_send_refusal() -- link both real, pure,
            # no-ESP-IDF-dependency objects in; relay_authority_heat_run_active()
            # itself is faked in test_ota_http.c, same convention as its other
            # profile_executor/autotune_engine fakes.
            "`"$(Join-Path $driversDir 'safety/system_mode_gate.c')`" " +
            "`"$(Join-Path $driversDir 'http/system_mode_gate_http.c')`""
    # WEB_AUTH_PLAN.md section 2b: ota_http.c now calls
    # http_auth_policy_web_enabled() (http_auth_policy_iface.c) directly, and
    # already used kiln_http_register() (http_auth_http.c) from the earlier
    # route-rewiring pass; http_auth_http.c in turn needs
    # http_auth_enforce.c's pure decision function and
    # http_session_iface.c's session resolver. All four linked in for REAL,
    # same rationale as web_auth_store.c above: they are already host-tested
    # elsewhere (test_http_auth_enforce.c) and this executable needs their
    # real symbols to link, not a stand-in.
    # hal_time migration (HW_ABSTRACTION.md item 5): ota_http_pico.c, one of
    # the files #included directly into test_ota_http.c above, now calls
    # hal_time_now_us() instead of esp_timer_get_time(); fake_time.c supplies it.
    # stack_margin.c added DRAM_PSRAM_STATUS.md Phase 0 (4.2): ota_http.c's
    # recovery-exit/rollback/pico-rollback reboot task starts now call
    # stack_margin_register() (registration only, no size change), and this
    # executable #includes ota_http.c directly.
    # fake_kv.c added HW_ABSTRACTION.md Phase 3 item 3 (nvs.h -> hal_kv.h
    # migration): ota_record.c, linked in for real above, now calls hal_kv_*()
    # instead of nvs_*() directly. hal_esp_common.c added in the same pass's
    # flash-safety review follow-up: ota_record.c now also calls
    # hal_status_to_esp_err() to preserve its ESP_FAIL/mapped-error return
    # contract instead of collapsing every failure to plain ESP_FAIL.
    # web_auth_store.c added for docs/WEB_AUTH_PLAN.md item 12b: linked in for
    # REAL (not #included -- it has a public header and no static internals
    # this file needs to reach) so test_ota_http.c can prove a real credential
    # survives factory_reset_execute() across all four scopes. Its
    # psa/crypto.h host stub needs exactly one g_stub_psa_import_key_result
    # definition per executable; ota_http.c's own HMAC use already supplies
    # one in this executable (test_ota_http.c), so no new definition is added.

    Invoke-HostTestExe -Name "ota_http" -ExePath $exe8 -BuildCmd $cmd8

    # ---- test_dashboard_status_http.c: its own NINTH, separate executable ----
    # 2026-09-17 audit finding 7 follow-up: GET /api/status (ROUTE_TIER_OPEN,
    # no credentials) leaked the same build-identity fields (fw_build,
    # safety_build_commit/_datetime/_dirty) that 1a41a972 already redacted on
    # GET /api/ota/esp/status. dashboard_status_get_handler() is `static` with
    # no other seam into it, so this file #includes dashboard_status_http.c
    # directly -- same convention as test_ota_http.c's own #include of
    # ota_http.c above -- and needs its own executable for the same reason
    # (file-scope TAG/static collisions with every other test executable).
    # http_auth_http.c/http_auth_enforce.c/http_auth_policy_iface.c/
    # http_session_iface.c/web_auth_store.c/web_auth_session.c linked in for
    # REAL so the tests exercise the actual production gate
    # (http_auth_policy_web_enabled() && http_auth_caller_is_admin()), not a
    # transcribed stand-in for it -- same rationale as exe8 above.
    $exe9 = Join-Path $outDir "kilnctl_host_tests_dashboard_status_http.exe"
    $dashStatusObjDir = Join-Path $outDir "dashboard_status"
    New-Item -ItemType Directory -Force -Path $dashStatusObjDir | Out-Null
    # /I stubs_dashboard_status precedes @hostTestsRsp's own /I list so this
    # executable's private lvgl_port.h shim (see that file's header comment)
    # wins over the real drivers/ui/lvgl_port.h without touching the shared
    # response file or any other executable's include resolution.
    $dashStatusStubDir = Join-Path $testDir "stubs_dashboard_status"
    $cmd9 = "call `"$vcvars`" x64 >nul && cl /I`"$dashStatusStubDir`" @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$dashStatusObjDir\\`" /Fe:`"$exe9`" " +
            "`"$(Join-Path $testDir 'test_dashboard_status_http.c')`" " +
            "`"$(Join-Path $driversDir 'http/dashboard_json.c')`" " +
            # touch_dev.c linked for REAL (not stubbed): /api/status's
            # touch_cal_supported value is produced by the production
            # touch_cal_support_name(), so the exact strings on the wire --
            # which diagnostics_page.html compares against literally -- are
            # asserted against the real function rather than a copy of it.
            "`"$(Join-Path $driversDir 'hw/touch_dev.c')`" " +
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" " +
            "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "dashboard_status_http" -ExePath $exe9 -BuildCmd $cmd9

    # ---- test_ota_pico_relay.c: its own, separate executable -----------------
    # docs/PICO_AUTO_UPDATE_PLAN.md used to flag this gap: "ota_pico_relay.c's
    # own state machine has no host test today (only its terminal-state
    # recognition is exercised indirectly)." relay_task_fn() is `static` with
    # no other seam into it, so this file #includes ota_pico_relay.c directly
    # -- same convention as test_ota_http.c/test_dashboard_status_http.c above
    # -- and needs its own executable for the same file-scope TAG/static
    # collision reason. It also needs its own PRIVATE freertos/task.h (see
    # stubs_ota_pico_relay/freertos/task.h's own header comment): the shared
    # stub hardcodes xTaskGetTickCount() to 0, which would make
    # relay_wait_for_states()'s timeout branch unreachable and hang this
    # executable on any unmatched wait -- this private shim gives it a
    # controllable fake clock instead. hal_time_now_us() is linked for REAL
    # (hwAbstraction/host/fake_time.c), same convention as every other
    # executable that needs it.
    $exeOtaPicoRelay = Join-Path $outDir "kilnctl_host_tests_ota_pico_relay.exe"
    $otaPicoRelayObjDir = Join-Path $outDir "ota_pico_relay"
    New-Item -ItemType Directory -Force -Path $otaPicoRelayObjDir | Out-Null
    # /I stubs_ota_pico_relay precedes @hostTestsRsp's own /I list so this
    # executable's private freertos/task.h shim wins over the shared
    # stubs/freertos/task.h without touching the shared response file or any
    # other executable's include resolution.
    $otaPicoRelayStubDir = Join-Path $testDir "stubs_ota_pico_relay"
    $cmdOtaPicoRelay = "call `"$vcvars`" x64 >nul && cl /I`"$otaPicoRelayStubDir`" @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$otaPicoRelayObjDir\\`" /Fe:`"$exeOtaPicoRelay`" " +
            "`"$(Join-Path $testDir 'test_ota_pico_relay.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_time.c')`""

    Invoke-HostTestExe -Name "ota_pico_relay" -ExePath $exeOtaPicoRelay -BuildCmd $cmdOtaPicoRelay

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
            "`"$(Join-Path $hwAbsDir 'esp/spi/owner_slot_pool.c')`" `"$(Join-Path $driversDir 'common/stack_margin.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_time.c')`" `"$(Join-Path $driversDir 'safety/system_mode_gate.c')`""
    # docs/SYSTEM_MODE_GATE_PLAN.md, owner decision 2026-09-25 (Q1):
    # kiln_io_owner.c's relay_on_blocked() now calls system_mode_gate_
    # blocks_relay(), which calls the real system_mode_gate_check() -- linked
    # here for the same reason every other real dependency of kiln_io_owner.c
    # is linked into this executable rather than stubbed (it's a small, pure,
    # host-compilable module with no ESP-IDF dependency of its own).
    # CT_COMMISSIONING_PLAN.md step 2: kiln_io.c now calls hal_time_now_us()
    # (kiln_io_relays_off_ms()'s relays_all_off_since_us tracking) -- fake_time.c
    # supplies it, same as every other executable that links the real kiln_io.c/
    # any hal_time_now_us() caller (see the "hal_time_now_us() instead of
    # esp_timer_get_time()" notes elsewhere in this file).
    # stack_margin.c added DRAM_PSRAM_STATUS.md Phase 0 (4.2): kiln_io_owner.c's
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
                 "kilnlink_apply_config_volatile.c", "kilnlink_ceiling.c",
                 "kilnlink_clear_trip.c", "kilnlink_commit_config.c", "kilnlink_commit_config_rejected.c",
                 "kilnlink_context.c", "kilnlink_get_config_page.c", "kilnlink_get_ct_cal.c",
                 "kilnlink_get_param.c",
                 "kilnlink_rollback.c", "kilnlink_rollback_result.c", "kilnlink_reboot.c",
                 "kilnlink_reboot_result.c", "kilnlink_set_config.c", "kilnlink_set_ct_cal.c",
                 "kilnlink_set_log_level.c", "kilnlink_set_param.c", "kilnlink_frame.c", "kilnlink_crc.c",
                 "kilnlink_param.c", "kilnlink_param_value.c", "kilnlink_ct_auto_zero_begin.c",
                 "kilnlink_get_ct_auto_zero.c", "kilnlink_ct_auto_zero_status.c",
                 "kilnlink_stack_margin.c", "kilnlink_get_stack_margin.c") | ForEach-Object { "`"$(Join-Path $commonSrc $_)`"" }
    $cmd14 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$slObjDir\\`" /Fe:`"$exe14`" " +
            "`"$(Join-Path $testDir 'test_safety_link_compile.c')`" " +
            "`"$(Join-Path $driversDir 'common/stack_margin.c')`" `"$(Join-Path $driversDir 'safety/safety_trip_decision.c')`" " +
            "`"$(Join-Path $driversDir 'safety/safety_link_frame.c')`" " +
            "`"$(Join-Path $driversDir 'safety/heat_owner_active_decide.c')`" " +
            "`"$(Join-Path $testDir 'fake_danger_mode_for_safety_link.c')`" " +
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
            "`"$(Join-Path $driversDir 'persist/cfg_fs.c')`" `"$(Join-Path $driversDir 'persist/pref_cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs_status.c')`" " +
            "`"$(Join-Path $driversDir 'persist/flash_worker_wait.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
    # 2026-09-08: flash_worker_wait.c linked in -- adaptive_tune.c's
    # adaptive_tune_init() now calls flash_worker_wait_default() before its
    # kibase pref_cfg_fs_resolve() write (boot-ordering fix for the
    # relay_cycles/adaptive_tune/firing_stats migrate-on-load race, hardware
    # verification 3e226f28). Uses the shared bx_worker_stub.h's always-true
    # uart_bridge_ext_flash_worker_started() -- see that stub's own comment.
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
    # DRAM_PSRAM_STATUS.md section 7 safety-net pass: both modules' persist_locked()
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
            "`"$(Join-Path $driversDir 'persist/cfg_fs.c')`" `"$(Join-Path $driversDir 'persist/pref_cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs_status.c')`" " +
            "`"$(Join-Path $driversDir 'persist/flash_worker_wait.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
    # 2026-09-08: flash_worker_wait.c linked in -- relay_cycles_init() now
    # calls flash_worker_wait_default() before its migrate-on-load
    # pref_cfg_fs_resolve() write (same boot-ordering fix as adaptive_tune.c's
    # exe17, hardware verification 3e226f28).
    # relay_cycles cfg-filesystem dual-write bridge (docs/FILESYSTEM_USER_DATA_PLAN.md
    # section 5 step 6): relay_cycles.c (#included directly by test_relay_cycles.c
    # above) now also calls into pref_cfg_fs.c, which needs cfg_fs.c's real
    # mount/write-atomic/read/delete against a temp directory -- same convention
    # as exe2/exe4's cfg_fs.c+pref_cfg_fs.c link. The new dual-write tests live
    # directly in test_relay_cycles.c (it already #includes relay_cycles.c, so
    # it has file-scope access to s_rc/persist_snapshot/etc that a separate TU
    # would not).
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
    # FLASH_BUDGET.md sec 8 item 3's replacement for the broken JTAG-based
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
            "`"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            # kiln_http_register() rewiring: partition_info_http.c
            # (#included above) now calls it instead of
            # httpd_register_uri_handler() directly.
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            # http_auth_policy_iface.c/http_session_iface.c call into
            # web_auth_store.c (policy load) and web_auth_session.c (real
            # session table/role resolution) -- linked in for REAL, same
            # "already host-tested elsewhere, needs its real symbols to link"
            # reasoning used everywhere else these four files travel together.
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            # web_auth_store.c needs the host hal_kv backend (fake_kv.c);
            # http_session_iface.c needs hal_time_now_ms() (fake_time.c) --
            # neither otherwise linked into this executable.
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $testDir 'stubs/http_auth_link_stub.c')`""

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

    # ---- test_kiln_cfg_http.c: its own 24th, separate executable --------------
    # Closes a docs/SYSTEM_MODE_GATE_PLAN.md section 3.6 slice-4 known test gap:
    # kiln_cfg_http.c's apply_post_handler() (POST /api/kiln_configs/apply) had
    # NO host-test coverage of any kind before this -- its refusal order (404
    # nonexistent id, then the system_mode_gate, then the OTA interlock, then
    # http_async_job_busy()) was verified only by code-pattern review and an
    # ESP-IDF target build. #includes kiln_cfg_http.c directly (same "static
    # handler, no other seam" convention as test_zones_http.c/
    # test_backup_import.c) and stubs its whole store/swap-worker/interlock/
    # async-job dependency surface; system_mode_gate.c/system_mode_gate_http.c
    # are linked in for real, same convention as exe2/test_backup_import.c's
    # own executables, since apply_post_handler() calls those two directly.
    $exe24kcfg = Join-Path $outDir "kilnctl_host_tests_kiln_cfg_http.exe"
    $kcfgObjDir = Join-Path $outDir "kcfg"
    New-Item -ItemType Directory -Force -Path $kcfgObjDir | Out-Null
    $cmd24kcfg = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$kcfgObjDir\\`" /Fe:`"$exe24kcfg`" `"$(Join-Path $testDir 'test_kiln_cfg_http.c')`" " +
            "`"$(Join-Path $driversDir 'safety/system_mode_gate.c')`" " +
            "`"$(Join-Path $driversDir 'http/system_mode_gate_http.c')`""

    Invoke-HostTestExe -Name "kiln_cfg_http" -ExePath $exe24kcfg -BuildCmd $cmd24kcfg

    # ---- test_adaptive_tune_http_gate.c: its own separate executable ----------
    # Task 1a (docs/SYSTEM_MODE_GATE_PLAN.md known gap): adaptive_tune_http.c's
    # enable_post_handler()/revert_post_handler() system_mode_gate wiring
    # (owner decision 2026-09-25, narrowed same day: enable only gates
    # enabled=true, never enabled=false; revert has no such carve-out) was
    # verified only by code-pattern review and an ESP-IDF target build.
    # test_adaptive_tune_http.c already exists and deliberately never links
    # the rest of the translation unit (see its own header comment), so this
    # is a separate file/executable, same "own stub surface, own executable"
    # convention as test_kiln_cfg_http.c above. #includes adaptive_tune_http.c
    # directly (static handlers, no other seam); system_mode_gate.c/
    # system_mode_gate_http.c are linked in for real.
    $exeAtGate = Join-Path $outDir "kilnctl_host_tests_adaptive_tune_http_gate.exe"
    $atGateObjDir = Join-Path $outDir "atgate"
    New-Item -ItemType Directory -Force -Path $atGateObjDir | Out-Null
    $cmdAtGate = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$atGateObjDir\\`" /Fe:`"$exeAtGate`" `"$(Join-Path $testDir 'test_adaptive_tune_http_gate.c')`" " +
            "`"$(Join-Path $driversDir 'safety/system_mode_gate.c')`" " +
            "`"$(Join-Path $driversDir 'http/system_mode_gate_http.c')`""

    Invoke-HostTestExe -Name "adaptive_tune_http_gate" -ExePath $exeAtGate -BuildCmd $cmdAtGate

    # ---- test_uart_bridge_ext_control_gate.c: its own separate executable -----
    # #includes uart_bridge_ext_control.c (CONTROL task 8) directly (static
    # control_handle_message(), no other seam) to prove
    # CONTROL_CMD_SET_ZONE_PID/SET_ZONE_MODEL are refused by the system mode
    # gate while a firing or autotune run is active, same owner decision Q2
    # as adaptive_tune_http_gate above. system_mode_gate.c is linked in for
    # real (no system_mode_gate_http.c here -- this file's gate check replies
    # over the UART bridge's own ok/err framing, never HTTP). See the test
    # file's own header comment for why it pre-empts the UART_BRIDGE_H guard
    # (uart_bridge.h drags in safety_link.h -> screen_idle.h -> panel_spi.h,
    # which needs real ESP-IDF SPI/I2C driver headers no host stub covers,
    # and CONTROL task 8 needs nothing from uart_bridge.h but the
    # uart_protocol_t type).
    $exeUartBridgeControlGate = Join-Path $outDir "kilnctl_host_tests_uart_bridge_ext_control_gate.exe"
    $uartBridgeControlGateObjDir = Join-Path $outDir "uartbridgecontrolgate"
    New-Item -ItemType Directory -Force -Path $uartBridgeControlGateObjDir | Out-Null
    $cmdUartBridgeControlGate = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$uartBridgeControlGateObjDir\\`" /Fe:`"$exeUartBridgeControlGate`" " +
            "`"$(Join-Path $testDir 'test_uart_bridge_ext_control_gate.c')`" " +
            "`"$(Join-Path $driversDir 'safety/system_mode_gate.c')`""

    Invoke-HostTestExe -Name "uart_bridge_ext_control_gate" -ExePath $exeUartBridgeControlGate -BuildCmd $cmdUartBridgeControlGate

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

    # ---- test_profile_export_import.c: its own 29th, separate executable ------
    # Same reason as test_zones_http.c/test_profiles_http.c above: it #includes
    # profiles_export_http.c directly to reach import_post_handler(), `static`
    # with no public seam, and defines its own fake profiles_http_get()/
    # profiles_http_save()/profiles_http_get_bounds()/profiles_http_delete() --
    # the main executable and test_profiles_http.c each already link (or fake)
    # those same names differently, so this must be its own binary. Added
    # 2026-09-06 for the dwell_min upper-bound fix (previously only `dd < 0` was
    # checked before the (uint32_t) cast, so "dwell_min":1e30 was undefined
    # behavior instead of a clean 400).
    $exe29 = Join-Path $outDir "kilnctl_host_tests_profile_export_import.exe"
    $exe29ObjDir = Join-Path $outDir "profile_export_import_obj"
    New-Item -ItemType Directory -Force -Path $exe29ObjDir | Out-Null
    $cmd29 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$exe29ObjDir\\`" /Fe:`"$exe29`" " +
            "`"$(Join-Path $testDir 'test_profile_export_import.c')`" " +
            "`"$(Join-Path $driversDir 'persist/backup_json.c')`" " +
            # kiln_http_register() rewiring: profile_export_import's handler
            # (#included above) now calls it instead of
            # httpd_register_uri_handler() directly.
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            # http_auth_policy_iface.c/http_session_iface.c call into
            # web_auth_store.c (policy load) and web_auth_session.c (real
            # session table/role resolution) -- linked in for REAL, same
            # "already host-tested elsewhere, needs its real symbols to link"
            # reasoning used everywhere else these four files travel together.
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            # web_auth_store.c needs the host hal_kv backend (fake_kv.c);
            # http_session_iface.c needs hal_time_now_ms() (fake_time.c) --
            # neither otherwise linked into this executable.
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $testDir 'stubs/http_auth_link_stub.c')`""

    Invoke-HostTestExe -Name "profile_export_import" -ExePath $exe29 -BuildCmd $cmd29

    # ---- test_link_watchdog.c: its own THIRTIETH, separate executable --------
    # TODO.md: "The PC-link watchdog drops all relays every 5 s of host
    # silence" -- link_watchdog_decide.c/.h (bridge/) is the watchdog's owner-
    # distinction logic (uart_bridge.c's link_watchdog_task) pulled out so it
    # can be tested without FreeRTOS/kiln_io/lvgl/panel_spi. Links the REAL
    # relay_authority.c (its own executable so this test's relay-ownership
    # claims cannot collide with any other test's fakes of the same symbols,
    # same reasoning test_kiln_io_owner.c's header comment gives).
    $exe30 = Join-Path $outDir "kilnctl_host_tests_link_watchdog.exe"
    $lwObjDir = Join-Path $outDir "lw"
    New-Item -ItemType Directory -Force -Path $lwObjDir | Out-Null
    $cmd30 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$lwObjDir\\`" /Fe:`"$exe30`" " +
            "`"$(Join-Path $testDir 'test_link_watchdog.c')`" " +
            "`"$(Join-Path $driversDir 'bridge/link_watchdog_decide.c')`" " +
            "`"$(Join-Path $driversDir 'owners/relay_authority.c')`""

    Invoke-HostTestExe -Name "link_watchdog" -ExePath $exe30 -BuildCmd $cmd30

    # ---- test_time_sync.c: its own THIRTY-FIRST, separate executable ---------
    # docs/FILESYSTEM_USER_DATA_PLAN.md item 14 (TZ) close-out: this file used
    # to live in the "main" executable's $sources list, testing only
    # time_sync_tz.c's pure validation logic -- time_sync.c itself was
    # untested because it pulls in esp_netif_sntp.h with no host stub
    # (test_time_sync.c's own OLD header comment documented this gap
    # explicitly). test/stubs/esp_netif_sntp.h now exists, so this file
    # #includes time_sync.c directly (same convention as test_unit_pref.c)
    # and exercises its real NVS load / cfg_fs dual-write / tie-break paths.
    # Moved to its OWN executable rather than staying in "main": time_sync.c
    # defines the REAL time_sync_notify_got_ip(), which collides at link
    # time with test_wifi_prov.c's fake body of that name (wifi_prov.c calls
    # it; that test fakes it rather than pulling in the whole SNTP surface)
    # -- two definitions of the same external symbol cannot share one link.
    $exe31 = Join-Path $outDir "kilnctl_host_tests_time_sync.exe"
    $tsObjDir = Join-Path $outDir "ts"
    New-Item -ItemType Directory -Force -Path $tsObjDir | Out-Null
    $cmd31 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$tsObjDir\\`" /Fe:`"$exe31`" " +
            "`"$(Join-Path $testDir 'test_time_sync.c')`" " +
            "`"$(Join-Path $driversDir 'net/time_sync_tz.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs.c')`" `"$(Join-Path $driversDir 'persist/pref_cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "time_sync" -ExePath $exe31 -BuildCmd $cmd31

    # ---- test_cfg_fs_mount_reentrancy.c: its own THIRTY-SECOND, separate
    # executable ---------------------------------------------------------
    # docs/audits/filesystem_migration_review_2026-09-07.md section 1 /
    # check_flash_worker_lint.ps1's reentrancy-guard rule (bac50dbc):
    # cfg_fs_mount.c's cfg_fs_write_atomic_device() is the write function
    # installed into every *_cfg_fs.c bridge, and it used to dispatch onto
    # bx_flash_worker unconditionally -- a real deadlock when a bridge's
    # save() is reached from a caller already running ON that worker (e.g.
    # CONTROL_CMD_SET_UNIT_PREF -> unit_pref_set() -> pref_cfg_fs_save() ->
    # this same function, called again from inside itself). cfg_fs_mount.c
    # was previously "Not part of any host test build" (its own header
    # comment) because it #includes esp_littlefs.h/esp_partition.h -- new
    # stubs/esp_littlefs.h plus two added declarations in stubs/
    # esp_partition.h (esp_partition_find_first/esp_partition_read) close
    # that gap just enough for THIS translation unit to link. OWN, separate
    # executable rather than folded into "main": cfg_fs_mount.c needs its
    # own trivial per-exe stub bodies for esp_vfs_littlefs_register() and
    # friends (same "declared once, defined per test file" convention as
    # test_ota_http.c/test_partition_info_http.c), which would collide with
    # "main"'s own use of the real cfg_fs.c/cfg_fs_format_gate.c against a
    # DIFFERENT (real, mounted) filesystem in test_cfg_fs_status.c/test_cfg_
    # fs_format_gate.c's tests.
    $exe32 = Join-Path $outDir "kilnctl_host_tests_cfg_fs_mount_reentrancy.exe"
    $cfgMountObjDir = Join-Path $outDir "cfm"
    New-Item -ItemType Directory -Force -Path $cfgMountObjDir | Out-Null
    $cmd32 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$cfgMountObjDir\\`" /Fe:`"$exe32`" " +
            "`"$(Join-Path $testDir 'test_cfg_fs_mount_reentrancy.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs_mount.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs_format_gate.c')`" " +
            "`"$(Join-Path $driversDir 'persist/flash_worker_wait.c')`" " +
            "`"$(Join-Path $driversDir 'persist/boot_guard.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`" " +
            # check_hal_include_boundary.ps1: cfg_fs_mount.c's format-timeout
            # elapsed-time measurement moved off esp_timer_get_time() onto
            # hal_time_now_us() (interface/hal_time.h) -- fake_time.c supplies
            # it here, same convention as every other hal_time_now_us() caller
            # in this file (see the other "fake_time.c supplies it" comments).
            "`"$(Join-Path $hwAbsDir 'host/fake_time.c')`""

    Invoke-HostTestExe -Name "cfg_fs_mount_reentrancy" -ExePath $exe32 -BuildCmd $cmd32

    # ---- test_recovery_start_refusal.c retired 2026-09-27 (mode-gate slice
    # 2, docs/SYSTEM_MODE_GATE_PLAN.md section 3.6): its subject,
    # App/drivers/http/recovery_start_refusal.h, was deleted -- its two HTTP
    # call sites now go through system_mode_gate_check() instead
    # (dashboard_exec_http.c, dashboard_autotune_http.c), which shares its
    # wording across HTTP/UART/LCD. Coverage for the recovery-mode rule moved
    # into test_system_mode_gate.c below. The exe33 variable name is left
    # unused rather than renumbering every later $exeNN; only $totalExpected
    # (near the end of this script) is an enforced count.

    # ---- test_readiness_gate.c: its own THIRTY-FOURTH, separate
    # executable ----------------------------------------------------------
    # The readiness FIRING INTERLOCK (owner decision 2026-09-09):
    # App/drivers/safety/readiness_gate.h, which turns four /api/readiness
    # checklist items into a real refusal on every start path. Header-only
    # decision (static inline over readiness_http.h's shared predicates), so
    # this needs no sibling .c files -- just a fake body for the one declared
    # symbol, readiness_gate_collect(), same convention
    # test_ota_http.c uses for boot_guard_is_recovery_mode().
    # Own executable because that fake would collide at link time with
    # readiness_gate.c's real body wherever that gets linked.
    $exe34 = Join-Path $outDir "kilnctl_host_tests_readiness_gate.exe"
    $cmd34 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exe34`" `"$(Join-Path $testDir 'test_readiness_gate.c')`""

    Invoke-HostTestExe -Name "readiness_gate" -ExePath $exe34 -BuildCmd $cmd34

    # ---- test_s8_rate_guard_estimate.c: its own THIRTY-FIFTH, separate
    # executable. docs/audits/s8_auto_calc_design_2026-09-09.md's auto-calc
    # for the RP2040 safety processor's S8 rate-of-rise guard. Own executable
    # for the same reason zone_coupling_solve's exe20 is: s8_rate_guard_
    # estimate.c is a small, fully self-contained pure-math module (only
    # <math.h> and MAX31856.h's channel-count constant) with no fakes to
    # collide with anything already linked into another executable.
    $exe35 = Join-Path $outDir "kilnctl_host_tests_s8_rate_guard_estimate.exe"
    $cmd35 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exe35`" `"$(Join-Path $testDir 'test_s8_rate_guard_estimate.c')`" " +
            "`"$(Join-Path $driversDir 'control/s8_rate_guard_estimate.c')`""

    Invoke-HostTestExe -Name "s8_rate_guard_estimate" -ExePath $exe35 -BuildCmd $cmd35

    # ---- test_flash_worker_boot_order.c: its own THIRTY-FIFTH, separate
    # executable. Positive/negative test for the 2026-09-08 flash-worker
    # boot-ordering fix (1f741635, "boot order beats bounded waits" --
    # uart_bridge_ext_start_flash_worker() moved to run BEFORE
    # relay_cycles_init()/adaptive_tune_init() in main_control_bringup.c).
    # Found completely unwired 2026-09-10 (opus review): it has its own
    # main(), was never named in this script, and the only reference
    # anywhere in the tracked tree was a prose mention in
    # docs/FILESYSTEM_PLAN.md. Own executable because it hand-declares
    # uart_bridge_ext_flash_worker_started() itself (see the file's own
    # header comment) -- that would collide at link time with the real
    # uart_bridge.c body, or with exe17/exe19/exe20's own fakes of the same
    # symbol, wherever else it's linked.
    $exe36 = Join-Path $outDir "kilnctl_host_tests_flash_worker_boot_order.exe"
    $cmd36 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exe36`" `"$(Join-Path $testDir 'test_flash_worker_boot_order.c')`" " +
            "`"$(Join-Path $driversDir 'persist/flash_worker_wait.c')`""

    Invoke-HostTestExe -Name "flash_worker_boot_order" -ExePath $exe36 -BuildCmd $cmd36

    # ---- test_safety_stack_margin_http.c: its own THIRTY-SEVENTH, separate
    # executable. GET /api/saftyfw_stack_margin (safety_stack_margin_http.c),
    # the ESP-side surface for SaftyFW's nine live per-task stack marks
    # (docs/audits/saftyfw_live_stack_reporting_impl_2026-09-14.md). Own
    # executable, same "no other seam" reason as test_partition_info_http.c/
    # test_safety_cfg_http.c: it #includes safety_stack_margin_http.c
    # directly and defines its own fake safety_link_get_stack_margin()/
    # wifi_provision_http_get_server()/httpd_register_uri_handler() bodies,
    # which would multiply-define against test_safety_cfg_http.c's own
    # fakes of the httpd symbols if linked together.
    $exe37 = Join-Path $outDir "kilnctl_host_tests_safety_stack_margin_http.exe"
    $ssmObjDir = Join-Path $outDir "ssm"
    New-Item -ItemType Directory -Force -Path $ssmObjDir | Out-Null
    $cmd37 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$ssmObjDir\\`" /Fe:`"$exe37`" `"$(Join-Path $testDir 'test_safety_stack_margin_http.c')`" " +
            # kiln_http_register() rewiring: safety_stack_margin_http.c
            # (#included above) now calls it instead of
            # httpd_register_uri_handler() directly.
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            # http_auth_policy_iface.c/http_session_iface.c call into
            # web_auth_store.c (policy load) and web_auth_session.c (real
            # session table/role resolution) -- linked in for REAL, same
            # "already host-tested elsewhere, needs its real symbols to link"
            # reasoning used everywhere else these four files travel together.
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            # web_auth_store.c needs the host hal_kv backend (fake_kv.c);
            # http_session_iface.c needs hal_time_now_ms() (fake_time.c) --
            # neither otherwise linked into this executable.
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $testDir 'stubs/http_auth_link_stub.c')`""

    Invoke-HostTestExe -Name "safety_stack_margin_http" -ExePath $exe37 -BuildCmd $cmd37

    # ---- test_config_divergence.c: its own THIRTY-EIGHTH, separate
    # executable. config_divergence.{h,c} (App/drivers/safety) is the
    # 2026-09-14 owner decision's reusable "format version + hash" config
    # identity comparator ("if a config doesn't land and match on both
    # sides then alarm and dissable heaters"). Own executable: it is a
    # small, fully self-contained pure module (<stdio.h>/<stdlib.h>/
    # <string.h> only, no fakes) with no seam to share and no reason to
    # collide with anything else already linked -- same rationale as
    # exe35's s8_rate_guard_estimate.c.
    $exe38 = Join-Path $outDir "kilnctl_host_tests_config_divergence.exe"
    $cmd38 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exe38`" `"$(Join-Path $testDir 'test_config_divergence.c')`" " +
            "`"$(Join-Path $driversDir 'safety/config_divergence.c')`""

    Invoke-HostTestExe -Name "config_divergence" -ExePath $exe38 -BuildCmd $cmd38

    # ---- test_kiln_package.c: its own THIRTY-NINTH, separate executable.
    # docs/KILN_PROFILES_PLAN.md items 1/2/12 -- kiln_package.c walks the
    # ESP-side mirror of CONFIG_PARAM_TABLE via an INJECTED accessor pair
    # (kiln_pkg_pico_source_t), specifically so it never needs the real
    # safety_cfg_store.c (already linked for real into the MAIN executable
    # via test_safety_cfg_store.c -- linking it again here would multiply-
    # define every safety_cfg_store_* symbol, same reasoning exe3's own
    # comment gives for safety_cfg_http.c). Own executable, fake table only.
    # 2026-09-14 (docs/KILN_PROFILES_PLAN.md items 3/4/9/14, "finish upload/
    # download"): kiln_package.c now also builds/parses the section 5.1 JSON
    # envelope via backup_json.c's hand-rolled reader (the same one
    # backup_import.c already links) -- added as a second source file here,
    # not #included by test_kiln_package.c itself, so its own symbols are
    # compiled exactly once and this stays consistent with exe38's own
    # multi-source-file shape above.
    $exe39 = Join-Path $outDir "kilnctl_host_tests_kiln_package.exe"
    $cmd39 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exe39`" `"$(Join-Path $testDir 'test_kiln_package.c')`" " +
            "`"$(Join-Path $driversDir 'persist/backup_json.c')`""

    Invoke-HostTestExe -Name "kiln_package" -ExePath $exe39 -BuildCmd $cmd39

    # ---- test_safety_ceiling_sync_divergence.c: its own FORTIETH, separate
    # executable. 2026-09-14 opus review defect 3
    # (docs/audits/pico_ceiling_mirror_and_rate_guard_2026-09-14.md):
    # safety_ceiling_sync.h cited this exact file name as covering the
    # divergence-enforcement path (hooks firing, the latch, the
    # target_known exclusion) since that feature landed, but the file did
    # not exist -- this closes that gap. Own executable: it #includes
    # safety_ceiling_sync.h and links the REAL safety_ceiling_sync.c/
    # safety_ceiling_policy.c/config_divergence.c, but supplies its OWN
    # fake bodies for zones_config_is_valid()/_get_temp_limits(),
    # safety_cfg_store_param_count()/_get_by_index() and safety_cfg_http_
    # set_and_confirm_f32() -- linking the REAL zones_config_store.c/
    # safety_cfg_store.c/safety_cfg_http.c would pull in NVS/httpd
    # machinery this enforcement-only test has no need of, and would
    # multiply-define against exe2 (test_zones_http.c)'s own real links of
    # those same files if ever combined into one binary. fake_time.c
    # supplies hal_time_now_us(), same convention as exe2's own link of it.
    $exe40 = Join-Path $outDir "kilnctl_host_tests_safety_ceiling_sync_divergence.exe"
    $cmd40 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exe40`" `"$(Join-Path $testDir 'test_safety_ceiling_sync_divergence.c')`" " +
            "`"$(Join-Path $driversDir 'safety/safety_ceiling_sync.c')`" " +
            "`"$(Join-Path $driversDir 'safety/safety_ceiling_policy.c')`" " +
            "`"$(Join-Path $driversDir 'safety/config_divergence.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_time.c')`""

    Invoke-HostTestExe -Name "safety_ceiling_sync_divergence" -ExePath $exe40 -BuildCmd $cmd40

    # ---- test_crash_report.c: its own 41st, separate executable -----------
    # 2026-09-15 MEDIUM/LOW fixes (review_crash_report_relay_gate_61765de7):
    # crash_report_acknowledge()/crash_report_clear() now dispatch their NVS
    # writes through uart_bridge_ext_run_on_flash_worker() (PSRAM-stack safety,
    # same reasoning as relay_cycles.c/safety_cfg_store.c), so this file now
    # needs stubs/bx_worker_stub.h's non-static uart_bridge_ext_run_on_flash_
    # worker()/uart_bridge_ext_is_on_flash_worker() definitions. It used to be
    # folded into the main $sources executable (which also links
    # test_safety_cfg_store.c's OWN, differently-shaped, non-static definition
    # of uart_bridge_ext_run_on_flash_worker() -- a real LNK2005 duplicate
    # symbol once both were linked together, not merely a style question), so
    # it now gets its own executable instead, same pattern as test_relay_
    # cycles.c/test_run_state.c's exe19 above.
    $exe41 = Join-Path $outDir "kilnctl_host_tests_crash_report.exe"
    $crObjDir = Join-Path $outDir "cr"
    New-Item -ItemType Directory -Force -Path $crObjDir | Out-Null
    $cmd41 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$crObjDir\\`" /Fe:`"$exe41`" `"$(Join-Path $testDir 'test_crash_report.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" `"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "crash_report" -ExePath $exe41 -BuildCmd $cmd41

    # ---- test_heat_owner_active_decide.c: its own 42nd, separate executable --
    # 2026-09-15 (Opus adversarial re-review, F6: docs/audits/review_tc_type_
    # fixes_a2384dd5_2026-09-15.md). HEAT_OWNER_ACTIVE's producer
    # (safety_build_and_send_context() in safety_link_frames.c) had no test at
    # all -- the only host executable that links safety_link_frame.c
    # (singular; this new one links neither that file nor safety_link_frames.c,
    # plural, so there is no overlap) fakes heat_enable_is_held()/
    # danger_mode_active() fixed false, so nothing ever asserted the flag
    # flips true for PAUSED, a held claimant, or danger mode. Pulled the
    # decision into heat_owner_active_decide.c, a pure function taking the
    # executor state plus three bools, so it can be host-tested directly
    # without linking heat_enable.c/danger_mode.c/profile_executor.c. Own
    # executable, minimal deps (profile_executor_state.h + libc only).
    $exe42 = Join-Path $outDir "kilnctl_host_tests_heat_owner_active_decide.exe"
    $hoadObjDir = Join-Path $outDir "hoad"
    New-Item -ItemType Directory -Force -Path $hoadObjDir | Out-Null
    $cmd42 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$hoadObjDir\\`" /Fe:`"$exe42`" `"$(Join-Path $testDir 'test_heat_owner_active_decide.c')`""

    Invoke-HostTestExe -Name "heat_owner_active_decide" -ExePath $exe42 -BuildCmd $cmd42

    # ---- test_web_auth_store.c: its own 43rd, separate executable ----------
    # docs/WEB_AUTH_PLAN.md sections 2/3/11 -- the credential storage
    # foundation. Own executable (not joined into the combined $sources
    # executable) because it needs psa/crypto.h's host stub for real
    # PBKDF2-style hashing, and no file in the combined executable pulls
    # that stub in today (only ota_http.c does, and that file is deliberately
    # NOT linked there) -- isolating it avoids any static/global collision
    # with the ~30+ files the combined executable already shares one
    # namespace across. Same fake_kv.c/hal_status.c/hal_esp_common.c minimal
    # link set as exe41 (crash_report) above, since this module round-trips
    # through the same hal_kv.h fake.
    $exe43 = Join-Path $outDir "kilnctl_host_tests_web_auth_store.exe"
    $waObjDir = Join-Path $outDir "wa"
    New-Item -ItemType Directory -Force -Path $waObjDir | Out-Null
    $cmd43 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$waObjDir\\`" /Fe:`"$exe43`" `"$(Join-Path $testDir 'test_web_auth_store.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" " +
            "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" `"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "web_auth_store" -ExePath $exe43 -BuildCmd $cmd43

    # ---- test_lcd_credential_bridge.c: its own 44th, separate executable --
    # lcd_credential_bridge.c wires the LCD keypad/lock seams to
    # web_auth_store.c and carries two decisions of its own worth testing in
    # combination (role ordering, effective-enabled collapse) -- see that
    # file's header comment. Same isolation rationale and link set as exe43
    # immediately above: needs psa/crypto.h's host stub for real PBKDF2-style
    # hashing via web_auth_store.c, and the same fake_kv.c/hal_status.c/
    # hal_esp_common.c minimal set. Does NOT link lcd_auth_state.c or
    # ui_lcd_lock.c -- the latter is LVGL-dependent and not host-testable, so
    # the test file supplies its own trivial stub definitions of both
    # modules' setter functions instead.
    $exe44 = Join-Path $outDir "kilnctl_host_tests_lcd_credential_bridge.exe"
    $lcbObjDir = Join-Path $outDir "lcb"
    New-Item -ItemType Directory -Force -Path $lcbObjDir | Out-Null
    $cmd44 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$lcbObjDir\\`" /Fe:`"$exe44`" `"$(Join-Path $testDir 'test_lcd_credential_bridge.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" " +
            "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" `"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "lcd_credential_bridge" -ExePath $exe44 -BuildCmd $cmd44

    # ---- test_web_auth_login_http.c: its own 45th, separate executable ----
    # docs/audits/web_auth_adversarial_review_2026-09-17.md Findings 4/5:
    # per-source-IP login lockout and a properly-looped httpd_req_recv().
    # web_auth_login_http.c is #include'd directly (same convention as
    # exe43/exe44 above) to reach login_lockout_slot_for() and the per-IP
    # lockout table it owns, both `static`. This file supplies its OWN
    # test-controllable httpd_req_recv()/ota_http_get_client_ip() bodies
    # (never stubs/http_auth_link_stub.c, which is deliberately not
    # test-controllable) so a test can stage a source IP per call and force
    # httpd_req_recv() to hand the body back in single-byte chunks. Needs
    # psa/crypto.h's host stub + fake_kv.h (web_auth_store.c, also
    # #include'd directly, same as exe43) plus the real session-table/
    # policy/route seam (http_session_iface.c, web_auth_session.c,
    # http_auth_http.c, http_auth_enforce.c, http_auth_policy_iface.c)
    # linked in for real, same "already host-tested elsewhere, needs its
    # real symbols to link" reasoning used everywhere else those travel
    # together -- login_post_handler() calls http_session_table()/
    # http_session_hash_token() and web_auth_table_create_session() against
    # THIS real table, not a stand-in for it.
    $exe45 = Join-Path $outDir "kilnctl_host_tests_web_auth_login_http.exe"
    $walhObjDir = Join-Path $outDir "walh"
    New-Item -ItemType Directory -Force -Path $walhObjDir | Out-Null
    $cmd45 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$walhObjDir\\`" /Fe:`"$exe45`" `"$(Join-Path $testDir 'test_web_auth_login_http.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_session_iface.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            "`"$(Join-Path $driversDir 'net/ota_auth.c')`" `"$(Join-Path $driversDir 'http/ota_http_util.c')`" " +
            "`"$(Join-Path $driversDir 'net/login_backoff.c')`" " +
            "`"$(Join-Path $driversDir 'net/web_auth_login.c')`" " +
            "`"$(Join-Path $driversDir 'http/login_ip_scope.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" `"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "web_auth_login_http" -ExePath $exe45 -BuildCmd $cmd45

    # ---- test_readiness_crash_disclosure.c: its own 46th, separate
    # executable -- 2026-09-17 ROUTE_TIER_OPEN disclosure audit finding 2:
    # GET /api/readiness's crash-report checklist item leaked
    # exc_cause_str/exc_task with no auth check, sidestepping
    # GET /api/crash_report's deliberate ROUTE_TIER_ADMIN classification.
    # readiness_http.c itself cannot be host-compiled (two GCC-only
    # asm("_binary_...") blob externs `cl` cannot parse), so this file tests
    # the pure formatter (readiness_crash_report_detail(), readiness_http.h,
    # header-only) directly, plus the real may_disclose gate composed from
    # the actual http_auth_http.c/http_auth_enforce.c/http_auth_policy_iface.c/
    # http_session_iface.c/web_auth_session.c/web_auth_store.c stack -- same
    # link set and rationale as exe9 (test_dashboard_status_http.c) and
    # exe45 immediately above. 2026-09-17 adversarial-review follow-up: also
    # links http_auth_disclosure_gate.c for real (never stubbed) -- that is
    # now the ONE function (http_auth_may_disclose()) both this test and
    # readiness_http.c's actual call site invoke, closing the vacuous-test
    # gap a negative test found in the original version of this file (see
    # http_auth_disclosure_gate.h's own header comment). See the test
    # file's own header comment for the full split.
    $exe46 = Join-Path $outDir "kilnctl_host_tests_readiness_crash_disclosure.exe"
    $rcdObjDir = Join-Path $outDir "rcd"
    New-Item -ItemType Directory -Force -Path $rcdObjDir | Out-Null
    $cmd46 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$rcdObjDir\\`" /Fe:`"$exe46`" " +
            "`"$(Join-Path $testDir 'test_readiness_crash_disclosure.c')`" " +
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_disclosure_gate.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" " +
            "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "readiness_crash_disclosure" -ExePath $exe46 -BuildCmd $cmd46

    # ---- test_wifi_prov_status_disclosure.c: its own 47th, separate
    # executable -- 2026-09-17 ROUTE_TIER_OPEN disclosure audit finding 1:
    # GET /status leaked the saved home network's SSID and static-IP
    # topology with no auth check. wifi_provision_http.c itself cannot be
    # host-compiled (twelve GCC-only asm("_binary_...") blob externs plus an
    # #include <sys/socket.h> with no host stub anywhere in this repo), so
    # this file tests the pure formatter (wifi_prov_status_redact_field(),
    # wifi_prov.h, header-only) directly, plus the real may_disclose gate --
    # same link set and rationale as exe46 immediately above.
    $exe47 = Join-Path $outDir "kilnctl_host_tests_wifi_prov_status_disclosure.exe"
    $wpsdObjDir = Join-Path $outDir "wpsd"
    New-Item -ItemType Directory -Force -Path $wpsdObjDir | Out-Null
    $cmd47 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$wpsdObjDir\\`" /Fe:`"$exe47`" " +
            "`"$(Join-Path $testDir 'test_wifi_prov_status_disclosure.c')`" " +
            "`"$(Join-Path $driversDir 'persist/web_auth_store.c')`" `"$(Join-Path $driversDir 'net/web_auth_session.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_http.c')`" `"$(Join-Path $driversDir 'http/http_auth_enforce.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_policy_iface.c')`" `"$(Join-Path $driversDir 'http/http_session_iface.c')`" " +
            "`"$(Join-Path $testDir 'stubs/wifi_prov_unprovisioned_stub.c')`" " +
            "`"$(Join-Path $driversDir 'http/http_auth_disclosure_gate.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_sysinfo.c')`" `"$(Join-Path $hwAbsDir 'host/fake_time.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" " +
            "`"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""

    Invoke-HostTestExe -Name "wifi_prov_status_disclosure" -ExePath $exe47 -BuildCmd $cmd47

    # ---- test_log_store_mount.c: its own 48th AND 49th executables --------
    # docs/FILESYSTEM_PLAN.md step 2. log_store_mount.c was "not part of any
    # host test build" (its own header comment) -- stubs/esp_spiffs.h (new,
    # this change) plus the existing stubs/esp_littlefs.h close that gap.
    # Built TWICE from the SAME test source, once per CONFIG_KILNCTL_LOGS_
    # LITTLEFS state, so the plan's step 2 requirement ("compiles under both
    # flag states") is actually exercised rather than assumed: exe48 leaves
    # the macro undefined (the bench's default, SPIFFS branch) and exe49
    # defines it to 1 (the LittleFS branch) via /D on the cl command line.
    $exe48 = Join-Path $outDir "kilnctl_host_tests_log_store_mount_spiffs.exe"
    $lsmSpiffsObjDir = Join-Path $outDir "lsm_spiffs"
    New-Item -ItemType Directory -Force -Path $lsmSpiffsObjDir | Out-Null
    $cmd48 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$lsmSpiffsObjDir\\`" /Fe:`"$exe48`" " +
            "`"$(Join-Path $testDir 'test_log_store_mount.c')`" " +
            "`"$(Join-Path $driversDir 'persist/log_store_mount.c')`" " +
            "`"$(Join-Path $driversDir 'persist/log_store.c')`""

    Invoke-HostTestExe -Name "log_store_mount_spiffs" -ExePath $exe48 -BuildCmd $cmd48

    $exe49 = Join-Path $outDir "kilnctl_host_tests_log_store_mount_littlefs.exe"
    $lsmLittlefsObjDir = Join-Path $outDir "lsm_littlefs"
    New-Item -ItemType Directory -Force -Path $lsmLittlefsObjDir | Out-Null
    $cmd49 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 /DCONFIG_KILNCTL_LOGS_LITTLEFS=1 " +
            "/Fo:`"$lsmLittlefsObjDir\\`" /Fe:`"$exe49`" " +
            "`"$(Join-Path $testDir 'test_log_store_mount.c')`" " +
            "`"$(Join-Path $driversDir 'persist/log_store_mount.c')`" " +
            "`"$(Join-Path $driversDir 'persist/log_store.c')`""

    Invoke-HostTestExe -Name "log_store_mount_littlefs" -ExePath $exe49 -BuildCmd $cmd49

    # ---- test_system_mode_gate.c: its own FIFTIETH, separate executable.
    # docs/SYSTEM_MODE_GATE_PLAN.md's Phase 6 gate (2026-09-25) -- a small,
    # fully self-contained pure module (no ESP-IDF dependency at all, unlike
    # ota_interlock.c/readiness_gate.c which pull in more), so like exe35's
    # s8_rate_guard_estimate there is nothing here to fake and no fake to
    # collide with anything else already linked.
    $exe50 = Join-Path $outDir "kilnctl_host_tests_system_mode_gate.exe"
    $cmd50 = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exe50`" `"$(Join-Path $testDir 'test_system_mode_gate.c')`" " +
            "`"$(Join-Path $driversDir 'safety/system_mode_gate.c')`""

    Invoke-HostTestExe -Name "system_mode_gate" -ExePath $exe50 -BuildCmd $cmd50

    # ---- sim_iter_tune.exe / sim_wide_temp_sweep.exe: data-generating
    # harnesses (ITER_TUNE_REDESIGN_PLAN.md sec 6/7), not TEST_CHECK
    # pass/fail suites -- their stdout is the evidence for the audit docs
    # they feed, not a verdict. Until 2026-09-10 NEITHER had any build
    # recipe anywhere in the tree (confirmed by grep) -- both were edited by
    # the additive-coupling-model change (d63a5591) and compiled clean by
    # hand at review time, but that means every prior edit to either file
    # went in completely unverified by any automated path. Build them here,
    # informationally, same as sim_credibility_gate below: never counted in
    # $totalExpected/buildFailures/failedExes (they take bench captures /
    # profile arguments this script does not have and are not meant to run
    # unattended), but a build failure here is now visible instead of silent.
    $exeIterTune = Join-Path $outDir "kilnctl_sim_iter_tune.exe"
    $cmdIterTune = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeIterTune`" `"$(Join-Path $testDir 'sim_iter_tune.c')`" " +
            "`"$(Join-Path $testDir 'sim_plant.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/heater_output.c')`" " +
            "`"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`" " +
            "`"$(Join-Path $driversDir 'control/firing_score.c')`" `"$(Join-Path $driversDir 'control/firing_compare.c')`" " +
            "`"$(Join-Path $driversDir 'control/iter_tune.c')`""
    if (Test-Path $exeIterTune) { Remove-Item -Force $exeIterTune }
    cmd.exe /c $cmdIterTune
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exeIterTune)) {
        Write-Host "BUILD FAILED: sim_iter_tune (informational only, does not fail this script)"
    } else {
        Write-Host "sim_iter_tune: BUILD OK (data-generating harness, not run automatically -- see file header for usage)"
    }

    $exeWideSweep = Join-Path $outDir "kilnctl_sim_wide_temp_sweep.exe"
    $cmdWideSweep = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeWideSweep`" `"$(Join-Path $testDir 'sim_wide_temp_sweep.c')`" " +
            "`"$(Join-Path $testDir 'sim_plant.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/heater_output.c')`" " +
            "`"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`""
    if (Test-Path $exeWideSweep) { Remove-Item -Force $exeWideSweep }
    cmd.exe /c $cmdWideSweep
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exeWideSweep)) {
        Write-Host "BUILD FAILED: sim_wide_temp_sweep (informational only, does not fail this script)"
    } else {
        Write-Host "sim_wide_temp_sweep: BUILD OK (data-generating harness, not run automatically -- see file header for usage)"
    }

    # ---- sim_credibility_gate.exe: ITER_TUNE_REDESIGN_PLAN.md sec 6.5's
    # model credibility gate (docs/audits/sim_credibility_gate_real_cause_2026-09-10.md).
    # Not run through Invoke-HostTestExe and NOT counted in $totalExpected /
    # buildFailures / failedExes below -- two reasons, both deliberate:
    #   1. Its two capture inputs (logs/coupling/noise_floor_p7*.jsonl) and
    #      tools/PcTools/config_presets/noise_floor.json are gitignored/
    #      local-only. A fresh clone must SKIP (exit 3), never fail or
    #      silently pass -- the gate already refuses to run without them
    #      and prints why; that SKIP must stay visibly distinct from the
    #      main pass count, not vanish into it either direction.
    #   2. Even WITH the captures present, this gate currently FAILS on
    #      multiple bars, not one -- as of 2026-09-10, against the two
    #      checked-in captures: dwell offset fails 5 of 6 zone/capture
    #      cells (up to -4.82C against a +/-1.5C bar); z2 CALIBRATION ramp
    #      MAE (3.367C) exceeds its own 3.0C bar; the dwell-entry overshoot
    #      PEAK bar fails 2 of 12 evaluable cells (6 more are structurally
    #      UNEVALUABLE -- segment 2 occurs in a single capture tick); and
    #      the noise-floor spread check fails 4 of 6 cells. See the gate's
    #      own "-- honest per-bar tally --" output for the current counts;
    #      do not summarize this as "one known-open bar" anywhere -- that
    #      framing was wrong and made new regressions indistinguishable
    #      from the pre-existing failures. Folding a gate that fails this
    #      broadly into the blocking build would either turn every
    #      host-test run red for reasons already tracked here (masking a
    #      genuine NEW regression in the noise) or force loosening bars to
    #      make it pass -- both worse than surfacing it informationally.
    # So: build it, run it if the captures exist, print its own PASS/FAIL/
    # SKIP line and per-bar tally, but never let its exit code affect this
    # script's own exit code. This is strictly better than the prior state
    # (referenced by no build recipe at all, so nothing re-ran it) without
    # pretending the gaps above are closed.
    $exeGate = Join-Path $outDir "kilnctl_sim_credibility_gate.exe"
    $cmdGate = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeGate`" `"$(Join-Path $testDir 'sim_credibility_gate.c')`" " +
            "`"$(Join-Path $testDir 'sim_plant.c')`" `"$(Join-Path $driversDir 'control/heater_output.c')`""
    if (Test-Path $exeGate) { Remove-Item -Force $exeGate }
    cmd.exe /c $cmdGate
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exeGate)) {
        Write-Host "BUILD FAILED: sim_credibility_gate (informational only, does not fail this script)"
    } else {
        $repoRoot = Join-Path $testDir "..\..\..\.."
        $calPath = Join-Path $repoRoot "logs\coupling\noise_floor_p7_run1.jsonl"
        $holdPath = Join-Path $repoRoot "logs\coupling\noise_floor_p7d_run1.jsonl"
        $nfPath = Join-Path $repoRoot "tools\PcTools\config_presets\noise_floor.json"
        $gateOutput = & $exeGate $calPath $holdPath $nfPath
        $gateOutput | ForEach-Object { Write-Host $_ }
        $script:simCredibilityGateTally = ($gateOutput | Select-String -Pattern "^\s*(ramp MAE|dwell offset|dwell-entry peak|noise-floor spread):") -join "`n"
        switch ($LASTEXITCODE) {
            0 { $script:simCredibilityGateLine = "sim_credibility_gate: PASS" }
            3 { $script:simCredibilityGateLine = "sim_credibility_gate: SKIP (captures not present locally -- gitignored, expected on a fresh clone)" }
            default { $script:simCredibilityGateLine = "sim_credibility_gate: FAIL (informational only, does not fail this script -- see the per-bar tally above/below and docs/audits/sim_credibility_gate_real_cause_2026-09-10.md)" }
        }
        Write-Host $script:simCredibilityGateLine
    }

    # ---- sim_credibility_gate_closedloop.exe: candidate 3 from
    # docs/audits/sim_credibility_gate_real_cause_2026-09-10.md sec 6/8.
    # Found completely unwired 2026-09-10 (opus review): 600 lines, no build
    # recipe anywhere. Its own header comment states plainly that it is NOT
    # a pass/fail gate the way sim_credibility_gate.c is (its replay is
    # closed-loop against the SETPOINT through the real controller, so a
    # mismatch confounds plant model / controller gains / controller's own
    # plant model into one number) and that gains provenance for the two
    # checked-in captures is not established (both candidate sources --
    # tuned_baseline_20260831.json and the live board's control_get_zones()
    # -- are demonstrably wrong for the captures' 2026-09-02 timestamp; see
    # the file's own header for the numbers). Its negative/inconclusive
    # finding was deliberate, so treating its stdout as a verdict would be
    # papering over that finding, not fixing anything -- but leaving it
    # with literally no automated signal is how it went unwired in the
    # first place. Compromise: build it every run (a real, mechanical
    # signal, printed loudly either way -- if pid.c/heater_output.c/
    # zone_coupling_solve.c/profile_executor_feedforward.c's signatures
    # drift, "BUILD FAILED" now appears in this script's output instead of
    # nothing appearing at all), but never run it automatically and never
    # let its build/run result affect this script's own exit code -- same
    # non-gate posture as sim_iter_tune/sim_wide_temp_sweep above, for the
    # same reason sim_credibility_gate's own gains-uncertain conclusions
    # are informational rather than blocking.
    $exeClosedloop = Join-Path $outDir "kilnctl_sim_credibility_gate_closedloop.exe"
    $cmdClosedloop = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeClosedloop`" `"$(Join-Path $testDir 'sim_credibility_gate_closedloop.c')`" " +
            "`"$(Join-Path $testDir 'sim_plant.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/heater_output.c')`" " +
            "`"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`" " +
            "`"$(Join-Path $driversDir 'control/profile_executor_feedforward.c')`""
    if (Test-Path $exeClosedloop) { Remove-Item -Force $exeClosedloop }
    cmd.exe /c $cmdClosedloop
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exeClosedloop)) {
        # 2026-09-10: a build failure here used to be purely cosmetic --
        # printed and then discarded, same posture as sim_iter_tune/
        # sim_wide_temp_sweep/sim_credibility_gate above, which is right for
        # THEM (their run/verdict is genuinely informational) but wrong here:
        # this file has no informational verdict at all, since it is never
        # run automatically -- a BUILD failure is the only mechanical signal
        # this file can ever produce, so letting it be silently non-gating
        # meant it could never fail anything, which is exactly the "check
        # that cannot fail" class this repo is eliminating. Its RESULTS stay
        # non-gating (it is still never run automatically, and no PASS/FAIL
        # verdict from it is ever consulted) -- only a compile break now
        # fails this script, via the same $script:buildFailures list every
        # other Invoke-HostTestExe-tracked source uses.
        Write-Host "BUILD FAILED: sim_credibility_gate_closedloop -- this DOES fail this script (compile-only signal; the file is still never run automatically and its results, when it does run, stay non-gating -- see file header)"
        $script:buildFailures += "sim_credibility_gate_closedloop"
    } else {
        Write-Host "sim_credibility_gate_closedloop: BUILD OK (not run automatically -- diagnostic with unresolved gains provenance, see file header; run by hand with the two capture paths as argv)"
    }

    # ---- sim_fuzzy_closedloop.exe: the closed-loop fuzzy-path harness
    # docs/audits/fuzzy_controller_improvement_scoping_2026-09-11.md
    # identified as the blocking prerequisite (no sim exercised
    # pid_fuzzy_adjust() at all). Unlike sim_credibility_gate_closedloop
    # above, this one IS run automatically and DOES gate the build via
    # Invoke-HostTestExe -- every assertion inside it is deterministic,
    # needs no external capture files, and has a known-correct answer (the
    # strength_pct=0 bit-for-bit contract, and full 9-cell rule coverage at
    # strength_pct 25/50) -- see the file's own top comment for why this
    # posture differs from every other sim_*.c harness in this list.
    $exeFuzzyCl = Join-Path $outDir "kilnctl_sim_fuzzy_closedloop.exe"
    $cmdFuzzyCl = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeFuzzyCl`" `"$(Join-Path $testDir 'sim_fuzzy_closedloop.c')`" " +
            "`"$(Join-Path $testDir 'sim_plant.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/pid_fuzzy.c')`""
    Invoke-HostTestExe -Name "sim_fuzzy_closedloop" -ExePath $exeFuzzyCl -BuildCmd $cmdFuzzyCl

    # ---- sim_scenarios.exe: docs/SCENARIO_SIMULATION_PLAN.md WI-4/5/6/7
    # (runner, scenario table S0-S12, arm selection, all six arms; see
    # sim_scenarios.c's own top comment for exactly what is and is not real
    # yet, e.g. SIM_ARM_PID_AT/SIM_ARM_FUZZY_AT are single-firing stand-ins
    # until WI-8). Same gating posture as sim_fuzzy_closedloop.c immediately
    # above: run automatically (--shard 0 --of 1, the canonical unsharded
    # pass), DOES gate the build, deterministic, no external capture files.
    # The sharded (--of 4) determinism proof runs separately via
    # run_sim_scenarios.ps1, not as part of this default build-time pass
    # (WI-7). Negative-tested 2026-09-14 (zeroed SIM_SCENARIO_TABLE[0]'s
    # model_k_dc by hand, confirmed SCENARIO_REFUSED + non-zero exit with no
    # numeric row printed, restored by hand, forced a full rebuild,
    # reconfirmed PASS -- see the commit this shipped in for the
    # transcript).
    $exeScenarios = Join-Path $outDir "kilnctl_sim_scenarios.exe"
    $cmdScenarios = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeScenarios`" `"$(Join-Path $testDir 'sim_scenarios.c')`" " +
            "`"$(Join-Path $testDir 'sim_scenario_table.c')`" `"$(Join-Path $testDir 'sim_plant.c')`" " +
            "`"$(Join-Path $testDir 'sim_high_temp.c')`" `"$(Join-Path $testDir 'sim_mistune.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/pid_fuzzy.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid_autotune.c')`" `"$(Join-Path $driversDir 'control/firing_score.c')`""
    Invoke-HostTestExe -Name "sim_scenarios" -ExePath $exeScenarios -BuildCmd $cmdScenarios

    # ---- sim_scenarios_adaptive.exe: docs/SCENARIO_SIMULATION_PLAN.md WI-8
    # -- SIM_ARM_PID_AT/SIM_ARM_FUZZY_AT as a chain of 9 firings with
    # adaptation state carried across them, driving the REAL adaptive_
    # tune.c/adaptive_tune_model.c/adaptive_tune_ki.c (not a mirror) against
    # a single-zone zones_config test fake. Own executable, same reason
    # test_adaptive_tune.c's exe17 is (adaptive_tune.c's much larger link
    # surface -- hal_kv/esp_log/flash_worker_wait/pref_cfg_fs/cfg_fs_status/
    # a real FreeRTOS mutex against the host stub -- would multiply-define
    # against sim_scenarios.c's own fakes if forced into one executable).
    # Gates the build: a scenario/firing that cannot run refuses loudly, and
    # WI-8 acceptance (a) (S7's belief_k_dc must move toward the true plant
    # gain across the 9 runs) is a hard FAIL if violated. Negative-tested
    # 2026-09-14 (broke ADAPTIVE_TUNE_BLEND_ALPHA to 0.0f in adaptive_tune_
    # internal.h by hand -- refinement then never moves k_dc at all --
    # confirmed S7_DIRECTION_CHECK FAIL and non-zero exit, restored by hand,
    # forced a full rebuild, reconfirmed PASS -- see the commit this shipped
    # in for the transcript).
    $exeScenariosAdaptive = Join-Path $outDir "kilnctl_sim_scenarios_adaptive.exe"
    $atsObjDir = Join-Path $outDir "atsim"
    New-Item -ItemType Directory -Force -Path $atsObjDir | Out-Null
    $cmdScenariosAdaptive = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$atsObjDir\\`" /Fe:`"$exeScenariosAdaptive`" " +
            "`"$(Join-Path $testDir 'sim_scenarios_adaptive.c')`" " +
            "`"$(Join-Path $testDir 'sim_scenario_table.c')`" `"$(Join-Path $testDir 'sim_plant.c')`" " +
            "`"$(Join-Path $testDir 'sim_high_temp.c')`" `"$(Join-Path $testDir 'sim_mistune.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/pid_fuzzy.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid_autotune.c')`" " +
            "`"$(Join-Path $driversDir 'control/zone_coupling_solve.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs.c')`" `"$(Join-Path $driversDir 'persist/pref_cfg_fs.c')`" " +
            "`"$(Join-Path $driversDir 'persist/cfg_fs_status.c')`" " +
            "`"$(Join-Path $driversDir 'persist/flash_worker_wait.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`""
    Invoke-HostTestExe -Name "sim_scenarios_adaptive" -ExePath $exeScenariosAdaptive -BuildCmd $cmdScenariosAdaptive

    # ---- sim_strength_pct_adapt.exe: docs/SCENARIO_SIMULATION_PLAN.md WI-10
    # (docs/audits/strength_pct_adapter_design_2026-09-16.md is the design;
    # this executable is its simulation arm). A cross-firing scalar
    # hill-climb over strength_pct for S2/S5/S7/S12, adjudicated ONLY by the
    # real, unmodified firing_compare() comparing consecutive already-
    # finished firings' own firing_score_set_t -- never a within-run
    # inference (see the design doc sec 3). Own executable, lighter link
    # surface than sim_scenarios_adaptive.exe (no adaptive_tune.c, no
    # zones_config fake, no FreeRTOS mutex): only pid/pid_fuzzy/pid_autotune/
    # firing_score/firing_compare/sim_plant, same four-plus-two-file posture
    # as sim_scenarios.exe plus the comparator. Gates the build on internal
    # self-consistency only (a named scenario missing from the table, a NaN
    # this design's own refusal handling does not explain, or the
    # strength_pct=0 bit-exact contract breaking) -- a chain that never
    # accepts a step, oscillates, or wanders is a printed, non-fatal finding,
    # per WI-10's own acceptance line and the design doc sec 5.
    $exeStrengthAdapt = Join-Path $outDir "kilnctl_sim_strength_pct_adapt.exe"
    $cmdStrengthAdapt = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeStrengthAdapt`" `"$(Join-Path $testDir 'sim_strength_pct_adapt.c')`" " +
            "`"$(Join-Path $testDir 'sim_scenario_table.c')`" `"$(Join-Path $testDir 'sim_plant.c')`" " +
            "`"$(Join-Path $testDir 'sim_high_temp.c')`" `"$(Join-Path $testDir 'sim_mistune.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/pid_fuzzy.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid_autotune.c')`" `"$(Join-Path $driversDir 'control/firing_score.c')`" " +
            "`"$(Join-Path $driversDir 'control/firing_compare.c')`""
    Invoke-HostTestExe -Name "sim_strength_pct_adapt" -ExePath $exeStrengthAdapt -BuildCmd $cmdStrengthAdapt

    # ---- sim_fuzzy_overshoot.exe: docs/audits/fuzzy_overshoot_measurement_
    # 2026-09-13.md -- the owner correction that sim_fuzzy_closedloop.c's
    # tracking scenario (IAE/MAE averaged over a whole run) is the wrong
    # instrument for the fuzzy layer's actual stated purpose (reducing
    # over/undershoot, a transient concentrated at dwell entry). A
    # deliberately SEPARATE, standalone file (not an edit to sim_fuzzy_
    # closedloop.c, which was mid-edit by another session this same day --
    # see this file's own top comment) that runs ordinary ramp-to-dwell
    # transitions (no synthetic disturbance injection) and reports peak
    # overshoot/undershoot at dwell entry using firing_score.c's own
    # FIRING_SUBSCORE_ENTRY_PEAK_C window/peak-tracking definition. Gates
    # the build: its own assertions (strength_pct=0 bit-exact contract,
    # every dwell's entry window actually reached) are deterministic and
    # have a known-correct answer.
    $exeFuzzyOv = Join-Path $outDir "kilnctl_sim_fuzzy_overshoot.exe"
    $cmdFuzzyOv = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeFuzzyOv`" `"$(Join-Path $testDir 'sim_fuzzy_overshoot.c')`" " +
            "`"$(Join-Path $testDir 'sim_plant.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid.c')`" `"$(Join-Path $driversDir 'control/pid_fuzzy.c')`""
    Invoke-HostTestExe -Name "sim_fuzzy_overshoot" -ExePath $exeFuzzyOv -BuildCmd $cmdFuzzyOv

    # ---- fuzzy_nine_cell_probe.exe: docs/FUZZY_CONTROLLER_PLAN.md sec 5
    # Stage 0 ("(v-a) offline rule-cell probe") -- a pure, no-plant,
    # no-board numerical probe of pid_fuzzy_adjust() at all 9 rule-table
    # cells, reporting the REAL multiplicative kp/ki/kd effect (not the
    # rule table's raw +-1/0 integers) at strength_pct 25/50, the physical
    # (degC, degC/s) coordinates each cell dominates at, and a per-cell
    # reachability verdict against this plant's own measured ~0.083 degC/s
    # max ramp rate and ~40 degC max bench rise. Complementary to, not a
    # duplicate of, sim_fuzzy_closedloop.c above: that harness answers
    # whether a closed-loop trajectory can REACH all 9 cells; this one
    # answers what each cell actually DOES in physical units once it
    # fires, and asserts the centre-cell contract (the only cell ever
    # observed on hardware) stays exactly as documented. Deterministic,
    # gates the build via Invoke-HostTestExe -- see the file's own top
    # comment for the negative-test transcript.
    $exeFuzzyCell = Join-Path $outDir "kilnctl_fuzzy_nine_cell_probe.exe"
    $cmdFuzzyCell = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeFuzzyCell`" `"$(Join-Path $testDir 'fuzzy_nine_cell_probe.c')`" " +
            "`"$(Join-Path $driversDir 'control/pid_fuzzy.c')`""
    Invoke-HostTestExe -Name "fuzzy_nine_cell_probe" -ExePath $exeFuzzyCell -BuildCmd $cmdFuzzyCell

    # ---- test_iter_tune_http.c: its own 59th, separate executable ------------
    # step 7 review, 2026-09-23 (docs/ITER_TUNE_REDESIGN_PLAN.md step 7), finding
    # 2 + advisory A1: proves iter_tune_restore_post_handler() (POST
    # /api/iter_tune/restore_commissioned) (a) persists the RESTORED baseline
    # (not the stale pre-restore one) on a successful restore, (b) leaves the
    # persisted record completely untouched when zones_config_set_pid()
    # refuses, and (c) refuses with 409 -- touching neither
    # zones_config_set_pid() nor the persisted record -- while autotune owns
    # the zone. Own executable, same "#includes the driver .c directly to
    # reach a static handler" reason as test_partition_info_http.c: it
    # supplies its own fake iter_tune_store_*()/zones_config_set_pid()/
    # autotune_engine_is_active_on_zone() bodies (in-RAM, no NVS/cfg_fs --
    # the store's own persistence behaviour is test_iter_tune_store.c's job),
    # and links the REAL iter_tune.c so iter_tune_restore_commissioned()'s
    # actual baseline-mutation behaviour is exercised, not a stand-in for it.
    # kiln_http_register()/wifi_provision_http_get_server() are faked locally
    # (same lightweight pattern test_profiles_live_http.c uses), so no auth
    # stack needs to be linked in.
    $exeIth = Join-Path $outDir "kilnctl_host_tests_iter_tune_http.exe"
    $ithObjDir = Join-Path $outDir "ith"
    New-Item -ItemType Directory -Force -Path $ithObjDir | Out-Null
    $cmdIth = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$ithObjDir\\`" /Fe:`"$exeIth`" `"$(Join-Path $testDir 'test_iter_tune_http.c')`" " +
            "`"$(Join-Path $driversDir 'control/iter_tune.c')`" " +
            # ITER_TUNE_REDESIGN_PLAN.md step 8: iter_tune_http.c's status
            # handler now calls firing_shadow_get_status() -- link the real
            # module (already host-tested by test_firing_shadow.c, its own
            # executable) plus its hal_kv dependency chain, same reasoning as
            # test_profile_executor_prestart.c's own $cmd4 fix needed.
            "`"$(Join-Path $driversDir 'control/firing_shadow.c')`" `"$(Join-Path $driversDir 'control/firing_score.c')`" " +
            "`"$(Join-Path $driversDir 'control/firing_compare.c')`" " +
            "`"$(Join-Path $hwAbsDir 'host/fake_kv.c')`" `"$(Join-Path $hwAbsDir 'common/hal_status.c')`" " +
            "`"$(Join-Path $hwAbsDir 'esp/common/hal_esp_common.c')`" " +
            # system_mode_gate wiring (docs/SYSTEM_MODE_GATE_PLAN.md,
            # gate-slice-3-followup, 2026-09-25): iter_tune_restore_post_
            # handler() now calls system_mode_gate_check()/system_mode_gate_
            # http_send_refusal() -- link both real, pure, leaf modules,
            # same as $cmd50/$cmd4's own fix.
            "`"$(Join-Path $driversDir 'safety/system_mode_gate.c')`" " +
            "`"$(Join-Path $driversDir 'http/system_mode_gate_http.c')`""

    Invoke-HostTestExe -Name "iter_tune_http" -ExePath $exeIth -BuildCmd $cmdIth

    # ---- test_firing_compare_alloc.c: its own SEPARATE executable ---------
    # ITER_TUNE_REDESIGN_PLAN.md step 8 review: firing_compare() now heap-
    # allocates raw[]/norm[]/in_band[] (reachable from the profile_executor
    # task stack via firing_shadow_finish_firing()). This #includes
    # firing_compare.c with malloc/free redirected to counting fakes to prove
    # each allocation failure returns ALLOC_FAILED without leaking -- so
    # it cannot share an executable that links the real firing_compare.c.
    $exeFca = Join-Path $outDir "kilnctl_host_tests_firing_compare_alloc.exe"
    $fcaObjDir = Join-Path $outDir "fca"
    New-Item -ItemType Directory -Force -Path $fcaObjDir | Out-Null
    $cmdFca = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$fcaObjDir\\`" /Fe:`"$exeFca`" `"$(Join-Path $testDir 'test_firing_compare_alloc.c')`" " +
            "`"$(Join-Path $driversDir 'control/firing_score.c')`""

    Invoke-HostTestExe -Name "firing_compare_alloc" -ExePath $exeFca -BuildCmd $cmdFca

    # ---- firing_score_from_capture.exe: ITER_TUNE_REDESIGN_PLAN.md sec
    # 3.1.1's "recommended next step" -- feeds a recorded capture's real
    # per-tick data into the PRODUCTION firing_score.c/firing_compare.c
    # (linked as-is, same convention as sim_iter_tune above) so Bar 2's
    # real-hardware noise floor can be measured instead of guessed. Same
    # informational posture as sim_credibility_gate directly above: never
    # counted in $totalExpected/buildFailures/failedExes, and a missing
    # capture SKIPs (exit 3) rather than failing or silently passing.
    $exeFsfc = Join-Path $outDir "kilnctl_firing_score_from_capture.exe"
    $cmdFsfc = "call `"$vcvars`" x64 >nul && cl @`"$hostTestsRsp`" /std:c11 " +
            "/Fo:`"$outDir\\`" /Fe:`"$exeFsfc`" `"$(Join-Path $testDir 'firing_score_from_capture.c')`" " +
            "`"$(Join-Path $driversDir 'control/firing_score.c')`" `"$(Join-Path $driversDir 'control/firing_compare.c')`""
    if (Test-Path $exeFsfc) { Remove-Item -Force $exeFsfc }
    cmd.exe /c $cmdFsfc
    if ($LASTEXITCODE -ne 0 -or -not (Test-Path $exeFsfc)) {
        Write-Host "BUILD FAILED: firing_score_from_capture (informational only, does not fail this script)"
    } else {
        $repoRoot = Join-Path $testDir "..\..\..\.."
        $basePath = Join-Path $repoRoot "logs\coupling\noise_floor_p7_run1.jsonl"
        $trialPath = Join-Path $repoRoot "logs\coupling\noise_floor_p7d_run3.jsonl"
        $fsfcOutput = & $exeFsfc $basePath $trialPath
        $fsfcOutput | ForEach-Object { Write-Host $_ }
        switch ($LASTEXITCODE) {
            0 { $script:firingScoreFromCaptureLine = "firing_score_from_capture: RAN (informational -- see verdict/medians above)" }
            3 { $script:firingScoreFromCaptureLine = "firing_score_from_capture: SKIP (captures not present locally -- gitignored, expected on a fresh clone)" }
            default { $script:firingScoreFromCaptureLine = "firing_score_from_capture: ERROR exit $LASTEXITCODE (informational only, does not fail this script)" }
        }
        Write-Host $script:firingScoreFromCaptureLine
    }

    # ---- summary ----------------------------------------------------------
    #
    # 28 executables are attempted above (main + zones_http + safety_cfg_http +
    # profile_executor_prestart + autotune_engine_prestart + profiles_http +
    # ota_http + uart_protocol_link_delegate + board_temps + kiln_io_owner +
    # safety_trip_words + safety_trip_decision + safety_link + dashboard_json +
    # telemetry_format + adaptive_tune + event_log + run_state_relay_cycles +
    # zone_coupling_solve + partition_info_http + adaptive_tune_http +
    # profiles_builtin + ft6336u + max31856_hal_spi + hal_i2c_adopt +
    # hal_spi_adopt + hal_spi_async + profile_export_import + link_watchdog +
    # time_sync) --
    # opus review
    # finding (LOW): this used to say 28 (both in this comment and in
    # $totalExpected below) while only 27 Invoke-HostTestExe calls actually
    # existed above (the $exeN numbering itself skips from $exe5 to $exe7, a
    # leftover from some earlier executable that no longer exists -- there never
    # was an $exe6). That let the summary print "Built: 27/28 executables"
    # immediately followed by "all 28 host test executables built and passed":
    # the pass/fail gate below only ever checked buildFailures/failedExes, never
    # that the built count matched what was expected, so a silently-skipped
    # executable (never built, never run, never added to either failure list)
    # would read as a full green suite. Fixed two ways: the expected count now
    # matches the real number of calls above, AND the gate below independently
    # checks the built count against it, so a FUTURE executable silently dropped
    # from this list (added to the comment/count but never wired to an
    # Invoke-HostTestExe call, or vice versa) fails loud instead of depending on
    # this comment staying accurate by hand.
    # 30 -> 31 (this pass): a concurrent pass added test_cfg_fs_mount_
    # reentrancy.c as its own 31st Invoke-HostTestExe call (grep count above
    # confirms 31 real calls exist) without bumping this constant, which is
    # exactly the silent-mismatch failure mode this comment's own history
    # describes -- caught by this same gate, not introduced by it.
    # 31 -> 32: this pass added test_recovery_start_refusal.c as its own
    # 32nd Invoke-HostTestExe call (recovery_start_refusal.h's explicit,
    # named recovery-mode API-layer refusal, ui_aggregate_review_2026-09-08).
    # 32 -> 33: this pass added test_readiness_gate.c as its own 33rd
    # Invoke-HostTestExe call (readiness_gate.h's firing interlock, owner
    # decision 2026-09-09 -- see docs/SAFETY_CASE.md sec 3 item 10).
    # 33 -> 34: this pass added test_s8_rate_guard_estimate.c as its own 34th
    # Invoke-HostTestExe call (S8 rate-guard auto-calc,
    # docs/audits/s8_auto_calc_design_2026-09-09.md).
    # 34 -> 35: this pass wired test_flash_worker_boot_order.c as its own
    # 35th Invoke-HostTestExe call -- found completely unwired 2026-09-10
    # (opus review, check_no_orphaned_checks.ps1 gap): it had its own
    # main() and no build recipe anywhere in the tree, so no automated
    # path had ever run it since it was added for the 2026-09-08
    # flash-worker boot-order fix (1f741635).
    # 35 -> 36: this pass added sim_fuzzy_closedloop.c as its own 36th
    # Invoke-HostTestExe call -- the closed-loop fuzzy-path harness
    # docs/audits/fuzzy_controller_improvement_scoping_2026-09-11.md
    # identified as the missing blocking prerequisite (no sim ever called
    # pid_fuzzy_adjust()). Unlike sim_credibility_gate_closedloop, this one
    # IS deterministic/self-contained enough to gate the build.
    # 36 -> 37: this pass added fuzzy_nine_cell_probe.c as its own 37th
    # Invoke-HostTestExe call -- FUZZY_CONTROLLER_PLAN.md sec 5's Stage 0
    # offline 9-cell probe, reporting per-cell physical gain effects and
    # reachability verdicts (docs/audits/fuzzy_nine_cell_offline_probe_2026-09-11.md).
    # 37 -> 38: this pass added sim_fuzzy_overshoot.c as its own 38th
    # Invoke-HostTestExe call -- docs/audits/fuzzy_overshoot_measurement_
    # 2026-09-13.md's re-measurement of the fuzzy layer on overshoot
    # (its actual stated purpose) rather than IAE/MAE.
    # 38 -> 39: test_safety_stack_margin_http.c's own 39th Invoke-
    # HostTestExe call -- docs/audits/saftyfw_live_stack_reporting_impl_
    # 2026-09-14.md's HTTP surface for SaftyFW's nine live stack marks.
    # 39 -> 40: this pass added sim_scenarios.c as its own 40th Invoke-
    # HostTestExe call -- docs/SCENARIO_SIMULATION_PLAN.md WI-4's runner
    # skeleton (S0/S1/S3, all six arms), same gating posture as
    # sim_fuzzy_closedloop.c above (deterministic, no external captures).
    # 40 -> 41: this pass added sim_scenarios_adaptive.c as its own 41st
    # Invoke-HostTestExe call -- docs/SCENARIO_SIMULATION_PLAN.md WI-8's
    # 9-firing chained-adaptation harness, driving the real adaptive_tune.c
    # against a zones_config test fake (see that file's own header).
    # 42 -> 45: two Invoke-HostTestExe calls were added by other work without
    # updating this counter (found already stuck at 42 with 44 real calls
    # present before this pass touched the file -- not this pass' doing,
    # left as found rather than investigated further) plus this pass' own
    # 45th call, test_kiln_cfg_swap.c (docs/KILN_PROFILES_PLAN.md item 5).
    # 46 -> 47: this pass (F6, docs/audits/review_tc_type_fixes_a2384dd5_
    # 2026-09-15.md) added test_heat_owner_active_decide.c as its own 47th
    # Invoke-HostTestExe call -- counted by grep against the actual file at
    # edit time (47), not by incrementing the stale prior value by one, since
    # this counter had already drifted before (see the 42->45 note above).
    # 47 -> 48: this pass added sim_strength_pct_adapt.c as its own 48th
    # Invoke-HostTestExe call -- docs/SCENARIO_SIMULATION_PLAN.md WI-10's
    # strength_pct cross-firing adapter simulation arm (design doc
    # docs/audits/strength_pct_adapter_design_2026-09-16.md).
    # 49 -> 50: test_lcd_credential_bridge.c added as its own 50th
    # Invoke-HostTestExe call -- lcd_credential_bridge.c's role-ordering and
    # effective-enabled-collapse logic, previously untested (see that file's
    # header comment and App/test/test_lcd_credential_bridge.c).
    # 50 -> 51: this pass (docs/audits/web_auth_adversarial_review_2026-09-17.md
    # Findings 4/5) added test_web_auth_login_http.c as its own 51st
    # Invoke-HostTestExe call.
    # 51 -> 52: 2026-09-17 audit finding 7 follow-up added
    # test_dashboard_status_http.c as its own 52nd Invoke-HostTestExe call,
    # proving GET /api/status's build-identity redaction (same gate 1a41a972
    # established for GET /api/ota/esp/status).
    # 52 -> 54: 2026-09-17 ROUTE_TIER_OPEN disclosure audit (findings 1/2)
    # added test_readiness_crash_disclosure.c (53rd) and
    # test_wifi_prov_status_disclosure.c (54th) as their own Invoke-HostTestExe
    # calls -- readiness_http.c/wifi_provision_http.c both cannot be
    # host-compiled directly (GCC-only asm blob externs; wifi_provision_http.c
    # also #includes <sys/socket.h> with no host stub), so each tests its
    # fix's pure formatter plus the real may_disclose gate composition.
    # 54 -> 55: docs/LIVE_PROFILE_EDIT_PLAN.md pass 1 added
    # test_live_profile.c (55th) as its own Invoke-HostTestExe call --
    # live_profile.c's persistence functions and profile_executor_live_
    # pickup.c's pure pickup check, both untested until now.
    # 55 -> 56: docs/LIVE_PROFILE_EDIT_PLAN.md pass 2 added
    # test_profiles_live_http.c (56th) as its own Invoke-HostTestExe call --
    # profiles_live_http.c's five HTTP handlers (status/fork/accept/decide/
    # page), untested until now.
    # 58 -> 59: step 7 review, 2026-09-23 (finding 2 + advisory A1) added
    # test_iter_tune_http.c as its own 59th Invoke-HostTestExe call --
    # iter_tune_restore_post_handler()'s persist-on-success/refuse-on-
    # rejected-apply/refuse-while-autotune-active behaviour, previously
    # untested (see that file's own header comment).
    # 59 -> 60: added test_ota_pico_relay.c's own Invoke-HostTestExe call --
    # ota_pico_relay.c's relay state machine, untested until now
    # (docs/PICO_AUTO_UPDATE_PLAN.md).
    # 60 -> 61: added test_firing_compare_alloc.c's own Invoke-HostTestExe
    # call -- firing_compare()'s heap-allocation failure path
    # (ITER_TUNE_REDESIGN_PLAN.md step 8 review).
    # 61 -> 62: added test_system_mode_gate.c's own Invoke-HostTestExe call --
    # docs/SYSTEM_MODE_GATE_PLAN.md Phase 6's pure gate module (owner decisions
    # 2026-09-25).
    # 62 -> 61 (2026-09-27, mode-gate slice 2): retired test_recovery_start_
    # refusal.c's Invoke-HostTestExe call along with its subject header,
    # App/drivers/http/recovery_start_refusal.h -- both HTTP call sites now go
    # through system_mode_gate_check() instead, covered by test_system_mode_
    # gate.c's own (expanded) cases rather than a separate executable.
    # 61 -> 62: added test_kiln_cfg_http.c's own Invoke-HostTestExe call --
    # kiln_cfg_http.c's apply_post_handler() refusal order (404, then the
    # system mode gate, then the OTA interlock, then http_async_job_busy()),
    # a known test gap named in docs/SYSTEM_MODE_GATE_PLAN.md section 3.6
    # slice 4, previously untested at the handler level.
    # 62 -> 63: added test_adaptive_tune_http_gate.c's own Invoke-HostTestExe
    # call -- adaptive_tune_http.c's enable_post_handler()/revert_post_handler()
    # system_mode_gate wiring (Task 1a, docs/SYSTEM_MODE_GATE_PLAN.md known
    # gap), previously untested at the handler level.
    # 63 -> 64: added test_uart_bridge_ext_control_gate.c's own Invoke-HostTestExe
    # call -- uart_bridge_ext_control.c's CONTROL task 8
    # CONTROL_CMD_SET_ZONE_PID/SET_ZONE_MODEL system_mode_gate wiring, same
    # owner decision Q2 gap class as adaptive_tune_http_gate above, previously
    # untested at the UART bridge handler level.
    # 64 -> 65: added test_ui_edit_firing_apply.c's own Invoke-HostTestExe
    # call -- the LCD Edit-firing page's Apply sequence and step clamps
    # (ui_edit_firing_apply.c), exercised against the real live_profile.c.
    $totalExpected = 65
    Write-Host ""
    if ($script:simCredibilityGateLine) {
        # Non-blocking, but its verdict must not scroll off above the
        # summary unseen (2026-09-10: a non-blocking check whose result is
        # printed once, above a page of other output, and read by nothing
        # is close to the "reports green with zero coverage" shape this
        # repo has been burned by before). Repeat it here.
        Write-Host $script:simCredibilityGateLine
        if ($script:simCredibilityGateTally) { Write-Host $script:simCredibilityGateTally }
        Write-Host ""
    }
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

    if ($script:builtExes.Count -ne $totalExpected) {
        Write-Host ("MISMATCH: $($script:builtExes.Count) executables built but $totalExpected were expected -- " +
            "at least one was silently never attempted (not a build failure, not a run failure). Refusing to " +
            "report success.")
        exit 1
    }

    Write-Host "all $totalExpected host test executables built and passed"
    exit 0
} finally {
    Exit-BuildLock -Lock $buildLock
}
} finally {
    Exit-KilnBuildGate -Gate $buildGate
}
