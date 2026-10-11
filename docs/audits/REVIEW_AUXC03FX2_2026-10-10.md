# Review: AX-C03 review-fix round 2 (auxc03fx2), 2026-10-10

Scope: origin/dev `d7cf4dd0b` ("AX-C03 review LOW-1..6: ...") and doc commit `0f2c057f7`. The fix addresses
LOW-1..6 of `docs/audits/REVIEW_AUXC03FX_2026-10-10.md` in
`tools/PcTools/src/kilnctrl/bench_test/cases_aux.py` and `tools/PcTools/tests/test_bench_test_cases_aux.py`.
The 409 texts were checked against `firmware/KilnFW/App/drivers/http/zones_http_post.c`,
`zones_http_get.c`, `zones_http_post_parse.c`, `common/cfg_fs_refusal_http.h` and
`persist/zones_config_store.c`. Reviewed at origin/dev `d3187e59d`.

## Verification

- `uv run pytest tests/test_bench_test_cases_aux.py tests/test_mcp_server_control_zone_relay_mask.py`:
  82 passed.
- `tools\negtest.ps1 -Preset pytest` over both files, 7 mutations, baseline PASS:

| Mutation | Verdict |
|---|---|
| M1 drop the `r4.get("conflicted")` precondition | MISSED |
| M2 `restore()` returns True without the collateral re-check | MISSED |
| M3 `restore()` re-posts only the mask via `control_set_zone_relay_mask` | CAUGHT |
| M4 `c03_teardown_hook` uses `ctx.get` instead of `pop` (not idempotent) | MISSED |
| M5 PASS on `detail.startswith(AUX_OWNER_TEXT)` instead of equality | MISSED |
| M6 drop `zones_config_undecided` from `POST_AUX_409_KEYS` | CAUGHT |
| M7 `restore()` compares the snapshot against itself (`after = snapshot`) | MISSED |

## Answers to the review questions

1. **Do the 409 texts match the firmware's ordering?** Yes, for every 409 that can be sent.
   - The aux check is at `zones_http_post.c:657-667`; its body (663-664) is `AUX_OWNER_TEXT` byte for byte.
   - Every 409 the handler sends before that check falls through to INCONCLUSIVE. These are the mode gate,
     backup restore in flight, kiln-config rollback at risk, the OTA interlocks, the writer claim and the
     `expected_generation` stale 409.
   - `zones_config_undecided` (701), `safety_ceiling_raise_failed` and `zones_config_changed_concurrently`
     (commit lock) are sent after the check. They are in `POST_AUX_409_KEYS` and mapped to FAIL.
   - The commit-lock "run started" refusal is also sent after the check, but its text is the same as the
     entry mode gate's. INCONCLUSIVE is the only honest verdict for it.
   - One post-aux 409 is not covered: `store_unreadable_at_boot`. See LOW-1.
2. **Can the whole-snapshot re-post clobber anything?** Yes. See MED-1.
3. **Is the teardown idempotent?** Yes. `c03_teardown_hook` pops `_c03_restore_fn`, so a second run is a
   no-op. A failed restore taints the run and raises, and the runner turns that into a FAIL. No test pins this
   (M4 MISSED, LOW-3).
4. **Can the precondition read be stale?** No.
   - `_aux_state(ctx)` is a live read taken immediately before the write.
   - The only window is the operator or another session changing the aux binding mid-case. That is a test
     environment violation, not a code defect.
   - `aux_teardown_hook` runs before `c03_teardown_hook`. This is harmless, because the snapshot masks never
     contain relay 4.
5. **Can the case PASS without the firmware refusing?** No.
   - PASS needs the tool's `refused by firmware (HTTP 409)`, which only comes from a real HTTP 409.
   - It also needs a body equal to the owner text after the anchored prefix/suffix strip.
   - A 409 from that check means nothing was committed.

## Findings

### MED-1: [FIXED in auxc03fx3] the restore re-posts rounded GET prints of omit-preserved fields for every zone

`_default_zone_mask_fns.restore()` (`cases_aux.py:470`) posts `zhc.build_post_body(snapshot, {})` with no
`strip_omit_preserved`. `build_post_body` encodes these fields from GET's print, for every zone:

- `model_tau_s` and `model_dead_time_s` at `%.1f` (`zones_http_get.c:467`)
- `fuzzy_strength_pct` at `%.2f`
- `ease_off_window_mult`, `approach_rate_cap`, `error_band`, `progress_band` and `hyst_c` at `%.3f`
- `rate_band` at `%.4f`
- `coil_power_w` at `%.2f`

The firmware accepts posted values for these keys (`zones_http_post_parse.c`, omit-preserve branches), so the
restore overwrites the stored values with rounded ones.

