# Review: safety-link fix batch (F1-F5), 2026-10-10

Scope: the 9 commits `fb931e6f8..a50e575f0` on origin/dev, which fix F1-F5 of
`docs/audits/SAFETY_LINK_REVIEW_2026-10-09.md` (F6 deferred). Review only, no
code changed. Line numbers are at dev tip `4ff79bc8c`.

| Commit | Change |
|---|---|
| `5c7c6a451` | F1: heat_enable reconciles the held grant against the Pico-reported K4 |
| `e327e0e20` | F2: Pico drops an inherited grant on a new ESP session without HEAT_OWNER |
| `f58560ed7`, `2fef31929` | F3: persistent commit also requires DIAG VOLATILE_DIRTY clear |
| `4ab08d964` | F4: trip_seq wraps 255 -> 1 |
| `bc52e1517` | F5: HAL_DMB release/acquire around the trip-event publication |
| `5abfe307d` | stack ceilings |
| `cfbee2799`, `a50e575f0` | docs |

No HIGH findings. The physical failure direction of every change is safe:
nothing here can close K4 the Pico would not close on its own. The MED
findings concern a fail-safe property F1 removes (MED-1), a fault that stays
invisible to the run (MED-2), an E-stop verification invariant (MED-3) and
stack depth (MED-4).

## MED

### MED-1 (F1): a Pico reboot right after a trip now re-energises heat with no operator action

`heat_enable.c:678-707` (`heat_enable_note_pico_state`), fed from
`profile_executor.c:2417`.

- The executor faults the run, and so releases the claim, only on a **fresh
  DIAG reading TRIPPED** (`profile_executor.c:2225-2226`) or on 30 s of link
  silence. DIAG arrives every 2 s.
- The Pico does not latch a trip across its own reboot. `boot_reason` is only
  reported in DIAG.
- Scenario: the Pico trips (K4 cut locally), then resets (watchdog, assert,
  stack overflow in the trip path) before the ESP has seen a fresh TRIPPED
  DIAG. The ESP never faults the run and the claim stays held. The Pico comes
  back INIT -> GRACE -> ARMED with no trip. About 3 s after ARMED, F1 sees
  "granted, ARMED, K4 open", re-requests, and K4 closes. The trip has been
  turned into a brief pause with no clear and no operator.
- The same mechanism re-energises a boot-looping Pico on every cycle: each
  cycle that reaches ARMED and closes K4 resets the budget (`he_k4_reset_locked()`
  at :690), so the 4-resend bound never fires.
- Before F1, heat stayed off after any Pico reboot (the silent loss F1 fixed).
  That was wrong for a benign reboot, but fail-safe after a trip.
- Suggested direction: across a Pico boot_id change, do not re-request when DIAG
  `boot_reason` carries the watchdog or fatal bits (watchdog, stack overflow,
  malloc failed, assert). Fault the run instead, or require an operator action.
  **Owner decision needed:** is "a benign Pico reboot restores heat mid-firing"
  intended for every reboot cause? (`bench_pico_reboot_midfiring.py` E4 only
  requires the firing to stay RUNNING.)

### MED-2 (F1): a grant that cannot be obtained stays invisible to the run

`heat_enable.c:650-674` (`is_granted`, `retry_pending`, `grant_unconfirmed`).

- No consumer reads the new state. The executor never calls `is_granted`,
  `retry_pending` or `grant_unconfirmed`. The only non-test caller of
  `is_granted` is the auth-reset gesture (`ui_page_home_actions.c:603`).
  Nothing reaches `/api/status`, the dashboard or the LCD.
- `k4_unconfirmed` is terminal. After 4 resends (about 45 s) nothing is sent
  again until K4 closes while the Pico is fresh and ARMED, or the claim is
  released.
- Scenario: the Pico refuses ON (for example during a tc_type apply, an update
  transfer, `safety_tc_installed == 0` or uncommissioned) for longer than the
  budget. The firing then stays RUNNING with K4 open: the schedule advances,
  setpoints ramp and the kiln cools. The only evidence is one `ESP_LOGE`. This
  is physically safe but leaves the operator unaware that the firing has
  failed. Even when the refusal reason clears, heat does not come back, because
  no more sends are made.
- Suggested direction: either pause or fault the run on `grant_unconfirmed`
  with a named reason, or keep retrying at the 48 s cadence instead of giving
  up. Then surface the state in `/api/profile_exec` (or `/api/status`).

