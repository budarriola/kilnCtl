# Review: benchfx2 (`9a085e44f`), 2026-10-11

Scope: origin/dev `9a085e44f` ("bench_test: benchfx review fixes (non-vacuous LOW-A test, pinned HP field
lists, full read-back, omit-preserved strip, AX-C03 detail fixes)"). It fixes REVIEW_BENCHFX_2026-10-11 MED-1,
LOW-1, LOW-2 and INFO-1..4. Files: `tools/PcTools/src/kilnctrl/bench_test/cases_heat.py`, `cases_aux.py` and
their two test files. Reviewed read-only in a worktree at origin/dev `9a085e44f`, against
`tools/PcTools/src/kilnctrl/zones_http_client.py` and
`firmware/KilnFW/App/drivers/http/zones_http_{get,post_parse}.c`.

Verdict: the fix is correct and the restore is safe against the real POST contract. No HIGH or MED finding.
The prior review's vacuous tests (H1, H3, H5, H8, A8) are all CAUGHT now. Two gaps remain: the case POSTs'
strip is not pinned by any test, and the shared strip regex misses nine omit-preserved float fields.

## Verification

- `uv run pytest tests/test_bench_test_cases_heat.py tests/test_bench_test_cases_aux.py` (worktree uv): 217 passed.
- `tools\negtest.ps1 -Preset pytest -PresetArg "<both files>"`, two batches (17 mutations). Baseline passed,
  `real_tree_unchanged` true and `copies_removed` true in both. Result: 14 CAUGHT, 3 MISSED.

| Mutation | Verdict |
|---|---|
| M1 HP-03 case POST uses `build_post_body` (no strip) | **MISSED** |
| M2 HP-07 case POST uses `build_post_body` (no strip) | **MISSED** |
| M3 restore POST uses `build_post_body` (no strip) | CAUGHT |
| M4 `_case_post_body` keeps nothing (`frozenset()`) | CAUGHT |
| M5 `_READBACK_FIELDS` back to `zone_type`/`max_temp_c` | CAUGHT |
| M6 read-back float tolerance 0.05 -> 100 | CAUGHT |
| M6b read-back float tolerance 0.05 -> 0.5 | **MISSED** |
| M7 AX-C03 live-GET phase label never set | CAUGHT |
| M8 AX-C03 non-int generation branch removed | CAUGHT |
| M9 `_restore_preset` takes only the first field | CAUGHT |
| M10 `max_temp_c` dropped from `_HP07_CHANGED_FIELDS` | CAUGHT |
| H1 `hyst_c` dropped from `_HP03_CHANGED_FIELDS` (prior MISSED) | CAUGHT |
| H3 `_HP07_CHANGED_FIELDS` cut to `max_temp_c`/`zone_type` (prior MISSED) | CAUGHT |
| H5 restore posts the whole pre-case snapshot under the fresh generation (prior MISSED) | CAUGHT |
| H8 `_restore_preset` reads the first zone (prior MISSED) | CAUGHT |
| A8 `restore.detail` not reset per call (prior MISSED) | CAUGHT |
| A10 AX-C03 phase stays "live GET" after the GET succeeds | CAUGHT |

## Answers to the review questions

