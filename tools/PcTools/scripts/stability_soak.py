#!/usr/bin/env python3
"""stability_soak.py -- read-only stability soak for the kilnCtl board pair.

Defines and measures "stable" (docs/audits/stability_definition_2026-09-07.md)
concretely enough that the owner's stated sequence -- current sensor
commissioning, THEN confirm stability, THEN start filesystem work -- has an
actual evidence trail instead of a word with no threshold behind it.

Metrics sampled every cycle, appended to a CSV, all via the UART link
(kilnctrl.mcp_server's already-connected clients) and one HTTP GET per
cycle (dashboard_http_client.get_heap_status, which also carries
reset_reason/uptime_s/unacknowledged_crash through -- see that module's
2026-08-31-incident comment for why those three ride along instead of being
a separate call nobody remembers to make):

  * unacknowledged_crash / reset_reason      -- CLAUDE.md's crash-report gap
  * heap_internal free / min_free            -- 11.9 kB free is the
    documented HTTP-socket-reset failure threshold (docs/bench_snapshots/
    2026-09-04.md, docs/FILESYSTEM_PLAN.md); this script's floor is set with
    headroom above that measured failure point, not a guess
  * per-task stack margin (level, hwm_bytes) -- firmware's own
    StackMarginLevel classification against each task's *configured* size
    doubles as "above its registered minimum"; see the module docstring on
    StackMarginLevel (protocol.py) for why LOW/CRITICAL is already the
    threshold, not an arbitrary percentage invented here. NOTE: the
    idle-baseline finding (project_idle_stack_baseline_is_a_floor) applies
    here too -- a soak run with no firing active cannot exercise every
    task's deep path, so OK-at-idle is a floor, not proof a firing-time
    stack margin is also fine. Run one soak with a firing active for real
    coverage (see --firing-in-progress below).
  * safety link stats (crc_errors, timeouts, broadcast_dropped)  -- flat
    counters, not the absolute values, are the pass criterion: this link
    has never been observed reset these to zero. `timeouts` was redefined
    2026-09-10 (docs/audits/safety_link_get_status_timeout_counter_2026-09-10.md
    and its follow-up review): it no longer counts per-GET_STATUS-exchange
    misses (which could climb on a perfectly healthy link, a known false-
    positive risk this script's flat-not-absolute criterion was already
    written defensively against) -- it now counts ~500 ms poll iterations
    that saw zero new STATUS frames applied anywhere, which stays at zero
    on a healthy link and only climbs under real partial loss. The "flat is
    healthy" assumption below is unchanged and, if anything, more reliable
    now than before this redefinition.
  * safety GET_STATUS / GET_DIAG (warn_mask, trip_mask, link_up, boot_id)
    -- any NEW bit vs. the first sample's baseline is unexpected; a bit
    already set at the first sample (expected: S5, no safety thermocouple
    fitted per CLAUDE.md) is not re-flagged each cycle
  * ESP fw identity (commit/built/dirty) and Pico fw identity (commit/
    built/boot_id) -- an unexplained change mid-soak means a board rebooted
    onto a different build, which invalidates every OK reading before it
  * profiles_get_exec_status -- both to detect a firing (see --firing-*
    below) and because a full ProfileExecStatus/ThermoFault decode already
    happens on nearly every board interaction in this tree; reported as
    fault_guard trend rather than assumed clean

STRICTLY READ-ONLY: every call here is a GET_* / GET_STATUS-shaped query.
This script contains no relay write, no config write, no reset, no OTA, and
no firing control call -- grep it yourself before trusting that claim rather
than taking the docstring's word for it.

Firing-in-progress handling: by default this script REFUSES to start if
profiles_get_exec_status() reports RUNNING/PAUSED, because a soak taken
during a firing and one taken idle answer different questions (idle
stability says nothing about behaviour under thermal/PWM load, which is
where several of the bugs in CLAUDE.md's failure history actually lived --
PWM chopping disarming guards, the httpd-stack near-overflow under real
load, the DRAM exhaustion pattern). Pass --firing-in-progress to soak
*during* a firing deliberately; the CSV and the final report both say which
mode a run was.

Usage:
    uv run --project tools/PcTools python tools/PcTools/scripts/stability_soak.py \\
        --duration 3600 --interval 60
    ... --firing-in-progress     # soak while a profile is actively running
    ... --out my_soak.csv        # default: stability_soak_<timestamp>.csv
    ... --host 192.168.1.42      # dashboard HTTP host; default: autodetect

Exit status: 0 on PASS, 1 on FAIL, 2 on a setup/connection error.
"""
from __future__ import annotations