### MED-3 (F3): a landed polarity commit can leave the E-stop verification standing

`safety_cfg_write.c:553-556` (early return) and `:561-593` (the
`estop_verification_clear()` block).

- `confirm_commit_landed()` now returns false when the DIAG verdict is not
  PERSISTED, either on a 3 s timeout (`:316-323`) or with UNKNOWN (no DIAG
  ever received). That return happens **before** the estop invalidation loop.
- Scenario: an operator commits a new `estop_active_level` (persistent,
  disarmed). The Pico applies it: its RAM and flash change, and the read-back
  matches. But the next DIAG is lost or late beyond 3 s, or the board has
  never received a DIAG. The POST fails as "UNCONFIRMED", and
  `estop_verification_clear()` never runs. `/api/readiness` keeps reporting
  "E-stop confirmed by operator" for a polarity that is live and that nobody
  verified.
- Before F3 the clear always ran once the read-back matched.
- Suggested direction: run the estop invalidation as soon as the read-back
  matches (before the persisted check), because the value is live in Pico RAM
  either way.

### MED-4 (F1): the deepest known send chain now runs routinely on profile_exec_wdt's stack

`heat_enable.c:746-758` (`reconcile` calls `send_enable`, then
`safety_link_request_enable`, then `safety_exchange`, then the UART send
chain).

- Before F1, `send_enable("retry")` ran on the watchdog task only after a
  failed send, which is rare. F1 now forces it on every K4 mismatch: every
  benign Pico reboot mid-firing, and every lost frame.
- `heat_enable.c:722-742` and `:525-540` both record that this chain was the
  deepest contributor to four stack-smash panics on the 4096 B executor task.
  They also record that profile_exec_wdt (also 4096 B) is INDETERMINATE in the
  static walk.
- The re-pinned `profile_exec_wdt` ceiling (2704, now 2720,
  `check_all_task_stack_budgets.py:1056-1071`) is a baseline tripwire. Its own
  comment says the deep path "was never entered on the run that produced it".
  The "26.7 % free" in the F1 note therefore does not measure the path F1 added.
- Suggested direction: either measure a live high-water mark on the bench after
  forcing a K4 re-request mid-firing, or move the re-send to safety_poll_task
  (8192 B), which already drains `release_pending`. Have note_pico_state set
  `pending` only, and let the poll task call `send_enable`.

## LOW

### LOW-1 (F1): the resend budget is not reset per episode (reset-one-side)

`heat_enable.c:85` says "re-requests spent on this episode". However,
`k4_resends` and `k4_unconfirmed` are reset only by `he_k4_reset_locked()`,
which runs on init, on release, on `held_mask == 0`, and when K4 is closed
while fresh and ARMED (:682, :690). Two transitions do not reset them:

- The branch for a Pico that leaves ARMED (:684-687) clears only
  `k4_open_since_ms`.
- A Pico boot_id change does not touch heat_enable at all.

Scenario: two lost grants in one firing that never reach K4 closed share one
4-resend budget. The second episode can be declared unconfirmed after a single
resend. This is fail-safe (fewer retries), but the counter outlives the episode
its own comment names. This is the "who else holds a copy" class in CLAUDE.md:
the Pico reboot resets the Pico's grant, and the ESP-side episode state should
follow it.

### LOW-2 (F1): WARN is never timed, and K4 closing in WARN leaves the flag set

The branch condition at `heat_enable.c:684` treats WARN (state 3) as "do not
time". Two consequences follow:

- Scenario A: the grant is lost while the Pico sits in WARN for a long time
  (for example during a WARN-only guard). The grant is not re-requested until
  the Pico returns to ARMED. relay_owner has no WARN state of its own
  (`relay_owner.c:91` energises whenever its state is ARMED), so during DIAG
  WARN the Pico would have honoured a re-request.
- Scenario B: `k4_unconfirmed` is set, then the refusal clears and K4 closes
  while the Pico is in WARN. The reset at :689-690 needs ARMED, so `is_granted`
  reads false while K4 is energised. This only affects the auth-reset gesture,
  which also gates on `firing_active`.

### LOW-3 (F1): stale diag_state after a Pico reboot can start the clock in GRACE

- `safety_note_pico_reboot_locked()` (`safety_link_frames.c:270`) resets
  trip_last_seq and diag_trip_seq, but not `diag_state`.
- The executor's freshness check uses `age_ms`, which is shared with STATUS
  (`profile_executor.c:2209-2210`).
