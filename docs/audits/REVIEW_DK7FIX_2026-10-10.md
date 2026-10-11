# Review: K7 review fixes (dk7fix), 2026-10-10

Scope: `235e4e3c4` (K7 review fixes F1/F3/F4/F5 plus N1/N4/N8 tests) and the doc
commits `8a9ffd1bd` and `ce772bf30`, all on origin/dev. The source review is
`docs/audits/REVIEW_DANGER_K7_FIX_2026-10-10.md`. Reviewed at origin/dev
`9c9600384`, which contains `235e4e3c4`. Host-only review; no board access.

## Verdict

The production fixes are correct, and none of them makes anything less safe.
Two of the new tests do not test what they claim to:

- The F4 ordering assertion is vacuous. The locked half of the watchdog never
  runs in that test.
- The F1 test cannot see a rollback that runs without the lock.

One doc SHA is also wrong. There are no HIGH findings.

## Findings

### MED-1: the F4 test never reaches the locked half of the watchdog pass

`firmware/KilnFW/App/test/test_profile_executor_prestart.c:2566-2581`
(`test_watchdog_pass_wires_relay_unknown_and_orders_release`, second pass).

In the host semaphore stub, `g_test_stub_semaphore_take_default` defaults to
`pdFALSE` (`firmware/KilnFW/App/test/stubs/freertos/semphr.h:86`). This test
never sets it to `pdTRUE`, so the following happens:

1. `guard9_take_lock_bounded()` fails (`profile_executor.c:2358`).
2. The pass `continue`s.
3. `guard9_merge_pending()`, the stale branch, `guard9_assert_stale_tick_fault()`
   and `relay_unknown_release_locked()` never run.

The check `deasserts == 0` therefore holds for any order of the release.

Probe mutations prove this:

- After pass 2, `s_guard9_bookkeeping_pending` is still true.
- `global_fault_source` lacks APP.
- `s_relay_unknown_fault_asserted` is still true.
- `take_default != pdTRUE` on entry.

Every F4 ordering mutation (R5, R6, R7) was MISSED, including R6, which moves
the release between the merge and the assert. That is exactly the regression
the test names.

The N1 half of the test (first pass) is real, because the prelock checks are
lock-free.

Suggested fix (verified with negtest, see the probe table):

```c
    g_fault_source_deassert_count = 0;
    BaseType_t saved_take_f4 = g_test_stub_semaphore_take_default;
    g_test_stub_semaphore_take_default = pdTRUE;
    g_task_delay_budget = 1;
    ...
    deasserts = g_fault_source_deassert_count;
    g_test_stub_semaphore_take_default = saved_take_f4;
    TEST_CHECK(!s_guard9_bookkeeping_pending && (s_exec.global_fault_source & SAFETY_FAULT_SRC_APP),
               "F4 sanity: locked half ran");
    TEST_CHECK(!s_relay_unknown_fault_asserted, "F4 sanity: relay-unknown hold released");
```

With this change the test passes on unmutated code and catches R6. R5 (release
before the merge) and R7 (drop `!s_guard9_bookkeeping_pending`) each still pass
alone. That is correct, not a gap: the two guards back each other up.

- Release-before-merge is safe only because of the pending term.
- The pending term is redundant only because merge runs first.

A mutation that does both at once would be caught.

### LOW-1: the F1 test cannot see a rollback that runs unlocked (fixer's MISSED mutation is a real gap)

`firmware/KilnFW/App/test/test_danger_mode.c:338-342`;
code at `firmware/KilnFW/App/drivers/safety/danger_mode.c:109-121`.

The fixer called the "single take, rollback unlocked" mutation equivalent under
the single-threaded stub. It is not equivalent on target.

`s_dm.lock` is a FreeRTOS mutex. A `xSemaphoreGive` from a task that does not
hold it reaches `xTaskPriorityDisinherit()` and its
`configASSERT(pxTCB == pxCurrentTCB)`, which panics. Without that assert, the
rollback would race the holder's writes to `window_open`/`deadline_ms`.

