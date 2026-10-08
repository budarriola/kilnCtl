# Gate to negative-test evidence

Consolidated table for `docs/RELEASE_HARDENING_PLAN.md` section 3, acceptance step 4.
One row per check that `tools/run_all_checks.ps1 -ListOnly` discovers (plus the guard that
enforces this table). Source: the eleven `docs/audits/release_gate_vacuity_audit_*.md` files
(suffixes 09-16, 09-16b to 09-16g, 09-17, 09-18, 10-02, and 10-07 for pass 12,
`release_gate_vacuity_audit_2026-10-07.md`).

This is an index, not new evidence: every mutation and result below is a one-line
summary of what the named audit recorded. Read the audit for the full procedure and restore proof.

## How to read it

- **NEGATIVE-TESTED**: a real mutation of production source (or the guarded input) made the check go RED with a named reason, the mutation was restored by hand, and the check returned to PASS. For build-measuring checks the audit also forced a full rebuild.
- **PARTIAL**: only part of the check was exercised, or the FAIL path was seen incidentally rather than deliberately.
- **REVIEWED, NOT MUTATED**: read or screened (09-16 read-only, 09-18 screens A to D) and judged sound, but no mutation was run. Do not treat this as negative-tested.
- **NOT AUDITED**: no audit names it. A green run of this gate is unproven against the vacuous-pass failure classes.
- **NOT AUDITED (pass 12 pending)**: added to the repo after the 10-02 audit; pass 12 is expected to cover it.

Pass 09-18 also ran mechanical screens A to D over the whole population then in the repo (no failure path, skip-instead-of-fail, wrapper does not invoke its script, wired only by a comment). Only its findings are recorded per row; a clean screen is not a negative test.

Not exercisable locally, by design: `check_01_kilnfw_pushed_build.ps1` and `check_01_saftyfw_pushed_build.ps1` build `origin/main` itself, so a local mutation cannot reach them (see 09-18).

## Counts

Gate rows: 169.

| Status | Rows |
|---|---|
| NEGATIVE-TESTED | 60 |
| PARTIAL | 1 |
| REVIEWED, NOT MUTATED | 9 |
| NOT AUDITED | 83 |
| NOT AUDITED (pass 12 pending) | 17 |

## Table

Maintenance: when `tools/check_gate_negative_test_table.ps1` fails, a discovered check has no row (or a row names a check that no longer exists). Add or remove the row; a new check may start as `NOT AUDITED`. Do not rewrite audit evidence here without a new audit file behind it.

