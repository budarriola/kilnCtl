#!/usr/bin/env python3
"""Tests for the two real gaps closed in kilnsim.runner.run_scenario:

1. spi_underrun was hardcoded False regardless of what the link actually
   reported -- any scenario asserting "no SPI underruns" passed vacuously.
   run_scenario() now polls the TELEMETRY frame's own spi_underrun_count
   (protocol.py's TelemetryFrame.spi_underrun_count, kilnsim.payloads.
   decode_telemetry_frame()) for real, taking a baseline before any fault is
   armed and comparing against the last reading at the end -- and reports
   ValidityFlags.spi_underrun_signal_available=False (not a silent False)
   when the link cannot serve that signal at all (MockSimLink).
2. GUARD_TRIP/GUARD_WARN/LINK_UP/TRIP_INEFFECTIVE_LATCHED had no producer
   anywhere -- kilnsim.guard_observer.SafetyGuardObserver polls SaftyFW
   guard status through a kilnctrl SafetyClient (duck-typed here, no real
   kilnctrl import needed) and edge-detects it into those events;
   run_scenario() plumbs an optional `guard_observer=` through, and marks
   guard-typed `expect` clauses BLOCKED (not silently FAIL/PASS) when no
   observer is attached.

Runs against MockSimLink and small hand-rolled SimLink/GuardObserver test
doubles -- no hardware, no virtual_simfw process needed.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import itertools
import os
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim.guard_observer import (  # noqa: E402
    GUARD_OBSERVER_SEQ_BASE,
    SafetyGuardObserver,
)
from kilnsim.link import MockSimLink, SimLink  # noqa: E402
from kilnsim.protocol import CommandGroup, Event, EventType  # noqa: E402
from kilnsim.report import BLOCKED, PASS  # noqa: E402
from kilnsim.runner import run_scenario  # noqa: E402
from kilnsim.scenario import load_scenario_text  # noqa: E402


def _run_scenario_fast(link, scenario, **kwargs):
    """See test_kilnsim_runner.py's identical helper for the full rationale:
    patches runner.time.time so run_scenario()'s wait loop body never
    actually iterates (the loop condition is already false the first time
    it is checked), which is irrelevant to what these tests check and
    needlessly slow against a fake in-memory link."""
    fake_time = itertools.chain([0.0], itertools.repeat(1e9))
    with patch("kilnsim.runner.time.time", side_effect=lambda: next(fake_time)):
        return run_scenario(link, scenario, duration_s=0.0, **kwargs)


# ---------------------------------------------------------------------------
# Item 1: spi_underrun
# ---------------------------------------------------------------------------
class _FakeTelemetryLink(SimLink):
    """Minimal real SimLink double that DOES serve TELEMETRY (unlike
    MockSimLink) so run_scenario()'s spi_underrun baseline/final-read logic
    can be exercised without a real _FramedSimLink transport. `spi_underrun_
    counts` is popped front-to-back on each get_last_telemetry() call;
    once exhausted, the last value keeps being returned (steady state),
    matching a real link continuing to report its last-known count."""

    def __init__(self, spi_underrun_counts: list) -> None:
        self._connected = False
        self._counts = list(spi_underrun_counts)
        self._current = None

    @property
    def is_connected(self) -> bool:
        return self._connected

    def connect(self, port=None) -> str:
        self._connected = True
        return "FAKE"

    def disconnect(self) -> None:
        self._connected = False

    def send_command(self, group, cmd, payload=None, timeout=None) -> dict:
        return {}

    def read_events(self, timeout: float = 0.0) -> list:
        return []

    def get_last_telemetry(self):
        if self._counts:
            self._current = self._counts.pop(0)
        if self._current is None:
            return None
        return {
            "spi_underrun_count": self._current,
            "sim_time_us": 0,
            "zones": [],
            "fault_line_asserted": False,
            "estop_open": False,
        }


EMPTY_YAML = """
name: spi_underrun_probe
version: 1
timescale: 1
seed: 0
expect: []
"""


class SpiUnderrunSignalTests(unittest.TestCase):
    def test_underrun_count_increase_reports_true_and_available(self):
        # Baseline read (before any fault armed) returns 5; the one trailing
        # read after the wait loop returns 9 -- a real increase.
        link = _FakeTelemetryLink([5, 9])
        link.connect()
        scenario = load_scenario_text(EMPTY_YAML)
        report = _run_scenario_fast(link, scenario)
        self.assertTrue(report.validity.spi_underrun_signal_available)
        self.assertTrue(report.validity.spi_underrun)
        self.assertFalse(report.validity.valid)  # spi_underrun=True invalidates the run

    def test_underrun_count_unchanged_reports_false_and_available(self):
        link = _FakeTelemetryLink([5, 5])
        link.connect()
        scenario = load_scenario_text(EMPTY_YAML)
        report = _run_scenario_fast(link, scenario)
        self.assertTrue(report.validity.spi_underrun_signal_available)
        self.assertFalse(report.validity.spi_underrun)
        self.assertTrue(report.validity.valid)

    def test_mock_link_reports_signal_unavailable_not_silent_false(self):
        # This is the exact bug being fixed: MockSimLink has no
        # get_last_telemetry() at all (no real TELEMETRY signal possible),
        # so run_scenario() must say so explicitly rather than reporting a
        # bare spi_underrun=False that looks identical to "checked, clean."
        link = MockSimLink()
        link.connect()
        scenario = load_scenario_text(EMPTY_YAML)
        report = _run_scenario_fast(link, scenario)
        self.assertFalse(report.validity.spi_underrun_signal_available)
        self.assertFalse(report.validity.spi_underrun)


# ---------------------------------------------------------------------------
# Item 2: guard observer plumbing through run_scenario()
# ---------------------------------------------------------------------------
GUARD_EXPECT_YAML = """
name: guard_probe
version: 1
timescale: 1
seed: 0
expect:
  - name: trip_then_open
    event: { type: guard_trip }
    then: { dut: K4_open, within_s: 5 }
  - name: never_warns
    forbid:
      event: { type: guard_warn }
      before: { sim_time_s: 100 }
  - name: unrelated_forbid
    forbid:
      dut: K4_open
      before: { sim_time_s: 100 }
