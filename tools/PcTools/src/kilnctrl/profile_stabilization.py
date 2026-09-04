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

#: Ambient on this bench has been measured 26-31C (observed range as of the
#: 2026-08-31 A/B campaign) and one firing raises the working ambient
#: ~1.4-1.5C further -- so a worst-case room/enclosure start is comfortably
#: under 33C. 48 C ORIGINALLY looked like a safe choice on that margin alone
#: (well below the coupled hold solve's ~60C validity ceiling, far below
#: profile 7's ~70C top) -- but it collides with this fixture's own
#: lowest-temperature scored profile: profile 7's first segment targets 45C,
#: BELOW the 48C hold, so prepend_stabilization_hold() correctly refuses it
#: (a stabilisation hold hotter than the profile's own opening ramp would
#: reshape the profile, not just its starting temperature -- see
#: prepend_stabilization_hold()'s docstring). That made the bench's main
#: multi-zone profile entirely unusable with the hold, forcing a fallback to
#: profile 4 ('cpl_z0', single-zone) that cannot support the "same metric,
#: same direction, on at least 3 zones" decision rule at all.
#:
#: 40C clears both constraints with real margin:
#:   * above ambient: 40 - 31 (widest observed ambient) = 9C margin, or
#:     40 - 32.5 (ambient + a firing's own ~1.5C rise) = 7.5C margin --
#:     nearly double the ~5C worst-case ambient SPREAD this rig has shown
#:     (pid_ab_compare.py's 2026-09-02f note), which is the number that
#:     actually matters here (see the residual-confound arithmetic below).
#:   * below every scored profile's opening segment: profile 7 opens at 45C
#:     (5C of headroom), profile 4/cpl_z0 opens at 55C (15C of headroom) --
#:     profile 7, the multi-zone profile the 3-zone decision rule needs, is
#:     now usable WITH the hold.
#:
#: Residual-confound check (the 0.5C actionability bar from
#: PID_EXPANSION_PLAN.md section 8): the hold's job is to convert each arm's
#: scored start temperature from "whatever ambient happened to be" to
#: "whatever the PID settled to at the end of the dwell" -- a quantity
#: dominated by steady-state tracking precision, not by which fixed setpoint
#: was chosen. Moving the target from 48C to 40C does not change that
#: argument's shape, only its ambient margin (computed above, and still
#: comfortably positive). Taking the same deliberately pessimistic residual
#: spread used for 48C (up to 0.5C, i.e. assuming the hold does nothing to
#: tighten things beyond ordinary dwell tracking) against the fitted
#: sensitivity's own top end (0.133C of iae_normalized_whole_c per 1C of
#: start delta):
#:     0.133 C/C * 0.5 C residual spread = 0.0665 C predicted confound
#: -- still over 7x under the 0.5C bar, unchanged by the target move.
#:
#: Still well inside the coupled hold solve's ~60C validity ceiling
#: (infeasible above ~62C -- see project_ff_hold_infeasible_above_62c) and
#: far below profile 7's own ~70C top.
DEFAULT_STABILIZATION_TARGET_C = 40.0

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


#: THE SINGLE SOURCE OF TRUTH for "which segment index does the scored
#: schedule start at, once a stabilisation hold has been prepended". Defined
#: HERE, not in pid_ab_compare.py, because it is a structural fact of
#: :func:`prepend_stabilization_hold` itself (it always inserts exactly ONE
#: segment at index 0) -- pid_ab_compare.py imports this rather than
#: carrying its own copy of the number, so the runner (which prepends the
#: hold) and the analysis (which has to know where it ends) cannot drift
#: apart the way this repo's producer/consumer pairs have before (see
#: project_split_module_missing_name_class / project_consumer_without_
#: producer_class in the coordinator's memory). If this module ever grows a
#: mode that prepends more than one segment, this constant is the one place
#: that has to change, and every caller downstream follows automatically.
STABILIZATION_SEGMENT_INDEX = 1


def is_stabilization_segment(
    segment: object,
    target_c: float = DEFAULT_STABILIZATION_TARGET_C,
    ramp_c_per_hr: float = DEFAULT_STABILIZATION_RAMP_C_PER_HR,
    dwell_min: int = DEFAULT_STABILIZATION_DWELL_MIN,
    tol_c: float = 0.05,
) -> bool:
    """True if ``segment`` (a :class:`ProfileSegment`, or any object with
    ``target_c``/``ramp_c_per_hr``/``dwell_min`` attributes -- a plain dict
    accessed via ``SimpleNamespace(**d)`` works too) already matches the
    stabilisation segment :func:`build_stabilization_segment` would produce
    for these parameters.

    Used for IDEMPOTENCY: ``run_queue.py`` prepends a hold onto a board
    profile IN PLACE (see ``ensure_stabilized_profile``), and the same
    profile id is typically reused run after run within a campaign, and
    across campaigns. Without this check, prepending unconditionally would
    grow the profile by one segment every single run, and the second run
    would silently stack a SECOND stabilisation hold onto the first (wrong
    schedule, and eventually a real ``PROFILE_MAX_SEGMENTS`` refusal on an
    otherwise-legitimate campaign)."""
    seg_target = getattr(segment, "target_c", None)
    seg_ramp = getattr(segment, "ramp_c_per_hr", None)
    seg_dwell = getattr(segment, "dwell_min", None)
    if seg_target is None or seg_ramp is None or seg_dwell is None:
        return False
    return (
        abs(float(seg_target) - target_c) <= tol_c
        and abs(float(seg_ramp) - ramp_c_per_hr) <= tol_c
        and int(seg_dwell) == int(dwell_min)
    )


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
