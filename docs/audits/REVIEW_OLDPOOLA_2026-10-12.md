# Review OLDPOOLA: Pico uptime-regression bound and zones float tolerance (2026-10-12)

Scope: dev commit `78d3082d8` (REBOOTFX2 LOW-3 physical bound in
`safety_pico_uptime_behind_expected()`; ZONESPREC LOW-2/3 per-field float
tolerance in `zones_http_client.py`) and its docs commit `e0831b5a2`.
Reviewed at `e0831b5a2` (origin/dev). Adversarial review, no code changed.

Verdict: no HIGH or MED. Two LOW findings are new trade-offs introduced by
this commit, one LOW/MED is pre-existing and worth fixing now that the
function is being touched, two INFO. The test rewrites are legitimate, not
weakened. The docs commit's status claims match the code.

## Change 1: `safety_pico_uptime_behind_expected()` physical bound

`firmware/KilnFW/App/drivers/safety/safety_link_frame.c:51-65`. A DIAG
uptime that is more than `tol` behind `baseline + elapsed` now only counts
as a reboot when `now_ms <= esp_elapsed_ms + tol` (line 65): a rebooted Pico
cannot report more uptime than the ESP has seen elapse since the baseline,
plus tolerance. Caller: `safety_apply_diag`, `safety_link_frames.c:1131`.

### Can a real reboot now go undetected?

Physical argument holds for every reboot source: power-on, watchdog reset
and the slot-bootloader jump all restart `to_ms_since_boot()` at 0
(`firmware/SaftyFW/src/tasks/link_task.c` ~1095), so the new uptime is at
most the wall time since the reset, which is at most the wall time since
the baseline DIAG was *sent*. The gap is that `elapsed` is measured from
when the ESP *drained* the baseline frame, not when it was sent:

**LOW-1 (new, introduced by this commit): late-drained baseline hides an
early reboot.** If the baseline DIAG sat in the inbox for `d0` ms before
being stamped and the current DIAG is drained promptly (`d1` ~ 0), then
`elapsed` under-reads real time by `d0 - d1`. A reboot within roughly
`d0 - d1 - tol - pico_boot_ms` of the baseline being sent then gives
`now_ms > elapsed + tol` and is classified as "not a reboot". Model case:
`d0 = 3500 ms`, reboot 100 ms after the baseline was sent, 300 ms boot,
next DIAG sent 4 s later and drained immediately: `now = 3600`,
`elapsed = 500`, `tol = 2000`; old code counted the reboot, new code does
not. This is the mirror image of the false positive the commit fixes
(late-drained *current* DIAG). Mitigation already present: the FW_VERSION
boot_id branch (`safety_link_frames.c` ~400-440) and the protocol-18 V3
DIAG boot_id still catch it except on a boot_id repeat (8-bit id from
`get_rand_32()`, `link_task.c` ~3600-3606: 1 in 256). Needs two
coincidences (multi-second inbox stall on exactly the baseline frame, plus
a boot_id collision). Suggested hardening if wanted: stamp `esp_now_ms` at
UART receive time rather than drain time, which removes both the old false
positive and this false negative.

**LOW/MED-2 (pre-existing, not introduced here; cheap to fix now): 24.9
to 49.7 day wrap blind spot.** `diff = (int32_t)(now_ms - expected)` at
line 57 comes out *positive* when the Pico's pre-reboot uptime (baseline)
is between ~2^31 ms (24.86 days) and 2^32 ms (49.7 days) and the Pico
reboots to a small uptime, so line 58 returns `false` before the new bound
is ever consulted. Modelled: baseline `0xA0000000` (31 days), reboot to
500 ms: missed by both the old and new code. 20 d and 24.85 d detected;
24.9 d, 30 d, 49 d, 49.7 d missed. A kiln controller left powered for a
month is a realistic state. Again only the uptime path is blind; boot_id
still catches 255 of 256. Suggested fix, which the new bound makes
straightforward:

```c
uint32_t fwd  = now_ms - expected;          /* modular */
uint32_t back = expected - now_ms;          /* modular */
if (fwd <= (uint32_t)tol || back <= (uint32_t)tol) return false;
return now_ms <= esp_elapsed_ms + (uint32_t)tol;
```

This keeps the existing wrap test (`UINT32_MAX-600000, 900000, 299999`)
green and still rejects large forward steps as reboots only when they are
physically possible. Add a 31-day-baseline test case with it.

