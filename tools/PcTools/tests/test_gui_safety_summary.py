"""Status-bar safety summary (gui.py's compact column, fed by gui_safety.py).

Drives the real SafetyMixin._apply_safety_status / _on_safety_query_error
against crafted SafetyStatus values -- no mocking of the function under test,
only of the tkinter Var/Label surface it writes to and the popup-open check
it also touches.
"""
import tkinter as tk

import pytest

from kilnctrl.devices_safety import SafetyStatus
from kilnctrl.gui_safety import SafetyMixin, _BAD_COLOR, _EXPECTED_COLOR, _OK_COLOR
from kilnctrl.protocol import SAFETY_AGE_NEVER, SafetyFlag, ThermoFault


class _FakeLabel:
    def __init__(self):
        self.color = None

    def config(self, foreground=None):
        if foreground is not None:
            self.color = foreground


class _Harness(SafetyMixin):
    """Just enough of KilnCtrlApp for _apply_safety_status/_on_safety_query_error
    to run: the two widgets they write to, and a closed "safety" popup so the
    detail-row code path (which needs real Tk widgets) is skipped."""

    def __init__(self, root: tk.Tk):
        self.safety_summary_var = tk.StringVar(value="Safety: --")
        self.safety_summary_label = _FakeLabel()
        self.popups = {}

    def _is_open(self, key: str) -> bool:
        return False


@pytest.fixture
def harness():
    try:
        root = tk.Tk()
    except tk.TclError:
        pytest.skip("no display available for Tk")
    root.withdraw()
    try:
        yield _Harness(root)
    finally:
        root.destroy()


def _status(
    *,
    link_up=True,
    fault=False,
    estop=False,
    relay=False,
    enabled=False,
    temp_valid=True,
    age_ms=100,
) -> SafetyStatus:
    flags = SafetyFlag(0)
    if link_up:
        flags |= SafetyFlag.LINK_UP
    if fault:
        flags |= SafetyFlag.FAULT
    if estop:
        flags |= SafetyFlag.ESTOP
    if relay:
        flags |= SafetyFlag.RELAY
    if enabled:
        flags |= SafetyFlag.ENABLED
    if temp_valid:
        flags |= SafetyFlag.TEMP_VALID
    return SafetyStatus(
        flags=flags,
        temperature_c=25.0,
        cold_junction_c=24.0,
        fault_status=ThermoFault(0),
        current_a=(0.0, 0.0, 0.0),
        age_ms=age_ms,
        tx_dropped_sat=None,
    )


def test_summary_ok_when_clean(harness):
    harness._apply_safety_status(_status())
    assert harness.safety_summary_var.get() == "Safety: OK"
    assert harness.safety_summary_label.color == _OK_COLOR


def test_summary_stale_before_ok(harness):
    # item 3: age_ms thresholded, shown before the OK branch is reached.
    harness._apply_safety_status(_status(age_ms=999_999))
    text = harness.safety_summary_var.get()
    assert text.startswith("Safety: stale"), text
    assert harness.safety_summary_label.color == _EXPECTED_COLOR


def test_summary_not_stale_when_never_received(harness):
    # link down + SAFETY_AGE_NEVER must read as the expected "no Pico FW yet"
    # state, not get relabeled "stale" by the new threshold check.
    harness._apply_safety_status(_status(link_up=False, age_ms=SAFETY_AGE_NEVER))
    assert "link down" in harness.safety_summary_var.get()


def test_summary_fault_asserted_overrides_clean_tc(harness):
    # item 5: the Pico's own fault output must not read OK just because this
    # side's thermocouple happens to be fine.
    harness._apply_safety_status(_status(fault=True))
    assert "FAULT" in harness.safety_summary_var.get()
    assert harness.safety_summary_label.color == _BAD_COLOR


def test_summary_estop_overrides_fault(harness):
    harness._apply_safety_status(_status(fault=True, estop=True))
    assert "E-STOP" in harness.safety_summary_var.get()
    assert harness.safety_summary_label.color == _BAD_COLOR


def test_query_error_replaces_stale_good_text(harness):
    # item 4: a failed poll must not leave the last good summary standing.
    harness._apply_safety_status(_status())
    assert harness.safety_summary_var.get() == "Safety: OK"

    harness._on_safety_query_error(RuntimeError("no reply"))
    assert harness.safety_summary_var.get() == "Safety: no reply"
    assert harness.safety_summary_label.color == _BAD_COLOR