"""

GUARD_ATEND_YAML = """
name: guard_atend_probe
version: 1
timescale: 1
seed: 0
expect:
  - name: guard_tripped_by_end
    at_end: { event: { type: guard_trip } }
"""


class _FakeGuardObserver:
    """Returns `events` on its first observe() call, then nothing --
    matches how run_scenario() actually calls it in these tests: the
    wait-loop body never iterates (see _run_scenario_fast's own doc
    comment), so the only observe() call that happens is the single
    trailing poll after the loop."""

    def __init__(self, events: list) -> None:
        self._events = list(events)
        self.call_count = 0

    def observe(self) -> list:
        self.call_count += 1
        out, self._events = self._events, []
        return out


class GuardObserverMissingBlocksExpectationsTests(unittest.TestCase):
    def setUp(self):
        self.link = MockSimLink()
        self.link.connect()

    def test_guard_typed_clauses_blocked_when_no_observer_attached(self):
        scenario = load_scenario_text(GUARD_EXPECT_YAML)
        report = _run_scenario_fast(self.link, scenario)  # guard_observer=None (default)

        by_name = {r.name: r for r in report.expectations}
        self.assertEqual(by_name["trip_then_open"].verdict, BLOCKED)
        self.assertEqual(by_name["trip_then_open"].blocked_on["phase"], "runner")
        self.assertIn("guard_observer", by_name["trip_then_open"].blocked_on["reason"])

        # The forbid-on-an-impossible-event clause must ALSO be blocked, not
        # left to trivially PASS -- the exact "must not silently pass" case
        # the task brief calls out (a forbid on an event that structurally
        # can never occur without an observer would otherwise pass with zero
        # real signal behind it).
        self.assertEqual(by_name["never_warns"].verdict, BLOCKED)

        # A clause with NO guard-event reference at all must be evaluated
        # normally (untouched by the missing-observer block).
        self.assertNotEqual(by_name["unrelated_forbid"].verdict, BLOCKED)

        self.assertEqual(report.verdict, BLOCKED)

    def test_atend_guard_clause_blocked_when_no_observer(self):
        scenario = load_scenario_text(GUARD_ATEND_YAML)
        report = _run_scenario_fast(self.link, scenario)
        self.assertEqual(report.expectations[0].verdict, BLOCKED)

    def test_atend_guard_clause_passes_for_real_when_observer_attached(self):
        guard_event = Event(
            seq=GUARD_OBSERVER_SEQ_BASE, sim_time_us=0,
            event_type=EventType.GUARD_TRIP, payload={"guard": "S1", "trip_reason": 1},
        )
        observer = _FakeGuardObserver([guard_event])
        scenario = load_scenario_text(GUARD_ATEND_YAML)
        report = _run_scenario_fast(self.link, scenario, guard_observer=observer)

        self.assertEqual(report.expectations[0].verdict, PASS)
        self.assertGreaterEqual(observer.call_count, 1)
        # The event actually made it into the report's event list (through
        # the seq-rescaling merge), not just influenced the verdict.
        guard_trip_events = [e for e in report.events if e.event_type == EventType.GUARD_TRIP]
        self.assertEqual(len(guard_trip_events), 1)
        self.assertGreaterEqual(guard_trip_events[0].seq, GUARD_OBSERVER_SEQ_BASE)


# ---------------------------------------------------------------------------
# SafetyGuardObserver itself: edge detection against a duck-typed fake
# kilnctrl SafetyClient (no real kilnctrl import -- see guard_observer.py's
# own doc comment on why this class never imports kilnctrl at module level).
# ---------------------------------------------------------------------------
class _FakeDiag:
    def __init__(self, ever_received: bool, trip_reason: int, warn_mask: int) -> None:
        self.ever_received = ever_received
        self.trip_reason = trip_reason
        self.warn_mask = warn_mask


class _FakeStatus:
    def __init__(self, link_up: bool) -> None:
        self.link_up = link_up


class _FakeSafetyClient:
    def __init__(self, diags: list, statuses: list) -> None:
        self._diags = list(diags)
        self._statuses = list(statuses)

    def get_diag(self):
        return self._diags.pop(0)

    def get_status(self):
        return self._statuses.pop(0)


class SafetyGuardObserverEdgeDetectionTests(unittest.TestCase):
    def test_no_events_before_any_baseline_established(self):
        client = _FakeSafetyClient(
            diags=[_FakeDiag(ever_received=False, trip_reason=0, warn_mask=0)],
            statuses=[_FakeStatus(link_up=False)],
        )
        observer = SafetyGuardObserver(client)
        self.assertEqual(observer.observe(), [])

    def test_trip_edge_reports_correct_guard_label_s6a(self):
        # S6a (SAFETY_TRIP_MAIN_FAULT, reason 6) -- NOT "S6", the trap this
        # mapping table exists to avoid (safety_guards.h's own numbering
        # skips straight from S5=5 to S6a=6/S6b=7).
        client = _FakeSafetyClient(
            diags=[
                _FakeDiag(ever_received=True, trip_reason=0, warn_mask=0),
                _FakeDiag(ever_received=True, trip_reason=6, warn_mask=0),
            ],
            statuses=[_FakeStatus(link_up=True), _FakeStatus(link_up=True)],
        )
        observer = SafetyGuardObserver(client)
        observer.observe()  # establish baseline (trip_reason=0 -> no event)
        events = observer.observe()
        trip_events = [e for e in events if e.event_type == EventType.GUARD_TRIP]
        self.assertEqual(len(trip_events), 1)
        self.assertEqual(trip_events[0].payload["guard"], "S6a")
        self.assertEqual(trip_events[0].payload["trip_reason"], 6)

    def test_trip_ineffective_reason_also_emits_latched_event(self):
        client = _FakeSafetyClient(
            diags=[
                _FakeDiag(ever_received=True, trip_reason=0, warn_mask=0),
                _FakeDiag(ever_received=True, trip_reason=10, warn_mask=0),  # S9
            ],
            statuses=[_FakeStatus(link_up=True), _FakeStatus(link_up=True)],
        )
        observer = SafetyGuardObserver(client)
        observer.observe()
        events = observer.observe()
        types = sorted(e.event_type for e in events)
        self.assertEqual(types, sorted([EventType.GUARD_TRIP, EventType.TRIP_INEFFECTIVE_LATCHED]))
        for e in events:
            self.assertEqual(e.payload["guard"], "S9")

    def test_warn_edge_reports_guard_none_not_a_guess(self):
        client = _FakeSafetyClient(
            diags=[
                _FakeDiag(ever_received=True, trip_reason=0, warn_mask=0),
                _FakeDiag(ever_received=True, trip_reason=0, warn_mask=1),
            ],
            statuses=[_FakeStatus(link_up=True), _FakeStatus(link_up=True)],
        )
        observer = SafetyGuardObserver(client)
        observer.observe()
        events = observer.observe()
        warn_events = [e for e in events if e.event_type == EventType.GUARD_WARN]
        self.assertEqual(len(warn_events), 1)
        self.assertIsNone(warn_events[0].payload["guard"])

    def test_link_recovery_edge_emits_link_up(self):
        client = _FakeSafetyClient(
            diags=[_FakeDiag(ever_received=False, trip_reason=0, warn_mask=0)] * 2,
            statuses=[_FakeStatus(link_up=False), _FakeStatus(link_up=True)],
        )
        observer = SafetyGuardObserver(client)
        observer.observe()  # baseline: link_up=False, no event yet
        events = observer.observe()
        link_events = [e for e in events if e.event_type == EventType.LINK_UP]
        self.assertEqual(len(link_events), 1)

    def test_seqs_come_from_the_disjoint_guard_observer_namespace(self):
        client = _FakeSafetyClient(
            diags=[
                _FakeDiag(ever_received=True, trip_reason=0, warn_mask=0),
                _FakeDiag(ever_received=True, trip_reason=1, warn_mask=0),
            ],
            statuses=[_FakeStatus(link_up=True), _FakeStatus(link_up=True)],
        )
        observer = SafetyGuardObserver(client)
        observer.observe()
        events = observer.observe()
        self.assertTrue(events)
        for e in events:
            self.assertGreaterEqual(e.seq, GUARD_OBSERVER_SEQ_BASE)


if __name__ == "__main__":
    unittest.main()
