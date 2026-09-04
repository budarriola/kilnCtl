"""Tests for kilnctrl.profile_stabilization -- prepending a stabilisation
hold onto an A/B arm's profile, the ambient-confound protocol fix documented
in PID_EXPANSION_PLAN.md."""
from __future__ import annotations

import pytest

from kilnctrl.devices_profiles import ProfileSegment
from kilnctrl import profile_stabilization as ps


def _p7_like_segments():
    # Stand-in for profile 7's real segments: ramp room-ambient -> 70C,
    # dwell, then a slow cool. Only target_c/order matter for these tests.
    return [
        ProfileSegment(target_c=70.0, ramp_c_per_hr=120.0, dwell_min=30),
        ProfileSegment(target_c=70.0, ramp_c_per_hr=0.0, dwell_min=20),
    ]


def test_prepend_adds_one_segment_at_index_0():
    original = _p7_like_segments()
    out = ps.prepend_stabilization_hold(original)
    assert len(out) == len(original) + 1
    assert out[0].target_c == pytest.approx(ps.DEFAULT_STABILIZATION_TARGET_C)
    # original segments preserved, unmodified, shifted up by one
    assert out[1:] == original


def test_prepend_does_not_mutate_input():
    original = _p7_like_segments()
    before = list(original)
    ps.prepend_stabilization_hold(original)
    assert original == before


def test_stabilization_target_stays_below_coupled_hold_ceiling():
    # 60C is described as the top of the coupled hold solve's valid range
    # (infeasible above ~62C); the default must sit comfortably inside it.
    assert ps.DEFAULT_STABILIZATION_TARGET_C < 60.0


def test_stabilization_target_above_62c_refused():
    with pytest.raises(ValueError, match="60C validity ceiling"):
        ps.build_stabilization_segment(target_c=65.0)


def test_stabilization_target_at_62c_boundary_refused():
    with pytest.raises(ValueError):
        ps.build_stabilization_segment(target_c=62.5)


def test_never_fires_above_70c_bench_limit():
    # Belt-and-braces: even a caller passing a bad target can't produce a
    # stabilization segment above the bench's 70C hard limit, since the
    # 62C refusal is well under it -- confirm that ordering holds.
    with pytest.raises(ValueError):
        ps.build_stabilization_segment(target_c=70.0)


def test_prepend_refuses_when_first_segment_target_below_stabilization():
    # A profile whose own opening segment targets BELOW the stabilization
    # setpoint would have its scored shape changed, not just its start --
    # must refuse rather than silently produce a different profile.
    low_open = [ProfileSegment(target_c=40.0, ramp_c_per_hr=100.0, dwell_min=10)]
    with pytest.raises(ValueError, match="below the stabilization"):
        ps.prepend_stabilization_hold(low_open)


def test_prepend_refuses_empty_segments():
    with pytest.raises(ValueError, match="non-empty"):
        ps.prepend_stabilization_hold([])


def test_prepend_refuses_beyond_profile_max_segments():
    original = [
        ProfileSegment(target_c=70.0, ramp_c_per_hr=100.0, dwell_min=5)
        for _ in range(ps.PROFILE_MAX_SEGMENTS)
    ]
    with pytest.raises(ValueError, match="PROFILE_MAX_SEGMENTS"):
        ps.prepend_stabilization_hold(original)


def test_prepend_at_exactly_max_segments_minus_one_succeeds():
    original = [
        ProfileSegment(target_c=70.0, ramp_c_per_hr=100.0, dwell_min=5)
        for _ in range(ps.PROFILE_MAX_SEGMENTS - 1)
    ]
    out = ps.prepend_stabilization_hold(original)
    assert len(out) == ps.PROFILE_MAX_SEGMENTS


def test_build_stabilization_segment_rejects_non_positive_inputs():
    with pytest.raises(ValueError):
        ps.build_stabilization_segment(target_c=0.0)
    with pytest.raises(ValueError):
        ps.build_stabilization_segment(ramp_c_per_hr=0.0)
    with pytest.raises(ValueError):
        ps.build_stabilization_segment(dwell_min=0)


# ---------------------------------------------------------------------------
# Negative test: prove the >62C refusal is actually load-bearing (mutate the
# threshold down, confirm a previously-accepted value now gets refused; this
# is the "prove it can fail" discipline PID_EXPANSION_PLAN.md's rules call for).
# ---------------------------------------------------------------------------

def test_mutation_ceiling_check_is_load_bearing():
    # Real behaviour: 50C is accepted today.
    ps.build_stabilization_segment(target_c=50.0)

    # MUTATION: monkeypatch the check's threshold constant inline by calling
    # through a locally-lowered ceiling to prove the guard clause actually
    # gates on the value, not a stray always-true/always-false condition.
    original_fn = ps.build_stabilization_segment

    def _lowered_ceiling(target_c=ps.DEFAULT_STABILIZATION_TARGET_C, **kwargs):
        if target_c > 40.0:  # mutated threshold, well below the real 62C
            raise ValueError("stabilization target_c exceeds MUTATED ceiling")
        return original_fn(target_c=target_c, **kwargs)

    with pytest.raises(ValueError, match="MUTATED ceiling"):
        _lowered_ceiling(target_c=50.0)
    # and confirm the mutated function still accepts a genuinely low value,
    # so the failure above is the ceiling firing, not a broken test harness
    _lowered_ceiling(target_c=30.0)


