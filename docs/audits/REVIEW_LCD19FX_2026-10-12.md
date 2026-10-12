# Review: LCD-19 lockout fixes (origin/dev 5033604f3), 2026-10-12

Adversarial review of `5033604f3`, which fixes REVIEW_SWEEPFX4 (`docs/audits/REVIEW_SWEEPFX4_2026-10-11.md`)
LOW-A, LOW-B, INFO-1 and INFO-2 for the LCD-19 bench case.

- Code: `tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py`
- Tests: `tools/PcTools/tests/test_bench_test_cases_lcd.py`
- Firmware it is checked against:
  - `firmware/KilnFW/App/drivers/net/login_backoff.c`
  - `firmware/KilnFW/App/drivers/ui/lcd_auth_state.c/.h`
  - `firmware/KilnFW/App/drivers/ui/ui_lcd_keypad.c`
  - `firmware/KilnFW/App/drivers/ui/screen_idle.c`

Result: no HIGH or MED findings. There are four LOW findings and seven INFO items. The fix does what it claims:
- A retry cannot turn into a PASS on its own.
- A missing or unparseable N is fail-closed.
- `None` never becomes PASS.

The gaps are:
- A false FAIL on the verified right-PIN path. This predates the commit, and the retry doubles the exposure.
- No client-side cap on the retry's N.
- A wake-from-blank gap on long rungs.
- A refusal that is inferred from absence, not from a positive label.
- Weak assertions. Negtest shows four of ten mutations MISSED.

## Firmware facts used

- **Ladder.** `LOGIN_BACKOFF_LADDER_MS[5] = {5000, 10000, 30000, 60000, 300000}` (`login_backoff.c:3`).
  - DENIED raises the rung and sets `locked_until` (`:26`).
  - GRANTED resets the ladder.
  - The ladder resets itself once the fifth rung has expired.
- **Submit outcomes** (`lcd_keypad_state_submit`). LOCKED_OUT does not check the PIN and does not count a failure. DENIED counts a failure. GRANTED resets the ladder.
- **Labels** (`ui_lcd_keypad.c`). Dots and status share `s_info_label` (`:90`).
  - DENIED sets "Wrong PIN" (`:198`).
  - LOCKED_OUT sets a one-shot "Wrong PIN -- try again in %us" (`:191`). N = ceil(remaining/1000) is computed at submit time, so N <= 300, and it is never refreshed.
  - A digit or backspace calls `refresh_dots()`, which replaces the status (`:229`, `:232`).
  - `ui_lcd_keypad_show` resets the label to empty dots.
- **Backdrop.** A tap on the backdrop at x < 40 cancels the keypad.
- **Blanking.** `screen_idle` swallows the waking touch on a blanked panel. The shortest display timeout is 1 minute.

## Answers to the brief

1. **Can a retry PASS without a fresh, independently checked wrong-PIN rejection?**
   No. The retry block only runs after the wrong-PIN stage has already set `wrong_pin_refused` (True, or None during a running lock). It retypes only the right PIN, and the judge still needs `wrong_pin_refused is True`. A GRANTED on the retry is genuine right-PIN evidence. However, the earlier `wrong_pin_refused=True` is itself inferred from absence (LOW-4).
2. **Is a missing or unparseable N fail-closed?** Yes.
   - A probe with the label "try again in a bit" gave `pin_lockout=True`, no `right_pin_lockout_retry`, and INCONCLUSIVE.
   - No committed test covers this (INFO-3).
3. **Is the N+1 wait bounded?** Not on the client side (LOW-2).
   - A probe with "try again in 100000s" slept 100001 s.
   - The firmware caps N at 300, so this only bites with a corrupt or misread label.
   - The 300 s rung also exceeds the 1-minute display timeout (LOW-3).
