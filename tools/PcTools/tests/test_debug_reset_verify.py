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
from kilnctrl.http_auth import HttpAuthError  # noqa: E402
from kilnctrl.ota_http_client import OtaHttpError  # noqa: E402
from kilnctrl import mcp_server_debug as md  # noqa: E402


class FakeClock:
    def __init__(self):
        self.t = 0.0

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.t += s


def _run(get_boot_guard, get_uart=None, window_s=60.0, clk=None, hosts_fn=None):
    clk = clk or FakeClock()
    return reset_probe.probe_after_reset(
        hosts_fn=hosts_fn or (lambda: ["10.0.0.5"]),
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
                   lambda timeout: "fw1")
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

        res = _run(bg, lambda timeout: (_ for _ in ()).throw(RuntimeError("no reply")), window_s=10.0)
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


    def test_401_counts_as_answered_not_silent(self):
        def bg(host, timeout):
            raise OtaHttpError("/api/boot_guard refused: HTTP 401", 401, "unauthorized")

        res = _run(bg, window_s=30.0)
        self.assertEqual(res.http_answered_s, 0.0)
        self.assertIsNone(res.boot_guard)
        text = reset_probe.format_report("esp", "run", res)
        self.assertNotIn("WARNING", text)
        self.assertIn("recovery state unknown", text)
        self.assertIn("HTTP 401", text)

    def test_auth_error_counts_as_answered(self):
        def bg(host, timeout):
            raise HttpAuthError("no credential in the environment")

        res = _run(bg, window_s=30.0)
        self.assertIsNotNone(res.http_answered_s)
        self.assertTrue(res.any_answered)
        self.assertNotIn("WARNING", reset_probe.format_report("esp", "run", res))

    def test_transport_error_with_no_status_is_not_answered(self):
        def bg(host, timeout):
            raise OtaHttpError("/api/boot_guard unreachable: timed out")

        res = _run(bg, window_s=4.0)
        self.assertIsNone(res.http_answered_s)

    def test_uart_first_keeps_polling_http_and_reads_recovery_mode(self):
        calls = {"n": 0}

        def bg(host, timeout):
            calls["n"] += 1
            if calls["n"] < 6:
                raise OSError("down")
            return {"boot_count": 4, "recovery_mode": True}

        res = _run(bg, lambda timeout: "fw1", window_s=60.0)
        self.assertEqual(res.uart_answered_s, 0.0)
        self.assertEqual(res.http_answered_s, 10.0)
        self.assertTrue(res.recovery_mode)
        self.assertTrue(res.recovery_state_known)
        self.assertIn("recovery_mode=true", reset_probe.format_report("esp", "run", res))

    def test_uart_only_reports_recovery_unknown_and_real_elapsed(self):
        def bg(host, timeout):
            raise OSError("down")

        res = _run(bg, lambda timeout: "fw1", window_s=10.0)
        self.assertIsNone(res.http_answered_s)
        self.assertEqual(res.elapsed_s, 10.0)
        text = reset_probe.format_report("esp", "run", res)
        self.assertNotIn("answer over HTTP or UART", text)
        self.assertIn("HTTP: no answer after 10s (recovery state unknown)", text)
        self.assertIn("recovery_mode was never read", text)

    def test_deadline_not_overshot_and_hosts_resolved_once(self):
        clk = FakeClock()
        seen = []
        resolves = {"n": 0}

        def hosts():
            resolves["n"] += 1
            return ["a", "b", "c"]

        def bg(host, timeout):
            seen.append(timeout)
            clk.t += timeout  # an unreachable host burns its whole timeout
            raise OSError("timed out")

        def uart(timeout):
            seen.append(timeout)
            clk.t += timeout
            raise RuntimeError("no reply")

        res = _run(bg, uart, window_s=25.0, clk=clk, hosts_fn=hosts)
        self.assertLessEqual(clk.t, 25.0 + 1e-9)
        self.assertEqual(resolves["n"], 1)
        self.assertTrue(all(t <= reset_probe.HTTP_ATTEMPT_TIMEOUT_S for t in seen))
        self.assertFalse(res.any_answered)
        self.assertEqual(res.elapsed_s, 25.0)

    def test_report_uses_elapsed_not_window(self):
        res = reset_probe.ProbeResult(window_s=60.0, elapsed_s=10.0, uart_answered_s=1.0, uart_fw="fw1")
        text = reset_probe.format_report("esp", "run", res)
        self.assertIn("HTTP: no answer after 10s", text)
        self.assertIn("UART link answered", text)
        res2 = reset_probe.ProbeResult(window_s=60.0, elapsed_s=10.0)
        self.assertIn("UART link: no answer after 10s", reset_probe.format_report("esp", "run", res2))

    def test_uart_timeout_clamped_to_remaining_budget(self):
        clk = FakeClock()
        seen = []

        def bg(host, timeout):
            raise OSError("down")

        def uart(timeout):
            seen.append(timeout)
            clk.t += timeout
            raise RuntimeError("no reply")

        _run(bg, uart, window_s=1.0, clk=clk)
        self.assertEqual(seen[0], 1.0)
        self.assertLessEqual(clk.t, 1.0 + 1e-9)

    def test_errors_deduped_capped_and_reported(self):
        def bg(host, timeout):
            raise OSError("boom")

        res = _run(bg, window_s=30.0, hosts_fn=lambda: [f"h{i}" for i in range(9)])
        self.assertLessEqual(len(res.errors), reset_probe.MAX_ERRORS_REPORTED)
        self.assertEqual(len(res.errors), len(set(res.errors)))
        self.assertEqual(res.to_json()["errors"], res.errors)
        self.assertIn("probe errors: h0: boom", reset_probe.format_report("esp", "run", res))


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


    def test_probe_raising_keeps_reset_result_and_logs(self):
        with unittest.mock.patch.object(md, "_probe_esp_after_reset", side_effect=RuntimeError("kaboom")):
            out = md.debug_reset(peer="esp", mode="run")
        self.assertTrue(out.startswith("reset esp (run) OK"))
        self.assertIn("UNVERIFIED", out)
        self.assertIn("kaboom", self._history()[0]["probe_error"])

    def test_reset_raising_is_logged_and_surfaced(self):
        # The _tool() guard turns a raised exception into an "error: ..." string.
        with unittest.mock.patch.object(debug_probe, "reset", side_effect=ValueError("bad mode")):
            out = md.debug_reset(peer="esp", mode="bogus")
        self.assertTrue(out.startswith("error"), out)
        rec = self._history()[0]
        self.assertIn("bad mode", rec["reset_raised"])
        self.assertIsNone(rec["openocd_ok"])

    def test_probe_wiring_passes_right_callables(self):
        import kilnctrl.mcp_server_flash as mf
        fake_info = unittest.mock.Mock()
        fake_info.get_fw_version.return_value = "fw"
        with unittest.mock.patch.object(reset_probe, "probe_after_reset", return_value="R") as pr,                 unittest.mock.patch.object(mf, "_resolve_verify_hosts", return_value=["h1"]) as rh,                 unittest.mock.patch.object(md._srv, "_info", fake_info, create=True):
            out = md._probe_esp_after_reset(12.0)
            self.assertEqual(out, "R")
            kw = pr.call_args.kwargs
            self.assertEqual(kw["window_s"], 12.0)
            self.assertIs(kw["get_boot_guard"], md.ota_http.get_boot_guard_status)
            self.assertEqual(kw["hosts_fn"](), ["h1"])
            rh.assert_called_once_with(None)
            # The callables resolve _srv._info lazily, so they must be
            # invoked while the fake is still patched in (never a real link).
            self.assertEqual(kw["get_uart_fw"](timeout=1.5), "fw")
            fake_info.get_fw_version.assert_called_once_with(timeout=1.5)


if __name__ == "__main__":
    unittest.main()
