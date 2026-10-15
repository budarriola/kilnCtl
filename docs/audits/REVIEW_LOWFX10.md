# REVIEW_LOWFX10: adversarial review of the lowfx10 batch

Scope: `63bebb00f..013ccd9b3` on origin/dev, which is two commits:
- `424aecbd4`: lowfx10, the UPDLOWFX and RECQRFX LOW fixes (WIP).
- `013ccd9b3`: the fix-status lines for UPDLOWFX and RECQRFX.

The batch fixes REVIEW_UPDLOWFX.md LOW-1..4 and REVIEW_RECQRFX.md LOW-1..4. The board was not touched.

Result: HIGH 0, MED 0, LOW 1, INFO 7.

Fix status (xipfx, 2026-10-14): LOW-1 fixed. `update_install_staged` sets the downgrade sentinel only when the follow-up read answers 404; any other error (timeout, failed relogin, 5xx) gets neutral wording. Test: `test_non_404_tail_read_error_is_not_called_a_downgrade` (timeout and 502).

## Verified claims

- **No new internal RAM.** `recovery_lcd.c` adds no static. The new helpers
  (`text_mode_now()`, `qr_visible_now()`, `rlcd_band_width()`, `rlcd_qr_paint()`) use
  only locals and `static inline` header code. The PcTools changes are host-side only.
- **BUSY in-flight answer** (`mcp_server_update.py:773-775`). A second confirmed call
  for the same host now answers `BUSY:` instead of `REFUSED:`. RL-D01's definite set
  (`cases_release.py:885`) matches only `error: could not read GET`, `REFUSED` and
  `FAILED: install refused or lost: HTTP 4xx`. So BUSY falls through to "may have run":
  it never clears the stage and never returns PASS.
  - That is the right side to fail on. The first call's install may be live, and the PC
    cannot clear the stage while the application is down anyway.
  - No other caller parses the install tool's `REFUSED` answer (checked with grep across
    `tools/PcTools/src`).
- **Unreadable last_install becomes UNKNOWN** (`mcp_server_update.py:754-758`). The gate
  is `lost is None and not before.get("staged") and state == "unreadable"`.
  - A board that has a stage and an unreadable record still reaches the "would install"
    REFUSED path. That ordering is correct.
  - A board with no stage and an unreadable record can no longer be read as a proven
    "nothing staged" refusal.
- **Default wait.** `update_install_staged(wait_s=_SYNC_WAIT_CAP_S)` sits at
  `mcp_server_update.py:888`, so the default is 270 s.
  - The deadline is `t0 + min(wait_s, 270)`.
  - The in-loop GET timeout is `max(1.0, min(5.0, remaining))`.
  - `_reboot_proven` (`:638`) clamps its own timeout to `min(5.0, deadline - now)`.
  - The nominal worst case is about 18 s past the deadline (see INFO-2), so about 288 s.
    That is under the client's 300 s silence abort.
- **Recovery LCD clipping on the real 480x320 landscape geometry.**
  `rlcd_band_width()` (`recovery_lcd.c:339`) gives width 356 to a band that starts above
  y 124 only while the QR is visible. Y_JOIN 126 and every row below it stay full width.
  - The longest top-band text is "Boot guard count: unreadable": 28 chars x 12 px = 336 px,
    which is under 356. The title is 13 x 18 = 234 px. So no operator-needed text is lost.
  - Anything that is clipped was previously drawn under the QR anyway.
  - In ERROR and NET_FAIL modes, `qr_visible_now()` is false and `draw_status()` paints
    neither the passphrase nor the QR. The pre-paint blank clears a stale QR.
  - All setters and `draw_status()` run under `s_lock`, so the band width and the QR
    decision cannot race.
- **`lcd_ap_reader.py` region** (`:896`). `RECOVERY_QR_REGION = (0.5, 0.0, 1.0, 0.5)` is
  tested against the symbol centre as fractions of the original frame.
  - I mapped the QR through the `FRAME_CORNERS` homography: the square lands at
    x 807..1045, y 111..343 of 1280x720, with its centre at about (925, 229).
    That is fractions (0.72, 0.32), well inside the region.
  - The decoy tests cover the left half, the lower half and the top-left corner.

## Negative tests (re-run by this review)

pytest preset, `tools\negtest.ps1`, mutations over `mcp_server_update.py` and
`cases_release.py`. Baseline PASS. The run reported `real_tree_unchanged: true`.

