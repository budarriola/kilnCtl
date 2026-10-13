# Review: sweepfx (origin/dev 66400a90e), 2026-10-10

Adversarial review of `66400a90e` ("Sweep review LOW-3, INFO-1/3/5/6"), which closes
LOW-3 and INFO-1/3/5/6 of `docs/audits/REVIEW_SWEEP_2026-10-10.md`. Reviewed in a
clean worktree at `66400a90e`. No code changed by this review.

## Tests run

- `tests/test_bench_test_cases_lcd.py` + `tests/test_bench_test_cases_web_rw.py`:
  311 passed, 10 subtests passed.
- `check_cfg_convert_field_mirror_drift.ps1`: OK (105 zone keys, 2 version constants).
- `tools/check_worktree_mint.ps1`: all cases PASS.
- Scratch test (not committed) driving `_case_lcd19` with the existing fakes
  (`PinKeypadUiTest`, `FakeSrvFull`, `FakeSec04Client`): 2 failed / 2 passed, the
  two failures being the MED-1 masking below.
- `tools/negtest.ps1` results: see the "Negative tests" section.

## Summary

| ID | Grade | Area | One line |
|----|-------|------|----------|
| MED-1 | MED | LOW-3 (LCD-19) | Unverified-PIN override turns every FAIL into INCONCLUSIVE, including "Start not gated" and "any PIN accepted"; every bench run after the first takes this branch. |
| LOW-1 | LOW | LOW-3 (LCD-19/25) | A pre-set PIN that differs from the env PIN costs two failed keypad attempts per run (three with LCD-25); `failure_count` persists, so repeated runs walk the 5/10/30/60/300 s lockout ladder. |
| LOW-2 | LOW | INFO-6 (worktree_mint) | Fail-closed `-Remove` has no escape hatch and hides git's stderr; one corrupt ref anywhere, or an unborn HEAD, blocks every removal including `wt_status -Prune`. |
| INFO-1 | INFO | LOW-3 | "Verified" only proves the env PIN holds USER role (Start is USER-gated), not that it is the admin PIN; `unverified` is never cleared after a successful unlock. |
| INFO-2 | INFO | INFO-3 (drift check) | Per-function scoping works on today's file but has no fixture or self-test; a regression back to a global exemption passes on the real tree. |
| INFO-3 | INFO | INFO-5 (comment) | The new drift note names a non-existent "relay_authority.c" executable; the real `relay_authority_on_blocked()` is exercised in the `safety_link` executable, and its NULL-link and uninitialised-link semantics are not pinned there. |
| INFO-4 | INFO | INFO-1 (comment) | Accurate. |

## MED-1: unverified-PIN override masks real LCD-19 failures

`cases_lcd.py` (after `J.judge_lcd_pin_lock(...)`, ~line 3764):

```python
if (ctx.get("_lcd_pin") or {}).get("unverified"):
    result.observed["pin_unverified"] = right_pin_started is not True
    if right_pin_started is not True:
        result = CaseResult(Verdict.INCONCLUSIVE, ...)
```

The condition is "right PIN did not start", not "the only failure is that the right
PIN was refused". `right_pin_started` is only attempted when `wrong_pin_refused` is
truthy, so it stays `None` whenever an earlier step failed. Every such FAIL is then
rewritten to INCONCLUSIVE:

- `keypad_raised is False`: Start runs with no PIN keypad at all (PIN gate bypassed).
- `wrong_pin_refused is False`: the keypad accepts any PIN.
- `stop_gated is False` with `right_pin_started is True` still FAILs; that is the only
  FAIL shape that survives the override (`stop_gated` is read in the `allow_heat`
  block, after the right-PIN start).

Scenario: firmware regresses so the keypad accepts any PIN. On the bench the admin PIN
is already set from the first run (`seed_lcd_pin` now returns the env PIN with
`unverified=True` instead of raising), so every later run takes this branch. LCD-19
reports INCONCLUSIVE "may differ from the env var" and the security regression never
FAILs. Reproduced with the test file's own fakes: an `AnyPinUi` (accepts any PIN) and
`PinKeypadUiTest(policy_fn=lambda: False)` (keypad never raised) give FAIL with
`admin_pin_set=False` and INCONCLUSIVE with `admin_pin_set=True`.

