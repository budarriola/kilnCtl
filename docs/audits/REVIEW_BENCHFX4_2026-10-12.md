# Review: benchfx4 (`7949cf752`), 2026-10-12

Scope: origin/dev `7949cf752` ("PcTools: drift test checks omit branch assigns current_z, defaulted keys never
stripped, hyst_c tol 0.0005 (review BENCHFX3 MED-1, LOW-1)"). Files:
`tools/PcTools/tests/test_zone_omit_preserved_drift.py`, `tools/PcTools/src/kilnctrl/zones_http_client.py`
(`ZONE_OMIT_DEFAULTED_KEYS`, `ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS`) and
`tools/PcTools/src/kilnctrl/bench_test/cases_heat.py` (`hyst_c` read-back tolerance 0.0005). Reviewed in
worktree `C:\wt\bfx4rev_srpx8t` at `7949cf752`, against
`firmware/KilnFW/App/drivers/http/zones_http_post_parse.c` and `firmware/KilnFW/App/test/test_zones_http.c`.
No board was touched. No code was edited.

Verdict: the fix closes BENCHFX3 MED-1 and LOW-1 as written. The parser classifies every key in today's
firmware correctly, and the two mutations BENCHFX3 recorded as MISSED (N1, N2) are now CAUGHT. No HIGH. One
MED: an omit branch that copies the *wrong* `current_z` field passes both the drift test and the firmware
host tests. The drift test's omit check is a loose regex (any `current_z->` text after any `else`), so it
also passes a comment, a conditional preserve, a `current_z`-dependent default and a post-branch override.
The firmware host tests cover three of those four, which leaves the one listed under LOW-1 exposed.

## Verification

Two `tools\negtest.ps1` runs, both on base `7949cf752`, both with the unmodified baseline passing,
`real_tree_unchanged: true` and `copies_removed: true`.

**Run A, `-Preset pytest`.** 16 mutations over 11 test files (381 tests): the drift test,
`test_bench_test_cases_heat.py`, `test_mcp_server_control_{relay_type,zone_coupling,zone_limits,zone_relay_mask,zone_type,coupling}.py`,
`test_pctools_write_review_2026_10_09.py`, `test_zones_http_client.py` and `test_config_presets.py`. The
baseline passed in 19.2 s.

**Run B, KilnFW host tests.** The 7 firmware mutations, using
`build_host_tests.ps1 -Only "kilnctl_host_tests_zones\.exe"` (the executable built from `test_zones_http.c`)
with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES"`. The baseline passed in 62 s.

The first attempt at run B used `-Only "^test_zones_http$"` and failed at baseline, because that regex matched
no executable. Its verdicts were discarded and run B was repeated with the regex above.

| # | Mutation | Drift/pytest (A) | Host tests (B) | Net |
|---|---|---|---|---|
| M1 | fw: `hystc` omit branch writes `0.0f`; a trailing comment names `current_z->hyst_c` | MISSED | CAUGHT (`test_zones_http.c:6715`) | caught |
| M2 | fw: `hystc` omit preserves only when `current_z->zone_type == 1`, else `0.0f` | MISSED | MISSED | **slips** (LOW-1) |
| M3 | fw: `hystc` omit branch = `current_z`-type-dependent default | MISSED | CAUGHT (6715) | caught |
| M4 | fw: `runawaymargin` omit branch copies `current_z->guard_runaway_rate_c_per_min` | MISSED | MISSED | **slips** (MED-1) |
| M5 | fw: `hyst_c = 0` on omit, inserted after the preserving if/else | MISSED | CAUGHT (6715) | caught |
| N1 | fw: `runawaymargin` omit branch = `0.0f` (BENCHFX3 re-run) | CAUGHT (drift x2) | CAUGHT (1989, 1991) | caught |
| N2 | fw: `hystc` omit branch = `0.0f` (BENCHFX3 re-run) | CAUGHT (drift x2) | CAUGHT (6715) | caught |
| M6 | fw: coupling cell omit branch = `0.0f` | CAUGHT (drift x2) | not run | caught |
| M7 | fw: `settings_source` per-group omit = `ZONE_SETTINGS_SOURCE_CUSTOM` | CAUGHT (drift x2) | not run | caught |
| M8 | fw: `thermo_mask` omit becomes preserved (the safe direction) | CAUGHT (drift x2) | not run | caught |
| M9 | fw: `name` omit blanks the name | MISSED | not run | INFO-1 |
| N9 | py: `thermo_mask` added to the float strip list (BENCHFX3 re-run) | CAUGHT (4 tests, see Q4) | n/a | caught |
| N9b | py: `ct_mask` added to the strip regex only | CAUGHT (3 tests) | n/a | caught |
| M10 | py: `ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS` emptied | MISSED | n/a | INFO-1 |
| N6 | py: `hyst_c` tolerance back to 0.001 (BENCHFX3 re-run) | CAUGHT (`test_readback_float_tolerance_pinned`) | n/a | caught |
| M11 | py: `_float_close` default tolerance 1e-3 to 2e-3 | MISSED | n/a | LOW-2 |

"drift x2" means both `test_python_lists_match_firmware` and
`test_defaulted_keys_match_firmware_and_are_never_stripped` failed.

## Answers to the review questions

1. **Does the parser classify every key in `zones_http_post_parse.c` correctly?** Yes, for today's source. I
   walked all 46 key snprintfs by hand and confirmed the result with a replica of the test's regexes.
   - **Preserved:** `tctype`, `relaytype`, `zonetype`, `failsafe`, `hystc`, `minons`, `minoffs`, `coilpower`,
     the nine guard keys `wrongdirwindow`..`xzone` (lines 607-713), `k`, `tau`, `deadtime`,
     `fuzzy_strength`, `coupling_diag_k_dc`, `easeoffmult`, `approachratecap`, `errorband`, `rateband`,
     `progressband`, `coupling_c%u` (else at 984) and `settings_source`.
   - **Defaulted:** `thermo_mask` (else at 381 writes `1u << i`) and `ct_mask` (else at 407 writes `0`).
   - **Required, so skipped:** `timingprofile`, `cal`, `kp`, `ki`, `kd`, `ramp`, `sanity`, `mode`, `maxtemp`,
     `mintemp`, `window`, `minon`, `minoff`.
   - **Not seen by `_KEY_RE`, by design:** `name` (third idiom; `strncpy` from `current_z->name` at 88).
   - **One fragile spot.** The legacy `z%u_settings_source` key at 1028 has no `else` of its own. It
     classifies as preserved only because the per-group `gkey` loop's `else` at 1048-1049
     (`z->settings_source[g] = current_z->settings_source[g];`) falls inside the same block. That `gkey`
     snprintf at 1038 does not match `_KEY_RE`, so the block runs to end of file. The result is correct
     today, but by accident.

2. **Can a real regression slip past?** Yes. `_OMIT_PRESERVE_RE` (`\belse[^{]*\{[^{}]*?current_z->`) accepts
   any `current_z->` text in the first brace-free run after any `else` in the block. It does not check which
   field is assigned or whether `current_z->` sits in a comment. It ignores a conditional, a ternary, and a
   later assignment that overrides the branch. Run A shows the drift test passes all five of M1-M5. The
   firmware host tests catch M1, M3 and M5. They miss:
   - **M4**, because `test_zones_post_blank_guard_fields_and_pc_link` (lines 1970-2004) seeds all nine guard
     fields with the same 9.0f, so a cross-wired copy cannot be seen.
   - **M2**, because the only `hyst_c` omit test (around 6694-6715) uses an ON_OFF current zone.

3. **Do any tolerances still mask a real write failure?**
   - `cases_heat.py` `_READBACK_FLOAT_TOL` is correct: `hyst_c` 0.0005 against a `%.3f` print and
     `max_temp_c` 0.05 against `%.1f`, both half a step. Ints and the `failsafe` bool compare exactly.
     `mcp_server_control.py`'s zone-limit tolerance (0.05 against `%.1f`) is also correct.
   - **One full-step tolerance remains** in pre-existing code (LOW-2). `zones_http_client._float_close`
     (line 1247, `tol=1e-3`) is used by `_verify_against_preset` for every preset float. That includes
     `cal_offset_c` and the guard rates, which GET prints at `%.3f`, so a one-print-step mismatch passes. Run A's
     M11 widened it to 2e-3 and nothing failed, so the value is not pinned either.
   - `_ZONE_COUPLING_READBACK_TOLERANCE = 0.0005` against a `%.9g` print is looser than it needs to be, but it
     is not a full step, and the coupling range is [0, 100]. Acceptable.

4. **Which test caught N9?** Four tests failed, all named in the run A FAILED lines:
   - `test_zone_omit_preserved_drift.py::ZoneOmitPreservedDriftTest::test_defaulted_keys_match_firmware_and_are_never_stripped`.
     This is new in this commit, and it is the intended catcher.
   - `test_zone_omit_preserved_drift.py::ZoneOmitPreservedDriftTest::test_python_lists_match_firmware`. The
     Python preserved set gains a key the firmware defaults.
   - `test_mcp_server_control_relay_type.py::HappyPathTest::test_post_carries_exactly_one_relay_key_and_strips_omit_preserved`.
     Incidental, as in BENCHFX3.
   - `test_mcp_server_control_zone_coupling.py::EndToEndRealBodyTest::test_tool_posts_only_the_target_cell_in_bench_orientation`.
     Incidental.

   N9b (`ct_mask` added to the regex only) was caught by the new defaulted-keys test and the same two
   incidental tests. `test_python_lists_match_firmware` did not fail for N9b, because the regex-only change
   leaves the lists unchanged. So N9 is now caught by a test that targets it directly, not only by strip
   counts.

## Findings

### MED-1: a wrong-field omit copy passes the drift test and the firmware host tests

Mutation M4 (`z->guard_runaway_margin_c = current_z->guard_runaway_rate_c_per_min;` in the `runawaymargin`
omit branch) passes run A and run B. Every partial `/api/zones` POST takes this path, including
`control_set_zone_coupling`, `control_set_zone_type`, `control_set_relay_type` and the bench cases, which strip
`runawaymargin` precisely because it is omit-preserved. Each such POST would silently overwrite a stored
runaway-margin guard threshold with the runaway-rate value. That is a guard misconfiguration with no error
and no read-back failure: the PC tools strip the key, so they never compare it. The same blind spot covers any
of the nine guard fields, or any two preserved fields of the same type. The cause is the 9.0f-for-all seed in
`test_zones_post_blank_guard_fields_and_pc_link`, plus a drift regex that does not check the field name.

Fix:
- In `test_zones_http.c`, seed each preserved field with a distinct value (for example 9.0f + 0.25f * k for
  the k-th guard field) in both the omitted and the blank cases, and assert each field individually.
- In the drift test, require the omit branch to be exactly `z-><f> = current_z-><f>;` with the same field
  name on both sides. Also allow the indexed form (`[j]`/`[g]` on both sides) and the `strncpy`/`memcpy`
  forms.

### LOW-1: the drift test's omit check is a loose regex, and one conditional variant also passes the host tests

The drift test passed M1 (a constant with `current_z->hyst_c` only in a trailing comment), M2 (preserve only
for on/off zones), M3 (a `current_z`-dependent default) and M5 (a reset to 0 on omit after the preserving
branch). The host tests caught M1, M3 and M5, but not M2. With M2, a HEATER zone's stored `hyst_c` resets to
0 (which means "firmware default 2.0 C") on every partial POST. That is lost the moment the zone is converted
to on/off. The impact is low, because a HEATER zone does not use `hyst_c`. The settings_source block is
correct only through the `gkey` `else` (Q1).

Fix:
- Before matching, strip C comments from each block.
- Accept only the exact same-field assignment from MED-1's fix as the whole else body.
- Fail if any later statement in the same block assigns `z-><f>`.
- Give the legacy `settings_source` key a direct test, or a comment-anchored allowance that names the `gkey`
  loop, so a refactor of that loop cannot change the result silently.
- In `test_zones_http.c`, add a `hyst_c` omit case with a HEATER (`zone_type=0`) current zone and a non-zero
  stored `hyst_c`.

### LOW-2: `_verify_against_preset` uses a full-step float tolerance that no test pins

`zones_http_client._float_close` (`tol=1e-3`) is a full print step for every `%.3f` field (`cal_offset_c`,
the guard rates). A preset restore whose read-back is off by one step reports success. Run A's M11 widened the
tolerance to 2e-3 and no test failed. This is the same class as BENCHFX3 LOW-1, in older code.

Fix:
- Use a per-field tolerance of half the GET print step, built from the same field-to-format table that
  `cases_heat.py` uses, for example `{"%.3f": 0.0005, "%.1f": 0.05}`.
- Add a pinned test that a one-step mismatch fails, like `test_readback_float_tolerance_pinned`.

### INFO-1: `name` (third idiom) has no executable coverage on the PC side

M9 (the firmware's `name` omit branch blanks the name) and M10 (the Python third-idiom list emptied) both pass
run A. `ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS` is used only to assert that `name` is never stripped. M9 was not
measured against the firmware host tests. No fix is needed on the PC side, because no PC writer strips `name`.
If coverage matters, a firmware host test that omits `z%u_name` and asserts the stored name survives is the
right place for it.

### INFO-2: BENCHFX3 status row cites evidence that is not in the commit

`REVIEW_BENCHFX3_2026-10-11.md`'s status row says N1, N2 and N9 were CAUGHT "(see commit message)", but
`7949cf752`'s message carries no negtest output. Run A in this review re-ran all three and supplies the
evidence: CAUGHT, CAUGHT and CAUGHT, with the tests named under Q4.

Fix: point the BENCHFX3 row at this review.

## Fix status

| Finding | Severity | Status |
|---|---|---|
| MED-1 wrong-field omit copy slips both layers | MED | FIXED: `test_zones_post_blank_guard_fields_and_pc_link` seeds each guard field with a distinct value (9.0 + 0.25k) and asserts each separately; the drift test now requires the omit `else` to be exactly `z-><f> = current_z-><f>;` (same field and index both sides). M4 CAUGHT on both layers. |
| LOW-1 loose omit regex; HEATER `hyst_c` conditional slips | LOW | FIXED: comments stripped before matching, exact-assignment match, later same-field assignments flagged (`settings_source` and `tc_type` exempt, both named in the test), direct `settings_source` gkey-else test, new host test omitting `hyst_c` on a HEATER zone. M2 CAUGHT on both layers. |
| LOW-2 `_float_close` full-step, unpinned | LOW | fixed in f230b49a4: per-field half-step tolerance (`%.9g`: relative 1e-6), format table pinned to `zones_http_get.c` by `test_zones_http_float_tolerance_pin.py`; negtest widen-to-2e-3 and format-mapping mutations CAUGHT |
| INFO-1 `name` third idiom uncovered on PC | INFO | no action needed |
| INFO-2 BENCHFX3 evidence pointer | INFO | open (doc) |

Re-run (negtest, base = the fix commit): pytest side (drift + relay_type + zone_coupling tests) M1-M5, N1, N2, N9 all CAUGHT; host side (`kilnctl_host_tests_zones.exe`) M1-M5, N1, N2 all CAUGHT; baselines passed, real tree unchanged.

| Mutation | Drift/pytest | Host tests |
|---|---|---|
| M1 | CAUGHT | CAUGHT |
| M2 | CAUGHT | CAUGHT |
| M3 | CAUGHT | CAUGHT |
| M4 | CAUGHT | CAUGHT |
| M5 | CAUGHT (`test_firmware_has_no_later_overrides`) | CAUGHT |
| N1 | CAUGHT | CAUGHT |
| N2 | CAUGHT | CAUGHT |
| N9 | CAUGHT (4 tests) | n/a |

## Appendix: mutation JSON

Run A used all 16 entries, with `-Preset pytest` over the 11 files listed under Verification. Run B used
entries M1-M5, N1 and N2 (`bfx4_fw_mutations.json`), with the `-Command`/`-ExpectPattern` given above and
`-Parallel 2`.

```json
[
  {"name": "M1 fw hystc omit branch constant, trailing comment names current_z",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "z->hyst_c = current_z->hyst_c;",
   "replace": "z->hyst_c = 0.0f; /* was current_z->hyst_c */"},
  {"name": "M2 fw hystc omit branch preserves only for on/off zones, else 0",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "z->hyst_c = current_z->hyst_c;",
   "replace": "if (current_z->zone_type == 1u) { z->hyst_c = current_z->hyst_c; } else { z->hyst_c = 0.0f; }"},
  {"name": "M3 fw hystc omit branch = firmware default chosen by current_z type",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "z->hyst_c = current_z->hyst_c;",
   "replace": "z->hyst_c = (current_z->zone_type == 1u) ? ZONE_HYST_C_DEFAULT : 0.0f;"},
  {"name": "M4 fw runawaymargin omit branch copies the wrong field (copy-paste)",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "z->guard_runaway_margin_c = current_z->guard_runaway_margin_c;",
   "replace": "z->guard_runaway_margin_c = current_z->guard_runaway_rate_c_per_min;"},
  {"name": "M5 fw hystc reset to 0 on omit AFTER the preserving if/else",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "snprintf(key, sizeof(key), \"z%u_minons\", i);",
   "replace": "if (!zones_config_json_field_present(body, key)) { z->hyst_c = 0.0f; }\n    snprintf(key, sizeof(key), \"z%u_minons\", i);"},
  {"name": "N1 fw runawaymargin omit branch = 0.0f (re-run)",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "z->guard_runaway_margin_c = current_z->guard_runaway_margin_c;",
   "replace": "z->guard_runaway_margin_c = 0.0f;"},
  {"name": "N2 fw hystc omit branch = 0.0f (re-run)",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "z->hyst_c = current_z->hyst_c;",
   "replace": "z->hyst_c = 0.0f;"},
  {"name": "M6 fw coupling cell omit branch = 0.0f",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "z->coupling_coeff[j] = current_z->coupling_coeff[j];",
   "replace": "z->coupling_coeff[j] = 0.0f;"},
  {"name": "M7 fw settings_source per-group omit = CUSTOM",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "z->settings_source[g] = current_z->settings_source[g];",
   "replace": "z->settings_source[g] = ZONE_SETTINGS_SOURCE_CUSTOM;"},
  {"name": "M8 fw thermo_mask becomes omit-preserved",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "z->thermo_mask = (uint8_t)(1u << i);",
   "replace": "z->thermo_mask = current_z->thermo_mask;"},
  {"name": "M9 fw name omit blanks the name",
   "file": "firmware/KilnFW/App/drivers/http/zones_http_post_parse.c",
   "find": "strncpy(z->name, current_z->name, ZONE_NAME_MAX_LEN);",
   "replace": "z->name[0] = '\\0';"},
  {"name": "N9 py thermo_mask added to the float strip list (re-run)",
   "file": "tools/PcTools/src/kilnctrl/zones_http_client.py",
   "find": "\"driftperiod\", \"debounce\", \"frozenwindow\", \"xzone\",",
   "replace": "\"driftperiod\", \"debounce\", \"frozenwindow\", \"xzone\", \"thermo_mask\","},
  {"name": "N9b py ct_mask added to the strip regex only",
   "file": "tools/PcTools/src/kilnctrl/zones_http_client.py",
   "find": "r\"|coupling_c\\d+)$\")",
   "replace": "r\"|coupling_c\\d+|ct_mask)$\")"},
  {"name": "M10 py third-idiom list emptied",
   "file": "tools/PcTools/src/kilnctrl/zones_http_client.py",
   "find": "ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS = (\"name\",)",
   "replace": "ZONE_OMIT_PRESERVED_THIRD_IDIOM_KEYS = ()"},
  {"name": "N6 hyst_c tolerance back to a full step 0.001 (re-run)",
   "file": "tools/PcTools/src/kilnctrl/bench_test/cases_heat.py",
   "find": "\"hyst_c\": 0.0005}",
   "replace": "\"hyst_c\": 0.001}"},
  {"name": "M11 py preset read-back tolerance 1e-3 -> 2e-3",
   "file": "tools/PcTools/src/kilnctrl/zones_http_client.py",
   "find": "def _float_close(a: float, b: float, tol: float = 1e-3)",
   "replace": "def _float_close(a: float, b: float, tol: float = 2e-3)"}
]
```
