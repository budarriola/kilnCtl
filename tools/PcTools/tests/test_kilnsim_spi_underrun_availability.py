#!/usr/bin/env python3
"""`validity.spi_underrun_signal_available` must mean "somebody actually
looked", not "nobody objected".

Only `kilnsim.runner.run_scenario` polls TELEMETRY for `spi_underrun_count`
(see its own baseline/compare logic and `report.py`'s
`ValidityFlags.spi_underrun_signal_available` doc comment). Three callers
evaluate a scenario without going through it -- `mcp_server
.run_test_scenario`, `cli.cmd_run --mock`, and `testmgr.run_one_scenario`'s
mock branch -- and while the flag defaulted True, every report they produced
claimed the signal had been observed and was clean. `run_test_scenario` is
the serious one: `_link` there may be a real fixture, so a genuine SPI
underrun during an MCP-driven run was invisible.

These tests pin the honest default. Run with:
    python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import mcp_server  # noqa: E402
from kilnsim.link import MockSimLink  # noqa: E402
from kilnsim.report import evaluate_expectations  # noqa: E402
from kilnsim.scenario import load_scenario  # noqa: E402
from kilnsim.testmgr import default_scenarios_dir  # noqa: E402


def _a_scenario():
    return load_scenario(default_scenarios_dir() / "baseline_firing.yaml")


class EvaluateExpectationsDefaultTests(unittest.TestCase):
    def test_default_is_not_measured(self):
        """A caller that never mentions the parameter has, by definition, not
        polled TELEMETRY -- so the report must not claim it did."""
        report = evaluate_expectations(_a_scenario(), [])
        self.assertFalse(report.validity.spi_underrun_signal_available)

    def test_a_caller_that_measured_can_still_say_so(self):
        report = evaluate_expectations(_a_scenario(), [],
                                        spi_underrun_signal_available=True)
        self.assertTrue(report.validity.spi_underrun_signal_available)

    def test_unavailable_signal_does_not_by_itself_redden_validity(self):
        """"We didn't check" is not "we checked and it's bad" -- the same
        distinction BLOCKED vs FAIL preserves for expect clauses."""
        report = evaluate_expectations(_a_scenario(), [])
        self.assertFalse(report.validity.spi_underrun_signal_available)
        self.assertTrue(report.validity.valid)


class McpRunTestScenarioTests(unittest.TestCase):
    def setUp(self):
        mcp_server._link = MockSimLink()
        mcp_server._link.connect("MOCK")

    def tearDown(self):
        if mcp_server._link.is_connected:
            mcp_server._link.disconnect()

    def test_run_test_scenario_reports_the_signal_as_unmeasured(self):
        """This tool never calls run_scenario, so it never polls TELEMETRY --
        against a real fixture link as much as against this mock."""
        out = mcp_server.run_test_scenario(
            str(default_scenarios_dir() / "baseline_firing.yaml"))
        run_id = int(out.splitlines()[0].split(":")[1].strip())
        report = json.loads(mcp_server.get_test_report(run_id))
        self.assertFalse(report["validity"]["spi_underrun_signal_available"])


if __name__ == "__main__":
    unittest.main()
