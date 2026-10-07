#!/usr/bin/env python3
"""Time the 30 s link-silence firing-abort (ROADMAP.md M6 / LINK_PROTOCOL.md
sec 8) during a REAL running firing, so the owner's one firing can close the
item without a human holding a literal stopwatch.

Requirement, as found (LINK_PROTOCOL.md sec 8, "8. Liveness: the safety
processor must be alive to heat"):

    | Silence  | Action                                                     |
    |----------|------------------------------------------------------------|
    | > 1.5 s  | Assert SAFETY_FAULT_SRC_SAFETY_LINK. All relay-on refused.  |
    |          | Clears automatically on the next good frame.               |
    | > 30 s   | Abort the firing -- profile_executor faults, relays dropped|
    |          | and retried until the write succeeds.                      |

The 1.5 s half was bench-verified 2026-09-06 with a stopwatch against the
device log. This script closes the other half: "30 s silence aborts a
RUNNING firing" (ROADMAP.md's "Blocked on hardware that does not exist yet"
row -- the only thing actually missing is a firing in progress, everything
else is already software).

Definition used here, since "abort" is otherwise ambiguous (operator stop /
guard trip / link loss all exist in this codebase and are NOT the same
event): this measures ONLY the safety-link-silence path --
SAFETY_LINK_FIRING_ABORT_SILENCE_MS (30000, safety_link.h) as read by
profile_executor's watchdog task via safety_link_is_stale(age_ms, ...). An
operator-initiated profiles_stop() or a guard trip (thermal_guard.c etc.)
are different events with different timing budgets and are NOT what
ROADMAP.md's "30 s firing abort" row refers to -- see LINK_PROTOCOL.md sec 8
and the "Blocked on hardware" table entry, which both scope it to link
staleness alone.

Event pair measured:
    t0 = the wall-clock moment this script issues debug_reset(peer="pico")
         (the same technique the 1.5 s bench pass used to silence telemetry
         without a physical disconnect -- the Pico stops sending frames from
         that instant, so t0 is the last-good-frame boundary to within one
         Pico boot's jitter, same caveat the 1.5 s pass already accepted).
    t1 = first poll at which profiles_get_exec_status() reports
         state == PROFILE_EXEC_FAULTED (int 4) with a fault_reason naming the
         safety link.
    t2 = first poll at which io_read() reports every relay (1-4) commanded
         off.

Pass criterion: t1 - t0 in [30.0, 30.0 + POLL_PERIOD_S + slack] seconds
(SAFETY_LINK_FIRING_ABORT_SILENCE_MS is a hard >= comparison against
age_ms, itself only as fresh as the watchdog task's own
WATCHDOG_CHECK_PERIOD_MS poll -- see profile_executor.c's own comment beside
safety_link_silent_30s). t2 should follow t1 within one more poll period
(the fault branch calls kiln_io_all_relays_off() the same tick it faults).
No firmware timestamp had to be added for this: /api/profile_exec already
reports state/fault_reason and /api/status already reports relays[]/age_ms
(io_read()/profiles_get_exec_status() below use the UART bridge equivalents
of those, so this works over the same link a real firing runs on).

SAFETY: this silences the safety processor's telemetry on a board that MUST
have a real firing running, by design -- that is the whole point of the
test. It refuses to run at all unless profiles_get_exec_status() already
reports a RUNNING firing when invoked; it will not start one, and it never
turns a relay on itself. It aborts a fire in progress on purpose: run it only
when you intend exactly that. It does not stop the script's own safety net --
if the abort fails to fire, the firing keeps running and you still have the
kiln's own guards; this script only observes.

Usage (while a firing is already RUNNING):
    uv run --project tools/PcTools python \\
        tools/PcTools/scripts/bench_firing_abort_stopwatch.py

Exit status: 0 = abort observed within tolerance (PASS), 1 = FAIL or refused
to run (e.g. no firing in progress), 2 = usage/connection error.
"""
from __future__ import annotations

import argparse
import re
import sys
import time

from kilnctrl import mcp_server as m

PROFILE_EXEC_RUNNING = 1
PROFILE_EXEC_FAULTED = 4

#: SAFETY_LINK_FIRING_ABORT_SILENCE_MS, safety_link.h:404.
ABORT_THRESHOLD_S = 30.0

#: WATCHDOG_CHECK_PERIOD_MS granularity the profile_executor watchdog task
#: polls safety_link_get_status() at -- the same source of "worst-case slack
#: on top of the coded threshold" the 1.5 s bench pass measured (2250 ms
#: against a 1500 ms ceiling, ~750 ms over). Given as a poll period here
#: rather than hardcoded, so --poll-period lets a future re-run match a
#: changed constant without editing this file.
DEFAULT_POLL_PERIOD_S = 1.0

#: How long past the threshold to keep waiting before calling it a FAIL --
#: generous on purpose (a slow poll loop here must not manufacture a false
#: failure), but bounded so a genuinely dead abort path doesn't hang the
#: bench forever.
DEFAULT_MAX_WAIT_S = 60.0

_FAULT_RE = re.compile(r"fault_guard=(\d+)")


def _exec_status():
    """Return (state:int, fault_guard:int|None) from the structured exec
    status. The UART-bridge ProfileExecStatus carries fault_guard (an int
    code) but not the human fault_reason text -- that only reaches
    GET /api/profile_exec over HTTP. fault_guard is left unset (0, same as
    "no guard") on the safety-link-silent path (profile_executor.h's
    profile_executor_wd_decide(), the safety_link_silent_30s branch), so it
    cannot distinguish this cause from others by itself -- this script
    instead relies on being the one thing perturbing the board (nothing else
    should fault a RUNNING firing in the few seconds around the debug_reset
    call on a bench with no operator/guard activity) plus the causal
    debug_reset(peer="pico") this script itself just issued."""
    try:
        st = m._profiles.get_exec_status()  # structured, same object the tool wraps
    except Exception:  # noqa: BLE001
        return None, None
    return st.state, getattr(st, "fault_guard", None)


