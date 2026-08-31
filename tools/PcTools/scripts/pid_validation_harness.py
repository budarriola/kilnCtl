#!/usr/bin/env python3
"""One-command hardware-in-the-loop validation of the PID expansion work.

Runs, in order (each independently skippable with ``--skip-<stage>``):

  1. cooldown gate     -- wait for every zone within a spread of ambient
  2. tune all zones     -- step autotune each zone, accept the fitted gains
  3. coupling matrix     -- capture /api/autotune/matrix + RGA to a file
  4. backup round-trip   -- export, save, re-import, diff against the export
  5. profile tracking    -- run a bounded profile, sample, assert on error

SAFETY. This script enforces its own hard maximum temperature
(``--max-temp-c``, default ``pid_validation.DEFAULT_HARD_MAX_TEMP_C``) IN
ADDITION to whatever the firmware enforces -- see ``pid_validation.
check_hard_max``. Every exit path -- normal completion, an assertion
failure, an unhandled exception, Ctrl-C -- runs the same teardown
(``force_all_stop()``): stop the profile executor, abort any autotune,
acknowledge the last run, and record which relays are on afterwards. This is
wired through ``try/finally`` around the whole run, not around each stage
individually, so a Ctrl-C between stages still leaves the kiln safe.

This script never disables, bypasses, or overrides a firmware interlock, and
never flashes firmware.

USAGE
    python tools/PcTools/scripts/pid_validation_harness.py --host kilnctl.local
    python tools/PcTools/scripts/pid_validation_harness.py --dry-run
    python tools/PcTools/scripts/pid_validation_harness.py --host kilnctl.local \\
        --skip-tune --skip-backup   # re-run just the tracking test

Reports land under ``--report-dir`` (default ``tools/PcTools/logs/
pid_validation/``) as ``<timestamp>.json`` + ``<timestamp>.txt``.
Non-zero exit code on any failure, abort, or exception -- suitable for CI.
"""
from __future__ import annotations

import argparse
import json
import logging
import os
import sys
import time
from typing import Any, Optional

_HERE = os.path.dirname(os.path.abspath(__file__))
_SRC = os.path.join(_HERE, "..", "src")
_TESTS = os.path.join(_HERE, "..", "tests")
for p in (_SRC, _TESTS):
    if p not in sys.path:
        sys.path.insert(0, p)

from kilnctrl import pid_validation as pv  # noqa: E402
from bench_fixture_session import (  # noqa: E402
    BenchSession, BenchSessionError, HTTP_TIMEOUT_S, _http,
)

log = logging.getLogger("pid_validation_harness")

DEFAULT_REPORT_DIR = os.path.join(_HERE, "..", "logs", "pid_validation")
DEFAULT_BACKUP_PATH = os.path.join(_HERE, "..", "config_presets", "pid_validation_backup.json")

#: 0.4 duty step, per the brief. Kept as a module constant (not just an
#: argparse default) so tests can reference the same number.
DEFAULT_TUNE_DUTY = 0.4


# ---------------------------------------------------------------------------
# Extra HTTP calls bench_fixture_session.BenchSession does not carry: accept
# and the backup export/import round trip. Attached to the BenchSession
# CLASS at import time (rather than editing bench_fixture_session.py, which
# this pass must not touch) so every stage function below can call them as
# plain session methods -- ``session.autotune_accept()`` etc. -- and a
# FakeSession implementing the same three names is a drop-in dry-run
# substitute with no special-casing in the stage functions themselves.
# ---------------------------------------------------------------------------

def _bs_autotune_accept(self) -> "tuple[bool, str]":
    code, text = _http(self.host, "/api/autotune/accept", "")
    return code == 200, text


def _bs_backup_export(self) -> dict:
    code, text = _http(self.host, "/api/backup/export")
    if code != 200:
        raise BenchSessionError(f"GET /api/backup/export -> {code}: {text[:300]}")
    return json.loads(text)


