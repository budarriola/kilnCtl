# Review ZONESPRECFX6 (2026-10-12)

Scope: zonesprecfx6's fixes for REVIEW_ZONESPRECFX5 MED-1 and LOW-1/2/3 on
origin/dev (ddf044e12), in `tools/PcTools/src/kilnctrl/config_presets.py`,
`tools/PcTools/src/kilnctrl/mcp_server_config_presets.py`,
`tools/PcTools/tests/test_config_presets.py` and
`tools/PcTools/tests/test_pctools_write_review_2026_10_09.py`. Reviewed at ddf044e12.

Result: **0 HIGH, 1 MED, 3 LOW, 3 INFO.** The four fixes do what the commit says for
the inputs the tests use. The MED is that the test input is a stale GET capture: a
preset copied from a GET of current firmware is still refused, as MED-1 described.

## Questions asked

1. **Can a real writable field hide in `_PRESET_ZONE_KNOWN_IGNORED_FIELDS`?** Not
   today. That set and `_PRESET_ZONE_OVERRIDE_FIELDS` do not intersect (checked at
   ddf044e12), and `build_post_body` tests the override set first, so a key in both
   would be written but skip validation. Nothing pins the sets disjoint (INFO-1).
   `k_dc`/`tau_s`/`dead_time_s` are carved out correctly: they are the only ignored
   keys `apply_preset` writes (UART `set_zone_model`), and the validator still
   requires them to be finite non-bool numbers. The GET spellings
   `model_k_dc`/`model_tau_s`/`model_dead_time_s` are skipped, which is right because
   nothing writes them. `settings_source` is in the ignored set but is still checked
   as an int first. One written field is left unvalidated: zone `name` (LOW-3).
2. **ease_off_window_mult / pc_link_abort_silence_ms bounds.** The firmware refuses,
   it does not clamp. See LOW-1 for the gap and why it is not silent.
3. **Partial-note wording.** Honest for the zone-0 raise and for a mid-stage raise.
   It is wrong in one case: an unexpected exception from the post-loop PID read-back
   (LOW-2). A failed read-back verify is not an exception path and is reported
   honestly by `describe()` (INFO-2).
4. **Real GET capture, current?** It is a real capture, but from 2026-09-02
   (757b97857). It predates about 15 per-zone keys the firmware's `/api/zones` now
   emits. See MED-1.

## MED-1: the GET-capture fixture is stale, so MED-1's fix is proven only for an old firmware shape

**FIXED in zonesprecfx7 in beca47b9d.**

`test_get_capture_zones_load_as_preset` runs `tests/fixtures/zones_get_capture.json`
through `_validate`. That fixture was captured 2026-09-02 and has none of the keys
`zones_get_handler()` (`firmware/KilnFW/App/drivers/http/zones_http_get.c`) emits per
zone today: `relay_type`, `fuzzy_model_valid`, `zone_type`, `failsafe_state`,
`hyst_c`, `min_on_s`, `min_off_s`, `coupling_diag_k_dc`, `ease_off_window_mult`,
`approach_rate_cap_c_per_hr`, `error_band_c`, `rate_band_c_per_s`, `progress_band_c`,
`autotune_baseline_k_dc`, `coil_power_w` and `settings_source_groups`. Its top level
also lacks `generation`, `relay_types`, `safety_ceiling` and the other keys added
since.

Probe at ddf044e12 (scratch script, fixture zones plus those keys with
current-firmware types):

- `_validate` refuses: `zones[0].fuzzy_model_valid must be a finite number ...,
  got True`. `fuzzy_model_valid` is a JSON bool (`zones_http_get.c:491`) and is in
  `zones_http_client._ZONE_READONLY_KEYS` but not in
  `_PRESET_ZONE_KNOWN_IGNORED_FIELDS`, so the new skip does not cover it. This is the
  same refusal MED-1 reported for `normal_current_measured`.
- With `fuzzy_model_valid` removed, `_validate` passes. `build_post_body` then raises
  `ZonesHttpUnknownFieldError` for `relay_type`, `progress_band_c`,
  `autotune_baseline_k_dc` and `coil_power_w`. These are in neither preset set.
  `apply_preset` calls `build_post_body` only after the UART PID/model stage, so this
  path is a partial write. The partial note does report it.

The first refusal is fail-closed. The second is a partial apply, but it needs the
operator to have deleted one key by hand. That combination is MED, the same rating
as the original MED-1. The `build_post_body` gap predates this change. The new
validator's claim that "a GET-copied preset now loads" holds only for a September
GET.