The two new tests (`test_preexisting_admin_pin_verified_by_unlock`,
`test_preexisting_admin_pin_differs_is_inconclusive`) cover only the PASS and the
"PIN differs" shapes, so nothing pins the FAILs on the unverified path.

Fix: downgrade only the one shape the override exists for:

```python
if (keypad_raised is True and wrong_pin_refused is True
        and right_pin_started is not True and result.verdict != Verdict.PASS):
    result = CaseResult(Verdict.INCONCLUSIVE, ...)
```

Add unverified-path tests asserting FAIL for "keypad never raised" and "any PIN
accepted".

## LOW-1: lockout cost of an unverified PIN that does not match

`lcd_auth_state.c`'s backoff ladder (5 s, 10 s, 30 s, 60 s, 300 s) is driven by
`failure_count` in `ui_lcd_keypad.c`'s `s_ks`. Only a GRANTED submit resets it, and
it survives for the boot. A LOCKED_OUT submit never checks the PIN.

When the board PIN differs from `KILNCTL_LCD_PIN`, LCD-19 now enters the derived wrong
PIN (failure 1) and then the env PIN (failure 2). LCD-25's `_lcd25_pin` then takes
`ctx["_lcd_pin"]["right_pin"]`, the same known-bad PIN, and adds failure 3. Before this
commit LCD-19 raised in `seed_lcd_pin` and made no attempt at all, so this is new cost.
Repeated runs on one boot reach the 30/60/300 s rungs. During a lockout window the LCD's
Start/Stop/Pause/Menu are unusable for the operator (Stop is PIN-gated per the
2026-09-28 owner decision; the E-stop and the web UI still work), and later PIN cases
go INCONCLUSIVE for reasons unrelated to them.

A second, smaller effect: if the right-PIN submit lands inside the 5 s window opened by
the wrong-PIN failure, it is LOCKED_OUT and never checked, and the case reports "may
differ" for a PIN that is in fact correct. Bench timing has shown the unlock
succeeding, so this is latent.

Fix: when `unverified`, try the right PIN first. If it is refused, stop at one failure
with INCONCLUSIVE and mark `ctx["_lcd_pin"]` bad (or delete it) so LCD-25 does not
reuse it. If it is accepted, cancel Confirm Start, then run wrong-then-right as now.

## LOW-2: fail-closed `-Remove` has no escape hatch

`worktree_mint.ps1 -Remove` now exits 1 when
`git rev-list HEAD --not --remotes --branches` fails, even with `-Force`, and
`2>$null` discards git's message. Probed on git 2.52 in a scratch repo:

| State | rev-list rc |
|-------|-------------|
| branch with missing upstream | 0 (fine) |
| dangling `origin/HEAD` symref | 0 (fine) |
| zero-byte loose ref anywhere under `refs/` | 128 |
| remote ref pointing to a missing object | 128 |
| unborn HEAD (orphan worktree, no commits) | 128 |

So the answer to "does it refuse legitimately removable worktrees" is: not for a
missing upstream, but yes in the other three. Refs are shared by every worktree, so one
truncated ref (plausible: the main repo sits in OneDrive, and a crash can leave a
zero-byte ref file) makes every `-Remove` fail. Callers:
`wt_status.ps1 -Prune` passes `-Remove -Force` and counts each as FAILED with no
reason; `land.ps1 -RemoveWorktree` falls back to `git worktree remove --force` and
succeeds anyway, which bypasses the very check this change hardened. An orphan worktree
with nothing on it can never be removed by the tool.

Fix: print git's stderr in the REFUSED line. Treat an unborn HEAD as zero unlanded
commits. Let an explicit second flag (`-ForceUnverified`, or `-Force` given twice via a
separate switch) proceed with a loud warning, and have `wt_status` report the reason.

## INFO-1: "verified" proves USER role only

