#!/usr/bin/env python3
"""Reboot the Pico (debug_reset peer="pico") DURING a real running firing and
record what the ESP does -- RELEASE_HARDENING_PLAN section 5's never-exercised
"Pico reboots mid-firing" scenario.

Expected end state, derived from the code (not guessed). Paths are relative to
firmware/:

  1. The ESP flags the link down after > SAFETY_LINK_STALE_MS = 1500 ms of
     silence: KilnFW/App/drivers/safety/safety_link.h:460 (constant),
     safety_link_poll.c:313 (safety_link_set_fault_source(
     SAFETY_FAULT_SRC_SAFETY_LINK, !up || version_mismatch)). While that fault
     source is asserted every relay-on is refused; relay-off never is. A
     RUNNING firing is NOT touched by this 1.5 s condition
     (KilnFW/App/drivers/control/profile_executor.c:2085-2090 comment).
  2. A firing is aborted only by >= SAFETY_LINK_FIRING_ABORT_SILENCE_MS =
     30000 ms of continuous silence: safety_link.h:468,
     profile_executor.c:2118-2119 (safety_link_silent_30s), acted on at
     profile_executor.h:248-254 (-> FAULT "safety processor link silent for
     >=30000ms, firing aborted"). A Pico reboot takes seconds, far under 30 s,
     so the EXPECTATION IS THAT THE FIRING SURVIVES (state stays RUNNING). If
     the link stays down >= 30 s the code DOES abort; this script then fails.
  3. On the first FW_VERSION frame after the reboot the ESP sees a changed
     pico boot_id (safety_link_frames.c:279-282): it clears the trip dedup
     (trip_event_ever_received=false, trip_last_seq=0), forgets
     safety_relay_state_known and sets reannounce_pending (:282-304). So the
     Pico's boot_id MUST differ from the pre-snapshot and its fw_version
     (commit) must be reported again.
  4. The Pico does not trip S6b on its own reboot: s6b_link_down_elapsed_s
     starts at 0 on a fresh boot and only accumulates while the ESP is silent
     to it (SaftyFW/src/safety_guards.c:486-525; link_timeout_s default 10 s
     with current present, link_dead_hard_s default 120 s, :26-27). The ESP
     keeps polling during the reboot, so no S6b is expected: trip_reason must
     equal the pre-snapshot value (normally 0).

Expectations judged (exit 1 if any fails):
  E1 link reported down (age_ms > 1500 or link_up false) inside the window.
  E2 while the link is down the ESP's isolated fault line is asserted
     (safety status FAULT flag, driven by SAFETY_FAULT_SRC_SAFETY_LINK at
     safety_link_poll.c:313, which blocks every heater-on). FAIL if the fault
     line reads clear while the link is down (heat still being granted);
     "not observable" (reported, not failed) if the flag cannot be read.
     No relay write is ever made: the owner's system_mode_gate refuses relay
     and zone writes (409) during any firing regardless of link state, so a
     write probe would prove nothing, and we do not poke relays mid-firing.
  E3 link recovers and the Pico fw_version is reported with a changed
     boot_id, within the 45 s window.
  E4 link down for < 30 s and the firing is RUNNING afterwards, never FAULTED.
  E5 Pico trip_reason unchanged vs the pre-snapshot (no S6b or other trip).

SAFETY: reboots the safety processor under a live firing on purpose. It never
starts a firing, never writes a relay, never clears a trip.

Usage:
    uv run --project tools/PcTools python \\
        tools/PcTools/scripts/bench_pico_reboot_midfiring.py \\
        --i-am-rebooting-the-pico-mid-firing [--out evidence.json]

Exit status: 0 = all expectations hold, 1 = an expectation failed, 2 = refused
(missing confirm flag, or no firing running; nothing was reset).
"""
from __future__ import annotations

import argparse
import json
import sys
import time

PROFILE_EXEC_RUNNING = 1
PROFILE_EXEC_FAULTED = 4
STALE_MS = 1500            # safety_link.h:460
ABORT_SILENCE_S = 30.0     # safety_link.h:468
DEFAULT_WINDOW_S = 45.0
DEFAULT_POLL_S = 0.1


