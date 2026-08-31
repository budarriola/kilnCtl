#!/usr/bin/env python3
"""Pure decision logic for the PID-expansion hardware-in-the-loop harness.

WHY THIS IS SPLIT OUT. ``scripts/pid_validation_harness.py`` drives the real
board: cooldown polling, autotune start/wait/accept, matrix capture, backup
export/import, and a closed-loop profile run, all over HTTP with sleeps in
between. None of THAT is what makes the harness trustworthy -- what makes it
trustworthy is whether "max error 2.1 C against a 2.0 C threshold" correctly
reports FAIL. That is arithmetic and comparisons, and it belongs here, where
it can be exercised on a laptop with synthetic data and no board, exactly the
way ``tests/bench_fixture_session.py`` split ``cooldown_target_c()`` /
``cooldown_reached()`` out of ``BenchSession.wait_for_cooldown()`` for the
same reason (see ``tests/test_cooldown_policy.py``'s docstring).

Every threshold check in this module is a NEGATIVE-TESTABLE assertion: for
each one, ``tests/test_pid_validation.py`` first proves it can fail (feeds
data on the wrong side of the line) before trusting it to pass. An assertion
this module cannot be shown failing has no business gating a run.
"""
from __future__ import annotations

import json
import math
import time
from dataclasses import dataclass, field, asdict
from typing import Any, Optional


# ---------------------------------------------------------------------------
# Safety ceiling. This is the harness's OWN hard stop -- independent of, and
# in addition to, whatever the firmware's own safety processor enforces. A
# harness that only trusted the firmware's ceiling would have no story for
# "the firmware ceiling itself is misconfigured this run".
# ---------------------------------------------------------------------------

#: Default hard maximum. Above the bench fixture's normal 80 C ceiling
#: (bench_fixture_session.FIXTURE_MAX_TEMP_C) on purpose: this is the
#: harness's LAST-RESORT trip, not the working limit -- the working limit is
#: whatever config/profile the run uses, which should never get close to
#: this number. Command-line configurable; a caller running on a different
#: rig with a different ceiling can lower or raise it, but the harness never
#: silently assumes a higher one than it was told about.
DEFAULT_HARD_MAX_TEMP_C = 90.0


class HardMaxExceeded(RuntimeError):
    """The harness's own ceiling was breached. Always fatal, always aborts."""

    def __init__(self, hottest_c: float, limit_c: float, where: str) -> None:
        super().__init__(
            f"hard max temp exceeded during {where}: {hottest_c:.2f} C > {limit_c:.2f} C limit")
        self.hottest_c = hottest_c
        self.limit_c = limit_c
        self.where = where


def check_hard_max(hottest_c: float, limit_c: float, where: str) -> None:
    """Raise :class:`HardMaxExceeded` if ``hottest_c`` breaches ``limit_c``.

    NaN-safe the same way ``bench_fixture_session.cooldown_reached`` is: a
    channel that stopped reporting is not evidence of safety, so a NaN
    reading must never look like "under the limit" here. It is also not
    itself an over-limit reading (there's nothing to compare), so it passes
    THIS check silently -- callers that need "must have a valid reading" make
    that a separate assertion.
    """
    if hottest_c != hottest_c:  # NaN
        return
    if hottest_c > limit_c:
        raise HardMaxExceeded(hottest_c, limit_c, where)


# ---------------------------------------------------------------------------
# Spread across zones
# ---------------------------------------------------------------------------

def compute_spread_c(readings: "dict[int, float]") -> float:
    """max - min across the zones present in ``readings``.

    NEGATIVE TEST target: a caller that passes a single reading gets 0.0 --
    not an error, not NaN -- because "one zone" has no spread to report; a
    caller that passes zero readings gets NaN, because there THEN is nothing
    to compute and a 0.0 here would silently read as "perfectly matched".
    """
    values = [v for v in readings.values() if v == v]  # drop NaN
    if not values:
        return float("nan")
    return max(values) - min(values)