def _relays_all_off() -> "bool | None":
    try:
        state = m._io.read()
    except Exception:  # noqa: BLE001
        return None
    return all(not state.relay(n) for n in range(1, 5))


def require_running() -> bool:
    state, _ = _exec_status()
    if state != PROFILE_EXEC_RUNNING:
        print(
            f"REFUSED: profiles_get_exec_status() reports state={state}, "
            f"not RUNNING ({PROFILE_EXEC_RUNNING}). This script only measures "
            "a real running firing's abort -- start one first (profiles_start) "
            "and confirm it is actually heating before re-running this."
        )
        return False
    return True


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--poll-period", type=float, default=DEFAULT_POLL_PERIOD_S)
    ap.add_argument("--max-wait", type=float, default=DEFAULT_MAX_WAIT_S)
    ap.add_argument(
        "--i-am-aborting-a-real-firing",
        action="store_true",
        help="Required. States explicitly that you intend to abort the firing "
        "currently running on the board.",
    )
    args = ap.parse_args()

    if not args.i_am_aborting_a_real_firing:
        print(
            "REFUSED: pass --i-am-aborting-a-real-firing to confirm you intend "
            "to knock down the firing currently running on the board. This is "
            "not a dry run -- it silences the safety link on purpose."
        )
        return 2

    print(m.connect())

    if not require_running():
        return 1

    print("Pre-abort state: RUNNING, confirmed. Silencing the safety link now "
          "(debug_reset(peer=\"pico\")) -- the same technique the 1.5 s bench "
          "pass used.")
    meas = measure_abort(args.poll_period, args.max_wait, emit=print)
    ok, report = judge_abort(meas["t0"], meas["t_faulted"], meas["t_relays_off"],
                             args.poll_period, args.max_wait)
    for line in report:
        print(line)
    return 0 if ok else 1


def measure_abort(poll_period, max_wait, *, clock=time.time, sleep=time.sleep,
                  silence=None, exec_status=None, relays_all_off=None, emit=None):
    """Silence the link and poll until FAULTED + relays off or the deadline.
    All I/O is injectable so host tests can drive it with a fake clock."""
    silence = silence or (lambda: m.debug_reset(peer="pico"))
    exec_status = exec_status or _exec_status
    relays_all_off = relays_all_off or _relays_all_off
    t0 = clock()
    reset_result = silence()
    if emit:
        emit(f"  debug_reset(peer=pico) -> {reset_result}")
    t_faulted = None
    t_relays_off = None
    fault_guard = None
    log = []
    deadline = t0 + ABORT_THRESHOLD_S + max_wait
    while clock() < deadline:
        now = clock()
        state, guard = exec_status()
        if t_faulted is None and state == PROFILE_EXEC_FAULTED:
            t_faulted = now
            fault_guard = guard
        relays_off = relays_all_off()
        if t_relays_off is None and relays_off:
            t_relays_off = now
        elapsed = now - t0
        line = (f"  t+{elapsed:6.1f}s  exec_state={state}  relays_off={relays_off}"
                + (f"  fault_guard={fault_guard}" if fault_guard is not None else ""))
        log.append(line)
        if emit:
            emit(line)
        if t_faulted is not None and t_relays_off is not None:
            break
        sleep(poll_period)
    return {"t0": t0, "t_faulted": t_faulted, "t_relays_off": t_relays_off,
            "fault_guard": fault_guard, "reset_result": reset_result, "log": log}


def judge_abort(t0, t_faulted, t_relays_off, poll_period, max_wait):
    """Pure verdict. Returns (ok, report_lines)."""
    out = []
    if t_faulted is None:
        out.append(f"\nFAIL: firing never reached FAULTED within "
                   f"{ABORT_THRESHOLD_S + max_wait:.0f}s of silence. "
                   "The 30 s safety-link firing-abort did not fire.")
        return False, out

    abort_latency_s = t_faulted - t0
    upper_bound = ABORT_THRESHOLD_S + poll_period + 1.0  # +1s transport/exec slack
    ok = ABORT_THRESHOLD_S <= abort_latency_s <= upper_bound

    out.append(f"\nAbort latency (silence -> FAULTED): {abort_latency_s:.2f}s "
               f"(pass window [{ABORT_THRESHOLD_S:.1f}, {upper_bound:.1f}]s)")
    out.append("Note: fault_guard is 0/unset on this path by design (the "
               "safety-link-silent branch in profile_executor_wd_decide() does "
               "not set it) -- confirm the cause was this link-silence path, not "
               "an unrelated fault, by checking the ESP device log "
               "(get_device_log()) for 'safety processor link silent' around this "
               "timestamp if this run's context is not already unambiguous.")
    if t_relays_off is not None:
        out.append(f"Relays confirmed off at t+{t_relays_off - t0:.2f}s "
                   f"({t_relays_off - t_faulted:.2f}s after FAULTED)")
    else:
        out.append("Relays never confirmed off within the wait window -- "
                   "check io_read() by hand now.")
        ok = False

    verdict = "PASS" if ok else "FAIL"
    out.append(f"\n{verdict}: 30 s firing-abort "
               + ("fired within tolerance and dropped relays." if ok else
                  "did not meet the pass window or relays did not confirm off."))
    return ok, out


if __name__ == "__main__":
    sys.exit(main())
