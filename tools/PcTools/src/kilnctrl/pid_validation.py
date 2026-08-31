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
# Profile payload pre-flight. Mirrors the firmware's OWN bounds on
# POST /api/profile (firmware/KilnFW/App/drivers/profiles_http.c /
# profiles_http.h) so the harness refuses to start an hour-long heating stage
# it can already predict the firmware will reject with a 400 -- exactly what
# happened when stage 5 (profile_tracking) sent a 23-char name against a
# 15-char firmware limit and burned an hour of kiln heating before failing at
# t=0.1s. Every constant below is cited to the firmware line that owns it;
# these are NOT independently chosen numbers and must be kept in sync by hand
# if the firmware's ever change (there is no shared header to import from
# Python).
# ---------------------------------------------------------------------------

#: firmware/KilnFW/App/drivers/profiles_http.h:28
PROFILE_FW_MAX_COUNT = 8
#: firmware/KilnFW/App/drivers/profiles_http.h:29 -- name buffer is
#: PROFILE_NAME_MAX_LEN+1 bytes; http_form_find_field() returns -2 ("name too
#: long") the instant the field does not fit that +1 including the NUL, i.e.
#: an inclusive max of 15 characters.
PROFILE_FW_NAME_MAX_LEN = 15
#: firmware/KilnFW/App/drivers/profiles_http.h:30 and profiles_http.c:1382
#: ("seg_count out of range (1-12)")
PROFILE_FW_MAX_SEGMENTS = 12
#: firmware/KilnFW/App/drivers/profiles_http.c:102-103
PROFILE_FW_TARGET_C_MIN = 0.0
PROFILE_FW_TARGET_C_MAX = 1400.0
#: firmware/KilnFW/App/drivers/profiles_http.c:104-105
PROFILE_FW_RAMP_C_PER_HR_MIN = 0.0
PROFILE_FW_RAMP_C_PER_HR_MAX = 1000.0
#: firmware/KilnFW/App/drivers/profiles_http.c:106 (24h)
PROFILE_FW_DWELL_MIN_MAX = 1440
#: firmware/KilnFW/App/drivers/profiles_http.c:1367 -- zone_mask must be
#: nonzero and fit a uint8_t; which SPECIFIC bits are valid additionally
#: depends on the board's configured thermocouple count
#: (zones_config_get_thermo_count()), which this harness has no local copy
#: of and cannot check without a live call -- so this pre-flight only checks
#: the bounds that are knowable offline (nonzero, <= 0xFF) and leaves the
#: "is this zone actually configured" check to the firmware's own 400.
PROFILE_FW_ZONE_MASK_MAX = 0xFF


class ProfilePayloadError(ValueError):
    """A profile field would be rejected by the firmware's own /api/profile
    bounds. Raised by ``validate_profile_payload`` BEFORE any HTTP call, so
    the caller never starts a stage (and never heats the kiln) for a
    payload that is predictably a 400."""


def build_tracking_profile_name(prefix: str = "pv", when: "Optional[time.struct_time]" = None) -> str:
    """A profile name that (a) fits PROFILE_FW_NAME_MAX_LEN and (b) still
    lets a human find the right slot afterwards, by embedding a compact
    MMDDHHMM timestamp. ``prefix`` + 8 timestamp digits must total
    <= PROFILE_FW_NAME_MAX_LEN; the default ``"pv"`` (2 chars) + 8 digits is
    10, comfortably under the 15-char firmware limit with room to spare.
    """
    ts = time.strftime("%m%d%H%M", when if when is not None else time.localtime())
    name = f"{prefix}{ts}"
    if len(name) > PROFILE_FW_NAME_MAX_LEN:
        # Truncate rather than raise -- a caller passing a long prefix still
        # gets a usable (if less descriptive) name instead of a hard failure
        # over what is, worst case, a cosmetic label.
        name = name[:PROFILE_FW_NAME_MAX_LEN]
    return name


