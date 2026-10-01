#!/usr/bin/env python3
"""Tests for debug_reset's post-reset reachability probe and durable history
(reset_probe.py, mcp_server_debug.debug_reset). Fakes only -- no board, no
OpenOCD, no network.

Run with: python -m pytest tools/PcTools/tests/test_debug_reset_verify.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import debug_probe, reset_probe  # noqa: E402
from kilnctrl import mcp_server_debug as md  # noqa: E402


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


def _run(get_boot_guard, get_uart=None, window_s=60.0):
    clk = FakeClock()
    return reset_probe.probe_after_reset(
        hosts_fn=lambda: ["10.0.0.5"],
        get_boot_guard=get_boot_guard,
        get_uart_fw=get_uart,
        window_s=window_s,
        interval_s=2.0,
        clock=clk,
        sleep=clk.sleep,
    )


class ProbeTest(unittest.TestCase):
    def test_answers_immediately(self):
        res = _run(lambda host, timeout: {"boot_count": 1, "persisted_count": 0, "recovery_mode": False},
                   lambda: "fw1")
        self.assertEqual(res.http_answered_s, 0.0)
        self.assertEqual(res.uart_answered_s, 0.0)
        self.assertFalse(res.recovery_mode)
        text = reset_probe.format_report("esp", "run", res)
        self.assertNotIn("WARNING", text)
        self.assertIn("persisted_count=0", text)

    def test_answers_late(self):
        calls = {"n": 0}

        def bg(host, timeout):
            calls["n"] += 1
            if calls["n"] < 4:
                raise OSError("unreachable")
            return {"boot_count": 2, "recovery_mode": False}

        res = _run(bg)
        self.assertEqual(res.http_answered_s, 6.0)  # 3 failed polls x 2 s
        self.assertIsNone(res.uart_answered_s)
        self.assertNotIn("WARNING", reset_probe.format_report("esp", "run", res))

    def test_never_answers_warns_and_mentions_s6b(self):
        def bg(host, timeout):
            raise OSError("down")

        res = _run(bg, lambda: (_ for _ in ()).throw(RuntimeError("no reply")), window_s=10.0)
        self.assertFalse(res.any_answered)
        text = reset_probe.format_report("esp", "run", res)
        self.assertTrue(text.startswith("WARNING"))
        self.assertIn("S6b", text)
        self.assertIn("0x0040", text)
        self.assertIn("ANNOUNCE_REBOOT", text)
        self.assertIn("debug_read_registers", text)

    def test_recovery_mode_reported_loudly(self):
        res = _run(lambda host, timeout: {"boot_count": 3, "recovery_mode": True})
        self.assertTrue(res.recovery_mode)
        text = reset_probe.format_report("esp", "run", res)
        self.assertTrue(text.startswith("WARNING"))
        self.assertIn("recovery_mode=true", text)


class DebugResetWiringTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        patches = [
            unittest.mock.patch.object(debug_probe, "reset", return_value=(True, "ok")),
            unittest.mock.patch.object(debug_probe, "_repo_root", return_value=self.tmp.name),
            unittest.mock.patch.object(md._srv, "_session_log"),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def _history(self):
        path = reset_probe.history_path(self.tmp.name)
        with open(path, encoding="utf-8") as fh:
            return [json.loads(line) for line in fh if line.strip()]

    def test_verify_false_skips_probe(self):
        with unittest.mock.patch.object(md, "_probe_esp_after_reset") as probe:
            out = md.debug_reset(peer="esp", mode="run", verify=False)
        probe.assert_not_called()
        self.assertEqual(out, "reset esp (run) OK")
        rec = self._history()[0]
        self.assertEqual(rec["probe_skipped"], "verify=False")

    def test_verify_runs_probe_and_writes_history(self):
        res = reset_probe.ProbeResult(window_s=60.0, http_answered_s=4.0, http_host="h",
                                      boot_guard={"boot_count": 1, "recovery_mode": False})
        with unittest.mock.patch.object(md, "_probe_esp_after_reset", return_value=res) as probe:
            out = md.debug_reset(peer="esp", mode="run")
        probe.assert_called_once()
        self.assertIn("HTTP answered at h after 4.0s", out)
        recs = self._history()
        self.assertEqual(len(recs), 1)
        self.assertEqual(recs[0]["peer"], "esp")
        self.assertEqual(recs[0]["mode"], "run")
        self.assertTrue(recs[0]["openocd_ok"])
        self.assertEqual(recs[0]["probe"]["http_answered_s"], 4.0)
        self.assertIn("ts", recs[0])

    def test_silent_board_returns_warning_through_tool(self):
        res = reset_probe.ProbeResult(window_s=60.0)
        with unittest.mock.patch.object(md, "_probe_esp_after_reset", return_value=res):
            out = md.debug_reset(peer="esp", mode="run")
        self.assertIn("WARNING", out)
        self.assertIn("S6b", out)
        self.assertFalse(self._history()[0]["probe"]["any_answered"])

    def test_pico_and_halt_mode_do_not_probe_but_log(self):
        with unittest.mock.patch.object(md, "_probe_esp_after_reset") as probe:
            md.debug_reset(peer="pico", mode="run")
            md.debug_reset(peer="esp", mode="halt")
        probe.assert_not_called()
        self.assertEqual(len(self._history()), 2)

    def test_failed_reset_logged_without_probe(self):
        with unittest.mock.patch.object(debug_probe, "reset", return_value=(False, "Error: boom")), \
                unittest.mock.patch.object(md, "_probe_esp_after_reset") as probe:
            md.debug_reset(peer="esp", mode="run")
        probe.assert_not_called()
        rec = self._history()[0]
        self.assertFalse(rec["openocd_ok"])
        self.assertEqual(rec["openocd_decisive_line"], "Error: boom")


if __name__ == "__main__":
    unittest.main()
