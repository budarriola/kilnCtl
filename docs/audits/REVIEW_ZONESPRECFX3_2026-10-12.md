# Review: ZONESPRECFX3 (REVIEW_OLDPOOLA LOW-5 fix), 2026-10-12

Adversarial review of the origin/dev commits for REVIEW_OLDPOOLA LOW-5:

- `f3f4f7353` zones preset: reject float for int fields in validator; verify never raises KeyError
- `dc52235df` audit: mark OLDPOOLA LOW-5 fixed
- `c58cc350e` test: top-level float-on-int verify path

The fix is correct for the case it targets. `config_presets._validate` now
refuses a float on any `_ZONE_INT_FIELDS`/`_TOP_INT_FIELDS` key at load time,
before any UART or HTTP write. `_verify_against_preset` turns an unmapped float
into a mismatch report instead of a `KeyError`. No HIGH or MED findings.

**Counts: HIGH 0, MED 0, LOW 2, INFO 6.**

## LOW-1 (FIXED 2026-10-12): the validator checks only `float`; bool and string values on int fields still get past it

`tools/PcTools/src/kilnctrl/config_presets.py:199` and `:204` test only
`isinstance(..., float)`. Other non-int JSON types still pass:

- **bool.** `bool` is a subclass of `int`. `"control_mode": true` passes the
  validator. `_format_scalar` (`zones_http_client.py:755-756`) posts
  `str(int(True))` = `"1"`, and `_verify_against_preset` (`:1329`) compares
  `1 == True`, which is True. The preset silently writes mode 1 and reports a
  clean verify. The same happens with `"relay_mask": false` (zone left with no
  relay) and `"thermo_count": true`. A preset author who mistakes an enum for a
  flag gets no warning at any stage.
- **string.** `"control_mode": "2"` passes, posts `2`, and then verify reports a
  false mismatch after the write (`"2" != 2`). This is the same post-write
  false-mismatch shape LOW-5 was filed for.
- **non-numeric string.** `"control_mode": "2.5"` and `"control_mode": "false"`
  pass the validator. `int(value)` in `_format_scalar` (`:756`) then raises
  `ValueError`, which is not a `ZonesHttpError`. Inside `apply_preset` this
  happens after the UART PID write (`config_presets.py:480`) and before the
  POST. The exception escapes every `except` clause in `load_config_preset`
  (`mcp_server_config_presets.py:156-163`, which catch only ControlQueryError and the three `*HttpError` types).
  That is the "unexpected exception, contrary to the docstring" consequence
  LOW-5 described, reached through a different JSON type.

No shipped preset uses any of these. Verified by scanning every zone key and
value type in the 8 real presets: all int-field values are plain `int`.

Fix: require `type(v) is int` for int fields. That one check rejects float,
bool and str, and NaN/inf with them. Add a test for `True` and for `"2"`.

## LOW-2 (FIXED 2026-10-12): bool-encoded fields (`failsafe_state`, `continue_on_zone_trip`) are not validated, and truthiness inverts a string `"false"`

`_format_scalar` (`zones_http_client.py:748`) encodes these two fields as
`"1" if value else "0"`. The validator checks neither of them:

- `"failsafe_state": "false"` posts `"1"`. That is the opposite of what the
  author wrote, for the state an on/off device takes on a fault. Verify then
  reports `expected 'false', board reports True` after the board is already
  written.
- `"failsafe_state": 1.0` (or `"continue_on_zone_trip": 1.0`) passes the
  validator and posts `"1"`. Before this fix, verify raised `KeyError` here.
  Now the new `ok = False` branch (`:1324`/`:1303`) fires, so verify reports
  `expected 1.0, board reports True` even though the board holds exactly the
  intended value. This is the same LOW-5 false mismatch, still reachable for
  the two bool-encoded fields because they are in neither int set nor the float
  print table.

Neither case is silent, because verify reports a mismatch. But the write has
already landed, and in the string case it landed inverted. No shipped preset
sets either field.

Fix: in `_validate`, require `isinstance(v, bool)` for `failsafe_state` and
`continue_on_zone_trip` when present, the same rule `ramp_assist_enabled`
already gets at `config_presets.py:208`.

## INFO-1: the int-field sets match the firmware POST parser

Compared with `zones_http_post_parse.c`, every field parsed with
`zones_config_json_parse_u8_field` is in `_ZONE_INT_FIELDS`: `tctype` (:128),
`relay_mask` (:175), `relaytype` (:208), `zonetype` (:235), `timingprofile`
(:341), `thermo_mask` (:369), `ct_mask` (:396), `mode` (:538), and
`settings_source` plus its per-group keys (:1030/:1042). The one exception is
`failsafe` (:249), which is deliberately encoded as a bool (see LOW-2).
`minons`/`minoffs` are parsed as floats but printed as integers, and they are in
the set correctly. On the top-level side, `zones_http_post.c:383/388/399` parse
`thermo_count`, `relay_count` and `max_simultaneous_relays` as integers, which
matches `_TOP_INT_FIELDS`. `pc_link_abort_silence_ms` is parsed as a float
(:549) and has a `%.0f` entry in the print table.

