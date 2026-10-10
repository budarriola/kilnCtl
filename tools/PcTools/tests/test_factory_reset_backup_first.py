"""Backup-first rule for every PcTools FACTORY_RESET path, and the MCP call log.

docs/audits/KILN_NVS_LOSS_2026-10-09.md. Fake board only: no network, no serial.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
import unittest.mock as mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import actions, config_presets, factory_reset_guard, mcp_server  # noqa: E402
from kilnctrl.control import ControlQueryError  # noqa: E402
from mcpkit import call_log  # noqa: E402


def _good_export(host):
    return '{"kind":"kilnctl_backup","version":1}', {"kind": "kilnctl_backup", "version": 1}


def _bad_export(host):
    raise OSError("unreachable")


class GuardTests(unittest.TestCase):
    def test_saves_file_and_reports_path(self):
        with tempfile.TemporaryDirectory() as d:
            ok, text = factory_reset_guard.backup_before_reset("h", exporter=_good_export, out_dir=d)
            self.assertTrue(ok, text)
            self.assertTrue(os.path.isfile(text))
            self.assertIn("kilnctl_backup", open(text, encoding="utf-8").read())

    def test_failure_refuses(self):
        ok, text = factory_reset_guard.backup_before_reset("h", exporter=_bad_export)
        self.assertFalse(ok)
        self.assertIn("refused", text)

    def test_skip_needs_exact_true(self):
        ok, _ = factory_reset_guard.backup_before_reset("h", True, exporter=_bad_export)
        self.assertTrue(ok)
        ok, _ = factory_reset_guard.backup_before_reset("h", 1, exporter=_bad_export)
        self.assertFalse(ok)


class ActionTests(unittest.TestCase):
    def _run(self, exporter, **kw):
        sent = []
        with tempfile.TemporaryDirectory() as d, \
             mock.patch.object(factory_reset_guard, "BACKUP_DIR", d), \
             mock.patch("kilnctrl.backup_export_http_client.get_export", exporter), \
             mock.patch.object(actions, "_send", lambda ctx, task, payload: sent.append(payload) or "ok sent"):
            result = actions.ACTIONS["System: Factory Reset"].run(
                mock.Mock(), scope=1, confirm=True, host="h", **kw)
        return result, sent

    def test_backup_failure_blocks_send(self):
        result, sent = self._run(_bad_export)
        self.assertIn("refused", result)
        self.assertEqual(sent, [])

    def test_backup_ok_sends_and_reports_path(self):
        result, sent = self._run(_good_export)
        self.assertEqual(len(sent), 1)
        self.assertIn("backup:", result)
        self.assertIn("kilnctl_backup_prereset_", result)

    def test_skip_backup_sends_without_export(self):
        result, sent = self._run(_bad_export, skip_backup=True)
        self.assertEqual(len(sent), 1)
        self.assertIn("SKIPPED", result)


class PresetToolTests(unittest.TestCase):
    def _call(self, exporter, apply_exc=None, **kw):
        link = mock.Mock()
        link.send.return_value = mock.Mock(ok=True)
        info = mock.Mock()
        info.wait_for_boot_push.return_value = object()
        with tempfile.TemporaryDirectory() as d, \
             mock.patch.object(factory_reset_guard, "BACKUP_DIR", d), \
             mock.patch("kilnctrl.backup_export_http_client.get_export", exporter), \
             mock.patch.object(config_presets, "load_preset_data", return_value={}), \
             mock.patch.object(config_presets, "apply_preset", side_effect=apply_exc), \
             mock.patch("kilnctrl.mcp_server_ota._ota_resolve_host", return_value="h"), \
             mock.patch.object(mcp_server._srv._link if hasattr(mcp_server, "_srv") else mcp_server._link, "send", link.send), \
             mock.patch.object(mcp_server._info, "arm_boot_push"), \
             mock.patch.object(mcp_server._info, "wait_for_boot_push", info.wait_for_boot_push):
            result = mcp_server.factory_default_then_load_preset("p", confirm=True, **kw)
        return result, link

    def test_backup_failure_refuses_before_send(self):
        result, link = self._call(_bad_export)
        self.assertIn("refused", result)
        link.send.assert_not_called()

    def test_apply_failure_says_wiped_and_names_backup(self):
        result, link = self._call(_good_export, apply_exc=ControlQueryError("boom"))
        link.send.assert_called_once()
        self.assertIn("BOARD WIPED", result)
        self.assertIn("kilnctl_backup_prereset_", result)


class CallLogTests(unittest.TestCase):
    def test_line_has_keys_not_values(self):
        with tempfile.TemporaryDirectory() as d:
            with mock.patch.object(call_log, "log_dir", d), mock.patch.object(call_log, "_port", 8767):
                call_log.log_call("some_tool", {"password": "hunter2", "confirm": True})
            files = os.listdir(d)
            self.assertEqual(len(files), 1)
            self.assertTrue(files[0].startswith("8767_"))
            text = open(os.path.join(d, files[0]), encoding="utf-8").read()
            self.assertIn("some_tool confirm,password", text)
            self.assertNotIn("hunter2", text)

    def test_registry_entries_are_wrapped_at_import(self):
        # call_log.install() ran in mcp_server: every kiln_call/kiln_batch target logs.
        entry = mcp_server.registry.by_name["backup_export"]
        self.assertTrue(hasattr(entry.fn, "__wrapped__"))

    def test_wrapper_logs_and_passes_through(self):
        with tempfile.TemporaryDirectory() as d, mock.patch.object(call_log, "log_dir", d):
            wrapped = call_log.wrap_direct("t", lambda **kw: "r")
            self.assertEqual(wrapped(a=1), "r")
            self.assertIn("t a", open(os.path.join(d, os.listdir(d)[0]), encoding="utf-8").read())


if __name__ == "__main__":
    unittest.main()