- Scenario: the first STATUS frames after a Pico reboot arrive while the cached
  DIAG still says ARMED from the previous boot. F1 sees "fresh, ARMED, K4 open"
  during the real GRACE and can spend a resend that the Pico accepts but does
  not apply (relay_owner GRACE). This wastes budget, which compounds LOW-1. The
  window is under one DIAG period (2 s) against a 3 s confirm window, so it is
  usually harmless.

### LOW-4 (F3): the 3 s DIAG wait is shorter than two DIAG periods plus margin

`safety_cfg_write.c:182`.

- DIAG is sent every 2 s, so one dropped DIAG frame turns a persisted commit
  into a false "UNCONFIRMED" failure (and, via MED-3, skips the estop clear).
- Suggested: 5 s or more. That is still bounded, and these are httpd/worker
  callers.

### LOW-5 (F3): nonblocking callers judge a pre-commit DIAG

`safety_cfg_write.c:320` breaks immediately for `nonblocking_refetch`, so the
verdict comes from a DIAG that predates the commit.

- `kiln_cfg_swap.c:659` (step 13, the flash fallback, which runs right after
  its own volatile install) therefore always reads STILL_DIRTY and logs
  "ceiling flash persist did not land ... expected while ARMED". It logs this
  even when the board is unarmed and the persist did land. The INFO is
  misleading and harmless.
- The ceiling reconcile (`safety_ceiling_sync.c:245`) fails its first attempt
  after any volatile change, backs off, and converges on the next pass, which
  costs one backoff interval.

### LOW-6 (F5): the barrier pair is not a seqlock, and the reader comment overclaims

The writer is at `safety_core.c:1451-1458`, with fields, `HAL_DMB()`, then the
seq. The reader at `:1732-1733` loads the seq, runs `HAL_DMB()`, then loads the
fields, and never re-reads the seq.

- The barriers do order the first publication correctly.
- If a second trip publishes while the reader is between its seq load and its
  field loads, the reader returns seq N with trip N+1's fields. A second trip
  needs a clear in between, so the window is microseconds and the result is a
  mislabelled log or event, never a relay effect.
- The comment at :1722-1728 ("can never observe a torn mix") is false as
  written.
- Suggested: re-read `s_trip_seq` after a second `HAL_DMB()` and retry on a
  mismatch (2 lines). `s_trip_seq` is also not `volatile`. That is fine with the
  asm-clobber barrier, but worth a comment.

### LOW-7 (F2 test gap): the HEAT_OWNER argument at the call site is untested

Negtest (below): replacing
`(snap.flags & CONTEXT_FLAG_HEAT_OWNER_ACTIVE) != 0u` with `true` at
`link_task.c:1395-1396` defeats F2 entirely, because no drop ever happens.
**MISSED** by the SaftyFW host suite.

- The source scan at `test_link_staging.c:330-334` checks only that
  `safety_core_request_enable(false)` follows the decision call.
- Suggested: extend the scan to require the `CONTEXT_FLAG_HEAT_OWNER_ACTIVE`
  argument text.

### LOW-8 (F1/F3 test gaps): three mutations MISSED

Each mutation below was run against the 9 executables selected by
`-Only 'heat_enable|profile_exec|safety_cfg|safety_link|safety_ceiling'`
(including `main`, which carries `test_heat_enable.c`). All three were
**MISSED**:

1. `profile_executor.c:2213`: `safety_k4_closed = true;`. F1 is fully disabled,
   so no re-request ever fires, and nothing tests the executor-side wiring.
2. `heat_enable.c:687`: removing `s_he.k4_open_since_ms = 0u;` from the
   non-ARMED branch, so the clock keeps running through GRACE. The F1 test
   enters GRACE only before the first ARMED.
3. `safety_cfg_write.c:320`: dropping `nonblocking_refetch ||`, so the
   poll-task caller would sleep up to 3 s on the task that delivers DIAG. That
   is a self-deadlock-shaped stall of safety_poll_task, for which the original
   design forbade any block.

Mutation 3 is the most serious of the three gaps, because the property it
removes is a documented "must never block" rule.

### LOW-9 (F2): a session that never sends PUSH_CONTEXT keeps the grant

The drop runs only inside the PUSH_CONTEXT handler (`link_task.c:1389-1399`).
An ESP image that talks to the Pico but never pushes context (for example the
recovery image) inherits the grant until S6b. This predates the batch and is
noted for completeness.

