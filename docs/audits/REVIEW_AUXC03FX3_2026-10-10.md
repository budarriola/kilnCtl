# Review: AX-C03 review-fix round 3 (auxc03fx3), 2026-10-10

Scope: origin/dev `1d04391de` ("AX-C03 review fx2 MED-1, LOW-1..3: ...") and doc commit `b0ffbd8af`. The fix
addresses MED-1 and LOW-1..3 of `docs/audits/REVIEW_AUXC03FX2_2026-10-10.md` in
`tools/PcTools/src/kilnctrl/bench_test/cases_aux.py` and `tools/PcTools/tests/test_bench_test_cases_aux.py`.
Reviewed at origin/dev `66aab5fe9`. The response texts were checked against
`firmware/KilnFW/App/drivers/http/zones_http_post.c` and `common/cfg_fs_refusal_http.h`.

## Verification

- `uv run pytest tests/test_bench_test_cases_aux.py tests/test_mcp_server_control_zone_relay_mask.py`:
  90 passed.
- `tools\negtest.ps1 -Preset pytest -PresetArg tests/test_bench_test_cases_aux.py`, 12 mutations, baseline
  PASS:

| Mutation | Verdict |
|---|---|
| N1 drop `ctx["_tainted"] = True` in the save-failure branch | CAUGHT |
| N2 drop the `c03_teardown_hook` registration in the save-failure branch | MISSED |
| N3 restore posts `build_post_body` without `strip_omit_preserved` | CAUGHT |
| N4 drop `"settings storage (cfg) not mounted"` from the text list | CAUGHT |
| N5 drop `"could not be saved to flash"` from the text list | CAUGHT |
| N6 `any(...)` changed to `all(...)` in the save-failure check | CAUGHT |
| N7 save-failure check skipped for status 409 | CAUGHT |
| N8 save-failure verdict changed to INCONCLUSIVE | CAUGHT |
| N9 a second snapshot GET inside the `try` | CAUGHT |
| N10 restore ignores the `_zone_collateral_diff` result | CAUGHT |
| N11 restore skips the POST | CAUGHT |
| N12 save-failure branch never attempts the restore | CAUGHT |

- A scratch simulation (not committed) used the real `build_post_body` on
  `tests/fixtures/zones_get_capture.json` plus `"generation": 5`, against a fake board that bumps the
  generation on commit and refuses a stale `expected_generation` the same way `zones_http_post.c:362-379`
  does. After a committed bad write, `restore()` returned False and the board kept `relay_mask` 9
  (the original was 1). See MED-1.

## Answers to the review questions

1. **Does the stripped restore re-post every required field?** Yes.
   - `strip_omit_preserved` drops only the keys matched by `ZONE_OMIT_PRESERVED_KEY_RE`: `z<N>_` plus `k`,
     `tau`, `deadtime`, `coupling_diag_k_dc`, `coupling_c*`, `fuzzy_strength`, `easeoffmult`,
     `approachratecap`, `errorband`, `rateband`, `progressband`, `hystc` and `coilpower`.
   - Every required field stays in the body: relay_mask, control_mode, gains, limits, timing profiles,
     relay names and the top-level counts.
2. **Could the restore now leave a field changed that the case altered?** Not through the stripped keys.
   The AX-C03 write goes through `control_set_zone_relay_mask`, which strips the same key set
   (`_strip_omit_preserved_zone_fields(..., None)`). So the write cannot have changed any field the restore
   now leaves out. The restore can still fail to undo `relay_mask` itself. See MED-1.
3. **Is the restore verified by read-back?** Yes. `_zone_collateral_diff(snapshot, after, zone, set())`
   compares every field of every zone and every top-level field, skipping only the telemetry keys
   (`generation` is one of them). Two gaps remain:
   - A POST exception skips the read-back. See LOW-1.
   - The read-back compares GET prints, so it cannot see sub-print drift. This no longer matters, because
     the restore does not post the fields where that could happen.
4. **Are the save-failure texts exact, and do they match the firmware?** They match the firmware, but by
   substring (`any(t in text ...)`), not by equality.
   - `store_unreadable_at_boot` is in `cfg_fs_refusal_http.h:57`.
   - `could not be saved to flash` is at `:67`.
   - `settings storage (cfg) not mounted` is a prefix of both `CFG_FS_NOT_MOUNTED_TEXT` and
     `CFG_FS_RECOVERY_SKIPPED_TEXT` (`persist/cfg_fs.h:112,117`).
   - In `zones_http_post.c` these texts are sent only at `:672`, which is after the aux check, and at
     `:895`, which is after the commit. No response sent before the aux check contains any of them, so the
     substring match cannot turn a pre-aux refusal into a FAIL. See INFO-1 for the `:672` 503.
5. **Is the taint path complete?** Yes. The save-failure branch sets `_tainted` before the restore, attempts
   the restore once, and registers `c03_teardown_hook` with `_c03_restore_fn` on failure. It returns FAIL in
   every case. The registration is untested (N2 MISSED, LOW-2).