- **Scenario:** zone 1 has a measured `tau` of 812.37 s. AX-C03's write is accepted (a guard regression) or is
  possibly applied. The restore posts `z1_tau=812.4` and the same for every other zone. The firmware stores
  812.4.
  - `_zone_collateral_diff(snapshot, after, ...)` then compares two GET prints, which both read `812.4`, so it
    reports clean. The case logs "original mask restore ok" after silently altering plant-model data on every
    zone.
  - The same write happens again from `c03_teardown_hook` on a retry.
- **Why it matters:** the documented narrow-writer invariant (CLAUDE.md, `control_set_zone_coupling`;
  `mcp_server_control.py:1200-1240`) is that these fields stay bit-exact. `control_set_zone_relay_mask` itself
  strips them, so the bad write cannot have changed them, and re-posting them is pure collateral.
- **Fix:** post `zhc.strip_omit_preserved(zhc.build_post_body(snapshot, {}))`. The firmware then keeps the
  stored values bit-exact. The lossless `%.9g` fields (gains, `k_dc`, coupling) stay safe to re-post.
- **Test:** use the real `build_post_body` (not the `"BODY"` mock in
  `test_default_restore_reposts_whole_snapshot`) and assert that no posted key matches
  `zhc.ZONE_OMIT_PRESERVED_KEY_RE`.

### LOW-1: [FIXED in auxc03fx3] post-commit save failures are not classified as "guard let it through"

After the commit, `nvs_save` or `relay_names_save` can fail. `cfg_fs_http_persist_failed_for`
(`zones_http_post.c:895`) then answers in one of three ways, and in every case the new mask is already LIVE in
RAM:

- 503 if cfg is unmounted
- 500 `could not be saved to flash`
- 409 `store_unreadable_at_boot`

How AX-C03 handles each:

- **409 `store_unreadable_at_boot`:** classified as "409 from a gate before the aux check", which is
  INCONCLUSIVE with **no taint and no restore**.
  - Scenario: a zone keeps a relay_mask that claims relay 4 in RAM. Later aux cases (`AX-T01`, `AX-K01`) are
    not skipped, and they run with the zone and the aux output both driving relay 4.
  - This is nearly unreachable. `degraded("zones.json")` is set only together with `s_zones_cfg_undecided`
    (`zones_config_store.c:105-118`), and undecided is re-checked under the commit lock. It needs the flag to
    flip between that check and `nvs_save`.
- **500 and 503:** these surface from the tool as `error: POST /api/zones failed ...` with status None. They
  take the taint-and-restore path, which is safe, but the verdict is INCONCLUSIVE although the firmware
  provably passed the aux check.
  - Not every 5xx is post-aux. `zones_http_post.c:315` (body malloc 500), `:486` (parse OOM 503) and `:907`
    (scratch 500) come first.
- **Fix:**
  - Add `store_unreadable_at_boot` to a "post-commit, possibly applied" set that taints, restores and FAILs.
  - Map the body texts `could not be saved to flash` and the cfg-unmounted 503 text to FAIL.
  - Leave a bare `out of memory` as INCONCLUSIVE, since it is sent both before and after the check.

### LOW-2: [FIXED in auxc03fx3] the second snapshot GET is outside the try

At `cases_aux.py:457`, `snapshot = zhc.get_zones(host)` is a second GET taken outside the `try` that guards the
first one.

- **Scenario:** a transient HTTP failure on that GET raises out of `_default_zone_mask_fns`. The runner records
  `FAIL "case raised ..."` (`runner.py:609`), a false guard failure.
- **Fix:** keep the first `get_zones` result as the snapshot (one GET, no window between "pick zone" and
  "baseline"), or move the second GET inside the `try` so the case returns None and SKIPs.

### LOW-3: [FIXED in auxc03fx3] tests do not pin four of the fixed behaviours

The negtest MISSED M1, M2, M4, M5 and M7. Each MISSED mutation is a review fix that can regress unnoticed.

- **M1:** a test with relay 4 `ENABLED` but `conflicted` must expect INCONCLUSIVE and no POST.
- **M2 and M7:** `test_default_restore_reposts_whole_snapshot` mocks `_zone_collateral_diff` to `[]`, so a
  restore that skips or self-compares the re-check still passes. Add a case where the re-fetched GET differs
  and expect `restore()` to return False.
- **M4:** call `c03_teardown_hook` twice. Expect the restore function to be called once and the second call to
  be a no-op.
- **M5:** a 409 body of `AUX_OWNER_TEXT + " (extra)"` must not PASS.

### INFO

- Every 2xx and every possibly-applied path taints the run. `_gate` then SKIPs the later aux cases, so a stale
  snapshot from other zone writers between the case and teardown cannot affect them.
- An optional hardening: on PASS, re-read the zone's `relay_mask` and require it to be unchanged. This is not
  needed for correctness, because a firmware 409 at the aux check commits nothing.