def _bs_backup_import(self, doc: dict) -> "tuple[bool, str]":
    body = json.dumps(doc)
    code, text = _http(self.host, "/api/backup/import", body)
    return code == 200, text


BenchSession.autotune_accept = _bs_autotune_accept
BenchSession.backup_export = _bs_backup_export
BenchSession.backup_import = _bs_backup_import


# ---------------------------------------------------------------------------
# Stage implementations. Each takes the live session + args and returns a
# pid_validation.StageReport. Each is a plain function, not a method, so the
# dry-run path can call the SAME functions against a FakeSession.
# ---------------------------------------------------------------------------

class Aborted(Exception):
    """Raised by a stage to unwind the whole run through the finally block."""


def _guard(session, limit_c: float, where: str) -> None:
    hottest = session.hottest_channel_c()
    try:
        pv.check_hard_max(hottest, limit_c, where)
    except pv.HardMaxExceeded as exc:
        raise Aborted(str(exc)) from exc


def stage_cooldown(session, args) -> pv.StageReport:
    t0 = time.monotonic()
    try:
        result = session.wait_for_cooldown(
            target_c=args.cooldown_target_c, tolerance_c=args.cooldown_tolerance_c,
            timeout_s=args.cooldown_timeout_s, poll_s=args.cooldown_poll_s)
        check = pv.CheckResult(
            name="cooldown_reached", passed=bool(result["reached"]),
            actual=result["hottest_c"], threshold=result["target_c"])
        return pv.StageReport(name="cooldown", passed=check.passed, checks=[check],
                              detail=result, duration_s=time.monotonic() - t0)
    except BenchSessionError as exc:
        return pv.StageReport(name="cooldown", passed=False, error=str(exc),
                              duration_s=time.monotonic() - t0)


def stage_tune_all_zones(session, args) -> pv.StageReport:
    t0 = time.monotonic()
    checks: "list[pv.CheckResult]" = []
    detail: "dict[str, Any]" = {"zones": {}}
    passed = True
    for zone in args.zones:
        _guard(session, args.max_temp_c, f"tune zone {zone}")
        code, body = session.start_autotune_step(zone, args.tune_duty)
        if code != 200:
            checks.append(pv.CheckResult(name=f"zone{zone}.start", passed=False,
                                         actual=f"{code}: {body[:200]}", threshold="200"))
            passed = False
            continue
        status = session.wait_for_autotune_state(("done", "aborted"), args.tune_timeout_s,
                                                  poll_s=args.tune_poll_s)
        reason = pv.autotune_refusal_reason(status)
        checks.append(pv.CheckResult(name=f"zone{zone}.refusal", passed=reason is None,
                                     actual=status.get("refusal"), threshold="ok", detail=reason or ""))
        model = {k: status.get(k) for k in ("k_gain_c_per_duty", "tau_s", "dead_time_s")}
        gains = {k: status.get(k) for k in ("proposed_kp", "proposed_ki", "proposed_kd")}
        detail["zones"][zone] = {"status": status, "model": model, "proposed_gains": gains}
        if reason is not None:
            passed = False
            # Do not accept a refused fit -- accepting is exactly the "a
            # refused tune looked exactly like a successful one" bug class
            # this repo has already hit once (see commit 813ad90).
            continue
        ok, body = session.autotune_accept()
        checks.append(pv.CheckResult(name=f"zone{zone}.accept", passed=ok,
                                     actual=body[:200], threshold="ok"))
        passed = passed and ok
        # Cool down between zones so the next zone's step starts from a
        # settled baseline, not this zone's residual heat -- same reasoning
        # as bench_fixture_session.wait_for_cooldown's own docstring.
        if zone != args.zones[-1]:
            try:
                session.wait_for_cooldown(
                    target_c=args.cooldown_target_c, tolerance_c=args.cooldown_tolerance_c,
                    timeout_s=args.cooldown_timeout_s, poll_s=args.cooldown_poll_s)
            except BenchSessionError as exc:
                checks.append(pv.CheckResult(name=f"zone{zone}.intertune_cooldown", passed=False,
                                             actual=str(exc), threshold="reached"))
                passed = False
    return pv.StageReport(name="tune_all_zones", passed=passed, checks=checks, detail=detail,
                          duration_s=time.monotonic() - t0)