def validate_profile_payload(slot: int, name: str, zone_mask: int, segments: "list[dict]") -> None:
    """Check ``slot``/``name``/``zone_mask``/``segments`` against every bound
    the firmware's POST /api/profile parser enforces (see the PROFILE_FW_*
    constants above for the exact firmware citations). Raises
    :class:`ProfilePayloadError` naming the offending field on the first
    violation found; returns None if the whole payload would be accepted.

    Deliberately does NOT re-implement zone-mask-vs-configured-thermocouple-
    count (that needs a live board read) or the ramp-rate feasibility check
    (that needs the board's per-zone ceiling) -- both stay as firmware-side
    checks. This function only catches what is knowable from the payload
    alone, which is exactly the class of bug that just cost an hour of
    heating: a value the harness itself constructed being out of bounds.
    """
    if not isinstance(slot, int) or slot < 0 or slot >= PROFILE_FW_MAX_COUNT:
        raise ProfilePayloadError(
            f"slot {slot!r} out of range (0-{PROFILE_FW_MAX_COUNT - 1})")
    if not name or len(name) > PROFILE_FW_NAME_MAX_LEN:
        raise ProfilePayloadError(
            f"name {name!r} ({len(name)} chars) exceeds the firmware's "
            f"{PROFILE_FW_NAME_MAX_LEN}-char limit")
    if not isinstance(zone_mask, int) or zone_mask <= 0 or zone_mask > PROFILE_FW_ZONE_MASK_MAX:
        raise ProfilePayloadError(
            f"zone_mask {zone_mask!r} must select at least one zone and fit a byte "
            f"(1-{PROFILE_FW_ZONE_MASK_MAX})")
    if not segments or len(segments) > PROFILE_FW_MAX_SEGMENTS:
        raise ProfilePayloadError(
            f"segment count {len(segments)} out of range (1-{PROFILE_FW_MAX_SEGMENTS})")
    for i, seg in enumerate(segments):
        target_c = seg.get("target_c")
        if target_c is None or not (PROFILE_FW_TARGET_C_MIN <= target_c <= PROFILE_FW_TARGET_C_MAX):
            raise ProfilePayloadError(
                f"segment {i}: target_c {target_c!r} out of range "
                f"({PROFILE_FW_TARGET_C_MIN}-{PROFILE_FW_TARGET_C_MAX})")
        ramp = seg.get("ramp_c_per_hr")
        if ramp is None or not (PROFILE_FW_RAMP_C_PER_HR_MIN <= ramp <= PROFILE_FW_RAMP_C_PER_HR_MAX):
            raise ProfilePayloadError(
                f"segment {i}: ramp_c_per_hr {ramp!r} out of range "
                f"({PROFILE_FW_RAMP_C_PER_HR_MIN}-{PROFILE_FW_RAMP_C_PER_HR_MAX})")
        dwell = seg.get("dwell_min")
        if dwell is None or not (0 <= dwell <= PROFILE_FW_DWELL_MIN_MAX):
            raise ProfilePayloadError(
                f"segment {i}: dwell_min {dwell!r} out of range (0-{PROFILE_FW_DWELL_MIN_MAX})")