4. **Is None INCONCLUSIVE and never PASS?** Yes. `judge_lcd_pin_lock` returns FAIL on any False and INCONCLUSIVE on any None. The weak point is that the during-lock INCONCLUSIVE reason is the generic "missing UI_TEST API or camera" text (INFO-2), and the test that pins it only asserts `!= PASS` (INFO-1).
5. **Does the fake now match the firmware?** Only partly (LOW-4, INFO-5).
   - The countdown now uses ceil, but it is computed live at read time. The firmware fixes it at submit time.
   - The fake shows "Wrong PIN" whenever the keypad has no countdown. The firmware shows it only after a DENIED, until the next digit, backspace or show.
   - The second half of INFO-2 from REVIEW_SWEEPFX4 is therefore not fixed.

## Runs

All runs were in the minted worktree `C:\wt\lcd19fx_78z97s` (HEAD 5033604f3), never in the main tree.

| Run | Result |
|---|---|
| `pytest tests/test_bench_test_cases_lcd.py` | 243 passed, 87 s |
| Probe P1: OK on the right PIN reported ok but never applied, verified path | FAIL "the correct PIN did not start the firing", `right_pin_started=False` (false FAIL, LOW-1) |
| Probe P2: label "try again in 100000s" | sleeps `[2.5, 0.5, 6.5, 100001.0]` (unbounded, LOW-2) |
| Probe P3: label "try again in a bit" | INCONCLUSIVE, `pin_lockout=True`, no retry (fail-closed, correct) |
| Probe P4: wrong PIN during a running lock | INCONCLUSIVE with generic reason "could not exercise: wrong_pin_refused, right_pin_started, stop_gated (missing UI_TEST API or camera...)" (INFO-2) |
| `negtest -Preset pytest -PresetArg "tests/test_bench_test_cases_lcd.py -k Lcd19"`, 10 mutations (appendix) | baseline PASS; CAUGHT M1 M2 M3 M4 M8 M9; MISSED M5 M6 M7 M10; real tree unchanged, copies removed |

The probes were a throwaway untracked test file and were deleted before the commit.

## Findings

### LOW-1: The verified right-PIN path turns a lost OK click into a false FAIL

**Where:** `cases_lcd.py:3673`, `right_pin_started = "OK" not in names and "Cancel" in names`.

**Problem:**
- After `enter_pin_verified(right_pin)` and a stabilized read, any keypad that is still showing becomes `right_pin_started=False`, and the judge returns FAIL.
- If the OK click was reported but never applied (a lost injection), the keypad still shows the full dots and no status. That is an instrument failure, not a firmware fault.
- The unverified right-PIN-first path already tells these apart (`submit_not_applied`, `:3531`). The verified path does not.
- The retry adds a second submit, which doubles the chance of hitting this.

**Fix:** Give the verified path the same classification as the unverified one:
- `False` only when the dots have cleared and a "Wrong PIN" label is visible. That is a genuine DENIED of the correct PIN.
- `None` with `right_pin_outcome="submit_not_applied"` when the dots still show the full entry.
- `None` otherwise.
- Add a fake test whose OK is reported ok but not applied, and assert INCONCLUSIVE.

### LOW-2: No client-side cap on the retry's N

**Where:** `cases_lcd.py:3678-3680`.

**Problem:**
- `max(_secs) + 1.0` is slept with no ceiling.
- The firmware cannot print more than 300, but a misread or corrupted label (for example OCR or targets noise, or extra digits) would hang the case for as long as the digits say.
- The runner has no per-case timeout.

**Fix:**
- Define `_LCD19_LOCKOUT_MAX_S = 300`, pinned to `LOGIN_BACKOFF_LADDER_MS[4]/1000` by a test that parses `login_backoff.c`.
- If N is above the cap, set `state["pin_lockout_n_out_of_range"]=N` and report INCONCLUSIVE without sleeping.
- Otherwise wait with the bounded `_lcd19_wait_out_lockout` (re-read the label, `max_wait_s` about cap + 5).
- Add a test for "try again in 100000s" that asserts no sleep above the cap, and INCONCLUSIVE.

### LOW-3: Waits on rung 4 and above outlast the display timeout, so the retry's first digit is swallowed

