# Review ZONESPRECFX7 (2026-10-12)

Scope: zonesprecfx7's fix for REVIEW_ZONESPRECFX6 MED-1 and LOW-1/2/3, commits
beca47b9d and 90511c413 on origin/dev. Files: `tools/PcTools/src/kilnctrl/config_presets.py`,
`zones_http_client.py`, `mcp_server_ui_test.py`, `mcp_server_config_presets.py`,
`tests/fixtures/zones_get_capture.json`, `tests/test_zones_preset_review_fx7.py`. Reviewed at
90511c413.

Result: **0 HIGH, 0 MED, 3 LOW, 5 INFO.** Every key classification is right against firmware
source, and every cited bound and line number checks out. The gaps are in validation breadth
and in test reach. The affected pytest files pass (158 passed).

## Classification checked against firmware

- **relay_type -> override: correct.** The per-zone `relay_type` is the relay hardware kind
  (0-2: SSR/Contactor/Mercury, `ZONE_RELAY_TYPE_MAX 2`, `zones_config_json.h:208`). It is
  parsed omit-preserve at `zones_http_post_parse.c:203-215`. It is **not** the per-relay
  device type that `control_set_relay_type` writes: that one is `relay<N>_type`
  (`zones_http_post.c:631`), and GET emits it as the top-level `relay_types` array.
  `zones_http_client` keeps `relay_types` structural and never emits `relay<N>_type`, and the
  firmware keeps the stored value when that key is omitted (`zones_http_post.c:640`). So a
  preset cannot overwrite a device type. The one side effect is intended:
  `zones_config_push_all_relay_types()` (`zones_http_post.c:855`) calls
  `relay_cycles_set_type()`, which changes the life-budget type of the zone's relays. Their
  cycle counts are kept (`relay_cycles.c:591-609`).
- **progress_band_c, coil_power_w -> override: safe.** Both are optional and omit-preserve
  (`zones_http_post_parse.c:953-965`, `:316-328`). Their firmware ranges are `0` or
  `[0.5, 20]` and `0` or `[0.1, 100000]` (`zones_config_json.h:224-225`, `:309-310`). A
  preset that names them is re-posted from GET's `%.3f`/`%.2f` print
  (`zones_http_get.c:571`, `:644`). That rounding is below the noise floor. The print formats
  added to `_FLOAT_FIELD_PRINT_FORMAT` match.
- **fuzzy_model_valid -> ignored: correct.** It is computed per GET (`zones_http_get.c:442`)
  and has no POST key.
- **autotune_baseline_k_dc -> ignored: correct.** It has no POST key and no UART writer. The
  POST parser copies it from `current_z` (`zones_http_post_parse.c:505`). Its only writers are
  autotune accept, the adaptive-tune bootstrap and backup import. Restoring it from a preset
  would need a new write surface, and it would carry another board's anchor across. See
  INFO-2.
- **Bounds and line numbers:** `ZONE_PC_LINK_SILENCE_MS_MAX 600000.0f`
  (`zones_config_accessors.h:451`), `ZONE_NAME_MAX_LEN 15` (`:109`),
  `ZONE_EASE_OFF_WINDOW_MULT_MIN/MAX 0.1f/10.0f` (`zones_config_json.h:99-100`), and the
  `easeoffmult` logic at `zones_http_post_parse.c:869-880`. All match. The citation
  `zones_http_post.c:549` names the `if`; the parse call is on line 550. The name check
  matches the firmware: `http_form_find_field` URL-decodes into a 16-byte buffer, so the
  limit is 15 decoded UTF-8 bytes, and `http_form_value_has_ctl` refuses `< 0x20` and `0x7f`
  (`http_form.h:73-82`). The PC check is stricter in one way: it refuses an empty name, which
  the firmware accepts. The client's `_format_scalar` already refused an empty string, so
  nothing that used to work stops working.

## LOW-1: newly writable per-zone fields have no range check before the UART stage

These values pass `_validate` (probe at 90511c413): `relay_type` 3 or -1, `progress_band_c`
0.2 or 25, `coil_power_w` 0.05 or 2e5, `zone_type` 7, `hyst_c` -1, `min_on_s` -5, and
`approach_rate_cap_c_per_hr` -4. The firmware refuses each one with a 400, but only after
the PID/model values have already gone over UART. The board is then partially written. The
partial note reports this, so it is not silent. FX6 LOW-1 called this the "wider
pre-existing class". This change adds three more members by making `relay_type`,
`progress_band_c` and `coil_power_w` writable. Before the change those three already failed
after the UART stage, as an unknown field, so this is not a regression.

**Fix:** keep one per-zone range table in `config_presets`, pinned against the
`zones_config_json.h` `#define`s the way `BoundsTest` pins the current four. Cover at least
the new override keys and the `ON_OFF` fields.

## LOW-2: the drift test cannot see macro-built keys or a per-zone omission

`_emitted_keys()` matches only literal `\"name\":` strings. It finds 98 keys, so the `> 80`
sanity floor holds, but it cannot see:

- `coupling_c%u` (`zones_http_get.c:583`): the `%` is outside the character class.
- the `settings_source_groups` member names, which are emitted as `\"%s\"` from
  `SRC_GROUP_NAMES`.

