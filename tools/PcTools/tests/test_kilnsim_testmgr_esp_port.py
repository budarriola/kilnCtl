#!/usr/bin/env python3
"""The ESP presence probe must be given the ESP's port, never the fixture's.

Found on real hardware (2026-08-24), not by a unit test: a run of
`kilnsim --port COM13 testmgr --quick` against the real SimFW fixture
reported

    ESP: absent -- ESP not reachable: could not open port 'COM13':
         PermissionError(13, 'Access is denied.', None, 5)

COM13 is the FIXTURE's port, which kilnsim itself already had open. The ESP
is a separate device on a separate port. Two distinct failures fell out of
one wrong argument: tier 1 and tier 2 could never be reached on any run that
passed `--port`, and the reason printed named the wrong device -- worse than
a bare "absent", because it sends whoever reads it to check a cable that was
never the problem.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import cli as _cli  # noqa: E402
from kilnsim import testmgr as tm  # noqa: E402
from kilnsim.link import MockSimLink  # noqa: E402


class EspProbeReceivesEspPortTests(unittest.TestCase):
    def setUp(self):
        self._real_probe = tm.default_esp_and_saftyfw_probe
        self.seen: "list" = []

        def recording_probe(port=None):
            self.seen.append(port)
            na = tm.PresenceResult(False, "stub: not probed")
            return na, na

        tm.default_esp_and_saftyfw_probe = recording_probe

    def tearDown(self):
        tm.default_esp_and_saftyfw_probe = self._real_probe

    def _run(self, **kwargs):
        link = MockSimLink()
        return tm.run_suite(
            link, tm.default_scenarios_dir(), quick=True,
            # must report PRESENT: run_suite only probes the ESP tier when the
            # fixture itself answered, so a stub absent fixture would skip the
            # very call these tests exist to inspect.
            fixture_probe=lambda l, p: tm.PresenceResult(True, "stub: fixture present"),
            **kwargs,
        )

    def test_fixture_port_is_never_handed_to_the_esp_probe(self):
        """The exact regression: --port names the fixture, and that value
        must not reach the ESP probe."""
        self._run(port="COM13")
        self.assertEqual(self.seen, [None],
                          "the ESP probe was given the fixture's port")

    def test_esp_port_is_handed_through_when_given(self):
        self._run(port="COM13", esp_port="COM9")
        self.assertEqual(self.seen, ["COM9"])

    def test_no_ports_at_all_still_probes_with_none(self):
        self._run()
        self.assertEqual(self.seen, [None])


class CliEspPortWiringTests(unittest.TestCase):
    def test_parser_accepts_esp_port_and_defaults_to_none(self):
        parser = _cli.build_parser()
        args = parser.parse_args(["--port", "COM13", "testmgr", "--quick"])
        self.assertIsNone(args.esp_port)
        args = parser.parse_args(
            ["--port", "COM13", "testmgr", "--esp-port", "COM9"])
        self.assertEqual(args.esp_port, "COM9")

    def test_cmd_testmgr_forwards_esp_port_to_run_suite(self):
        captured = {}

        real_run_suite = _cli._testmgr.run_suite

        def fake_run_suite(link, scenarios_dir=None, **kwargs):
            captured.update(kwargs)
            raise SystemExit(0)

        _cli._testmgr.run_suite = fake_run_suite
        try:
            parser = _cli.build_parser()
            args = parser.parse_args(
                ["--mock", "testmgr", "--quick", "--esp-port", "COM9"])
            with self.assertRaises(SystemExit):
                args.func(args)
        finally:
            _cli._testmgr.run_suite = real_run_suite

        self.assertEqual(captured.get("esp_port"), "COM9")
        # and the fixture port stays its own separate argument
        self.assertIn("port", captured)


if __name__ == "__main__":
    unittest.main()