class RealBoard:
    """Thin adapter over the kilnctrl MCP server's clients. Methods may
    raise; the runner treats an exception as an unreadable sample."""

    def __init__(self):
        from kilnctrl import mcp_server as m
        self.m = m

    def connect(self):
        return self.m.connect()

    def exec_status(self):
        st = self.m._profiles.get_exec_status()
        return {"state": st.state, "segment": st.segment_index,
                "segment_count": st.segment_count,
                "segment_elapsed_s": st.segment_elapsed_s,
                "zone_temps": [z.actual_c for z in st.zones]}

    def link(self):
        st = self.m._safety.get_status()
        return {"link_up": bool(st.link_up), "age_ms": st.age_ms,
                "fault_asserted": bool(st.fault_asserted)}

    def pico_version(self):
        v = self.m._safety.get_fw_version()
        return {"known": bool(v.known), "commit": v.commit, "boot_id": v.boot_id}

    def counters(self):
        d = self.m._safety.get_diag()
        t = self.m._safety.get_trip_event()
        s = self.m._safety.get_link_stats()
        return {"trip_reason": d.trip_reason, "trip_mask": d.trip_mask,
                "diag_ever_received": d.ever_received,
                "trip_event_last_seq": t.last_seq,
                "trip_event_ever_received": t.ever_received,
                "frames_sent": s.frames_sent, "frames_received": s.frames_received,
                "crc_errors": s.crc_errors, "timeouts": s.timeouts}

    def reset_pico(self):
        return self.m.debug_reset(peer="pico")


def _safe(fn, default=None):
    try:
        return fn()
    except Exception as exc:  # noqa: BLE001
        return default if default is not None else {"error": repr(exc)}


def snapshot(board):
    return {"exec": _safe(board.exec_status), "link": _safe(board.link),
            "pico_version": _safe(board.pico_version),
            "counters": _safe(board.counters)}


def link_is_down(link):
    return bool(link) and "error" not in link and (
        (not link["link_up"]) or link["age_ms"] > STALE_MS)


def run_reboot(board, *, window_s=DEFAULT_WINDOW_S, poll_s=DEFAULT_POLL_S,
               clock=time.time, sleep=time.sleep, emit=None):
    """Pre-snapshot, reset the Pico, poll, post-snapshot. Returns evidence."""
    ev = {"pre": snapshot(board), "t_reset_call_s": None, "reset_result": None,
          "t_link_down_s": None, "link_down_fault": None,
          "t_link_recovered_s": None, "t_version_back_s": None,
          "firing_state_samples": [], "post": None, "window_s": window_s}
    t0 = clock()
    ev["reset_result"] = str(_safe(board.reset_pico, "reset raised"))
    ev["t_reset_call_s"] = clock() - t0
    if emit:
        emit(f"debug_reset(pico) returned after {ev['t_reset_call_s']:.2f}s: "
             f"{ev['reset_result'][:120]}")
    pre_boot = (ev["pre"]["pico_version"] or {}).get("boot_id")
    while clock() - t0 < window_s:
        t = clock() - t0
        link = _safe(board.link)
        down = link_is_down(link)
        if down and ev["t_link_down_s"] is None:
            ev["t_link_down_s"] = t
            if emit:
                emit(f"  t+{t:6.2f}s link DOWN {link}")
        if down and ev["link_down_fault"] is None:
            fa = link.get("fault_asserted")
            ev["link_down_fault"] = fa if isinstance(fa, bool) else "unreadable"
        ex = _safe(board.exec_status)
        if len(ev["firing_state_samples"]) < 600:
            ev["firing_state_samples"].append([round(t, 2), (ex or {}).get("state")])
        if ev["t_link_down_s"] is not None and not down and link and "error" not in link:
            if ev["t_link_recovered_s"] is None:
                ev["t_link_recovered_s"] = t
                if emit:
                    emit(f"  t+{t:6.2f}s link UP again")
            v = _safe(board.pico_version)
            if v and v.get("known") and v.get("boot_id") != pre_boot:
                ev["t_version_back_s"] = t
                if emit:
                    emit(f"  t+{t:6.2f}s Pico fw_version back: {v}")
                break
        sleep(poll_s)
    ev["post"] = snapshot(board)
    return ev