| Gate | Status | Audit | Mutation | Result |
|---|---|---|---|---|
| `firmware/CommonFW/test/check_commonfw_ctest.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/CommonFW/test/check_commonfw_diag_vectors.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/hwAbstraction/test/compile_esp_backends.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/hwAbstraction/test/compile_pico_backends.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/hwAbstraction/test/test_host_fakes.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_00_kilnfw_host_tests.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_00_kilnfw_recovery_target_build.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_00_kilnfw_target_build.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_01_kilnfw_pushed_build.ps1` | PARTIAL | 09-18 | cannot be driven locally (builds origin/main in a throwaway worktree); 09-16e saw it genuinely FAIL on a real -Werror=format-truncation defect on origin/main | FAIL path propagated incidentally; no deliberate mutation |
| `firmware/KilnFW/App/test/check_all_task_stack_budgets.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_approach_rate_cap_mirror_drift.ps1` | NEGATIVE-TESTED | 09-16 | production divisor 3600.0f changed to 1800.0f in the real cap loop | RED, both fragments shown; hand-restored; PASS |
| `firmware/KilnFW/App/test/check_boot_guard_reset_reachability.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_cfg_convert_field_mirror_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_cfg_fs_tie_break.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_cfgfs_nvs_only_drift.ps1` | NEGATIVE-TESTED | 09-16 | empty persist/zz_audit_dummy_cfg_fs.c created | RED, names the new bridge file; file deleted |
| `firmware/KilnFW/App/test/check_embedded_pico_image_fresh.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_executor_task_stack_budget.ps1` | NEGATIVE-TESTED | 09-16b | 1024 B volatile local injected in zone_coupling_gauss_solve_partial_pivot_vec, real rebuild | +1024 B exactly, RED at 2800 B > 1936 B; fullclean rebuild back to baseline |
| `firmware/KilnFW/App/test/check_flash_partition_map.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_flash_worker_lint.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_frame_a_offset_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_fuzzy_gain_mirror_drift.ps1` | NEGATIVE-TESTED | 09-16b | kp/ki argument order swapped in the real pid_fuzzy_adjust call | RED, first divergent line shown; hand-restored; PASS |
| `firmware/KilnFW/App/test/check_heater_output_pwm_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_httpd_task_stack_budget.ps1` | NEGATIVE-TESTED | 09-16c | 1024 B volatile local in profile_decode_blob, real rebuild | +1024 B exactly, RED 5328 B > 4832 B; fullclean rebuild back to 4304 B |
| `firmware/KilnFW/App/test/check_js_host_tests.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_kilnfw_dram_bss_budget.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_kv_narrow_stack.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_label_column_overflow_wrap.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_littlefs_component_pinned.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_main_task_stack_budget.ps1` | NEGATIVE-TESTED | 09-16, 09-16b | [09-16] --stack-bytes 100 override against a real ELF (compare arithmetic only) ; [09-16b] 2048 B then 4096 B volatile local injected in profile_encode_current_blob, real idf.py rebuild | [09-16] RED, names the worst path ; [09-16b] measured +2048 B exactly, then RED at 7728 B > 6144 B; forced fullclean rebuild back to 3632 B |
| `firmware/KilnFW/App/test/check_no_duplicate_commissioning_impl.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_on_off_trigger_input_producers.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_partition_labels_vs_firmware.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_pid_fuzzy_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_power_diag_flag_mirror_drift.ps1` | NEGATIVE-TESTED | 09-16b | safety_link.h POWER_FLAG_ANY_CHANNEL_CLIPPED 0x02 changed to 0x04 | RED, names both values; hand-restored; PASS |
| `firmware/KilnFW/App/test/check_profile_executor_wd_input_producers.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_ramp_lock_decision_mirror_drift.ps1` | NEGATIVE-TESTED | 09-16b | fabsf() wrapped around the ramp-lock subtraction; also > changed to >= | RED (fail-closed extraction error, not a semantic diff); hand-restored; PASS |
| `firmware/KilnFW/App/test/check_ramp_stepping_gate_mirror_drift.ps1` | NEGATIVE-TESTED | 09-16b | ramp step sign + flipped to - in profile_executor.c | RED, semantic diff shown; hand-restored; PASS |
| `firmware/KilnFW/App/test/check_readiness_ct_channel_map_mirror_drift.ps1` | NEGATIVE-TESTED | 09-16b | readiness_http.h ct_topology test == 0u flipped to != 0u | RED, two disagreeing truth-table rows named; hand-restored; PASS |
| `firmware/KilnFW/App/test/check_readiness_gate_display_agreement.ps1` | REVIEWED, NOT MUTATED | 09-18 | screen A: Python check signals failure as return 1 with sys.exit(main()) | failure path present; no mutation |
| `firmware/KilnFW/App/test/check_safety_cfg_param_table_mirror_drift.ps1` | NEGATIVE-TESTED | 09-16b | abs_max_temp_c declared type F32 changed to U16 in safety_cfg_store.c | RED, type mismatch named; hand-restored; PASS |
| `firmware/KilnFW/App/test/check_safety_fault_hold_mirror_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_safety_link_status_producers.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_sdkconfig_defaults_applied.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_setup_wizard_step_count_mirror_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_setup_wizard_zones_post_helper.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_sim_iter_tune_bars.ps1` | REVIEWED, NOT MUTATED | 09-18 | left alone as settled (A8 cross-profile bar deliberately outside its exit code) | no mutation |
| `firmware/KilnFW/App/test/check_sim_scenarios.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_source_path_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_stop_bar_body_padding.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_stub_signature_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_system_uart_bridge_stack_budget.ps1` | NEGATIVE-TESTED | 09-16c | 512 B volatile local in cfg_fs.c sweep_tmp, real rebuild | +512 B exactly, RED 2448 B > 1936 B; fullclean rebuild back to baseline |
| `firmware/KilnFW/App/test/check_thermal_guard_cfg_producers.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_thermal_guard_input_producers.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_uart_log_bridge_stack_budget.ps1` | NEGATIVE-TESTED | 09-16c | 256 B volatile local in uart_protocol.c frame_and_send, real rebuild | +256 B exactly, RED 1456 B > 1200 B; fullclean rebuild back to baseline |
| `firmware/KilnFW/App/test/check_ui_budget_asserts.ps1` | NEGATIVE-TESTED | 09-16f | _Static_assert in ui_page_temperature.c wrapped in a comment | RED, names the missing assertion; hand-restored; PASS |
| `firmware/KilnFW/App/test/check_ui_relay_reset_removed.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_ui_responsive_sweep.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_ui_shell_layout.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_ui_status_color.ps1` | NEGATIVE-TESTED | 09-16f | --ok colour in main_page.html changed to a low-contrast value | RED below 3:1 floor; hand-restored; PASS |
| `firmware/KilnFW/App/test/check_ui_test_click_result_mirror_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_wire_protocol_fingerprint.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/test_check_config_migration_steps.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/test_check_hal_include_boundary.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/test_check_lcd_home_nav_gated.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/test_check_route_tier_coverage.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/test_check_stop_path_requires_pin.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/test_check_ui_responsive_sweep.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/test_sdkconfig_sibling_pair_guard.py` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/test_stack_budget_symbol_bounds.py` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW_recovery/main/check_recovery_apply.ps1` | NOT AUDITED | - | none (added 2026-10-05, after the 10-02 pass; pass 12 pending) | NOT AUDITED (pass 12 pending) |
| `firmware/KilnFW_recovery/main/check_recovery_boot_verify.ps1` | NOT AUDITED | - | none (added 2026-10-04, after the 10-02 pass; pass 12 pending) | NOT AUDITED (pass 12 pending) |
| `firmware/KilnFW_recovery/main/check_recovery_health.ps1` | NOT AUDITED | - | none (added 2026-10-03, after the 10-02 pass; pass 12 pending) | NOT AUDITED (pass 12 pending) |
| `firmware/KilnFW_recovery/main/check_recovery_hold.ps1` | NOT AUDITED | - | none (added 2026-10-02, after the 10-02 pass; pass 12 pending) | NOT AUDITED (pass 12 pending) |
| `firmware/KilnFW_recovery/main/check_recovery_image_check.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW_recovery/main/check_recovery_lcd_policy.ps1` | NOT AUDITED | - | none (added 2026-10-03, after the 10-02 pass; pass 12 pending) | NOT AUDITED (pass 12 pending) |
| `firmware/KilnFW_recovery/main/check_recovery_page_crc.ps1` | NEGATIVE-TESTED | 10-07 | three real-source mutants on top of the built-in battery | all RED for the right reason; restored |
| `firmware/KilnFW_recovery/main/check_recovery_passphrase.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW_recovery/main/check_recovery_pico_proto.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW_recovery/main/check_recovery_upload.ps1` | NOT AUDITED | - | none (added 2026-10-02, after the 10-02 pass; pass 12 pending) | NOT AUDITED (pass 12 pending) |
| `firmware/KilnFW_recovery/main/check_recovery_wifi_policy.ps1` | NOT AUDITED | - | none (added 2026-10-02, after the 10-02 pass; pass 12 pending) | NOT AUDITED (pass 12 pending) |
| `firmware/SaftyFW/test/check_00_saftyfw_host_tests.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/SaftyFW/test/check_00_saftyfw_target_build.ps1` | NEGATIVE-TESTED | 09-17 | garbage top-level token inserted in link_frame.c | RED, ninja errors, exit 1; hand-restored; deleted build/ and rebuilt PASS 477/477 |
| `firmware/SaftyFW/test/check_01_saftyfw_pushed_build.ps1` | REVIEWED, NOT MUTATED | 09-18 | cannot be driven locally (builds origin/main) | not exercised; a deliberate FAIL would need a broken commit pushed to origin/main |
| `firmware/SaftyFW/test/check_no_sim_plant_guard_disable.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/SaftyFW/test/check_saftyfw_task_count.ps1` | REVIEWED, NOT MUTATED | 09-18 | wrapper reference changed to prove the orphan guard (the check itself was not mutated) | its .py has a return-1 failure path (screen A) |
| `firmware/SaftyFW/test/check_saftyfw_task_stack_budgets.ps1` | NEGATIVE-TESTED | 09-16c | 128 B volatile local in log_task_fn, from-scratch ARM rebuild | +128 B exactly, RED 600 B > 472 B; rebuilt from deleted build/ to 472 B |
| `firmware/SaftyFW/test/test_regsp_margin_against_declared.py` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/SaftyFW/test/test_regsp_stale_literal.py` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/SaftyFW/tools/check_bootloader_builds.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/SaftyFW/tools/check_guard_input_producers.ps1` | NEGATIVE-TESTED | 09-16c | tc_valid initializer line commented out in safety_core_build_input | RED, field named as having no producer; hand-restored; PASS |
| `firmware/SaftyFW/tools/check_isolation.ps1` | NEGATIVE-TESTED | 09-16c | (a) link_task.h include in safety_core.c; (b) GPIO6 reference in link_task.c | both RED naming file and line; hand-restored; PASS |
| `firmware/SaftyFW/tools/check_link_impl_isolation.ps1` | NEGATIVE-TESTED | 09-16c | CRC-named function added to safety_core.c | RED; hand-restored; PASS |
| `firmware/SaftyFW/tools/check_thermo_snapshot_producers.ps1` | NEGATIVE-TESTED | 09-16c | all three production assignments of cj_valid commented out in thermo_task.c | RED, field named; hand-restored; PASS |
| `firmware/SaftyFW/tools/check_unused_setters.ps1` | NEGATIVE-TESTED | 09-16c | uncalled zz_audit setter declared and defined in current_sense | RED, no call site named; both files hand-restored; PASS |
| `tools/check_app_image_size.ps1` | NEGATIVE-TESTED | 10-07 | image 1 B over the bound; app row shrunk below the image / grown past 0x400000; missing CSV; missing or duplicate app row | each RED with the right message; no image yet is SKIP / SKIP-FAST, so it grades nothing until build/KilnCtrl.bin exists |
| `tools/check_aux_relay_conflict_sites.ps1` | NEGATIVE-TESTED | 10-07 | aux_conflict_mask call deleted from zones_post_apply (ordinary POST /api/zones commit); other sites, backup import, unlisted-site scan | first mutation PASSED (file-level match satisfied by the restore function): WEAK, fixed by pinning the call inside zones_post_apply; now RED; restored |
| `tools/check_bench_test_registry.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_bridge_reject_reason.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_c_files_in_cmakelists.ps1` | NEGATIVE-TESTED | 09-16d, 09-16g | [09-16d] MAX31856.c entry removed from the drivers CMakeLists ; [09-16g] untracked unwired drivers/zz_audit_unwired.c created | [09-16d] RED; hand-restored; PASS (re-tested in 09-16g) ; [09-16g] RED; file removed; PASS |
| `tools/check_ceiling_sync_init_order.ps1` | NEGATIVE-TESTED | 10-02 | (a) real ceiling_sync_init call deleted; (b) moved after safety_link_start in main_control_bringup.c | both RED (first try renamed a commented-out call and falsely passed: a bad mutation, not a weakness); restored |
| `tools/check_cfgfs_never_gates_nvs.ps1` | REVIEWED, NOT MUTATED | 09-18 | screen A: Python check signals failure as return 1 with sys.exit(main()) | failure path present; no mutation |
| `tools/check_coil_power_w_sentinel_guard.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_config_convert_mirror.ps1` | NEGATIVE-TESTED | 10-02 | version constant changed in zones_config_json.h and profiles_types.h | RED names the constant; restored |
| `tools/check_config_migration_steps.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_ct_cal_write_surface.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_disclosure_gate_call_sites.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_doc_citations.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_doc_hash_citations.ps1` | REVIEWED, NOT MUTATED | 09-16 | read in full, not sabotaged (a planted bad hash would trip the audit itself) | judged sound; 09-16d later fixed 11 false positives in a doc, not the checker |
| `tools/check_check_cache.ps1` | NEGATIVE-TESTED | check-cache (10-08) | per assertion group: dirty-tree test disabled; tree hash dropped from key and entry check; fingerprint env lines dropped; marker check removed (store and lookup); non-PASS filter removed (store and lookup); expiry window widened; atomic first-writer-wins Move replaced by overwrite | each RED naming the right assertion(s); restored by hand; PASS |
| `tools/check_duplicate_symbols.ps1` | NEGATIVE-TESTED | 09-16e, 09-16f, 09-16g | [09-16e] baseline only against a fresh build (263 objects, none duplicated) ; [09-16f] same external symbol appended to screen_idle.c and telemetry_log.c, real build (link fails, objects inspected) ; [09-16g] duplicate symbol in hal_status.c and hal_esp_common.c after fixing the component source-root map (264 to 265 objects) | [09-16e] FAIL path not exercised in this pass ; [09-16f] RED naming the symbol; hand-restored; fullclean rebuild PASS 263 objects; found hal_status.c.obj misclassified stale ; [09-16g] RED naming both objects (run in the isolated build dir); hand-restored; rebuild PASS |
| `tools/check_flash_partition_offset_guard.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_gate_negative_test_table.ps1` | NEGATIVE-TESTED | table (10-07) | one gate row removed from the table; a stray row for a nonexistent check added | RED naming the missing / stale row; both restored by hand; PASS |
| `tools/check_hal_include_boundary.ps1` | REVIEWED, NOT MUTATED | 09-18 | screen B read: hard throw on missing dir, 200-file floor, throw on unreadable file | cited as the strongest anti-vacuity model; no mutation |
| `tools/check_heartbeat_contract.ps1` | NEGATIVE-TESTED | 09-16d, 09-16g | [09-16d] heartbeat thread start call commented out in link_hub.py ; [09-16g] heartbeat producer start commented out | [09-16d] RED; hand-restored; PASS (re-tested in 09-16g) ; [09-16g] RED; hand-restored; PASS |
| `tools/check_heat_enable_wiring.ps1` | NEGATIVE-TESTED | 09-16d, 09-16g | [09-16d] both heat_enable_acquire_since call sites commented out ; [09-16g] three heat_enable_release calls commented out | [09-16d] RED; both hand-restored; PASS (re-tested in 09-16g, release side) ; [09-16g] RED; hand-restored; PASS |
| `tools/check_host_embed_symbols_defined.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_html_escape_helpers.ps1` | NEGATIVE-TESTED | 10-02 | violating construct added to app.js | RED on the right rule; restored |
| `tools/check_iter_tune_write_surface.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_kiln_auth_config_isolation.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_kiln_scope_cfg_mirrors.ps1` | NEGATIVE-TESTED | 10-07 | scope-list vs cfg-mirror drift in kiln_scope_cfg_files.c, unit_pref.[ch], kiln_package.c, cfg_fs.c | each RED, names the item; restored |
| `tools/check_lcd_home_nav_gated.ps1` | NEGATIVE-TESTED | 10-02 | nav ungated in ui_page_home_actions.c | RED; restored |
| `tools/check_lint_pages.ps1` | NEGATIVE-TESTED | 10-02 | violating construct added to app.js / nav.js | RED on the right rule; restored |
| `tools/check_mcp_facade_coverage.ps1` | NEGATIVE-TESTED | 09-16e, 09-16f | [09-16e] uncovered @_srv._tool() negtest function added to mcp_server_io.py; the documented plant_sim_compare example found stale ; [09-16f] ramp_assist_set_enabled KEYWORDS entry removed (replaces the stale documented example) | [09-16e] RED naming the tool; hand-restored; docstring example fixed in 09-16f ; [09-16f] RED naming the tool; hand-restored; PASS |
| `tools/check_mcp_tool_count_doc.ps1` | NEGATIVE-TESTED | 09-16e | CLAUDE.md edited to claim 999 tools | RED, doc vs actual count; hand-restored; PASS |
| `tools/check_monocypher_vendored.ps1` | NEGATIVE-TESTED | 10-07 | byte flip in a vendored file; README hash edited; README row dropped; directory deleted; extra unhashed extra.c | all RED except extra.c, which PASSED: WEAK, fixed (unhashed .c/.h/.S fails); extra.c now RED (re-run by the opus review) |
| `tools/check_mykicad_golden_suite_runs.ps1` | NEGATIVE-TESTED | 09-16e | submodule conftest pointed at a nonexistent kiln project dir | RED (53 passed, 19 errored); hand-restored; 72 passed |
| `tools/check_no_bench_text_in_ui.ps1` | NEGATIVE-TESTED | 10-02 | bench-wattage text added to a page title | RED naming the line; throws on a missing scan dir; restored |
| `tools/check_no_doubled_apostrophes.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_no_duplicate_crc.ps1` | NEGATIVE-TESTED | 09-16d, 09-16g | [09-16d] working 0x1021 CRC-16 function appended to wifi_provision_http.c ; [09-16g] tracked file with 0x1021 polynomial staged | [09-16d] RED; hand-restored; PASS (re-tested in 09-16g) ; [09-16g] RED; unstaged and removed; PASS |
| `tools/check_no_exec_status_stack_locals.ps1` | NEGATIVE-TESTED | 10-02 | profile_exec_status_t automatic variable added in dashboard_exec_http.c and ui_page_home_actions.c | RED naming file and line; also found vacuous on a missing scan dir and fixed (hard fail + 150-file floor) |
| `tools/check_no_handler_direct_driver_calls.ps1` | NEGATIVE-TESTED | 10-02 | direct driver call inserted in dashboard_http.c and wifi_prov.c | RED naming the call; also found vacuous on a missing scan dir and fixed (hard fail + 30-file floor) |
| `tools/check_no_native_dialogs_in_ui.ps1` | NEGATIVE-TESTED | 10-02 | violating construct added to app.js / nav.js | RED on the right rule; restored |
| `tools/check_no_orphaned_checks.ps1` | NEGATIVE-TESTED | 09-16, 09-18 | [09-16] orphan test_zz_audit_orphan.py created (a bare check_*.ps1 is covered by the glob, a non-test) ; [09-18] wrapper rule and raw-text fallback both absolved a check wired only by a comment; sabotaged the one non-comment reference in check_saftyfw_task_count.ps1 | [09-16] RED; file deleted ; [09-18] pre-fix PASS (vacuous), one-fix PASS, both fixes RED; hand-restored; guard fixed |
| `tools/check_nvs_key_length.ps1` | NEGATIVE-TESTED | 09-16 | 17-character NVS key literal added to adaptive_tune.h | RED, names file, line, literal; hand-restored; PASS |
| `tools/check_nvs_write_guard_coverage.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_ota_esp_refuses_running_target.ps1` | NEGATIVE-TESTED | 10-07 | guard kept as text but defeated: if (0 && !...), negation dropped, goto cleanup removed | PASSED (call located by position only): WEAK, fixed by matching the whole refusing if-block; all three now RED (re-run by the opus review); Pico mutations RED as before |
| `tools/check_page_js_tests.ps1` | NEGATIVE-TESTED | 10-02 | delete-count filter loosened in backup_page.html | RED naming test_backup_page.js (36/37); restored |
| `tools/check_persist_scratch_malloc_caps.ps1` | NEGATIVE-TESTED | 10-07 | plain malloc in zones_http_post.c, zones_config_store.c, zone_aux_convert_http.c; an adopter file unlisted | PASSED (files not in the scan list): WEAK, fixed by listing them plus an adoption guard (floor 8); all RED (re-run by the opus review); two real plain mallocs converted |
| `tools/check_pico_update_mutex_balance.ps1` | NEGATIVE-TESTED | 10-02 | unbalanced take/give in pico_auto_update_boot.c | RED, imbalance reported; restored byte-identical |
| `tools/check_profiles_capacity.ps1` | NEGATIVE-TESTED | 10-02 | capacity constant changed in profiles_types.h | RED, mirror mismatch; restored |
| `tools/check_python_zero_caller_sweep.ps1` | NEGATIVE-TESTED | 10-02 | uncalled function added to ui_test_client.py | RED, exit 1, names it; restored |
| `tools/check_recovery_image_size.ps1` | REVIEWED, NOT MUTATED | 09-18 | screen A: Python check signals failure as return 1 with sys.exit(main()) | failure path present; no mutation |
| `tools/check_relay_authority_paths.ps1` | NEGATIVE-TESTED | 09-16d, 09-16e, 09-16g | [09-16d] PC side: link.send(devices.io_set_relay(...)) added; the bare-call shape passed clean (rule narrower than its summary) ; [09-16e] rule widened to wrapper-presence; PC bare call, PC .send call, and firmware-side kiln_io_set_relay in profile_executor.c ; [09-16g] kiln_io_set_relay call inserted in dashboard_json.c json_escape | [09-16d] RED for the .send shape; hand-restored; rule widened in 09-16e ; [09-16e] all three RED; hand-restored; also found and fixed a live GUI relay bypass in actions.py ; [09-16g] RED at that line; hand-restored; PASS |
| `tools/check_relay_writes_through_owner.ps1` | NEGATIVE-TESTED | 09-16 | unauthorized set_relay_mask call added in main.c | RED at that line; hand-restored; PASS |
| `tools/check_release_manifest.ps1` | NEGATIVE-TESTED | 10-07 | size gate 0x400000 -> 0x500000; dirty-tree refusal off; sha256 compare off; draft flag off; open gates not refused; provenance refusal off; token forwarded on hop 2; semver gate off | all RED except semver gate off, which PASSED (a later gate also exits 1): WEAK, fixed by requiring the is-not-semver text; now RED |
| `tools/check_release_version_regex.ps1` | NEGATIVE-TESTED | 10-07 | cap 32 -> 33; cap dropped; - dropped from the prerelease charset; leading-zero reject neutralised; + allowed in the prerelease charset; badtag[] edits; table renamed | all RED except the + charset, which PASSED: WEAK (minor), fixed by pinning v1.2.3-rc+1 on both sides; now RED |
| `tools/check_route_tier_coverage.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_safe_remove_junction.ps1` | NEGATIVE-TESTED | 10-07 | unlink skipped in Remove-TreeSafe; no descent in Remove-ReparsePointsUnder; unlink dropped at both call sites; site pattern made blind; an unguarded $r = & git ... worktree remove site | skip-unlink and blind mutants PASSED: WEAK, fixed (unlink report required, command-line site pattern); the $r = & git site PASSED that fix too and is caught after the opus review reordered the pattern; all RED; recursive-delete mutant equivalent on this PowerShell |
| `tools/check_safety_baud_sync.ps1` | NEGATIVE-TESTED | 09-16d | SaftyFW bootloader uart_init baud 230400 changed to 115200 | RED, two rates named; hand-restored; PASS |
| `tools/check_safety_call_results_checked.ps1` | NEGATIVE-TESTED | 09-16 | bare (result-discarding) all-relays-off call in uart_bridge_io.c | RED, names the sabotaged line; hand-restored; PASS |
| `tools/check_safety_trip_mask_docs.ps1` | NEGATIVE-TESTED | 09-16e | wrong, non-negated S6a trip_mask sentence added to CLAUDE.md | RED; hand-restored; PASS |
| `tools/check_safety_trip_words_sync.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_skip_fast_classification.ps1` | NEGATIVE-TESTED | 10-02 | SKIP-FAST literal matched by run_all_checks.ps1 broken | RED, dummy SKIP-FAST counted as a plain skip; restored |
| `tools/check_stack_margin_baseline.ps1` | REVIEWED, NOT MUTATED | 09-18 | screen A: Python check signals failure as return 1 with sys.exit(main()) | failure path present; no mutation |
| `tools/check_stack_margin_registration.ps1` | NEGATIVE-TESTED | 09-16e, 09-16f | [09-16e] create-vs-register sub-check only: unrecognized xTaskCreatePinnedToCore added in kiln_io_owner.c ; [09-16f] four remaining sub-checks: required name renamed, duplicate registration added, cap lowered 48 to 20, stack_margin.h included from a HAL backend | [09-16e] RED; hand-restored; other four sub-checks tested in 09-16f ; [09-16f] all four RED; each hand-restored; PASS |
| `tools/check_stack_task_table_consistency.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_stop_path_requires_pin.ps1` | NEGATIVE-TESTED | 10-02 | stop path ungated in ui_page_home_actions.c | RED; restored |
| `tools/check_test_c_files_wired.ps1` | NEGATIVE-TESTED | 09-16e, 09-16g | [09-16e] untracked orphan test_negtest_orphan_zzz.c created ; [09-16g] untracked zz_audit_orphan.c created | [09-16e] RED; file deleted (re-tested in 09-16g) ; [09-16g] RED; removed; PASS |
| `tools/check_test_has_assertions.ps1` | NEGATIVE-TESTED | 09-16, 09-16e | [09-16] read in full (negative-tested later in 09-16e) ; [09-16e] dispatched assertion-free test function appended to test_adaptive_tune.c | [09-16] see 09-16e ; [09-16e] RED, names the function; hand-restored; fresh host-test rebuild 47/47 |
| `tools/check_uart_version_independence.ps1` | NEGATIVE-TESTED | 09-16d, 09-16g | [09-16d] UART_PROTOCOL_VERSION re-derived from KILNLINK_PROTOCOL_VERSION ; [09-16g] alias to KILNLINK_PROTOCOL_VERSION again | [09-16d] RED; hand-restored; PASS (re-tested in 09-16g) ; [09-16g] RED; hand-restored; PASS |
| `tools/check_uri_handler_cap.ps1` | NEGATIVE-TESTED | 09-16d, 09-16g | [09-16d] max_uri_handlers lowered below the real route count ; [09-16g] max_uri_handlers set to 1 | [09-16d] RED, both counts named; hand-restored; PASS (re-tested in 09-16g) ; [09-16g] RED; hand-restored; PASS |
| `tools/check_volatile_ceiling_write_callers.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_wait_for.ps1` | NEGATIVE-TESTED | 10-08 | UTF-16 detection disabled; -Any made behave as -All; TIMEOUT exit 124 changed to 1; PID-exit never recorded; -File test path broken; incremental carry dropped | each RED (named case FAIL, exit 1); hand-restored; PASS |
| `tools/check_web_gzip_parity.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/check_wifi_ram_storage_mirror.ps1` | NEGATIVE-TESTED | 10-02 | mirrored constant edited in wifi_prov.c | RED; restored |
| `tools/check_zone_graphic_render.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/PcTools/check_zones_per_zone_field_drift.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/PcTools/selfcheck.py` | NOT AUDITED | - | none | NOT AUDITED |
| `tools/PcTools/tests/check_web_commission_cdp_driver.ps1` | NOT AUDITED | - | none | NOT AUDITED |
| `firmware/KilnFW/App/test/check_ui_content_smoke.ps1` | NOT AUDITED | - | none (added after the 10-02 pass; pass 12 pending) | NOT AUDITED (pass 12 pending) |
| `tools/check_build_gate_usage.ps1` | NOT AUDITED | - | none (added 2026-10-07; negative-tested by fixtures at authoring: sleep in pair, test exe in pair, missing Exit, gate before lock each RED) | NOT AUDITED (pass 12 pending) |
| `tools/check_land.ps1` | NOT AUDITED | - | none (author negative-tested each case by hand, not an audit; pass 12 pending) | NOT AUDITED (pass 12 pending) |
| `tools/check_wt_status.ps1` | NOT AUDITED | - | none (added 2026-10-08; negative-tested at authoring against a mutated copy via -ScriptUnderTest: cherry-landed detection disabled, dirty ignored for HAS_WORK, prune widened to HAS_WORK/ACTIVE, -WhatIf bypassed, junction-following delete each RED, then restored) | NOT AUDITED (pass 12 pending) |
| `tools/check_pushed_build_stamp.ps1` | NOT AUDITED | - | none (added after the 10-02 pass; pass 12 pending) | NOT AUDITED (pass 12 pending) |