# ---------------------------------------------------------------------------
# Tracking-error math
# ---------------------------------------------------------------------------

@dataclass
class TrackingSample:
    """One sample of one zone's tracking during the profile-run stage."""
    t_s: float
    zone: int
    measured_c: float
    setpoint_c: float
    in_hold: bool = False


@dataclass
class ZoneTrackingStats:
    zone: int
    max_abs_err_c: float
    rms_err_c: float
    steady_state_err_c: float
    n_samples: int
    n_hold_samples: int


def tracking_stats(samples: "list[TrackingSample]", zone: int) -> ZoneTrackingStats:
    """Reduce one zone's samples to the three numbers the report thresholds
    against.

    * ``max_abs_err_c`` -- worst instantaneous |measured - setpoint| over the
      WHOLE run (ramps included): the number that answers "did it ever get
      badly wrong", not just "was it wrong at the end".
    * ``rms_err_c`` -- root-mean-square error over the whole run: a single
      number for overall tracking quality that one outlier sample cannot
      dominate the way ``max_abs_err_c`` can, nor hide the way a mean error
      (which cancels + and -) can.
    * ``steady_state_err_c`` -- mean SIGNED error restricted to samples
      flagged ``in_hold``. Signed, not absolute: a controller that is
      consistently 1.5 C low at steady state is a different finding (probably
      needs more Ki) from one that is 0.1 C off but wanders +/-1.5 C, and the
      RMS number above already reports the latter. NaN when no hold samples
      were taken -- never silently 0.0, which would read as "verified
      accurate" for a run that never reached steady state at all.
    """
    zs = [s for s in samples if s.zone == zone]
    if not zs:
        raise ValueError(f"no samples for zone {zone}")
    errs = [s.measured_c - s.setpoint_c for s in zs]
    max_abs = max(abs(e) for e in errs)
    rms = math.sqrt(sum(e * e for e in errs) / len(errs))
    hold_errs = [s.measured_c - s.setpoint_c for s in zs if s.in_hold]
    steady = (sum(hold_errs) / len(hold_errs)) if hold_errs else float("nan")
    return ZoneTrackingStats(
        zone=zone, max_abs_err_c=max_abs, rms_err_c=rms,
        steady_state_err_c=steady, n_samples=len(zs), n_hold_samples=len(hold_errs))


# ---------------------------------------------------------------------------
# Thresholds and pass/fail
# ---------------------------------------------------------------------------

@dataclass
class TrackingThresholds:
    """Command-line-configurable gates for the profile-tracking stage.

    Defaults are deliberately loose-but-meaningful for a 3-zone bench rig
    with type-K + MAX31856 noise (~0.3-0.5 C) riding on top of real thermal
    lag: tight enough that a genuinely broken loop (a sign error, an
    unclamped integrator, a zone with no gains) fails loudly, loose enough
    that ordinary sensor noise does not.
    """
    max_abs_err_c: float = 5.0
    rms_err_c: float = 2.0
    steady_state_err_c: float = 1.5
    spread_c: float = 2.0


@dataclass
class CheckResult:
    name: str
    passed: bool
    actual: Any
    threshold: Any
    detail: str = ""

    def to_dict(self) -> dict:
        return asdict(self)


