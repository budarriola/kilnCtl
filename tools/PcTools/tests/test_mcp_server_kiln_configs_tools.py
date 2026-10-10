#!/usr/bin/env python3
"""Tool-level tests for kiln_configs_quarantine_clear and kiln_config_apply (the HTTP
clients had unit tests; the MCP tools themselves, with their confirm gate and
read-back / terminal-state verification, did not). Fake HTTP clients only.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_kiln_configs_tools.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock as um

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import kiln_configs_apply_http_client as ac  # noqa: E402
from kilnctrl import kiln_configs_quarantine_http_client as qc  # noqa: E402
from kilnctrl import mcp_server_info as info  # noqa: E402
from kilnctrl import mcp_server_ota  # noqa: E402


class _Base(unittest.TestCase):
    def setUp(self):
        p = um.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")
        p.start()
        self.addCleanup(p.stop)


class QuarantineClearTests(_Base):
    def _patch(self, statuses, post=None):
        """statuses: successive get_quarantine_status results."""
        seq = list(statuses)
        post_mock = um.Mock(return_value=post if post is not None else {"ok": True})
        patches = [
            um.patch.object(qc, "get_kiln_configs_list", return_value={"configs": []}),
            um.patch.object(qc, "get_quarantine_status", side_effect=lambda h: seq.pop(0)),
            um.patch.object(qc, "post_quarantine_clear", post_mock),
        ]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)
        return post_mock

    def test_dry_run_and_truthy_non_true_never_post(self):
        for bad in (False, "yes", 1, "true"):
            post = self._patch([(True, "wrong size")])
            out = info.kiln_configs_quarantine_clear(confirm=bad)
            self.assertIn("DRY RUN", out, bad)
            post.assert_not_called()
            um.patch.stopall()

    def test_not_quarantined_does_nothing_even_with_confirm(self):
        post = self._patch([(False, "fine")])
        out = info.kiln_configs_quarantine_clear(confirm=True)
        self.assertIn("not quarantined", out)
        post.assert_not_called()

    def test_success_requires_reprobe_not_quarantined(self):
        post = self._patch([(True, "wrong size"), (False, "fine")])
        out = info.kiln_configs_quarantine_clear(confirm=True)
        post.assert_called_once()
        self.assertTrue(out.startswith("ok - quarantine cleared and confirmed by read-back"), out)

    def test_still_quarantined_after_post_fails_loud(self):
        self._patch([(True, "wrong size"), (True, "still wrong size")], post={"ok": True})
        out = info.kiln_configs_quarantine_clear(confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("Do not trust", out)

    def test_reprobe_failure_is_unknown_not_ok(self):
        post = um.Mock(return_value={"ok": True})
        calls = {"n": 0}

        def status(h):
            calls["n"] += 1
            if calls["n"] == 1:
                return (True, "wrong size")
            raise qc.KilnConfigsQuarantineHttpError("down")
        with um.patch.object(qc, "get_kiln_configs_list", return_value={"configs": []}), \
                um.patch.object(qc, "get_quarantine_status", side_effect=status), \
                um.patch.object(qc, "post_quarantine_clear", post):
            out = info.kiln_configs_quarantine_clear(confirm=True)
        self.assertNotIn("ok - ", out)
        self.assertIn("UNKNOWN", out)


class ConfigApplyTests(_Base):
    def test_unconfirmed_and_truthy_values_send_nothing(self):
        for bad in (False, "yes", 1, "true", [1]):
            with um.patch.object(ac, "post_apply") as post:
                out = info.kiln_config_apply(3, confirm=bad)
            self.assertIn("DRY RUN", out, bad)
            post.assert_not_called()

    def _apply(self, post=(202, "running"), final=None, poll_exc=None, id=3):
        with um.patch.object(ac, "post_apply", return_value=post), \
                um.patch.object(ac, "poll_apply_status",
                                side_effect=poll_exc, return_value=final):
            return info.kiln_config_apply(id, confirm=True)

    def test_done_ok_is_ok(self):
        out = self._apply(final={"state": "done_ok", "diverged": False, "id": 3})
        self.assertTrue(out.startswith("ok - applied id=3"), out)

    def test_diverged_fails_loud_even_if_state_done_ok(self):
        out = self._apply(final={"state": "done_ok", "diverged": True, "reason": "x", "id": 3})
        self.assertTrue(out.startswith("FAILED"), out)

    def test_done_failed_and_nonterminal_are_not_ok(self):
        self.assertTrue(self._apply(final={"state": "done_failed", "reason": "r"}).startswith("FAILED"))
        out = self._apply(final={"state": "running"})
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_other_id_in_status_is_unconfirmed_not_ok(self):
        out = self._apply(final={"state": "done_ok", "diverged": False, "id": 9})
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_poll_failure_is_unknown(self):
        out = self._apply(poll_exc=ac.KilnConfigsApplyHttpError("down"))
        self.assertIn("outcome UNKNOWN", out)
        self.assertNotIn("ok - ", out)

    def test_mid_run_409_is_reported_as_refusal_without_polling(self):
        with um.patch.object(ac, "post_apply", return_value=(409, "system_mode_gate: running")), \
                um.patch.object(ac, "poll_apply_status") as poll, \
                um.patch("kilnctrl.zones_http_client.is_system_mode_gate_refusal", return_value=True):
            out = info.kiln_config_apply(3, confirm=True)
        self.assertTrue(out.startswith("refused"), out)
        poll.assert_not_called()

    def test_428_hardware_differs_not_auto_acked(self):
        with um.patch.object(ac, "post_apply", return_value=(428, "differs")) as post, \
                um.patch.object(ac, "is_hardware_differs_body", return_value=True):
            out = info.kiln_config_apply(3, confirm=True)
        self.assertTrue(out.startswith("refused (428, hardware differs)"), out)
        self.assertFalse(post.call_args.kwargs.get("ack_hardware_differs"))


if __name__ == "__main__":
    unittest.main()
