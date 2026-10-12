# Review: profilesfx5 and zprecfx3 (2026-10-12)

Adversarial review of two fix batches that landed on `origin/dev` with
`land.ps1 -AllowFail`. Reviewed at dev tip `25857a570` (worktree base
`648e97e6d`).

## Commits reviewed

| Batch | Commit | Content |
|---|---|---|
| profilesfx5 | `603f5283c` | REVIEW_PROFILESFX3 MED-1 (decide error body read once, `saved`/`id` parsed from the same bytes), LOW-1 (`PROFILES_SAVE_ERR_MSG_LEN` 128 shared by the LCD save dialog), INFO-1 (JS test for the "Changed elsewhere" banner) |
| profilesfx5 | `3729149ae`, `c8be82d63` | Doc only: REVIEW_PROFILESFX3 fix status and SHA |
| zprecfx3 | `b0a181ef6` | Setup wizard lossless unedited save (`kgR6`/`kgRawAttrs`/`kgPost` copies, step 3 cal and step 6 limit raw posts, hoisted `step6ReadLiveZones`/`step6RevertInputsOn`), 9-digit `sanity_rate` test value |
| zprecfx3 | `2e897c0e8`, `67beaf2fa`, `7a3c54e14` | Wizard tests: displayed rounded values, step 3 read-back, doc |
| zprecfx3 | `6beda0601` | Doc only: REVIEW_ZPRECFX2 fix status. The code is in `b0a181ef6..7a3c54e14` above |

## Checks bypassed by -AllowFail, rerun

Run directly in a clean worktree at the dev tip:

| Check | Result |
|---|---|
| `check_build_gate_reentrant.ps1` | PASS |
| `check_source_path_drift.ps1` | FAIL, unrelated (INFO-3) |
| `check_setup_wizard_zones_post_helper.ps1` | PASS |
| `check_no_duplicate_commissioning_impl.ps1` | PASS |
| `check_lcd_admin_gates.ps1` | PASS |
| `setup_wizard_step_count_mirror_drift_check.py` | PASS |
| JS: `test_setup_wizard`, `test_zones_kgr6`, `test_live_profile_action_errors`, `test_zones_type_toggle`, `test_strict_form_pages`, `test_unsaved_guard_pages`, `test_web_review_fixes`, `test_web_ui_js_audit_fixes`, `test_web_ui_write_gates_behaviour`, `test_host_refusal_pages`, `test_advanced_commissioning` | all PASS |
| pytest `tests/test_profile_live_http_client.py` | PASS |
| host tests `-Only "profiles_http|zones_http"` | PASS |

None of the bypassed failures comes from these commits. The check suite did
not catch MED-1 below, because no test covers a fractional zone max against a
Pico ceiling that mirrors it.

## Findings

### MED-1: wizard step 6 refuses an unedited save when a zone max temp is fractional

`b0a181ef6`, `firmware/KilnFW/App/drivers/http/setup_wizard_page.html:918`
(`validateStep6`), fed by `step6ReadLiveZones` (`:2601`) and called at `:2673`.

Since `b0a181ef6`, an unedited step 6 field returns the raw `GET /api/zones`
value through `kgPost`. That value is printed at `%.9g`. `validateStep6` then
compares it with `z.max_temp_c > absMaxTempC`, where `absMaxTempC` comes from
`getAbsMaxTempC` (`:893`). That ceiling is `GET /api/safety/commissioning`'s
`value`, printed at `%.6g` (`safety_cfg_http.c:416`). The Pico ceiling follows
the highest zone max exactly (`safety_ceiling_policy_target_c`, no headroom).
So for a float32 zone max such as 1260.3, the comparison is
`1260.30005 > 1260.3`, which is true. The wizard then refuses with a message
that contradicts itself:

```
Zone 1 max temp (1260.3 °C) exceeds the safety processor's abs_max_temp_c ceiling (1260.3 °C). ... Refused, not clamped.
```

Typing 1260.3 back into the field does not help. The typed text equals
`data-disp`, so `kgPost` still returns the raw value. The only way past the
check is to change the value.

