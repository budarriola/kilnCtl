"""profile_live_* writes read the live status back."""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock as um

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_profile_live as pl  # noqa: E402

H = pl.profile_live_http


class ReadbackTests(unittest.TestCase):
    def _hostpatch(self):
        return um.patch.object(pl, "_profile_live_resolve_host", return_value="h")

    def test_fork_ok_and_failed(self):
        with self._hostpatch(), um.patch.object(H, "fork_live", return_value={"ok": True}):
            with um.patch.object(H, "get_live_status", return_value={"active": True, "working_id": 4}):
                self.assertIn("read-back OK", pl.profile_live_fork(confirm=True))
            with um.patch.object(H, "get_live_status", return_value={"active": False, "working_id": -1}):
                self.assertIn("FAILED read-back", pl.profile_live_fork(confirm=True))

    def test_edit_name_mismatch(self):
        with self._hostpatch(), um.patch.object(H, "edit_live", return_value={"ok": True}), \
                um.patch.object(H, "get_live_status", return_value={}), \
                um.patch.object(H, "get_live_content", return_value={"name": "other"}):
            self.assertIn("FAILED read-back", pl.profile_live_edit("n", 1, [], confirm=True))

    def test_decide_pending_remains(self):
        with self._hostpatch(), um.patch.object(H, "decide_live_discard", return_value={"ok": True}):
            with um.patch.object(H, "get_live_status", return_value={"pending_decision": False, "working_id": 4}):
                self.assertIn("FAILED read-back", pl.profile_live_decide("discard", confirm=True))
            with um.patch.object(H, "get_live_status", return_value={"pending_decision": False}):
                self.assertIn("FAILED read-back", pl.profile_live_decide("discard", confirm=True))
            with um.patch.object(H, "get_live_status", return_value={"pending_decision": False, "working_id": -1}):
                self.assertIn("read-back OK", pl.profile_live_decide("discard", confirm=True))
            with um.patch.object(H, "get_live_status", return_value={"pending_decision": True}):
                self.assertIn("FAILED read-back", pl.profile_live_decide("discard", confirm=True))
            with um.patch.object(H, "get_live_status", side_effect=OSError("x")):
                self.assertIn("UNVERIFIED", pl.profile_live_decide("discard", confirm=True))


if __name__ == "__main__":
    unittest.main()
