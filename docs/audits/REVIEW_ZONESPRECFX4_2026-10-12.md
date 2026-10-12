# Review ZONESPRECFX4 (2026-10-12)

Adversarial review of the ZONESPRECFX3 LOW-1/LOW-2 fixes on origin/dev:

- `d72340615` Presets: int fields require exact int; `failsafe_state`/`continue_on_zone_trip` require bool
- `b0836e81e` test: shipped presets list
- `f9570e67d` audit doc: mark LOW-1/LOW-2 fixed
- `2d9cdae82` (presetfx1, added to scope by the coordinator): `NON_PRESET_FILES`, so `list_presets()` skips the three data files and `load_preset_data()` refuses them

Files: `tools/PcTools/src/kilnctrl/config_presets.py`, `mcp_server_config_presets.py`,
`zones_http_client.py` (`_format_scalar`), `tools/PcTools/tests/test_config_presets.py`.

**Counts: HIGH 0, MED 0, LOW 3, INFO 5.**

## Q1: int-field type checking is complete

Every u8 field that `zones_http_post_parse.c` / `zones_http_post.c` parse with
`zones_config_json_parse_u8_field()` or `http_form_parse_long()` is in
`_ZONE_INT_FIELDS` or `_TOP_INT_FIELDS`:

- per zone: `tc_type`, `relay_mask`, `relay_type`, `zone_type`, `timing_profile`, `thermo_mask`, `ct_mask`, `control_mode`, `settings_source`
- top level: `thermo_count`, `relay_count`, `max_simultaneous_relays`

`failsafe_state` (u8 0..1) and `continue_on_zone_trip` ("0"/"1") are now bool-checked.
`min_on_s`/`min_off_s` are float on the firmware side. The PC side is stricter and
requires an int, which is safe. The validator now rejects a float, bool, str, NaN
or inf on every int field. The `type(v) is not int` form also rejects bool. M9
below shows that the weaker `isinstance(v, int)` form would be caught.

Not covered: the nested `settings_source_groups` values. They are in
`_PRESET_ZONE_KNOWN_IGNORED_FIELDS`, so a preset never posts them (INFO-1).

## Q2: the `_format_scalar` bool guard breaks no caller

`_format_scalar` is called only inside `zones_http_client.py` (`_encode_zone`,
`_encode_timing_profile`, and the top-level loop in `build_post_body`). Every
`build_post_body` caller merges onto a live GET. `zones_http_get.c:315` and `:517`
emit both fields as JSON `true`/`false`. The only literal producers
(`cases_heat.py:964`, `:1563`) pass `False`. `control_set_zone_*`, `cases_aux.py`,
`cases_web_prof.py` and the aux tools never coerce these fields to int before
encoding. `cases_web_prof.py:710` converts them to int only on a local copy it
reads and never posts. Backup import is firmware-side (`POST /api/backup/import`)
and never reaches `_format_scalar`. Affected suites pass: 261 tests in
config_presets, zones_http_client, float_tolerance_pin and cases_heat, plus 406
tests in the preset, run_queue, preflight and ui_test files at `2d9cdae82`.

## Q3: no write happens before validation, and the partial-apply note is honest

Every `apply_preset()` caller loads the preset through `load_preset_data()`,
which runs `_validate()`, before any write: `load_config_preset`,
`factory_default_then_load_preset` (`mcp_server_ui_test.py:248`),
`ui_test_runner.py:271` and `run_queue.py:1119/1320`. So a type error the
validator covers never causes a write. A failure after the UART PID stage carries
`preset_partial`, which lists the stages that had landed. The note is accurate
wherever a handler prints it. LOW-1 covers the exceptions that bypass those
handlers.

## LOW-1: `load_config_preset` drops the partial-apply note for any exception it does not list -- FIXED (zonesprecfx5)

`mcp_server_config_presets.py:155-165` catches only ControlQueryError,
ZonesHttpError, SafetyCfgHttpError, RampAssistHttpError and (new) ValueError. A
TypeError raised after the UART PID stage goes to `mcp_server_core._tool`'s
generic handler instead. One example is `repr(float([80]))` in `_format_scalar`
for a list on a float field, which LOW-2 lets through. A non-numeric gain reaching
`devices.control_set_zone_pid` on zone N>0 does the same. The generic handler
prints `error: <msg>` without `_partial_note`, so a board with PID gains already
written reads as untouched. `factory_default_then_load_preset`
(`mcp_server_ui_test.py:294`) already ends with `except Exception` plus
`_partial_note`. `load_config_preset` should end the same way.

The new `except ValueError` branch has no test, and presets cannot currently
reach it: every int-field `int("x")` case is now rejected earlier. Negtest M8
removes the branch and the suite does not notice (MISSED).

## LOW-2: float fields are still not type-checked -- FIXED (zonesprecfx5)

`_validate()` checks the int and bool fields but none of the float fields. A
probe against `bench_fixture` showed that all of these validate cleanly:

- `max_temp_c: [80]`
- `pid_kp: "1.0"`
- `sanity_rate_c_per_min: NaN`
- `coupling_coeff: ["x", ...]`
- `index: true` (`isinstance(True, int)`)

Firmware refuses NaN (`http_form_parse_float` requires a finite value) and string
garbage with a 400. However, the UART PID stage has already written every zone by
then. The list, string and garbage-cell cases raise TypeError or ValueError
inside the client after the PID writes, which feeds LOW-1. Fix: require
`isinstance(v, (int, float)) and not isinstance(v, bool) and math.isfinite(v)`
for every `_REQUIRED_ZONE_FIELDS` float and every other float key the preset
names, apply the same rule to each `coupling_coeff` cell, and require
`type(index) is int`.

