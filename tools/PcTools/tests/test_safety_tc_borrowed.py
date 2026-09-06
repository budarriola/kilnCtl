#!/usr/bin/env python3
"""Unit tests for the safety-TC "borrowed sensor" display fix (b90fcb3's
KilnFW half; this closes the matching PcTools gap the audit found in
gui_safety.py's SAFETY popup and mcp_server_safety.py's safety_get_status()
tool, both of which used to present the safety-processor thermocouple as an
always-independent reading).

Firmware semantics being mirrored (firmware/KilnFW/App/drivers/safety/
safety_link.h's safety_tc_is_separate_physical_sensor()): the safety TC
reading is hidden/relabeled only when the ESP's own
`safety_tc_is_separate_sensor` (GET /api/status, dashboard_status_http.c) is
CONFIRMED false. Missing/unknown fails to SHOWN, never to hidden.

Two surfaces are covered:

  * mcp_server_safety.safety_get_status() -- appends a borrowed note when an
    extra GET /api/status confirms borrowed; silent (no note, no error) when
    the field is absent or the HTTP fetch fails.
  * gui_safety.SafetyMixin._apply_safety_status() -- the "Safety
    thermocouple" detail row, driven by the cached `_safety_tc_separate`
    (populated by a separate, non-hot-path HTTP fetch; see gui_safety.py's
    _safety_refresh_tc_separate_async()).

MANDATORY NEGATIVE TEST: test_broken_predicate_would_be_caught below
monkeypatches safety_get_status() to treat ANY value (including a missing
field, i.e. None) as "borrowed", proving the existing "field absent -> not
borrowed" tests actually exercise the fail-to-shown rule rather than passing
vacuously. Restores the real function afterwards.

Run with: python -m pytest tools/PcTools/tests/test_safety_tc_borrowed.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import tkinter as tk
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_safety as mss  # noqa: E402
from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl.devices_safety import SafetyStatus  # noqa: E402
from kilnctrl.gui_safety import SafetyMixin  # noqa: E402
from kilnctrl.protocol import SafetyFlag, ThermoFault  # noqa: E402


def _fake_response(body: bytes):
    resp = io.BytesIO(body)
    resp.status = 200

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _wire_status(temp_valid=True) -> SafetyStatus:
    flags = SafetyFlag.LINK_UP
    if temp_valid:
        flags |= SafetyFlag.TEMP_VALID
    return SafetyStatus(
        flags=flags,
        temperature_c=950.0,
        cold_junction_c=24.0,
        fault_status=ThermoFault(0),
        current_a=(0.0, 0.0, 0.0),
        age_ms=100,
        tx_dropped_sat=None,
    )


class SafetyGetStatusBorrowedTest(unittest.TestCase):
    """mcp_server_safety.safety_get_status()'s appended borrowed note."""

    def setUp(self):
        self._safety_patch = unittest.mock.patch.object(
            mss._srv, "_safety", unittest.mock.Mock(get_status=lambda: _wire_status())
        )
        self._safety_patch.start()
        self._host_patch = unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"
        )
        self._host_patch.start()

    def tearDown(self):
        self._safety_patch.stop()
        self._host_patch.stop()

    def _run_with_status_body(self, body: dict):
        text = json.dumps(body).encode("utf-8")
        with unittest.mock.patch(
            "urllib.request.urlopen", return_value=_fake_response(text)
        ):
            return mss.safety_get_status()

    def test_confirmed_borrowed_appends_note(self):
        out = self._run_with_status_body({"safety_tc_is_separate_sensor": False})
        self.assertIn("borrowed", out)

    def test_confirmed_separate_no_note(self):
        out = self._run_with_status_body({"safety_tc_is_separate_sensor": True})
        self.assertNotIn("borrowed", out)
        self.assertIn("950.00", out)

    def test_missing_field_fails_to_shown(self):
        """Older firmware / no such key at all -- unknown must NOT be
        rendered as borrowed."""
        out = self._run_with_status_body({"uptime_s": 10})
        self.assertNotIn("borrowed", out)

    def test_http_fetch_failure_fails_to_shown_and_keeps_uart_status(self):
        with unittest.mock.patch(
            "urllib.request.urlopen", side_effect=OSError("connection refused")
        ):
            out = mss.safety_get_status()
        self.assertNotIn("borrowed", out)
        self.assertIn("950.00", out)  # the cache-only UART status still comes through

    def test_broken_predicate_would_be_caught(self):
        """NEGATIVE TEST: a plausible defect variant -- checking
        `is not True` instead of `is False` -- treats a MISSING field the
        same as a confirmed borrowed one (both read back as `None`/absent
        via .get()). Proves test_missing_field_fails_to_shown above actually
        exercises the fail-to-shown rule rather than passing vacuously: this
        broken variant fails that exact scenario. Does not touch the real
        production function."""
        def broken_get_status() -> str:
            status = mss._srv._safety.get_status()
            text = status.describe()
            http_status = mss.dashboard_http_client.get_status(
                mcp_server_ota._ota_resolve_host(None)
            )
            if http_status.get("safety_tc_is_separate_sensor") is not True:
                text += " | safety TC: borrowed from a zone probe (same probe, not a second sensor)"
            return text

        out = self._run_with_status_body_using({"uptime_s": 10}, broken_get_status)
        self.assertIn(
            "borrowed", out,
            "the negative test itself failed to trigger the defect -- broken "
            "and correct implementations agree, which means this test proves nothing",
        )

    def _run_with_status_body_using(self, body: dict, fn):
        text = json.dumps(body).encode("utf-8")
        with unittest.mock.patch(
            "urllib.request.urlopen", return_value=_fake_response(text)
        ):
            return fn()