Negtest M17 renamed the firmware key to `couplingx_c%u`, and every test still passed. In
the same way, `_all_keys` takes one union across the whole document. A key that is missing
from one zone, or present only in a nested object, still passes. Negtest M16 removed
`coil_power_w` from zone 2 only, and nothing failed.

**Fix:** expand `%u` keys over `range(MAX31856_CHANNEL_COUNT)`, or match them as regex
templates. Pin `SRC_GROUP_NAMES` from source. Compare each per-zone key set against every
zone in the fixture.

## LOW-3: two behaviours added in beca47b9d have no test

- M12: after read-back, `done[-1] = _uart_line(results)` refreshes the PID line. Deleting
  that line leaves `pid=ok` in the partial note even when read-back verification marked a
  zone FAILED. Nothing fails. A partial note that claims success after a failed verify is
  the dishonest-report class that FX6 LOW-2 was about.
- M5: dropping the `0x7F` part of the name check is not caught. The test uses only `\x01`.

**Fix:** add a test where read-back flips one zone to FAILED and a later stage raises, and
assert `pid=FAILED` in `preset_partial`. Add `"bad\x7fname"` to `test_zone_name_checked`.

## INFO

- **INFO-1: the fixture is derived from source, not captured.** It is valid JSON, and its
  types match the firmware print formats: ints for `%u`/`%.0f`, bools for `true`/`false`,
  and `relay_types` of length 4 = `KILN_IO_RELAY_COUNT`. It is a hybrid, though. It carries
  the `coupling_tau_c*`/`coupling_dead_time_c*` keys that `/api/zones_diag` emits, without
  that route's `model_fit_temp_c`/`model_fit_ambient_c`. Both are classified as ignored, so
  this does not matter today. The bench was not read: `control_get_zones` needs a serial
  connect, which this review skipped so as not to collide with the bench owner. A read-only
  `GET /api/zones` plus `/api/zones_diag` capture would make it a real capture.
- **INFO-2: pre-existing, not introduced.** A preset writes `k_dc` with UART
  `SET_ZONE_MODEL`, which does not move `autotune_baseline_k_dc`. On a board that already
  has an anchor, adaptive-tune's ratio test (`adaptive_tune_model.c:130-143`) stays anchored
  to the old board value. If the preset's K is more than 5x away, it refuses every
  refinement. After a factory default the anchor is 0 and bootstraps from the preset's
  value, so `factory_default_then_load_preset` is unaffected.
- **INFO-3:** `preset_zone_key_known` accepts `coupling_c<N>` for any `N`. `build_post_body`
  then silently drops an out-of-range cell such as `coupling_c7`, which the firmware only
  parses for `j < 3`. This is pre-existing regex breadth, and harmless.
- **INFO-4:** `_validate` still accepts unknown **top-level** keys. Only zone keys are now
  checked. This is outside FX6's scope, and noted for completeness.
- **INFO-5:** the overlap between the override and ignored sets is now pinned
  (`test_ignored_and_override_sets_are_disjoint`). The LOW-2 ordering fix is covered:
  negtest M11 was CAUGHT by 12 tests.

## Negtest table

The run used `tools\negtest.ps1 -Preset pytest` (no `-RequireAssertion`) with
`-PresetArg "tests/test_zones_preset_review_fx7.py tests/test_config_presets.py
tests/test_zones_http_client.py tests/test_zones_http_float_tolerance_pin.py"`, base
90511c413. The baseline passed. The run reported `real_tree_unchanged: true`. Every CAUGHT
below is a pytest `FAILED` assertion line, never an import or collection `ERROR`.

| # | Mutation | Verdict | Caught by |
|---|----------|---------|-----------|
| M1 | `relay_type` removed from override set | CAUGHT | classification, fixture load/post, live-GET round trip |
| M2 | `fuzzy_model_valid`/`autotune_baseline_k_dc` removed from ignored set | CAUGHT | classification, fixture tests |
| M3 | unknown-zone-key refusal disabled | CAUGHT | `test_unknown_zone_key_refused_by_validate` |
| M4 | name limit off by one (16) | CAUGHT | `test_zone_name_checked` |
| M5 | name `0x7F` check dropped | **MISSED** | LOW-3 |
| M6 | name length in chars, not bytes | CAUGHT | `test_zone_name_checked` |
| M7 | `ease_off` 0 no longer allowed | CAUGHT | bounds and fixture tests |
| M8 | `ease_off` min made strict | CAUGHT | bounds tests |
| M9 | `pc_link` max made exclusive | CAUGHT | `test_top_level_ranges` |
| M10 | per-zone `ease_off` check removed | CAUGHT | `test_per_zone_ease_off_range` |
| M11 | PID line recorded after read-back (LOW-2 revert) | CAUGHT | 12 tests |
| M12 | post-read-back PID line refresh removed | **MISSED** | LOW-3 |
| M13 | `progress_band_c` print format removed | CAUGHT | float-tolerance pin |
| M14 | `coupling_c` regex dropped from known-key predicate | CAUGHT | fixture classification |
| M15 | fixture loses top-level `generation` | CAUGHT | drift test |
| M16 | fixture loses `coil_power_w` in zone 2 only | **MISSED** | LOW-2 |
| M17 | firmware renames `coupling_c%u` | **MISSED** | LOW-2 |
| M18 | firmware renames literal `coil_power_w` | CAUGHT | drift test, format pin |

The three LOWs are follow-ups. None blocks the FX6 fixes, which do what they claim.