import argparse
import csv
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))

from kilnctrl import mcp_server as m  # noqa: E402
from kilnctrl import dashboard_http_client  # noqa: E402
from kilnctrl.mcp_server_ota import _ota_resolve_host  # noqa: E402
from kilnctrl.protocol import StackMarginLevel  # noqa: E402

# 11.9 kB free heap_internal is the measured, documented failure point where
# HTTP sockets started resetting (docs/bench_snapshots/2026-09-04.md,
# docs/FILESYSTEM_PLAN.md). This floor sits at 3x that with a round number,
# not at the failure point itself -- the goal is a working margin, not a
# threshold that only fires after the board has already started misbehaving.
HEAP_INTERNAL_FLOOR_BYTES = 36_000

# A downward run this long, sampled every --interval, is treated as "trending
# down" rather than sampling noise. Conservative on purpose: this is a NEW
# check with no false-positive history yet to tune against (see
# feedback_negative_test_every_check.md -- prove it can fail, then prove it
# does not cry wolf on noise).
TREND_MIN_SAMPLES = 5

# Trend thresholds, expressed as a least-squares slope in units per HOUR --
# not a first-vs-last endpoint comparison (that shape is blind to a monotone
# drift that happens to return near its starting value between two noisy
# endpoint samples, and is dominated by noise in those two samples). A run's
# fitted slope must exceed one of these magnitudes to be called DOWN/UP; a
# smaller slope is flat/noise. Conservative starting points, same rationale
# as TREND_MIN_SAMPLES above -- tune against real run history once it exists.
HEAP_TREND_FLOOR_BYTES_PER_HOUR = 2_000.0
STACK_PCT_TREND_FLOOR_PP_PER_HOUR = 2.0

CSV_FIELDS = [
    "t_s", "wall_time",
    "esp_reset_reason", "esp_uptime_s", "unacknowledged_crash",
    "heap_internal_free", "heap_internal_min_free",
    "esp_commit", "esp_built", "esp_dirty",
    "stack_worst_level", "stack_worst_task", "stack_min_headroom_pct",
    "link_up", "link_age_ms", "warn_mask", "trip_mask",
    "crc_errors", "timeouts", "broadcast_dropped", "frames_sent",
    "pico_commit", "pico_built", "pico_boot_id",
    "exec_state", "fault_guard",
    "http_ok", "http_latency_ms",
]


@dataclass
class Baseline:
    esp_build: "tuple[str, str, bool] | None" = None
    pico_build: "tuple[str, str] | None" = None
    pico_boot_id: "int | None" = None
    warn_mask: "int | None" = None
    trip_mask: "int | None" = None


@dataclass
class Sample:
    row: dict
    problems: list = field(default_factory=list)


def _is_firing(exec_status) -> bool:
    return exec_status.state in (1, 2)  # running, paused