# ---------------------------------------------------------------------------
# Zone commissioning pre-flight.
#
# WHY. Stage 5 (profile tracking) discovered, via a bare 400 AFTER the
# cooldown gate had already run, that zones 1 and 2 were never commissioned:
# autotune had written gains and a fitted model to them, but the separate
# guard-limit fields (max_ramp_c_per_hr, max_temp_c, cross_zone_max_delta_c)
# stayed at their post-flash 0.0. This section checks those fields against
# the profile the harness is about to run, BEFORE it posts anything, so an
# uncommissioned zone fails locally with a message naming the zone, the
# field, the required value and the actual value -- not a 400 an hour into
# a heating run.
#
# The three fields do NOT share one "0 means X" convention -- verified by
# reading the firmware, not assumed:
#
#   * max_ramp_c_per_hr: 0.0 means "never configured", and the firmware
#     treats that as a ceiling of zero -- every nonzero ramp rate is
#     rejected. profiles_http.c:1573-1580 (the comment at the /api/profile
#     PUT handler), enforced at profiles_http.c:870-876 and :1603-1610
#     ("segment %u: ramp rate ... exceeds zone %u's ... ceiling" -- the
#     exact 400 that started this). zones_http.c:344 carries the same "0 =
#     never configured" convention on the struct field itself.
#
#   * max_temp_c: 0.0 means "not set, no ceiling" and DISABLES guard 5
#     (the over-temperature trip) entirely -- thermal_guard.c:106,
#     `cfg->max_temp_c > 0.0f && in->measurement_c >= cfg->max_temp_c`. A
#     zone with max_temp_c == 0.0 is not "unlimited but still checked", the
#     check itself does not run. zones_http.c:347 documents the field the
#     same way ("guard 5; 0 = not set, no ceiling").
#
#   * cross_zone_max_delta_c: 0.0 DISABLES guard 8 (cross-zone plausibility)
#     entirely -- thermal_guard.c:331, `cfg->cross_zone_max_delta_c > 0.0f
#     && in->peer_c && in->peer_count > 0`. zones_http.c:381-389 spells out
#     the asymmetry explicitly in its own doc comment: "0 = 'not
#     configured', which DISABLES the guard rather than substituting a
#     default -- the opposite convention to sanity_rate_c_per_min above".
#
# So max_ramp_c_per_hr at 0 is the MOST restrictive state (nothing is
# feasible), while max_temp_c and cross_zone_max_delta_c at 0 are the LEAST
# restrictive state (the guard is off). Getting this backwards for
# cross_zone_max_delta_c in particular -- treating 0 as "maximally strict"
# instead of "interlock disabled" -- would silently run a multi-zone
# profile with guard 8 switched off while looking commissioned. This
# module's check therefore requires all three fields to be explicitly
# nonzero (and, where meaningful, to actually cover the profile) before
# calling a zone commissioned; it never infers "0 is fine because it's
# permissive" for any of them.
#
# This module NEVER writes these fields -- see the harness's
# --commission-zones flag (scripts/pid_validation_harness.py) for the only,
# explicitly opt-in, path that does.
# ---------------------------------------------------------------------------

@dataclass
class ZoneCommissioningRequirement:
    """What a profile demands of one zone: the steepest ramp rate and
    highest target temperature among ITS OWN segments -- computed once from
    the profile and applied identically to every zone the profile drives,
    since ramp-lock holds every zone's setpoint to the profile's shared
    schedule regardless of which zone is slowest."""
    zone: int
    steepest_ramp_c_per_hr: float
    highest_target_c: float


class ZoneNotCommissionedError(RuntimeError):
    """One or more zones cannot safely run the intended profile. Raised by
    :func:`check_zones_commissioned` BEFORE any HTTP call -- callers must
    report this to the operator, never auto-widen the offending limit (see
    this section's module-level docstring)."""

    def __init__(self, failures: "list[str]") -> None:
        super().__init__("; ".join(failures))
        self.failures = list(failures)


def profile_commissioning_requirements(zones: "list[int]", segments: "list[dict]"
                                        ) -> "dict[int, ZoneCommissioningRequirement]":
    """Steepest ``ramp_c_per_hr`` and highest ``target_c`` across
    ``segments`` (a dwell segment with ``ramp_c_per_hr`` 0/None does not
    count toward the ramp requirement), applied to every zone in ``zones``.
    """
    ramps = [float(s["ramp_c_per_hr"]) for s in segments
             if s.get("ramp_c_per_hr")]
    steepest = max(ramps) if ramps else 0.0
    targets = [float(s["target_c"]) for s in segments if s.get("target_c") is not None]
    highest = max(targets) if targets else 0.0
    return {z: ZoneCommissioningRequirement(zone=z, steepest_ramp_c_per_hr=steepest,
                                            highest_target_c=highest)
            for z in zones}