def stage_coupling_matrix(session, args) -> pv.StageReport:
    t0 = time.monotonic()
    try:
        matrix = session.autotune_matrix()
    except BenchSessionError as exc:
        return pv.StageReport(name="coupling_matrix", passed=False, error=str(exc),
                              duration_s=time.monotonic() - t0)
    zone_count = matrix.get("zone_count", 0)
    cells = matrix.get("cells", [])
    n_valid_rows = 0
    if zone_count:
        for i in range(zone_count):
            row_cells = cells[i * zone_count:(i + 1) * zone_count]
            if row_cells and all(c.get("valid") for j, c in enumerate(row_cells) if j != i):
                n_valid_rows += 1
    check = pv.CheckResult(name="matrix_rows_filled", passed=n_valid_rows == zone_count and zone_count > 0,
                           actual=n_valid_rows, threshold=zone_count)
    if args.matrix_path:
        os.makedirs(os.path.dirname(args.matrix_path) or ".", exist_ok=True)
        with open(args.matrix_path, "w", encoding="utf-8") as fh:
            json.dump(matrix, fh, indent=2)
    return pv.StageReport(name="coupling_matrix", passed=check.passed, checks=[check],
                          detail={"matrix": matrix, "saved_to": args.matrix_path},
                          duration_s=time.monotonic() - t0)


def stage_backup_roundtrip(session, args) -> pv.StageReport:
    t0 = time.monotonic()
    try:
        exported = session.backup_export()
    except BenchSessionError as exc:
        return pv.StageReport(name="backup_roundtrip", passed=False, error=str(exc),
                              duration_s=time.monotonic() - t0)
    if args.backup_path:
        os.makedirs(os.path.dirname(args.backup_path) or ".", exist_ok=True)
        with open(args.backup_path, "w", encoding="utf-8") as fh:
            json.dump(exported, fh, indent=2)
    ok, body = session.backup_import(exported)
    if not ok:
        return pv.StageReport(
            name="backup_roundtrip", passed=False,
            error=f"import refused: {body[:300]}", duration_s=time.monotonic() - t0)
    try:
        reimported = session.backup_export()
    except BenchSessionError as exc:
        return pv.StageReport(name="backup_roundtrip", passed=False, error=str(exc),
                              duration_s=time.monotonic() - t0)
    mismatches = pv.diff_backup_zones(exported, reimported)
    check = pv.CheckResult(name="reimport_matches_export", passed=not mismatches,
                           actual=mismatches, threshold=[])
    return pv.StageReport(name="backup_roundtrip", passed=check.passed, checks=[check],
                          detail={"saved_to": args.backup_path, "zone_count_exported":
                                  len(exported.get("zones", []))},
                          duration_s=time.monotonic() - t0)


def _profile_segments_c_to_80(peak1: float, peak2: float) -> "list[dict]":
    """Two ramp/hold pairs entirely inside 0..80 C, per the brief's example.
    ramp_c_per_hr chosen to reach each peak in a few minutes on this bench
    (measured ~2.8-3.8 C/min under full duty -- see bench_fixture_session.py)
    without the profile itself becoming the long pole in the run."""
    return [
        {"target_c": peak1, "ramp_c_per_hr": 120.0, "dwell_min": 8},
        {"target_c": peak2, "ramp_c_per_hr": 120.0, "dwell_min": 8},
    ]