1. **Is the restore correct against the real `/api/zones` POST contract?** Yes.
   - Omit-preserved in `parse_zone_fields()` (`field_present ? parse : current_z->...`): `tctype`, `relaytype`,
     `zonetype`, `failsafe`, `hystc`, `minons`, `minoffs`, `coilpower`, `thermo_mask`, `ct_mask`, `k`, `tau`,
     `deadtime`, `fuzzy_strength`, `coupling_diag_k_dc`, `easeoffmult`, `approachratecap`, `errorband`,
     `rateband`, `progressband`, `coupling_c<j>` and `settings_source[_<group>]`. The eight `guard_*` fields
     and `xzone` keep the stored value when blank or missing (`zones_http_field_nonblank`).
   - Required, so a POST missing them is refused with 400 (`"... missing or out of range"`): `relay_mask`,
     `timing_profile`, `cal_offset_c`, `pid_kp/ki/kd`, `max_ramp_c_per_hr`, `sanity_rate_c_per_min`,
     `control_mode`, `max_temp_c`, `min_temp_c`, `heater_window_ms`, `heater_min_on_ms`, `heater_min_off_ms`.
   - The strip regex (`ZONE_OMIT_PRESERVED_KEY_RE`) removes only firmware omit-preserved keys. It never removes a
     required key, so neither body can 400 for a missing field and nothing is zeroed.
   - Each attempt re-GETs, overlays only the case-changed fields and posts under the fresh `expected_generation`.
     A commit between GET and POST is refused 409 with nothing written, and the retry starts over.
   - Required floats are re-posted at GET precision. PID gains print `%.9g` (lossless). `max_temp_c`/`min_temp_c`
     print `%.1f`. This rounding is inherent to every GET-merge-POST writer. See INFO-1.
   - The fields the HP cases restore are all omit-preserved (`zone_type`, `failsafe_state`, `hyst_c`,
     `min_on_s`, `min_off_s`) or required (`max_temp_c`). The restore preset names them, so they are always sent.
2. **Could `_case_post_body` strip a field the case means to change, or keep one it should drop?**
   - **It never strips a case-changed field.** HP-03 changes `zone_type`, `failsafe_state`, `hyst_c`, `min_on_s`
     and `min_off_s`. HP-07 changes `max_temp_c`, `zone_type`, `failsafe_state`, `min_on_s` and `min_off_s`.
     Only `hyst_c` (`z<i>_hystc`) matches the regex. It is in `_PRESET_ZONE_OVERRIDE_FIELDS`, so
     `_preset_named_omit_preserved_keys` keeps it whenever the preset names it.
   - HP-07 does not name `hyst_c`, so `z<i>_hystc` is stripped from its case body and the firmware keeps the
     stored value bit-exact.
   - **It keeps nine fields it could drop.** Every zone's `guard_*` floats and `cross_zone_max_delta_c` are
     omit-preserved, but they are not in the regex. They are re-posted at `%.1f`/`%.3f`. See LOW-1.
3. **Can the new tests fail?** Mostly yes: 14 of 17 mutations are CAUGHT, including every prior MISSED one.
   - Nothing pins the two case-POST call sites (M1, M2 MISSED). See LOW-2.
   - The 0.05 tolerance is not pinned (M6b). See INFO-2.
4. **Is the fake realistic enough?** Yes, for what the tests claim.
   - `_GenerationFakeZhc` now deep-copies on GET and applies `body["merged"]` on a successful POST. That makes
     the read-back and the "mid-case edit survives" assertion real: H5 is CAUGHT.
   - The fake's `strip_omit_preserved` strips nothing. It only records `stripped` and `keep_keys`. The real strip
     is tested separately (`test_pctools_write_review_2026_10_09.py`), and
     `test_case_post_body_strips_and_keeps_hyst` calls the real `_preset_named_omit_preserved_keys`.
   - Limits: the fake does not model GET print rounding or required-field 400s, and it applies omit-preserved
     fields the real strip would drop. None of the new assertions depends on that, so it is acceptable. See
     INFO-3.
5. **Does the restore read-back use sane tolerances?** Yes, with one loose spot.
   - Bools and ints are compared exactly: `failsafe_state`, `zone_type`, `min_on_s`, `min_off_s`.
   - Floats are compared within 0.05. For `max_temp_c` (`%.1f`) that is half a print step, which is right.
   - For `hyst_c` (`%.3f`, range 0.5..25) the tolerance is 100 times the print step. A restore that left
     HP-03's 2.0 where the operator had 2.04 would pass. See INFO-2.

## Findings

### LOW-1 (pre-existing in the shared helper): `strip_omit_preserved` misses nine omit-preserved float fields

`ZONE_OMIT_PRESERVED_KEY_RE` covers the plant model, coupling, fuzzy/easeoff/approach/band, `hystc` and
`coilpower`. It does not cover these fields, which `parse_zone_fields()` also preserves when blank or missing:
`wrongdirwindow`, `wrongdirrate`, `offsettle`, `runawayrate`, `runawaymargin`, `driftperiod`, `debounce`,
`frozenwindow` and `xzone`.