def check_zone_commissioned(req: ZoneCommissioningRequirement, zone_cfg: dict) -> "Optional[str]":
    """None when ``zone_cfg`` (one zone's config as returned by GET
    /api/zones) is commissioned to run ``req`` safely; otherwise a message
    naming the zone, the offending field, the required value and the
    actual value. See this section's module docstring for the zero-
    semantics citations behind each branch below.
    """
    zone = req.zone
    max_ramp = zone_cfg.get("max_ramp_c_per_hr")
    max_temp = zone_cfg.get("max_temp_c")
    cross_zone = zone_cfg.get("cross_zone_max_delta_c")

    if max_ramp is None or max_ramp <= 0.0:
        return (f"zone {zone}: max_ramp_c_per_hr={max_ramp!r} -- 0/unset means 'never "
                f"configured', which the firmware treats as a ceiling of zero and rejects "
                f"every nonzero ramp rate against (profiles_http.c:1576-1580); needs "
                f">= {req.steepest_ramp_c_per_hr:.1f} C/hr for this profile's steepest segment")
    if req.steepest_ramp_c_per_hr > max_ramp:
        return (f"zone {zone}: max_ramp_c_per_hr={max_ramp:.1f} C/hr is below this "
                f"profile's steepest segment ({req.steepest_ramp_c_per_hr:.1f} C/hr required)")

    if max_temp is None or max_temp <= 0.0:
        return (f"zone {zone}: max_temp_c={max_temp!r} -- 0/unset DISABLES guard 5's "
                f"over-temperature trip entirely (thermal_guard.c:106), it does not mean "
                f"'no limit but still checked'; needs >= {req.highest_target_c:.1f} C for "
                f"this profile's highest target")
    if req.highest_target_c > max_temp:
        return (f"zone {zone}: max_temp_c={max_temp:.1f} C is below this profile's "
                f"highest target ({req.highest_target_c:.1f} C required)")

    if cross_zone is None or cross_zone <= 0.0:
        return (f"zone {zone}: cross_zone_max_delta_c={cross_zone!r} -- 0/unset DISABLES "
                f"guard 8's cross-zone plausibility interlock entirely (thermal_guard.c:331, "
                f"zones_http.c:383-389), the opposite convention from max_ramp_c_per_hr; must "
                f"be set to a nonzero value before running a multi-zone profile against this zone")
    return None


def check_zones_commissioned(zones: "list[int]", zone_configs: "dict[int, dict]",
                              segments: "list[dict]") -> None:
    """Raises :class:`ZoneNotCommissionedError` (naming every zone/field/
    required/actual mismatch found) if any zone in ``zones`` is not
    commissioned to run ``segments`` safely. ``zone_configs`` is
    ``{zone_index: zone_dict}`` from an already-fetched GET /api/zones --
    this function makes no HTTP calls of its own and never mutates
    anything; a caller with a zone missing from ``zone_configs`` gets a
    failure naming that zone rather than a KeyError.
    """
    reqs = profile_commissioning_requirements(zones, segments)
    failures: "list[str]" = []
    for zone in zones:
        cfg = zone_configs.get(zone)
        if cfg is None:
            failures.append(f"zone {zone}: no zone config returned by the board")
            continue
        reason = check_zone_commissioned(reqs[zone], cfg)
        if reason:
            failures.append(reason)
    if failures:
        raise ZoneNotCommissionedError(failures)


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
# Retry policy for transient HTTP failures.
#
# WHY. A run costs an hour of real heating. The harness used to treat every
# HTTP error the same way -- one urlopen timeout during /api/autotune killed
# an otherwise-healthy run. That specific instance turned out to be a real
# firmware panic, so aborting was correct THAT time; but a transient timeout,
# a momentary mDNS hiccup, or one slow response must not get the same
# treatment. This section is deliberately split into small pure pieces so
# each half of "retry the blip, abort the crash" can be proven independently:
# retry_backoff_s / call_with_retry / is_transient_http_error cover the
# retry half, detect_board_restart / BoardHealthTracker cover the "but not
# past a dead board" half.
# ---------------------------------------------------------------------------

