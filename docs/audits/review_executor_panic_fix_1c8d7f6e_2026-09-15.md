# Adversarial review: deferred heat_enable release (commits 1c8d7f6e, 6f7f462a, ab625bcc)

Read-only review, 2026-09-15. No flash, no board access. The fix under review
moves `heat_enable_release()`'s `REQUEST_ENABLE(false)` send off the caller's
stack. It is a response to the recurring `profile_executor` panic
(`docs/audits/profile_executor_coredump_2026-09-15.md`).

## 0. Git reconstruction: HEAD is intact

* **1c8d7f6e** changed `heat_enable.c/.h`, four test files, and
  `safety_link_poll.c`. The `safety_link_poll.c` diff contained two things:
  the intended one-line `heat_enable_service_pending_release()` call plus its
  include, and about 100 lines of another session's F3 WIP (removal of
  `safety_sync_tc_type()` and related code). One piece of that F3 WIP also
  landed in `test_safety_link_compile.c`: a comment edit about "the removed
  safety_sync_tc_type()". It is harmless and consistent with the F3 commit
  that followed.
* **6f7f462a** set `safety_link_poll.c` to exactly the pre-1c8d7f6e content
  plus the drain call and include. That is correct for the fix, but it
  temporarily reverted the F3 content.
* **ab625bcc** re-applied the F3 content.
  `git diff 1c8d7f6e ab625bcc -- safety_link_poll.c` is empty, so the file
  now matches what 1c8d7f6e had.
* `34779f0a` (F3) sits between them and does not touch `safety_link_poll.c`.
  No commit after ab625bcc touches `heat_enable.[ch]`, `safety_link_poll.c`
  or `safety_link_inbox.c` (checked up to 917ca784).
* **Verdict:** nothing was lost. The only smuggled content is the F3 WIP,
  which its owner was committing anyway. The history is noisy: bisecting
  6f7f462a gives a tree without F3 in `safety_link_poll.c` but with F3's
  header changes. That checkpoint may not have built. The build was not
  verified at 6f7f462a.

**Build at origin/main 917ca784 (clean worktree `C:\wt\rvpanic`, since
removed):**

* KilnFW host tests: 46/46 built and passed.
* `idf.py build`: exit 0, fresh ELF.
* `check_executor_task_stack_budget`: OK. Deepest path 1776 B (was 1936 B),
  now through `adaptive_tune_refine_coupled_locked`. Honest headroom is
  1100 B (26.9%, LOW).
* `check_00_kilnfw_target_build.ps1` does not build HEAD. It mirrors the main
  tree's dirty WIP into `C:\wt\checkbuild`. It also failed this run on an
  uninitialised lvgl submodule. It is not usable as "HEAD builds" evidence.

## Findings, by severity

### HIGH 1: a release and a re-enable can now reorder on the wire, leaving the Pico disabled while the ESP thinks heat is granted, with nothing to retry

Location: `heat_enable.c:240-258` (drain) together with `send_enable()`
`heat_enable.c:87-89`.

**How the old code prevented this.** The old synchronous send ran inside
`profile_executor_pause()`/`halt()` while `s_exec.lock` was held
(`profile_executor_status.c:136..163`, `:31..82`). `profile_executor_resume()`
takes the same lock (`:181`), so a resume's enable=true could never overtake
a pause's enable=false.

**What changed.** The drain now runs on `safety_poll_task`, which does not
hold `s_exec.lock`. It clears `release_pending` under `s_he.lock`, releases
that lock, and only then calls `safety_link_request_enable(false)`. That call
waits on `xact_lock` (`safety_link_inbox.c`, safety_exchange). Other senders
also queue for `xact_lock`, in particular the poll task's own GET_STATUS and
any context exchange.

**Failure scenario:**

1. Pause (bridge task, `uart_bridge_ext_control.c:512`) sets
   `release_pending`.
2. `safety_poll_task` drains: the flag is cleared, then the task blocks for
   `xact_lock`.
3. Resume (`:518`) calls `acquire` -> `send_enable`. Its flush sees nothing
   pending. It sends enable=true, winning `xact_lock` first.
