#!/usr/bin/env python3
"""Unit tests for mcp_server_info.estop_verify() -- the MCP tool that wraps
GET /api/readiness + POST /api/estop/verify. All against mocked
readiness_http_client/estop_verify_http_client calls; no real socket, no
live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_estop_verify.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_info as msi  # noqa: E402
from kilnctrl import readiness_http_client  # noqa: E402
from kilnctrl import estop_verify_http_client  # noqa: E402


def _readiness(estop_status="not_done", trip_status="ok", include_trip_item=True):
    items = []
    if include_trip_item:
        items.append({"key": "safety_trip", "label": "Safety processor trip status",
                       "status": trip_status, "detail": "no trip", "fix_url": ""})
    items.append({"key": "estop_verified", "label": "E-stop interlock verified",
                  "status": estop_status, "detail": "not yet verified", "fix_url": ""})
    return {"items": items}


class _Base(unittest.TestCase):
    def _resolve_host_patch(self):
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")


class RefusalTest(_Base):
    def test_refuses_without_confirm(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness",
                                         return_value=_readiness()) as get_mock, \
             unittest.mock.patch.object(estop_verify_http_client, "post_estop_verify") as post_mock:
            result = msi.estop_verify(confirm=False)
        self.assertIn("DRY RUN", result)
        get_mock.assert_called_once()
        post_mock.assert_not_called()

    def test_refuses_on_latched_trip_even_with_confirm(self):
        """A latched safety_trip must refuse regardless of confirm -- an
        E-stop verification is never a way to paper over an active trip."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness",
                                         return_value=_readiness(trip_status="not_done")), \
             unittest.mock.patch.object(estop_verify_http_client, "post_estop_verify") as post_mock:
            result = msi.estop_verify(confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("safety_trip", result)
        post_mock.assert_not_called()

    def test_refuses_when_safety_trip_item_is_absent(self):
        """readiness_http.c can drop items when its buffer fills -- a
        missing safety_trip item must refuse, never be treated as an
        implicit ok."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness",
                                         return_value=_readiness(include_trip_item=False)), \
             unittest.mock.patch.object(estop_verify_http_client, "post_estop_verify") as post_mock:
            result = msi.estop_verify(confirm=True)
        self.assertIn("refused", result.lower())
        self.assertIn("safety_trip", result)
        post_mock.assert_not_called()

    def test_confirm_not_exactly_true_is_refused(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness",
                                         return_value=_readiness()), \
             unittest.mock.patch.object(estop_verify_http_client, "post_estop_verify") as post_mock:
            result = msi.estop_verify(confirm=1)  # truthy, but not True
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()

    def test_unreadable_readiness_errors_before_any_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(
                 readiness_http_client, "get_readiness",
                 side_effect=readiness_http_client.ReadinessHttpError("unreachable")), \
             unittest.mock.patch.object(estop_verify_http_client, "post_estop_verify") as post_mock:
            result = msi.estop_verify(confirm=True)
        self.assertIn("error", result.lower())
        post_mock.assert_not_called()


class ConfirmedVerifyTest(_Base):
    def test_confirmed_verify_posts_and_confirms_read_back(self):
        get_calls = [_readiness(estop_status="not_done"), _readiness(estop_status="ok")]

        def fake_get(host):
            return get_calls.pop(0)

        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", side_effect=fake_get), \
             unittest.mock.patch.object(estop_verify_http_client, "post_estop_verify",
                                         return_value={"ok": True}) as post_mock:
            result = msi.estop_verify(confirm=True)
        post_mock.assert_called_once_with("10.0.0.5")
        self.assertIn("ok - recorded and confirmed", result)
        # The printed state must be the AFTER read-back (status=ok), not the
        # stale BEFORE state (status=not_done) -- a success line that shows
        # not_done would contradict itself.
        self.assertIn("status='ok'", result)
        self.assertIn("before: estop_verified status='not_done'", result)

    def test_verify_that_does_not_clear_fails_loud(self):
        """The board can answer {"ok":true} to the POST and STILL read back
        estop_verified not-ok on the immediate re-fetch -- this must never
        be reported as success (same class as boot_guard's write-lies bug
        per CLAUDE.md)."""
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness",
                                         return_value=_readiness(estop_status="not_done")), \
             unittest.mock.patch.object(estop_verify_http_client, "post_estop_verify",
                                         return_value={"ok": True}):
            result = msi.estop_verify(confirm=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok - recorded", result)
        # The re-fetched (after) state, still not_done, must be what is
        # printed as the primary status.
        self.assertIn("status='not_done'", result)

    def test_post_failure_is_reported(self):
        err = estop_verify_http_client.EstopVerifyHttpError("refused", status=500, detail="failed")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness",
                                         return_value=_readiness()), \
             unittest.mock.patch.object(estop_verify_http_client, "post_estop_verify",
                                         side_effect=err):
            result = msi.estop_verify(confirm=True)
        self.assertIn("failed", result.lower())
        self.assertNotIn("ok - recorded", result)


if __name__ == "__main__":
    unittest.main()