#: Retries beyond the first attempt. 4 retries (5 attempts total) survives a
#: multi-second mDNS re-resolve or a couple of slow control-loop ticks
#: without turning a genuinely dead board into a long silent hang.
DEFAULT_MAX_RETRIES = 4
#: First retry waits this long.
DEFAULT_RETRY_BACKOFF_S = 3.0
#: Exponential growth per subsequent retry.
DEFAULT_RETRY_BACKOFF_MULTIPLIER = 2.0
#: Backoff ceiling -- with the defaults above, uncapped growth would reach
#: 3 * 2**3 = 24s by the 4th retry already; the cap exists so a caller who
#: raises max_retries doesn't end up sleeping for minutes between attempts.
DEFAULT_RETRY_BACKOFF_MAX_S = 30.0


@dataclass
class RetryPolicy:
    """Command-line-configurable retry policy. See the module-level
    DEFAULT_* constants for the defaults and their rationale."""
    max_retries: int = DEFAULT_MAX_RETRIES
    backoff_s: float = DEFAULT_RETRY_BACKOFF_S
    backoff_multiplier: float = DEFAULT_RETRY_BACKOFF_MULTIPLIER
    backoff_max_s: float = DEFAULT_RETRY_BACKOFF_MAX_S


def retry_backoff_s(attempt: int, policy: "RetryPolicy") -> float:
    """Delay before retry attempt number ``attempt`` (1-based: ``attempt=1``
    is the wait after the FIRST failure, before the first retry).
    Exponential, capped at ``policy.backoff_max_s``.

    NEGATIVE TEST target: an uncapped implementation would keep growing
    without bound -- this must visibly top out instead.
    """
    if attempt < 1:
        raise ValueError(f"attempt must be >= 1, got {attempt}")
    raw = policy.backoff_s * (policy.backoff_multiplier ** (attempt - 1))
    return min(raw, policy.backoff_max_s)


#: Substring markers (lower-cased) that indicate a TRANSIENT network problem
#: rather than a real failure worth aborting for. Matched against the str()
#: of the exception -- this project's BenchSessionError wraps urllib's own
#: OSError/URLError text verbatim (see bench_fixture_session._http), so
#: these are the phrases that actually land here on a timeout, a refused/
#: reset connection, or an mDNS resolution hiccup.
TRANSIENT_HTTP_MARKERS = (
    "timed out", "connection refused", "connection reset",
    "connection aborted", "temporarily unavailable", "network is unreachable",
    "no route to host", "name or service not known", "getaddrinfo failed",
    "econnreset", "econnrefused",
    # Windows (this platform) phrases urllib's OSError text completely
    # differently from the Linux/glibc strings above -- these are what a
    # rebooting-then-refusing / reset-mid-request ESP32 actually produces
    # here, verified empirically (see the B2 fix writeup). Kept alongside,
    # not instead of, the Linux markers: this must work on both.
    "winerror 10061", "actively refused",       # WSAECONNREFUSED
    "winerror 10054", "forcibly closed",         # WSAECONNRESET
    "winerror 10060", "did not properly respond",  # WSAETIMEDOUT
)
#: Deliberately NOT a marker: a bare "timeout" substring would match any
#: non-200 response body text that happens to mention the word (e.g. a
#: BenchSessionError wrapping "GET ... -> 500: profile timeout warning"),
#: which is not a network-transport problem at all. "timed out" above is
#: the actual urllib/OS phrasing and is specific enough to keep.


def is_transient_http_error(exc: BaseException) -> bool:
    """True when ``exc`` looks like a transient network blip worth retrying.

    Deliberately narrow: an exception whose message matches none of
    ``TRANSIENT_HTTP_MARKERS`` is NOT transient. Treating every unrecognized
    exception as retryable is exactly the "silently retry past a dead
    board" failure mode this whole mechanism exists to avoid -- an unknown
    error must fall through to an abort, not a retry loop.
    """
    msg = str(exc).lower()
    return any(marker in msg for marker in TRANSIENT_HTTP_MARKERS)


class RetryExhausted(RuntimeError):
    """Raised by :func:`call_with_retry` when every retry was itself a
    transient failure. Always fatal -- callers must abort loudly, never
    treat this as a pass."""

    def __init__(self, attempts: int, last_error: BaseException) -> None:
        super().__init__(
            f"exhausted {attempts} attempt(s), last error: {last_error}")
        self.attempts = attempts
        self.last_error = last_error


