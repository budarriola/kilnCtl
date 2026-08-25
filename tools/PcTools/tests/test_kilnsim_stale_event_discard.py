#!/usr/bin/env python3
"""A scenario must never collect the previous scenario's events.

Found on real hardware 2026-08-24. `kilnsim testmgr` ran the tier-0
scenario `fixture_fault_lifecycle` and it FAILED with
`validity.event_seq_gap = True`, having received event seqs [0, 1, 7] --
while passing standalone with [0, 1]. Both of its expectations passed; the
run failed purely on the gap.

The 7 was real, correctly numbered, and belonged to the scenario that ran
before it. EVT frames are buffered by the RX thread the instant they
arrive, on a connection-lifetime buffer with no notion of a scenario
boundary; a scenario's trailing drain is a zero-timeout sweep, so a frame
still in flight when a run ends lands in the buffer just afterwards and is
collected by the NEXT run's first poll.

The gap is the mild symptom. The serious one is that a wire seq restarts at
0 on every SYS/RESET_SIM, so a straggler is genuinely indistinguishable
from a same-run event after the fact -- had its payload matched one of the
new scenario's `expect:` filters, it would have produced a PASS or FAIL
attributed to a run it was never part of.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import testmgr as tm  # noqa: E402
from kilnsim.link import MockSimLink  # noqa: E402
from kilnsim.protocol import Event, EventType  # noqa: E402


def _evt(seq: int) -> Event:
    return Event(seq=seq, sim_time_us=seq * 1000,
                  event_type=EventType.FAULT_FIRED, payload={}, a=0, b=0, f0=0.0)


class DiscardBufferedEventsTests(unittest.TestCase):
    def test_discard_drops_everything_and_reports_the_count(self):
        link = MockSimLink()
        link.connect()
        link._events.extend([_evt(5), _evt(6), _evt(7)])
        self.assertEqual(link.discard_buffered_events(), 3)
        self.assertEqual(link.read_events(timeout=0.0), [],
                          "discarded events must not come back from a later read")

    def test_discard_on_an_empty_buffer_is_zero_and_does_not_block(self):
        link = MockSimLink()
        link.connect()
        self.assertEqual(link.discard_buffered_events(), 0)

    def test_discard_does_not_return_the_events_to_the_caller(self):
        """The whole point: these must not reach a scenario's collected
        list. A method that returned them would just move the bug."""
        link = MockSimLink()
        link.connect()
        link._events.append(_evt(7))
        result = link.discard_buffered_events()
        self.assertIsInstance(result, int)


class RunOneScenarioDiscardsStragglersTests(unittest.TestCase):
    """The reproduction, in miniature: an event from a previous run is
    already buffered when a scenario starts, and must not appear in that
    scenario's report."""

    _YAML = """
name: straggler_probe
version: 1
exercises: []
seed: 1
timescale: 1
dut:
  profile: none
faults: []
expect:
  - name: never_estops
    forbid: { dut: estop_open, before: { sim_end: true } }
"""

    def _run(self):
        from kilnsim.scenario import load_scenario_text

        scenario = load_scenario_text(self._YAML)
        link = MockSimLink()
        link.connect()
        # A straggler from "the previous scenario", buffered before this run
        # begins -- exactly the hardware situation.
        link._events.append(_evt(7))
        presence = tm.HardwarePresence(
            fixture=tm.PresenceResult(True, "ok"),
            saftyfw=tm.PresenceResult(True, "ok"),
            esp=tm.PresenceResult(True, "ok"),
        )
        return tm.run_one_scenario(link, scenario, presence, mock=True)

    def test_stale_event_never_reaches_the_scenarios_report(self):
        outcome = self._run()
        seqs = [e.seq for e in (outcome.report.events if outcome.report else [])]
        self.assertNotIn(7, seqs,
                          "a previous run's event was collected by this scenario")

    def test_the_discard_is_reported_not_silent(self):
        outcome = self._run()
        self.assertIn("stale event", outcome.detail or "",
                      "the discard must be visible in the report, not swallowed")


if __name__ == "__main__":
    unittest.main()