def stage_profile_tracking(session, args) -> pv.StageReport:
    t0 = time.monotonic()
    checks: "list[pv.CheckResult]" = []
    detail: "dict[str, Any]" = {}
    try:
        zone_mask = sum(1 << z for z in args.zones)
        segments = _profile_segments_c_to_80(args.tracking_peak1_c, args.tracking_peak2_c)
        session.put_profile(args.tracking_slot, "pid_validation_tracking", zone_mask, segments)
        code, body = session.start_profile(args.tracking_slot)
        if code != 200:
            return pv.StageReport(name="profile_tracking", passed=False,
                                  error=f"start refused: {code}: {body[:300]}",
                                  duration_s=time.monotonic() - t0)
        raw_rows = session.sample_response(
            args.tracking_duration_s, period_s=args.tracking_period_s, zone_index=args.zones[0])
    except BenchSessionError as exc:
        return pv.StageReport(name="profile_tracking", passed=False, error=str(exc),
                              duration_s=time.monotonic() - t0)

    hold_target_low = min(args.tracking_peak1_c, args.tracking_peak2_c) - 0.5
    samples: "list[pv.TrackingSample]" = []
    spreads: "list[float]" = []
    for row in raw_rows:
        target = row.get("target_c")
        if target is None:
            continue
        chans = row.get("channels_c") or {}
        spreads.append(pv.compute_spread_c({int(k): float(v) for k, v in chans.items()}))
        # A sample is "in hold" when the executor's own target has stopped
        # moving relative to the last one seen for that zone -- approximated
        # here as "close to one of the two programmed peaks", since the raw
        # rows do not carry the executor's own ramp/hold phase flag.
        in_hold = any(abs(target - peak) < 0.75
                      for peak in (args.tracking_peak1_c, args.tracking_peak2_c))
        for z, temp_c in chans.items():
            samples.append(pv.TrackingSample(
                t_s=row["t_s"], zone=int(z), measured_c=float(temp_c),
                setpoint_c=float(target), in_hold=in_hold))
        hottest = row.get("hottest_c")
        if hottest is not None:
            try:
                pv.check_hard_max(float(hottest), args.max_temp_c, "profile tracking sample")
            except pv.HardMaxExceeded as exc:
                raise Aborted(str(exc)) from exc

    thresholds = pv.TrackingThresholds(
        max_abs_err_c=args.max_abs_err_c, rms_err_c=args.rms_err_c,
        steady_state_err_c=args.steady_state_err_c, spread_c=args.spread_c)
    passed = True
    for zone in args.zones:
        try:
            stats = pv.tracking_stats(samples, zone)
        except ValueError as exc:
            checks.append(pv.CheckResult(name=f"zone{zone}.samples", passed=False,
                                         actual=0, threshold=">0", detail=str(exc)))
            passed = False
            continue
        zchecks = pv.evaluate_zone_tracking(stats, thresholds)
        checks.extend(zchecks)
        passed = passed and all(c.passed for c in zchecks)
        detail[f"zone{zone}_stats"] = {
            "max_abs_err_c": stats.max_abs_err_c, "rms_err_c": stats.rms_err_c,
            "steady_state_err_c": stats.steady_state_err_c, "n_samples": stats.n_samples,
            "n_hold_samples": stats.n_hold_samples,
        }
    spread_check = pv.evaluate_spread(spreads, thresholds)
    checks.append(spread_check)
    passed = passed and spread_check.passed
    detail["n_raw_rows"] = len(raw_rows)
    return pv.StageReport(name="profile_tracking", passed=passed, checks=checks, detail=detail,
                          duration_s=time.monotonic() - t0)


STAGES = [
    ("cooldown", stage_cooldown),
    ("tune", stage_tune_all_zones),
    ("matrix", stage_coupling_matrix),
    ("backup", stage_backup_roundtrip),
    ("tracking", stage_profile_tracking),
]


# ---------------------------------------------------------------------------
# Dry-run: exercises the exact stage functions above against a FakeSession
# that returns canned, internally-consistent data -- no network, no board.
# ---------------------------------------------------------------------------

