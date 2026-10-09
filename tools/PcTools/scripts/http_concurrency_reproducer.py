#!/usr/bin/env python3
"""Reproduce and characterize ROADMAP.md's "HTTP connection resets under
concurrency" item (~line 1664).

Drives N concurrent HTTP requests at the board's web server and records, per
burst: per-request outcome (ok / reset / timeout / other error), the
`/api/debug/lwip_stats` TCP-layer counters sampled immediately before and
after the burst, and `/api/status`'s internal-heap fields (free,
largest_free_block, min_free) sampled the same way. This does not repeat the
two dead ends already recorded in the ROADMAP item or diagnostics_http.c's
own comment (bare-printf logging bypass, ESP_LOG_DEBUG stripped by
CONFIG_LOG_MAXIMUM_LEVEL) -- `/api/debug/lwip_stats` already sidesteps both.

Read-only against application state: every request here is a GET against a
static/JSON endpoint. Nothing here commands relays, starts a firing, writes
NVS, or changes Wi-Fi state -- safe to run against a board that is IDLE.

Usage:
    uv run --project tools/PcTools python tools/PcTools/scripts/http_concurrency_reproducer.py
    ... --host 192.168.1.156           # default; board's current DHCP/static IP
    ... --concurrency 4,8,12,16,20,28   # comma list of burst widths to sweep
    ... --bursts 5                      # bursts per concurrency level
    ... --path /app.js                  # request target (the original 9/80 repro path)
    ... --timeout 5                     # per-request socket timeout, seconds

Exit status is 0 if it ran to completion (regardless of whether resets were
found -- a clean run is a real result, not a test failure), 1 on a driver
error (board unreachable at all, etc).
"""
from __future__ import annotations

import argparse
import json
import statistics
import sys
import time
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass, field


def _get_json(url: str, timeout: float) -> dict:
    with urllib.request.urlopen(url, timeout=timeout) as resp:  # noqa: S310
        return json.loads(resp.read().decode("utf-8"))


def lwip_stats(host: str, timeout: float) -> dict | None:
    try:
        d = _get_json(f"http://{host}/api/debug/lwip_stats", timeout)
        return d.get("tcp") if d.get("ok") else None
    except Exception:  # noqa: BLE001
        return None


def heap_snapshot(host: str, timeout: float) -> dict | None:
    try:
        d = _get_json(f"http://{host}/api/status", timeout)
        return d.get("heap_internal")
    except Exception:  # noqa: BLE001
        return None


@dataclass
class RequestResult:
    ok: bool
    outcome: str  # "ok" | "reset" | "timeout" | "http_error" | "other"
    elapsed: float
    detail: str = ""


def _fetch_one(host: str, path: str, timeout: float) -> RequestResult:
    url = f"http://{host}{path}"
    t0 = time.time()
    try:
        # This server keeps HTML/JS pages only as a gzip-compressed blob
        # (web_encoding.c) and answers a request that does not advertise
        # gzip support with a real 406, not a transport-layer failure --
        # urllib sends no Accept-Encoding by default, so every request was
        # getting a 406 before this header was added (found while building
        # this script: 100% "failure" at concurrency=1, which is not a
        # concurrency effect at all). See diagnostics_http.c/web_encoding.c.
        req = urllib.request.Request(url, method="GET", headers={"Accept-Encoding": "gzip"})
        with urllib.request.urlopen(req, timeout=timeout) as resp:  # noqa: S310
            resp.read()
            dt = time.time() - t0
            return RequestResult(True, "ok", dt)
    except urllib.error.HTTPError as e:
        dt = time.time() - t0
        return RequestResult(False, "http_error", dt, f"HTTP {e.code}")
    except TimeoutError:
        dt = time.time() - t0
        return RequestResult(False, "timeout", dt)
    except (ConnectionResetError, ConnectionAbortedError, BrokenPipeError) as e:
        dt = time.time() - t0
        return RequestResult(False, "reset", dt, str(e))
    except OSError as e:
        # Covers urllib wrapping a reset/refused as a generic OSError/URLError
        # on Windows (WinError 10054 == connection reset by peer).
        dt = time.time() - t0
        msg = str(e)
        outcome = "reset" if ("10054" in msg or "reset" in msg.lower()) else "other"
        return RequestResult(False, outcome, dt, msg)
    except Exception as e:  # noqa: BLE001
        dt = time.time() - t0
        return RequestResult(False, "other", dt, repr(e))


@dataclass
class BurstResult:
    concurrency: int
    results: list[RequestResult] = field(default_factory=list)
    tcp_before: dict | None = None
    tcp_after: dict | None = None
    heap_before: dict | None = None
    heap_after: dict | None = None

    @property
    def outcomes(self) -> dict[str, int]:
        counts: dict[str, int] = {}
        for r in self.results:
            counts[r.outcome] = counts.get(r.outcome, 0) + 1
        return counts

    def tcp_delta(self) -> dict[str, int]:
        if not (self.tcp_before and self.tcp_after):
            return {}
        return {k: self.tcp_after[k] - self.tcp_before.get(k, 0) for k in self.tcp_after}


