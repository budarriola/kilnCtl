# Review: AX-C03 review-fix round 4 (auxc03fx4), 2026-10-11

Scope: origin/dev `4efaf2cda` ("AX-C03 review fx3: restore posts the live generation, always reads back;
teardown-registration test; cfg-unmounted wording"). It fixes MED-1, LOW-1, LOW-2 and INFO-1 of
`docs/audits/REVIEW_AUXC03FX3_2026-10-10.md` in `tools/PcTools/src/kilnctrl/bench_test/cases_aux.py`
(`_default_zone_mask_fns().restore`, `_case_ax_c03`) and `tools/PcTools/tests/test_bench_test_cases_aux.py`
(new class `AuxC03RestoreGenerationTest`). I reviewed it at origin/dev `4efaf2cda` and checked it against
`firmware/KilnFW/App/drivers/http/zones_http_post.c` and `tools/PcTools/src/kilnctrl/zones_http_client.py`.

Verdict: MED-1, LOW-1, LOW-2 and INFO-1 are fixed. There is no HIGH or MED finding. Two LOW findings remain:
a test gap in the dangerous direction, and a reporting gap in the 2xx branch.

## Verification

- `pytest tests/test_bench_test_cases_aux.py tests/test_mcp_server_control_zone_relay_mask.py`, run in a
  worktree at `4efaf2cda`: 94 passed.
- `tools\negtest.ps1 -Preset pytest -PresetArg tests/test_bench_test_cases_aux.py -Mutations <12> -Parallel 4`:
  the baseline passed, and `real_tree_unchanged` was true.

| Mutation | Verdict |
|---|---|
| M1 restore posts the snapshot's generation instead of the live one | CAUGHT |
| M2 a POST error returns False before the read-back (the fx3 behaviour) | CAUGHT |
| M3 `return ok` changed to `return ok or err is not None` (any POST error counts as restored) | **MISSED** |
| M4 save-failure reason drops `restore.detail` | MISSED |
| M5 `restore.detail` not reset at the start of each call | MISSED |
| M6 read-back result ignored (`ok = True`) | CAUGHT |
| M6b read-back GET exception returns True | **MISSED** |
| M7 no live GET (`live = {}`, so the body has no `expected_generation`) | CAUGHT |
| M8 save-failure branch drops the teardown registration (fx3 N2) | CAUGHT |
| M9 detail always says "matches" | MISSED |
| M10 restore posts the unstripped body | CAUGHT |
| M11 body built from `live` instead of `snapshot` (would re-post the bad mask) | CAUGHT |

## Answers to the review questions

1. **Is anything between the fresh GET and the POST still racy? Does the teardown retry take the same path?**
   - A commit that lands between `live = zhc.get_zones(host)` and the POST makes the firmware answer 409
     `zones_config_stale` (`zones_http_post.c:369-378`), and nothing is written.
   - A commit that lands after the token check but before the commit gets 409 `zones_config_changed_concurrently`
     (`:797-811`). The token becomes `gen_at_snapshot` at `:378`, so this re-check covers it.
   - Either way the read-back differs, `restore()` returns False, and the case registers `c03_teardown_hook`.
     The hook calls the same closure, so the retry does a fresh GET and posts under a fresh generation. That is
     the right shape.
   - What remains is by design: taking the live generation deliberately gives up lost-update protection for
     the window from the original snapshot to the restore. See INFO-1.
2. **Is `generation` the key `build_post_body` reads? What if `live` has no generation?**
   - Yes. `build_post_body` reads `current.get("generation")` and emits `expected_generation` only for a
     non-bool int >= 0 (`zones_http_client.py:1222-1224`).
   - `GET /api/zones` always prints `"generation"` (`zones_http_get.c:766`).
   - If `live` lacks it, or the value is not an int, the override sets `generation=None`. The body then
     silently carries no token, and the POST becomes an unconditional write. Older firmware ignores the token
     anyway, and the handler's own commit re-check still guards the in-handler window. Nothing fails loud,
     though. See INFO-2.
   - M7 shows the test pins the token being present when the GET carries one.
3. **Can a "matching" read-back hide a real failure?**
   - The read-back is strong. `_zone_collateral_diff(snapshot, after, zone, set())` compares every field of
     every zone and every top-level field, and skips only telemetry, which includes `generation`.
   - A read-back GET exception returns False, which is the safe direction.
   - "Matches" after a POST error therefore really means the live RAM config equals the snapshot.
   - It does not say the restore persisted. In the 2xx branch that hides a real failure, and the reason text
     never shows the error. See LOW-2.
   - The tests pin only the true direction of the error-plus-read-back logic (M3 and M6b MISSED). See LOW-1.
4. **Does the fake board match the firmware's generation semantics?** Yes, for what these tests exercise.
   - **Stale-token refusal:** a token that differs from the live generation is refused and nothing is
     written, the same as `:369-378`.
   - **Bump on commit:** an accepted POST bumps the generation once.
   - **Save failure after commit:** the fake bumps, applies, then raises "500 could not be saved to flash".
     This matches the firmware, which bumps at `:844` before `nvs_save()` at `:855` and answers
     `cfg_fs_http_persist_failed_for` at `:895`.
   - **Not modelled:** the 409 `zones_config_changed_concurrently` and `zones_config_undecided` paths (both
     before the commit, with no bump), body validation, and fields other than `z0_relay_mask`.
   - The fake raises `ZonesHttpError` without the status code and detail that the real `post_zones` attaches,
     and its 409 message text differs from the real one. Neither matters, because `restore()` reads neither.
   - `test_stale_generation_is_refused_by_fake_board` pins the fake's refusal, so the fake cannot quietly
     accept everything.
5. **Are the tests vacuous anywhere?** The new tests are real.
   - `test_restore_after_committed_write_uses_current_generation` catches M1, M7 and M11. It would have caught
     fx3 MED-1.
   - `test_post_error_with_matching_readback_counts_as_restored` catches M2.
   - `test_save_failure_with_failed_restore_registers_teardown_once` catches M8, which closes fx3 N2.
   - The gaps are the inverse direction (M3, M6b) and the detail text (M4, M5, M9).

## Findings

### LOW-1: no test covers "POST error and a differing or unreadable read-back is NOT restored" (M3, M6b MISSED)

Every new restore test that has a POST error expects True.

- Changing the result to `ok or err is not None` passes the suite. So does making a read-back GET exception
  return True.
- These two mutations break the safety-relevant direction. A stale-token 409 (nothing written, the bad mask
  still live) would report "original mask restore ok". The case would then register no teardown retry, and the
  runner would not raise its loud "restore NOT confirmed".
- The code is correct today. The gap is that nothing pins it.
- **Fix:** add two tests on the same fake board:
  1. Commit a bad mask, bump the generation, then make the restore POST stale. Either have the fake refuse
     every POST, or bump the generation between the live GET and the POST. Assert that `restore()` is False,
     that the mask still reads `orig | 8`, and that `restore.detail` says "differs".
  2. Make the read-back GET raise. Assert that `restore()` is False.

  Both tests should catch M3, M6b and M9.

### LOW-2: in the 2xx and possibly-applied branches, an error plus a matching read-back reports "restore ok", while NVS may still hold the bad mask

Only the save-failure branch appends `restore.detail` to the reason (`cases_aux.py:567`). The 2xx and
possibly-applied note (`:601`) does not.

- **Scenario:** a guard regression accepts `orig | 8` with a 2xx, so it is committed and persisted. The
  restore POST then commits the original to RAM, but `nvs_save()` fails with 500 `could not be saved to flash`.
  - The read-back matches, so `restore()` returns True.
  - The case reports "original mask restore ok -- run tainted", with no mention of the error.
  - NVS still holds `orig | 8`. After the next reboot, zone 0 claims relay 4 again, alongside the aux binding.
- The run is still FAIL and tainted, so this is the loud direction overall. Only the restore status is
  misleading.
- In the save-failure branch the same outcome is a real restore: the bad write never persisted, so NVS still
  holds the original.
- **Fix:** append `getattr(rfn, 'detail', '')` to the `:601` note as well.
  - Preferably, when a POST error accompanies a matching read-back and the original write was a 2xx, report
    "restored in RAM only, persisted state unknown" rather than "ok".
  - Alternatively, return a tri-state from `restore()`.
  - A test would also catch M4.

### INFO-1: the restore overwrites any change made between the original snapshot and the restore

Posting the snapshot under the live generation is intended: it reverts the case's own write and any
collateral. It also silently reverts an unrelated edit made in that window, for example from a web page.

- The bench run is gated idle (no firing, no autotune) and AX-C03 is short, so this is acceptable.
- If it matters later, diff `live` against `snapshot` before posting. Note in `restore.detail` any field
  other than zone `zone`'s `relay_mask` that differs.

### INFO-2: if the live GET lacks `generation`, the restore posts with no token

`dict(snapshot, generation=live.get("generation"))` replaces even a valid snapshot token with None, and
`build_post_body` then drops `expected_generation`.

- Current firmware always prints `generation`, so the only realistic cause is older firmware, which ignores
  the token anyway.
- If loud failure is preferred, record "no live generation; unpinned restore" in `restore.detail`.

### INFO-3: a failed read-back GET discards the POST error from the detail

When the read-back GET raises, `restore()` returns False before it sets `restore.detail`. A POST error from
the same call is therefore lost.

- `c03_teardown_hook` never surfaces `detail` either. Its RuntimeError says only "restore NOT confirmed after
  teardown retry".
- This is diagnostic only. Folding `restore.detail` into that message would help an operator.