class FakeSession:
    """Duck-types the subset of BenchSession the stage functions call.

    Deliberately produces DATA THAT FAILS ONE CHECK (zone 2's steady-state
    error is set past the default threshold) so a dry run proves the wiring
    end to end -- callers exercising --dry-run with default thresholds
    should see stage 5 FAIL on exactly that number, not a suite that always
    reports green regardless of what the numbers were.
    """

    def __init__(self) -> None:
        self.host = "dry-run.invalid"
        self._t = 30.0
        self._matrix_calls = 0

    def hottest_channel_c(self) -> float:
        return self._t

    def wait_for_cooldown(self, target_c=None, tolerance_c=3.0, timeout_s=2700.0, poll_s=20.0):
        self._t = (target_c if target_c is not None else 33.0)
        return {"reached": True, "hottest_c": self._t, "target_c": self._t,
                "ambient_c": 30.0, "waited_s": 12.0, "samples": 3, "history": []}

    def start_autotune_step(self, zone_index, step_duty):
        return 200, "ok"

    def wait_for_autotune_state(self, states, timeout_s, poll_s=2.0):
        return {
            "state": "done", "model_valid": True, "refusal": "ok", "refusal_reason": "",
            "k_gain_c_per_duty": 40.0, "tau_s": 150.0, "dead_time_s": 12.0,
            "proposed_kp": 1.2, "proposed_ki": 0.02, "proposed_kd": 3.5,
        }

    def autotune_matrix(self) -> dict:
        self._matrix_calls += 1
        cells = []
        for i in range(3):
            for j in range(3):
                if i == j:
                    cells.append({"valid": False})
                else:
                    cells.append({"valid": True, "k": 0.1 + 0.01 * (i + j), "tau_s": 200.0,
                                  "dead_time_s": 15.0})
        return {"zone_count": 3, "cells": cells,
                "rga": {"valid": True, "lambda": [1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0]}}

    def autotune_accept(self) -> "tuple[bool, str]":
        return True, '{"ok":true}'

    def backup_export(self) -> dict:
        self._backup_doc = getattr(self, "_backup_doc", None) or {
            "kind": "kilnctl_backup", "version": 1, "profiles": [],
            "zones": [{"index": z, "pid_kp": 1.2, "pid_ki": 0.02, "pid_kd": 3.5,
                       "model_k_dc": 40.0, "model_tau_s": 150.0, "model_dead_time_s": 12.0}
                      for z in range(3)],
        }
        return self._backup_doc

    def backup_import(self, doc: dict) -> "tuple[bool, str]":
        # A real import is a write the next export would reflect; the fake
        # mirrors that by adopting exactly what was imported, so a caller
        # that mutated the doc before re-importing sees that mutation on the
        # next backup_export() the same way the real board would.
        self._backup_doc = doc
        return True, '{"ok":true}'

    def put_profile(self, slot, name, zone_mask, segments):
        return None

    def start_profile(self, slot):
        return 200, "ok"

    def sample_response(self, duration_s, period_s=2.0, zone_index=0):
        rows = []
        t = 0.0
        peaks = (50.0, 70.0)
        while t < duration_s:
            phase = 0 if t < duration_s / 2 else 1
            target = peaks[phase]
            # zone 2 deliberately tracks 2.0 C low at steady state -- past
            # the 1.5 C default threshold -- so a dry run demonstrably FAILS
            # stage 5 rather than trivially passing every number.
            chans = {0: target - 0.2, 1: target - 0.3, 2: target - 2.0}
            rows.append({
                "t_s": round(t, 1), "zone_c": chans.get(zone_index, target),
                "channels_c": chans, "hottest_c": max(chans.values()),
                "relays_on": [], "safety_heating_enabled": True,
                "safety_relay_energized": True, "heat_block_sources_words": [0, 0],
                "exec_state": "running", "target_c": target,
            })
            t += period_s
        return rows

    def force_all_stop(self) -> dict:
        return {"relays_on": [], "state": "idle", "autotune_state": "idle"}