**INFO-3: frozen-uptime false positive survives in one corner.** A Pico
whose reported uptime froze within its first ~`elapsed + tol` ms still
satisfies line 65 and is counted as a reboot on each DIAG outside the note
window. The commit's LOW-3 tests (`test_safety_link_compile.c:1233`,
`:1235`) cover frozen values far above elapsed only. Not a safety problem
(over-detection), noting for completeness.

**INFO-4: bound boundary not pinned.** No test exercises
`now_ms == esp_elapsed_ms + tol` or `+1`. Mutations C2 (`<=` to `<`) and C4
(bound loosened to `2*tol`) both survive (table below). Low value, but a
two-line pair of cases at `(B, E, E+tol)` = reboot and `(B, E, E+tol+1)` =
not a reboot would pin it.

### Were the rewritten tests weakened?

No. The two replaced cases, `(5000, 0, 2999)` and `(20000, 300000,
310000)`, each had `now_ms > elapsed + tol`, i.e. they described a "reboot"
reporting more uptime than could physically have elapsed, so they encoded
the very false positive LOW-3 removes. Their replacements
(`test_safety_link_compile.c:1230` `(5000, 1000, 2999)`, `:1240`
`(20000, 300000, 100000)`, and the scenario at ~3496 using uptime
`100000`) still exercise the same intent (step back past tolerance; LOW-1
"uptime above old baseline but behind baseline+elapsed") with physically
possible values. The C5 mutant (bound against baseline instead of elapsed)
is caught at `:1240` and `:3500`, so the LOW-1 guarantee is still enforced.

### Does any other path depend on the old behaviour?

- Only two host test executables link `safety_link_frame.c`
  (`test_safety_link_compile.c`, `test_safety_link_endian.c`); the latter
  does not call this function.
- The only firmware caller is `safety_apply_diag`
  (`safety_link_frames.c:1131`). It refreshes the baseline from every DIAG
  regardless of the verdict, so a "not a reboot" verdict on a real reboot
  re-seeds onto the new boot and the uptime path stays blind to that reboot
  forever after (LOW-1/LOW/MED-2 are one-shot misses, not stuck states).
- boot_id paths are independent of this function. The double-count
  suppression between the boot_id and uptime paths is time-bounded by
  `SAFETY_REBOOT_NOTE_WINDOW_MS` (`safety_link_frames.c:429`, `:1148-1153`)
  and is unaffected by this change.
- ESP reboot clears the baseline; the first FW_VERSION counts via
  `!ever_seen`. Unaffected.
- `esp_elapsed_ms >= 0x80000000u` (line 53) returns `true` before the
  bound, so `esp_elapsed_ms + tol` at line 65 cannot overflow uint32.

## Change 2: `zones_http_client.py` per-field float tolerance

`tools/PcTools/src/kilnctrl/zones_http_client.py:1251-1289`.
`_FLOAT_FIELD_PRINT_FORMAT` now carries `pc_link_abort_silence_ms: "%.0f"`
(line 1255, matches `zones_http_get.c:279`), the absolute floor dropped
from 1.0 to `1e-12`, relative eps `1e-6`, and `_float_tolerance()` raises
`KeyError` for a named field with no map entry (line 1280).

**Tolerance: fine.** `1e-6` relative is above float32 eps (~6e-8), so a
float32 round-trip through firmware never false-fails. The `1e-12` floor
only hides mismatches below 1e-12 absolute, which is negligible, and keeps
a float32 denormal/flush-to-zero `0.0` vs tiny value from failing.

**Can a real `GET /api/zones` response raise the KeyError? No.**
`_verify_against_preset` (line 1291) iterates only preset-named keys drawn
from the fixed sets `_TOP_FIELD_FORM_KEY` (line 657) and
`_PRESET_ZONE_OVERRIDE_FIELDS` (line 848), and calls `_float_close` (lines
1304, 1323) only when the *preset's* expected value is a Python `float`.
Every float-printed field in those two sets has a map entry (pinned by the
new `test_every_top_level_float_field_is_mapped`). GET does print other
unmapped floats (`autotune_baseline_k_dc`, `bangbang_hysteresis_c`,
`coil_power_w`, `model_*`, `tuning_*`, timing-profile fields,
`tc_temp_c`, `target_c`, ...) but none of them is in either iterated set.
Int fields print `%u`, bools `%s`, `name` is a string.

