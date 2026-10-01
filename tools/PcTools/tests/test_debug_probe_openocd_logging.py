#!/usr/bin/env python3
"""Unit tests for mcp_server_debug.py's OpenOCD failure logging.

Gap found 2026-09-06: debug_reset() (and the other debug_* OpenOCD wrappers)
logged only "ok=False" to the session log, discarding OpenOCD's full
stdout/stderr -- a failed reset could not be diagnosed after the fact from
the log alone. Fixed via `_log_openocd_result()`/`_openocd_error_message()`/
`_decisive_openocd_line()` in mcp_server_debug.py:
  - on failure, the full OpenOCD transcript is persisted to the session log
    (truncated), and the tool's own returned `error` string leads with the
    single decisive OpenOCD line rather than a generic message.
  - on success, the session log gets one short line, never the transcript.

All against a mocked debug_probe and a mocked `_srv._session_log` -- no real
OpenOCD session, no live board.

Run with: python -m pytest tools/PcTools/tests/test_debug_probe_openocd_logging.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_debug as md  # noqa: E402
from kilnctrl import debug_probe  # noqa: E402


class DecisiveLineTest(unittest.TestCase):
    def test_picks_error_marker_line_over_noise(self):
        output = "Info : some noise\nError: flash write failed\nmore noise"
        self.assertEqual(md._decisive_openocd_line(output), "Error: flash write failed")

    def test_falls_back_to_last_line_when_no_marker(self):
        output = "line one\nline two\nline three"
        self.assertEqual(md._decisive_openocd_line(output), "line three")

    def test_never_empty_on_blank_input(self):
        self.assertTrue(md._decisive_openocd_line(""))
        self.assertTrue(md._decisive_openocd_line(None))


class OpenocdResultLoggingTest(unittest.TestCase):
    def test_success_logs_short_line_only(self):
        with unittest.mock.patch.object(md._srv, "_session_log") as log_mock:
            md._log_openocd_result("debug_reset(peer=pico, mode=run)", True, "lots of routine OpenOCD chatter\n" * 50)
        log_mock.warning.assert_called_once()
        args = log_mock.warning.call_args[0]
        # The formatted message must stay short -- it must NOT embed the
        # (potentially huge) routine output on a successful call.
        self.assertNotIn("chatter", args[0] % args[1:] if len(args) > 1 else args[0])

    def test_failure_persists_full_output_truncated(self):
        big_output = "X" * (md._OPENOCD_LOG_TRUNCATE_BYTES + 5000) + "\nError: verify failed at the end"
        with unittest.mock.patch.object(md._srv, "_session_log") as log_mock:
            md._log_openocd_result("debug_reset(peer=pico, mode=run)", False, big_output)
        log_mock.warning.assert_called_once()
        args = log_mock.warning.call_args[0]
        logged_text = args[-1]
        # Truncated, but the tail (with the decisive content) survives.
        self.assertLessEqual(len(logged_text), md._OPENOCD_LOG_TRUNCATE_BYTES + 200)
        self.assertIn("Error: verify failed at the end", logged_text)

    def test_failure_message_leads_with_decisive_line(self):
        output = "Info: init\nError: Target not examined yet\nmore trailing context"
        msg = md._openocd_error_message("reset failed for pico", output)
        self.assertTrue(msg.startswith("error: reset failed for pico: Error: Target not examined yet"))
        self.assertIn("more trailing context", msg)


class DebugResetLoggingIntegrationTest(unittest.TestCase):
    """Proves the wiring end-to-end through the actual debug_reset() tool,
    not just the helper in isolation -- a negative test that would fail if
    debug_reset() stopped calling the logging helper."""

    def test_failed_reset_logs_full_output_and_surfaces_decisive_line(self):
        fake_output = "Info: examine\nError: Could not find MEM-AP to control the core\ntail line"
        with unittest.mock.patch.object(debug_probe, "reset", return_value=(False, fake_output)), \
                unittest.mock.patch.object(md.reset_probe, "append_history", return_value=None), \
                unittest.mock.patch.object(md._srv, "_session_log") as log_mock:
            result = md.debug_reset(peer="pico", mode="run")
        # The tool's own return leads with the decisive line.
        self.assertIn("Error: Could not find MEM-AP to control the core", result)
        # The session log received the full transcript, not just "ok=False".
        log_mock.warning.assert_called_once()
        logged_text = log_mock.warning.call_args[0][-1]
        self.assertIn("Could not find MEM-AP to control the core", logged_text)
        self.assertIn("tail line", logged_text)

    def test_successful_reset_does_not_dump_body_to_log(self):
        fake_output = "Info: examine\n" + ("Info: routine step\n" * 100)
        with unittest.mock.patch.object(debug_probe, "reset", return_value=(True, fake_output)), \
                unittest.mock.patch.object(md.reset_probe, "append_history", return_value=None), \
                unittest.mock.patch.object(md._srv, "_session_log") as log_mock:
            result = md.debug_reset(peer="pico", mode="run")
        self.assertIn("OK", result)
        log_mock.warning.assert_called_once()
        args = log_mock.warning.call_args[0]
        formatted = args[0] % args[1:] if len(args) > 1 else args[0]
        self.assertNotIn("routine step", formatted)


if __name__ == "__main__":
    unittest.main()
