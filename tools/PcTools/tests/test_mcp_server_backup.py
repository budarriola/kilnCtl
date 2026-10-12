#!/usr/bin/env python3
"""Unit tests for mcp_server_info.backup_export()/backup_import() -- the
MCP tools wrapping GET /api/backup/export and POST /api/backup/import. All
against mocked backup_export_http_client/backup_import_http_client/
readiness_http_client calls; no real socket, no live board, no filesystem
writes outside a temp directory.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_backup.py -q
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_info as msi  # noqa: E402
from kilnctrl import backup_export_http_client  # noqa: E402
from kilnctrl import backup_import_http_client as bi  # noqa: E402
from kilnctrl import readiness_http_client  # noqa: E402


_GOOD_DOC = {
    "kind": "kilnctl_backup",
    "version": 3,
    "profiles": [{"name": "bisque"}, {"name": "glaze"}],
    "zones": [{"id": 0}],
    "kiln_configs": [],
}

_READINESS_OK = {"items": [{"key": "a", "status": "ok"}, {"key": "b", "status": "ok"}]}
_READINESS_NOT_DONE = {"items": [{"key": "a", "status": "ok"}, {"key": "b", "status": "not_done"}]}


class _Base(unittest.TestCase):
    def setUp(self):
        self._tmpdir = tempfile.mkdtemp(prefix="backup_mcp_test_")
        self.addCleanup(shutil.rmtree, self._tmpdir, ignore_errors=True)
        # Review confx HIGH-1: no test here may reach the board; individual tests patch over this.
        _p = unittest.mock.patch.object(backup_export_http_client, "get_export",
                                        side_effect=backup_export_http_client.BackupExportHttpError("no board in tests"))
        _p.start()
        self.addCleanup(_p.stop)

    def _resolve_host_patch(self):
        return unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")


class BackupExportTest(_Base):
    def test_writes_file_and_reports_summary(self):
        raw_text = json.dumps(_GOOD_DOC)
        out_path = os.path.join(self._tmpdir, "out.json")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(backup_export_http_client, "get_export",
                                         return_value=(raw_text, _GOOD_DOC)):
            result = msi.backup_export(out_path=out_path)
        with open(out_path, "r", encoding="utf-8") as f:
            self.assertEqual(json.loads(f.read()), _GOOD_DOC)
        self.assertIn("version=3", result)
        self.assertIn("profiles: 2 entries", result)
        self.assertIn("zones: 1 entries", result)
        self.assertIn("contains wifi/password-shaped fields: False", result)

    def test_default_out_path_under_logs_backup_export(self):
        raw_text = json.dumps(_GOOD_DOC)
        # The real default is anchored at the repo root, not the cwd.
        self.assertTrue(msi._BACKUP_EXPORT_DEFAULT_DIR.replace("\\", "/").endswith("/logs/backup_export"))
        self.assertTrue(os.path.isdir(os.path.join(
            os.path.dirname(os.path.dirname(msi._BACKUP_EXPORT_DEFAULT_DIR)), "tools", "PcTools")))
        fake_dir = os.path.join(self._tmpdir, "logs", "backup_export")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(msi, "_BACKUP_EXPORT_DEFAULT_DIR", fake_dir), \
             unittest.mock.patch.object(backup_export_http_client, "get_export",
                                         return_value=(raw_text, _GOOD_DOC)):
            result = msi.backup_export()
        self.assertTrue(os.path.isdir(fake_dir))
        self.assertEqual(len(os.listdir(fake_dir)), 1)
        self.assertIn("logs", result.replace("\\", "/"))

    def test_sensitive_field_reported_as_bool_never_value(self):
        doc = dict(_GOOD_DOC, wifi_ssid="MyHomeNetwork")
        raw_text = json.dumps(doc)
        out_path = os.path.join(self._tmpdir, "out.json")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(backup_export_http_client, "get_export",
                                         return_value=(raw_text, doc)):
            result = msi.backup_export(out_path=out_path)
        self.assertIn("contains wifi/password-shaped fields: True", result)
        self.assertNotIn("MyHomeNetwork", result)

    def test_fetch_failure_reports_error_not_a_file(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(
                 backup_export_http_client, "get_export",
                 side_effect=backup_export_http_client.BackupExportHttpError("unreachable")):
            result = msi.backup_export(out_path=os.path.join(self._tmpdir, "out.json"))
        self.assertIn("error", result.lower())
        self.assertFalse(os.path.exists(os.path.join(self._tmpdir, "out.json")))


class BackupImportConfirmGateTest(_Base):
    def test_refuses_without_confirm_true(self):
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(_GOOD_DOC))
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", return_value=_READINESS_OK), \
             unittest.mock.patch.object(bi, "post_import") as post_mock:
            result = msi.backup_import(path, confirm=False)
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()

    def test_truthy_non_true_confirm_still_refused(self):
        """confirm=1 (truthy but not exactly True) must not be treated as
        confirmed -- same exact-True gate convention as every other
        confirm-gated tool in this module."""
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(_GOOD_DOC))
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", return_value=_READINESS_OK), \
             unittest.mock.patch.object(bi, "post_import") as post_mock:
            result = msi.backup_import(path, confirm=1)
        self.assertIn("DRY RUN", result)
        post_mock.assert_not_called()

    def test_missing_file_errors_before_any_post(self):
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", return_value=_READINESS_OK), \
             unittest.mock.patch.object(bi, "post_import") as post_mock:
            result = msi.backup_import(os.path.join(self._tmpdir, "nope.json"), confirm=True)
        self.assertIn("error", result.lower())
        post_mock.assert_not_called()


class BackupImportSuccessTest(_Base):
    def test_confirmed_import_posts_and_reports_readiness_before_after(self):
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(_GOOD_DOC))
        readiness_calls = [_READINESS_NOT_DONE, _READINESS_OK]

        def fake_readiness(host):
            return readiness_calls.pop(0)

        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", side_effect=fake_readiness), \
             unittest.mock.patch.object(backup_export_http_client, "get_export",
                                        side_effect=[("", {"profiles": []}), ("", dict(_GOOD_DOC))]), \
             unittest.mock.patch.object(bi, "post_import", return_value=(200, '{"ok":true}')) as post_mock:
            result = msi.backup_import(path, confirm=True)
        post_mock.assert_called_once()
        self.assertIn("ok - restored", result)
        self.assertIn("readiness before", result)
        self.assertIn("readiness after", result)
        self.assertIn("1 not_done", result)  # before
        self.assertIn("2 ok", result)  # after

    def _import_with(self, readiness, export):
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(_GOOD_DOC))
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", side_effect=readiness), \
             unittest.mock.patch.object(backup_export_http_client, "get_export", side_effect=export), \
             unittest.mock.patch.object(bi, "post_import", return_value=(200, '{"ok":true}')):
            return msi.backup_import(path, confirm=True)

    def test_unreadable_readiness_after_is_unverified(self):
        def rd(host):
            if not hasattr(rd, "n"):
                rd.n = 1
                return _READINESS_OK
            raise readiness_http_client.ReadinessHttpError("down")
        out = self._import_with(rd, lambda h: ("", dict(_GOOD_DOC)))
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertNotIn("ok - restored", out)

    def test_zone_count_mismatch_is_unverified(self):
        bad = dict(_GOOD_DOC)
        bad["zones"] = []
        out = self._import_with(lambda h: _READINESS_OK, lambda h: ("", bad))
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertIn("zones", out)

    def test_reexport_failure_is_unverified(self):
        def boom(h):
            raise backup_export_http_client.BackupExportHttpError("boom")
        out = self._import_with(lambda h: _READINESS_OK, boom)
        self.assertTrue(out.startswith("UNVERIFIED"), out)

    def test_dry_run_reports_plan_and_does_not_claim_restored(self):
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(_GOOD_DOC))
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", return_value=_READINESS_OK), \
             unittest.mock.patch.object(bi, "post_import",
                                         return_value=(200, "would restore 2 profiles")) as post_mock:
            result = msi.backup_import(path, confirm=True, dry_run=True)
        args, kwargs = post_mock.call_args
        self.assertTrue(kwargs.get("dry_run") or (len(args) > 3 and args[3]))
        self.assertIn("dry run only, nothing written", result)
        self.assertIn("would restore 2 profiles", result)
        self.assertNotIn("ok - restored.", result)

    def test_reports_same_elapsed_time_for_post_and_full_job(self):
        """This route is synchronous -- there is no separate poll, so the
        result must not invent two different numbers for "the POST" and
        "the full job"."""
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(_GOOD_DOC))
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", return_value=_READINESS_OK), \
             unittest.mock.patch.object(backup_export_http_client, "get_export",
                                        side_effect=[("", {"profiles": []}), ("", dict(_GOOD_DOC))]), \
             unittest.mock.patch.object(bi, "post_import", return_value=(200, '{"ok":true}')):
            result = msi.backup_import(path, confirm=True)
        self.assertIn("synchronous", result)


class BackupImportRefusalTest(_Base):
    def _run(self, status, body):
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(_GOOD_DOC))
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", return_value=_READINESS_OK), \
             unittest.mock.patch.object(bi, "post_import", return_value=(status, body)):
            return msi.backup_import(path, confirm=True)

    def test_mode_gate_409_is_reported_distinctly(self):
        result = self._run(409, "refused -- a firing or autotune run is active")
        self.assertIn("mode_gate", result)
        self.assertIn("refused", result)

    def test_async_busy_409_is_reported_distinctly(self):
        result = self._run(409, bi.ASYNC_BUSY_MARKER)
        self.assertIn("async_busy", result)

    def test_ordinary_interlock_409_is_reported_distinctly(self):
        result = self._run(409, "heater is on")
        self.assertIn("interlock", result)
        self.assertNotIn("interlock_needs_ack", result)

    def test_428_needs_ack_is_reported_distinctly(self):
        result = self._run(428, "safety link is down")
        self.assertIn("interlock_needs_ack", result)

    def test_400_validation_is_reported_distinctly(self):
        result = self._run(400, "bad profile entry 2")
        self.assertIn("validation", result)
        self.assertIn("bad profile entry 2", result)

    def test_500_partial_write_fails_loud(self):
        result = self._run(500, "kiln_configs committed, profiles failed")
        self.assertIn("FAILED", result)
        self.assertIn("PARTIAL", result)
        self.assertNotIn("ok - restored", result)

    def test_500_bare_out_of_memory_is_not_a_partial_write(self):
        result = self._run(500, "out of memory")
        self.assertIn("out_of_memory", result)
        self.assertNotIn("PARTIAL", result)
        self.assertNotIn("ok - restored", result)

    def test_transport_failure_reports_error(self):
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(_GOOD_DOC))
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", return_value=_READINESS_OK), \
             unittest.mock.patch.object(
                 bi, "post_import",
                 side_effect=bi.BackupImportHttpError("unreachable")):
            result = msi.backup_import(path, confirm=True)
        self.assertIn("error", result.lower())


class BackupImportTransportWordingTest(_Base):
    def test_transport_failure_says_outcome_unknown_may_have_committed(self):
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(_GOOD_DOC))
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(readiness_http_client, "get_readiness", return_value=_READINESS_OK), \
             unittest.mock.patch.object(bi, "post_import", side_effect=bi.BackupImportHttpError("timed out")):
            result = msi.backup_import(path, confirm=True)
        self.assertIn("UNKNOWN", result)
        self.assertIn("MAY HAVE COMMITTED", result)




class StaleStoresNoteTest(_Base):
    """persfx3 LOW-4: stale_or_unknown_stores is surfaced by export and by an import of such a file."""

    def test_export_warns_when_stores_are_stale(self):
        doc = dict(_GOOD_DOC, stale_or_unknown_stores=["zones.json", "aux_outputs.bin"])
        out_path = os.path.join(self._tmpdir, "out.json")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(backup_export_http_client, "get_export",
                                         return_value=(json.dumps(doc), doc)):
            result = msi.backup_export(out_path=out_path)
        self.assertIn("WARNING", result)
        self.assertIn("zones.json", result)

    def test_export_silent_when_key_absent(self):
        out_path = os.path.join(self._tmpdir, "out.json")
        with self._resolve_host_patch(), \
             unittest.mock.patch.object(backup_export_http_client, "get_export",
                                         return_value=(json.dumps(_GOOD_DOC), _GOOD_DOC)):
            result = msi.backup_export(out_path=out_path)
        self.assertNotIn("stale_or_unknown_stores", result)

    def test_note_helper(self):
        self.assertEqual(msi._stale_stores_note({"stale_or_unknown_stores": []}, "x"), "")
        self.assertEqual(msi._stale_stores_note([], "x"), "")
        self.assertIn("a.bin", msi._stale_stores_note({"stale_or_unknown_stores": ["a.bin"]}, "x"))


class ReadbackContentTest(unittest.TestCase):
    """mcpfx2 M2: the post-import read-back compares content per profile id and zone index."""

    WANT = {"zones": [{"index": 0, "pid_kp": 1.5, "max_temp_c": 1200.0}],
            "profiles": [{"id": 3, "name": "a", "zone_mask": 1, "segments": [{"zone": 0, "target": 100.0}]}]}

    def _problem(self, got):
        with unittest.mock.patch.object(backup_export_http_client, "get_export", return_value=("{}", got)):
            return msi._backup_import_readback_problem("h", json.dumps(self.WANT), "ready")

    def test_identical_is_ok(self):
        self.assertIsNone(self._problem(json.loads(json.dumps(self.WANT))))

    def test_float_print_rounding_tolerated(self):
        got = json.loads(json.dumps(self.WANT))
        got["zones"][0]["pid_kp"] = 1.5000000001
        self.assertIsNone(self._problem(got))

    def test_zone_gain_not_written_detected(self):
        got = json.loads(json.dumps(self.WANT))
        got["zones"][0]["pid_kp"] = 9.0
        self.assertIn("pid_kp", self._problem(got))

    def test_profile_body_not_written_detected(self):
        got = json.loads(json.dumps(self.WANT))
        got["profiles"][0]["segments"][0]["target"] = 50.0
        self.assertIn("target", self._problem(got))
        got["profiles"][0]["name"] = "b"
        self.assertIn("profiles[id=3]", self._problem(got))

    def test_absent_key_in_reexport_detected(self):
        got = json.loads(json.dumps(self.WANT))
        del got["zones"][0]["max_temp_c"]
        self.assertIn("absent from the re-export", self._problem(got))

    def test_list_length_difference_detected(self):
        got = json.loads(json.dumps(self.WANT))
        got["profiles"][0]["segments"].append({"zone": 1, "target": 5.0})
        self.assertIn("entries", self._problem(got))
        got["profiles"][0]["segments"] = []
        self.assertIn("entries", self._problem(got))

    def test_legacy_import_only_zone_keys_not_demanded(self):
        want = json.loads(json.dumps(self.WANT))
        want["zones"][0].update(coupling_coeff=0.1, coupling_neighbor_zone=1)
        got = json.loads(json.dumps(self.WANT))
        got["zones"][0]["coupling_c1"] = 0.1  # the derived cell (LOW-3 verifies it)
        with unittest.mock.patch.object(backup_export_http_client, "get_export", return_value=("{}", got)):
            self.assertIsNone(msi._backup_import_readback_problem("h", json.dumps(want), "ready"))

    def test_idless_profile_matched_by_content_not_position(self):
        want = {"profiles": [{"name": "n", "zone_mask": 1, "segments": []}]}
        got = {"profiles": [{"id": 1, "name": "other", "zone_mask": 1, "segments": []},
                            {"id": 2, "name": "n", "zone_mask": 1, "segments": []}]}
        with unittest.mock.patch.object(backup_export_http_client, "get_export", return_value=("{}", got)):
            self.assertIsNone(msi._backup_import_readback_problem("h", json.dumps(want), "ready"))
        got["profiles"][1]["name"] = "zzz"
        with unittest.mock.patch.object(backup_export_http_client, "get_export", return_value=("{}", got)):
            self.assertIsNotNone(msi._backup_import_readback_problem("h", json.dumps(want), "ready"))

    def _idless(self, want_profiles, got_profiles, before=None):
        want = {"profiles": want_profiles}
        got = {"profiles": got_profiles}
        with unittest.mock.patch.object(backup_export_http_client, "get_export", return_value=("{}", got)):
            return msi._backup_import_readback_problem("h", json.dumps(want), "ready", before)

    P = {"name": "n", "zone_mask": 1, "segments": []}

    def test_two_identical_idless_profiles_one_landed_detected(self):
        # MED-1 case 1: both backup entries must not pair with the one board slot.
        got = [dict(self.P, id=1), dict(self.P, id=2, name="other")]
        self.assertIsNotNone(self._idless([dict(self.P), dict(self.P)], got))
        got[1] = dict(self.P, id=2)
        self.assertIsNone(self._idless([dict(self.P), dict(self.P)], got))

    def test_idless_does_not_reuse_slot_claimed_by_id_entry(self):
        # MED-1 case 2.
        self.assertIsNotNone(self._idless([dict(self.P, id=1), dict(self.P)], [dict(self.P, id=1)]))

    def test_idless_matching_preexisting_board_profile_detected(self):
        # MED-1 case 3: the identical profile was already on the board and the import wrote nothing.
        got = [dict(self.P, id=1)]
        self.assertIsNotNone(self._idless([dict(self.P)], got, before=[dict(self.P, id=1)]))
        # Landed in a new slot alongside the old one: fine.
        got2 = [dict(self.P, id=1), dict(self.P, id=2)]
        self.assertIsNone(self._idless([dict(self.P)], got2, before=[dict(self.P, id=1)]))

    def test_legacy_coupling_pair_derived_cell_verified(self):
        want = json.loads(json.dumps(self.WANT))
        want["zones"][0].update(coupling_coeff=0.1, coupling_neighbor_zone=1)
        got = json.loads(json.dumps(self.WANT))
        got["zones"][0]["coupling_c1"] = 0.1
        with unittest.mock.patch.object(backup_export_http_client, "get_export", return_value=("{}", got)):
            self.assertIsNone(msi._backup_import_readback_problem("h", json.dumps(want), "ready"))
        got["zones"][0]["coupling_c1"] = 0.7
        with unittest.mock.patch.object(backup_export_http_client, "get_export", return_value=("{}", got)):
            self.assertIn("coupling_c1", msi._backup_import_readback_problem("h", json.dumps(want), "ready"))

    def test_profile_missing_by_id_detected(self):
        got = json.loads(json.dumps(self.WANT))
        got["profiles"][0]["id"] = 4
        self.assertIn("missing", self._problem(got))


class BackupImportSnapshotTest(_Base):
    """Review testnet MED-B/MED-C/LOW-3/LOW-4: the pre-import snapshot wiring."""

    P = {"name": "n", "zone_mask": 1, "segments": []}

    def _run(self, backup_profiles, export, dry_run=False):
        doc = dict(_GOOD_DOC, profiles=backup_profiles)
        path = os.path.join(self._tmpdir, "backup.json")
        with open(path, "w") as f:
            f.write(json.dumps(doc))
        with self._resolve_host_patch(),              unittest.mock.patch.object(readiness_http_client, "get_readiness", return_value=_READINESS_OK),              unittest.mock.patch.object(backup_export_http_client, "get_export", side_effect=export) as ge,              unittest.mock.patch.object(bi, "post_import", return_value=(200, '{"ok":true}')):
            out = msi.backup_import(path, confirm=True, dry_run=dry_run)
        return out, ge

    def test_snapshot_failure_with_idless_profile_is_unverified(self):  # MED-B
        after = ("", dict(_GOOD_DOC, profiles=[dict(self.P, id=1)]))
        err = backup_export_http_client.BackupExportHttpError("snapshot down")
        out, _ = self._run([dict(self.P)], [err, after])
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertIn("snapshot unavailable", out)
        self.assertNotIn("ok - restored", out)

    def test_preexisting_identical_profile_is_unverified_via_tool(self):  # MED-C
        same = dict(self.P, id=1)
        out, ge = self._run([dict(self.P)], [("", {"profiles": [same]}), ("", {
            "kind": "kilnctl_backup", "profiles": [same], "zones": [{"id": 0}]})])
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertEqual(ge.call_count, 2)

    def test_new_slot_next_to_preexisting_is_ok_via_tool(self):  # MED-C control
        old = dict(self.P, id=1)
        out, _ = self._run([dict(self.P)], [("", {"profiles": [old]}), ("", {
            "kind": "kilnctl_backup", "profiles": [old, dict(self.P, id=2)], "zones": [{"id": 0}]})])
        self.assertIn("ok - restored", out)

    def test_snapshot_skipped_when_every_profile_has_id(self):  # LOW-3
        profs = [dict(self.P, id=1)]
        out, ge = self._run(profs, [("", dict(_GOOD_DOC, profiles=profs))])
        self.assertIn("ok - restored", out)
        self.assertEqual(ge.call_count, 1)

    def test_snapshot_skipped_on_dry_run(self):  # LOW-3
        out, ge = self._run([dict(self.P)], [AssertionError("no export on dry run")], dry_run=True)
        self.assertIn("dry run only", out)
        ge.assert_not_called()

    def test_idless_before_idcarrying_does_not_reuse_its_slot(self):  # LOW-4
        P = self.P
        self.assertIsNotNone(ReadbackContentTest._idless(
            ReadbackContentTest, [dict(P), dict(P, id=1)], [dict(P, id=1), dict(P, id=2, name="x")]))


if __name__ == "__main__":
    unittest.main()
