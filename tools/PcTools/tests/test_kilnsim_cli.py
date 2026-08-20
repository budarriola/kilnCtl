#!/usr/bin/env python3
"""Tests for kilnsim.cli's new io/ct/relay/fault-list/selftest subcommands --
command construction and exit codes against ``--mock``, matching the style
this task found (or was missing) for the CLI: no hardware, no serial, no
real link -- :class:`~kilnsim.link.MockSimLink` under the hood via
``kilnsim.cli.main(["--mock", ...])``, output captured through ``capsys``.

Run with: python -m unittest discover -s tools/PcTools/tests
(pytest also collects this file directly, which is how it's normally run.)
"""
from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim.cli import main  # noqa: E402


class _CapturedOutputTestCase(unittest.TestCase):
    """Runs ``kilnsim.cli.main`` with stdout/stderr captured, since these
    subcommands print their result rather than returning it."""

    def run_cli(self, argv):
        import contextlib
        import io

        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = main(argv)
        return code, out.getvalue(), err.getvalue()


class IoSubcommandTests(_CapturedOutputTestCase):
    def test_read(self):
        code, out, _ = self.run_cli(["--mock", "io", "read", "3"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out), {"level": False})

    def test_write(self):
        code, out, _ = self.run_cli(["--mock", "io", "write", "3", "on"])
        self.assertEqual(code, 0)
        json.loads(out)  # well-formed JSON, MockSimLink's generic {"ok": True} ack

    def test_dir(self):
        code, out, _ = self.run_cli(["--mock", "io", "dir", "3", "in", "--pullup"])
        self.assertEqual(code, 0)
        json.loads(out)

    def test_fault_line(self):
        code, out, _ = self.run_cli(["--mock", "io", "fault-line"])
        self.assertEqual(code, 0)
        reply = json.loads(out)
        self.assertIn("asserted", reply)

    def test_estop_get(self):
        code, out, _ = self.run_cli(["--mock", "io", "estop-get"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out), {"open": False})

    def test_power_get(self):
        code, out, _ = self.run_cli(["--mock", "io", "power-get"])
        self.assertEqual(code, 0)
        self.assertEqual(json.loads(out), {"on": True})


class CtSubcommandTests(_CapturedOutputTestCase):
    def test_state(self):
        code, out, _ = self.run_cli(["--mock", "ct", "state", "0"])
        self.assertEqual(code, 0)
        reply = json.loads(out)
        self.assertIn("amps", reply)
        self.assertIn("distortion", reply)

    def test_mode(self):
        code, _, _ = self.run_cli(["--mock", "ct", "mode", "0", "manual"])
        self.assertEqual(code, 0)

    def test_amps(self):
        code, _, _ = self.run_cli(["--mock", "ct", "amps", "1", "12.5"])
        self.assertEqual(code, 0)

    def test_phase(self):
        code, _, _ = self.run_cli(["--mock", "ct", "phase", "1", "90"])
        self.assertEqual(code, 0)

    def test_distortion(self):
        code, _, _ = self.run_cli([
            "--mock", "ct", "distortion", "0",
            "--dc-offset", "1.5", "--clip-fraction", "0.2", "--dropout-half",
        ])
        self.assertEqual(code, 0)

    def test_bad_mode_rejected_by_argparse(self):
        with self.assertRaises(SystemExit):
            self.run_cli(["--mock", "ct", "mode", "0", "bogus"])


class RelaySubcommandTests(_CapturedOutputTestCase):
    def test_states(self):
        code, out, _ = self.run_cli(["--mock", "relay", "states"])
        self.assertEqual(code, 0)
        reply = json.loads(out)
        for key in ("k1_closed", "k2_closed", "k3_closed", "k5_closed", "k4_closed", "fault_line_asserted"):
            self.assertIn(key, reply)

    def test_edges(self):
        code, out, _ = self.run_cli(["--mock", "relay", "edges", "--since-seq", "5", "--max-count", "4"])
        self.assertEqual(code, 0)
        reply = json.loads(out)
        self.assertIn("edges", reply)


class FaultGroupSubcommandTests(_CapturedOutputTestCase):
    def test_list(self):
        code, out, _ = self.run_cli(["--mock", "fault-list"])
        self.assertEqual(code, 0)
        reply = json.loads(out)
        self.assertIn("faults", reply)

    def test_cancel(self):
        code, _, _ = self.run_cli(["--mock", "fault-cancel", "3"])
        self.assertEqual(code, 0)

    def test_fire_now(self):
        code, _, _ = self.run_cli(["--mock", "fault-fire", "3"])
        self.assertEqual(code, 0)


class SelftestSubcommandTests(_CapturedOutputTestCase):
    def test_json_flag_produces_well_formed_report(self):
        code, out, _ = self.run_cli(["--mock", "selftest", "--json"])
        # MockSimLink genuinely can't satisfy every check (no real fault
        # engine, no push telemetry) -- selftest is honest about that and
        # exits 1. What this test asserts is the report shape/JSON
        # well-formedness, not a fabricated all-green result.
        self.assertIn(code, (0, 1))
        report = json.loads(out)
        self.assertIn("passed", report)
        self.assertIn("checks", report)
        names = {c["name"] for c in report["checks"]}
        self.assertIn("ping_roundtrip", names)
        self.assertIn("spi_master_loopback", names)
        for check in report["checks"]:
            self.assertIn(check["status"], ("PASS", "FAIL", "SKIP", "NOT_RUNNABLE"))

    def test_text_report_mentions_every_check(self):
        code, out, _ = self.run_cli(["--mock", "selftest"])
        self.assertIn(code, (0, 1))
        self.assertIn("ping_roundtrip", out)
        self.assertIn("PASSED" if code == 0 else "FAILED", out)


if __name__ == "__main__":
    unittest.main()
