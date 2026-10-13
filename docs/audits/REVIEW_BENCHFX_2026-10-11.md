# Review: benchfx (`dfd10845b`), 2026-10-11

Scope: origin/dev `dfd10845b` ("bench_test: HP-03/HP-07 restore overlays case-changed fields on a fresh GET
(WEBGENFX LOW-A); AX-C03 restore tests, detail in messages (AUXC03FX4 LOW-1/2, INFO-2/3)"). Files:
`tools/PcTools/src/kilnctrl/bench_test/cases_heat.py`, `cases_aux.py`, and their two test files. I reviewed it
in a worktree at origin/dev `25c5aa2b6`, which contains the commit unchanged, and checked it against
`zones_http_client.py` and `firmware/KilnFW/App/drivers/http/zones_http_{get,post_parse}.c`.

Verdict: the code is correct. I found no HIGH finding, and no path that reports a failed restore as a success.
The HP preset field lists match exactly what each case changes. The weak point is the tests. The one test meant
to pin the headline WEBGENFX LOW-A fix passes vacuously (MED-1), and the field lists are not pinned (LOW-1).

## Verification

- `uv run pytest tests/test_bench_test_cases_heat.py tests/test_bench_test_cases_aux.py`: 209 passed.
- `tools\negtest.ps1 -Preset pytest -PresetArg "<both files>" -Mutations <18> -Parallel 4`: the baseline passed,
  `real_tree_unchanged` was true and `copies_removed` was true. Result: 13 CAUGHT, 5 MISSED.

| Mutation | Verdict |
|---|---|
| H1 drop `hyst_c` from `_HP03_CHANGED_FIELDS` | **MISSED** |
| H2 drop `max_temp_c` from `_HP07_CHANGED_FIELDS` | CAUGHT |
| H3 drop `failsafe_state`/`min_on_s`/`min_off_s` from `_HP07_CHANGED_FIELDS` | **MISSED** |
| H4 restore overlays nothing (`build_post_body(fresh, {})`) | CAUGHT |
| H5 restore posts the whole pre-case snapshot under the fresh generation (the old LOW-A behaviour) | **MISSED** |
| H6 a failed GET still posts | CAUGHT |
| H7 read-back mismatch ignored | CAUGHT |
| H8 `_restore_preset` takes its values from the first zone instead of the target zone | **MISSED** |
| H9 HP-07 call site passes the HP-03 field list | CAUGHT |
| A1 AX-C03 no-generation path keeps the snapshot's stale generation | CAUGHT |
| A2 a failed read-back drops the POST error from the detail | CAUGHT |
| A3 the 2xx note drops `restore.detail` | CAUGHT |
| A4 the teardown RuntimeError drops the detail | CAUGHT |
| A5 a read-back GET exception returns True | CAUGHT |
| A6 a POST error counts as restored (`ok or err is not None`) | CAUGHT |
| A7 no-generation path posts the live (bad) config | CAUGHT |
| A8 `restore.detail` not reset at the start of each call | **MISSED** |
| A9 the no-generation note dropped | CAUGHT |

AUXC03FX4's open mutations M3, M6b, M4 and M9 (here A6, A5, A3 and A2) are all CAUGHT now, so its LOW-1, LOW-2
and INFO-3 are closed. M5 (here A8) is still MISSED, as before.

## Answers to the review questions

1. **Do the restore paths silently clobber or skip fields? Do the HP-03/HP-07 field lists match the cases?**
   - HP-03's preset writes `zone_type, failsafe_state, hyst_c, min_on_s, min_off_s` on the target zone.
     `_HP03_CHANGED_FIELDS` is exactly that set.
   - HP-07's preset writes `max_temp_c, zone_type, failsafe_state, min_on_s, min_off_s`.
     `_HP07_CHANGED_FIELDS` is exactly that set.
   - Every one of these fields is in `_PRESET_ZONE_OVERRIDE_FIELDS`, so `build_post_body` overlays it and never
     refuses it. `index` is in `_PRESET_ZONE_KNOWN_IGNORED_FIELDS`.
   - Each attempt does a fresh GET, overlays the preset and posts under that GET's `expected_generation`. A
     commit between the GET and the POST is refused with 409 and nothing is written, and the retry starts over.
     Edits made during the case to any other field now survive, which is the LOW-A fix.
   - A field missing from the snapshot is left out of the preset, so the restore keeps the fresh (case-set)
     value. Current firmware always prints all of these fields, so this cannot happen on the bench.
   - Not caught: the read-back (`_restore_readback_mismatch`) checks only `zone_type` and `max_temp_c`. A
     restore that failed to put back `hyst_c`, `failsafe_state` or `min_on_s`/`min_off_s` would therefore go
     unnoticed on hardware as well as in the tests. See LOW-1.
   - The HP restore, like the case's own POST, does not strip omit-preserved fields. See LOW-2.
2. **Could the AX-C03 no-generation fallback overwrite a concurrent edit on current firmware?**
   - No, not in practice. `GET /api/zones` always prints `"generation"` (`zones_http_get.c:766`), so on current
     firmware the fallback is unreachable.
   - It is also not a behaviour change. Before this commit, `dict(snapshot, generation=None)` already made
     `build_post_body` drop `expected_generation`. The commit only makes this explicit and records a note in
     `restore.detail`.
   - The handler's own commit re-check (`zones_config_changed_concurrently`) still guards the in-handler window.
   - The branch tests `is None` only. A non-int generation goes down the "pinned" branch, where `build_post_body`
     silently drops it with no note. See INFO-3.
3. **Tests that pass vacuously.** See MED-1 (H5), LOW-1 (H1, H3, H8) and INFO-1.
4. **Exception paths that report success.** I found none.
   - HP: a fresh-GET failure sets `last_error`, posts nothing and retries (H6 CAUGHT).
   - HP: a `build_post_body` raise (for example `ZonesHttpUnknownFieldError` on a new GET field) or a
     `post_zones` raise is caught into `last_error` and retried.
   - HP: "ok" is believed only after a matching read-back (H7 CAUGHT).
   - AX-C03: a read-back GET exception returns False (A5), and a POST error counts as restored only when the
     read-back matches (A6).
5. **Item 5 / AUXC03FX4 INFO-1 not applied: is that acceptable?** Yes.
   - An overlay-only AX-C03 restore would conflict with the exact-snapshot read-back
     (`_zone_collateral_diff(snapshot, after, zone, set())`). That read-back is what proves a guard regression
     left nothing behind: not the bad mask, and no collateral change from the narrow writer.
   - Narrowing the read-back to make room for an overlay would weaken the check that matters most in this case.
   - The window from snapshot to restore is a few seconds, and the run is gated idle (no firing, no autotune).
   - The remaining cost is that an unrelated web edit made in those seconds is reverted silently. That is
     acceptable for a bench case that already taints the run whenever its restore path runs at all.
   - If it ever matters, diff `live` against `snapshot` before the POST and name in `restore.detail` any field
     other than the target zone's `relay_mask`. Do not change the read-back.

## Findings

### MED-1: the LOW-A regression test passes vacuously, because the fake's GET returns aliased zone dicts (H5 MISSED)

`RestoreFromFreshGetTest.test_mid_case_edit_survives_restore` is the only test of the commit's headline
behaviour: a mid-case edit survives the restore. Reverting `_post_zones_restore` to the pre-commit design (post
the whole pre-case snapshot under the fresh generation) still passes it.

- **Cause:** `_GenerationFakeZhc.get_zones` returns `dict(self.snapshot)`, which is a shallow copy. The test's
  `snapshot` and the fake's live state therefore share the same zone dicts.
  - `self.fake.snapshot["zones"][2]["adaptive_kp"] = 9.5` also writes into the "pre-case" `snapshot`. I confirmed
    this with a probe: `snapshot["zones"][2]["adaptive_kp"]` reads 9.5 afterwards.
  - A restore built from the snapshot therefore also carries `adaptive_kp == 9.5`.
  - The `zone_type == 0` assertion holds either way, because `restore_preset` was computed before the mutation.
- **Same aliasing elsewhere:** the read-back passes only because the expected snapshot also shows the mutated
  `zone_type == 1`. The base `_FakeZonesHttpClient.post_zones` never applies a body to the fake's state, so the
  "restored" GET still reads 1.
- **Effect:** the code is correct today. Nothing pins it, though. A future refactor back to a whole-snapshot
  body would silently bring back LOW-A, the silent revert of adaptive gains or an autotune accept during HP-07.
  The read-back would not catch that on hardware either, because it compares only `zone_type`/`max_temp_c`.
- **Fix:**
  - Have `_GenerationFakeZhc.get_zones` return `copy.deepcopy(...)`.
  - Have its `post_zones` apply `body["merged"]` to the fake's state, so the read-back is real.
  - Then assert that `posted["adaptive_kp"] == 9.5` while the captured `snapshot` has no `adaptive_kp`.
  - Re-run H5. It should be CAUGHT.

### LOW-1: the HP field lists and the target-zone lookup are not pinned (H1, H3, H8 MISSED)

The canned zones in `_FakeZonesHttpClient` carry only `index`, `zone_type` and `max_temp_c`. As a result:

- Dropping `hyst_c` from HP-03's list passes (H1). So does dropping `failsafe_state`/`min_on_s`/`min_off_s` from
  HP-07's list (H3).
- `_restore_preset` taking its values from the wrong zone passes (H8). Every canned zone has identical values.
- On hardware the read-back does not check these fields either. A dropped field would leave the zone with
  `failsafe_state=false`, `min_on_s=min_off_s=0` and, for HP-03, `hyst_c=2.0` after every run, with no report.
  - `zone_type` itself is restored and read back, so the zone returns to PID control and these on/off-only
    fields are inert until someone types the zone on/off again.
  - When that happens, the operator inherits the bench values instead of their own.
- **Fix:**
  - Give the canned zones distinct values for all five fields, different per zone. Assert the full preset that
    `_restore_preset` returns for each case.
  - Preferably, also extend `_restore_readback_mismatch` to compare the fields named in the case's own
    `*_CHANGED_FIELDS` on the target zone, keeping the 0.05 float tolerance.

### LOW-2 (pre-existing, not introduced): HP-03/HP-07 POSTs re-commit every zone's omit-preserved fields at GET print precision

Neither the case's own POST nor the restore POST goes through `strip_omit_preserved`. Both bodies therefore
carry every zone's `k`/`tau`/`deadtime`/`coupling_c*`/`coupling_diag_k_dc`/fuzzy/easeoff/band/`coilpower`
values, as GET printed them (`%.1f` to `%.4f`).

- Each HP-03 or HP-07 run rounds those values on all zones, which is the AUXC03 MED-1 class.
- AX-C03, `control_set_zone_*` and `apply_zone_preset` all strip.
- The fresh-GET design makes the fix easy: `strip_omit_preserved(build_post_body(fresh, preset),
  _preset_named_omit_preserved_keys(preset))`, as `apply_zone_preset` does.
- `hyst_c` is itself omit-preserved (`z%u_hystc`, `zones_http_post_parse.c:258`), so the keep-set must contain
  `z{i}_hystc` for HP-03's case POST and its restore. `_preset_named_omit_preserved_keys` already produces it,
  because `hyst_c` is an override field.

### INFO-1: the AX-C03 no-generation test passes through the POST-error path, not the clean path

In `test_live_get_without_generation_posts_without_token`, the fake's `post_zones` applies the mask, then runs
`state["cap"]["generation"] += 1` on a dict whose `generation` was popped, which raises `KeyError`.

- `restore()` returns True through "POST error plus matching read-back". The probe's detail reads `...; restore
  POST error ('generation'), read-back matches`.
- The assertions still hold. The test does pin the missing token (A1, A7, A9 CAUGHT), but it does not model a
  clean older-firmware POST.
- **Fix:** have the fake bump the generation only when one is present, and assert `"restore POST error" not in
  restore.detail`.

### INFO-2: `restore.detail` reset is still unpinned (A8 MISSED, carried from AUXC03FX4 M5)

The closure is reused by the teardown retry. Without the reset, the retry's detail would carry the first
attempt's text. This affects diagnostics only.

### INFO-3: small AX-C03 detail inaccuracies

- When the live GET itself raises, nothing is posted, but the detail says "restore POST error (...)". "restore
  error" would be accurate.
- A generation that is present but not an int is posted unpinned with no note, because the branch tests only
  `is None`. This cannot happen on current firmware.

### INFO-4: a stale comment in `_case_hp07`

`cases_heat.py:1541` still says "The restore below POSTs the whole original snapshot (`build_post_body(snapshot,
{})`), which already puts `zone_type` (and these other fields) back". The restore now overlays
`_HP07_CHANGED_FIELDS` on a fresh GET. The comment should name that list, which is also where a future field
added to the preset must go.

## Fix status

All findings fixed in `9a085e44f`.

| Finding | Status | Fix |
|---|---|---|
| MED-1 | fixed | `_GenerationFakeZhc.get_zones` deep-copies; its `post_zones` applies the body to the fake state; the test asserts the mid-case `adaptive_kp` survives and the snapshot is not aliased (H5 CAUGHT) |
| LOW-1 | fixed | Canned zones carry distinct per-zone values for all five fields; both `*_CHANGED_FIELDS` and `_restore_preset` per zone are pinned; `_restore_readback_mismatch` now checks `_READBACK_FIELDS` (H1, H3, H8 CAUGHT) |
| LOW-2 | fixed | `_case_post_body` strips omit-preserved fields (keeping the preset-named ones, e.g. `z{i}_hystc`) for the HP-03/HP-07 case POSTs and the restore POST; tests added |
| INFO-1 | fixed | AX-C03 fake bumps the generation only when present; the no-generation test asserts no "restore POST error" |
| INFO-2 | fixed | Test pins the `restore.detail` reset per call (A8 CAUGHT) |
| INFO-3 | fixed | A failing live GET is labelled "restore live GET (no POST was sent)"; a non-int generation is noted and posted without a token |
| INFO-4 | fixed | HP-07 comment names `_HP07_CHANGED_FIELDS` |