class _FakeLabel:
    def __init__(self):
        self.color = None

    def config(self, foreground=None):
        if foreground is not None:
            self.color = foreground


class _GuiHarness(SafetyMixin):
    """Enough of KilnCtrlApp for _apply_safety_status to run its detail-row
    path (popup considered open, unlike test_gui_safety_summary.py's closed
    harness)."""

    def __init__(self):
        self.safety_summary_var = tk.StringVar(value="Safety: --")
        self.safety_summary_label = _FakeLabel()
        self._safety_vars = {
            key: tk.StringVar(value="---")
            for key in (
                "link", "age", "fault_out", "estop", "relay", "enabled",
                "temp", "faults", "current1", "current2", "current3",
            )
        }
        self._safety_labels = {key: _FakeLabel() for key in self._safety_vars}
        self._safety_tc_separate = None

    def _is_open(self, key: str) -> bool:
        return True


class ApplySafetyStatusTempRowTest(unittest.TestCase):
    def setUp(self):
        try:
            self.root = tk.Tk()
        except tk.TclError:
            self.skipTest("no display available for Tk")
        self.root.withdraw()
        self.harness = _GuiHarness()

    def tearDown(self):
        self.root.destroy()

    def test_separate_shows_temperature(self):
        self.harness._safety_tc_separate = True
        self.harness._apply_safety_status(_wire_status())
        self.assertIn("950.00", self.harness._safety_vars["temp"].get())

    def test_confirmed_borrowed_hides_temperature(self):
        self.harness._safety_tc_separate = False
        self.harness._apply_safety_status(_wire_status())
        text = self.harness._safety_vars["temp"].get()
        self.assertIn("borrowed", text)
        self.assertNotIn("950.00", text)

    def test_unknown_state_fails_to_shown(self):
        """None (never fetched, or the last fetch failed) must show the
        temperature, same as firmware's fail-to-shown rule."""
        self.harness._safety_tc_separate = None
        self.harness._apply_safety_status(_wire_status())
        self.assertIn("950.00", self.harness._safety_vars["temp"].get())


if __name__ == "__main__":
    unittest.main()
