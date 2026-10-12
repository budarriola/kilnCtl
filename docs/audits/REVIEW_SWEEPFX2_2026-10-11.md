# Review: sweepfx2 (2026-10-11)

Adversarial review of the four origin/dev commits in `4cfa75a42..e951b452f`. They fix
`docs/audits/REVIEW_SWEEPFX_2026-10-10.md` (MED-1, LOW-1, LOW-2, INFO-1..3):

- `c6fe849fa` LCD-19 unverified-PIN flow: the right PIN is tried first, the downgrade is narrowed, lockout is detected, `unverified` is cleared after an unlock, and LCD-25 skips a "bad" PIN.
- `92581598f` worktree_mint `-Remove`: shows git stderr, accepts an unborn HEAD, adds `-ForceUnverifiable`; wt_status prints a prune failure reason; tests.
- `90b45add5` drift check: scoping self-test (`selftest_scoping`) and a fix to the stub comment.
- `e951b452f` drops the unreachable legacy downgrade.

Reviewed at base `cd6efa472` (origin/dev). Firmware cross-checked: `ui_lcd_lock.c`,
`ui_lcd_keypad.c`, `lcd_auth_state.c`, `kiln_ui.c`, `kiln_ui.h`, `login_backoff.c`.

## Verdict

Three of the previous findings are closed: MED-1 (FAIL shapes are no longer downgraded), INFO-2 and INFO-3. LOW-1 (the extra lockout
failure) is fixed for attempt counts, but the new right-first path adds a regression. On
hardware, a pre-set PIN that the run verifies can never reach PASS (MED-A). The new unborn-HEAD
branch in worktree_mint can drop unlanded commits under `-Force` without `-ForceUnverifiable`
(LOW-B). The new wt_status reason text has no test.

No HIGH findings. No path heats, and no path reports PASS where FAIL is right.

## Runs

| Run | Result |
|---|---|
| `tests/test_bench_test_cases_lcd.py` (main-tree venv python, PYTHONPATH = worktree `tools/PcTools/src`) | 228 passed |
| `tools/check_worktree_mint.ps1` | all cases passed |
| `tools/check_wt_status.ps1` | passed, 124 assertions |
| `firmware/KilnFW/App/test/check_cfg_convert_field_mirror_drift.ps1` | OK (105 zone keys, 2 constants) |
| negtest pytest: drop `pin_cfg["bad"] = True` in the right-first refused branch | CAUGHT (`test_unverified_right_pin_refused_costs_one_failure_and_marks_pin_bad`) |
| negtest pytest: disable the `"try again in"` lockout detection | CAUGHT (`test_unverified_right_pin_inside_lockout_reports_lockout`) |
| negtest check: drift scoping reverted to a global exemption (`if k in SCOPED_NON_ZONE_KEYS:`) | CAUGHT (selftest_scoping) |
| negtest check: wt_status reason filter replaced with `Where-Object { $false }` | **MISSED** (see LOW-C) |
| scratch-repo probe of `worktree_mint.ps1 -Remove -Force` on a branch worktree with a deleted branch ref | removed with "unborn HEAD ... nothing unlanded to lose"; the unlanded commit is left dangling (see LOW-B) |

## MED

### MED-A: a verified pre-set PIN can never reach PASS; LCD-19 is INCONCLUSIVE on every bench run after the first

`tools/PcTools/src/kilnctrl/bench_test/cases_lcd.py:3433-3449`, together with
`firmware/KilnFW/App/drivers/ui/ui_lcd_lock.c:318-336` (`ui_lcd_lock_has_role`), `:365-380`
(`gate_keypad_done_cb`) and `:383-390` (`ui_lcd_lock_run_gated`).

Failure scenario:
1. The board already has an admin PIN, so `unverified` is set (every run after the one that seeded it).
2. The right PIN is entered first and GRANTED. `gate_keypad_done_cb` calls `lcd_lock_grant()`, so the session now holds USER or ADMIN.
3. The case dismisses the Confirm Start popup (`_dismiss_lcd19_overlay` only cancels and never relocks) and clicks Start again, expecting a fresh keypad.
4. `ui_lcd_lock_run_gated` sees `has_role(USER)` true and opens Confirm Start directly, with no keypad.
5. `_wait_for_keypad_raise` fails and sets `unverified_reraise_failed`, which sets `wrong_pin = None`. Keypad, wrong-PIN and right-PIN are left unjudged, so the result is INCONCLUSIVE.

The run never reaches the wrong-then-right checks, so the PIN lock is never actually verified on the bench once a PIN exists. Before c6fe849fa,
the wrong-then-right order could PASS (at the cost of two ladder failures).

Why the tests pass: `PinKeypadUiTest` (`tools/PcTools/tests/test_bench_test_cases_lcd.py:2380`)
opens the keypad on every idle Start click and does not model a granted session.
`test_preexisting_admin_pin_verified_by_unlock` (`:2668`) therefore sees the re-raise and asserts
`right_pin_started=True`. That cannot happen on firmware.

Not a safety issue. Nothing confirms the start, and the `finally` block dismisses the overlay.

Fix direction:
- Relock between the unlock and the re-click: toggle the LCD auth policy off and on (as the allow_heat path already does), or use the force-lock route.
- Make the fake keep a granted session, so Start skips the keypad until relocked, then add a test that fails without the relock.

## LOW

### LOW-A: the right-first "refused" branch marks a good PIN bad, and its "may differ" reason covers three other shapes

`cases_lcd.py:3450-3456` (else branch) and `:3807-3816` (reason text).

