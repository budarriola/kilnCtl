# Review: sweepfx4 (cbe04d2ea + 63bb16e53) -- 2026-10-11

This review covers two origin/dev commits. Together they fix REVIEW_SWEEPFX3 MED-A, LOW-A, LOW-B and LOW-C:

- `cbe04d2ea`, `cases_lcd.py`: `_lcd19_wait_out_lockout()` now always sleeps `_LCD19_LOCKOUT_FIRST_STEP_S` (5.0 s) plus a 1.5 s margin. Its 20 s bound is now measured with the elapsed clock, so read time counts. `_lcd19_relock()` now also requires `ok:true` in the body of the off POST. The commit adds tests, including one that parses `LOGIN_BACKOFF_LADDER_MS[0]` out of `login_backoff.c`.
- `63bb16e53`, `check_worktree_mint.ps1`: the `-not $looseExists` case now uses a directory at `refs/heads/<name>`. A zero-byte file made `symbolic-ref` fail first, so it never reached the clause.

## Summary

There are no HIGH or MED findings. The 5 s first step is correct, and so is how the LCD applies it:

- The LCD PIN uses its own `login_backoff_state_t` (`lcd_keypad_state_t.lockout`, `lcd_auth_state.h:114`). It shares the web login's ladder and functions but not its counters (`lcd_auth_state.h:103-116`), so a web login failure cannot lengthen an LCD lock.
- The lock is panel-wide: one counter covers every role.
- `lcd_keypad_state_submit()` (`lcd_auth_state.c:73-110`) calls `login_backoff_record_failure()` on DENIED. That starts the lock at `LOGIN_BACKOFF_LADDER_MS[failure_count-1]`.
- The keypad shows plain "Wrong PIN" on DENIED (`ui_lcd_keypad.c:194-198`). It shows "Wrong PIN -- try again in %us" (rounded up) only on LOCKED_OUT.
- A LOCKED_OUT submit never reaches `record_failure`.

The relock checks fail closed. Both POSTs need status 200 and a body with `ok` set to `True`, and the home page must settle before the Start tap. The firmware's `set_policy` answers through `set_ok()` (`security_http_core.c:188-200`). The worktree_mint case is real: the negative test below catches it.

The step is correct only for a ladder at rung 0, though. See LOW-A.

## Runs

| Run | Result |
|---|---|
| `negtest -Preset pytest -PresetArg "tests/test_bench_test_cases_lcd.py -k Lcd19"`, baseline | 96 passed |
| Mutation: drop the unconditional sleep | CAUGHT (2 tests) |
| Mutation: `_LCD19_LOCKOUT_FIRST_STEP_S` changed to 3.0 | CAUGHT (C-ladder pin test plus the wait-out test) |
| Mutation: unconditional sleep changed to 4.0 s | CAUGHT |
| Mutation: drop the off-POST body check / drop the on-POST body check / drop the off-POST status check / skip the home-settle check | each CAUGHT by its own `test_failed_relock_*` |
| Mutation: bound the loop by a counter that never advances (an elapsed-clock regression) | the test hung in an endless loop and I killed it; negtest reports MISSED (exit -1). See INFO-1. |
| `negtest -Preset check -PresetArg tools/check_worktree_mint.ps1`, mutation: remove `-and -not $looseExists` | CAUGHT ("loose ref path present + empty reflog: not unborn, refused" FAIL); the baseline passed |

## Findings

### LOW-A: a fixed 6.5 s wait assumes the LCD ladder is at rung 0, but the rung persists across cases and runs

The keypad state `s_ks` is a static (`ui_lcd_keypad.c:93`) that is initialised once per boot. `lcd_keypad_state_init()` is called only from `build_overlay()` (`ui_lcd_keypad.c:238`), and `build_overlay()` runs only while `s_mbox` is NULL (`:296-297`). `close_overlay()` only hides the msgbox and never sets `s_mbox` back to NULL, so later opens skip the init. `ui_lcd_lock_force_lock()` and PIN changes leave the lockout alone too.

`failure_count` goes back to 0 only in two cases: after a GRANTED submit, or through `login_backoff_cycle_reset_if_due()` after the 5th rung has expired. So a wrong PIN with no later success leaves the next wrong PIN on the 10 s rung, and then the 30 s rung. Examples of such a leftover:

- an earlier LCD-19 whose right PIN was `entry_incomplete`, landed LOCKED_OUT, or was never typed
- an LCD-25 PIN that was rejected

On the verified path, LCD-19 types no correct PIN before its wrong one. So on rung 1 or higher, the right PIN lands inside the lock, after the fixed 6.5 s. The lock is invisible: the screen shows plain "Wrong PIN", so the `try again in` loop never runs. The right PIN then reads LOCKED_OUT, `pin_lockout` is set, and the case is INCONCLUSIVE.

This fails safe: it is never a false FAIL or a false PASS. It is a coverage gap, though: the case cannot pass again until a correct PIN is entered somewhere.

Fix: if the right PIN's stable read shows "try again in Ns", sleep N+1 s and type the right PIN once more. A LOCKED_OUT submit does not touch `failure_count`, so retrying is free. The `BackoffUi` fake models a single 5 s rung, so it cannot exercise this. Add a test where the fake's first wrong PIN sets a 10 s lock.

### LOW-B (pre-existing, not introduced by sweepfx4): a wrong PIN submitted while a lock is already running counts as "refused"

If a lock is already running when the wrong PIN is typed, firmware returns LOCKED_OUT and never checks the PIN. It clears the dots, keeps OK and Cancel, and shows "try again in Ns". `_dots_have_cleared()` plus OK and Cancel then sets `wrong_pin_refused = True` (`cases_lcd.py`, about line 3604).

`_lcd19_wait_out_lockout()` then sees the countdown (`lockout_seen`) and waits. The right PIN grants, and LCD-19 can PASS even though the wrong PIN was never rejected on its merits.

This needs a lock that is still running when LCD-19 starts, for example an LCD-25 rejection less than 5-300 s earlier. That makes it unlikely, but it gives the "wrong PIN refused" stage a false PASS.

Fix: if `after_wrong_pin_names` contains "try again in", leave `wrong_pin_refused` at None, or wait the lock out and type the wrong PIN again.

### INFO-1: the elapsed-clock bound is only guarded by a hang

In `test_lockout_wait_bound_counts_read_time`, a regression that stops the loop's bound from advancing turns into an infinite loop, not an assertion failure. The pytest process hangs until someone kills it.

Optional fix: give the fake `_wait_stable_names` an iteration cap that raises, so the test fails cleanly.

### INFO-2: small mismatches between the fake and the firmware

`BackoffUi` floors the remaining time (`int()`); the firmware rounds up (`(ms+999)/1000`). The fake also shows "Wrong PIN" on a fresh keypad before any submit, where the firmware shows the empty dots label. Neither difference changes any assertion: the regex matches both, and `_dots_have_cleared()` ignores names that are not all `*`.

### INFO-3: the directory-ref case covers the clause, not a realistic corruption

A zero-byte loose ref already fails closed earlier, at `symbolic-ref`. The `-not $looseExists` clause now matters only for odd shapes, such as a directory, or a D/F conflict with `refs/heads/<name>/...`. Refusing those is correct. The new case does prove the clause is live (negtest CAUGHT).
