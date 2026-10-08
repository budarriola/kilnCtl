# Release gate vacuity audit: repo tooling checks, 2026-10-08

Scope: `docs/RELEASE_HARDENING_PLAN.md` section 3 step 4, the non-firmware tools rows.
Method: `tools/negtest.ps1` applied the stated mutation in a throwaway worktree (baseline
required green, real tree untouched). Every mutation went RED; no vacuous check was found.
Three first-try mutations were wrong (word-boundary or wrong-file guards, GREEN by design)
and were redone correctly; those are not check defects.

Not covered here: `tools/PcTools/check_zones_per_zone_field_drift.ps1`, `tools/PcTools/selfcheck.py`,
`tools/PcTools/tests/check_web_commission_cdp_driver.ps1`, and the `firmware/KilnFW/App/test` web-lint rows.

| Check | Mutation | Result |
|---|---|---|
| `tools/check_bench_test_registry.ps1` | registry.py: added bogus case id ZZ-77 absent from the plan doc | RED (exit 1) |
| `tools/check_bridge_reject_reason.ps1` | uart_bridge.c: bridge_reply_unsupported passes NULL reason | RED (BRIDGE REJECT REASON CHECK FAILED) |
| `tools/check_coil_power_w_sentinel_guard.ps1` | zones_current_sweep_engine.c: sentinel guard `> 0.0f` weakened to `>= 0.0f` | RED (SENTINEL GUARD CHECK: FAILED) |
| `tools/check_ct_cal_write_surface.ps1` | safety.py: reintroduced `def set_ct_cal(` | RED (CT_CAL WRITE SURFACE CHECK: FAILED) |
| `tools/check_config_migration_steps.ps1` | zones_config_json.h: ZONES_CFG_VERSION 26 -> 27 with no step | RED (3 problems) |
| `tools/check_disclosure_gate_call_sites.ps1` | wifi_provision_http.c and readiness_http.c: `may_disclose = true` (two mutations) | RED both |
| `tools/check_doc_citations.ps1` | RELEASE_HARDENING_PLAN.md: added citation zones_config_json.h:999999 (past EOF) | RED (exit 1) |
| `tools/check_flash_partition_offset_guard.ps1` | mcp_server_flash.py: guard call result assignment replaced by None | RED (call site gone) |
| `tools/check_host_embed_symbols_defined.ps1` | test_zones_http.c: removed tuning_recommendations_json_start definition | RED (HOST EMBED SYMBOLS CHECK FAILED) |
| `tools/check_iter_tune_write_surface.ps1` | profile_executor.c calls iter_tune_enable; iter_tune.c calls nvs_set_blob (two mutations; a first try with undeclared iter_tune_reset stayed GREEN by design, name list is parsed from the header) | RED both |
| `tools/check_kiln_auth_config_isolation.ps1` | profile_executor.c: added identifier `web_auth` (a first try with web_auth_zz is GREEN by design, word-boundary match) | RED |
| `tools/check_no_doubled_apostrophes.ps1` | RELEASE_HARDENING_PLAN.md: added `doesn''t` | RED (exit 1) |
| `tools/check_nvs_write_guard_coverage.ps1` | kiln_cfg_store.c quarantine_clear: guard `if (caller_stack_is_external())` -> `if (0)` (a first try in relay_cycles.c persist_snapshot was GREEN: that function writes via cfg-fs, not NVS) | RED |
| `tools/check_route_tier_coverage.ps1` | profile_executor.c: added `.uri = "/api/zz_new_route"` with no tier row | RED |
| `tools/check_safety_trip_words_sync.ps1` | safety_page.html: S4 warn word text changed in the JS mirror | RED |
| `tools/check_stack_task_table_consistency.ps1` | removed backlight_pwm from $requiredNames; added ghost CEILING_BYTES entry (two mutations) | RED both |
| `tools/check_volatile_ceiling_write_callers.ps1` | profile_executor.c: added a call to safety_cfg_write_set_and_confirm_f32_volatile | RED |
| `tools/check_web_gzip_parity.ps1` | fabricated .gz from HEAD sources, then changed zones_page.html relative to its gz | RED (exit 1, mismatch) |
| `tools/check_zone_graphic_render.ps1` | zones_page.html: renamed kg-port; moved zone label x 345 -> 346 (two mutations) | RED both |
| `tools/check_build_gate_usage.ps1` | check_commonfw_ctest.ps1: Start-Sleep inserted between gate enter and exit | RED |
| `tools/check_pushed_build_stamp.ps1` | pushed_build_stamp.ps1: sha mismatch test replaced by $false | RED |
| `tools/check_main_baseline.ps1` | main_baseline_lib.ps1: other-lineage ancestor rule disabled | RED |
| `tools/check_land.ps1` | land.ps1: post-rebase check failure no longer finishes non-zero | RED |
| `tools/check_wt_status.ps1` | wt_status.ps1: HAS_WORK classification disabled | RED |
| `tools/check_negtest.ps1` | negtest.ps1: MISSED verdict never counted | RED |
| `tools/check_agent_tail.ps1` | agent_tail.ps1: dead-log heuristic off; repeat heuristic off (two mutations) | RED both |