def evaluate_zone_tracking(stats: ZoneTrackingStats, thresholds: TrackingThresholds
                            ) -> "list[CheckResult]":
    """One :class:`CheckResult` per gated number for this zone. Kept as a
    list rather than a single bool so the report can show every number even
    when only one of them failed."""
    out = [
        CheckResult(
            name=f"zone{stats.zone}.max_abs_err_c",
            passed=stats.max_abs_err_c <= thresholds.max_abs_err_c,
            actual=stats.max_abs_err_c, threshold=thresholds.max_abs_err_c),
        CheckResult(
            name=f"zone{stats.zone}.rms_err_c",
            passed=stats.rms_err_c <= thresholds.rms_err_c,
            actual=stats.rms_err_c, threshold=thresholds.rms_err_c),
    ]
    if stats.n_hold_samples > 0:
        ss_ok = abs(stats.steady_state_err_c) <= thresholds.steady_state_err_c
        out.append(CheckResult(
            name=f"zone{stats.zone}.steady_state_err_c", passed=ss_ok,
            actual=stats.steady_state_err_c, threshold=thresholds.steady_state_err_c))
    else:
        # A hold phase that produced zero samples is a HARNESS bug (bad
        # sample period vs. hold duration, or the executor never reached the
        # hold), not evidence of good tracking -- report it as a failed
        # check rather than omitting the number silently.
        out.append(CheckResult(
            name=f"zone{stats.zone}.steady_state_err_c", passed=False,
            actual=None, threshold=thresholds.steady_state_err_c,
            detail="no hold-phase samples were collected for this zone"))
    return out


def evaluate_spread(per_sample_spreads: "list[float]", thresholds: TrackingThresholds
                     ) -> CheckResult:
    """Worst spread seen across the run, gated against ``thresholds.spread_c``.

    Worst, not average: a spread threshold exists to catch one zone falling
    badly behind its siblings, and an average over the whole run (much of it
    spent close together during ramps that start together) would wash that
    moment out.
    """
    valid = [s for s in per_sample_spreads if s == s]
    worst = max(valid) if valid else float("nan")
    passed = (worst == worst) and worst <= thresholds.spread_c
    return CheckResult(name="max_zone_spread_c", passed=passed, actual=worst,
                        threshold=thresholds.spread_c,
                        detail="" if valid else "no valid multi-zone samples to compute a spread")


# ---------------------------------------------------------------------------
# Autotune refusal / acceptance
# ---------------------------------------------------------------------------

def autotune_refusal_reason(status: dict) -> "Optional[str]":
    """None when the fit is clean and acceptable; otherwise the reason,
    straight from the firmware's own ``refusal``/``refusal_reason`` fields
    (see dashboard_http.c's ``autotune_refusal_name`` -- "ok" is the only
    passing value, anything else, including a state that never reached
    "done", is a refusal this harness must not paper over).
    """
    state = status.get("state")
    if state == "aborted":
        return f"autotune aborted: {status.get('abort_reason') or '(no reason reported)'}"
    if state != "done":
        return f"autotune did not reach 'done' (state={state!r})"
    if not status.get("model_valid"):
        return "fitted model is not valid (model_valid=false)"
    refusal = status.get("refusal")
    if refusal not in (None, "ok"):
        return f"gains refused: {refusal} ({status.get('refusal_reason') or 'no detail'})"
    return None


# ---------------------------------------------------------------------------
# Backup export/import round-trip diff
# ---------------------------------------------------------------------------

#: Fields compared per zone. Anything NOT in this list (e.g. a field one side
#: omits because zones_config_get_model() legitimately answers false) is
#: intentionally out of scope -- see diff_backup_zones()'s docstring.
_BACKUP_ZONE_FIELDS = (
    "pid_kp", "pid_ki", "pid_kd", "model_k_dc", "model_tau_s", "model_dead_time_s",
)


def diff_backup_zones(exported: dict, reimported: dict, tol: float = 1e-3
                       ) -> "list[str]":
    """Compare the ``zones`` array of two ``/api/backup/export`` documents.

    This is stage 4's actual proof-of-life: the brief says the export path
    "has never been tested" and a silent no-op (export returns something,
    import 200s, but nothing actually changed) must not read as a pass. So
    this compares the RE-EXPORTED config after import against the ORIGINAL
    export, field by field, and returns every mismatch found -- an empty
    list is the only passing result, and callers must not treat "import
    returned 200" as sufficient on its own.

    A zone present in one document and absent in the other is ALSO a
    mismatch (not silently skipped): the whole point is catching an import
    that dropped a zone's config entirely.
    """
    mismatches: "list[str]" = []
    exp_zones = {z["index"]: z for z in exported.get("zones", [])}
    imp_zones = {z["index"]: z for z in reimported.get("zones", [])}
    for idx in sorted(set(exp_zones) | set(imp_zones)):
        if idx not in exp_zones:
            mismatches.append(f"zone {idx}: present after re-import but not in the original export")
            continue
        if idx not in imp_zones:
            mismatches.append(f"zone {idx}: present in the original export but missing after re-import")
            continue
        a, b = exp_zones[idx], imp_zones[idx]
        for field_name in _BACKUP_ZONE_FIELDS:
            av, bv = a.get(field_name), b.get(field_name)
            if av is None or bv is None:
                if av != bv:
                    mismatches.append(f"zone {idx}.{field_name}: {av!r} vs {bv!r} (one side missing)")
                continue
            if abs(float(av) - float(bv)) > tol:
                mismatches.append(f"zone {idx}.{field_name}: exported {av!r} != re-imported {bv!r}")
    return mismatches


