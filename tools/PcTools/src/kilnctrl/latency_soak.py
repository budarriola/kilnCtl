#!/usr/bin/env python3
"""latency_soak.py -- READ-ONLY per-surface HTTP latency soak. Polls a fixed
set of GET routes (one per UI/firmware surface) every few seconds for a
configurable duration, timing each request, then reports per-surface
p50/p95/max/errors and any "clustered stall" ticks (see latency_soak_stats.py).

Written for the one-off 2026-08 multi-surface stall that was never reproduced
(KilnFW TODO): leave it running while the bench does ordinary work and read the
clustered-stall list afterwards.

Every surface request is a GET with no body. The one non-GET the soak can cause
is http_auth.urlopen's own login (POST /api/auth/login, from KILNCTL_WEB_USERNAME
/ KILNCTL_WEB_PASSWORD) when an admin-tier route answers 401 -- once in the
unrecorded warm-up, and again only if the session expires mid-soak (that one
sample then includes the login round trip). Credentials are never printed here;
a failed login is recorded as an HTTP 401 error, not a stall, since the board
did answer. Each tick
polls the surfaces sequentially (so the soak itself adds at most one in-flight
request to the board), and the tick is the clustering window.

CLI: python -m kilnctrl.latency_soak --host 192.168.1.156 --duration-s 1800
"""
from __future__ import annotations

import argparse
import time
import urllib.error
import urllib.request
from typing import Callable, List, Optional, Sequence, Tuple

from . import http_auth
from .latency_soak_stats import Sample, format_report

#: surface name -> GET path. Tiers per firmware/KilnFW/App/drivers/http/route_tier_table.h.
SURFACES: "Tuple[Tuple[str, str], ...]" = (
    ("status", "/api/status"),               # OPEN: dashboard
    ("profile_exec", "/api/profile_exec"),   # OPEN: executor state
    ("readiness", "/api/readiness"),         # OPEN
    ("ota_status", "/api/ota/esp/status"),   # ADMIN: OTA/partition state
    ("profiles", "/api/profiles"),           # USER: profile list
    ("zones", "/api/zones"),                 # ADMIN
    ("control", "/api/control"),             # ADMIN
    ("autotune", "/api/autotune"),           # ADMIN
    ("network", "/api/debug/lwip_stats"),    # ADMIN: network stack counters
    ("display", "/api/settings/display_power"),  # ADMIN: LCD power/brightness read
)

DEFAULT_DURATION_S = 1800.0
DEFAULT_INTERVAL_S = 5.0
DEFAULT_TIMEOUT_S = 8.0
DEFAULT_STALL_MS = 1000.0
DEFAULT_MIN_SURFACES = 3

#: fetch(host, path, timeout) -> (latency_ms, status or None, error text)
Fetch = Callable[[str, str, float], "Tuple[float, Optional[int], str]"]


class SoakAborted(RuntimeError):
    """The soak refused to start (see :func:`run_soak`)."""


def real_fetch(host: str, path: str, timeout: float) -> "Tuple[float, Optional[int], str]":
    req = urllib.request.Request(f"http://{host}{path}", method="GET")
    t0 = time.perf_counter()
    status: Optional[int] = None
    err = ""
    try:
        with http_auth.urlopen(req, timeout=timeout) as resp:
            resp.read()
            status = resp.status
    except urllib.error.HTTPError as exc:
        status, err = exc.code, f"HTTP {exc.code}"
    except http_auth.HttpAuthError:
        # The board answered 401 and the login could not answer it (no
        # credential in the env, or login refused/failed). A fast answer is an
        # error, not a stall; a slow one still trips the latency threshold.
        # The exception text is deliberately not recorded.
        status, err = 401, "HTTP 401 (login failed)"
    except Exception as exc:  # noqa: BLE001 - timeout/unreachable/auth-missing
        err = type(exc).__name__
    return (time.perf_counter() - t0) * 1000.0, status, err


def run_soak(host: str, duration_s: float = DEFAULT_DURATION_S,
             interval_s: float = DEFAULT_INTERVAL_S, timeout_s: float = DEFAULT_TIMEOUT_S,
             surfaces: Sequence[Tuple[str, str]] = SURFACES, fetch: Fetch = real_fetch,
             clock: Callable[[], float] = time.monotonic,
             sleep: Callable[[float], None] = time.sleep) -> "List[Sample]":
    """Poll every surface once per tick until ``duration_s`` elapses (always at
    least one tick). Returns the samples; clock/sleep/fetch are injectable.
    Raises :class:`SoakAborted` if a warm-up request still answers 401."""
    samples: List[Sample] = []
    # Warm-up (not recorded): one pass over every surface, so http_auth logs in
    # on the first admin-tier 401 here and the login round trip does not
    # pollute tick 0's latency.
    for name, path in surfaces:
        if fetch(host, path, timeout_s)[1] == 401:
            # Every later tick would retry the login once per admin surface
            # and could walk the board into its failed-login lockout.
            raise SoakAborted(f"warm-up GET {path} ({name}) answered 401 and the login did not "
                              f"fix it; check that KILNCTL_WEB_USERNAME/KILNCTL_WEB_PASSWORD are "
                              f"set and valid. Not soaking.")
    start = clock()
    tick = 0
    while True:
        tick_start = clock()
        for name, path in surfaces:
            lat, status, err = fetch(host, path, timeout_s)
            ok = status is not None and 200 <= status < 300
            samples.append(Sample(tick, name, lat, ok, status, err))
        tick += 1
        if clock() - start >= duration_s:
            break
        sleep(max(0.0, interval_s - (clock() - tick_start)))
    return samples


def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description="READ-ONLY per-surface HTTP latency soak")
    ap.add_argument("--host", required=True)
    ap.add_argument("--duration-s", type=float, default=DEFAULT_DURATION_S)
    ap.add_argument("--interval-s", type=float, default=DEFAULT_INTERVAL_S)
    ap.add_argument("--timeout-s", type=float, default=DEFAULT_TIMEOUT_S)
    ap.add_argument("--stall-ms", type=float, default=DEFAULT_STALL_MS)
    ap.add_argument("--min-surfaces", type=int, default=DEFAULT_MIN_SURFACES)
    a = ap.parse_args(argv)
    try:
        samples = run_soak(a.host, a.duration_s, a.interval_s, a.timeout_s)
    except SoakAborted as exc:
        print(f"error: {exc}")
        return 2
    print(format_report(samples, a.stall_ms, a.min_surfaces, f"latency soak host={a.host} (GET only)"))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