def run_dry_run(args) -> pv.RunReport:
    session = FakeSession()
    return _run(session, args)


# ---------------------------------------------------------------------------
# Orchestration
# ---------------------------------------------------------------------------

def _run(session, args) -> pv.RunReport:
    report = pv.RunReport(host=session.host, started_at=pv.now_iso(), dry_run=bool(args.dry_run),
                          hard_max_temp_c=args.max_temp_c)
    skip = {
        "cooldown": args.skip_cooldown, "tune": args.skip_tune, "matrix": args.skip_matrix,
        "backup": args.skip_backup, "tracking": args.skip_tracking,
    }
    try:
        for name, fn in STAGES:
            if skip[name]:
                report.stages.append(pv.StageReport(name=name, passed=True, skipped=True))
                continue
            log.info("=== stage: %s ===", name)
            try:
                stage_report = fn(session, args)
            except Aborted as exc:
                report.aborted = True
                report.abort_reason = f"{name}: {exc}"
                report.stages.append(pv.StageReport(name=name, passed=False, error=str(exc)))
                break
            report.stages.append(stage_report)
            log.info("stage %s: %s", name, "PASS" if stage_report.passed else "FAIL")
            if not stage_report.passed and args.stop_on_first_failure:
                break
    except KeyboardInterrupt:
        report.aborted = True
        report.abort_reason = "interrupted (Ctrl-C)"
        log.warning("interrupted -- forcing a safe stop before exiting")
    except Exception as exc:  # noqa: BLE001 - must still reach the finally below
        report.aborted = True
        report.abort_reason = f"unhandled exception: {exc!r}"
        log.exception("unhandled exception during run")
    finally:
        try:
            stop_detail = session.force_all_stop()
            report.stages.append(pv.StageReport(
                name="teardown_force_all_stop", passed=stop_detail.get("relays_on") == [],
                detail=stop_detail))
        except Exception as exc:  # noqa: BLE001 - teardown must never itself raise past here
            report.aborted = True
            report.abort_reason = (report.abort_reason or "") + f"; teardown failed: {exc!r}"
            log.exception("force_all_stop failed during teardown")
        report.finished_at = pv.now_iso()
    return report