## LOW-3: two tests prove less than their names claim -- FIXED (zonesprecfx5)

- `test_bad_bool_field_never_writes` patches `post_zones` but calls only
  `_validate()`, which can never reach `post_zones`. It repeats
  `test_validator_rejects_non_bool_bool_fields` and never exercises
  `load_config_preset`/`apply_preset`. The fixer said so. A real version calls
  `load_config_preset(confirm=True)` against a temporary preset file and asserts
  that neither `set_zone_pid` nor `post_zones` was called.
- `test_non_preset_files_are_not_listed_and_refuse_load` asserts only
  `ConfigPresetError`. The three data files also fail validation ("missing
  required field 'name'"), so removing the explicit `NON_PRESET_FILES` refusal in
  `load_preset_data` goes unnoticed (negtest M7 MISSED). The test should assert
  the "is a data file" message. It should also assert that each
  `NON_PRESET_FILES` entry exists on disk, so a stale entry fails the test.

## Q4 and presetfx1: the three files are not presets, and a hardcoded list is the right fix

- `noise_floor.json` holds noise-floor statistics. `noise_floor.py` and `gen_noise_floor_panel.py` read it by path.
- `tuning_recommendations.json` is an analysis report that `gen_noise_floor_panel.py` generates. The firmware embeds it (`tuning_recommendations_json` EMBED_FILES).
- `tuned_baseline_20260831.json` is a raw `kind: kilnctl_backup` export (see `cplval75_coupling_verdict_2026-09-10.md` D1-D3).

Moving the files would mean touching CMake EMBED paths, scripts and audit docs, so
skipping them in place is reasonable. A content-based skip (no `name`, or
`kind != preset`) would also hide a genuine preset that lost its `name` field.
That is the "broken file silently vanishes from the listing" failure that
`list_presets()`'s own docstring forbids. With the explicit list, a new data file
dropped into the directory shows up as `error:` in the listing, and
`ShippedPresetsValidateTest.test_every_listed_preset_validates` fails, which
forces an explicit decision. Recommendation: keep the list (INFO-2).

No code loads these three files by preset name. `run_queue` /
`coupling_workflow` / `ui_test_runner` / `capability_preflight` take preset names
from callers. The only matches are the `noise_floor` run label in
`test_run_queue.py:1133` and audit-doc prose. `capability_preflight` against
`tuned_baseline_20260831` still refuses, now with the clearer "data file" message.
`b0836e81e`'s hardcoded eight-name list test is superseded by `2d9cdae82`'s
every-listed test. The old test is redundant but harmless (INFO-3).

## INFO

- INFO-1: The validator does not check `settings_source_groups` values. The apply path ignores them, so this is harmless. A non-int `settings_source` is refused even though it is also ignored. That is harmless too, since only an exported backup carries it.
- INFO-2: Keep `NON_PRESET_FILES` as an explicit list (see above).
- INFO-3: The `test_eight_shipped_presets_still_load` list is now redundant with `ShippedPresetsValidateTest`.
- INFO-4: `_format_scalar` passes a NaN on a float field through as `"nan"`. Firmware rejects it with a 400, so the failure is loud, but only after the PID stage (LOW-2).
- INFO-5: `load_preset_data`'s `NON_PRESET_FILES` check is case-sensitive. On Windows, `Noise_Floor` opens the file and then fails validation, so it is still refused.

## Negative tests

`tools\negtest.ps1 -Preset pytest -PresetArg tests/test_config_presets.py`, base
`2d9cdae82`, in throwaway worktrees. Baseline PASS, `real_tree_unchanged: true`.
Each CAUGHT was confirmed in its log as an AssertionError ("not raised"), with no
collection or import errors.

| # | Mutation | Verdict | Failing test(s) |
|---|---|---|---|
| M1 | zone int check back to float-only | CAUGHT | test_validator_rejects_bool_and_str_int_fields |
| M2 | top-level int check back to float-only | CAUGHT | test_validator_rejects_bool_and_str_int_fields |
| M3 | drop failsafe_state bool check | CAUGHT | test_validator_rejects_non_bool_bool_fields, test_bad_bool_field_never_writes |
| M4 | drop continue_on_zone_trip bool check | CAUGHT | test_validator_rejects_non_bool_bool_fields |
| M5 | `_format_scalar` bool guard -> pass | CAUGHT | test_format_scalar_refuses_non_bool_failsafe |
| M6 | `list_presets` no NON_PRESET skip | CAUGHT | test_every_listed_preset_validates, test_non_preset_files_are_not_listed_and_refuse_load |
| M7 | `load_preset_data` NON_PRESET refusal -> pass | MISSED | (masked by validation error, LOW-3) |
| M8 | remove `except ValueError` in load_config_preset | MISSED | (untested and unreachable, LOW-1) |
| M9 | int check `isinstance(v, int)` (accepts bool) | CAUGHT | test_validator_rejects_bool_and_str_int_fields |

## Fix status (zonesprecfx5, 2026-10-12)

LOW-1, LOW-2, LOW-3 fixed. `load_config_preset` now ends with `except Exception` (the unreachable `except ValueError` branch is folded into it) and appends `_partial_note`; `_validate` requires finite int-or-float (not bool) for every non-int zone field and each `coupling_coeff` cell, and `type(index) is int`; the no-write and data-file tests now exercise the real paths. Negtests (`-Preset pytest`, tests/test_config_presets.py): no `except Exception` CAUGHT, no float check CAUGHT, bool index CAUGHT, no NON_PRESET refusal CAUGHT. INFO-1, 2, 4, 5 deferred (harmless / by recommendation); INFO-3 (eight-name list test) left in place.