def run_burst(host: str, path: str, concurrency: int, timeout: float) -> BurstResult:
    br = BurstResult(concurrency=concurrency)
    br.tcp_before = lwip_stats(host, timeout)
    br.heap_before = heap_snapshot(host, timeout)

    with ThreadPoolExecutor(max_workers=concurrency) as pool:
        futures = [pool.submit(_fetch_one, host, path, timeout) for _ in range(concurrency)]
        for fut in as_completed(futures):
            br.results.append(fut.result())

    br.tcp_after = lwip_stats(host, timeout)
    br.heap_after = heap_snapshot(host, timeout)
    return br


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="192.168.1.156", help="board IP or host:port (default 192.168.1.156)")
    ap.add_argument("--concurrency", default="4,8,12,16,20,28",
                     help="comma-separated list of concurrent-request counts to sweep")
    ap.add_argument("--bursts", type=int, default=5, help="bursts per concurrency level (default 5)")
    ap.add_argument("--path", default="/app.js", help="request target (default /app.js, the original repro)")
    ap.add_argument("--timeout", type=float, default=5.0, help="per-request socket timeout, seconds")
    ap.add_argument("--interburst-sleep", type=float, default=1.0,
                     help="seconds to rest between bursts (default 1.0)")
    args = ap.parse_args()

    host = args.host
    levels = [int(x) for x in args.concurrency.split(",") if x.strip()]

    # Sanity: board reachable at all before sweeping.
    try:
        first = lwip_stats(host, args.timeout)
        if first is None:
            print(f"WARNING: /api/debug/lwip_stats not reachable/enabled on {host} "
                  f"-- counters will be blank for this run", file=sys.stderr)
    except Exception as e:  # noqa: BLE001
        print(f"ERROR: cannot reach {host}: {e!r}", file=sys.stderr)
        return 1

    print(f"target: http://{host}{args.path}  levels={levels}  bursts/level={args.bursts}  "
          f"timeout={args.timeout}s")
    print(f"{'conc':>5} {'burst':>5} {'ok':>4} {'reset':>5} {'timeout':>7} {'other':>5} "
          f"{'med_s':>7} {'max_s':>7}  tcp_delta  heap_free_before->after")

    total_requests = total_ok = total_reset = total_timeout = total_other = 0
    interesting: list[BurstResult] = []

    for conc in levels:
        for b in range(args.bursts):
            br = run_burst(host, args.path, conc, args.timeout)
            oc = br.outcomes
            ok = oc.get("ok", 0)
            reset = oc.get("reset", 0)
            timeout_n = oc.get("timeout", 0)
            other = sum(v for k, v in oc.items() if k not in ("ok", "reset", "timeout"))
            times = [r.elapsed for r in br.results]
            med = statistics.median(times) if times else 0.0
            mx = max(times) if times else 0.0

            total_requests += len(br.results)
            total_ok += ok
            total_reset += reset
            total_timeout += timeout_n
            total_other += other

            delta = br.tcp_delta()
            delta_str = ",".join(f"{k}+{v}" for k, v in delta.items() if v) or "none"
            hb = br.heap_before.get("free") if br.heap_before else None
            ha = br.heap_after.get("free") if br.heap_after else None

            print(f"{conc:>5} {b:>5} {ok:>4} {reset:>5} {timeout_n:>7} {other:>5} "
                  f"{med:>7.3f} {mx:>7.3f}  {delta_str}  {hb}->{ha}")

            if reset or timeout_n or other:
                interesting.append(br)
                # Print the failing requests' detail immediately -- do not make
                # the reader hunt for them after the fact.
                for r in br.results:
                    if not r.ok:
                        print(f"      FAIL outcome={r.outcome} elapsed={r.elapsed:.3f}s detail={r.detail!r}")

            time.sleep(args.interburst_sleep)

    print()
    print(f"total requests: {total_requests}  ok={total_ok}  reset={total_reset}  "
          f"timeout={total_timeout}  other={total_other}")
    if total_requests:
        fail_rate = (total_reset + total_timeout + total_other) / total_requests
        print(f"failure rate: {fail_rate:.1%}")
    if interesting:
        print(f"\n{len(interesting)} burst(s) with a non-ok outcome -- see FAIL lines above and the "
              f"tcp_delta column for whether lwip_stats.tcp moved with them.")
    else:
        print("\nno failures reproduced at any level tried -- record the exact concurrency/bursts/path "
              "tried in ROADMAP.md rather than treating this as closure.")

    return 0


if __name__ == "__main__":
    sys.exit(main())
