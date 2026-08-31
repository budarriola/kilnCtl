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
from kilnctrl import zones_http_client  # noqa: E402
from bench_fixture_session import (  # noqa: E402
    BenchSession, BenchSessionError, HTTP_TIMEOUT_S, _http,
)

#: zone_control_mode_t (firmware/KilnFW/App/drivers/zones_http.h:695-698):
#: OFF=0, BANGBANG=1, PID=2, PID_FUZZY=3. autotune_accept() writes gains and
#: the fitted model but does NOT change control_mode -- a zone can come out
#: of "accept" with a freshly-tuned PID and still be sitting in OFF, in which
#: case a profile run against it measures nothing. See
#: _bs_ensure_pid_mode()/stage_profile_tracking() below.
ZONE_CONTROL_MODE_PID = 2

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


def _bs_get_zones(self) -> dict:
    return zones_http_client.get_zones(self.host, timeout=HTTP_TIMEOUT_S)


def _bs_commission_zones(self, current: dict, preset: dict) -> None:
    body = zones_http_client.build_post_body(current, preset)
    zones_http_client.post_zones(self.host, body, timeout=HTTP_TIMEOUT_S)


def _bs_ensure_pid_mode(self, zones: "list[int]") -> "dict[int, int]":
    """Make sure every zone in ``zones`` is in PID mode (2), and READ IT
    BACK to confirm the write actually took -- accepting a 200 from POST
    /api/zones is not itself proof, the same class of gap
    diff_backup_zones() exists to catch for the backup path. Returns
    ``{zone_index: control_mode_after}`` for every zone touched; a caller
    that finds anything other than ZONE_CONTROL_MODE_PID in the result
    knows the mode did not take.

    Only zones that are NOT already in PID mode are POSTed -- an
    already-correct zone is left alone rather than round-tripped for no
    reason.
    """
    current = zones_http_client.get_zones(self.host, timeout=HTTP_TIMEOUT_S)
    by_index = {z["index"]: z for z in current.get("zones", [])}
    needs_write = [z for z in zones
                   if by_index.get(z, {}).get("control_mode") != ZONE_CONTROL_MODE_PID]
    if needs_write:
        preset = {"zones": [{"index": z, "control_mode": ZONE_CONTROL_MODE_PID}
                             for z in needs_write]}
        body = zones_http_client.build_post_body(current, preset)
        zones_http_client.post_zones(self.host, body, timeout=HTTP_TIMEOUT_S)
    after = zones_http_client.get_zones(self.host, timeout=HTTP_TIMEOUT_S)
    after_by_index = {z["index"]: z for z in after.get("zones", [])}
    return {z: after_by_index.get(z, {}).get("control_mode") for z in zones}


BenchSession.autotune_accept = _bs_autotune_accept
BenchSession.backup_export = _bs_backup_export
BenchSession.backup_import = _bs_backup_import
BenchSession.ensure_pid_mode = _bs_ensure_pid_mode
BenchSession.get_zones = _bs_get_zones
BenchSession.commission_zones = _bs_commission_zones


# ---------------------------------------------------------------------------
# Stage implementations. Each takes the live session + args and returns a
# pid_validation.StageReport. Each is a plain function, not a method, so the
# dry-run path can call the SAME functions against a FakeSession.
# ---------------------------------------------------------------------------

class Aborted(Exception):
    """Raised by a stage to unwind the whole run through the finally block."""


def _guard(session, args, limit_c: float, where: str) -> None:
    # Routed through _retrying() rather than calling session.hottest_channel_c()
    # directly: this both (a) survives a transient blip on the pre-tune
    # temperature read instead of aborting the whole run over it, and (b)
    # gets a board-health observation on every guard point via _retrying's
    # own health check -- see B1 in the retry-hardening review.
    hottest = _retrying(session, args, "guard", where, session.hottest_channel_c)
    try:
        pv.check_hard_max(hottest, limit_c, where)
    except pv.HardMaxExceeded as exc:
        raise Aborted(str(exc)) from exc