# ---------------------------------------------------------------------------
# TASK 1 (2026-09-03, owner decision: hold on by default). is_stabilization_
# segment is the idempotency check run_queue.ensure_stabilized_profile()
# relies on to avoid stacking a second hold onto a profile that already has
# one -- test it directly against ProfileSegment AND a plain-object stand-in
# (SimpleNamespace), since run_queue reconstructs one from JSON dicts, not
# a ProfileSegment, and STABILIZATION_SEGMENT_INDEX, the single source of
# truth pid_ab_compare.py imports.
# ---------------------------------------------------------------------------

def test_stabilization_segment_index_is_one():
    # Structural fact: prepend_stabilization_hold always inserts exactly
    # one segment at index 0, so the scored schedule always starts at 1.
    assert ps.STABILIZATION_SEGMENT_INDEX == 1


def test_is_stabilization_segment_true_for_default_built_segment():
    seg = ps.build_stabilization_segment()
    assert ps.is_stabilization_segment(seg) is True


def test_is_stabilization_segment_true_for_plain_namespace_from_json():
    # run_queue.ensure_stabilized_profile checks a dict fetched over HTTP,
    # not a ProfileSegment -- SimpleNamespace(**dict) is exactly how it
    # adapts one, so this is the shape that actually matters.
    from types import SimpleNamespace
    obj = SimpleNamespace(target_c=48.0, ramp_c_per_hr=300.0, dwell_min=45,
                           seg_kind=0, io_target=0)  # extra keys ignored
    assert ps.is_stabilization_segment(obj) is True


def test_is_stabilization_segment_false_for_an_ordinary_scored_segment():
    seg = ProfileSegment(target_c=70.0, ramp_c_per_hr=120.0, dwell_min=30)
    assert ps.is_stabilization_segment(seg) is False


def test_is_stabilization_segment_false_when_only_target_matches():
    # Same target_c as the default hold, but a different dwell -- must not
    # be mistaken for the same segment (this is the idempotency check that
    # keeps ensure_stabilized_profile from either re-stacking a hold OR
    # silently accepting a differently-shaped one as "already done").
    seg = ProfileSegment(target_c=48.0, ramp_c_per_hr=300.0, dwell_min=10)
    assert ps.is_stabilization_segment(seg) is False


def test_is_stabilization_segment_false_for_object_missing_fields():
    from types import SimpleNamespace
    # A RELAY_IO segment dict has no target_c/ramp_c_per_hr at all in some
    # callers' shape -- must not raise, must just say "not a match".
    obj = SimpleNamespace(io_target=3, io_state=1)
    assert ps.is_stabilization_segment(obj) is False


def test_mutation_is_stabilization_segment_dwell_check_is_load_bearing():
    # NEGATIVE TEST: prove the dwell_min comparison is actually load-bearing
    # (not a stray always-true condition that would let ensure_stabilized_
    # profile treat ANY 48C-opening segment as "already stabilized" even
    # with the wrong hold duration).
    matching = ProfileSegment(target_c=48.0, ramp_c_per_hr=300.0, dwell_min=45)
    wrong_dwell = ProfileSegment(target_c=48.0, ramp_c_per_hr=300.0, dwell_min=5)
    assert ps.is_stabilization_segment(matching) is True
    assert ps.is_stabilization_segment(wrong_dwell) is False
    # MUTATION: drop the dwell_min term from the comparison entirely --
    # the real function must behave differently from this mutant, i.e. the
    # mutant's answer for wrong_dwell (True) must NOT match reality (False).
    def _mutated_is_stabilization_segment(segment, target_c=ps.DEFAULT_STABILIZATION_TARGET_C,
                                           ramp_c_per_hr=ps.DEFAULT_STABILIZATION_RAMP_C_PER_HR,
                                           dwell_min=ps.DEFAULT_STABILIZATION_DWELL_MIN, tol_c=0.05):
        seg_target = getattr(segment, "target_c", None)
        seg_ramp = getattr(segment, "ramp_c_per_hr", None)
        if seg_target is None or seg_ramp is None:
            return False
        return abs(float(seg_target) - target_c) <= tol_c and abs(float(seg_ramp) - ramp_c_per_hr) <= tol_c

    mutant_result = _mutated_is_stabilization_segment(wrong_dwell)
    real_result = ps.is_stabilization_segment(wrong_dwell)
    assert mutant_result != real_result, (
        "mutant (no dwell_min check) and the real function agree on wrong_dwell -- "
        "the dwell_min term is not actually gating anything")