The unlock uses Start, which is gated at `LCD_PIN_ROLE_USER`. A USER PIN equal to the
env PIN verifies even when the admin PIN differs, yet the reason text and the
`admin_pin_set_before`/`pin_unverified` fields speak of the admin PIN. After a
successful unlock `ctx["_lcd_pin"]["unverified"]` stays True, so a later case cannot
tell a proven PIN from an unproven one. Fix: verify through an ADMIN-gated action (Edit
firing) or rename the claim to "a configured PIN with at least USER role"; set
`unverified=False` after a successful unlock.

## INFO-2: drift-check scoping has no self-test

`FUNC_DEF_RE` plus `_enclosing_function` (last definition whose `{` precedes the
match) finds 10 definitions in `backup_export.c`, matching the 10 column-0 braces; all
58 zone printf calls attribute to `backup_export_get_handler`, `"c"` to
`backup_export_relay_cycles`, and `"unit"`/`"type"` to `backup_export_prefs`. One call
attributes to `backup_stream_raw` (the `backup_stream_printf` definition itself), which
is harmless. Shapes the regex misses (name on an indented line, attribute-first
prefix) attribute the following calls to the previous function, which can only widen
the reported key set or exempt a key in a neighbouring scoped function, a rare
mis-attribution rather than a silent hole in today's file.

The gap is that nothing tests the scoping: reverting it to a global exemption (M1
below) still passes on the real tree, because today no zone printf emits `c`, `unit`
or `type`. Fix: add a fixture C snippet with a zone-scope `"type"` key and assert the
check reports it, plus one asserting `"type"` inside `backup_export_prefs` is exempt.

## INFO-3: INFO-5 comment names the wrong executable

The new note in `test_autotune_engine_prestart.c` says the real semantics are "pinned
by the relay_authority host test built from relay_authority.c itself
(build_host_tests.ps1, "relay_authority.c" executable)". No such executable exists. The
only build line that compiles `owners/relay_authority.c` as a source is
`link_watchdog`, and `test_link_watchdog.c` states it never calls
`relay_authority_on_blocked()`. The real function is exercised by the `safety_link`
executable, where `test_safety_link_compile.c` `#include`s `relay_authority.c`
(~line 523) and tests it at ~lines 2264-2345, but only with a live link and fault
sources; the NULL-link (APP source) and uninitialised-link (reads 0) branches that the
stub mirrors are not tested there. Fix: point the comment at the `safety_link`
executable and add two checks there for the NULL and uninitialised-link cases.

## INFO-4: INFO-1 comment is accurate

The narrowed `nvs_save()` comment in `zones_config_store.c` is correct:
`relay_names_save()`/`relay_names_save_locked()` (lines ~1437/1455, called from
`zones_http_post.c:866` and `zones_config_accessors.c:604/648`) and
`zone_normals_save_locked()` write their own keys without `nvs_save()`, and
`kiln_cfg_swap.c` references neither relay names nor zone normals, so the "harmless
while the journal never restores them" clause holds.

## Negative tests

`tools/negtest.ps1` at `66400a90e`; baseline passed and the real tree stayed unchanged
in both runs.

`-Preset check -PresetArg firmware\KilnFW\App\test\check_cfg_convert_field_mirror_drift.ps1`:

| Mutation | Verdict | Meaning |
|----------|---------|---------|
| M1: `if func in SCOPED_NON_ZONE_KEYS.get(k, ()):` to `if k in SCOPED_NON_ZONE_KEYS:` (back to a global exemption) | MISSED | INFO-2: the scoping itself is untested. |
| M2: add `\"type\":0` to the zone printf in `backup_export.c` | CAUGHT | The scoped check does catch a zone-scope `type`. |
| M3: M1 + M2 together | MISSED | With the scoping reverted, the same zone-scope `type` passes: the fix is real, but only the real tree protects it. |

`-Preset pytest -PresetArg tests/test_bench_test_cases_lcd.py`:

| Mutation | Verdict |
|----------|---------|
| L1: `if right_pin_started is not True:` to `if False:` (no downgrade) | CAUGHT |

L1 shows the new "differs" test pins the downgrade. MED-1 is the opposite gap: nothing
pins that the downgrade stays narrow.