def call_with_retry(fn, *, policy: "RetryPolicy", is_transient=is_transient_http_error,
                     on_retry=None, sleep_fn=time.sleep):
    """Call ``fn()`` (no arguments), retrying on exceptions ``is_transient``
    classifies as transient, up to ``policy.max_retries`` times.

    * A non-transient exception is re-raised IMMEDIATELY, unretried -- this
      is what keeps a real crash from being retried past.
    * A transient exception, once retries are exhausted, is re-raised
      wrapped in :class:`RetryExhausted` so callers can tell "gave up after
      N attempts" apart from "failed on the first try".
    * ``on_retry(attempt, wait_s, exc)`` -- if given -- fires before each
      sleep, so a caller can log/record what happened without this function
      knowing anything about reports or logging.
    * ``sleep_fn`` is injectable so tests exercise the real backoff
      schedule without actually sleeping.
    """
    attempt = 0
    while True:
        try:
            return fn()
        except Exception as exc:  # noqa: BLE001 - reclassified below
            if not is_transient(exc):
                raise
            attempt += 1
            if attempt > policy.max_retries:
                raise RetryExhausted(attempt - 1, exc) from exc
            wait_s = retry_backoff_s(attempt, policy)
            if on_retry is not None:
                on_retry(attempt, wait_s, exc)
            sleep_fn(wait_s)


@dataclass
class RetryEvent:
    """One retried attempt, recorded so the run report shows every retry
    that happened -- a run that "passed" after 30 retries must be visibly
    different from one that passed cleanly."""
    stage: str
    operation: str
    attempt: int
    wait_s: float
    error: str

    def to_dict(self) -> dict:
        return asdict(self)


# ---------------------------------------------------------------------------
# Distinguishing a transient blip from a dead/rebooted board.
# ---------------------------------------------------------------------------

#: reset_reason strings (see dashboard_http.c's reset_reason_name) that name
#: a crash-class reboot rather than a deliberate/benign one. A board that
#: rebooted for one of these reasons must never be reported as having
#: survived a "transient" HTTP blip.
PANIC_RESET_REASONS = frozenset({
    "panic/exception", "interrupt watchdog", "task watchdog", "other watchdog",
    "brownout",
})


class BoardRestartDetected(RuntimeError):
    """The board evidently rebooted mid-run. Always fatal."""

    def __init__(self, reason: str) -> None:
        super().__init__(reason)
        self.reason = reason


def detect_board_restart(prev_uptime_s: "Optional[float]", prev_reset_reason: "Optional[str]",
                          status: dict) -> "Optional[str]":
    """Pure decision: does ``status`` (a fresh ``/api/status`` snapshot) show
    evidence the board rebooted since ``prev_uptime_s``/``prev_reset_reason``
    were last observed? Returns the (loud, human-readable) reason string, or
    ``None`` when nothing indicates a restart.

    Two independent signals, either is sufficient:

    * ``uptime_s`` went backwards -- the clearest possible proof of a
      reboot, independent of whether ``reset_reason`` names anything
      recognizable.
    * ``reset_reason`` CHANGED to a panic/watchdog/brownout-class value --
      catches a reboot fast enough that uptime hasn't visibly moved
      backwards relative to the last observation (e.g. a request landed
      just after boot).

    ``prev_reset_reason`` matters, not just membership in
    :data:`PANIC_RESET_REASONS`: a board can have a panic in its OWN
    history from before this run started, and that stale fact must not
    trip an abort on the first observation of a run -- only a reason that
    *changed* to a panic class mid-run is evidence something just crashed.
    """
    new_uptime = status.get("uptime_s")
    if new_uptime is not None:
        try:
            new_uptime = float(new_uptime)
        except (TypeError, ValueError):
            # A non-numeric uptime is a malformed/garbled response, not
            # proof of anything -- treat it as absent rather than letting a
            # bare comparison below raise TypeError past this function's
            # callers as an "unhandled exception".
            new_uptime = None
    reset_reason = status.get("reset_reason")
    if prev_uptime_s is not None and new_uptime is not None and new_uptime < prev_uptime_s:
        return (f"board uptime went backwards ({prev_uptime_s:.0f}s -> {new_uptime:.0f}s) -- "
                f"the board restarted; reset_reason={reset_reason!r}")
    if (reset_reason in PANIC_RESET_REASONS and prev_reset_reason is not None
            and reset_reason != prev_reset_reason):
        return (f"board reset_reason changed to {reset_reason!r} (was {prev_reset_reason!r}) -- "
                f"the board crashed and rebooted")
    return None