- `zones_http_get.c` prints them at `%.1f` (windows, periods, margin, `cross_zone_max_delta_c`), `%.3f` (rates)
  and `%.0f` (debounce).
- Every HP-03/HP-07 case POST and restore POST therefore re-commits every zone's guard thresholds at print
  precision. `apply_zone_preset` and AX-C03's restore do the same.
- The comment above the regex says "Nothing else is strippable". That is wrong for these nine fields.
- **Effect:** values entered with finer resolution than the print (for example a runaway margin of 12.25 C) are
  rounded to 12.2 or 12.3 on each run. The rounding error is small, and these are guard thresholds, so the
  rounding can loosen a guard as well as tighten it.
- **Fix:** add the nine suffixes to the regex. They are all in `_PRESET_ZONE_OVERRIDE_FIELDS`, so a preset that
  names one keeps it. Correct the comment. Add a regex case for one of them to
  `test_pctools_write_review_2026_10_09.py`.

### LOW-2: the HP-03 and HP-07 case POSTs' strip is not pinned (M1, M2 MISSED)

The new tests pin `_case_post_body` directly and the restore call site (M3 CAUGHT). Nothing checks the two case
call sites. Reverting `on_off_body` (HP-03) or `limited_body` (HP-07) to plain `build_post_body` passes every
test, and LOW-2 of the prior review would quietly return for the case POST.

- **Fix:** in an existing `HP03Test`/`HP07Test` run, assert that the first posted body has `stripped` true. For
  HP-03, also assert that `keep_keys` contains `z<target>_hystc`. For HP-07, assert that it does not.

### INFO-1: required floats are re-posted at GET print precision

`max_temp_c`, `min_temp_c` (`%.1f`), `cal_offset_c` and `sanity_rate_c_per_min` (`%.3f`) and
`max_ramp_c_per_hr` (`%.2f`) are required. Every GET-merge-POST body, including the HP restore, re-commits them
at print precision. For `max_temp_c` the error is at most 0.05 C, which is below the noise floor. The Pico's
`abs_max_temp_c` follows the zone maxima, and it moves by the same amount. The only fix is lossless GET printing
(`%.9g`), as was done for PID gains. Not caused by this commit.

### INFO-2: the 0.05 read-back tolerance is loose for `hyst_c` and is not pinned (M6b MISSED)

GET prints `hyst_c` at `%.3f`, so a correct restore reads back identically to the snapshot. A 0.05 window
accepts a restore that left HP-03's `hyst_c=2.0` where the operator had anything in 1.95..2.05. A per-field
tolerance (0.001 for `hyst_c`, 0.05 for `max_temp_c`) would close this. Adding a 0.1 C `hyst_c` mismatch to
`test_readback_checks_every_restored_field` would pin it. Low impact, because `hyst_c` is inert while the zone
is a heater.

### INFO-3: test-fake limits

- `_FakeZonesHttpClient.strip_omit_preserved` returns the body unchanged plus markers, so no heat-case test sees
  a real form body. This is acceptable because real stripping is tested in `test_pctools_write_review_2026_10_09.py`.
- `_GenerationFakeZhc` applies `merged` (pre-strip) and models neither print rounding nor required-field 400s.
- The read-back now spans `_READBACK_FIELDS` on every zone, not just the target. A legitimate edit to another
  zone's `hyst_c` or `max_temp_c` during the case would count as a failed restore and taint the run. That is the
  conservative direction.

### INFO-4: stale docstring

`_post_zones_restore`'s docstring still says the read-back compares "the same `zone_type`/`max_temp_c` per
zone". It now compares `_READBACK_FIELDS`.

## Prior findings

Every REVIEW_BENCHFX_2026-10-11 finding is fixed as its Fix status table states. H1, H3, H5, H8 and A8 are
CAUGHT above, and INFO-1 is confirmed by `test_live_get_without_generation_posts_without_token`.