- Reproduced in node by extracting the page functions: both the unedited case
  and the retyped `1260.3` case are refused. The parent commit
  (`b0a181ef6~1`, which read `.value`) accepts the same input.
- Scale: 1600 of the 3600 fractional 0.1 °C steps between 1000 and 1400 °C
  are refused (any float32 whose `%.9g` form is above its `%.6g` form).
- The firmware accepts the same save: the values are equal floats, and
  `SAFETY_CEILING_EPSILON_C` is 0.01. Only the web pre-check is too strict.
  This undoes the improvement that REVIEW_ZPRECFX2 credited to lossless step 6.

Fix: compare at the precision of the coarser side, for example
`kgR6(z.max_temp_c) > kgR6(absMaxTempC)`, or allow
`SAFETY_CEILING_EPSILON_C` (0.01) slack in `validateStep6`. Add a
`test_setup_wizard.js` case with a float32 fractional max (1260.30005) against
a `%.6g` ceiling (1260.3) that must validate. Negative-test it by reverting the
comparison.

### LOW-1: step 6 revert of max_ramp is not covered by any test

`firmware/KilnFW/App/test/test_setup_wizard.js:1123`. The revert test edits
only `s6maxtemp`, so mutation J6, which drops `kgR6` from the ramp revert
(`setup_wizard_page.html:2622`), is MISSED. The effect is cosmetic: a reverted
ramp field would show 9 digits (`123.400002`) instead of `123.4`, and it still
posts the same number. Fix: edit `s6ramp` before the revert and assert that it
reads `'123.4'` afterwards.

### LOW-2: `_parse_saved` bool-id rejection is untested

`tools/PcTools/src/kilnctrl/profile_live_http_client.py:130`. Mutation P4,
which drops `and not isinstance(i, bool)`, is MISSED. A body of
`{"saved":true,"id":true}` would report saved id 1. Firmware never sends a
bool id, so there is no live impact today. Fix: add a one-line test case to
`test_profile_live_http_client.py`, or remove the clause if it is not wanted.

### INFO-1: longest LCD save refusal has little headroom in PROFILES_SAVE_ERR_MSG_LEN

`firmware/KilnFW/App/drivers/persist/profiles_store.h:59` (128). The longest
format is `profiles_validate.c:106`, "segment %u: relay %u is bound to an aux
output -- only relays not owned by a zone or an aux output can be a segment
target". It expands to 122 characters with realistic values, or 124 with
3-digit `%u` values, so there are only 3 to 5 bytes spare. `snprintf`
truncates safely, so this cannot overflow. The test
(`test_profiles_http.c:2726`) only measures the 93-character stale-rev
message. If a future message is longer, the refusal would be cut short on the
LCD with nothing to flag it. Consider asserting the worst-case expansion of
every `profiles_validate.c` format.

### INFO-2: the LCD buffer size is guarded only by source text

Mutation H1 (`ui_page_profile_builder_review.c:93` back to `char err_msg[64]`)
is CAUGHT only by the source-text check at `test_profiles_http.c:3068`. That is
acceptable for a host test that cannot run LVGL. Recorded so nobody reads it as
behavioural coverage.

### INFO-3: check_source_path_drift failure is an unrelated false positive

`tools/check_push_verify.ps1:97` contains the regex string
`'lib_push_verify_walk\.ps1'`, which the drift check reads as a path.
`tools/lib_push_verify_walk.ps1` exists. That file was last changed by toolfx
commits (`b369df678`, `aa7cc80e1`), not by the commits reviewed here.

### INFO-4: step6RevertInputsOn does not revert min_temp

`setup_wizard_page.html:2615-2624` restores max temp and ramp only. This is
unchanged from before `b0a181ef6`. It is noted only because the revert now
has a lossless counterpart for `s6mintemp`.

### Verified correct

- `_error_info` reads the `HTTPError` body exactly once. `http_auth.urlopen`
  re-raises non-401 errors without consuming the body. `saved` must be the
  literal `True`. The loopback tests use a real `http.server`, and the
  `_OneShotBody` fake cannot seek.
- `lv_msgbox_add_text` copies its text, so a stack `err_msg` is safe.
- The wizard `kgR6`/`kgRawAttrs`/`kgPost` are byte-identical to
  `zones_page.html:633-634`, and a drift-pin test enforces it.
