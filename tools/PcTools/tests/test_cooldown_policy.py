#!/usr/bin/env python3
"""Host tests for the cooldown gate's DECISION LOGIC.

No board, no HTTP, no sleeping. ``bench_fixture_session.wait_for_cooldown()``
is deliberately split so that everything worth arguing about -- what
temperature counts as "cool", what happens when the reading is missing, what
happens when a caller asks for something unreachable -- lives in two pure
functions this file can exercise exhaustively, leaving only the polling loop
itself to the live bench.

The point of the split is that the interesting failure of a cooldown gate is
not "it waited too long"; it is "it opened when it should not have". That is
a policy question, and a policy question can be tested on a laptop.
"""
from __future__ import annotations

import os
import sys

import pytest

sys.path.insert(0, os.path.dirname(__file__))

from bench_fixture_session import (  # noqa: E402
    COOLDOWN_DEFAULT_TOLERANCE_C,
    BenchSessionError,
    cooldown_reached,
    cooldown_target_c,
)


# -- cooldown_target_c ----------------------------------------------------

def test_target_is_ambient_plus_tolerance_not_a_constant():
    """The whole reason this function exists: the gate has to move with the
    room. At the ~34 C cold junction this bench actually reports, a 25 C
    hardcoded target would never be reached at all."""
    assert cooldown_target_c(34.0, None, 3.0) == pytest.approx(37.0)
    # A colder room gives a colder gate, from the same code.
    assert cooldown_target_c(20.0, None, 3.0) == pytest.approx(23.0)


def test_default_tolerance_is_applied_as_documented():
    assert cooldown_target_c(30.0, None, COOLDOWN_DEFAULT_TOLERANCE_C) == pytest.approx(
        30.0 + COOLDOWN_DEFAULT_TOLERANCE_C)


def test_explicit_target_wins_and_is_not_second_guessed():
    """Including an unreachable one. Substituting a reachable number for the
    requested one would be invisible in the result -- the caller would
    believe they had gated on 20 C."""
    assert cooldown_target_c(34.0, 20.0, 3.0) == pytest.approx(20.0)
    assert cooldown_target_c(34.0, 50.0, 3.0) == pytest.approx(50.0)
    # ...and a zero target is a target, not a falsy "unset".
    assert cooldown_target_c(34.0, 0.0, 3.0) == pytest.approx(0.0)


def test_no_ambient_reading_refuses_rather_than_defaulting():
    """NEGATIVE TEST. A board reporting no usable cold junction has told us
    nothing about the room; inventing 25 C there is exactly the bug this
    helper replaces."""
    with pytest.raises(BenchSessionError) as exc:
        cooldown_target_c(float("nan"), None, 3.0)
    assert "cold-junction" in str(exc.value)
    with pytest.raises(BenchSessionError):
        cooldown_target_c(float("inf"), None, 3.0)


def test_no_ambient_reading_is_still_fine_with_an_explicit_target():
    """The refusal above is about DERIVING a target, not about waiting."""
    assert cooldown_target_c(float("nan"), 40.0, 3.0) == pytest.approx(40.0)


def test_negative_tolerance_is_refused():
    """NEGATIVE TEST. A negative tolerance would demand the bench get colder
    than the room, which no wait can deliver -- fail at the call, not 45
    minutes later at the timeout."""
    with pytest.raises(BenchSessionError):
        cooldown_target_c(34.0, None, -1.0)


# -- cooldown_reached -----------------------------------------------------

def test_reached_is_at_or_below_the_target():
    assert cooldown_reached(37.0, 37.0) is True
    assert cooldown_reached(36.9, 37.0) is True
    assert cooldown_reached(37.1, 37.0) is False


def test_nan_is_not_cool():
    """NEGATIVE TEST, and the one that matters most here. A dropped-out
    thermocouple reads NaN; every comparison against NaN is False, so a
    naive ``hottest <= target`` written the other way round would open the
    gate the instant a sensor failed -- handing the next test a hot bench
    AND a dead sensor."""
    assert cooldown_reached(float("nan"), 37.0) is False


def test_a_hot_bench_never_passes_however_high_the_ambient():
    """NEGATIVE TEST for the composition of the two: even a 40 C room does
    not make a 70 C zone cool."""
    target = cooldown_target_c(40.0, None, 3.0)
    assert cooldown_reached(70.0, target) is False