def judge(ev):
    """Pure verdict. Returns (ok, report_lines, checks)."""
    checks = {}
    pre, post = ev["pre"], ev["post"] or {}
    checks["E1_link_down_seen"] = ev["t_link_down_s"] is not None
    fa = ev["link_down_fault"]
    if fa is True:
        checks["E2_heat_blocked_while_down"] = True
    elif fa is False:
        checks["E2_heat_blocked_while_down"] = False
    else:
        checks["E2_heat_blocked_while_down"] = None  # not observable
    checks["E3_link_and_version_back"] = (
        ev["t_link_recovered_s"] is not None and ev["t_version_back_s"] is not None)
    down_s = None
    if ev["t_link_down_s"] is not None:
        end = ev["t_link_recovered_s"] if ev["t_link_recovered_s"] is not None else ev["window_s"]
        down_s = end - ev["t_link_down_s"]
    states = [s for _, s in ev["firing_state_samples"]]
    final_state = (post.get("exec") or {}).get("state")
    checks["E4_firing_survived"] = (
        down_s is not None and down_s < ABORT_SILENCE_S
        and final_state == PROFILE_EXEC_RUNNING
        and PROFILE_EXEC_FAULTED not in states)
    pre_trip = (pre.get("counters") or {}).get("trip_reason")
    post_trip = (post.get("counters") or {}).get("trip_reason")
    checks["E5_no_new_trip"] = pre_trip is not None and pre_trip == post_trip
    out = [f"link down at t+{ev['t_link_down_s']}s, recovered t+{ev['t_link_recovered_s']}s, "
           f"version back t+{ev['t_version_back_s']}s (down {down_s}s)",
           f"fault line asserted while link down: {fa}",
           f"firing state final={final_state} faulted_seen={PROFILE_EXEC_FAULTED in states}",
           f"trip_reason pre={pre_trip} post={post_trip}"]
    for k, v in checks.items():
        out.append(f"  {k}: " + ("n/a (not observable)" if v is None else "PASS" if v else "FAIL"))
    ok = all(v is not False for v in checks.values())
    out.append("PASS" if ok else "FAIL")
    return ok, out, checks


def main(argv=None, board=None, clock=time.time, sleep=time.sleep) -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--window", type=float, default=DEFAULT_WINDOW_S)
    ap.add_argument("--poll-period", type=float, default=DEFAULT_POLL_S)
    ap.add_argument("--out", default=None, help="write JSON evidence here")
    ap.add_argument("--i-am-rebooting-the-pico-mid-firing", action="store_true",
                    help="Required. States you intend to reboot the safety "
                    "processor during the firing running on the board.")
    args = ap.parse_args(argv)
    if not args.i_am_rebooting_the_pico_mid_firing:
        print("REFUSED: pass --i-am-rebooting-the-pico-mid-firing to confirm you "
              "intend to reboot the Pico under a live firing. Nothing was sent.")
        return 2
    board = board or RealBoard()
    print(board.connect())
    st = _safe(board.exec_status)
    if not st or st.get("state") != PROFILE_EXEC_RUNNING:
        print(f"REFUSED: profile exec status {st} is not RUNNING; this script "
              "only exercises a real running firing. Nothing was reset.")
        return 2
    ev = run_reboot(board, window_s=args.window, poll_s=args.poll_period,
                    clock=clock, sleep=sleep, emit=print)
    ok, report, checks = judge(ev)
    ev["checks"] = checks
    ev["ok"] = ok
    for line in report:
        print(line)
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(ev, fh, indent=2, default=str)
    print("Do not auto-clear any trip; see docs/MCP_SERVERS.md flash section.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