**Problem:**
- Rungs 60 s and 300 s are at or above the shortest display timeout (1 minute). `screen_idle` swallows the waking touch.
- The first digit of the retyped PIN only wakes the panel. `enter_pin_verified` sees the dot fail to advance, giving `entry_incomplete`, then INCONCLUSIVE.
- The result is fail-safe, but the retry is useless exactly on the long rungs it exists for.
- A 300 s sleep also sits right at the MCP client's 300 s idle watchdog when the case runs through `bench_test_run` instead of `bench_test_start`.

**Fix:**
- After the wait and before retyping, read `touch.get_state().screen_on`. If it is False, send one wake tap and poll until it reads on.
- Never send a wake tap to an awake panel at x < 40, because that is the keypad backdrop and cancels it. Use a point inside the keypad's non-button area, or wake only when the panel is confirmed off.
- Log progress at least every 60 s during long waits.
- Document that LCD-19 with a long rung must run through `bench_test_start`.

### LOW-4: `wrong_pin_refused=True` is inferred from the absence of dots and countdown, not from a positive "Wrong PIN" label

**Where:** `cases_lcd.py:3607`.

**Problem:**
- The wrong-PIN stage passes when OK and Cancel are present and the dots have cleared.
- The firmware's DENIED shows a positive "Wrong PIN" label, which the case never requires.
- Any state with the keypad up and an empty label also passes, for example a keypad that was re-shown (`ui_lcd_keypad_show` resets the label to empty dots).
- The fake hides this because it shows "Wrong PIN" in every keypad state without a countdown. That is also why INFO-2 of REVIEW_SWEEPFX4 is only half fixed.

**Fix:**
- Require an exact "Wrong PIN" target name (no "try again in") for `wrong_pin_refused=True`. Keep `None` for "cleared but no label".
- Fix the fake so that:
  - "Wrong PIN" appears only after a DENIED submit;
  - any digit, backspace or show clears it;
  - LOCKED_OUT shows the countdown fixed at submit time.
- Add a test where a cleared keypad with no label gives INCONCLUSIVE.

### INFO-1: The LOW-B test assertion is too weak

`test_wrong_pin_during_running_lock_is_not_refused` asserts only `wrong_pin_refused is None` and `verdict != PASS`. M5 turns the branch into a false FAIL through `keypad_raised=False`, and M6 drops the `wrong_pin_during_lock` flag. Both were MISSED.

**Fix:** Assert `verdict == INCONCLUSIVE`, `observed["wrong_pin_during_lock"] is True` and `keypad_raised is True`.

### INFO-2: The during-lock INCONCLUSIVE reason is generic

Probe P4 shows the reason "missing UI_TEST API or camera". That sends an operator to check the wrong subsystem.

**Fix:** When `wrong_pin_during_lock` is set, override the reason to say that a login lockout from an earlier run was still active, and to wait it out (N s) and rerun.

### INFO-3: No committed test for the fail-closed unparseable-N path

Probe P3 confirms the current behavior, but nothing pins it.

**Fix:** Add a test with "try again in a bit" that asserts INCONCLUSIVE, `pin_lockout` set and no `right_pin_lockout_retry`.

### INFO-4: The retry covers only one of three lockout paths

- The unverified right-PIN-first path (around `:3474-3537`) still gives INCONCLUSIVE "lockout" with no wait-and-retry.
- The wrong-PIN-during-lock path gives INCONCLUSIVE with no wait-and-retry either.

Both are fail-safe, so this is a coverage gap, not a defect.

**Fix:** Route all three through one bounded helper (LOW-2), or record in the case doc that only the verified right-PIN path retries.

### INFO-5: The fake's countdown is live, but the firmware's is fixed at submit time

`BackoffUi` computes `math.ceil(self._until - now)` on every read. The firmware writes the label once. M10 (ceil to int) was MISSED, so the INFO-2 ceil change is untested, and M7 (margin +1 to +0) was MISSED as well.