def sample_once(host: str, baseline: Baseline, t0: float) -> Sample:
    problems: list[str] = []
    row: dict = {"t_s": round(time.monotonic() - t0, 1), "wall_time": time.strftime("%Y-%m-%dT%H:%M:%S")}

    # --- HTTP: heap / reset_reason / crash report ---
    http_start = time.monotonic()
    try:
        heap = dashboard_http_client.get_heap_status(host)
        row["http_ok"] = 1
        row["http_latency_ms"] = round((time.monotonic() - http_start) * 1000, 1)
        row["esp_reset_reason"] = heap.get("reset_reason")
        row["esp_uptime_s"] = heap.get("uptime_s")
        crash = heap.get("unacknowledged_crash")
        row["unacknowledged_crash"] = 1 if crash else 0
        if crash:
            problems.append(
                f"UNACKNOWLEDGED CRASH: exc_task={crash.get('exc_task')!r} "
                f"cause={crash.get('exc_cause_str')!r}"
            )
        if heap.get("reset_reason") in dashboard_http_client.UNCLEAN_RESET_REASONS:
            problems.append(f"unclean reset_reason={heap.get('reset_reason')!r}")
        hi = heap["heap_internal"]
        row["heap_internal_free"] = hi["free"]
        row["heap_internal_min_free"] = hi["min_free"]
        if hi["free"] < HEAP_INTERNAL_FLOOR_BYTES:
            problems.append(f"heap_internal.free={hi['free']} B below floor {HEAP_INTERNAL_FLOOR_BYTES} B")
    except dashboard_http_client.DashboardHttpError as exc:
        row["http_ok"] = 0
        row["http_latency_ms"] = round((time.monotonic() - http_start) * 1000, 1)
        for k in ("esp_reset_reason", "esp_uptime_s", "unacknowledged_crash",
                   "heap_internal_free", "heap_internal_min_free"):
            row[k] = ""
        problems.append(f"HTTP /api/status failed: {exc}")

    # --- ESP build identity (UART, cheap query already needed for compat check) ---
    try:
        fw = m._info.get_fw_version()
        row["esp_commit"] = fw.commit
        row["esp_built"] = fw.built
        row["esp_dirty"] = int(fw.dirty)
        build = (fw.commit, fw.built, fw.dirty)
        if baseline.esp_build is None:
            baseline.esp_build = build
        elif build != baseline.esp_build:
            problems.append(f"ESP build changed mid-soak: {baseline.esp_build} -> {build} (unexpected reboot onto different firmware)")
    except Exception as exc:  # noqa: BLE001
        row["esp_commit"] = row["esp_built"] = row["esp_dirty"] = ""
        problems.append(f"get_fw_version failed: {exc}")

    # --- Stack margin (UART) ---
    try:
        entries = m._info.get_stack_margin()
        worst_level = StackMarginLevel.OK
        worst_task = ""
        min_pct = None
        for e in entries:
            if not e.alive:
                continue
            if int(e.level) > int(worst_level):
                worst_level = e.level
                worst_task = e.name
            pct = e.headroom_pct
            if pct is not None and (min_pct is None or pct < min_pct):
                min_pct = pct
        row["stack_worst_level"] = worst_level.name
        row["stack_worst_task"] = worst_task
        row["stack_min_headroom_pct"] = round(min_pct, 1) if min_pct is not None else ""
        if worst_level != StackMarginLevel.OK:
            problems.append(f"stack margin {worst_level.name} on task {worst_task!r}")
    except Exception as exc:  # noqa: BLE001
        row["stack_worst_level"] = row["stack_worst_task"] = row["stack_min_headroom_pct"] = ""
        problems.append(f"get_stack_margin failed: {exc}")

    # --- Safety link (UART) ---
    try:
        status = m._safety.get_status()
        link_stats = m._safety.get_link_stats()
        diag = m._safety.get_diag()
        row["link_up"] = int(status.link_up)
        row["link_age_ms"] = status.age_ms
        row["crc_errors"] = link_stats.crc_errors
        row["timeouts"] = link_stats.timeouts
        row["broadcast_dropped"] = link_stats.broadcast_dropped
        row["frames_sent"] = link_stats.frames_sent
        row["warn_mask"] = diag.warn_mask if diag.ever_received else ""
        row["trip_mask"] = diag.trip_mask if diag.ever_received else ""
        if diag.ever_received:
            if baseline.warn_mask is None:
                baseline.warn_mask = diag.warn_mask
            new_warn = diag.warn_mask & ~baseline.warn_mask
            if new_warn:
                problems.append(f"new safety warn bits since baseline: 0x{new_warn:02X}")
            if baseline.trip_mask is None:
                baseline.trip_mask = diag.trip_mask
            new_trip = diag.trip_mask & ~baseline.trip_mask
            if new_trip:
                problems.append(f"new safety trip bits since baseline: 0x{new_trip:02X}")
        try:
            pfw = m._safety.get_fw_version()
            row["pico_commit"] = pfw.commit
            row["pico_built"] = pfw.built
            row["pico_boot_id"] = pfw.boot_id
            pbuild = (pfw.commit, pfw.built)
            if baseline.pico_build is None:
                baseline.pico_build = pbuild
                baseline.pico_boot_id = pfw.boot_id
            else:
                if pbuild != baseline.pico_build:
                    problems.append(f"Pico build changed mid-soak: {baseline.pico_build} -> {pbuild}")
                if pfw.boot_id != baseline.pico_boot_id:
                    problems.append(
                        f"Pico boot_id changed mid-soak: {baseline.pico_boot_id} -> {pfw.boot_id} "
                        f"(Pico rebooted -- see CommonFW/docs/LINK_PROTOCOL.md boot_id_changed handling)"
                    )
        except Exception as exc:  # noqa: BLE001
            row["pico_commit"] = row["pico_built"] = row["pico_boot_id"] = ""
            problems.append(f"safety get_fw_version failed: {exc}")
    except Exception as exc:  # noqa: BLE001
        for k in ("link_up", "link_age_ms", "warn_mask", "trip_mask",
                   "crc_errors", "timeouts", "broadcast_dropped", "frames_sent"):
            row[k] = ""
        problems.append(f"safety status/link_stats/diag failed: {exc}")

    # --- Profile executor state (UART) ---
    try:
        exec_status = m._profiles.get_exec_status()
        row["exec_state"] = exec_status.state_name
        row["fault_guard"] = exec_status.fault_guard
        if exec_status.fault_guard:
            problems.append(f"profile executor fault_guard={exec_status.fault_guard}")
    except Exception as exc:  # noqa: BLE001
        row["exec_state"] = row["fault_guard"] = ""
        problems.append(f"profiles_get_exec_status failed: {exc}")

    return Sample(row=row, problems=problems)


