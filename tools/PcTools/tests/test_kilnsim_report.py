#!/usr/bin/env python3
"""Tests for kilnsim.report.evaluate_expectations -- the run-report
expectation evaluator, per firmware/SimFW/docs/PLAN.md section 8.2. This is
the most valuable pure-logic piece in kilnsim (task brief): exercises
event/then-within-deadline pass and fail, forbid-before violation, and
at_end, all against synthetic event lists -- no hardware, no link.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim.protocol import Event, EventType  # noqa: E402
from kilnsim.report import FAIL, PASS, SKIPPED, evaluate_expectations, report_from_dict  # noqa: E402
from kilnsim.scenario import load_scenario_text  # noqa: E402

WELDED_SSR_YAML = """
name: welded_ssr_midfire
version: 1
seed: 42
timescale: 10
faults:
  - id: weld
    type: welded_ssr
    target: relay:K1
    trigger: { at_zone_temp: { zone: 0, temp_c: 400, edge: rising } }
    duration: permanent
expect:
  - name: safety_trips
    event: { type: fault_fired, slot: weld }
    then: { dut: K4_open, within_s: 5 }
  - name: no_early_trip
    forbid: { dut: K4_open, before: { fault: weld } }
  - name: trip_latched
    at_end: { dut: K4_open }