Every int-typed key in the shipped presets is either in an int set or in
`_FLOAT_FIELD_PRINT_FORMAT` (for example `guard_sensor_fault_debounce_ticks`,
`heater_*_ms`), so none is wrongly refused.

## INFO-2: shipped presets load the same before and after the fix

There are 11 JSON files under `tools/PcTools/config_presets/`. The 8 zone
presets load and validate: `bench_fixture`, both `coupling_matrix_*`, both
`easeoff_ab_*`, both `fuzzy_ab_*`, and `ui_test_baseline`. The other 3 are
`noise_floor`, `tuned_baseline_20260831` and `tuning_recommendations`. They are
data files (`schema_version`/`generated_from`), not presets. They fail
`missing required field 'name'` the same way at `f3f4f7353^` and at
`c58cc350e` (checked by running `list_presets()` against both trees). The fix
introduces no regression. `list_presets()` already shows those 3 as errors;
that behavior predates this change.

## INFO-3: NaN and infinity

`json.load` accepts `NaN` and `Infinity`.

- **Int fields.** The new check rejects both, since both are floats. Before the
  fix, `int(inf)` raised `OverflowError` and `int(nan)` raised `ValueError`
  inside `_format_scalar`, the same escaping-exception shape as LOW-1. So the
  fix also closes this path.
- **Float fields.** `"pid_kp": NaN` still passes the validator. The board
  refuses it on both write paths: the UART `zones_config_set_pid_checked`
  checks `isfinite` (`zones_config_accessors.c:697`), and HTTP
  `zones_config_json_parse_float_field` uses `http_form_parse_float`, which
  requires a finite value (`zones_config_json.c:766-779`). The refusal is
  loud: `pid_ok=False` / a 400 error. A load-time `math.isfinite` check would
  be cheap defense in depth, but nothing is silent today.

## INFO-4: no production caller bypasses `_validate`

Every production path reaches `apply_zone_preset` with a dict from
`load_preset_data`:

- `load_config_preset` (`mcp_server_config_presets.py:147`)
- `run_queue.run_entry` (`run_queue.py:1320`) and `_resolve_preset` (`:1119`),
  which feeds `_apply_preset_http_only` (`:1077`)
- `ui_test_runner.py:271` and `mcp_server_ui_test.py:248`

`coupling_workflow.py:463` runs `_validate` on a preset before writing it to
disk. The callers that skip validation are the test-injected `apply_preset_fn`
path in `run_queue` and the bench_test helpers that call `build_post_body`
directly with code-built dicts (`cases_heat.py:1125`, `cases_aux.py:491`,
`cases_web_prof.py:670`). Those dicts are written in code, not loaded from user
JSON.

## INFO-5: the verify change does not weaken ZONESPREC LOW-3

The new branch makes an unmapped float a mismatch instead of a `KeyError`. A
missing print-table entry for a real float field would now show up as a
confusing post-write mismatch rather than a crash. However, LOW-3's "force a new
float field into the table" rule is still enforced statically by
`test_every_preset_float_field_is_mapped_or_nonfloat` and
`test_every_top_level_float_field_is_mapped`
(`tests/test_zones_http_float_tolerance_pin.py:29-39`). `_float_close` itself
still raises for an unmapped name (`test_unmapped_named_field_raises`). So the
gap is caught before any board write.

## INFO-6: negative tests

Ran `tools\negtest.ps1 -Preset pytest -PresetArg tests/test_config_presets.py`
at base `c58cc350e`, with `-ExpectPattern '(?m)^E\s+AssertionError'`. The
mutation JSON was kept in a private scratch directory. The unmutated baseline
passed. Verdict `ALL_CAUGHT`, `real_tree_unchanged: true`, `copies_removed: true`.
Each mutant ran to a normal pytest summary (`1 failed, 40 passed`). In every
case the failure was a test assertion, not an import or collection error.

| Mutation | Origin | Verdict | Failing assertion |
|---|---|---|---|
| zone validator check disabled (`config_presets.py:199`) | fixer | CAUGHT | `AssertionError: ConfigPresetError not raised` |
| zone verify `ok = False` branch removed (`zones_http_client.py:1324`) | fixer | CAUGHT | `AssertionError: KeyError escaped verify: ... 'control_mode' ...` |
| top-level verify branch removed (`zones_http_client.py:1303`) | reviewer | CAUGHT | `AssertionError: KeyError escaped top-level verify: ... 'thermo_count' ...` |
| top-level validator check disabled (`config_presets.py:204`) | reviewer | CAUGHT | `AssertionError: ConfigPresetError not raised` |

A first run used `^FAILED \S+ - AssertionError`. pytest's `-rf` summary has no
` - msg` suffix, so that pattern scored all four as MISSED even though each log
showed the assertion. The table above comes from a clean rerun with the
corrected pattern.
