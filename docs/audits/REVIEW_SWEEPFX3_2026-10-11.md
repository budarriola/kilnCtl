# Review: sweepfx3 (2d191c82f) -- 2026-10-11

Reviewed commit `2d191c82f` on origin/dev. It claims to fix REVIEW_SWEEPFX2 MED-A, LOW-A, LOW-B and LOW-C:

- **MED-A (LCD-19):** `_lcd19_relock()` (policy `lcd_enabled` off then on) runs before the second Start tap.
- **LOW-A (LCD-19):** a lost OK tap, a closed keypad or a lockout is now INCONCLUSIVE and no longer marks the PIN bad. `_lcd19_wait_out_lockout()` was added before the right PIN.
- **LOW-B (`tools/worktree_mint.ps1 -Remove`):** only a genuinely unborn HEAD skips the unlanded check. Any other unresolvable HEAD is REFUSED unless `-ForceUnverifiable` is given.
- **LOW-C (`tools/check_wt_status.ps1`):** new `failprune` fixture.

Line numbers below are for `2d191c82f`.

## Summary

There are no HIGH findings. LCD-19 still cannot PASS without proof. `judge_lcd_pin_lock()` (`judgments.py:1600`) returns PASS only when `keypad_raised`, `wrong_pin_refused`, `right_pin_started` and `stop_gated` are all `True`:

- `wrong_pin_refused` needs a stable read with OK+Cancel present and the dots cleared.
- `right_pin_started` needs a stable read with OK absent and Cancel present.

None of the new INCONCLUSIVE paths sets either one to `True`. The relock is correct against the firmware: `web_auth_policy_transition()` (`web_auth_store.c:718-723`) clears the LCD session on the `lcd_enabled` off->on edge. If the relock silently fails to lock, the re-raise check (`unverified_reraise_failed`) yields INCONCLUSIVE, not a PASS. The mint change touches only the `-Remove` block. The normal mint paths still work.

There is one MED. The new lockout wait keys on text that the firmware never shows after a wrong PIN, so in practice it never waits.

## Runs