def _linreg_slope_per_sec(t_values: "list[float]", values: "list[float]") -> float:
    """Ordinary least-squares slope of `values` against `t_values` (seconds),
    units per second. Returns 0.0 if there are fewer than 2 distinct t's."""
    n = len(t_values)
    if n < 2:
        return 0.0
    mean_t = sum(t_values) / n
    mean_y = sum(values) / n
    num = sum((t - mean_t) * (y - mean_y) for t, y in zip(t_values, values))
    den = sum((t - mean_t) ** 2 for t in t_values)
    if den == 0.0:
        return 0.0
    return num / den


def _trend_direction(t_values: "list[float]", values: "list[float]", floor_per_hour: float) -> "tuple[str, float]":
    """Real least-squares slope test over ALL samples (not a first-vs-last
    endpoint comparison -- that shape is blind to a monotone drift that
    happens to return near its starting value, and is dominated by noise in
    just the two endpoint samples). Fits a line to (t_values, values) and
    expresses the slope in units per hour; classifies DOWN/UP only once the
    magnitude of that fitted slope exceeds `floor_per_hour`, so a handful of
    noisy samples cannot flip the verdict the way two noisy endpoints could.

    Returns (label, slope_per_hour). label is "insufficient samples" below
    TREND_MIN_SAMPLES points; otherwise one of "DOWN", "UP", "flat".
    Callers must check the length themselves before trusting the label, same
    as before.
    """
    if len(values) < TREND_MIN_SAMPLES:
        return "insufficient samples", 0.0
    slope_per_hour = _linreg_slope_per_sec(t_values, values) * 3600.0
    if slope_per_hour <= -abs(floor_per_hour):
        return "DOWN", slope_per_hour
    if slope_per_hour >= abs(floor_per_hour):
        return "UP", slope_per_hour
    return "flat", slope_per_hour


def preflight_mode(exec_status, firing_in_progress: bool) -> "tuple[int | None, str]":
    """Decide whether the soak may start. Returns (rc, mode): rc is an exit
    code to return immediately (2 = refuse) or None to proceed."""
    firing_now = _is_firing(exec_status)
    if firing_now and not firing_in_progress:
        print(
            f"error: a firing is in progress (exec_state={exec_status.state_name!r}) -- "
            f"refusing to start an IDLE-mode soak against a live firing. Pass "
            f"--firing-in-progress if this is deliberate (a soak during a firing is the "
            f"more valuable run, and is supported -- it is just never the silent default).",
            file=sys.stderr,
        )
        return 2, "idle"
    if firing_in_progress and not firing_now:
        print(
            "WARNING: --firing-in-progress was passed but no firing is currently active "
            f"(exec_state={exec_status.state_name!r}). Proceeding, but this run will be "
            "labelled 'firing-in-progress' in the CSV/report even though it captured an idle board.",
        )
    return None, ("firing-in-progress" if firing_in_progress else "idle")