**Fix:** refresh the fixture from a current `GET /api/zones` (read-only, bench), or
add a second, current fixture. Add `fuzzy_model_valid`, `autotune_baseline_k_dc`,
`relay_type`, `progress_band_c` and `coil_power_w` to the preset ignored or override
set, whichever is right for each. A writable field such as `relay_type` or
`coil_power_w` probably belongs in `_PRESET_ZONE_OVERRIDE_FIELDS`. Then make
`_validate` check every zone key against the same two sets that `build_post_body`
uses, so an unknown key is refused before the UART stage, not after it. A drift test
that derives the per-zone key list from `zones_http_get.c` would keep the fixture
honest.

## LOW-1: validated range is wider than the firmware's, so an in-range-looking value still causes a partial write

**FIXED in zonesprecfx7 in beca47b9d.**

Firmware bounds (refuse, never clamp):

- `pc_link_abort_silence_ms`: `zones_http_post.c:549` parses against
  `[0, ZONE_PC_LINK_SILENCE_MS_MAX]` = `[0, 600000]` (`zones_config_accessors.h:451`)
  and answers 400 otherwise. `zones_config_json.c:260` repeats the check.
- `z%u_easeoffmult`: `zones_http_post_parse.c:869-880` takes `0` (firmware default)
  or `[0.1, 10.0]` (`ZONE_EASE_OFF_WINDOW_MULT_MIN/MAX`, `zones_config_json.h:99-100`)
  and refuses the whole POST otherwise.

The new top-level check only requires finite and `>= 0`. These all validate at
ddf044e12: top-level `ease_off_window_mult` 0.05 or 50.0,
`pc_link_abort_silence_ms` 1e9. Per-zone `ease_off_window_mult` goes through the
generic float loop, which accepts -3.0 as well.

**Is it silent?** No. The POST gets a 400, `post_zones` raises `ZonesHttpError`, and
the MCP tool reports the error with the PID stage in the partial note. Read-back
verification is not reached in that case. It would catch a clamp: the legacy key is
expanded per zone before `_verify_against_preset`, and the print-step tolerances are
0.0005 (`%.3f`) and 0.5 (`%.0f`). But the PID gains have already been written over
UART. That is the partial apply LOW-1 of ZONESPRECFX5 set out to prevent.

**Fix:** check the firmware ranges in `_validate`, for the top-level keys and for the
per-zone `ease_off_window_mult`: `0 or [0.1, 10.0]`, and `[0, 600000]`. The same
holds for every other per-zone range, which is a wider pre-existing class. A pinned
mirror test against the firmware `#define`s would stop drift.

## LOW-2: an unexpected exception in the post-loop PID read-back reports "no stage had confirmed landing" after every zone's PID was ACKed

**FIXED in zonesprecfx7 in beca47b9d.**

`_apply_preset_stages` calls `_verify_pid_readback` after the per-zone loop, outside
its `try`, and before the `done.append("PID/model written over UART: ...")`.
`_verify_pid_readback` catches only `ControlQueryError` and `AttributeError`. Any other
exception leaves `done` empty, so `preset_partial == ""`. One example is a `TypeError`
from `_pid_matches` when a GET_ZONES field is None. The new wording then reads:

```
error: TypeError: unsupported operand type(s) for -: 'float' and 'NoneType'
no stage had confirmed landing before the failure; a write in flight may still have applied
```

Probe at ddf044e12: a fake control ACKed all three `set_zone_pid` calls, and the
output above is exactly what the tool returned. The PID gains for every zone were
written. The note says at most one write may have landed. This is narrow (it needs an
unexpected exception type from the read-back), but the generic `except Exception`
branch exists for exactly that class.

**Fix:** record the ACKed PID writes in `done` before the read-back, or put the
read-back inside the stage's `try` so its `except` path reports `results`.

## LOW-3: zone `name` is written but never type-checked

**FIXED in zonesprecfx7 in beca47b9d.**

`name` is in `_ZONE_NON_FLOAT_KEYS`, so `_validate` skips it, and nothing else checks
it. It is in `_PRESET_ZONE_OVERRIDE_FIELDS`, so `build_post_body` writes it. The
following all pass `_validate` and fail only in `_format_scalar` or at the firmware,
after the UART stage: `None` (`ZonesHttpError`), `""` (`ZonesHttpError`), a list
(`float()` `TypeError`), or an over-long string (firmware 400). No shipped preset
sets `name` on a zone. A GET-copied preset always carries it, but as a valid string.

**Fix:** require a non-empty `str` within the firmware's length limit in `_validate`.