**Fix:** Have the fake snapshot N at submit. Add a test with a fractional remaining time (for example 9.2 s, so the label must read 10) that asserts the retry sleep is at least N+1.

### INFO-6: `_lcd19_wait_out_lockout`'s loop is dead in production

After LOW-B, `wrong_pin_refused=True` names never contain "try again in", so the loop after the unconditional 6.5 s sleep never runs. `test_lockout_wait_bound_counts_read_time` (which catches M9) covers dead code.

**Fix:** Reuse the helper for the LOW-2 bounded retry wait, which makes it live and keeps the bound test meaningful, or delete the loop together with its test.

### INFO-7: Attempt-0 diagnostics leak into attempt 1

Only `pin_lockout` is popped between attempts. Entry diagnostics from attempt 0 (`empty_polls`, `truncated`, and similar) survive into the report of a successful retry.

**Fix:** Pop or namespace the per-attempt keys (for example `attempt0_*`) at the top of each attempt.

## Appendix: mutation JSON

Run with:
`tools\negtest.ps1 -Preset pytest -PresetArg "tests/test_bench_test_cases_lcd.py -k Lcd19" -Mutations mutations.json`.
The anchors use LF (the git copy negtest mutates is LF).

```json
[
 {"name":"M1 drop right-PIN retry continue","file":"tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py",
  "find":"state['right_pin_lockout_retry'] = max(_secs)\n                                    continue","replace":"state['right_pin_lockout_retry'] = max(_secs)\n                                    pass"},
 {"name":"M2 single attempt only","file":"tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py",
  "find":"for _attempt in range(2):","replace":"for _attempt in range(1):"},
 {"name":"M3 keep stale pin_lockout across attempts","file":"tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py",
  "find":"state.pop('pin_lockout', None)","replace":"pass"},
 {"name":"M4 revert LOW-B branch (lock text no longer checked)","file":"tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py",
  "find":"if stabilized and any(\"try again in\" in str(n).lower() for n in names):","replace":"if False:"},
 {"name":"M5 LOW-B branch turns into a false FAIL via keypad_raised","file":"tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py",
  "find":"state[\"wrong_pin_during_lock\"] = True","replace":"keypad_raised = False"},
 {"name":"M6 drop wrong_pin_during_lock flag","file":"tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py",
  "find":"state[\"wrong_pin_during_lock\"] = True","replace":"pass"},
 {"name":"M7 retry margin N+1 -> N+0","file":"tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py",
  "find":"max(_secs) + 1.0","replace":"max(_secs) + 0.0"},
 {"name":"M8 retry sleep N+1 -> N-1","file":"tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py",
  "find":"max(_secs) + 1.0","replace":"max(_secs) - 1.0"},
 {"name":"M9 wait-out loop unbounded (INFO-1)","file":"tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py",
  "find":"while now() - start < max_wait_s:","replace":"while True:"},
 {"name":"M10 fake countdown floor instead of ceil (INFO-2)","file":"tools/PcTools/tests/test_bench_test_cases_lcd.py",
  "find":"max(1, math.ceil(self._until - BackoffUi.now))","replace":"max(1, int(self._until - BackoffUi.now))"}
]
```

Results (base 5033604f3, baseline PASS 83 s):

| Mutation | Verdict | Caught by |
|---|---|---|
| M1 | CAUGHT | `test_first_lock_ten_seconds_waits_and_retries_right_pin` |
| M2 | CAUGHT | same |
| M3 | CAUGHT | same |
| M4 | CAUGHT | `test_wrong_pin_during_running_lock_is_not_refused` |
| M5 | MISSED | (INFO-1) |
| M6 | MISSED | (INFO-1) |
| M7 | MISSED | (INFO-5) |
| M8 | CAUGHT | `test_first_lock_ten_seconds_waits_and_retries_right_pin` |
| M9 | CAUGHT | `test_lockout_wait_bound_counts_read_time` |
| M10 | MISSED | (INFO-5) |
