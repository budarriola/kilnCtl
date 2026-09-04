"""Prepends a stabilisation hold onto an existing profile's segments --
the recommended fix for the profile-7 ambient-start confound documented in
``firmware/KilnFW/docs/PID_EXPANSION_PLAN.md`` ("A/B campaign ambient-confound
protocol"). See that section for the full writeup; this module is just the
mechanical piece: given the segments an A/B arm was already going to run,
return a new segment list with one extra ZONE_RAMP segment in front that
ramps to a fixed, above-worst-case-ambient setpoint and holds there long
enough for every zone to settle, before the original (unscored) segments
begin.

WHY A PROFILE SEGMENT AND NOT A RUNNER-SIDE PRE-HOLD. profile_executor's
segment machinery (``firmware/KilnFW/App/drivers/profiles_http.h``'s
``profile_segment_t``: ``target_c`` / ``ramp_c_per_hr`` / ``dwell_min``, kind
``PROFILE_SEG_KIND_ZONE_RAMP``) already expresses exactly "ramp to a target,
then hold there for N minutes" -- that IS a stabilisation hold. No new
firmware is needed: this is a profile DEFINITION problem, solved entirely by
prepending one segment, never a code change. (What the firmware does NOT
have is a "hold until settled to a tolerance" segment kind -- dwell_min is a
fixed duration, not a settling criterion -- so ``dwell_min`` here has to be
chosen generously enough that a worst-case ambient start still settles well
inside it; see ``pick_stabilization_dwell_min``.)

WHY PREPEND RATHER THAN HAND-EDIT EACH PROFILE. Every existing A/B arm's
segment list (already tuned, already validated) is reused byte-for-byte --
only a new segment 0 is inserted ahead of it. Segment indices of the
original schedule all shift up by one, which is exactly why
``pid_ab_compare.STABILIZATION_SEGMENT_INDEX`` (1) exists: the scored window
starts at the original schedule's new position, segment 1.
"""
from __future__ import annotations

from typing import Sequence

from .devices_profiles import ProfileSegment

#: Mirrors firmware/KilnFW/App/drivers/profiles_http.h's PROFILE_MAX_SEGMENTS.
#: Not imported (no python binding for the C header) -- kept in sync by hand;
#: see that header if this ever needs bumping.
PROFILE_MAX_SEGMENTS = 12

#: Ambient on this bench has been measured 28-31C (PID_EXPANSION_PLAN.md's
#: ambient-confound protocol note) and one firing raises the working ambient
#: ~1.4-1.5C further -- so a worst-case room/enclosure start is comfortably
#: under 33C. 48C sits well above that with margin, well below the coupled
#: hold solve's ~60C validity ceiling (infeasible above ~62C -- see
#: project_ff_hold_infeasible_above_62c), and far below profile 7's own
#: ~70C top (this bench never fires above 70C; cone temperatures are
#: simulator-only).
DEFAULT_STABILIZATION_TARGET_C = 48.0

#: A conservative ramp rate for the stabilisation segment itself -- fast
#: enough not to waste bench time, slow enough not to fight the same
#: guard/ramp-lock machinery the scored segments will use. Not critical:
#: this segment is never scored (see STABILIZATION_SEGMENT_INDEX), so its
#: own tracking quality doesn't matter, only that it reliably reaches and
#: holds the target.
DEFAULT_STABILIZATION_RAMP_C_PER_HR = 300.0

#: Fixed-duration hold long enough that a worst-case ~5C ambient spread
#: (the widest range this rig has actually shown across a repeat set,
#: pid_ab_compare.py's 2026-09-02f note) settles out. Chosen generously:
#: the "how long does 45-50C settling actually take on this bench" number
#: has not been measured directly (no full-settle capture through a
#: stabilisation hold exists yet, because the hold doesn't exist yet) --
#: 45 minutes is a bench-time-affordable upper bound, well past the several
#: minutes a first-order thermal lag at this temperature range and duty
#: would need to settle from a few degrees of ambient spread. Treat this as
#: a starting value to be tightened (or loosened) once the first
#: stabilised-protocol captures exist -- see the settling criterion note in
#: PID_EXPANSION_PLAN.md.
DEFAULT_STABILIZATION_DWELL_MIN = 45


def build_stabilization_segment(
    target_c: float = DEFAULT_STABILIZATION_TARGET_C,
    ramp_c_per_hr: float = DEFAULT_STABILIZATION_RAMP_C_PER_HR,
    dwell_min: int = DEFAULT_STABILIZATION_DWELL_MIN,
) -> ProfileSegment:
    if target_c <= 0.0:
        raise ValueError(f"stabilization target_c must be positive, got {target_c!r}")
    if target_c > 62.0:
        # profile_feasibility's coupled hold solve is valid below ~60C and
        # infeasible above ~62C (project_ff_hold_infeasible_above_62c) -- a
        # stabilisation target above that band defeats its own purpose: the
        # scored segments that follow would inherit a coupling-solve state
        # this bench has never validated.
        raise ValueError(
            f"stabilization target_c={target_c!r} exceeds the coupled hold solve's "
            "~60C validity ceiling (infeasible above ~62C) -- pick a lower target"
        )
    if ramp_c_per_hr <= 0.0:
        raise ValueError(f"stabilization ramp_c_per_hr must be positive, got {ramp_c_per_hr!r}")
    if dwell_min <= 0:
        raise ValueError(f"stabilization dwell_min must be positive, got {dwell_min!r}")
    return ProfileSegment(target_c=target_c, ramp_c_per_hr=ramp_c_per_hr, dwell_min=dwell_min)


def prepend_stabilization_hold(
    segments: Sequence[ProfileSegment],
    target_c: float = DEFAULT_STABILIZATION_TARGET_C,
    ramp_c_per_hr: float = DEFAULT_STABILIZATION_RAMP_C_PER_HR,
    dwell_min: int = DEFAULT_STABILIZATION_DWELL_MIN,
) -> list[ProfileSegment]:
    """Returns a NEW segment list: [stabilization hold] + segments, unmodified
    otherwise. Raises ValueError if that would exceed PROFILE_MAX_SEGMENTS
    (the firmware refuses a SAVE beyond that count too, but failing here
    gives a readable error before any HTTP/UART call is attempted) or if any
    original segment's own target_c is BELOW the stabilization target (that
    would mean the "stabilised" start is hotter than where the scored
    ramp is trying to go, which is not a stabilisation hold, it's a
    different profile)."""
    if not segments:
        raise ValueError("segments must be non-empty -- nothing to stabilize ahead of")
    if len(segments) + 1 > PROFILE_MAX_SEGMENTS:
        raise ValueError(
            f"prepending a stabilization segment would produce {len(segments) + 1} segments, "
            f"exceeding PROFILE_MAX_SEGMENTS={PROFILE_MAX_SEGMENTS}"
        )
    stabilize = build_stabilization_segment(target_c=target_c, ramp_c_per_hr=ramp_c_per_hr, dwell_min=dwell_min)
    first = segments[0]
    if first.target_c < stabilize.target_c:
        raise ValueError(
            f"first scored segment's target_c={first.target_c!r} is below the stabilization "
            f"target_c={stabilize.target_c!r} -- this profile's own opening ramp is not above "
            "the stabilization setpoint, so prepending a hold there would change the profile's "
            "own scored shape, not just its starting temperature"
        )
    return [stabilize, *segments]
