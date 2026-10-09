#!/usr/bin/env python3
"""Regression test for the Firing Status control-mode display.

gui_wifi_firing.py:_apply_firing_profile() renders each zone's
control_mode int through a small hand-maintained dict. That dict must
stay in sync with zone_control_mode_t
(firmware/KilnFW/App/drivers/http/zones_http.h) -- it drifted once already
(mode 3, PID_FUZZY, rendered as "?" for every zone actually running
fuzzy control). This test pins all four known values to distinct
non-"?" strings and pins the "?" fallback for anything outside that
range, so a future enum addition that isn't mirrored here fails loudly
instead of silently showing "?" to an operator watching a live firing.
"""

from kilnctrl.gui_wifi_firing import WifiFiringMixin


class _StubFiringView:
    """Minimal stand-in for the Tk mixin host: just enough state for
    _apply_firing_profile to run without a real Tk root or StringVar."""

    def __init__(self) -> None:
        self.firing_profile_var = self
        self._value = None

    # stand in for tk.StringVar.set
    def set(self, value: str) -> None:
        self._value = value

    def _firing_status_is_open(self) -> bool:
        return True


def _render_modes(*control_modes: int) -> str:
    stub = _StubFiringView()
    data = {
        "state": "running",
        "zone_mask": 0x1,
        "profile_name": "test",
        "profile_id": 1,
        "segment_index": 0,
        "segment_count": 1,
        "dwelling": False,
        "target_c": 100.0,
        "zones": [
            {
                "zone": i,
                "actual_c": 50.0,
                "actual_valid": True,
                "control_mode": mode,
                "relay_on": False,
                "faulted": False,
                "duty": 0.0,
            }
            for i, mode in enumerate(control_modes)
        ],
    }
    WifiFiringMixin._apply_firing_profile(stub, data)
    return stub._value


def test_all_four_control_modes_render_distinctly():
    # zone_control_mode_t: OFF=0, BANGBANG=1, PID=2, PID_FUZZY=3
    rendered = _render_modes(0, 1, 2, 3)
    lines = [ln for ln in rendered.splitlines() if ln.strip().startswith("Zone")]
    assert len(lines) == 4
    assert "?" not in "".join(lines), rendered
    # every rendered mode token must be distinct -- no two enum values
    # may collapse onto the same (or a "?") label
    tokens = []
    for line in lines:
        # "  Zone N: 50.0C   <MODE>   off   duty=0.00"
        parts = line.split()
        tokens.append(parts[3])
    assert len(set(tokens)) == 4, rendered
    assert "PID_FUZZY" in rendered


def test_unknown_control_mode_still_falls_back_to_unknown_marker():
    rendered = _render_modes(99)
    lines = [ln for ln in rendered.splitlines() if ln.strip().startswith("Zone")]
    assert len(lines) == 1
    parts = lines[0].split()
    assert parts[3] == "?", rendered