- Step 3 cal: `.s3cal` carries `data-raw`, and `step3ReadCurrentZones`
  (`:2150`) uses `kgPost`.
- `sanity_rate` test value `0.000100027195f` against `%.9g`
  (`zones_http_get.c:448`). Both `%.8g` and `%.6g` mutants fail the test.
- The INFO-1 banner test is valid: the sandbox `dirty` binding matches the
  page's `var dirty`.

## Mutation table

Every run used `tools/negtest.ps1` in a throwaway worktree, with an unmutated
baseline that PASSed. For the host runs, each CAUGHT was confirmed in the
mutant log as a `FAIL file:line` test assertion with a successful compile, not
a build failure.

| ID | Mutation | Runner | Verdict | Failing assertion |
|---|---|---|---|---|
| J1 | step 3 read-back `.value` instead of `kgPost` | node | CAUGHT | step3: unedited read-back keeps the raw cal |
| J2 | step 6 max_temp `.value` | node | CAUGHT | step6: unedited save posts raw max_temp |
| J3 | step 6 ramp `.value` | node | CAUGHT | step6: unedited save posts raw max_ramp |
| J4 | step 6 min_temp `.value` | node | CAUGHT | step6: unedited save posts raw min_temp |
| J5 | revert max_temp without `kgR6` | node | CAUGHT | step6RevertInputs restores the displayed value |
| J6 | revert ramp without `kgR6` | node | **MISSED** | LOW-1 |
| J7 | `s6ramp` loses `kgRawAttrs` | node | CAUGHT | step6: unedited save posts raw max_ramp |
| J8 | `s3cal` loses `kgRawAttrs` | node | CAUGHT | step3: cal input carries data-raw |
| J9 | wizard `kgPost` always returns `.value` | node | CAUGHT | wizard kgPost: unedited posts raw (+9) |
| J10 | live page `if (dirty)` becomes `if (false && dirty)` | node | CAUGHT | failed decide + moved generation + dirty form shows the Changed elsewhere banner |
| P1 | `_error_info` drops `_parse_saved` | pytest | CAUGHT | |
| P2 | `saved` truthy instead of `is True` | pytest | CAUGHT | |
| P3 | `exc.fp.seek(0)` before read (old bug) | pytest | CAUGHT | |
| P4 | bool id accepted | pytest | **MISSED** | LOW-2 |
| H1 | LCD `err_msg[64]` | kilnfw-host | CAUGHT | `test_profiles_http.c:3069` (source text) |
| H2 | `PROFILES_SAVE_ERR_MSG_LEN 64` | kilnfw-host | CAUGHT | `test_profiles_http.c:2727` |
| H3 | `sanity_rate` GET `%.8g` | kilnfw-host | CAUGHT | `test_zones_http.c:7699` |
| H4 | `sanity_rate` GET `%.6g` | kilnfw-host | CAUGHT | `test_zones_http.c:7699` |

Method:
- JS: `-Command` running the three node tests, with `-RequireAssertion`.
- Python: `-Preset pytest -PresetArg tests\test_profile_live_http_client.py`.
- Host: `build_host_tests.ps1 -OutDir {OUT} -Only "profiles_http|zones_http"`
  with `-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`
  and no `-RequireAssertion`.

## Fix status

Fixed in `9099f81bc` (rebased SHA recorded by the landing commit): MED-1 (steps 6 and 7 compare with 0.01 slack, matching `SAFETY_CEILING_EPSILON_C`; step 7 had the same class, REVIEW_ZPRECFX2 INFO-1; tests for 1260.30005 vs 1260.3 accepted and 1260.5 / 1260.1 refused), LOW-1 (ramp revert asserted), LOW-2 (bool id test), INFO-1 (`PROFILES_SAVE_ERR_MSG_LEN` 128 -> 192, host test requires >= 16 bytes margin on the longest io-segment refusals). Negtests all CAUGHT: step6/step7 slack removal, slack too loose, ramp revert without kgR6, bool id accepted, LEN back to 128, padded aux message (host ones confirmed as `test_profiles_http.c:4497` assertions on a successful compile).