# ---------------------------------------------------------------------------
# Report assembly
# ---------------------------------------------------------------------------

@dataclass
class StageReport:
    name: str
    passed: bool
    skipped: bool = False
    checks: "list[CheckResult]" = field(default_factory=list)
    detail: "dict[str, Any]" = field(default_factory=dict)
    error: "Optional[str]" = None
    duration_s: float = 0.0

    def to_dict(self) -> dict:
        d = asdict(self)
        return d


@dataclass
class RunReport:
    host: str
    started_at: str
    dry_run: bool
    hard_max_temp_c: float
    stages: "list[StageReport]" = field(default_factory=list)
    finished_at: "Optional[str]" = None
    aborted: bool = False
    abort_reason: "Optional[str]" = None

    @property
    def passed(self) -> bool:
        """Overall pass: no abort, and every non-skipped stage passed. A run
        with zero stages executed (everything skipped) is NOT a pass -- see
        test_pid_validation.py's negative test for this."""
        if self.aborted:
            return False
        ran = [s for s in self.stages if not s.skipped]
        if not ran:
            return False
        return all(s.passed for s in ran)

    def to_dict(self) -> dict:
        d = {
            "host": self.host, "started_at": self.started_at, "finished_at": self.finished_at,
            "dry_run": self.dry_run, "hard_max_temp_c": self.hard_max_temp_c,
            "aborted": self.aborted, "abort_reason": self.abort_reason,
            "overall_passed": self.passed,
            "stages": [s.to_dict() for s in self.stages],
        }
        return d

    def to_json(self) -> str:
        return json.dumps(self.to_dict(), indent=2, sort_keys=False)

    def summary_text(self) -> str:
        lines = [
            f"PID validation run: host={self.host} dry_run={self.dry_run}",
            f"  started  {self.started_at}",
            f"  finished {self.finished_at}",
        ]
        if self.aborted:
            lines.append(f"  ABORTED: {self.abort_reason}")
        for s in self.stages:
            if s.skipped:
                lines.append(f"  [SKIP] {s.name}")
                continue
            status = "PASS" if s.passed else "FAIL"
            lines.append(f"  [{status}] {s.name} ({s.duration_s:.1f}s)")
            if s.error:
                lines.append(f"         error: {s.error}")
            for c in s.checks:
                cstatus = "ok " if c.passed else "BAD"
                lines.append(f"         [{cstatus}] {c.name}: actual={c.actual} threshold={c.threshold}"
                              + (f" -- {c.detail}" if c.detail else ""))
        lines.append(f"OVERALL: {'PASS' if self.passed else 'FAIL'}")
        return "\n".join(lines)


def now_iso() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%S", time.localtime())


__all__ = [
    "DEFAULT_HARD_MAX_TEMP_C", "HardMaxExceeded", "check_hard_max",
    "compute_spread_c", "TrackingSample", "ZoneTrackingStats", "tracking_stats",
    "TrackingThresholds", "CheckResult", "evaluate_zone_tracking", "evaluate_spread",
    "autotune_refusal_reason", "diff_backup_zones",
    "StageReport", "RunReport", "now_iso",
]
