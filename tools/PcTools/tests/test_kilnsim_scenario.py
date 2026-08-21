#!/usr/bin/env python3
"""Tests for kilnsim.scenario -- the YAML scenario loader/validator and
fault-schedule compiler, per firmware/SimFW/docs/DESIGN_NOTES.md section 8.1's
schema.

No hardware, no link -- pure text-in, dataclasses-out.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim.protocol import DurationKind, Edge, RepeatKind, TriggerKind  # noqa: E402
from kilnsim.scenario import (  # noqa: E402
    AtEndExpect,
    EventThenExpect,
    ForbidExpect,
    ScenarioError,
    compile_faults,
    load_scenario_text,
)

#: The exact example from DESIGN_NOTES.md section 8.1.
WELDED_SSR_YAML = """
name: welded_ssr_midfire
version: 1
exercises: [S3, S4]
preset: fast_test
timescale: 10
seed: 42
overrides:
  zones[0].R_element: 12.0
dut:
  profile: cone6_fast
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
report_keep: [telemetry, events]
"""


class LoadPlanExampleTests(unittest.TestCase):
    """The DESIGN_NOTES.md sec 8.1 example must parse cleanly end to end."""

    def setUp(self):
        self.scenario = load_scenario_text(WELDED_SSR_YAML)

    def test_top_level_fields(self):
        self.assertEqual(self.scenario.name, "welded_ssr_midfire")
        self.assertEqual(self.scenario.version, 1)
        self.assertEqual(self.scenario.exercises, ["S3", "S4"])
        self.assertEqual(self.scenario.preset, "fast_test")
        self.assertEqual(self.scenario.timescale, 10)
        self.assertEqual(self.scenario.seed, 42)
        self.assertEqual(self.scenario.overrides, {"zones[0].R_element": 12.0})
        self.assertEqual(self.scenario.dut.profile, "cone6_fast")
        self.assertEqual(self.scenario.report_keep, ["telemetry", "events"])

    def test_one_fault_parsed(self):
        self.assertEqual(len(self.scenario.faults), 1)
        f = self.scenario.faults[0]
        self.assertEqual(f.id, "weld")
        self.assertEqual(f.type, "welded_ssr")
        self.assertEqual(f.target, "relay:K1")
        self.assertEqual(f.trigger.kind, TriggerKind.AT_ZONE_TEMP)
        self.assertEqual(f.trigger.zone, 0)
        self.assertEqual(f.trigger.temp_c, 400)
        self.assertEqual(f.trigger.edge, Edge.RISING)
        self.assertEqual(f.duration.kind, DurationKind.PERMANENT)
        self.assertEqual(f.repeat.kind, RepeatKind.ONCE)

    def test_three_expect_clauses_of_each_kind(self):
        self.assertEqual(len(self.scenario.expect), 3)
        self.assertIsInstance(self.scenario.expect[0], EventThenExpect)
        self.assertEqual(self.scenario.expect[0].name, "safety_trips")
        self.assertEqual(self.scenario.expect[0].event, {"type": "fault_fired", "slot": "weld"})
        self.assertEqual(self.scenario.expect[0].then, {"dut": "K4_open", "within_s": 5})
        self.assertIsInstance(self.scenario.expect[1], ForbidExpect)
        self.assertEqual(self.scenario.expect[1].forbid["dut"], "K4_open")
        self.assertIsInstance(self.scenario.expect[2], AtEndExpect)
        self.assertEqual(self.scenario.expect[2].at_end, {"dut": "K4_open"})

    def test_content_hash_is_stable(self):
        again = load_scenario_text(WELDED_SSR_YAML)
        self.assertEqual(self.scenario.content_hash(), again.content_hash())


class CompileFaultsTests(unittest.TestCase):
    def test_compiles_to_one_command_per_fault_in_order(self):
        scenario = load_scenario_text(WELDED_SSR_YAML)
        compiled = compile_faults(scenario)
        self.assertEqual(len(compiled), 1)
        cmd = compiled[0]
        self.assertEqual(cmd.fault_slot, 0)
        self.assertEqual(cmd.fault_type, "welded_ssr")
        self.assertEqual(cmd.target, "relay:K1")
        self.assertEqual(cmd.params, (0.0, 0.0, 0.0, 0.0))

    def test_slot_order_matches_file_order(self):
        yaml_text = """
name: two_faults
version: 1
faults:
  - id: a
    type: tc_noise
    target: tc:0
    trigger: { manual: {} }
  - id: b
    type: tc_stuck
    target: tc:1
    trigger: { manual: {} }