6. **Do hidden mocks make the tests vacuous?** Partly.
   - `test_restore_ok_and_body_has_no_omit_preserved_keys` and `test_restore_recheck_with_differing_get_is_false`
     mock `build_post_body` with a fixed string. That string never carries `expected_generation`, which hides
     MED-1.
   - `test_post_commit_save_failures_fail_taint_restore` uses tool lines in a made-up format, for example
     `error: POST /api/zones failed (HTTP 500): ...`. The real line is
     `error: POST /api/zones failed (host=h): POST /api/zones refused: HTTP 500: {...}`. The substring match
     makes this harmless today.
   - The other new tests are real. N1 and N3-N12 are caught.

## Findings

### MED-1: the restore can never undo a committed write: it re-posts the snapshot's stale `expected_generation`

`restore()` posts `strip_omit_preserved(build_post_body(snapshot, {}))`.

- `build_post_body` adds `expected_generation=<snapshot["generation"]>` (`zones_http_client.py:1221-1224`).
  `strip_omit_preserved` keeps it, because it is not a `z<N>_` key.
- Every commit bumps `s_config_generation` (`zones_http_post.c:844`). This includes the post-commit
  save-failure paths, because the bump happens before `nvs_save`.
- The restore's POST is therefore refused with 409 `zones_config_stale` (`:370-378`) in exactly the
  situations the restore exists for. `post_zones` raises, and `restore()` returns False.
- The teardown retry posts the same body and fails the same way, then raises
  "AX-C03 zone config restore NOT confirmed".

The bug predates this round: the whole-snapshot restore has always carried the token. The MED-1 fix kept it,
and the fx2 review missed it.

- **Scenario:** a guard regression lets `z0_relay_mask = orig | 8` through (2xx), or it commits and then
  `nvs_save` fails (500 `could not be saved to flash`).
  - The case taints the run and calls `restore()`. The restore POST is answered 409 `zones_config_stale`,
    so nothing is written. The teardown retry fails too.
  - The run ends loud, but zone 0 still claims relay 4. On a 2xx the bad mask is also persisted.
  - `aux_teardown_hook` puts the aux binding back to its original state, so relay 4 can end up driven by
    zone 0's heater output until an operator fixes the mask by hand.
  - The simulation reproduced this end to end.
- **Fix:** take the generation from a fresh GET just before the restore POST and post the snapshot's
  fields under it. For example: `live = zhc.get_zones(host)`, then
  `body = build_post_body(dict(snapshot, generation=live.get("generation")), {})`, then strip and post. A
  concurrent writer in the short window still gets a 409, and the teardown retry covers that.
- **Test:** use the real `build_post_body` on `tests/fixtures/zones_get_capture.json` with a `generation`
  field. The fake `post_zones` should bump the generation on commit and refuse a stale
  `expected_generation`. Commit a bad mask, then assert that `restore()` returns True and the mask reads back
  as the original. The current tests mock `build_post_body` and cannot see this.

### LOW-1: a POST error in `restore()` skips the read-back, so a restore that did apply reports "NOT confirmed"

`restore()` wraps the POST and the re-GET in one `try`. Any `ZonesHttpError` from `post_zones` returns False
without reading back.

- **Scenario:** this is the case the new LOW-1 branch handles: the bad write committed, then
  `nvs_save` failed.
  - Once MED-1 is fixed, the restore POST commits the original mask to RAM, then hits the same save failure:
    500 `could not be saved to flash`, or 409 `store_unreadable_at_boot`.
  - `restore()` returns False although the live mask is back, and the teardown retry repeats the 500. The
    run ends "restore NOT confirmed", which wrongly says the mask may still claim relay 4.
  - This is the safe direction, but it is misleading.
- **Fix:** catch the POST error and always do the read-back. Judge restored by
  `not _zone_collateral_diff(snapshot, after, zone, set())`. Report "restored in RAM, not persisted" when the
  POST error was a save failure.

### LOW-2: no test pins the teardown registration in the save-failure branch (N2 MISSED)

`test_post_commit_save_failures_fail_taint_restore` uses a restore that always succeeds. Deleting the
`_c03_restore_fn`/`teardown_hooks` registration in the new branch therefore passes.

- **Scenario:** a refactor drops the registration. A failed save-path restore then gets no teardown retry,
  and the runner never raises the loud "restore NOT confirmed after teardown retry".
- **Fix:** add a variant with a restore that returns False. Assert that `ctx["_c03_restore_fn"]` is set and
  that `c03_teardown_hook` is in `ctx["teardown_hooks"]` exactly once.

### INFO-1: the cfg-unmounted 503 is sent before the commit, but the FAIL text says "then the save failed"

`zones_http_post.c:672` (`cfg_fs_http_refuse_if_unmounted`) sends this 503 after the aux check but before the
commit, so nothing is live.

- FAIL is still the correct verdict, because the guard let the mask through.
- The attempted restore is harmless.
- Only the reason text is inaccurate. It could say "past the aux check, then refused/failed to save".

### INFO-2: the substring match is acceptable

The match is `any(t in text ...)` over the whole tool line, not equality on the error field. Every listed text
is unique to the `:672`/`:895` paths in the zones handler, so no false FAIL is reachable today. If an exact
match is preferred, parse the JSON body after `_fw_detail` and compare `error`, with a prefix match only for
the cfg-unmounted texts. That is optional.