## INFO-1: nothing pins the ignored and override sets disjoint

**FIXED in zonesprecfx7 in beca47b9d.**

`_validate` skips `_PRESET_ZONE_KNOWN_IGNORED_FIELDS`. `build_post_body` writes any
key in `_PRESET_ZONE_OVERRIDE_FIELDS` and checks that set first. If a future change
adds a key to both sets, that key would be written without validation. The sets are
disjoint today. A one-line test assertion would keep them that way.

## INFO-2: the paths that do not raise

**FIXED in zonesprecfx7 in beca47b9d.**

- A zones read-back mismatch (`ZonesApplyResult.ok == False`) does not raise.
  `apply_preset` continues to the ramp-assist and safety stages, and the tool
  returns `FAILED (partial apply):` with the mismatches. That is honest. Continuing
  after a mismatch is a design choice that predates this change.
- A `get_zones` raise during that verify (after a POST answered `ok`) reports the PID
  stage as landed and says the failing stage's own write "may also have applied". In
  that case the POST did apply, so the wording understates.
- `mcp_server_ui_test._partial_note` still prints nothing for an empty
  `preset_partial`. Before this change it printed "(nothing had landed yet)". Its
  factory-reset message for the zone-0 case now has no in-flight caveat, and its
  `ZonesHttpError` message still says "PID/model applied" even when the PID
  read-back marked a zone FAILED.

## INFO-3: mid-stage wording

**FIXED in zonesprecfx7 in beca47b9d.**

A raise on zone N after zones 0..N-1 were written produces
`PID written over UART before the failure: zone 0 pid=ok, ...` under the header
"stages that had ALREADY landed ... (board is partially written; the failing stage's
own write may also have applied)". That is honest. If the best-effort read-back
marks an earlier zone `pid=FAILED`, the header still says "ALREADY landed" above a
FAILED line. That is minor, and the line itself is accurate.

## Negtest

`tools
egtest.ps1 -Preset pytest -PresetArg "tests/test_config_presets.py
tests/test_pctools_write_review_2026_10_09.py"` at ddf044e12, 11 mutants. The
baseline passed (90 tests), `real_tree_unchanged: true` and `copies_removed: true`.
Every CAUGHT log shows the mutant imported and a test failed on behaviour, not on
import. Ten failed on an `AssertionError`. M1 failed because the test under
mutation raised the validator's own `ConfigPresetError` on the fixture, which is the
refusal the test exists to prevent.

| # | Mutation | Verdict | Caught by |
|---|---|---|---|
| M1 | drop the ignored-field skip in the zone float loop | CAUGHT | `test_get_capture_zones_load_as_preset` (ConfigPresetError on `normal_current_measured`) |
| M2 | empty `_ZONE_NUMERIC_IGNORED_EXCEPTIONS` | CAUGHT | `test_get_capture_zones_load_as_preset` |
| M3 | drop the top-level `< 0` check | **MISSED** | no test uses a negative top-level value |
| M4 | drop the legacy ease-off key from the top-level loop | CAUGHT | `test_top_level_float_fields_validated` |
| M5 | skip `pc_link_abort_silence_ms` in the top-level loop | CAUGHT | `test_top_level_float_fields_validated` |
| M6 | empty partial back to "(nothing had landed yet)" | CAUGHT | `test_zone0_pid_raise_does_not_claim_partial_write`, `test_first_zone_raise_says_nothing_landed` |
| M7 | empty partial prints nothing | CAUGHT | `test_zone0_pid_raise_does_not_claim_partial_write` |
| M8 | drop "the failing stage's own write may also have applied" from the non-empty header | **MISSED** | no test asserts that caveat |
| M9 | suppress the post-loop `done` PID record | CAUGHT | `test_unexpected_exception_after_pid_stage_reports_partial`, `test_value_error_after_pid_stage_reports_partial` |
| M10 | suppress the except-path PID record | CAUGHT | `test_raise_on_zone2_reports_zones_0_and_1_and_reads_back`, `test_readback_mismatch_shown_in_partial` |
| M11 | let bool through the zone float loop | CAUGHT | `test_get_capture_zones_load_as_preset`, `test_validator_rejects_non_finite_or_non_numeric_floats` |

Two gaps follow from the MISSED rows. They are folded into the findings: M3 under
LOW-1 (add a negative and an out-of-range case to `test_top_level_float_fields_validated`)
and M8 as a test gap on the LOW-3 fix of ZONESPRECFX5 (assert the in-flight caveat on a
non-empty partial).
