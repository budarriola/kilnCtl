#!/usr/bin/env python3
"""Tests for kilnsim.runner.run_scenario's FAULT_SCHEDULE issuance --
specifically the UNTIL_TRIGGER two-frame sequence (PROTOCOL.md sec 5.6:
FAULT_SCHEDULE parks the fault, FAULT_SET_UNTIL_TRIGGER supplies the release
trigger and performs the actual arm). Runs against MockSimLink so it needs
no hardware/virtual device and completes instantly (no real waiting).

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import itertools
import os
import sys
import unittest
from unittest.mock import patch

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim.link import MockSimLink  # noqa: E402
from kilnsim.protocol import CommandGroup, FaultCmd  # noqa: E402
from kilnsim.runner import run_scenario  # noqa: E402
from kilnsim.scenario import load_scenario_text  # noqa: E402


def _run_scenario_fast(link, scenario):
    """run_scenario() waits a real wall-clock budget (duration_s/timescale +
    a fixed 5s overhead, runner.py's own `wall_budget`) polling
    read_events/telemetry before returning -- irrelevant to what these tests
    check (the FAULT_SCHEDULE/FAULT_SET_UNTIL_TRIGGER commands issued, which
    all happen before that wait loop starts) and needlessly slow against
    MockSimLink, which has nothing to usefully wait for. Patches
    ``runner.time.time`` so the very first `wall_budget` check already reads
    as elapsed, without touching MockSimLink or run_scenario itself."""
    fake_time = itertools.chain([0.0], itertools.repeat(1e9))
    with patch("kilnsim.runner.time.time", side_effect=lambda: next(fake_time)):
        return run_scenario(link, scenario, duration_s=0.0)

UNTIL_TRIGGER_YAML = """
name: until_trigger_probe
version: 1
preset: fast_test
timescale: 1
seed: 0
faults:
  - id: noisy
    type: tc_noise
    target: tc:0
    trigger: { at_sim_time: { t: 1 } }
    duration: { until_trigger: { at_sim_time: { t: 5 } } }
    params: [3.0]
expect: []
"""

PERMANENT_YAML = """
name: permanent_probe
version: 1
preset: fast_test
timescale: 1
seed: 0
faults:
  - id: weld
    type: welded_ssr
    target: relay:K1
    trigger: { manual: {} }
    duration: permanent
expect: []
"""


class UntilTriggerTwoFrameSequenceTests(unittest.TestCase):
    def setUp(self):
        self.link = MockSimLink()
        self.link.connect()

    def test_until_trigger_duration_issues_schedule_then_set_until_trigger(self):
        scenario = load_scenario_text(UNTIL_TRIGGER_YAML)
        _run_scenario_fast(self.link, scenario)

        fault_calls = [
            (group, cmd, payload)
            for group, cmd, payload in self.link.sent_commands
            if group is CommandGroup.FAULT
        ]
        self.assertEqual(len(fault_calls), 2, f"expected exactly 2 FAULT commands, got {fault_calls}")

        (g1, c1, p1), (g2, c2, p2) = fault_calls
        self.assertEqual(c1, FaultCmd.SCHEDULE)
        self.assertEqual(p1["fault_slot"], 0)
        self.assertEqual(p1["duration"]["kind"], "until_trigger")

        self.assertEqual(c2, FaultCmd.SET_UNTIL_TRIGGER)
        self.assertEqual(p2["fault_slot"], 0)
        # The release trigger (duration.until, at_sim_time t=5) must be what
        # travels in frame 2, not the ARM trigger (at_sim_time t=1) from
        # frame 1.
        self.assertEqual(p2["trigger"]["kind"], "at_sim_time")
        self.assertEqual(p2["trigger"]["t"], 5.0)
        self.assertNotEqual(p2["trigger"]["t"], p1["trigger"]["t"])

    def test_schedule_sent_before_set_until_trigger_in_order(self):
        scenario = load_scenario_text(UNTIL_TRIGGER_YAML)
        _run_scenario_fast(self.link, scenario)

        fault_calls = [(cmd) for group, cmd, _ in self.link.sent_commands if group is CommandGroup.FAULT]
        schedule_idx = fault_calls.index(FaultCmd.SCHEDULE)
        set_until_idx = fault_calls.index(FaultCmd.SET_UNTIL_TRIGGER)
        self.assertLess(schedule_idx, set_until_idx)

    def test_permanent_duration_does_not_issue_set_until_trigger(self):
        scenario = load_scenario_text(PERMANENT_YAML)
        _run_scenario_fast(self.link, scenario)

        fault_calls = [
            (group, cmd, payload)
            for group, cmd, payload in self.link.sent_commands
            if group is CommandGroup.FAULT
        ]
        self.assertEqual(len(fault_calls), 1)
        self.assertEqual(fault_calls[0][1], FaultCmd.SCHEDULE)


if __name__ == "__main__":
    unittest.main()