# ---------------------------------------------------------------------------
# Retry wiring. See pid_validation.call_with_retry / detect_board_restart for
# the actual policy -- this is just the glue that (a) threads a RetryPolicy
# built from CLI args through every HTTP-touching stage call, (b) records
# every retry onto args._retry_events so the final report shows them, and
# (c) checks the board's own /api/status after a retry recovers, aborting
# loudly instead of reporting a pass if the board evidently rebooted.
# ---------------------------------------------------------------------------

def _retry_policy(args) -> pv.RetryPolicy:
    return pv.RetryPolicy(max_retries=args.max_retries, backoff_s=args.retry_backoff_s,
                          backoff_multiplier=args.retry_backoff_multiplier,
                          backoff_max_s=args.retry_backoff_max_s)


def _retrying(session, args, stage_name: str, operation: str, fn):
    """Call ``fn()`` under the run's retry policy. Transient HTTP errors are
    retried with backoff; EVERY call -- not just one that actually retried
    -- is followed by a board health check (uptime/reset_reason). This is
    the fix for B1: a mid-run reboot fast enough that the very next call
    lands on the rebooted board with no exception at all (no retry, no
    error) must still be caught here, not just the case where a retry
    visibly fired. Exhausted retries, or a detected restart, unwind the run
    via Aborted -- the same path a hard-max breach takes."""
    policy = _retry_policy(args)
    events: "list[pv.RetryEvent]" = []

    def on_retry(attempt, wait_s, exc):
        ev = pv.RetryEvent(stage=stage_name, operation=operation, attempt=attempt,
                           wait_s=wait_s, error=str(exc))
        events.append(ev)
        log.warning("transient error in %s.%s (attempt %d/%d, retrying in %.1fs): %s",
                    stage_name, operation, attempt, policy.max_retries, wait_s, exc)

    try:
        result = pv.call_with_retry(fn, policy=policy, is_transient=pv.is_transient_http_error,
                                    on_retry=on_retry)
    except Exception as exc:
        args._retry_events.extend(events)
        raise Aborted(
            f"{stage_name}.{operation}: gave up after {len(events)} retry attempt(s) "
            f"(max_retries={policy.max_retries}) -- last error: {exc}") from exc

    args._retry_events.extend(events)
    # Observe board health unconditionally -- see the docstring above for
    # why this must not be gated on ``if events``.
    try:
        status = session.status()
    except BenchSessionError:
        status = None
    if status is not None:
        restart_reason = args._health.observe(status)
        if restart_reason:
            raise Aborted(f"{stage_name}.{operation}: {restart_reason}")
    return result


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
        _guard(session, args, args.max_temp_c, f"tune zone {zone}")
        code, body = _retrying(session, args, "tune_all_zones", f"zone{zone}.start_autotune_step",
                               lambda z=zone: session.start_autotune_step(z, args.tune_duty))
        if code != 200:
            checks.append(pv.CheckResult(name=f"zone{zone}.start", passed=False,
                                         actual=f"{code}: {body[:200]}", threshold="200"))
            passed = False
            continue
        # B3 fix: wait_for_autotune_state is itself a bounded polling loop
        # (up to args.tune_timeout_s, e.g. 3600s by default). Retrying the
        # WHOLE call on a transient blip must not hand it a fresh
        # tune_timeout_s budget each attempt -- worst case that is
        # (max_retries + 1) x tune_timeout_s of driven heat for one zone.
        # Instead compute one absolute wall-clock deadline before the first
        # attempt and pass each retry only the time remaining until it, so
        # a retry can never extend the total time this zone is allowed to
        # run past what a single clean attempt would have taken.
        zone_tune_deadline = time.monotonic() + args.tune_timeout_s
        status = _retrying(
            session, args, "tune_all_zones", f"zone{zone}.wait_for_autotune_state",
            lambda: session.wait_for_autotune_state(
                ("done", "aborted"), max(0.0, zone_tune_deadline - time.monotonic()),
                poll_s=args.tune_poll_s))
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
        ok, body = _retrying(session, args, "tune_all_zones", f"zone{zone}.autotune_accept",
                             session.autotune_accept)
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
        matrix = _retrying(session, args, "coupling_matrix", "autotune_matrix", session.autotune_matrix)
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
        exported = _retrying(session, args, "backup_roundtrip", "backup_export", session.backup_export)
    except BenchSessionError as exc:
        return pv.StageReport(name="backup_roundtrip", passed=False, error=str(exc),
                              duration_s=time.monotonic() - t0)
    if args.backup_path:
        os.makedirs(os.path.dirname(args.backup_path) or ".", exist_ok=True)
        with open(args.backup_path, "w", encoding="utf-8") as fh:
            json.dump(exported, fh, indent=2)
    ok, body = _retrying(session, args, "backup_roundtrip", "backup_import",
                         lambda: session.backup_import(exported))
    if not ok:
        return pv.StageReport(
            name="backup_roundtrip", passed=False,
            error=f"import refused: {body[:300]}", duration_s=time.monotonic() - t0)
    try:
        reimported = _retrying(session, args, "backup_roundtrip", "backup_export_2", session.backup_export)
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
        # autotune_accept() (stage 2) writes gains + model but NOT
        # control_mode -- a zone tuned there can still be sitting in OFF, in
        # which case this whole stage would run a profile against a zone
        # that never actually heats and "pass" on data that measured
        # nothing. Force every tracked zone into PID mode and read the
        # result back to confirm it actually took, same discipline as
        # backup_roundtrip's diff_backup_zones() re-export check.
        modes_after = _retrying(session, args, "profile_tracking", "ensure_pid_mode",
                                lambda: session.ensure_pid_mode(args.zones))
        for zone in args.zones:
            mode = modes_after.get(zone)
            checks.append(pv.CheckResult(
                name=f"zone{zone}.control_mode_pid", passed=mode == ZONE_CONTROL_MODE_PID,
                actual=mode, threshold=ZONE_CONTROL_MODE_PID))
        if not all(c.passed for c in checks):
            return pv.StageReport(name="profile_tracking", passed=False, checks=checks,
                                  error="one or more zones did not confirm PID mode before "
                                        "the profile run -- refusing to start it",
                                  duration_s=time.monotonic() - t0)

        zone_mask = sum(1 << z for z in args.zones)
        segments = _profile_segments_c_to_80(args.tracking_peak1_c, args.tracking_peak2_c)

        # Zone commissioning pre-flight (see pv.check_zones_commissioned's
        # docstring for the full "0 means what, on which field" citations).
        # This is what would have caught "zone 1 and 2 were never
        # commissioned" locally, before the cooldown gate ran, instead of
        # as a bare 400 an hour in. Never auto-corrects: an uncommissioned
        # zone is reported to the operator, not silently widened, unless
        # --commission-zones was explicitly passed (see below).
        current_zones = _retrying(session, args, "profile_tracking", "get_zones_preflight",
                                  session.get_zones)
        zone_configs = {z["index"]: z for z in current_zones.get("zones", [])}
        try:
            pv.check_zones_commissioned(args.zones, zone_configs, segments)
        except pv.ZoneNotCommissionedError as exc:
            if not args.commission_zones:
                return pv.StageReport(
                    name="profile_tracking", passed=False,
                    error="one or more zones are not commissioned for this profile, refusing "
                          "to start it (pass --commission-zones to have the OPERATOR-reviewed "
                          "values below written first): " + "; ".join(exc.failures),
                    detail={"commissioning_failures": exc.failures},
                    duration_s=time.monotonic() - t0)
            # --commission-zones: an explicit, opt-in override. Loudly logged
            # (this widens safety guard ceilings) and limited to the fields
            # this run's pre-flight actually found deficient -- never a bulk
            # "reset everything to some default" write.
            log.warning("COMMISSIONING ZONES (--commission-zones was passed): %s",
                        "; ".join(exc.failures))
            reqs = pv.profile_commissioning_requirements(args.zones, segments)
            commission_preset = {"zones": []}
            for zone in args.zones:
                req = reqs[zone]
                cfg = zone_configs.get(zone, {})
                zentry = {"index": zone}
                if not cfg.get("max_ramp_c_per_hr") or cfg["max_ramp_c_per_hr"] < req.steepest_ramp_c_per_hr:
                    zentry["max_ramp_c_per_hr"] = req.steepest_ramp_c_per_hr
                if not cfg.get("max_temp_c") or cfg["max_temp_c"] < req.highest_target_c:
                    zentry["max_temp_c"] = req.highest_target_c
                if len(zentry) > 1:
                    commission_preset["zones"].append(zentry)
                    log.warning("  zone %d: writing %s", zone,
                               {k: v for k, v in zentry.items() if k != "index"})
            if commission_preset["zones"]:
                _retrying(session, args, "profile_tracking", "commission_zones",
                         lambda: session.commission_zones(current_zones, commission_preset))
            # cross_zone_max_delta_c is NEVER auto-written, even under
            # --commission-zones -- see pv.check_zones_commissioned's
            # docstring: there is no correct default, it depends on how
            # strongly this specific kiln's zones couple, and a wrong guess
            # either nuisance-trips or catches nothing. If that was the
            # (only) remaining failure, re-raise instead of silently
            # proceeding with guard 8 disabled.
            refreshed = _retrying(session, args, "profile_tracking", "get_zones_postcommission",
                                  session.get_zones)
            refreshed_cfgs = {z["index"]: z for z in refreshed.get("zones", [])}
            try:
                pv.check_zones_commissioned(args.zones, refreshed_cfgs, segments)
            except pv.ZoneNotCommissionedError as exc2:
                return pv.StageReport(
                    name="profile_tracking", passed=False,
                    error="--commission-zones could not fully commission every zone (fields "
                          "it never auto-writes, e.g. cross_zone_max_delta_c, still need an "
                          "operator-chosen value): " + "; ".join(exc2.failures),
                    detail={"commissioning_failures": exc2.failures},
                    duration_s=time.monotonic() - t0)

        # Build a name that fits the firmware's PROFILE_NAME_MAX_LEN (15
        # chars, profiles_http.h:29) while still embedding a timestamp so
        # the run is identifiable in the slot afterwards -- see
        # pv.build_tracking_profile_name's docstring.
        profile_name = pv.build_tracking_profile_name()
        # Pre-flight against every firmware bound BEFORE the HTTP call: a
        # 400 discovered here costs nothing, discovered after start_profile()
        # it costs the run. This is exactly the check missing when this
        # stage sent a 23-char name and burned an hour of heating on a 400
        # at t=0.1s.
        try:
            pv.validate_profile_payload(args.tracking_slot, profile_name, zone_mask, segments)
        except pv.ProfilePayloadError as exc:
            return pv.StageReport(name="profile_tracking", passed=False,
                                  error=f"payload would be rejected by firmware, not sent: {exc}",
                                  duration_s=time.monotonic() - t0)
        _retrying(session, args, "profile_tracking", "put_profile",
                 lambda: session.put_profile(args.tracking_slot, profile_name,
                                              zone_mask, segments))
        code, body = _retrying(session, args, "profile_tracking", "start_profile",
                               lambda: session.start_profile(args.tracking_slot))
        if code != 200:
            return pv.StageReport(name="profile_tracking", passed=False,
                                  error=f"start refused: {code}: {body[:300]}",
                                  duration_s=time.monotonic() - t0)
        # B4: deliberately NOT run through _retrying(). sample_response()
        # polls and accumulates rows over the whole tracking_duration_s
        # while the profile keeps executing on the board -- it is not
        # idempotent. Retrying the whole call would throw away every row
        # already collected and re-sample for a fresh full duration starting
        # mid-profile, producing a time-shifted trace compared against
        # expectations for the ORIGINAL start time: a spurious FAIL, or a
        # PASS on data that never covered the ramp. A transient error here
        # must surface as a stage failure (via the BenchSessionError catch
        # below), not be silently retried past.
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

    def status(self) -> dict:
        # Healthy, monotonically-increasing uptime and a benign reset
        # reason by default -- FakeSession never crashes on its own.
        self._uptime = getattr(self, "_uptime", 0.0) + 1.0
        return {"uptime_s": self._uptime, "reset_reason": getattr(self, "_reset_reason", "power-on")}

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

    def ensure_pid_mode(self, zones):
        # Fake board: every zone is already in PID mode, no write needed --
        # exercises the "already correct, nothing to POST" path.
        return {z: ZONE_CONTROL_MODE_PID for z in zones}

    def get_zones(self) -> dict:
        # Fake board: every zone is already commissioned generously (mirrors
        # zone 0's real-hardware values from the bug report) so a dry run
        # exercises the "already commissioned, pre-flight passes" path by
        # default -- see test_pid_validation.py for the failing-path tests,
        # which feed pv.check_zones_commissioned synthetic configs directly
        # rather than going through FakeSession.
        zones = getattr(self, "_zone_cfgs", None)
        if zones is None:
            zones = {z: {"index": z, "max_ramp_c_per_hr": 900.0, "max_temp_c": 80.0,
                          "cross_zone_max_delta_c": 50.0} for z in range(3)}
            self._zone_cfgs = zones
        return {"zones": list(zones.values())}

    def commission_zones(self, current: dict, preset: dict) -> None:
        zones = getattr(self, "_zone_cfgs", None)
        if zones is None:
            self.get_zones()
            zones = self._zone_cfgs
        for entry in preset.get("zones", []):
            zones[entry["index"]].update(entry)

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
    # Retry/board-health state threaded through every stage via _retrying().
    # Seeded with a best-effort initial status BEFORE any stage runs, so a
    # panic-class reset_reason already present on the board at run start is
    # recorded as the baseline (not itself flagged) -- only a CHANGE to a
    # panic reason, or uptime going backwards, mid-run counts as a restart.
    args._retry_events = []
    args._health = pv.BoardHealthTracker()
    try:
        args._health.observe(session.status())
    except Exception:  # noqa: BLE001 - best-effort baseline, never fatal here
        # WARNING, not DEBUG: if this fails, the run has NO baseline and the
        # earliest crash in the run can never be flagged by uptime/reset
        # comparison until some later observation succeeds -- an operator
        # watching the log needs to see that up front, not only by passing
        # -v after the fact.
        log.warning("could not seed board-health baseline before the run", exc_info=True)
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
        report.retries = list(args._retry_events)
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

    g = p.add_argument_group("transient-error retry")
    g.add_argument("--max-retries", type=int, default=pv.DEFAULT_MAX_RETRIES,
                   help=f"retries per HTTP call on a transient error, e.g. timeout/refused/reset "
                        f"(default: {pv.DEFAULT_MAX_RETRIES})")
    g.add_argument("--retry-backoff-s", type=float, default=pv.DEFAULT_RETRY_BACKOFF_S,
                   help=f"wait before the first retry, seconds (default: {pv.DEFAULT_RETRY_BACKOFF_S})")
    g.add_argument("--retry-backoff-multiplier", type=float, default=pv.DEFAULT_RETRY_BACKOFF_MULTIPLIER,
                   help=f"exponential growth per subsequent retry "
                        f"(default: {pv.DEFAULT_RETRY_BACKOFF_MULTIPLIER})")
    g.add_argument("--retry-backoff-max-s", type=float, default=pv.DEFAULT_RETRY_BACKOFF_MAX_S,
                   help=f"backoff cap, seconds (default: {pv.DEFAULT_RETRY_BACKOFF_MAX_S})")

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
    g.add_argument("--tracking-slot", type=int, default=pv.PROFILE_FW_MAX_COUNT - 1,
                   help="user profile slot to write/run the tracking test in "
                        f"(default: {pv.PROFILE_FW_MAX_COUNT - 1}, the last of the firmware's "
                        f"{pv.PROFILE_FW_MAX_COUNT} slots -- profiles_http.h:28). Was 15 (out of "
                        "the firmware's 0-7 range), which the firmware silently reinterpreted as "
                        "\"first free slot\" (profiles_http.c's id>=0 && id<PROFILES_MAX_COUNT "
                        "check) rather than rejecting -- validate_profile_payload() now catches "
                        "any future out-of-range value explicitly instead of relying on that "
                        "fallback.")
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
    g.add_argument("--commission-zones", action="store_true",
                   help="OPT-IN, off by default: if the profile-tracking pre-flight finds a "
                        "zone missing max_ramp_c_per_hr/max_temp_c coverage for this profile, "
                        "write ONLY the deficient fields (never cross_zone_max_delta_c, which "
                        "has no safe default) and log loudly what changed, instead of aborting "
                        "the stage. Default behaviour without this flag is to report the "
                        "requirement and refuse to start -- this harness never silently widens "
                        "a safety guard ceiling.")

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