def summarize_and_verdict(rows, all_problems, mode, t0, out_path) -> int:
    """Final PASS/FAIL + trends over the collected rows. Returns the exit code."""
    def col(name: str) -> "list[float]":
        out = []
        for r in rows:
            v = r.get(name)
            if v in (None, ""):
                continue
            out.append(float(v))
        return out

    def col_with_t(name: str) -> "tuple[list[float], list[float]]":
        ts: list[float] = []
        vals: list[float] = []
        for r in rows:
            v = r.get(name)
            if v in (None, ""):
                continue
            ts.append(float(r["t_s"]))
            vals.append(float(v))
        return ts, vals

    heap_t, heap_vals = col_with_t("heap_internal_free")
    heap_trend, heap_slope_per_hour = _trend_direction(heap_t, heap_vals, HEAP_TREND_FLOOR_BYTES_PER_HOUR)
    stack_t, stack_vals = col_with_t("stack_min_headroom_pct")
    stack_pct_trend, stack_slope_per_hour = _trend_direction(stack_t, stack_vals, STACK_PCT_TREND_FLOOR_PP_PER_HOUR)
    crc_vals = col("crc_errors")
    timeout_vals = col("timeouts")
    bd_vals = col("broadcast_dropped")

    def _delta(vals: "list[float]") -> "float | None":
        return (vals[-1] - vals[0]) if len(vals) >= 2 else None

    crc_delta = _delta(crc_vals)
    timeout_delta = _delta(timeout_vals)
    bd_delta = _delta(bd_vals)

    print()
    print("=" * 70)
    print(f"SOAK SUMMARY -- mode={mode}, samples={len(rows)}, "
          f"duration={round(time.monotonic() - t0, 1)}s, csv={out_path}")
    print(f"  heap_internal.free trend: {heap_trend} (slope {heap_slope_per_hour:+.1f} B/hour)"
          + (" *** FLOOR BREACH ABOVE ***" if any(v < HEAP_INTERNAL_FLOOR_BYTES for v in col("heap_internal_free")) else ""))
    print(f"  stack min headroom% trend: {stack_pct_trend} (slope {stack_slope_per_hour:+.2f} pp/hour) "
          "(idle-only run cannot prove firing-time headroom -- see docstring)")

    # --- predicted-vs-actual cross-check (docs/audits/2026-09-08-httpd-stack-gap.md) ---
    # The static check's own "honest" headroom estimate is a constant-overhead
    # ALLOWANCE, not a measurement -- this is the live cross-check that audit
    # recommended to actually validate it, cheaply, on every soak run rather
    # than only when someone remembers to re-run the audit by hand.
    httpd_pct_samples = [
        float(r["stack_min_headroom_pct"]) for r in rows
        if r.get("stack_worst_task") == "httpd_worker" and r.get("stack_min_headroom_pct") not in (None, "")
    ]
    if httpd_pct_samples:
        worst_live_pct = min(httpd_pct_samples)
        try:
            import subprocess
            check_path = Path(__file__).resolve().parents[3] / "firmware" / "KilnFW" / "App" / "test" / "check_httpd_task_stack_budget.py"
            out = subprocess.run([sys.executable, str(check_path)], capture_output=True, text=True, timeout=60).stdout
            pred_pct = None
            for line in out.splitlines():
                if line.startswith("honest free"):
                    pred_pct = float(line.split("(")[1].split("%")[0])
                    break
            if pred_pct is not None:
                divergence = abs(pred_pct - worst_live_pct)
                print(f"  httpd_worker predicted-vs-actual: static honest estimate={pred_pct:.1f}%, "
                      f"live worst this run={worst_live_pct:.1f}% (divergence {divergence:.1f} pp)")
                if divergence > 8.0:
                    all_problems.append(
                        f"httpd_worker static honest-headroom estimate ({pred_pct:.1f}%) diverges from "
                        f"live worst ({worst_live_pct:.1f}%) by {divergence:.1f} pp -- "
                        "UNMODELED_OVERHEAD_BYTES in check_httpd_task_stack_budget.py may need retuning")
        except Exception as exc:  # noqa: BLE001 -- cross-check is best-effort, never blocks the soak
            print(f"  httpd_worker predicted-vs-actual: skipped ({exc})")
    print(f"  safety crc_errors delta over run: {crc_delta}")
    print(f"  safety timeouts delta over run: {timeout_delta}")
    print(f"  safety broadcast_dropped delta over run: {bd_delta}")
    for counter_name, delta in (("crc_errors", crc_delta), ("timeouts", timeout_delta), ("broadcast_dropped", bd_delta)):
        if delta is not None and delta > 0:
            all_problems.append(f"{counter_name} climbed by {delta} over the run (not flat)")
    if heap_trend == "DOWN":
        all_problems.append("heap_internal.free trended DOWN over the run")
    if stack_pct_trend == "DOWN":
        all_problems.append("stack min headroom% trended DOWN over the run")

    print()
    if all_problems:
        print(f"RESULT: FAIL ({len(all_problems)} problem(s))")
        for p in all_problems:
            print(f"  - {p}")
        return 1
    print("RESULT: PASS")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--duration", type=float, default=3600, help="total soak length in seconds (default 3600 = 1h)")
    ap.add_argument("--interval", type=float, default=60, help="seconds between samples (default 60)")
    ap.add_argument("--out", type=str, default=None, help="CSV output path (default: stability_soak_<ts>.csv in cwd)")
    ap.add_argument("--host", type=str, default=None, help="dashboard HTTP host (default: autodetect via wifi status)")
    ap.add_argument("--firing-in-progress", action="store_true",
                     help="acknowledge a firing is active and soak through it anyway (the more valuable run); "
                          "without this flag the script refuses to start if a firing is detected")
    ap.add_argument("--max-samples", type=int, default=None, help="stop after N samples regardless of --duration (for smoke tests)")
    args = ap.parse_args()

    print(m.connect())
    host = _ota_resolve_host(args.host)
    print(f"dashboard host: {host}")

    try:
        exec_status = m._profiles.get_exec_status()
    except Exception as exc:  # noqa: BLE001
        print(f"error: could not read profile executor state before starting: {exc}", file=sys.stderr)
        return 2

    rc, mode = preflight_mode(exec_status, args.firing_in_progress)
    if rc is not None:
        return rc
    print(f"soak mode: {mode}")

    out_path = Path(args.out) if args.out else Path(f"stability_soak_{time.strftime('%Y%m%d_%H%M%S')}.csv")
    print(f"writing: {out_path}")

    baseline = Baseline()
    t0 = time.monotonic()
    rows: list[dict] = []
    all_problems: list[str] = []

    with out_path.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=CSV_FIELDS)
        writer.writeheader()
        f.flush()

        n = 0
        while True:
            sample = sample_once(host, baseline, t0)
            rows.append(sample.row)
            writer.writerow(sample.row)
            f.flush()
            n += 1
            if sample.problems:
                for p in sample.problems:
                    print(f"  [t={sample.row['t_s']}s] PROBLEM: {p}")
                    all_problems.append(f"t={sample.row['t_s']}s: {p}")
            else:
                print(f"  [t={sample.row['t_s']}s] ok "
                      f"(heap_internal.free={sample.row.get('heap_internal_free')} B, "
                      f"link_up={sample.row.get('link_up')}, "
                      f"stack={sample.row.get('stack_worst_level')})")

            if args.max_samples is not None and n >= args.max_samples:
                break
            elapsed = time.monotonic() - t0
            if elapsed >= args.duration:
                break
            time.sleep(min(args.interval, max(0.0, args.duration - elapsed)))

    return summarize_and_verdict(rows, all_problems, mode, t0, out_path)


if __name__ == "__main__":
    sys.exit(main())