"""
        scenario = load_scenario_text(yaml_text)
        compiled = compile_faults(scenario)
        self.assertEqual([c.fault_slot for c in compiled], [0, 1])
        self.assertEqual([c.fault_type for c in compiled], ["tc_noise", "tc_stuck"])


class ValidationTests(unittest.TestCase):
    def test_missing_name_rejected(self):
        with self.assertRaises(ScenarioError):
            load_scenario_text("version: 1\n")

    def test_missing_version_rejected(self):
        with self.assertRaises(ScenarioError):
            load_scenario_text("name: x\n")

    def test_not_a_mapping_rejected(self):
        with self.assertRaises(ScenarioError):
            load_scenario_text("- just\n- a\n- list\n")

    def test_invalid_yaml_rejected(self):
        with self.assertRaises(ScenarioError):
            load_scenario_text("name: [unterminated\n")

    def test_duplicate_fault_ids_rejected(self):
        yaml_text = """
name: dup
version: 1
faults:
  - id: same
    type: tc_noise
    target: tc:0
    trigger: { manual: {} }
  - id: same
    type: tc_stuck
    target: tc:1
    trigger: { manual: {} }
"""
        with self.assertRaises(ScenarioError):
            load_scenario_text(yaml_text)

    def test_unknown_trigger_kind_rejected(self):
        yaml_text = """
name: bad_trigger
version: 1
faults:
  - id: a
    type: tc_noise
    target: tc:0
    trigger: { nonsense: {} }
"""
        with self.assertRaises(ScenarioError):
            load_scenario_text(yaml_text)

    def test_expect_clause_with_no_recognized_form_rejected(self):
        yaml_text = """
name: bad_expect
version: 1
expect:
  - name: nothing_here
"""
        with self.assertRaises(ScenarioError):
            load_scenario_text(yaml_text)

    def test_expect_clause_with_two_forms_rejected(self):
        yaml_text = """
name: ambiguous_expect
version: 1
expect:
  - name: both
    forbid: { dut: K4_open, before: { sim_time: 1 } }
    at_end: { dut: K4_open }
"""
        with self.assertRaises(ScenarioError):
            load_scenario_text(yaml_text)

    def test_after_fault_referencing_unknown_fault_rejected(self):
        yaml_text = """
name: bad_ref
version: 1
faults:
  - id: a
    type: tc_noise
    target: tc:0
    trigger: { after_fault: { fault: does_not_exist } }
"""
        with self.assertRaises(ScenarioError):
            load_scenario_text(yaml_text)

    def test_random_in_with_t1_before_t0_rejected(self):
        yaml_text = """
name: bad_random
version: 1
faults:
  - id: a
    type: tc_noise
    target: tc:0
    trigger: { random_in: { t0: 10, t1: 5 } }
"""
        with self.assertRaises(ScenarioError):
            load_scenario_text(yaml_text)


class ManualChecksAndOptionalFieldsTests(unittest.TestCase):
    def test_minimal_scenario_defaults(self):
        scenario = load_scenario_text("name: minimal\nversion: 1\n")
        self.assertEqual(scenario.timescale, 1.0)
        self.assertEqual(scenario.seed, 0)
        self.assertEqual(scenario.faults, [])
        self.assertEqual(scenario.expect, [])
        self.assertIsNone(scenario.dut)
        self.assertEqual(scenario.manual_checks, [])

    def test_manual_checks_preserved(self):
        yaml_text = """
name: with_manual
version: 1
manual_checks: [lcd_shows_fault, web_dashboard_banner]
"""
        scenario = load_scenario_text(yaml_text)
        self.assertEqual(scenario.manual_checks, ["lcd_shows_fault", "web_dashboard_banner"])


class RealScenarioFilesSmokeTest(unittest.TestCase):
    """Opportunistic smoke test against firmware/SimFW/scenarios/*.yaml, if
    a parallel effort has already written any by the time this runs (see
    this module's own docstring / the task brief: not a dependency, just a
    bonus check when the files happen to exist)."""

    def test_any_existing_scenario_files_parse(self):
        import glob

        repo_root = os.path.join(os.path.dirname(__file__), "..", "..", "..")
        pattern = os.path.join(repo_root, "firmware", "SimFW", "scenarios", "*.yaml")
        paths = glob.glob(pattern)
        if not paths:
            self.skipTest("no firmware/SimFW/scenarios/*.yaml files exist yet")
        from kilnsim.scenario import load_scenario

        for path in paths:
            with self.subTest(path=path):
                load_scenario(path)  # must not raise


if __name__ == "__main__":
    unittest.main()
