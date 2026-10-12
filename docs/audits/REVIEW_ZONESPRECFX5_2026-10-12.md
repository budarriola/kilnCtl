# Review ZONESPRECFX5 (2026-10-12)

Scope: the zonesprecfx5 fixes for REVIEW_ZONESPRECFX4 LOW-1/2/3 on origin/dev
(1bbe796db, f72ecef8c, 944f3c159, status doc 0b691b6c7), in
`tools/PcTools/src/kilnctrl/config_presets.py`,
`tools/PcTools/src/kilnctrl/mcp_server_config_presets.py` and
`tools/PcTools/tests/test_config_presets.py`. Reviewed at daa74cef1.

Result: **0 HIGH, 1 MED, 3 LOW, 2 INFO.** `tests/test_config_presets.py`: 53 passed.

## Questions asked

1. **Does the "every other zone field must be a number" default reject a real preset?**
   No shipped preset. All 8 listed presets load cleanly: bench_fixture,
   coupling_matrix_20260831, coupling_matrix_pre20260902, easeoff_ab_2p0/3p0_20260903,
   fuzzy_ab_baseline/strength50_20260903 and ui_test_baseline. The 3 `NON_PRESET_FILES`
   are refused by name. The default does reject a documented preset shape. See MED-1.
2. **Are top-level float fields still unchecked?** Yes. See LOW-1.
3. **What does `except Exception` do with the error?** The handler returns an
   `error: <Type>: ...` string with the partial note. It does not re-raise, which
   matches the convention of every other branch in this tool, so the MCP result
   still reports a failure. `KeyboardInterrupt` and `SystemExit` are not
   `Exception` subclasses and still propagate. See INFO-1.
4. **Is the partial note correct when the failure comes before any write?** No.
   The header contradicts its own body. See LOW-2.
5. **Do the tests prove what they claim?** Mostly yes. Negtest M9 shows that the
   partial-note test cannot tell a real PID-stage record from "nothing landed".
   See LOW-3.

## MED-1: generic float rule refuses a preset captured from a board GET or backup

`config_presets.py:191` (`_ZONE_NON_FLOAT_KEYS`) and `config_presets.py:227-233`
(the `for key, val in zone.items()` loop) now require every zone key that is not
excluded to be a finite non-bool number. However,
`zones_http_client._PRESET_ZONE_KNOWN_IGNORED_FIELDS` (`zones_http_client.py:1041`)
explicitly tolerates the read-only telemetry keys that `GET /api/zones` always
emits. That module's comment calls copying a live GET or a backup export "the most
natural way to author" a preset, and says these keys are "reference-only: ignored,
never overlaid". Five of them are JSON bools: `normal_current_measured`,
`tuning_valid`, `tuning_settled`, `tuning_extrapolation_converged` and
`tuning_tau_consistent`.

**Failure scenario:** an operator saves the zones from
`tests/fixtures/zones_get_capture.json` (a real GET capture) as a preset with
`name` and `ramp_assist_enabled`. Before the fix it validated and applied, and
`build_post_body` ignored those keys. It is now refused:

```
preset 'x': zones[0].normal_current_measured must be a finite number (int or
float, not bool/str/list/NaN/inf), got False
```

The fix is fail-closed (it refuses, it never writes), and no shipped preset carries
these keys, so this is MED, not HIGH.

**Fix:** skip the keys in `_PRESET_ZONE_KNOWN_IGNORED_FIELDS` other than
`k_dc`/`tau_s`/`dead_time_s`, which are written over UART and must stay numeric.
Alternatively, allow bool for the five tuning/current flags. Add a test that runs
the GET-capture fixture's zones through `_validate`.

## LOW-1: top-level float fields still unchecked

`config_presets.py:245-257` checks only `_TOP_INT_FIELDS`, `continue_on_zone_trip`
and `ramp_assist_enabled`. These values for the two top-level float fields
`build_post_body` consumes all validate:

- legacy `ease_off_window_mult`: `"abc"`, NaN
- `pc_link_abort_silence_ms`: inf, `"x"`, `True`

`ease_off_window_mult` is expanded onto every zone. `_format_scalar` passes a str
through verbatim and encodes NaN as `"nan"`.

**Failure scenario:** a typo such as `"ease_off_window_mult": "2.0x"` passes
validation. The PID stage writes over UART, then the zones POST carries
`easeoffmult=2.0x` and fails or is misparsed. The result is the partial apply that
LOW-2 of REVIEW_ZONESPRECFX4 meant to rule out for float fields.