### LOW-10 (stack): commit message and ceiling provenance

- `5abfe307d`'s message says "re-pin profile_exec_wdt **and safety_core**", but
  it touches only `firmware/KilnFW/App/test/check_all_task_stack_budgets.py`.
- The `safety_core: 2208` ceiling (`check_saftyfw_task_stack_budgets.py:377`)
  came from `163980001` (the guard-fixes pass), and its comment attributes the
  growth to that pass, not to F4/F5.
- Either F4/F5 did not move safety_core's measured depth (plausible:
  `link_frame_next_trip_seq` is a leaf and `HAL_DMB` adds no stack), or the
  re-pin was lost. A fresh SaftyFW ELF measurement would settle it.

## NIT

None.

## Checked, no issue

- **F4:** `link_frame_next_trip_seq` (`link_frame.c:248-252`) never yields 0.
  The ESP dedup in `safety_trip_decision.c` is inequality-based, so 255 -> 1 is
  a new event. `safety_note_pico_reboot_locked()` still pairs the Pico's seq
  restart with clearing `trip_last_seq`/`diag_trip_seq`. The wrap and the
  boot_id reset are consistent.
- **F2:** only the de-energise direction is used, and
  `safety_core_request_enable(false)` is never refused. The drop happens only
  on a new session (boot_id change or a context gap of 5000 ms or more), not on
  every context frame. A running firing sets HEAT_OWNER_ACTIVE through
  `heat_owner_active_decide` (`safety_link_frames.c:624-627`), so a firing's own
  grant is kept. An enable that races ahead of a stale-snapshot context is
  dropped, then recovered by F1, which is fail-safe.
- **F3 verdict function:** `safety_cfg_persist_verdict.h` is pure, and
  UNKNOWN/STILL_DIRTY/PERSISTED map correctly. Volatile installs skip the
  persisted check (`require_persisted = !volatile_install`) as intended.
- **F1 executor ordering:** `note_pico_state` runs after the wd_decide switch.
  A TRIPPED fault that already released the claim takes the `held_mask == 0`
  reset path, so no re-request follows a seen trip.

## F6 recommendation

F6 (CLEAR_TRIP carries no boot identity) is worth doing, but not urgently and
not on its own. The attack needs a stale or replayed CLEAR_TRIP on a wired
point-to-point UART whose frames are CRC-checked and sequence-deduped. The clear
must also land after a Pico reboot, carry the same seq (in practice 1) and the
same mask, and arrive after the new trip's condition has already gone away. A
clear never overrides a holding condition, so the worst outcome is clearing a
trip an operator had not yet acknowledged. That is real but narrow.

A protocol 17 -> 18 bump costs a coordinated change on three trees plus
frame/decide tests on both sides, and it adds a version-compatibility row.
Recommendation: defer F6 until the next protocol bump that is needed for another
reason, and fold it in then (bind the clear to `0x100 | seq` plus the Pico's
8-bit `boot_id`, refuse a mismatch, and keep the unbound-clear path only for
pre-18 peers). Do not bump the protocol for F6 alone. A cheaper interim step needs no wire change, but it has a trade-off: the Pico
could keep refusing unbound clears for a short window after an ESP boot_id
change, until the new peer announces its version. That closes the second F6
bullet, but it delays the clear after a rollback to a pre-17 ESP, which is the
case the version reset exists for. Owner call.

## Tests run (worktree at dev tip)

- SaftyFW host tests (`firmware\SaftyFW\test\build_host_tests.ps1`) at
  `6201932f5`: all passed ("72/72 checks passed" in the last section, "SAFTYFW
  HOST TESTS: all passed").
- KilnFW host tests (`firmware\KilnFW\App\test\build_host_tests.ps1`, full) at
  `4ff79bc8c`: 81/81 executables built and passed. `sim_credibility_gate`
  skipped as expected because its captures are gitignored.
- `tools\negtest.ps1 -Preset saftyfw-host` at `6201932f5`. The baseline passed.
  - CAUGHT: removing `safety_core_request_enable(false)` from the F2 call site.
  - MISSED: HEAT_OWNER argument replaced with `true` (LOW-7).
- `tools\negtest.ps1 -Command "build_host_tests.ps1 -Only '...'"` at
  `4ff79bc8c`. The baseline passed. All three mutations listed in LOW-8 were
  MISSED.
- No target build, no `run_all_checks.ps1`, no bench access.
