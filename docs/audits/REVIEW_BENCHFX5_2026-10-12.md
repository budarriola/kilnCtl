# Review: benchfx5 (`315b4ff5d`), 2026-10-12

Scope: origin/dev `315b4ff5d` ("Review benchfx4 MED-1/LOW-1: distinct guard seeds, exact same-field omit
match in drift test, HEATER hyst_c host test, fix status"). Files: `firmware/KilnFW/App/test/test_zones_http.c`,
`tools/PcTools/tests/test_zone_omit_preserved_drift.py` and `docs/audits/REVIEW_BENCHFX4_2026-10-12.md`.
Reviewed in worktree `C:\wt\rvbfx5_qclk35` at `315b4ff5d`, against
`firmware/KilnFW/App/drivers/http/zones_http_post_parse.c` and `zones_http_post.c`. No board was touched. No
code was edited.

Verdict: the commit closes BENCHFX4 MED-1 and LOW-1 as written. The two mutations behind them (M2, M4) are
CAUGHT on both layers, and the earlier-unrun M6, M7, M8, M9 (as M9b), N9b, N6 and the adapted M11a/M11b are CAUGHT too. No
HIGH. One MED: five omit-preserved float fields have no host-test omit coverage at all, so their only guard is
the static drift test. Three plausible edit shapes (an `else if` default, a compound assignment, an override in
a later block) pass that test, and they pass the host tests for those five fields too. The commit changes
tests and a doc only, so firmware behaviour cannot regress.

## Verification

Four `tools\negtest.ps1` runs, all on base `315b4ff5d`. Every baseline passed and every copy was removed. Runs P, F and F2
report `real_tree_unchanged: true`. Run F3 reports `real_tree_unchanged: false` (verdict ERROR) only because I
wrote this review file into the worktree while it ran. `git status` shows no other change, so X4b's CAUGHT
verdict stands. Mutation JSON is in the appendix.

**Run P, `-Preset pytest`**, using the `tools\PcTools\.venv` python of the main tree with the copy's `src` on
`PYTHONPATH`. It applied 14 mutations over 13 test files (400 tests): `test_zone_omit_preserved_drift.py`,
`test_bench_test_cases_heat.py`, `test_mcp_server_control_{coupling,model_fields,relay_type,zone_coupling,zone_limits,zone_relay_mask,zone_type}.py`,
`test_pctools_write_review_2026_10_09.py`, `test_zones_http_client.py`, `test_config_presets.py` and
`test_zones_http_float_tolerance_pin.py`. The baseline passed (400 passed).

**Run F, KilnFW host tests.** It used `build_host_tests.ps1 -OutDir {OUT} -Only "zones"` (builds the one
`test_zones_http.c` executable) with
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"` and `-Parallel 2`, over 9 firmware
mutations. The baseline passed in 61.8 s.

**Runs F2 and F3** each re-ran one firmware mutation that run F had measured wrongly:

- **M9b, run F2:** M9's replacement `'\0'` lost its escape in the JSON, so the mutant did not compile, and the
  `BUILD FAILURES` alternative scored it CAUGHT (see INFO-2).
- **X4b, run F3:** X4's default value `3u` equals the test fixture's seeded `tc_type` (3), so X4 could not fail
  any test.

| # | Mutation | Drift/pytest (P) | Host tests (F) | Net |
|---|---|---|---|---|
| M6 | fw: coupling cell omit = `0.0f` | CAUGHT (drift x2) | CAUGHT (`test_zones_http.c:6961`, 6963) | caught |
| M7 | fw: `settings_source` per-group omit = `CUSTOM` | CAUGHT (drift x2, gkey-else test) | CAUGHT (6966, 7158-7164) | caught |
| M8 | fw: `thermo_mask` omit becomes preserved | CAUGHT (drift x2) | CAUGHT (1530) | caught |
| M9 | fw: `name` omit blanks the name (`'\0'` lost, does not compile) | MISSED | "CAUGHT" by `BUILD FAILURES` only | invalid, see M9b |
| M9b | fw: `name` omit: `z->name[0] = 0;` in place of the `strncpy` | (same source shape as M9: MISSED) | CAUGHT (`test_zones_http.c:1278`) | caught |
| X1 | fw: `approachratecap`: `else if (current_z->zone_type == 0u) { = 0.0f; }` before the preserving `else` | MISSED | MISSED | **slips** (MED-1) |
| X2 | fw: `z->error_band_c = 0.0f;` after the coupling `memcpy` (a later block) | MISSED | MISSED | **slips** (MED-1) |
| X3 | fw: `coilpower`: `} { z->coil_power_w *= 0.0f;` after the preserving assignment | MISSED | MISSED | **slips** (MED-1) |
| X4 | fw: `tc_type = 3u` on omit, before `if (i >= thermo_count)` (exempt field) | MISSED | MISSED (value equals fixture seed) | invalid, see X4b |
| X4b | fw: as X4 with `5u` | (same source shape as X4: MISSED) | CAUGHT (`test_zones_http.c:1279`) | caught |
| X5 | fw: `settings_source[0] = CUSTOM` when the legacy key is absent, after the gkey loop (exempt field) | MISSED | CAUGHT (6966, 7089, 7158) | caught |
| N9b | py: `ct_mask` added to the strip regex only | CAUGHT (3 tests) | n/a | caught |
| M10 | py: `ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS` emptied | MISSED | n/a | equivalent (INFO-1) |
| N6 | py: `hyst_c` read-back tolerance back to 0.001 | CAUGHT (`test_readback_float_tolerance_pinned`) | n/a | caught |
| M11a | py: `_float_tolerance` fixed-format half step to full step | CAUGHT (3 tests) | n/a | caught |
| M11b | py: `_FLOAT_EPS_REL` 1e-6 to 1e-3 | CAUGHT (2 tests) | n/a | caught |

"drift x2" means both `test_python_lists_match_firmware` and
`test_defaulted_keys_match_firmware_and_are_never_stripped` failed.

M11 was adapted. BENCHFX4's M11 widened a `tol` default parameter of `_float_close`, but LOW-2's fix
(`f230b49a4`) removed that parameter, so the old find string no longer exists. M11a and M11b mutate the two
constants that replaced it.

N9b, M10 and N6 are Python mutations, so run F does not apply to them. X1-X5 were added by this review. They are
the parser-fooling shapes from Q3, applied to real fields.

## Answers to the review questions

1. **Does the test now cover every omit-preserved field?**

   *The static drift test:* yes. `_classify_full` finds every presence-checked key in `zones_http_post_parse.c`
   (the `test_python_lists_match_firmware` set equals the Python lists). It excludes `name` by design (third
   idiom, not checked) and the keyless carry-throughs (`tuning_*`, `model_fit_*`, `autotune_baseline_k_dc`,
   `adaptive_tune_enabled`, and the unconditional `memcpy` of `coupling_tau_s`/`coupling_dead_time_s`).

   *The host tests:* no. They assert omit-preservation through `zones_http_parse_zone_fields()` for:
   - the nine guard fields, `hyst_c` (both zone types), coupling cells, `settings_source`, `fuzzy_strength_pct`,
     `coupling_diag_k_dc`, `ease_off_window_mult`, the model triple, `relay_type`, `zone_type`, `failsafe_state`,
     `min_on_s`/`min_off_s`, `name` and `tc_type`.

   They have no POST-omit assertion for five fields: `approach_rate_cap_c_per_hr`, `error_band_c`,
   `rate_band_c_per_s`, `progress_band_c` and `coil_power_w`. Their only `TEST_CHECK`s are migration-path
   (`out_cfg`) or accessor tests. See MED-1.

   *Outside the drift test entirely:* the handler-level top-level keys in `zones_http_post.c`. These are
   `pc_link_abort_silence_ms` (blank or omitted keeps `s_zones.cfg`), `relay<N>_name` and `relay<N>_type`. Host
   tests cover all three (`pc_link`, `relay1_type`, and `test_zones_post_relay_name_omitted_preserves_current_value`).
   See INFO-3.

2. **Are the `settings_source` and `tc_type` exemptions correct?**

   Both are needed. Each field has a legitimate later same-field assignment in the same block:
   - `z->tc_type = preserved_tc_type;` inside the `i >= thermo_count` early-return branch;
   - `z->settings_source[g] = src_raw;` in the gkey loop, after the legacy-key parse.

   The exemptions are coarser than they need to be. `_LATER_ASSIGN_EXEMPT` drops later-assignment checking for
   the whole field, not just the one known assignment. X4 and X5 (an omit default inserted after the preserving
   `else` on each exempt field) both pass the drift test. The host tests catch X5. They catch X4b (`tc_type`) too, so
   neither exemption masks a drift today (see LOW-1). A narrower exemption would match the known RHS
   (`preserved_tc_type`, `src_raw`) instead of the field name.

3. **Can the regex or parser be fooled?**

   Yes, by plausible edits. I probed `_classify_full` with synthetic blocks (script in the review scratchpad, not
   committed).

   These shapes classify as preserved with no violation:
   - an `else if (<cond>) { z->f = 0.0f; } else { z->f = current_z->f; }` chain, which is the BENCHFX4 LOW-1
     conditional default rewritten as `else if`, and also `else if (1)`;
   - a compound `z->f *= 0.0f;` after the `else`;
   - `memset(&z->f, ...)` after the `else`;
   - a pointer alias (`float *p = &z->f; *p = 0;`);
   - an override hidden behind a `/*` inside a string literal (the comment stripper is not string-aware);
   - an override through a different index expression (`z->f[j + 0] = 0`);
   - an override placed in a *later* key's block, because the later-assignment scan is block-local.

   These shapes fail loud (classified defaulted, so the drift lists mismatch):
   - a brace-less `else`;
   - a `PRESERVE(f)` macro;
   - two statements in the `else`;
   - a dead `if (0) {}` in the `else`.

   A K&R next-line brace is handled correctly. X1, X2 and X3 confirm the `else if`, cross-block and compound
   shapes on real fields.

4. **Unrun mutations:** M6, M7, M8, N9b, N6, M11a and M11b are CAUGHT. M9b is CAUGHT. M10 is MISSED, and it is
   an equivalent mutation (INFO-1). See the table.

5. **Behaviour regression:** none. The commit touches only a host test, a pytest file and an audit doc. The
   rewritten guard-field test still asserts each key for both the blank (`f == k`) and the omitted case, and its
   `keys[]` order matches `fields[]`. The new HEATER `hyst_c` test seeds `zone_type = 0` and `hyst_c = 3.5f`, and
   asserts that the value survives an omitting body. Both baselines pass.

## Findings

### MED-1: five omit-preserved fields are guarded only by the static drift test, which three edit shapes pass

The affected fields and keys are:

| Field | Key |
|---|---|
| `approach_rate_cap_c_per_hr` | `approachratecap` |
| `error_band_c` | `errorband` |
| `rate_band_c_per_s` | `rateband` |
| `progress_band_c` | `progressband` |
| `coil_power_w` | `coilpower` |

No host test posts a body that omits these keys and checks the stored value survives. For these fields the drift
test is the only guard, and Q3 shows it passes three edit shapes. X1, X2 and X3 apply those shapes to real
fields, and each slips both layers:

- **X1:** an `else if` HEATER default on `approachratecap`. This is the same conditional-default class as
  BENCHFX4 LOW-1, which the commit fixed for `hyst_c` only.
- **X2:** `error_band_c` zeroed in a later block. Because the override sits after the parse, it zeroes the field
  even when the key is posted.
- **X3:** a compound `*= 0.0f` on `coil_power_w`.

The effect of such an edit is a silent reset on any GET-merge-POST writer, such as `control_set_zone_limits` and
other narrow tools that strip omit-preserved keys. The reset hits the approach cap (0 means uncapped), the error,
rate and progress bands (0 means off), or the coil power figure. None of the five is a Pico-side safety guard,
so this is MED, not HIGH.

Fix direction (not applied):
1. Extend `test_post_omitting_new_fields_preserves_stored_values` (or a sibling test) to seed the five fields
   with distinct nonzero values and assert each one after an omitting body, for both zone types.
2. Optionally, make the drift test fail on any `else if` in a key block, on any compound assignment
   (`[-+*/|&]=`) to the field, and on a field assignment anywhere after its own block, not only inside it.

### LOW-1: the exemptions cover the whole field, not the one legitimate assignment

`_LATER_ASSIGN_EXEMPT = {"settings_source", "tc_type"}` skips the later-assignment check for every later
assignment to those fields. X4 and X5 show the drift test passes an omit default added after the preserving
`else` on either field.

The host tests back this up for `settings_source` (X5 CAUGHT). For `tc_type`, X4b (the same edit with a value that differs from the fixture's seed) is CAUGHT by `test_zones_http.c:1279`. Neither exemption therefore hides a drift today. But that is the host test doing the work, and the drift test has a blind spot that a future edit could fall into on a field with weaker host coverage.

Fix direction: exempt only the known right-hand sides (`= preserved_tc_type;`, `= src_raw;`), so that any other
later assignment to those fields still fails.

### INFO-1: M10 is an equivalent mutation

`ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS` feeds only the never-stripped loop in
`test_defaulted_keys_match_firmware_and_are_never_stripped`. `name` is also in the hard-coded never-matched list
in `test_regex_matches_exactly_the_float_keys`, and no runtime code reads the tuple. Emptying it changes nothing
observable, so MISSED is the expected result. No action.

### INFO-2: `BUILD FAILURES` in the kilnfw-host ExpectPattern scores an uncompilable mutant as CAUGHT

The ExpectPattern recommended for kilnfw-host negtests includes `BUILD FAILURES`. Run F's M9 did not compile,
because `'\0'` lost its escape in JSON and became `''` (`error C2137: empty character constant`). negtest
reported it CAUGHT. A reader of the summary line alone would take that as real coverage.

BENCHFX4's own M9 used the same find/replace and never ran on the host side, so it was not affected. A mutant
that does not build should be reported as INVALID, or checked for in the log, before it counts as caught.

### INFO-3: the top-level handler keys sit outside the drift test

`pc_link_abort_silence_ms`, `relay<N>_name` and `relay<N>_type` are omit-preserved in `zones_post_handler()`
(`zones_http_post.c`), not in `zones_http_parse_zone_fields()`, so the drift test never sees them. All three
have host-test coverage today, and no PC writer strips them. No action unless a writer starts stripping them.

### INFO-4: BENCHFX4 INFO-2 is still open

BENCHFX4's own fix-status table still lists its INFO-2 (the BENCHFX3 evidence pointer) as open. This commit did
not address it.

## Appendix: mutation JSON

**Run P** used M6, M7, M8, M9, X1-X5, N9b, M10, N6, M11a and M11b, with `-Preset pytest`.

**Run F** used M6, M7, M8, M9 and X1-X5 (`fw.json`), with the `-Command`/`-ExpectPattern` given under
Verification.

**Run F2** used M9b, and **run F3** used X4b.

```json
[
 {
  "name": "M6 fw coupling cell omit = 0.0f",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "z->coupling_coeff[j] = current_z->coupling_coeff[j];",
  "replace": "z->coupling_coeff[j] = 0.0f;"
 },
 {
  "name": "M7 fw settings_source per-group omit = CUSTOM",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "z->settings_source[g] = current_z->settings_source[g];",
  "replace": "z->settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;"
 },
 {
  "name": "M8 fw thermo_mask becomes omit-preserved",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "z->thermo_mask = (uint8_t)(1u << i);",
  "replace": "z->thermo_mask = current_z->thermo_mask;"
 },
 {
  "name": "M9 fw name omit blanks the name",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "strncpy(z->name, current_z->name, ZONE_NAME_MAX_LEN);",
  "replace": "z->name[0] = '\u0000';"
 },
 {
  "name": "X1 fw approachratecap else-if HEATER default before preserving else",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "        } else {\n            z->approach_rate_cap_c_per_hr = current_z->approach_rate_cap_c_per_hr;",
  "replace": "        } else if (current_z->zone_type == 0u) {\n            z->approach_rate_cap_c_per_hr = 0.0f;\n        } else {\n            z->approach_rate_cap_c_per_hr = current_z->approach_rate_cap_c_per_hr;"
 },
 {
  "name": "X2 fw error_band_c zeroed in a later block (after coupling memcpy)",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "    memcpy(z->coupling_dead_time_s, current_z->coupling_dead_time_s, sizeof(z->coupling_dead_time_s));",
  "replace": "    memcpy(z->coupling_dead_time_s, current_z->coupling_dead_time_s, sizeof(z->coupling_dead_time_s));\n    z->error_band_c = 0.0f;"
 },
 {
  "name": "X3 fw coil_power_w compound *= 0 after preserving else",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "z->coil_power_w = current_z->coil_power_w;",
  "replace": "z->coil_power_w = current_z->coil_power_w; } { z->coil_power_w *= 0.0f;"
 },
 {
  "name": "X4 fw tc_type omit default after else (exempt field)",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "    if (i >= thermo_count) {",
  "replace": "    if (!zones_config_json_field_present(body, key)) { z->tc_type = 3u; }\n    if (i >= thermo_count) {"
 },
 {
  "name": "X5 fw settings_source group 0 forced CUSTOM when legacy key omitted (exempt field)",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "            z->settings_source[g] = src_raw;",
  "replace": "            z->settings_source[g] = src_raw;\n        }\n        if (!have_legacy) { z->settings_source[0] = ZONE_SETTINGS_SOURCE_CUSTOM; }\n        {"
 },
 {
  "name": "M9b fw name omit blanks the name (compiling form)",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "strncpy(z->name, current_z->name, ZONE_NAME_MAX_LEN);",
  "replace": "z->name[0] = 0;"
 },
 {
  "name": "X4b fw tc_type omit default 5u before thermo_count check (exempt field; distinct from seed 3)",
  "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
  "find": "    if (i >= thermo_count) {",
  "replace": "    if (!zones_config_json_field_present(body, key)) { z->tc_type = 5u; }\n    if (i >= thermo_count) {"
 },
 {
  "name": "N9b py ct_mask added to the strip regex only",
  "file": "tools/PcTools/src/kilnctrl/zones_http_client.py",
  "find": "r\"|coupling_c\\d+)$\")",
  "replace": "r\"|coupling_c\\d+|ct_mask)$\")"
 },
 {
  "name": "M10 py third-idiom list emptied",
  "file": "tools/PcTools/src/kilnctrl/zones_http_client.py",
  "find": "ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS = (\"name\",)",
  "replace": "ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS = ()"
 },
 {
  "name": "N6 hyst_c tolerance back to 0.001",
  "file": "tools/PcTools/src/kilnctrl/bench_test/cases_heat.py",
  "find": "\"hyst_c\": 0.0005}",
  "replace": "\"hyst_c\": 0.001}"
 },
 {
  "name": "M11a py fixed-format tolerance half step -> full step",
  "file": "tools/PcTools/src/kilnctrl/zones_http_client.py",
  "find": "return 0.5 * 10.0 ** -decimals",
  "replace": "return 1.0 * 10.0 ** -decimals"
 },
 {
  "name": "M11b py %.9g relative eps 1e-6 -> 1e-3",
  "file": "tools/PcTools/src/kilnctrl/zones_http_client.py",
  "find": "_FLOAT_EPS_REL = 1e-6",
  "replace": "_FLOAT_EPS_REL = 1e-3"
 }
]
```
