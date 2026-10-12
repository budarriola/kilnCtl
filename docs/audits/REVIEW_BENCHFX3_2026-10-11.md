# Review: benchfx3 (`66593acf6`), 2026-10-11

Scope: origin/dev `66593acf6` ("bench_test/zones: strip all omit-preserved float fields (guard thresholds),
firmware drift test, pin case-POST strip, tight hyst_c read-back"). It fixes REVIEW_BENCHFX2_2026-10-11 LOW-1,
LOW-2, INFO-2 and INFO-4. Files: `tools/PcTools/src/kilnctrl/zones_http_client.py`, `mcp_server_control.py`,
`bench_test/cases_heat.py`, the new `tools/PcTools/tests/test_zone_omit_preserved_drift.py` and four test files.
Reviewed in a worktree at origin/dev `cd0c20e8d`, against
`firmware/KilnFW/App/drivers/http/zones_http_post_parse.c` and `zones_http_get.c`.

Verdict: the strip change is correct. Every key the regex now strips is truly omit-preserved in firmware, no
required field is stripped, and preset-named keys are kept. No HIGH finding. One MED: the new drift test is
blind to the dangerous direction of drift, and it already certifies two keys that are not omit-preserved.

## Verification

- Full PcTools suite in worktree `C:\wt\bfx3_zwu08o` at origin/dev `cd0c20e8d`, `KILNCTL_SLOW_TESTS=1`,
  `uv run pytest -n 8`: **7508 passed, 59 skipped, 138 subtests passed** (894.97 s), exit 0.
- `tools\negtest.ps1 -Preset pytest` over the drift test, `test_bench_test_cases_heat.py`, the five
  `test_mcp_server_control_*` zone/relay writer tests, `test_pctools_write_review_2026_10_09.py`,
  `test_zones_http_client.py` and `test_config_presets.py`. Baseline PASS (18 s).

| # | Mutation | Verdict | Caught by |
|---|---|---|---|
| N1 | firmware: `runawaymargin` omit branch writes `0.0f` (omit-defaulted) | MISSED | nothing (MED-1) |
| N2 | firmware: `hystc` omit branch writes `0.0f` (omit-defaulted) | MISSED | nothing (MED-1) |
| N3 | firmware: `hystc` presence check removed (required) | CAUGHT | drift test |
| N4 | python: `xzone` dropped from the float list | CAUGHT | drift test, regex tests, write-review test |
| N5 | python: `kp` (required) added to the float list | CAUGHT | regex test, relay-type, coupling, write-review |
| N6 | `hyst_c` tolerance 0.001 -> 0.0015 | MISSED | nothing (LOW-1) |
| N7 | HP-07 first case POST not stripped | CAUGHT | `test_bench_test_cases_heat` HP-07 |
| N8 | HP-03 first case POST not stripped | CAUGHT | `test_bench_test_cases_heat` HP-03 |
| N9 | python: `thermo_mask` (omit-defaulted) moved into the strip list | CAUGHT | only incidental strip counts in relay-type/coupling tests, not the drift test |

negtest exited 2 only because this review file was written into the worktree during the run ("REAL TREE
CHANGED"). Every mutation verdict above was recorded before that check ran.

## Answers to the review questions

1. **Is the drift test's parse robust?** Partly.
   - It fails loudly when the source shape changes in a way that removes matches: a moved or renamed file
     (`assertTrue(_SRC.is_file())`, no skip), a renamed `key` buffer, a helper macro replacing `snprintf`, or a
     presence check moved more than 4 lines below the `snprintf`. Each of these shrinks the firmware set, and
     `py - fw` fails. The `> 25` floor also catches a parse that finds almost nothing.
   - The `gkey` sub-keys (`z%u_settings_source_%s`) do not match `_KEY_RE` (the `%s` stops the lazy group), so
     they are ignored as intended.
   - It does **not** check what the omit branch does. It counts a key as omit-preserved when a
     `field_present`/`field_nonblank` call follows the `snprintf`. It never checks that the `else` branch
     assigns from `current_z->`. See MED-1.
   - A new omit-preserved field added with a different idiom (or in another file) is not found. That direction
     is safe: the field is simply not stripped and is re-posted at GET precision.
2. **Does every caller still send each required field?** Yes.
   - I read every branch for the 21 float keys plus `coupling_c<j>`. All 13 `field_present` branches and the 9
     `zones_http_field_nonblank` branches (eight `guard_*` plus `xzone`) assign `current_z->...` when the key
     is missing. None is required and none falls back to a default. The coupling loop does the same per cell.
   - The required keys (`relay_mask`, `timingprofile`, `cal`, `kp`/`ki`/`kd`, `ramp`, `sanity`, `mode`,
     `maxtemp`, `mintemp`, `window`, `minon`, `minoff`) are not in the regex. `test_regex_matches_exactly_the_float_keys`
     pins this, and negtest N5 (adding `kp` to the float list) is CAUGHT.
   - Callers: `control_set_zone_limits`, `control_set_zone_type`, `control_set_zone_relay_mask`,
     `control_set_zone_coupling` (keeps its one target cell), `control_set_relay_type`, `apply_zone_preset`
     (used by `config_presets`), bench HP-03/HP-07/restore and AX-C03. None of the MCP writers sets a guard
     field, so stripping guard keys only stops the lossy re-post.
3. **Is the non-stripped non-float list correct?** Not all of it. `tctype`, `relaytype`, `zonetype`,
   `failsafe`, `minons`, `minoffs` and `settings_source` are omit-preserved. `thermo_mask` and `ct_mask` are
   **not**: a missing `thermo_mask` becomes `1u << i` (the legacy channel map), and a missing `ct_mask` becomes
   0. They are not stripped, so nothing breaks today. But the list's comment ("omit-preserved enums/ints/masks")
   and the drift test both claim otherwise. See MED-1. (REVIEW_BENCHFX2 question 1 listed them as
   omit-preserved too. That was wrong.)
4. **Do `apply_zone_preset` and `config_presets` keep preset keys?** Yes.
   `_preset_named_omit_preserved_keys` keeps `z<i>_<suffix>` for every preset key in
   `_PRESET_ZONE_OVERRIDE_FIELDS`. All eight `guard_*` fields and `cross_zone_max_delta_c` are in that set
   and in `_ZONE_FIELD_FORM_KEY` (`wrongdirwindow` .. `frozenwindow`, `xzone`). A preset that names a guard
   threshold, including an explicit `cross_zone_max_delta_c: 0`, still sends it. `config_presets` reaches the
   zones POST only through `apply_zone_preset`.
5. **Tests.** See Verification.

## Findings

### MED-1: the drift test cannot see an omit-preserved field becoming omit-defaulted, and already mislabels two keys

`firmware_omit_preserved_keys()` takes the presence of a `field_present`/`field_nonblank` call as proof that
omission preserves the stored value. It never looks at the `else` branch.

- **Today:** `z%u_thermo_mask` and `z%u_ct_mask` pass that test but are omit-defaulted
  (`zones_http_post_parse.c`: `z->thermo_mask = (uint8_t)(1u << i);` and `z->ct_mask = 0;`). The test
  therefore requires them to be listed in `ZONE_OMIT_PRESERVED_NONFLOAT_KEYS`, whose comment calls them
  omit-preserved. Anyone who later extends the strip to "every omit-preserved key" (the list exists to make
  that easy) would silently remap every zone's thermocouple to channel `i` and clear every CT mapping on each
  narrow MCP write. No test would fail. Negtest N9 (moving `thermo_mask` into the float list) is caught only by
  the incidental per-zone strip counts in the relay-type and coupling tests, not by the drift test.
- **Future drift:** if a firmware change turns one of the 21 stripped floats into omit-defaulted (for example
  an `else` that writes `0.0f` or a firmware default), the drift test still passes, while every stripped
  POST now resets that field. For the `guard_*` keys that resets a thermal-guard threshold on every narrow
  write. That is the "worse, defaulted" case the strip list must never hit. Negtests N1 (`runawaymargin`
  else branch to `0.0f`) and N2 (`hystc` else branch to `0.0f`) are both MISSED.
- **Fix:**
  - Make the parser read the presence check's block through its `else` and require
    `current_z->` (or `current_z->...[j]` for the coupling cell) in the omit branch. Classify a key with a
    presence check but no `current_z->` omit branch as omit-defaulted.
  - Move `thermo_mask` and `ct_mask` to a separate `ZONE_OMIT_DEFAULTED_KEYS` tuple (never stripped), and assert
    that the regex never matches it.
  - Re-run N1, N2 and N9 to show they are CAUGHT by the drift test itself.
  - `z%u_name` is also omit-preserved (`name_len < 0` -> `strncpy(... current_z->name ...)`), through a third
    idiom that the parser does not see. It is not a float and must not be stripped, so this is a
    completeness note only. If the parser is tightened, record `name` explicitly as known, so the lists
    really are the complete set.

### LOW-1: the `hyst_c` read-back tolerance still accepts a one-print-step mismatch

`_READBACK_FLOAT_TOL["hyst_c"]` is 0.001, which is the full `%.3f` print step, not half of it (the comment says
"%.3f for hyst_c (0.001)", while `max_temp_c` correctly uses half its step, 0.05). Two adjacent printed values
differ by slightly less than 0.001 in binary floating point: `2.041 - 2.040 == 0.0009999999999998899`. So a
restore that left `hyst_c` one step off (2.040 where the snapshot read 2.041) passes the read-back.
`test_readback_float_tolerance_pinned` tries +0.01 (must fail) and +0.0004 (must pass) and never tries
exactly one step. Negtest N6 (tolerance 0.001 -> 0.0015) is MISSED.

- **Fix:** use 0.0005 for `hyst_c`, and add an exactly-one-step (+0.001) mismatch case to the pinned test.
- Impact is small: `hyst_c` is only used by on/off zones, and a 0.001 C error is far below the noise floor.

### INFO-1: no change to the pooled INFO-1

The pooled INFO-1 (required guard floats re-posted at GET precision) is as described in REVIEW_BENCHFX2. This
commit neither makes it better nor worse.

## Fix status

| Finding | Status | Where |
|---|---|---|
| MED-1 drift test does not check the omit branch; `thermo_mask`/`ct_mask` mislabelled | OPEN | |
| LOW-1 `hyst_c` tolerance is a full print step | OPEN | |
