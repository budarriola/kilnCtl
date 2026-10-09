# Release-gate vacuity audit, remaining rows (2026-10-08)

Method: tools/negtest.ps1 (throwaway copy, baseline must PASS) with one mutation of the guarded source per check, or a hand test with fabricated inputs where the check needs build artifacts. Restored by hand; the worktree was clean after every hand test.

Result: 60 rows audited, no vacuous check found, no check changed. 52 gates RED on their mutation; every row is in GATE_NEGATIVE_TEST_EVIDENCE.md (audit tag `rest-10-08`).

Notes:
- First attempts on test_check_lcd_home_nav_gated and test_check_stop_path_requires_pin were MISSED because the mutation sat in the check's main body, which the selftest never runs (it calls the scan function). Redone inside the function: CAUGHT. Bad mutation, not a vacuous check.
- test_check_config_migration_steps: first mutation matched 4 sites; redone with more context.
- compile_esp_backends: the include list comes from the main tree's compile_commands.json, so a header edit inside a worktree is shadowed by main-tree -I paths (the backend-source mutation was caught). Run it from the tree whose headers you changed.
- check_all_task_stack_budgets: the only ELF available (main tree, 2026-09-25) is stale, so the baseline FAILs loud on unresolved roots and on the sibling-sdkconfig mismatch. No PASS baseline, so PARTIAL.
- compile_pico_backends needs SaftyFW/build/compile_commands.json, absent: PARTIAL.
- check_embedded_pico_image_fresh: fabricated bins prove the identical, unequal-length and no-record FAIL paths and the missing-bin SKIP; the matching-record PASS path was not exercised.
- Hand-tested (fabricated inputs): check_no_sim_plant_guard_disable, check_kilnfw_dram_bss_budget, check_sdkconfig_defaults_applied.
- Not mutated: check_00_* aggregators and target builds (heavy), check_bootloader_builds (full pico build), test_host_fakes (external negtest baseline did not finish under machine load; the script has its own in-script negative tests).
- Out of scope (other agents): check_ui_*, check_js_host_tests, check_web_commission_cdp_driver, check_zones_per_zone_field_drift, selfcheck.py, CommonFW rows.