**LOW-5 (new, introduced by this commit): float value for an int field
raises KeyError after the POST.** A preset that writes an int field as a
float (e.g. `"control_mode": 2.0`) is accepted by `config_presets._validate`
(`config_presets.py` 173-199, no type check on int fields), POSTs fine
because `_format_scalar` (line 740) coerces with `int(value)`, and then
`_verify_against_preset` passes the float to `_float_close` with an
unmapped int field name, raising `KeyError`. Before this commit the `%.9g`
fallback compared it equal. Affected fields: `control_mode`, `relay_mask`,
`ct_mask`, `thermo_mask`, `min_on_s`, `min_off_s`, `tc_type`,
`timing_profile`, `zone_type`, `thermo_count`, `relay_count`,
`max_simultaneous_relays`. Consequences: the board has already been written
(line 1409 `apply_zone_preset` POSTs first), no verification report is
produced, and `KeyError` is not `ZonesHttpError`, so callers that catch
`ZonesHttpError` (`config_presets.py:516`, `run_queue.py:1077`) see an
unexpected exception, contrary to the `apply_zone_preset` docstring. No
shipped preset under `tools/PcTools/config_presets/` (11 files, scanned)
has a float-typed int field, so latent only (hand-written or JSON-generated
presets with `2.0`). Suggested fix: in `_verify_against_preset`, compare a
float expected value for a known int field exactly after `int()` (or
reject it in `_validate`), keeping the KeyError for genuinely unmapped
float fields.

## Docs commit `e0831b5a2`

Marks REBOOTFX2 LOW-3 and ZONESPREC LOW-2/LOW-3 fixed and re-notes WEB7
LOW-8 as documented only. All three claims match the code at
`e0831b5a2`.

## Negative tests (`tools\negtest.ps1`, base `e0831b5a2`)

Every CAUGHT below was confirmed from its per-mutation log to have compiled
(no `BUILD FAILURES`, no compiler `error`) and failed on a real test
assertion (`FAIL file:line` for C, `AssertionError`/`KeyError` inside a test
body for Python). Baselines passed; `real_tree_unchanged: true` for both
runs.

C: `build_host_tests.ps1 -Only safety_link`, ExpectPattern
`(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES`; each mutation
replaces line 65 of `safety_link_frame.c`. Logs:
`%TEMP%\negtest_logs\20261012_071930_21mz`.

| Id | Mutation | Verdict | First assertion |
|----|----------|---------|-----------------|
| C1 | `return true;` (drop bound) | CAUGHT | `test_safety_link_compile.c:1233` LOW-3 frozen uptime |
| C2 | `<=` to `<` | MISSED | boundary not pinned (INFO-4) |
| C3 | `now_ms <= esp_elapsed_ms` (drop tol) | CAUGHT | `:1226`, `:1230`, `:1275`, `:3132`, `:3449`, `:3547` |
| C4 | bound `elapsed + 2*tol` | MISSED | boundary not pinned (INFO-4) |
| C5 | `now_ms < baseline_ms` | CAUGHT | `:1233`, `:1235`, `:1240`, `:3500` |

Python: pytest preset on `tests/test_zones_http_float_tolerance_pin.py`.
Logs: `%TEMP%\negtest_logs\20261012_071931_xzw8`. ALL_CAUGHT.

| Id | Mutation | Verdict | Assertion |
|----|----------|---------|-----------|
| P1 | unmapped field falls back to `%.9g` | CAUGHT | `test_unmapped_named_field_raises`: `KeyError not raised` |
| P2 | restore 1.0 abs floor | CAUGHT | `test_small_magnitude_mismatch_fails`: `True is not false` |
| P3 | remove `pc_link_abort_silence_ms` entry | CAUGHT | `test_every_top_level_float_field_is_mapped`, `test_fractional_abort_silence_reads_back_rounded` |
| P4 | abs floor `1e-6` | CAUGHT | `test_small_magnitude_mismatch_fails` |
| P5 | rel eps `1e-5` | CAUGHT | `test_tolerance_below_one_step`: `0.0001 not less than 0.0001` |
| P6 | `pc_link_abort_silence_ms` as `%.9g` | CAUGHT | `test_format_table_matches_firmware`, `test_fractional_abort_silence_reads_back_rounded` |

## Findings summary

| Id | Severity | Introduced here | Where |
|----|----------|-----------------|-------|
| LOW-1 | LOW | yes (trade-off) | `safety_link_frame.c:65`; drain-time stamping in `safety_link_frames.c:1131` |
| LOW/MED-2 | LOW/MED | no (pre-existing) | `safety_link_frame.c:57-58` int32 diff, 24.9-49.7 d wrap |
| INFO-3 | INFO | no | `safety_link_frame.c:65` frozen uptime early in boot |
| INFO-4 | INFO | yes (test gap) | `test_safety_link_compile.c` ~1226-1240, C2/C4 MISSED |
| LOW-5 | LOW | yes | `zones_http_client.py:1280` via `:1304`/`:1323`, after POST in `:1409` |
