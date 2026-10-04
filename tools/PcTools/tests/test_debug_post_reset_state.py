#!/usr/bin/env python3
"""debug_reset post-reset curstate check + fallback resume, per-peer register
lists, catch-then-resume guarding and per-target debug_resume. Mocked OpenOCD
only -- no board.

Run with: python -m pytest tools/PcTools/tests/test_debug_post_reset_state.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import debug_probe  # noqa: E402
from kilnctrl import mcp_server_debug as md  # noqa: E402


class _Base(unittest.TestCase):
    def setUp(self):
        self.run_mock = unittest.mock.Mock(return_value=(True, "ok"))
        for p in (
            unittest.mock.patch.object(debug_probe.openocd_util, "run_openocd", self.run_mock),
            unittest.mock.patch.object(debug_probe, "_openocd_exe_or_raise", return_value="fake.exe"),
            unittest.mock.patch.object(debug_probe, "_refuse_if_esp_adapter_absent", return_value=None),
        ):
            p.start()
            self.addCleanup(p.stop)

    def tcl(self) -> str:
        return self.run_mock.call_args.args[2]


class ResetTclTest(_Base):
    def test_esp_run_has_poll_and_conditional_resume_after_reset(self):
        debug_probe.reset(debug_probe.PEER_ESP, "run")
        t = self.tcl()
        self.assertIn("curstate", t)
        self.assertIn("sleep 100", t)
        self.assertIn("KCTL_STATE", t)
        self.assertIn('if {$_kctl_s eq "halted"}', t)
        self.assertIn("resume", t.split("reset run", 1)[1])
        self.assertLess(t.index("reset run"), t.index("curstate"))

    def test_halt_and_init_modes_never_resume(self):
        for mode in ("halt", "init"):
            debug_probe.reset(debug_probe.PEER_ESP, mode)
            t = self.tcl()
            self.assertNotIn("curstate", t)
            self.assertNotIn("resume", t)

    def test_pico_run_has_no_state_poll(self):
        debug_probe.reset(debug_probe.PEER_PICO, "run")
        self.assertNotIn("curstate", self.tcl())


class ParseAndFailureTest(_Base):
    def test_parse_lines(self):
        out = "x\nKCTL_STATE esp32s3.cpu0 running\nKCTL_STATE esp32s3.cpu1 halted\n" \
              "KCTL_RESUMED esp32s3.cpu1\nKCTL_FINAL esp32s3.cpu1 running\n"
        info = debug_probe.parse_post_reset(out)
        self.assertEqual(info["states"], {"esp32s3.cpu0": "running", "esp32s3.cpu1": "halted"})
        self.assertEqual(info["resumed"], ["esp32s3.cpu1"])
        self.assertEqual(info["still_halted"], [])

    def test_halted_after_fallback_is_not_ok(self):
        self.run_mock.return_value = (True, "KCTL_STATE esp32s3.cpu0 halted\n"
                                      "KCTL_RESUMED esp32s3.cpu0\nKCTL_FINAL esp32s3.cpu0 halted\n")
        ok, out = debug_probe.reset(debug_probe.PEER_ESP, "run")
        self.assertFalse(ok)
        self.assertIn("NOT running", out)

    def test_resumed_ok_when_fallback_works(self):
        self.run_mock.return_value = (True, "KCTL_STATE esp32s3.cpu0 halted\n"
                                      "KCTL_RESUMED esp32s3.cpu0\nKCTL_FINAL esp32s3.cpu0 running\n")
        ok, _ = debug_probe.reset(debug_probe.PEER_ESP, "run")
        self.assertTrue(ok)


class McpResetTest(_Base):
    def _call(self, openocd_out):
        self.run_mock.return_value = (True, openocd_out)
        with tempfile.TemporaryDirectory() as d:
            with unittest.mock.patch.object(debug_probe, "_repo_root", return_value=d), \
                 unittest.mock.patch.object(md.reset_probe, "recent_dark_esp_reset", return_value=None):
                msg = md.debug_reset("esp", "run", verify=False)
            with open(os.path.join(d, "logs", "debug_reset", "history.jsonl"), encoding="utf-8") as f:
                rec = json.loads(f.readlines()[-1])
        return msg, rec

    def test_history_records_states_and_resume(self):
        msg, rec = self._call("KCTL_STATE esp32s3.cpu0 halted\nKCTL_RESUMED esp32s3.cpu0\n"
                              "KCTL_FINAL esp32s3.cpu0 running\n")
        self.assertIn("OK", msg)
        self.assertIn("fallback resume", msg)
        self.assertEqual(rec["resumed_targets"], ["esp32s3.cpu0"])
        self.assertIn("esp32s3.cpu0", rec["post_reset_states"])
        self.assertTrue(rec["openocd_ok"])

    def test_still_halted_is_loud_failure(self):
        msg, rec = self._call("KCTL_STATE esp32s3.cpu0 halted\nKCTL_RESUMED esp32s3.cpu0\n"
                              "KCTL_FINAL esp32s3.cpu0 halted\n")
        self.assertNotIn("reset esp (run) OK", msg)
        self.assertIn("=halted", msg)
        self.assertFalse(rec["openocd_ok"])


class RegistersTest(_Base):
    def test_esp_list_has_no_cortex_m_regs(self):
        debug_probe.read_registers(debug_probe.PEER_ESP)
        t = self.tcl()
        self.assertNotIn("r0 ", t)
        self.assertNotIn("xpsr", t)
        self.assertIn("get_reg {pc ps a0 a1", t)
        self.assertNotIn("r0", debug_probe._CORE_REGS_ESP.split())

    def test_pico_list_unchanged(self):
        debug_probe.read_registers(debug_probe.PEER_PICO)
        self.assertIn("xpsr", self.tcl())

    def test_dump_is_caught_and_resume_follows(self):
        debug_probe.read_registers(debug_probe.PEER_ESP)
        t = self.tcl()
        self.assertIn("catch {foreach {_kctl_n _kctl_v} [get_reg", t)
        self.assertIn("KCTL_ERR", t)
        after = t.split("_kctl_err]", 1)[1]
        self.assertIn("resume", after)

    def test_leave_halted_has_no_resume(self):
        debug_probe.read_registers(debug_probe.PEER_ESP, leave_halted=True)
        self.assertNotIn("resume", self.tcl())

    def test_caught_error_surfaces_as_failure(self):
        self.run_mock.return_value = (True, "KCTL_ERR unknown register 'r0'\n")
        ok, out = debug_probe.read_registers(debug_probe.PEER_ESP)
        self.assertFalse(ok)
        self.assertIn("unknown register", out)

    def test_read_memory_is_guarded_too(self):
        debug_probe.read_memory(debug_probe.PEER_ESP, 0x3FC80000)
        t = self.tcl()
        self.assertIn("catch {mem2array", t)
        self.assertIn("resume", t.split("_kctl_err]", 1)[1])


class ResumeTest(_Base):
    def test_running_target_is_informative_not_error(self):
        self.run_mock.return_value = (True, "KCTL_BEFORE cpu0 running\nKCTL_AFTER cpu0 running\n")
        ok, out = debug_probe.resume(debug_probe.PEER_ESP)
        self.assertTrue(ok)
        self.assertIn("cpu0: running (no resume needed)", out)
        self.assertNotIn("init; halt", self.tcl().split("curstate")[0])

    def test_halted_target_reported_and_failure_if_still_halted(self):
        self.run_mock.return_value = (True, "KCTL_BEFORE cpu0 halted\nKCTL_AFTER cpu0 running\n")
        ok, out = debug_probe.resume(debug_probe.PEER_ESP)
        self.assertTrue(ok)
        self.assertIn("cpu0: halted -> running", out)
        self.run_mock.return_value = (True, "KCTL_BEFORE cpu0 halted\nKCTL_AFTER cpu0 halted\n")
        ok, _ = debug_probe.resume(debug_probe.PEER_ESP)
        self.assertFalse(ok)


class ReviewFixesTest(_Base):
    def test_resume_failure_in_guarded_batch_is_not_ok(self):
        self.run_mock.return_value = (True, "REG pc 0x1\nKCTL_ERR resume: resume of a SMP target failed\n")
        ok, out = debug_probe.read_registers(debug_probe.PEER_PICO)
        self.assertFalse(ok)
        self.assertIn("resume of a SMP target failed", out)

    def test_guarded_tail_prints_resume_error(self):
        debug_probe.read_memory(debug_probe.PEER_PICO, 0x20000000)
        self.assertIn('KCTL_ERR resume: $_kctl_err2', self.tcl())

    def test_fallback_resume_error_text_printed(self):
        debug_probe.reset(debug_probe.PEER_ESP, "run")
        self.assertIn('KCTL_ERR resume $_kctl_t: $_kctl_err', self.tcl())

    def test_all_unknown_is_not_ok(self):
        self.run_mock.return_value = (True, "KCTL_STATE esp32s3.cpu0 unknown\n")
        ok, out = debug_probe.reset(debug_probe.PEER_ESP, "run")
        self.assertFalse(ok)
        self.assertIn("esp32s3.cpu0=unknown", out)

    def test_missing_state_lines_is_not_ok(self):
        self.run_mock.return_value = (True, "Info : something\n")
        ok, out = debug_probe.reset(debug_probe.PEER_ESP, "run")
        self.assertFalse(ok)
        self.assertIn("no KCTL_STATE", out)

    def test_resume_unknown_attempts_resume(self):
        debug_probe.resume(debug_probe.PEER_ESP)
        self.assertIn('$_kctl_s eq "unknown"', self.tcl())
        self.run_mock.return_value = (True, "KCTL_BEFORE cpu0 unknown\nKCTL_AFTER cpu0 running\n")
        ok, out = debug_probe.resume(debug_probe.PEER_ESP)
        self.assertTrue(ok)
        self.assertIn("cpu0: unknown -> running", out)


class McpFailureAndGuardTest(McpResetTest):
    def test_unknown_state_is_loud_and_history_keeps_states(self):
        msg, rec = self._call("KCTL_STATE esp32s3.cpu0 unknown\n")
        self.assertIn("FAILED", msg)
        self.assertFalse(rec["openocd_ok"])
        self.assertIn("esp32s3.cpu0", rec["post_reset_states"])
        self.assertTrue(rec["still_halted"])

    def test_guard_honours_still_halted_flag(self):
        import datetime
        from kilnctrl import reset_probe
        with tempfile.TemporaryDirectory() as d:
            ts = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S+00:00")
            rec = {"ts": ts, "peer": "esp", "mode": "run", "openocd_ok": False, "still_halted": True}
            self.assertIsNone(reset_probe.recent_dark_esp_reset(d))
            self.assertIsNone(reset_probe.append_history(d, rec))
            dark = reset_probe.recent_dark_esp_reset(d)
            self.assertIsNotNone(dark)
            later = datetime.datetime.now(datetime.timezone.utc) + datetime.timedelta(seconds=500)
            self.assertIsNone(reset_probe.recent_dark_esp_reset(d, now=later))
            # an openocd failure WITHOUT the flag stays "unknown, not dark"
            rec2 = dict(rec, still_halted=False)
            d2 = tempfile.mkdtemp()
            reset_probe.append_history(d2, rec2)
            self.assertIsNone(reset_probe.recent_dark_esp_reset(d2))


if __name__ == "__main__":
    unittest.main()