The stub hides this because `xSemaphoreGive` clamps `g_test_stub_lock_depth`
at 0, so a give with no matching take is invisible.

Suggested fix (verified: passes on unmutated code, catches R2): start from a
sentinel depth.

```c
    g_test_stub_semaphore_fail_nth = 2;
    g_test_stub_lock_depth = 1; /* sentinel: an unheld give would drop this to 0 */
    CHECK(!danger_mode_request_start());
    g_test_stub_semaphore_fail_nth = 0;
    CHECK(g_test_stub_lock_depth == 1); /* every give matched a successful take */
    g_test_stub_lock_depth = 0;
```

A more general fix is a stub flag set on any give when depth is 0, asserted
clear by the test harness.

The production code is correct.

### LOW-2: a SAFETY_FAULT_SRC_APP foreign holder can be wrongly latched for the whole boot, or missed

`firmware/KilnFW/App/drivers/control/profile_executor.c:2198-2214`
(`pe_app_owner_foreign`, `pe_app_note_foreign_before_assert`), plus the callers
at `:2148`, `:2226` and inside `guard9_assert_stale_tick_fault()`.

This is an F5 completeness issue. There are three cases.

(a) False foreign, sticky until reboot. `pe_app_note_foreign_before_assert()`
runs lock-free from `guard9_prelock_check()` and
`relay_unknown_prelock_check()`. It reads the link (`get_fault_sources`) and
`s_exec.global_fault_source` at different moments. Suppose a halt runs
`clear_this_runs_faults()` (under `s_exec.lock`) in between, so the link still
shows APP but global is already 0, and no other owner flag is set. The executor
then records a foreign holder that does not exist. `pe_app_owner_foreign` is
never cleared, so APP stays asserted and relay ON is refused until reboot. This
fails safe, but it is an outage until reboot, from a narrow race.

(b) Lost relay-unknown hold. `relay_unknown_prelock_check()` sets
`s_relay_unknown_fault_asserted` and asserts APP without the lock. A concurrent
locked halt can read the flag as false and then deassert APP after the watchdog
asserted it. The edge-triggered assert (`:2224`) never re-asserts, so the
Pico-side fault is lost while relay state is still unknown.
`kiln_io_set_relay_mask` still refuses ON while unknown
(`kiln_io.c` relay-unknown ON refusal), so the local block holds. The link-level
block does not.

(c) Missed foreign. Foreign is noted only at the moment the executor asserts.
Suppose a boot safe-state latch (`main_bridges_bringup.c:240`
`main_kiln_enter_safe_state`, `main_boot_early.c:602` boot fault sources,
`main_network_http.c:968/998`) asserts APP while the executor already holds it.
That holder is never recorded, and a later executor release clears the latch.
Those latches are rare and mostly boot-time, so this is narrow.

Suggested fix, in two parts:

1. Make the relay-unknown hold a level: re-assert APP every watchdog pass while
   `kiln_io_relay_state_unknown()`. This closes (b), and the assert is
   idempotent.
2. Replace the inferred foreign flag with explicit registration. Add a setter
   (for example `profile_executor_note_external_app_hold()`) called by
   `main_kiln_enter_safe_state()` and the boot fault-source asserts. This
   closes (a) and (c).

The minimum fix for (a) is to call `pe_app_note_foreign_before_assert()` only
with `s_exec.lock` held.

### LOW-3: the CMD_ALL_RELAYS_OFF off-tracker gate on ESP_OK is untested

`firmware/KilnFW/App/drivers/owners/kiln_io_owner.c:499-501`.

The owner notes "all OFF" in the off-tracker only when
`kiln_io_all_relays_off()` returns `ESP_OK`. That is correct after F3, because
the unserialised path now returns `KILN_IO_ERR_UNSERIALISED_OFF`. However,
mutation R15 (`r.err == ESP_OK || r.err == KILN_IO_ERR_UNSERIALISED_OFF`) was
MISSED: no owner test drives the busy-lock path through the command.

