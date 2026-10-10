# Release gate vacuity audit, firmware side, 2026-10-08

Status note (ported to dev 2026-10-09 from an abandoned worktree): this is the point-in-time pass. Dev's
GATE_NEGATIVE_TEST_EVIDENCE.md has since moved on (all 193 rows audited; the MISSED rows below were hardened and re-negative-tested, see the tools 10-08 audit). Dev's table is authoritative; do not copy this file's per-row results over it.

Scope: `docs/RELEASE_HARDENING_PLAN.md` section 3 step 4, firmware-side rows of
`GATE_NEGATIVE_TEST_EVIDENCE.md` (76 rows; UI checks, PcTools and tooling rows excluded, covered elsewhere).
Method: `tools/negtest.ps1` mutation in a throwaway worktree, restore by negtest.

Result: 51 rows CAUGHT (NEGATIVE-TESTED), 10 MISSED (vacuous, PARTIAL, fixes landing separately),
12 NOT VERIFIED (timeouts, skips, heavy builds not run). Remaining rows of the 76 were covered in the same pass and appear in the table.

## Vacuous checks (fixes in progress by separate agents)
- check_recovery_apply: test never injects sha_finish/sha_update failure.
- test_check_lcd_home_nav_gated: `-not $direct` never exercised.
- check_flash_partition_offset_guard: `if partition_mismatch` removal not caught.
- check_nvs_write_guard_coverage: `caller_stack_is_external()` guard removal not caught.
- check_kiln_auth_config_isolation: scans *.c only, header leak missed.
- check_ota_esp_refuses_running_target: dropping `target != running` not caught.
- check_disclosure_gate_call_sites: gate function forced true not caught (call-site mutation was).
- check_on_off_trigger_input_producers, check_thermal_guard_cfg_producers: field assignment masked by unrelated drivers/*.c assignments.
- check_pid_fuzzy_drift: abs tolerance 1e-4 hides a 7.5e-5 ki delta (partial).
- Minor: check_persist_scratch_malloc_caps scans a hardcoded file list.

## Not verified
check_commonfw_ctest (timeout, needs ~40 min quiet), check_commonfw_diag_vectors, test_host_fakes (baseline timeout under load),
compile_esp/pico_backends, check_all_task_stack_budgets, check_embedded_pico_image_fresh, check_kilnfw_dram_bss_budget (SKIP, no build artifacts),
the three check_00 KilnFW host/target builds (heavy, not run), check_kilnfw_ccache_no_stale (runtime proof, no mutation designed).