@dataclass
class BoardHealthTracker:
    """Stateful wrapper around :func:`detect_board_restart` for the harness's
    retry loop: remembers the last-observed uptime/reset_reason and updates
    on every call, so callers don't have to thread that state through by
    hand. Call :meth:`observe` with each fresh ``/api/status`` snapshot;
    the FIRST call ever establishes the baseline and never itself reports a
    restart (there is nothing to compare against yet)."""
    last_uptime_s: "Optional[float]" = None
    last_reset_reason: "Optional[str]" = None

    def observe(self, status: dict) -> "Optional[str]":
        reason = detect_board_restart(self.last_uptime_s, self.last_reset_reason, status)
        # Coerce the same way detect_board_restart does, and keep the last
        # KNOWN-GOOD baseline on a missing/non-numeric uptime rather than
        # overwriting it with None -- an absent reading must not silently
        # disable restart detection for every observation after it.
        raw_uptime = status.get("uptime_s")
        if raw_uptime is not None:
            try:
                self.last_uptime_s = float(raw_uptime)
            except (TypeError, ValueError):
                pass
        reset_reason = status.get("reset_reason")
        if reset_reason is not None:
            self.last_reset_reason = reset_reason
        return reason


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
    #: Every retried attempt across the whole run (see RetryEvent) -- so a
    #: run that "passed" after 30 retries is visibly not the same as one
    #: that passed cleanly.
    retries: "list[RetryEvent]" = field(default_factory=list)

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
            "retries": [r.to_dict() for r in self.retries],
            "retry_count": len(self.retries),
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
        if self.retries:
            lines.append(f"  RETRIES: {len(self.retries)} transient-error retr"
                          f"{'y' if len(self.retries) == 1 else 'ies'} occurred during this run")
            for r in self.retries:
                lines.append(f"         [retry] {r.stage}.{r.operation} attempt {r.attempt} "
                              f"(waited {r.wait_s:.1f}s): {r.error}")
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
    "PROFILE_FW_MAX_COUNT", "PROFILE_FW_NAME_MAX_LEN", "PROFILE_FW_MAX_SEGMENTS",
    "PROFILE_FW_TARGET_C_MIN", "PROFILE_FW_TARGET_C_MAX",
    "PROFILE_FW_RAMP_C_PER_HR_MIN", "PROFILE_FW_RAMP_C_PER_HR_MAX",
    "PROFILE_FW_DWELL_MIN_MAX", "PROFILE_FW_ZONE_MASK_MAX",
    "ProfilePayloadError", "build_tracking_profile_name", "validate_profile_payload",
    "ZoneCommissioningRequirement", "ZoneNotCommissionedError",
    "profile_commissioning_requirements", "check_zone_commissioned", "check_zones_commissioned",
    "compute_spread_c", "TrackingSample", "ZoneTrackingStats", "tracking_stats",
    "TrackingThresholds", "CheckResult", "evaluate_zone_tracking", "evaluate_spread",
    "autotune_refusal_reason", "diff_backup_zones",
    "StageReport", "RunReport", "now_iso",
    "DEFAULT_MAX_RETRIES", "DEFAULT_RETRY_BACKOFF_S", "DEFAULT_RETRY_BACKOFF_MULTIPLIER",
    "DEFAULT_RETRY_BACKOFF_MAX_S", "RetryPolicy", "retry_backoff_s",
    "TRANSIENT_HTTP_MARKERS", "is_transient_http_error", "RetryExhausted",
    "call_with_retry", "RetryEvent",
    "PANIC_RESET_REASONS", "BoardRestartDetected", "detect_board_restart", "BoardHealthTracker",
]