**Fix:** apply `_is_finite_number` to every top-level key in
`zones_http_client._TOP_FIELD_FORM_KEY` and to the legacy ease-off key, except the
int and bool keys.

## LOW-2: the partial note claims a partial write when nothing landed

`apply_preset` sets `preset_partial = "(nothing had landed yet)"` when `done` is
empty (`config_presets.py:510`). `_partial_note` only checks for a truthy value, so
it prints:

```
stages that had ALREADY landed before the failure (board is partially written):
(nothing had landed yet)
```

(`mcp_server_config_presets.py:174`). The header says the board is partially
written, and the body says nothing landed. The generic `except Exception` branch
added in zonesprecfx5 now routes every unexpected error through this text, for
example a `TypeError` raised by zone 0's own `set_zone_pid`.

"Nothing had landed yet" also overclaims for that case. A UART write that raised
after sending (a lost ACK, for instance) may still have applied.

**Fix:** when nothing confirmed landing, print "no stage had confirmed before the
failure; a write in flight may still have applied" without the "partially written"
header. Add a test that drives a zone-0 `set_zone_pid` raise through
`load_config_preset`. This behaviour predates the fix (2026-10-09); this change
widens its reach.

## LOW-3: the partial-note test does not check what landed

`tests/test_config_presets.py:185-197`
(`test_unexpected_exception_after_pid_stage_reports_partial`) asserts `TypeError`,
`ALREADY landed` and `board is partially written`. Per LOW-2, all three strings
also appear when `preset_partial` is "(nothing had landed yet)". Negtest M9
suppressed the `done.append("PID/model written over UART: ...")` record, and the
whole file still passed (MISSED). The test name promises that the PID stage is
reported, and that is not checked.

**Fix:** also assert `"PID/model written over UART"` and the zone list
(`"zone 0 pid=ok"`) in `out`. Do the same in `test_value_error_after_pid_stage_reports_partial`.

## INFO-1: what actually reports an exception that escapes the tool

The `_core._tool()` wrapper also converts a `ValueError` that escapes
`load_config_preset` into `error: <msg>`, with no type name and no partial note.
Negtest M2 showed this: it narrowed the handler to `except TypeError`, and the tool
returned `error: bad literal`. Inside the `try` this can no longer happen. Outside
it (`_ota_resolve_host`, `load_preset_data`) no write has happened yet, so no
partial note is owed.

## INFO-2: the `index` and coupling checks

`type(idx) is int` refuses bool indexes. Negtest M7 confirms this, and the
single-zone test means a duplicate-index error cannot mask the result.
`coupling_coeff` is checked as a list of finite non-bool numbers. A bare string
such as `"abc"` is refused by both the list check and the per-cell check. The
scalar `coupling_cN` keys go through the generic float loop.

## Negtest

`tools\negtest.ps1 -Preset pytest -PresetArg tests/test_config_presets.py`, base
daa74cef1. The unmutated baseline passed, and `real_tree_unchanged` was true. I
checked every CAUGHT log for `AssertionError`, and none of the CAUGHT results came
from a crash or import error.

| # | Mutation | Verdict | Caught by |
|---|---|---|---|
| M1 | generic handler drops `_partial_note` | CAUGHT | test_unexpected_exception_after_pid_stage_reports_partial, test_value_error_after_pid_stage_reports_partial |
| M2 | `except Exception` narrowed to `except TypeError` | CAUGHT | test_value_error_after_pid_stage_reports_partial |
| M3 | `_is_finite_number` drops `math.isfinite` | CAUGHT | test_validator_rejects_non_finite_or_non_numeric_floats |
| M4 | `_is_finite_number` accepts bool | CAUGHT | test_validator_rejects_non_finite_or_non_numeric_floats |
| M5 | generic float loop never refuses | CAUGHT | test_validator_rejects_non_finite..., test_bad_float_preset_through_loader_never_writes |
| M6 | `coupling_coeff` cell check removed | CAUGHT | test_validator_rejects_non_finite_or_non_numeric_floats |
| M7 | index check back to `isinstance(idx, int)` | CAUGHT | test_validator_rejects_bool_index |
| M8 | `pid_kp`, `max_temp_c` added to `_ZONE_NON_FLOAT_KEYS` | CAUGHT | test_validator_rejects_non_finite..., test_bad_float_preset_through_loader_never_writes |
| M9 | PID-stage `done.append(...)` suppressed | MISSED | (LOW-3) |
| M10 | NON_PRESET refusal replaced by `pass` | CAUGHT | test_non_preset_files_are_not_listed_and_refuse_load |
