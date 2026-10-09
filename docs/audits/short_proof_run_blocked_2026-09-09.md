# Short proof-run: blocked before heating, 2026-09-09

## Outcome

**No heat was applied.** The board's own firing interlock
(`firmware/KilnFW/App/drivers/safety/readiness_gate.h`) refuses to start any
firing while two readiness items are outstanding, and reports no override for
either:

1. **Unacknowledged crash report** — `exc_task='profile_executo'`,
   `exc_cause_str='IllegalInstruction'`, `reset_reason='PANIC'`. This is the
   earlier `profile_executor` stack-overflow panic; it has not been cleared
   via `POST /api/crash_report/ack` on this board.
2. **E-stop interlock never verified** (`estop_verified`) — pole 1 (contactor
   coil) is wiring the firmware cannot see; the bench verification procedure
   in `README.md` has not been confirmed via `/safety` on this board.

`capability_preflight_check` surfaced both as `[FATAL]` and stated plainly:
"the board's firing interlock blocks on crash_report, estop_verified -- this
run would be refused; do not start it."

Per the task brief, an unacknowledged crash report blocking a run is a
stop-and-ask condition, not something to clear unilaterally — especially
since this crash is the exact failure class (`profile_executor` panic) the
precondition check in this same run existed to guard against, and a second,
independently-blocking item (E-stop verification) was also outstanding.
Heating was not attempted.

## Precondition check (passed)

The ESP was reflashed to HEAD (`0dddd435`, built 2026-09-10 00:09:57Z) by
another session during this task. `379f3fe6` (the `profile_executor`
stack-overflow fix) is confirmed an ancestor of the running commit
(`git merge-base --is-ancestor 379f3fe6 0dddd435` → true). The firmware
precondition for a heating run was satisfied; the block is the unrelated
readiness-gate items above.

## Other pre-flight findings, for the record

- `safety_get_status`: link up, SaftyFW armed, not tripped, safety TC valid
  (~42.5 C), `ct_counts 17, 17, 54`, `tx_dropped 0`.
- `safety_get_commissioning`: `commissioned=True`, config CRC matches live.
  S1 (`abs_max_temp_c=80C`) and S8 (`max_rate_c_per_min=20C/min`) both ARMED.
- All three main-board thermocouples read ~43-44.5 C with no faults
  (SR 0x00 on all channels) — well above the 20-25 C ambient the run's
  35-40 C target guidance assumed. Had the run proceeded, the profile
  (`livefire`, slot #2) would have needed a target above current temperature
  (e.g. ~60 C) rather than the originally-suggested 35-40 C, to actually
  exercise closed-loop rise. This was corrected in a working copy of the
  profile during pre-flight and then **reverted back to its original values
  (`target_c=45.0`, `ramp_c_per_hr=600.0`, `dwell_min=3`, `zone_mask=0x1`)**
  once the run was called off, so no profile is left modified on the board.
- `io_read`: all four relays (R1-R4) read 0 (off) both before and after
  pre-flight.
- `profiles_get_exec_status`: idle (`state=0`), no active firing.

## Board final state

Left safe and idle: relays off, no firing in progress, safety link up and
armed, no configuration changes retained (the `livefire` profile edit was
reverted). The unacknowledged crash report and unverified E-stop item remain
open — clearing them is a decision for the board owner, not this task.

## Next steps (for the owner, not actioned here)

- Decide whether the outstanding crash report is truly stale (predates the
  `379f3fe6`/`51e1ef5` fixes) and can be acknowledged via
  `POST /api/crash_report/ack`, or whether it needs further review first.
- Run the E-stop bench-verification procedure (see `README.md`, and
  `docs/audits/` entries from 2026-09-09 on E-stop bench verification) and
  confirm via `/safety` so `estop_verified` clears.
- Only then retry this short proof run.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>