| Mutation | Verdict | Failing assertion |
|---|---|---|
| BUSY answer back to REFUSED | CAUGHT | `FlightAndStaleTests::test_second_confirmed_call_while_one_is_in_flight_is_refused` (and the marker test) |
| RL-D01 treats BUSY as definite | CAUGHT | `test_install_non_definite_error_is_may_have_run_and_never_cleans_up[BUSY: ...]` |
| unreadable->UNKNOWN branch off | CAUGHT | `RecoveryApplyingTests::test_unreadable_install_record_is_unknown_not_ok` |
| default wait 270 -> 240 | CAUGHT | `RecoveryApplyingTests::test_default_wait_leaves_margin_over_the_bench_return_time` |
| tail `_reboot_proven` gets a 60 s deadline | MISSED | equivalent mutant: `_reboot_proven` caps each read at 5 s, so no test can observe it |
| tail sentinel on any error vs 404 | CAUGHT | `RecoveryApplyingTests::test_404_while_app_status_answers_is_an_app_without_the_install_route` |

`check_recovery_lcd_policy.ps1` ran directly: PASS, 154 assertions, and every self-mutant
failed as required. Its header mutants are compiled with MSVC and run. A `cl` failure
throws and fails the check, so a mutant that does not compile cannot be scored as caught.

## LOW

**LOW-1: any tail-read error is reported as "an application WITHOUT the install route (a downgrade?)".**
`mcp_server_update.py:846-870`.

The tail re-read runs when the board answers `/api/status` after the deadline. If its
`get_install_status()` raises `UpdateHttpError` for any reason, `n404` becomes negative
(`:865`). That includes a timeout (status None), a 401 whose relogin failed, and a 5xx,
not only a 404. The `elif n404 < 0` branch (`:868-870`) then asserts that the board
"runs an application WITHOUT the install route (a downgrade?), not the recovery image
applying".

Failure scenario:
1. A good install finishes and the new application comes up just after the last in-loop
   poll.
2. `/api/status` is ROUTE_TIER_OPEN and answers within 2.5 s.
3. `/api/update/status` is ROUTE_TIER_ADMIN, and the session died with the reboot. So
   `http_auth.urlopen` must log in again (a slow KDF) and retry, which takes longer than
   the 2.5 s `_TAIL_PROBE_S`.
4. The read times out, and the operator is told the board may have been downgraded to an
   image that has no install route. The image actually has the route and probably
   installed fine.

The verdict stays UNKNOWN, so nothing acts on it wrongly. But the text contradicts the
UPDLOWFX fix-status claim that this wording appears "only when the install route stays
absent". No test covers a non-404 tail error.

Fix: set the sentinel only when `e.status == 404`. Report any other tail error as
"install record unreadable after reboot". Add a timeout or 401 tail test.

## INFO

- **INFO-1: BUSY becomes job state FAILED.** `classify_install_report`
  (`mcp_server_update.py:918-926`) maps everything except `ok`/`UNKNOWN` to `failed`, and
  its docstring lists only FAILED, REFUSED and error. A `bench_test_job_status` reader
  sees FAILED for a call that never ran. The report text says BUSY, so this is cosmetic.
  Consider a `busy` (or `refused`) class.
- **INFO-2: the overrun comment understates the bound.** `:849` says "<~ 8 s" and the
  docstring at `:907` says "total call under 290 s". The nominal worst case past the
  deadline is about 18 s, built from these steps:
  - the last in-loop GET: up to 1 s, or about 3 s with a relogin;
  - the uptime probe: 2.5 s;
  - the tail GET: 2.5 s, or 7.5 s with a relogin;
  - `_reboot_proven`: up to 5 s.

  That gives about 288 s, so the call still fits under 300 s. But the margin is about
  12 s, not 30 s, and urllib timeouts are per operation (connect plus read), so the bound
  is nominal.
- **INFO-3: the LOW-3 late-landing rescue does not cover the observed bench case.** The
  tail is gated on `n404 > 0`. benchupd5 (`docs/BENCH_TEST_LOG.md`) returned "board never
  answered the application API": no 404 was ever seen during the real apply, so the tail
  would not have run.
- **INFO-4: the `recovery_lcd.c` wiring is source-scanned, not compiled, by the host
  test** (`test_recovery_lcd_policy.c:243`). A refactor that keeps the scanned string but
  changes the call's effect would pass. The policy header itself is compiled and tested
  exhaustively.
- **INFO-5: the recovery target build is not exercised under `-Fast`**, and `424aecbd4`
  is labelled WIP. The recovery image needs a full `check_00_kilnfw_recovery_target_build`
  (and a `check_recovery_image_size`) before release.
- **INFO-6: the band-width tests use literal 480/124**
  (`test_recovery_lcd_policy.c:525-531`). No static assertion ties `RQR_AREA_PX < Y_JOIN`
  in `recovery_lcd.c`. If a future QR size grows past 126, the join row would silently
  clip.
- **INFO-7: the `lcd_ap_reader.py:893` comment numbers are slightly off.** The comment
  says x 820..1045, y 118..327. The homography gives x 807..1045, y 111..343. Both are
  well inside the region, so only the comment is off.