"""


def _fault_fired(seq, sim_time_us, fault_id="weld"):
    return Event(seq=seq, sim_time_us=sim_time_us, event_type=EventType.FAULT_FIRED,
                 payload={"fault_id": fault_id})


def _k4(seq, sim_time_us, open_: bool):
    return Event(seq=seq, sim_time_us=sim_time_us, event_type=EventType.RELAY_EDGE,
                 payload={"entity": "K4", "state": open_})


class EventThenWithinDeadlineTests(unittest.TestCase):
    def setUp(self):
        self.scenario = load_scenario_text(WELDED_SSR_YAML)

    def test_passes_when_dut_flag_observed_within_deadline(self):
        events = [
            _fault_fired(1, 300_000_000),          # t=300s
            _k4(2, 302_000_000, open_=True),        # t=302s, 2s later
        ]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["safety_trips"]
        self.assertEqual(result.verdict, PASS)
        self.assertEqual(result.evidence, [1, 2])

    def test_fails_when_dut_flag_observed_too_late(self):
        events = [
            _fault_fired(1, 300_000_000),
            _k4(2, 310_000_000, open_=True),        # 10s later, deadline is 5s
        ]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["safety_trips"]
        self.assertEqual(result.verdict, FAIL)

    def test_fails_when_dut_flag_never_observed(self):
        events = [_fault_fired(1, 300_000_000)]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["safety_trips"]
        self.assertEqual(result.verdict, FAIL)

    def test_skipped_when_triggering_event_never_occurs(self):
        events = [_k4(1, 100_000_000, open_=True)]  # no fault_fired at all
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["safety_trips"]
        self.assertEqual(result.verdict, SKIPPED)

    def test_exact_deadline_boundary_passes(self):
        # within_s: 5 -> exactly 5.0s later must still count.
        events = [
            _fault_fired(1, 300_000_000),
            _k4(2, 305_000_000, open_=True),
        ]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["safety_trips"]
        self.assertEqual(result.verdict, PASS)

    def test_dut_flag_before_the_cause_event_does_not_count(self):
        # K4 happened to be "open" earlier for an unrelated reason, before
        # the fault fired -- must not satisfy the *consequence*.
        events = [
            _k4(1, 100_000_000, open_=True),
            _fault_fired(2, 300_000_000),
        ]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["safety_trips"]
        self.assertEqual(result.verdict, FAIL)


class ForbidBeforeTests(unittest.TestCase):
    def setUp(self):
        self.scenario = load_scenario_text(WELDED_SSR_YAML)

    def test_passes_when_flag_never_observed_before_the_fault(self):
        events = [
            _fault_fired(1, 300_000_000),
            _k4(2, 302_000_000, open_=True),  # after the boundary -- fine
        ]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["no_early_trip"]
        self.assertEqual(result.verdict, PASS)

    def test_fails_when_flag_observed_before_the_fault(self):
        events = [
            _k4(1, 50_000_000, open_=True),   # too early: K4 open before weld fires
            _fault_fired(2, 300_000_000),
        ]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["no_early_trip"]
        self.assertEqual(result.verdict, FAIL)
        self.assertEqual(result.evidence, [1])

    def test_skipped_when_boundary_fault_never_fires(self):
        events = [_k4(1, 50_000_000, open_=True)]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["no_early_trip"]
        self.assertEqual(result.verdict, SKIPPED)


class AtEndTests(unittest.TestCase):
    def setUp(self):
        self.scenario = load_scenario_text(WELDED_SSR_YAML)

    def test_passes_when_final_state_matches(self):
        events = [
            _fault_fired(1, 300_000_000),
            _k4(2, 302_000_000, open_=True),
        ]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["trip_latched"]
        self.assertEqual(result.verdict, PASS)

    def test_fails_when_final_state_reverts(self):
        # K4 opened then re-closed -- the LATEST observation governs at_end.
        events = [
            _fault_fired(1, 300_000_000),
            _k4(2, 302_000_000, open_=True),
            _k4(3, 320_000_000, open_=False),
        ]
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["trip_latched"]
        self.assertEqual(result.verdict, FAIL)
        self.assertEqual(result.evidence, [3])

    def test_fails_when_never_observed(self):
        events = []
        report = evaluate_expectations(self.scenario, events)
        result = {r.name: r for r in report.expectations}["trip_latched"]
        self.assertEqual(result.verdict, FAIL)


class OverallVerdictAndValidityTests(unittest.TestCase):
    def setUp(self):
        self.scenario = load_scenario_text(WELDED_SSR_YAML)

    def test_overall_pass_when_every_expectation_passes(self):
        events = [
            _fault_fired(1, 300_000_000),
            _k4(2, 302_000_000, open_=True),
        ]
        report = evaluate_expectations(self.scenario, events)
        self.assertEqual(report.verdict, PASS)
        self.assertTrue(report.passed)

    def test_overall_fail_when_any_expectation_fails(self):
        events = [
            _fault_fired(1, 300_000_000),
            _k4(2, 310_000_000, open_=True),  # too late -> safety_trips FAILs
        ]
        report = evaluate_expectations(self.scenario, events)
        self.assertEqual(report.verdict, FAIL)
        self.assertFalse(report.passed)

    def test_event_seq_gap_marks_run_invalid_and_fails_overall(self):
        events = [
            _fault_fired(1, 300_000_000),
            _k4(3, 302_000_000, open_=True),  # seq jumps 1 -> 3, gap at 2
        ]
        report = evaluate_expectations(self.scenario, events)
        self.assertTrue(report.validity.event_seq_gap)
        self.assertFalse(report.validity.valid)
        self.assertEqual(report.verdict, FAIL)

    def test_spi_underrun_flag_marks_run_invalid(self):
        events = [
            _fault_fired(1, 300_000_000),
            _k4(2, 302_000_000, open_=True),
        ]
        report = evaluate_expectations(self.scenario, events, spi_underrun=True)
        self.assertTrue(report.validity.spi_underrun)
        self.assertFalse(report.validity.valid)
        self.assertEqual(report.verdict, FAIL)

    def test_no_gap_when_seqs_are_consecutive_regardless_of_input_order(self):
        events = [
            _k4(2, 302_000_000, open_=True),
            _fault_fired(1, 300_000_000),
        ]
        report = evaluate_expectations(self.scenario, events)
        self.assertFalse(report.validity.event_seq_gap)

    def test_scenario_metadata_carried_through(self):
        report = evaluate_expectations(self.scenario, [])
        self.assertEqual(report.scenario_name, "welded_ssr_midfire")
        self.assertEqual(report.scenario_version, 1)
        self.assertEqual(report.seed, 42)
        self.assertEqual(report.timescale, 10)

    def test_explicit_seed_and_timescale_override_scenario_defaults(self):
        report = evaluate_expectations(self.scenario, [], seed=7, timescale=1.0)
        self.assertEqual(report.seed, 7)
        self.assertEqual(report.timescale, 1.0)


class SerializationRoundTripTests(unittest.TestCase):
    def test_to_dict_and_back(self):
        scenario = load_scenario_text(WELDED_SSR_YAML)
        events = [
            _fault_fired(1, 300_000_000),
            _k4(2, 302_000_000, open_=True),
        ]
        report = evaluate_expectations(scenario, events)
        restored = report_from_dict(report.to_dict())
        self.assertEqual(restored.scenario_name, report.scenario_name)
        self.assertEqual(restored.verdict, report.verdict)
        self.assertEqual(len(restored.events), len(report.events))
        self.assertEqual(
            [r.verdict for r in restored.expectations],
            [r.verdict for r in report.expectations],
        )

    def test_to_json_is_valid_json(self):
        import json

        scenario = load_scenario_text(WELDED_SSR_YAML)
        report = evaluate_expectations(scenario, [])
        parsed = json.loads(report.to_json())
        self.assertEqual(parsed["scenario"]["name"], "welded_ssr_midfire")


if __name__ == "__main__":
    unittest.main()
