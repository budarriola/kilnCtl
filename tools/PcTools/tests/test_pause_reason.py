"""pause_reason text map + profiles_get_exec_status surfacing it (HTTP read mocked)."""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server, run_queue  # noqa: E402
from kilnctrl.devices_profiles import ProfileExecStatus  # noqa: E402
from kilnctrl.pause_reason import pause_reason_text  # noqa: E402


def _status(state):
    return ProfileExecStatus(
        state=state, profile_id=3, name="Bisque", zone_mask=1, segment_index=1, segment_count=4,
        dwelling=False, target_c=1000.0, segment_elapsed_s=60, dwell_remaining_s=0,
        ramp_lock_held=False, ramp_lock_lagging_mask=0, fault_guard=0, zones=[],
    )


class PauseReasonTextTests(unittest.TestCase):
    def test_known_and_unknown(self):
        self.assertEqual(pause_reason_text(""), "")
        self.assertEqual(pause_reason_text(None), "")
        self.assertIn("Heat withheld", pause_reason_text("pico_reboot_undecided"))
        self.assertIn("fatal fault", pause_reason_text("pico_fatal_reboot"))
        self.assertIn("heat grant", pause_reason_text("heat_grant_unconfirmed"))
        self.assertEqual(pause_reason_text("brand_new"), "Paused: brand_new")


class ProfilesGetExecStatusPauseReasonTests(unittest.TestCase):
    def _run(self, state, http):
        with unittest.mock.patch.object(mcp_server._profiles, "get_exec_status", return_value=_status(state)), \
             unittest.mock.patch.object(run_queue, "get_exec", **http) as g:
            return mcp_server.profiles_get_exec_status(host="10.0.0.5"), g

    def test_running_shows_reason(self):
        out, g = self._run(1, {"return_value": {"pause_reason": "pico_reboot_undecided"}})
        self.assertIn("pause_reason: pico_reboot_undecided -- Heat withheld", out)
        g.assert_called_once()

    def test_http_failure_is_reported_not_raised(self):
        out, _ = self._run(2, {"side_effect": OSError("down")})
        self.assertIn("pause_reason: unavailable", out)

    def test_idle_makes_no_http_call(self):
        out, g = self._run(0, {"return_value": {}})
        self.assertNotIn("pause_reason", out)
        g.assert_not_called()


if __name__ == "__main__":
    unittest.main()