Suggested fix: in `test_kiln_io_owner.c`, make the stub's all-off return
`KILN_IO_ERR_UNSERIALISED_OFF` and check that the tracker is not noted.

### LOW-4: the fix-status section cites a pre-rebase SHA

`docs/audits/REVIEW_DANGER_K7_FIX_2026-10-10.md` "Fix status" (from
`ce772bf30`) says the fixes are in `b0035aa13`. That commit exists locally but
is not on origin/dev; the landed commit is `235e4e3c4`.

Suggested fix: change the SHA to `235e4e3c4`.

### NIT

- `kiln_io.c:610` / `kiln_io_track()` `:84` set `last_i2c_failed = true` after
  a successful unserialised write. Nothing reads that flag today. If it ever
  feeds a bus-health view, it will misreport.
- Callers log the 0x10C result with `esp_err_to_name()`, so it prints as
  `ESP_ERR_NOT_FINISHED`. For example, `main.c:173` prints "could not drop the
  relays: ESP_ERR_NOT_FINISHED". A distinct log string at the unserialised site
  already exists ("unlocked OFF write"), so this is cosmetic.
- `kiln_io.h:204` hard-codes 0x10C. A target-build
  `_Static_assert(KILN_IO_ERR_UNSERIALISED_OFF == ESP_ERR_NOT_FINISHED, ...)`
  would pin it. The value does equal `ESP_ERR_NOT_FINISHED` in IDF v6.0.2
  (`esp_err.h:36`).
- `danger_mode.c:109-111` retries every 50 ms with `ESP_LOGE` and no bound.
  This is fine given how short the holders are, but a log rate-limit would
  avoid spam if a holder ever wedged.
- `profile_executor_relay_io.c:1380` (`escalate_guard_trip`) assigns
  `global_fault_source = source` rather than OR-ing it in. This is
  pre-existing, not part of this change, and it interacts with F5's
  "APP in global" ownership test. Worth a look in the next executor pass.

## Checked and correct

- **F1** (`danger_mode.c:90,109-121`): the rollback retries its take until it
  succeeds, then restores `heat_requested` and `deadline_ms` (already-open) or
  closes the window. The window can no longer stay open under a firing.
  Mutations R1 and R3 were CAUGHT.
- **F3** (`kiln_io.c:596-613`): the unserialised all-off never returns
  `ESP_OK`, never touches `relay_shadow`, and leaves `relay_state_unknown`
  raised. R4 was CAUGHT.
- **Callers of `kiln_io_all_relays_off()` and the owner command**: every caller
  treats non-OK as not verified.
  - `kiln_io_owner.c:499` notes the tracker on OK only.
  - `main.c:171` logs.
  - `uart_bridge_io.c:392` returns the error to the PC.
  - `zones_current_sweep_engine.c:873`, `danger_mode.c:346/394` and
    `main_control_bringup.c:46` log.
- **The five `profile_executor.c` sites that ignore the return (`:2152`,
  `:2222`, `:2388`, `:2426`, `:2447`)** are safe. On a 0x10C return,
  `relay_state_unknown` stays raised. `relay_unknown_prelock_check()` runs
  every watchdog pass, idle or not, and runs in the same pass right after
  `guard9_prelock_check()`. It retries the all-off and holds APP until a
  locked, verified all-off clears the flag, and `kiln_io_set_relay_mask`
  refuses ON in the meantime.
  `tools/check_safety_call_results_checked.ps1` allowlists exactly this
  snippet, with that rationale.
- **Lock ordering**: no new nesting.
  - The danger_mode rollback takes only `s_dm.lock`.
  - kiln_io's fail-safe takes only the kiln_io lock, with a 2000 ms bound.
  - The executor's foreign/relay-unknown notes run lock-free or under
    `s_exec.lock`, and call `safety_link_*`, which takes its own leaf lock.
    The watchdog already did this before.