| Run | Result |
|---|---|
| `tests/test_bench_test_cases_lcd.py` (`uv run --frozen pytest`, worktree) | 234 passed |
| `tools/check_worktree_mint.ps1` | all cases passed (incl. default origin/dev, `-Base origin/main`, deleted-branch-ref) |
| `tools/check_wt_status.ps1` | passed, 129 assertions |
| Manual: `worktree_mint.ps1 -NoSubmodules` (2d191c82f copy), HEAD == origin/dev; `git checkout -b` a new branch; `-Remove` | minted, then REMOVED, rc 0 |
| Mint without `-NoSubmodules` (this review's worktree) | submodules initialised, OK |
| negtest `-Preset pytest`, 12 own mutations (`-Parallel 4`) | 8 CAUGHT, 4 MISSED (below) |
| negtest `-Preset check tools\check_worktree_mint.ps1`, 5 mutations | 4 CAUGHT, 1 MISSED |
| negtest `-Preset check tools\check_wt_status.ps1`, 2 mutations | 2 CAUGHT (LOW-C closed) |

I reran the whole pytest file with the final test set in place. All 12 pytest mutations ran against that same final file, which also covers the fixer's earlier mutation set. The baseline passed every time, and negtest reported the real tree unchanged.

| Mutation (cases_lcd.py unless noted) | Verdict |
|---|---|
| P1 relock result ignored (`relock_ok = True`, relock still called) | **MISSED** |
| P2 relock call removed | CAUGHT |
| P3 relock ignores POST status (`ok = True`) | **MISSED** |
| P4 relock ignores home-settle (`return True`) | **MISSED** |
| P5 keypad_closed branch removed (falls into "rejected", marks PIN bad) | CAUGHT |
| P6 submit_not_applied branch removed | CAUGHT |
| P7 "rejected" no longer marks PIN bad | CAUGHT |
| P8 lockout regex never matches | CAUGHT |
| P9 lockout wait not called | CAUGHT |
| P10 right-PIN lockout detection removed (lockout reads as FAIL) | CAUGHT |
| P11 lockout-wait bound 20 s -> 2000 s | **MISSED** |
| P12 unverified lockout outcome recorded as "rejected" + bad | CAUGHT |
| M1 mint: unborn test ignores reflog | CAUGHT |
| M2 mint: unborn test ignores the loose-ref file | **MISSED** |
| M3 mint: any unresolvable HEAD treated as unborn | CAUGHT |
| M4 mint: `-ForceUnverifiable` ignored | CAUGHT |
| M5 mint: REFUSED path exits 0 | CAUGHT |
| W1 wt_status: reason filter `Where-Object { $false }` | CAUGHT |
| W2 wt_status: `$failed++` dropped | CAUGHT |

## MED

### MED-A: the lockout wait never fires against real firmware, and the test fake hides this

`_lcd19_wait_out_lockout()` (`cases_lcd.py:3003`) waits only if the post-wrong-PIN read contains `"try again in Ns"`. The firmware never shows that text after a wrong PIN:

- A wrong PIN outside a lockout returns `LCD_KEYPAD_SUBMIT_DENIED` (`lcd_auth_state.c:102-105`). The keypad then shows plain `"Wrong PIN"` (`ui_lcd_keypad.c:194-198`), even though `login_backoff_record_failure()` has just started a 5 s lock (`login_backoff.c:3,26`, ladder step 1 = 5000 ms).
- `"Wrong PIN -- try again in %us"` (`ui_lcd_keypad.c:176-192`) appears only on `LCD_KEYPAD_SUBMIT_LOCKED_OUT`. That is the result of a submit made *during* the lockout.

So `after_wrong_pin_names` never holds the lockout text, `lockout_seen` stays False, and the helper returns at once. The right PIN is typed straight away:

- If the typing and the OK tap finish less than 5 s after the wrong-PIN submit, the firmware answers LOCKED_OUT. The new right-path check (`cases_lcd.py:3647`) then records `pin_lockout`, and the case is INCONCLUSIVE.
- This affects both the self-seeded path and the verified pre-set PIN path that MED-A just unblocked. The unverified GRANTED step resets the ladder (`login_backoff_record_success`), so the next wrong PIN starts a fresh 5 s lock.
- Whether the right PIN lands in the lockout depends on how fast the run types. It no longer causes a false FAIL, which is what the commit fixed. But the bench can still read INCONCLUSIVE on runs that should PASS, and the "waits out the 5 s lockout" claim is not true on hardware.

The test hides this. `test_wrong_pin_lockout_is_waited_out_before_right_pin`'s `BackoffUi` (`test_bench_test_cases_lcd.py:2805-2834`) puts `"Wrong PIN -- try again in Ns"` on screen right after a DENIED wrong PIN. That is the opposite of `ui_lcd_keypad.c`. The test passes against a model the firmware does not follow.

**Suggested fix:** after `wrong_pin_refused`, wait unconditionally until at least 5 s (ladder step 1, plus a margin) have passed since the wrong-PIN OK tap. Keep the text-driven wait only as an extra step for a run that sees LOCKED_OUT. Make the fake show `"Wrong PIN"` on DENIED and the lockout text only on a submit inside the lockout. Then assert that a right PIN typed with no wait comes back INCONCLUSIVE, and that with the fix it comes back `right_pin_started=True`.

## LOW

### LOW-A: no test covers a failed relock (P1, P3, P4 MISSED)

The production code handles a failed relock correctly:

- `_lcd19_relock()` requires both POSTs to return 200, the on-POST to say `ok:true`, and the home page to settle.
- Any failure skips the Start tap and records `unverified_reraise_failed`, which gives INCONCLUSIVE.
- The `finally` block restores the original policy, so a failed on-POST does not leave `lcd_enabled` off.

But no test makes `set_policy` fail or makes `_wait_for_home_settled` return False during the relock. Three mutations went uncaught because of this: ignoring the relock result, ignoring the POST status, and ignoring the settle. Today they would only turn INCONCLUSIVE into a different INCONCLUSIVE, because the re-raise check still catches a session that did not lock. But a regression here would not be detected. A minor point: the off-POST's body is not checked for `ok:true`, only its status. A 200 with `ok:false` would make the on-POST a no-op re-save, which is not an edge, so no relock happens. The re-raise check would catch that too, but it would be reported as a re-raise failure, not as a relock failure.

### LOW-B: the lockout-wait bound is untested, and its real worst case is about 140 s, not 20 s (P11 MISSED)

The loop is bounded: `waited < max_wait_s` with `step >= min(1 s, remainder)`. But each pass also calls `_wait_stable_names(timeout_s=6.0)`, and that time is not counted in `waited`.

- The firmware's lockout text is a one-shot string. It is not refreshed (`ui_lcd_keypad.c:180-187`, "No periodic refresh").
- So once LOCKED_OUT text is on screen, it stays until a digit is typed. The loop sleeps `secs+1` again and again until the 20 s sleep budget is spent.
- Each pass can also spend up to 6 s reading. With small `secs` values that adds up to roughly 20 + 20x6 s.

It is still bounded, and today it only runs when the case itself saw LOCKED_OUT, which per MED-A it almost never does. Count read time against the budget, or stop after the first wait, because the text can never clear by itself. Add a test that keeps the text on screen and checks the total time.

### LOW-C: the mint loose-ref clause is untested (M2 MISSED)

The `-not $looseExists` term in the unborn test can be removed and `check_worktree_mint.ps1` still passes. It is defence in depth: an unreadable loose file for HEAD's own branch, in a worktree whose reflog is empty. The existing zero-byte-ref case covers a different ref and goes through the rev-list path. Add a case with a zero-byte `refs/heads/<branch>` in an orphan-style worktree with no reflog.

## INFO

- **Unborn heuristic assumes reflogs:** in a repo with `core.logAllRefUpdates=false`, a deleted branch ref with unlanded commits has an empty `logs/HEAD`. It would be classed as unborn and removed without the unlanded check. The default for a non-bare repo is true, so this does not happen in this repo.
- **Fail-closed on an orphan branch after history:** `git checkout --orphan` in a worktree that has history leaves a non-empty `logs/HEAD`. `-Remove` then needs `-ForceUnverifiable`. That is correct, because the earlier detached commits may be unlanded.
- **The relock fake fires on the wrong edge:** the fake (`_wire_relock`) relocks on the `lcd_enabled=False` POST, while the firmware relocks on the off->on edge. The end result is the same for the tests.
- **Classifying a right-PIN rejection:** with the lockout expired, a real rejection of the right PIN shows `"Wrong PIN"` with OK present. That gives `right_pin_started=False`, which is a FAIL, as it should be. Only a submit inside the lockout (LOCKED_OUT text) becomes INCONCLUSIVE, and there the firmware never checked the PIN. In the unverified path, a closed keypad with no Confirm dialog is INCONCLUSIVE and the PIN is not marked bad. That cannot hide a PASS, because nothing reaches `right_pin_started=True` from there.
- **Normal mint paths:** default origin/dev, `-Base origin/main`, `-NoSubmodules`, minting without it, and `-Remove` of a new-branch worktree all behave as before.

## Fix status

| Finding | Status | Fix |
|---|---|---|
| MED-A | Fixed | `_lcd19_wait_out_lockout` always sleeps `_LCD19_LOCKOUT_FIRST_STEP_S` (5 s, pinned by a test to `LOGIN_BACKOFF_LADDER_MS[0]` in `login_backoff.c`) plus a 1.5 s margin; the text-driven wait is now only an extra. `BackoffUi` fake shows plain "Wrong PIN" on DENIED and the countdown only for a submit inside the lock. |
| LOW-A | Fixed | `_lcd19_relock` also requires the off-POST body `ok:true`; four failed-relock tests (on-POST not ok, off-POST status, off-POST body, home not settled) catch P1, P3, P4. |
| LOW-B | Fixed | Bound uses elapsed clock time (reads included); `test_lockout_wait_bound_counts_read_time` catches P11. |
| LOW-C | Fixed | `check_worktree_mint.ps1` case: zero-byte loose ref + empty reflog is refused, not unborn (M2). |
