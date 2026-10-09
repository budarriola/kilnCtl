#!/usr/bin/env python3
"""Bench regression check for the ESP PC link: every bridge answers, and fast.

Encodes the measurements taken on 2026-08-20 while root-causing the board's
long-unexplained spontaneous reboot, so they are repeatable instead of being
one-off numbers in a session log. Two classes of bug hide from a functional
test and are caught here:

  1. A bridge task that stops answering. All 12 UART tasks failing to register
     was a real, recurring bug (internal-DRAM exhaustion at task creation).
  2. A bridge task that answers but slowly. WIFI_CMD_GET_NETWORKS ran a full
     blocking radio scan on every call -- 2.7s against 0.2s for everything
     else -- which made *unrelated* queries look randomly slow, because they
     queued behind it. That is why this measures per command rather than in
     aggregate: the aggregate view is what disguised it as "intermittent link
     congestion" for days.

Read-only. Nothing here commands relays, starts a firing, writes NVS, or
changes Wi-Fi state, so it is safe to run against a board on the bench.

Exit status is 0 if every surface answers within its budget, 1 otherwise, so
this can gate a bench session.

Usage:
    uv run --project tools/PcTools python tools/PcTools/scripts/bench_link_health.py
    ... --repeat 20        # more samples per command
    ... --soak 900         # then watch that long, in seconds, for a reset
"""
from __future__ import annotations

import argparse
import re
import statistics
import sys
import time

from kilnctrl import mcp_server as m

# Per-command median budget in seconds.
#
# 0.6 rather than the ~0.2 actually measured: the point is to catch a command
# that has become seconds-slow, not to fail the run over normal jitter on a
# shared link. wifi_get_networks gets 1.5 because its scan cache is cold on
# the first call of a fresh process -- a real, expected ~2.6s hit that says
# nothing about health. Tighten these only with measurements to back it up.
CHECKS: list[tuple[str, str, float]] = [
    ("INFO", "get_fw_version", 0.6),
    ("IO", "io_read", 0.6),
    ("THERMO", "thermo_read", 0.6),
    ("THERMO", "thermo_read_faults", 0.6),
    ("SAFETY", "safety_get_status", 0.6),
    ("SAFETY", "safety_get_link_stats", 0.6),
    ("CONTROL", "control_get_zones", 0.6),
    # 1.4s, not the 0.6s the other single-round-trip queries get: since the 28
    # shipped schedules landed (2026-08-20), a full listing is 36 summaries
    # against a 253-byte UART_PROTO_MAX_PAYLOAD, so the client pages it in
    # three round trips instead of one. Measured 0.601s median / 1.0s max on
    # the bench right after that change. Raised deliberately with the reason
    # recorded -- if this ever exceeds 1.4s, something has genuinely regressed
    # rather than the budget being stale again.
    ("PROFILES", "profiles_list", 1.4),
    ("PROFILES", "profiles_get_exec_status", 0.6),
    ("AUTOTUNE", "autotune_get_status", 0.6),
    ("WIFI", "wifi_get_status", 0.6),
    ("WIFI", "wifi_get_networks", 1.5),
    ("TOUCH", "touch_get_state", 1.0),
    ("OTA", "ota_status", 1.5),
]

_TS = re.compile(r"\((\d+)\)")


def _device_uptime_ms() -> int | None:
    """Newest log timestamp, which tracks the device's uptime closely enough
    that a decrease means it reset. Cheaper and less invasive than rebooting
    to read a counter, and it works with the link already open."""
    try:
        vals = [int(x) for x in _TS.findall(m.get_device_log(6))]
    except Exception:  # noqa: BLE001
        return None
    return max(vals) if vals else None


def _call(fn) -> tuple[bool, float]:
    t0 = time.time()
    try:
        r = fn()
    except Exception:  # noqa: BLE001
        return False, time.time() - t0
    dt = time.time() - t0
    ok = not (isinstance(r, str) and r.lower().startswith("error"))
    return ok, dt


def run_checks(repeat: int) -> int:
    failures = 0
    print(f"{'surface':<9} {'command':<26} {'fails':>5} {'med':>7} {'max':>7}  verdict")
    for surface, name, budget in CHECKS:
        fn = getattr(m, name, None)
        if fn is None:
            # A renamed/removed MCP call is a real finding, not a reason to
            # crash mid-run -- report it and keep going.
            print(f"{surface:<9} {name:<26} {'-':>5} {'-':>7} {'-':>7}  MISSING from mcp_server")
            failures += 1
            continue

        fails, times = 0, []
        for _ in range(repeat):
            ok, dt = _call(fn)
            times.append(dt)
            if not ok:
                fails += 1

        med, mx = statistics.median(times), max(times)
        bad = fails > 0 or med > budget
        failures += bool(bad)
        verdict = "ok" if not bad else (f"FAIL {fails} error(s)" if fails else f"SLOW > {budget:.1f}s")
        print(f"{surface:<9} {name:<26} {fails:>5} {med:>7.3f} {mx:>7.3f}  {verdict}")
    return failures


def run_soak(seconds: float) -> int:
    """Drive the link continuously and watch for a reset.

    The reboot this script exists to guard against took ~450s of uptime to
    appear and fired while polling wifi_get_status, so a soak needs to both
    take real time and keep the Wi-Fi bridge busy -- a quiet idle wait would
    not have caught it.
    """
    print(f"\nsoak: {int(seconds)}s, watching for a device reset")
    drivers = [
        getattr(m, n)
        for n in ("wifi_get_status", "get_fw_version", "io_read", "thermo_read", "wifi_get_networks")
        if getattr(m, n, None)
    ]
    start = time.time()
    prev = _device_uptime_ms()
    cycles = resets = errors = 0
    while time.time() - start < seconds:
        cycles += 1
        for fn in drivers:
            ok, _ = _call(fn)
            if not ok:
                errors += 1
        now = _device_uptime_ms()
        if prev is not None and now is not None and now < prev:
            resets += 1
            print(f"  *** RESET: device uptime {prev} -> {now} ms")
            print(m.get_device_log(12))
        prev = now
        print(f"  cycle {cycles:<4} elapsed {int(time.time() - start):>4}s  uptime {now}ms  "
              f"errors={errors} resets={resets}")
        time.sleep(20)
    print(f"soak done: cycles={cycles} errors={errors} resets={resets}")
    return resets + errors


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--repeat", type=int, default=10, help="samples per command (default 10)")
    ap.add_argument("--soak", type=float, default=0.0, help="after the checks, soak this many seconds")
    args = ap.parse_args()

    print(m.connect())

    failures = run_checks(args.repeat)
    if args.soak > 0:
        failures += run_soak(args.soak)

    if failures:
        print(f"\n{failures} problem(s) found")
        return 1
    print("\nall surfaces healthy")
    return 0


if __name__ == "__main__":
    sys.exit(main())