- **Rebase onto the guard-9 merge**: the merge-before-release order at
  `profile_executor.c:2374-2393` came from the earlier commit `3c57e1d54`
  (review-2 LOW-2). `235e4e3c4` rebased cleanly onto it. Its own F4 delta is
  the `!s_guard9_bookkeeping_pending` term (`:2240`). Under the current order
  that term is redundant, but it is real backup if the release ever moves
  ahead of the merge (see MED-1).
- **N1, N4, N8 tests**: real.
  - R12 (prelock not wired) was CAUGHT.
  - R13 (MED-3 exception dropped) was CAUGHT.
  - R14 (SX_RESET skips the tracker note) was CAUGHT in two executables.
- **F5 owner tracking**: correct for the sequential cases. R8, R9, R10 and R11
  were all CAUGHT.

## Tests run

- `firmware\KilnFW\App\test\build_host_tests.ps1 -Only 'kiln_io_sx_fake|profile_executor_prestart|danger_mode|kiln_io_owner'`
  at `9c9600384`: 6/6 executables built and passed
  (`profile_executor_prestart`, `kiln_io_owner`, `kiln_io_sx_fake`,
  `kiln_io_owner_sx_dispatch`, `safety_link`, `danger_mode`).

Negtest (`tools\negtest.ps1`, same command with `-OutDir {OUT}`,
`-ExpectPattern "(?m)^\s+FAIL |FAIL .*\.c:\d+|RUN FAILURES|BUILD FAILURES"`),
baseline passed, `real_tree_unchanged: true`:

| Mutation | What it reverts | Result |
|---|---|---|
| R1 | F1: single guarded take (rollback skipped on timeout) | CAUGHT |
| R2 | F1: single take, rollback runs unlocked | MISSED (LOW-1) |
| R3 | F1: no deadline restore | CAUGHT |
| R4 | F3: unlocked all-off returns OK and zeroes shadow | CAUGHT |
| R5 | F4: release before the guard-9 merge | MISSED (MED-1) |
| R6 | F4: release after merge, before the stale assert | MISSED (MED-1) |
| R7 | F4: release ignores the pending flag | MISSED (MED-1; redundant under current order) |
| R8 | F5: halt ignores the relay-unknown owner | CAUGHT |
| R9 | F5: halt ignores the foreign owner | CAUGHT |
| R10 | F5: release ignores the foreign owner | CAUGHT |
| R11 | F5: foreign never noted | CAUGHT |
| R12 | N1: prelock check not wired into the watchdog | CAUGHT |
| R13 | N4: MED-3 INVALID_RESPONSE exception dropped | CAUGHT |
| R14 | N8: SX_RESET skips the off-tracker note | CAUGHT |
| R15 | owner notes OFF on 0x10C | MISSED (LOW-3) |

Probe runs (test-side edits only, never landed):

| Probe | Result | Meaning |
|---|---|---|
| F4 test, after pass 2: pending still latched | true | merge never ran |
| F4 test, after pass 2: global has APP | false | stale branch never ran |
| F4 test, after pass 2: relay-unknown hold released | false | release never ran |
| F4 test, entry: `take_default == pdTRUE` | false | every lock take fails |
| proposed F4 test fix alone | passes | |
| proposed F4 test fix + R6 | CAUGHT | |
| proposed F4 test fix + R5 / + R7 | pass | guards back each other up (MED-1) |
| proposed F1 lock-depth sentinel alone | passes | |
| proposed F1 lock-depth sentinel + R2 | CAUGHT | |

## Fix status

MED-1, LOW-1, LOW-3 and the 0x10C static assert (NIT) are fixed in e88985d37 and d62cb2ee1 (R2, R6, R15 negtest CAUGHT; R5 and R7 stay MISSED by design). LOW-4 was already fixed on dev (the K7 doc cites 235e4e3c4). LOW-2 is FIXED in 75eee94b2: boot latches register via profile_executor_note_external_app_hold() (main.c, main_control_bringup.c), no inference left; the relay-unknown assert is level-triggered every watchdog pass; host tests (a)(b)(c) negtest CAUGHT.