The branch is reached whenever the post-entry names are stable and are not "Cancel without OK". The comment says "Keypad still open: right PIN refused", but the branch also covers these cases, and each one sets `pin_cfg["bad"] = True` and reports "it may differ from the env var":
- **Lost OK tap.** All digits applied, but the OK tap was not; the dots are still full. The wrong-PIN path checks `_dots_have_cleared` (`:2930`) before trusting a refusal. This path does not.
- **Keypad closed to home.** Neither OK nor Cancel is present.
- **Carried-over lockout.** The keypad shows "try again in" and `lcd_auth_state.c` never checked the PIN. The reason text is correct here (lockout), but `bad=True` is still set.

Effect: `_lcd25_pin` (`:5321`) returns None for the rest of the run, so LCD-25 is skipped for a PIN that may be good, and the diagnosis points the operator at the env var. No extra ladder failures: at most one DENIED per run.

Fix direction: set `bad` only on a refusal that was actually checked (dots cleared, "Wrong PIN" shown, no lockout text), and give the other shapes their own INCONCLUSIVE reason.

### LOW-B: worktree_mint treats any `rev-parse HEAD` failure as an unborn HEAD, bypassing the `-ForceUnverifiable` fail-closed check

`tools/worktree_mint.ps1:174-178`.

`git rev-parse --verify -q HEAD` failing is taken to mean "unborn HEAD; nothing unlanded to
lose", and the `rev-list` check that `-ForceUnverifiable` guards (`:181-195`) is skipped.

Reproduced in a scratch repo:
- **Deleted branch ref.** A branch worktree with one unlanded commit, then the branch ref deleted (`update-ref -d`). `rev-parse` exits 1. `git status` exits 0 and lists every tracked file as `A`. `-Remove` without `-Force` refuses (dirty). `-Remove -Force` prints the unborn note and removes the worktree. The commit object is left dangling, no ref or reflog names it, and gc prunes it later.
- **Zero-byte branch ref.** Same path: `rev-parse` exits 1 and `rev-list` exits 128. The common-dir branch reflog may still name the commit, but the worktree's own HEAD reflog is deleted with it.

Mitigation: `-Force` is required, and the "uncommitted changes" warning lists every file as added. wt_status's automatic prune only removes STALE_CLEAN entries, which need a clean tree, so it does not reach this path. The note "nothing unlanded to lose" is still false, and the fail-closed design from INFO-6 is bypassed by plain `-Force`.

Fix direction: treat HEAD as unborn only when `git symbolic-ref -q HEAD` names a ref and that ref is genuinely absent (`git show-ref --verify` exits 1 and no loose ref file or packed-refs entry exists). Treat a detached HEAD or a present-but-unreadable ref as unverifiable, which requires `-ForceUnverifiable`. Add a w9 case to `check_worktree_mint.ps1` for the deleted-ref shape.

### LOW-C: the wt_status prune failure reason (LOW-2 fix) is untested

`tools/wt_status.ps1:407-408`.

Replacing the reason filter with `Where-Object { $false }` leaves `check_wt_status.ps1` green (negtest MISSED, 124 assertions pass). Nothing asserts that a FAILED prune result carries git's message. The fix may regress silently.

Fix direction: add a prune case whose mint `-Remove` fails (for example, the w6 zero-byte-ref shape), and assert that the FAILED result contains `REFUSED` or `git said`.

## INFO

- **INFO-A: the lockout label really is a tap target.** `kiln_ui.c` (generic CLICKABLE path, about lines 636-748) lets a clickable container borrow its first visible child label's text. The keypad's shared info label shows the lockout text (`ui_lcd_keypad.c:191`, "Wrong PIN -- try again in %us"). At most 30 characters ("...300s"), so it fits `name[32]` (`kiln_ui.h:121`). The `"try again in"` match in `cases_lcd.py:3453` is valid.
- **INFO-B: attempt budget per run.** At most one DENIED against the `login_backoff.c` 5/10/30/60/300 s ladder in either PIN state. A bad pre-set PIN costs 1, and LCD-25 then skips it. A good pre-set PIN costs 0 today, because MED-A stops the run after the unlock. After a MED-A fix it would cost 1 (the deliberate wrong PIN), and the right PIN then lands inside the 5 s lock that wrong PIN opened. See INFO-C.
- **INFO-C: the wrong-then-right path has no wait for the lockout (unchanged by these commits).** `login_backoff.c:3,26`: the first DENIED locks for 5 s. `cases_lcd.py` about lines 3531-3538 enters the right PIN immediately after the wrong-PIN refusal. It relies on the verified digit taps taking longer than 5 s. If they finish sooner, the submit is LOCKED_OUT ("Wrong PIN -- try again in Ns"), the keypad stays open, and `right_pin_started=False` gives FAIL, even though nothing is wrong with the lock. The lockout text is not consulted there. The fresh-seed path already hits this, and a MED-A fix would add the good pre-set PIN path. Fix direction: wait out `locked_until` (or detect the lockout text and report INCONCLUSIVE) before the right-PIN entry.
- **INFO-D: the stub comment (`test_autotune_engine_prestart.c:254-260`) is now accurate.** The `safety_link` executable `#include`s `relay_authority.c` (`test_safety_link_compile.c:523`). The `link_watchdog` executable also links it (`build_host_tests.ps1:2237`), which the comment does not mention. Cosmetic.
- **INFO-E: MED-1 (previous review) is confirmed closed by e951b452f.** The `elif` at `cases_lcd.py:3817-3820` only annotates `pin_unverified` and never replaces a FAIL verdict. `test_unverified_keypad_never_raised_still_fails` and `test_unverified_keypad_accepts_any_pin_still_fails` pin this.
