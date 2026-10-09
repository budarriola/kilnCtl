# Release-gate vacuity audit, 2026-10-02 (eleventh pass)

Continues `docs/audits/release_gate_vacuity_audit_2026-09-18.md`; BLOCKER 3 of
`docs/RELEASE_HARDENING_PLAN.md` item 3.

Work was done in a dedicated worktree, `C:\wt\vacuity_vp3jmw`, minted at
origin/main. No hardware was touched, no MCP tool called, no `.kicad_*` file
opened, and the recovery checks were excluded as instructed.

## Scope and method

Scope: the `check_*.ps1` scripts and wrapped `*_check.py` tests added or
materially changed on origin/main since 2026-09-18. For each, the smallest
source change that violates exactly the claim was made, only that check was
run (`run_all_checks.ps1 -Fast -AllowFewerChecks -Only <regex>`), the verdict
and reason of the failure were read, and the source was then restored by hand
from a backup and verified byte-identical with `cmp` (`git status` clean
afterwards). No compiled code was mutated by any case below, so no forced
rebuild was needed.

## Results

| Check | Mutation | Result | Verdict |
|---|---|---|---|
| check_no_exec_status_stack_locals | add a `profile_exec_status_t` automatic variable in `dashboard_exec_http.c` and `ui_page_home_actions.c` | FAIL, names file and line | OK on content; FIXED for discovery (below) |
| check_no_handler_direct_driver_calls | direct driver primitive call inserted in `dashboard_http.c` and `wifi_prov.c` | FAIL, names the call | OK on content; FIXED for discovery (below) |
| check_pico_update_mutex_balance | unbalanced take/give in `pico_auto_update_boot.c` | FAIL, imbalance reported | OK |
| check_profiles_capacity | change a capacity constant in `profiles_types.h` | FAIL, mirror mismatch | OK |
| check_config_convert_mirror | change a version constant in `zones_config_json.h` / `profiles_types.h` | FAIL, names the constant | OK |
| check_wifi_ram_storage_mirror | edit the mirrored constant in `wifi_prov.c` | FAIL | OK |
| check_html_escape_helpers / check_no_native_dialogs_in_ui / check_lint_pages | violating construct added to `app.js` / `nav.js` | FAIL on the right rule | OK |
| check_lcd_home_nav_gated / check_stop_path_requires_pin | ungate the nav / stop path in `ui_page_home_actions.c` | FAIL | OK |
| check_ceiling_sync_init_order | (a) delete the real `ceiling_sync_init` call, (b) swap it after `safety_link_start` in `main_control_bringup.c` | (a) FAIL, (b) FAIL "AFTER" | OK. A first attempt that renamed a commented-out call passed falsely; the check strips comments, so that was a bad mutation, not a weakness |
| check_page_js_tests | loosen the delete-count filter in `backup_page.html` | FAIL, `test_backup_page.js` named, 36/37 | OK. Tests load the real page, not a copy |
| check_skip_fast_classification | break the `SKIP-FAST` literal matched in `run_all_checks.ps1` | FAIL: dummy SKIP-FAST counted as a plain skip, run failed | OK |
| check_no_bench_text_in_ui | add "on this ~4 W bench" to a page `<title>` | FAIL, names the line; it also throws if any scan directory is missing | OK |
| check_python_zero_caller_sweep | add an uncalled function in `ui_test_client.py` | FAIL, exit 1, names it | OK |

Not exercised: the three heavy target builds, host-test builds and checks that
read pushed or published build output. The machine-wide four-build cap makes
sabotaging them serially expensive, and none of them changed materially
since the last pass.

## Findings and fixes

Two checks were vacuous on a moved tree. Both scan a hard-coded directory and
silently treated a missing directory as "nothing to scan":

- `tools/check_no_exec_status_stack_locals.py`: `find_c_files` did `continue`
  on a missing `SCAN_DIRS` entry and `main()` printed OK for zero files.
- `tools/check_no_handler_direct_driver_calls.py`: `find_files` returned `[]`
  for a missing `SCAN_DIR` and `main()` printed OK.

Fix, following the discovery-floor model of `check_hal_include_boundary.ps1`:
a missing scan directory is now a hard `SystemExit` failure, and a floor on
files scanned fails the check (150 of 267 for the first, 30 of 59 for the
second). Proof that the fix bites, using `--root`:

| Root | Before | After |
|---|---|---|
| real tree | exit 0 | exit 0 (unchanged) |
| empty directory | exit 0, "OK" | exit 1, "scan directory ... is missing" |
| scan directory present but empty | exit 0, "OK" | exit 1, "scanned only 0 .c file(s), floor is N" |

## Status

No WEAK verdicts remain open from this pass. The earlier-pass population
(113-plus checks) was not re-audited.