def write_report(report: pv.RunReport, report_dir: str) -> "tuple[str, str]":
    os.makedirs(report_dir, exist_ok=True)
    stamp = time.strftime("%Y%m%d_%H%M%S", time.localtime())
    json_path = os.path.join(report_dir, f"pid_validation_{stamp}.json")
    txt_path = os.path.join(report_dir, f"pid_validation_{stamp}.txt")
    with open(json_path, "w", encoding="utf-8") as fh:
        fh.write(report.to_json())
    with open(txt_path, "w", encoding="utf-8") as fh:
        fh.write(report.summary_text() + "\n")
    return json_path, txt_path


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--host", default=os.environ.get("KILNCTRL_BENCH_HOST", "kilnctl.local"),
                   help="board hostname/IP (default: $KILNCTRL_BENCH_HOST or kilnctl.local)")
    p.add_argument("--dry-run", action="store_true",
                   help="exercise the full flow against synthetic data -- no network, no board")
    p.add_argument("--zones", type=lambda s: [int(x) for x in s.split(",")], default=[0, 1, 2],
                   help="comma-separated zone indices to tune/track (default: 0,1,2)")
    p.add_argument("--max-temp-c", type=float, default=pv.DEFAULT_HARD_MAX_TEMP_C,
                   help=f"harness's own hard abort ceiling (default: {pv.DEFAULT_HARD_MAX_TEMP_C})")
    p.add_argument("--stop-on-first-failure", action="store_true",
                   help="stop after the first FAILED stage instead of running every stage regardless")

    p.add_argument("--skip-cooldown", action="store_true")
    p.add_argument("--skip-tune", action="store_true")
    p.add_argument("--skip-matrix", action="store_true")
    p.add_argument("--skip-backup", action="store_true")
    p.add_argument("--skip-tracking", action="store_true")

    g = p.add_argument_group("cooldown gate")
    g.add_argument("--cooldown-target-c", type=float, default=None,
                   help="explicit cooldown target; default derives from ambient + tolerance")
    g.add_argument("--cooldown-tolerance-c", type=float, default=0.8,
                   help="cooldown gate spread from ambient, degC (default: 0.8)")
    g.add_argument("--cooldown-timeout-s", type=float, default=2700.0,
                   help="cooldown budget, seconds (default: 2700 = 45 min)")
    g.add_argument("--cooldown-poll-s", type=float, default=20.0)

    g = p.add_argument_group("tuning")
    g.add_argument("--tune-duty", type=float, default=DEFAULT_TUNE_DUTY,
                   help=f"step duty for autotune (default: {DEFAULT_TUNE_DUTY})")
    g.add_argument("--tune-timeout-s", type=float, default=3600.0)
    g.add_argument("--tune-poll-s", type=float, default=5.0)

    g = p.add_argument_group("coupling matrix")
    g.add_argument("--matrix-path", default=None,
                   help="save raw matrix JSON here (default: <report-dir>/coupling_matrix_<ts>.json)")

    g = p.add_argument_group("backup")
    g.add_argument("--backup-path", default=DEFAULT_BACKUP_PATH,
                   help=f"save the exported config here (default: {DEFAULT_BACKUP_PATH})")

    g = p.add_argument_group("profile tracking")
    g.add_argument("--tracking-slot", type=int, default=15,
                   help="user profile slot to write/run the tracking test in (default: 15)")
    g.add_argument("--tracking-peak1-c", type=float, default=50.0)
    g.add_argument("--tracking-peak2-c", type=float, default=70.0)
    g.add_argument("--tracking-duration-s", type=float, default=1800.0,
                   help="total sampling duration, seconds (default: 1800 = 30 min)")
    g.add_argument("--tracking-period-s", type=float, default=5.0)
    g.add_argument("--max-abs-err-c", type=float, default=5.0,
                   help="tracking threshold: worst instantaneous error (default: 5.0)")
    g.add_argument("--rms-err-c", type=float, default=2.0,
                   help="tracking threshold: RMS error over the run (default: 2.0)")
    g.add_argument("--steady-state-err-c", type=float, default=1.5,
                   help="tracking threshold: mean signed error during holds (default: 1.5)")
    g.add_argument("--spread-c", type=float, default=2.0,
                   help="tracking threshold: worst cross-zone spread (default: 2.0)")

    p.add_argument("--report-dir", default=DEFAULT_REPORT_DIR,
                   help=f"where to write timestamped reports (default: {DEFAULT_REPORT_DIR})")
    p.add_argument("-v", "--verbose", action="store_true")
    return p


def main(argv: "Optional[list[str]]" = None) -> int:
    args = build_arg_parser().parse_args(argv)
    logging.basicConfig(level=logging.DEBUG if args.verbose else logging.INFO,
                        format="%(asctime)s %(levelname)-7s %(message)s")
    if args.matrix_path is None:
        os.makedirs(args.report_dir, exist_ok=True)
        args.matrix_path = os.path.join(
            args.report_dir, f"coupling_matrix_{time.strftime('%Y%m%d_%H%M%S')}.json")

    if args.dry_run:
        report = run_dry_run(args)
    else:
        session = BenchSession(host=args.host)
        report = _run(session, args)

    json_path, txt_path = write_report(report, args.report_dir)
    print(report.summary_text())
    print(f"\nreport: {json_path}\n        {txt_path}")
    return 0 if report.passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
