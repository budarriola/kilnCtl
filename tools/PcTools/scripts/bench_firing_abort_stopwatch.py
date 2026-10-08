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
    t0 = the wall-clock moment this script issues the Pico halt (see "Halt
         method"). The Pico stops sending frames from that instant, so t0 is
         the last-good-frame boundary to within the OpenOCD call's own
         duration, which is measured and added to the pass window's upper
         bound.
    t1 = first poll at which profiles_get_exec_status() reports
         state == PROFILE_EXEC_FAULTED (int 4) with a fault_reason naming the
         safety link.
    t2 = first poll at or after t1 at which io_read() reports every relay
         (1-4) commanded off.

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

Halt method (owner decision 2026-10-07): the link is PCB traces, so there is
no cable to unplug, and debug_reset(peer="pico") reboots the Pico which resumes
sending within seconds (it cannot hold 30 s of silence). Instead the Pico is
HALTED over the CMSIS-DAP probe with the sanctioned tools debug_halt(peer=
"pico") plus debug_read_registers(peer="pico", target="rp2040.core1",
leave_halted=True) (an RP2040 debug access must name a core, so core 1 is
halted explicitly as well), and resumed with debug_resume(peer="pico"). Never a
memory write, never a flash, no raw OpenOCD.

Halt bound: the Pico stays halted only until the abort is proven (FAULTED then
relays off) or max_halt_s() = threshold + poll period + 5 s has elapsed,
whichever comes first. The resume runs in a `finally`, so it also happens on an
exception, KeyboardInterrupt or a timeout. The resume is confirmed by the
resume tool reporting no core still halted AND safety_get_status answering; if
it cannot be confirmed the run FAILS loudly with an operator instruction (the
safety processor may still be halted: power-cycle it or debug_reset the pico,
and call profiles_stop()). The halt duration is reported.

HAZARD: while the Pico is halted the independent safety processor is OUT OF THE
LOOP -- nothing but the ESP's own fault path is protecting the firing. That
ESP path is exactly what is under test. After the resume the Pico sees a long
link gap, so an S6b (link dead) and/or S6a trip is the EXPECTED aftermath.
This script never clears it; clear it by the usual rules (docs/MCP_SERVERS.md
flash section: verify trip_reason/trip_mask, then safety_clear_trip()).

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

#: Extra seconds of halt beyond threshold + one poll before giving up. The
#: Pico is halted (safety processor out of the loop) for at most
#: ABORT_THRESHOLD_S + poll_period + HALT_MARGIN_S.
HALT_MARGIN_S = 5.0

#: How long to wait for safety_get_status to answer after the resume.
RESUME_CONFIRM_S = 15.0

OPERATOR_RESUME_FAILED = (
    "OPERATOR ACTION REQUIRED: the Pico resume could NOT be confirmed. The "
    "safety processor may STILL BE HALTED and the firing has no independent "
    "safety monitor. Power-cycle the board or run debug_reset(peer=\"pico\") "
    "now, and call profiles_stop() to stop the firing."
)


def max_halt_s(poll_period):
    """Hard bound on how long the Pico may stay halted."""
    return ABORT_THRESHOLD_S + poll_period + HALT_MARGIN_S


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
    should fault a RUNNING firing in the few seconds around the Pico halt
    call on a bench with no operator/guard activity) plus the causal
    Pico halt this script itself just issued."""
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

    print("Pre-abort state: RUNNING, confirmed. HALTING the Pico now (safety "
          "processor out of the loop until resumed; bound "
          f"{max_halt_s(args.poll_period):.0f}s).")
    meas = measure_abort(args.poll_period, emit=print)
    ok, report = judge_run(meas, args.poll_period)
    for line in report:
        print(line)
    return 0 if ok else 1


def _is_error(result) -> bool:
    return not isinstance(result, str) or result.lstrip().lower().startswith("error")


def _halt_pico():
    """Halt both RP2040 cores via the sanctioned debug tools. (ok, detail)."""
    r0 = m.debug_halt(peer="pico")
    if _is_error(r0):
        return False, f"debug_halt(pico) -> {r0}"
    r1 = m.debug_read_registers(peer="pico", target="rp2040.core1", leave_halted=True)
    if _is_error(r1):
        return False, f"core1 halt -> {r1}"
    return True, f"debug_halt(pico) -> {r0}; core1 halted"


def _resume_pico():
    r = m.debug_resume(peer="pico")
    return (not _is_error(r)), str(r)


def _link_alive() -> bool:
    return not _is_error(m.safety_get_status())


def _resume_and_confirm(resume, link_alive, clock, sleep):
    """Resume the Pico (one retry) and confirm. Never raises. (ok, detail)."""
    detail = ""
    resumed = False
    for _ in range(2):
        try:
            resumed, detail = resume()
        except Exception as exc:  # noqa: BLE001
            resumed, detail = False, f"resume raised {exc!r}"
        if resumed:
            break
    if not resumed:
        return False, f"resume failed: {detail}"
    deadline = clock() + RESUME_CONFIRM_S
    while True:
        try:
            if link_alive():
                return True, detail
        except Exception:  # noqa: BLE001
            pass
        if clock() >= deadline:
            return False, ("resume reported ok but safety_get_status never "
                           f"answered: {detail}")
        sleep(1.0)


def measure_abort(poll_period, *, clock=time.time, sleep=time.sleep,
                  halt=None, resume=None, link_alive=None,
                  exec_status=None, relays_all_off=None, emit=None):
    """Halt the Pico and poll until FAULTED + relays off or the halt bound.
    The Pico is ALWAYS resumed in a finally (exception, KeyboardInterrupt,
    timeout). All I/O is injectable so host tests can drive it with fakes."""
    halt = halt or _halt_pico
    resume = resume or _resume_pico
    link_alive = link_alive or _link_alive
    exec_status = exec_status or _exec_status
    relays_all_off = relays_all_off or _relays_all_off
    meas = {"t0": None, "t_faulted": None, "t_relays_off": None,
            "fault_guard": None, "halt_ok": False, "halt_detail": "",
            "halt_call_s": 0.0, "halt_duration_s": None,
            "resume_ok": None, "resume_detail": "", "log": []}
    halt_attempted = False
    t_halted = None
    try:
        t0 = clock()
        meas["t0"] = t0
        halt_attempted = True
        try:
            ok, detail = halt()
        except Exception as exc:  # noqa: BLE001
            ok, detail = False, f"halt raised {exc!r}"
        t_halted = clock()
        meas["halt_call_s"] = t_halted - t0
        meas["halt_ok"] = ok
        meas["halt_detail"] = detail
        if emit:
            emit(f"  halt pico -> ok={ok} {detail}")
        if not ok:
            return meas  # no measurement; the finally still resumes
        deadline = t0 + max_halt_s(poll_period)
        while clock() < deadline:
            now = clock()
            state, guard = exec_status()
            if meas["t_faulted"] is None and state == PROFILE_EXEC_FAULTED:
                meas["t_faulted"] = now
                meas["fault_guard"] = guard
            relays_off = relays_all_off()
            # Only a reading taken at or after FAULTED counts: relays read off
            # earlier (a time-proportioned off-phase, or the 1.5 s link-fault
            # relay-on refusal) say nothing about the abort dropping them.
            # `is True` so an unreadable (None) reading never counts as off.
            if (meas["t_relays_off"] is None and meas["t_faulted"] is not None
                    and relays_off is True):
                meas["t_relays_off"] = now
            line = (f"  t+{now - t0:6.1f}s  exec_state={state}  relays_off={relays_off}"
                    + (f"  fault_guard={meas['fault_guard']}"
                       if meas["fault_guard"] is not None else ""))
            meas["log"].append(line)
            if emit:
                emit(line)
            if meas["t_faulted"] is not None and meas["t_relays_off"] is not None:
                break
            sleep(poll_period)
        return meas
    finally:
        if halt_attempted:
            ok, detail = _resume_and_confirm(resume, link_alive, clock, sleep)
            meas["resume_ok"] = ok
            meas["resume_detail"] = detail
            if t_halted is not None:
                meas["halt_duration_s"] = clock() - t_halted
            if emit:
                emit(f"  resume pico -> ok={ok} {detail}")
                if not ok:
                    emit(OPERATOR_RESUME_FAILED)


def judge_run(meas, poll_period):
    """Whole-run verdict: halt, abort timing, and the mandatory resume."""
    out = []
    if not meas["halt_ok"]:
        out.append(f"\nFAIL: could not halt the Pico ({meas['halt_detail']}); "
                   "no measurement taken.")
        ok = False
    else:
        ok, rep = judge_abort(meas["t0"], meas["t_faulted"], meas["t_relays_off"],
                              poll_period, max_halt_s(poll_period) - ABORT_THRESHOLD_S,
                              halt_call_s=meas["halt_call_s"])
        out.extend(rep)
    if meas["halt_duration_s"] is not None:
        out.append(f"Pico was halted for {meas['halt_duration_s']:.1f}s "
                   f"(bound {max_halt_s(poll_period):.0f}s).")
    if not meas["resume_ok"]:
        out.append("\nFAIL: Pico resume NOT confirmed. " + OPERATOR_RESUME_FAILED)
        ok = False
    else:
        out.append("Pico resumed and confirmed. Expect an S6b (link dead) and/or "
                   "S6a trip: do NOT auto-clear; follow the docs/MCP_SERVERS.md "
                   "flash section (check trip_reason/trip_mask first).")
    return ok, out


def judge_abort(t0, t_faulted, t_relays_off, poll_period, max_wait,
                halt_call_s=0.0):
    """Pure verdict. Returns (ok, report_lines)."""
    out = []
    if t_faulted is None:
        out.append(f"\nFAIL: firing never reached FAULTED within "
                   f"{ABORT_THRESHOLD_S + max_wait:.0f}s of silence. "
                   "The 30 s safety-link firing-abort did not fire.")
        return False, out

    abort_latency_s = t_faulted - t0
    # +1s transport/exec slack, plus the halt call's own duration (t0 is taken
    # before it; the Pico actually went silent somewhere inside it).
    upper_bound = ABORT_THRESHOLD_S + poll_period + 1.0 + halt_call_s
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