4. The poll task then sends enable=false.

**Result.**

* ESP side: `granted=true`, `pending=false`, so `heat_enable_reconcile()`
  never retries.
* Pico side: heat not permitted, K4 stays open for the rest of the firing.
* The failure is fail-safe (no heat, not uncontrolled heat), but it is
  silent. Guard 1 stall or a ruined firing is the likely visible symptom.

**Why the new test misses it.** `test_reenable_never_races_ahead_of_a_pending_release`
is single-threaded. It only covers the case where the flag is still set, so
it does not exercise this race.

**Second, smaller window.** In `heat_enable_release()` the bookkeeping
(`:185-195`) and `release_pending = true` (`:228-230`) are separate lock
sections. An acquire in between sends true, then the flag is set, then
false goes out while the claim is held.

**Why this matters beyond this module.** It is exactly the reset-one-side
class: "ESP granted" and "Pico enabled" diverge with both sides internally
consistent.

**Fix direction.**

* Keep the release queued as the next frame: set a sequence number or
  "generation" under `s_he.lock`, and have the drain skip the send if an
  enable has happened since.
* Alternatively, make `send_enable()` wait for an in-flight drain, for
  example a `release_in_flight` flag cleared after the send.
* Also reconcile against the Pico's reported enable/relay state instead of
  local `granted` alone.

### HIGH 2: the root cause is not proven, and the measured stack gain is 160 B

**The diagnosis rests on static analysis only.** The coredump was never
symbolised: no matching ELF, and no `/api/coredump` endpoint on the running
image (audit sec. 4). The claim that this chain was "the single deepest
contributor" comes from the static checker, not from a backtrace.

**What the rebuilt ELF shows.**

* Removing the chain lowers the executor's deepest static path by only
  160 B, from 1936 B to 1776 B.
* The old path already had 940 B of estimated honest headroom.
* So a plain self-overflow through this path needed about 2.1 kB of
  unmodelled overhead, not 1.2 kB.

**The other hypothesis is untouched.** Adjacent-stack corruption (the
2026-09-04 `thermo_owner` class) remains open, and nothing in this change
addresses it.

**The commit message overstates this.** It is a reasonable defence-in-depth
change, not a demonstrated fix.

**What would settle it.**

* Flash a build at or after b38b1498 so the next occurrence is symbolisable.
* Keep the memory note `project_profile_executor_panic_at_stop` open.
* Do not close it on the absence of recurrence alone: the four panics were
  spread across about a week.

### MEDIUM 3: the deep release chain moved onto other callers' stacks and longer lock holds, not only onto safety_poll

`send_enable()` now flushes a pending release synchronously, just before its
own enable=true exchange (`heat_enable.c:87`). `send_enable()` is reached
from:

* **`profile_executor_resume()` and `profile_executor_run()`.** Both call
  `heat_enable_acquire` while holding `s_exec.lock`
  (`profile_executor_status.c:191`, `profile_executor_run.c:933`).
  * Two sequential link exchanges now run under `s_exec.lock` (before: one).
  * That is up to two `SAFETY_XACT_LOCK_TIMEOUT_MS` waits.
  * Meanwhile the control tick, `profile_executor_get_status()` (called
    every cycle from `safety_build_and_send_context` on safety_poll itself),
    and the HTTP status handlers all block on `s_exec.lock`.
  * `safety_poll_task` can therefore block on `s_exec.lock` while the
    resuming task waits for `xact_lock`. That is not a deadlock only because
    the context send happens after the exchange releases `xact_lock`.
  * Worth a lock-order note: this is the "lock held across producer calls"
    pattern CLAUDE.md warns about. The enable=true half predates this change.
* **`heat_enable_reconcile()`**, on `profile_exec_wdt` (4096 B,
  `profile_executor_start.c:129`).

Peak depth on those stacks is unchanged, because the sends are sequential,
not nested. The bridge/HTTP stacks already paid for enable=true. But the
commit's claim that the release send runs "on safety_poll_task's 8192 B
stack" is only true when no acquire follows the release within about one
poll iteration.

### MEDIUM 4: latency while the Pico still permits heat

**Bound.** The drain runs once per loop, at the top of the loop.

* Healthy link: one iteration is at most one GET_STATUS exchange (reply
  wait, SAFETY_LINK_REPLY_TIMEOUT_MS), plus a FW_VERSION exchange if the
  version is unknown, plus the context send, plus a sleep of
  `period - spent` (500 ms default).
* That gives about 0.5-0.9 s typical and about 1 s worst case healthy.
* With `no_reply_streak > 0`, the sleep adds up to
  `SAFETY_LINK_BACKOFF_MAX_EXTRA_MS = 4500` ms, so about 5.5 s. In that state
  the send is likely failing anyway.
* When `poll_period_ms == 0`, each iteration is a 200 ms idle drain.

**Stalls.**

* `safety_poll_task` blocked on `s_exec.lock` (finding 3) or on `xact_lock`
  extends the bound by that hold.
* A wedged `safety_poll_task` means the release is never sent. There is no
  timeout or fallback sender. Before this change a wedged poll task did not
  affect releases.

**Recovery mode.** `profile_executor`/`autotune_engine` are not started,
so no release originates. `safety_poll_task` still runs. There is no exposure.

**Effect during the window.** Relays are already de-energised
synchronously, so this is a loss of the second, independent pole (K4) for
about 1 s, not heat.

**Pico tc_type gate.** `link_task_heat_is_safe_for_tc_type_change()`
(`link_task.c:~1605`) will refuse or skip a commissioning tc_type write sent
within about 1 s of a stop. It is benign but can confuse an operator; the
refusal message already names the reason.

### LOW 5: a failed drain is dropped, not retried

The drain clears `release_pending` before the send (`heat_enable.c:244`). A
failed `safety_link_request_enable(false)` is logged, then forgotten.

This is behaviour parity with the old code, which made one attempt. It also
means the header's "A pending release is never dropped" (`heat_enable.h`) is
only true in the sense of "always attempted once". The comment should say
that.

A retry-on-failure would be cheap here, since the task already loops.

### LOW 6: S6a / link-dead

No new frames or cadence changes. The release goes out at most one poll
iteration later and does not replace the GET_STATUS heartbeat, so the Pico's
S6b liveness is unaffected. No guard reads `heat_enable_is_granted()` (grep:
no users outside `heat_enable.c`), so no ESP-side guard misfires.

`SAFETY_FLAG_ENABLED` (dashboard/diagnostics) means "SaftyFW armed", not
this request, and is unaffected.

### LOW 7: the tests pin the new property, but the source scan is narrow

* `heat_enable_read_source()` reads the real `drivers/control/heat_enable.c`
  (anchored path), not a mirror, so the scan does test production code.
* The behavioural half (`release_sends()==0` before drain) links the real
  `heat_enable.c` and would go red on a revert.
* The scan only covers the text between `heat_enable_release(` and
  `\nvoid heat_enable_service_pending_release(void)`. Reordering the two
  functions would make the boundary check fail loudly (good). A helper that
  wraps the send, called from `heat_enable_release()`, would pass the scan
  silently, although the behavioural half would still catch it.
* Neither test models concurrency (finding 1).

## Summary

| # | Sev | Issue |
|---|-----|-------|
| 1 | HIGH | Drain on the poll task lost `s_exec.lock` serialisation; resume can get enable=true on the wire before the drained false. The Pico ends up disabled while `granted=true`, with no reconcile. |
| 2 | HIGH | Root cause unproven (no symbolised coredump); only 160 B of static depth removed, from a path that already had 940 B of estimated headroom. |
| 3 | MED | Flush-in-`send_enable` puts a second exchange under `s_exec.lock` on resume/run callers. |
| 4 | MED | Pico keeps permitting heat for about 1 s (about 5.5 s in backoff); forever if `safety_poll_task` wedges. |
| 5 | LOW | A failed drain is not retried; the header overstates "never dropped". |
| 6 | LOW | No S6a/S6b or guard impact found. |
| 7 | LOW | Tests use production source; no concurrency coverage. |

The git history is clean at HEAD. HEAD builds (target and host tests).
