#!/usr/bin/env python3
"""Unit tests for mcp_server_recovery -- the MCP tools for the standalone
recovery image (firmware/KilnFW_recovery/). A FAKE board only: both HTTP
layers (recovery_http_client GETs and recovery_ota_auth_client.signed_post)
are replaced, no socket is opened, sleeping is stubbed, and the host is a
documentation address. Never run against hardware.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_recovery.py -q
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
import unittest.mock
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_recovery as mr  # noqa: E402
from kilnctrl import recovery_http_client as rhc  # noqa: E402
from kilnctrl import recovery_ota_auth_client as roac  # noqa: E402

HOST = "192.0.2.7"
SECRET = "unit-test-ap-secret"


def _status(**kw):
    d = {"running": "recovery", "app_present": True, "app_size": 1000, "app_desc_present": True,
         "app_valid": True, "max_upload": 1000, "record_present": True, "record_len": 12,
         "boot_count": 4, "relay_fault": False, "relays_verified_off": True,
         "nvs_unavailable": False, "nvs_failed_mask": 0, "heap_internal_min_free": 50000,
         "free_heap": 90000}
    d.update(kw)
    return d


def _pico(**kw):
    d = {"phase": "idle", "busy": False, "psram": True, "bytes_sent": 0, "total_bytes": 0,
         "gap_count": 0, "gap_rounds": 0, "pico_mode": "unknown", "pico_state": 0, "pico_err": 0,
         "target_slot": "unknown", "target_source": "unresolved", "power_cycle": False,
         "refusal": "", "error": "", "message": ""}
    d.update(kw)
    return d


class FakeBoard:
    """Scripted recovery board. `status_script`/`pico_script` are consumed
    one entry per GET (the last entry repeats); an entry may be a dict or an
    Exception to raise."""

    def __init__(self, status=None, pico=None, post_reply=None, post_error=None):
        self.status_script = list(status if status is not None else [_status()])
        self.pico_script = list(pico if pico is not None else [_pico()])
        self.post_reply = post_reply or {"status": 200, "text": "ok"}
        self.post_error = post_error
        self.posts = []
        self.status_gets = 0
        self.pico_gets = 0

    @staticmethod
    def _next(script):
        item = script.pop(0) if len(script) > 1 else script[0]
        if isinstance(item, Exception):
            raise item
        return item

    def get_status(self, host, timeout=None):
        self.status_gets += 1
        return self._next(self.status_script)

    def get_pico_status(self, host, timeout=None):
        self.pico_gets += 1
        return self._next(self.pico_script)

    def signed_post(self, host, path, context, ap_password, *, data=b"", query="", timeout=None):
        self.posts.append({"host": host, "path": path, "context": context, "data": data,
                           "query": query, "password_given": ap_password == SECRET})
        if self.post_error:
            raise self.post_error
        return self.post_reply


def _unreachable():
    return rhc.RecoveryHttpError("unreachable", None)


def _not_found():
    return rhc.RecoveryHttpError("404", 404)


class _Base(unittest.TestCase):
    def run_tool(self, fn, board, *, env=True, **kw):
        clock = {"t": 0.0}

        def fake_sleep(s):
            clock["t"] += s

        env_patch = unittest.mock.patch.dict(os.environ, {mr.AP_PASSWORD_ENV: SECRET} if env else {},
                                             clear=False)
        if not env:
            os.environ.pop(mr.AP_PASSWORD_ENV, None)
        with env_patch, \
             unittest.mock.patch.object(mr, "_resolve_host", return_value=HOST), \
             unittest.mock.patch.object(mr, "_sleep", fake_sleep), \
             unittest.mock.patch.object(mr, "_monotonic", lambda: clock["t"]), \
             unittest.mock.patch.object(rhc, "get_status", board.get_status), \
             unittest.mock.patch.object(rhc, "get_pico_status", board.get_pico_status), \
             unittest.mock.patch.object(roac, "signed_post", board.signed_post):
            if not env:
                os.environ.pop(mr.AP_PASSWORD_ENV, None)
            return fn(**kw)

    def assertNoSecret(self, text):
        self.assertNotIn(SECRET, text)


class StatusTest(_Base):
    def test_reports_both_routes_and_never_posts(self):
        board = FakeBoard()
        out = self.run_tool(mr.recovery_status, board)
        self.assertIn("running=recovery", out)
        self.assertIn("phase='idle'", out)
        self.assertEqual(board.posts, [])

    def test_404_means_not_recovery_image(self):
        board = FakeBoard(status=[_not_found()])
        out = self.run_tool(mr.recovery_status, board)
        self.assertIn("not the recovery image", out)

    def test_wrong_running_label(self):
        board = FakeBoard(status=[_status(running="app")])
        self.assertIn("not 'recovery'", self.run_tool(mr.recovery_status, board))

    def test_warns_on_outcome_unknown(self):
        board = FakeBoard(pico=[_pico(phase="outcome_unknown")])
        self.assertIn("OUTCOME UNKNOWN", self.run_tool(mr.recovery_status, board))

    def test_does_not_need_the_password(self):
        board = FakeBoard()
        out = self.run_tool(mr.recovery_status, board, env=False)
        self.assertIn("running=recovery", out)


class ConfirmGateTest(_Base):
    """Every mutating tool: anything but the literal True refuses before ANY
    network access (zero GETs, zero POSTs)."""

    def _tools(self):
        img = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        img.write(b"x" * 64)
        img.close()
        self.addCleanup(os.unlink, img.name)
        return [
            ("exit", mr.recovery_exit, {}),
            ("wifi_reset", mr.recovery_wifi_reset, {}),
            ("boot_guard_reset", mr.recovery_boot_guard_reset, {}),
            ("pico_upload", mr.recovery_pico_upload, {"image_path": img.name}),
        ]

    def test_non_true_confirm_refuses_with_no_io(self):
        for name, fn, extra in self._tools():
            for bad in (False, None, 1, "true", "yes", 1.0, [True]):
                board = FakeBoard()
                out = self.run_tool(fn, board, confirm=bad, **extra)
                self.assertTrue(out.startswith("REFUSED"), (name, bad, out))
                self.assertEqual((board.status_gets, board.pico_gets, board.posts), (0, 0, []), (name, bad))

    def test_default_confirm_refuses(self):
        for name, fn, extra in self._tools():
            board = FakeBoard()
            out = self.run_tool(fn, board, **extra)
            self.assertTrue(out.startswith("REFUSED"), name)
            self.assertEqual(board.posts, [])


class PasswordTest(_Base):
    def test_missing_password_refuses_and_reports_bool_only(self):
        for fn in (mr.recovery_exit, mr.recovery_wifi_reset, mr.recovery_boot_guard_reset):
            board = FakeBoard()
            out = self.run_tool(fn, board, env=False, confirm=True)
            self.assertTrue(out.startswith("REFUSED"))
            self.assertIn("set=False", out)
            self.assertEqual((board.status_gets, board.posts), (0, []))


class ExitTest(_Base):
    def test_success_when_app_answers_404(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _not_found()])
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertEqual([p["context"] for p in board.posts], ["recovery-exit"])
        self.assertEqual(board.posts[0]["path"], "/api/recovery/exit")
        self.assertEqual(board.posts[0]["query"], "")
        self.assertTrue(board.posts[0]["password_given"])
        self.assertNoSecret(out)

    def test_refuses_when_pico_busy(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True)])
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("REFUSED"))
        self.assertEqual(board.posts, [])

    def test_refuses_when_app_not_valid(self):
        board = FakeBoard(status=[_status(app_valid=False)])
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("REFUSED"))
        self.assertEqual(board.posts, [])

    def test_fails_when_board_never_restarts(self):
        board = FakeBoard()  # keeps answering as recovery
        out = self.run_tool(mr.recovery_exit, board, confirm=True, wait_s=5.0)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("never restarted", out)

    def test_fails_when_it_comes_back_as_recovery(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _status()])
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("RECOVERY image", out)

    def test_unverified_when_silent(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable()])
        out = self.run_tool(mr.recovery_exit, board, confirm=True, wait_s=5.0)
        self.assertTrue(out.startswith("UNVERIFIED"), out)

    def test_post_refusal_is_reported_with_status(self):
        board = FakeBoard(post_error=roac.RecoveryOtaAuthError("refused: HTTP 403: bad MAC", 403, "bad MAC"))
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("FAILED"))
        self.assertIn("403", out)
        self.assertNoSecret(out)

    def test_lost_reply_still_observes_the_restart(self):
        lost = roac.RecoveryOtaAuthError("timed out", None, stage="post")
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _not_found()], post_error=lost)
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertIn("reply lost", out)
        board = FakeBoard(status=[_status(), _status(), _unreachable()], post_error=lost)
        out = self.run_tool(mr.recovery_exit, board, confirm=True, wait_s=5.0)
        self.assertTrue(out.startswith("UNVERIFIED"), out)

    def test_not_recovery_image_does_nothing(self):
        board = FakeBoard(status=[_not_found()])
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertIn("not the recovery image", out)
        self.assertEqual(board.posts, [])


class WifiResetTest(_Base):
    def test_ok_when_restart_observed(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _status()])
        out = self.run_tool(mr.recovery_wifi_reset, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertIn("not readable", out)
        self.assertEqual([p["context"] for p in board.posts], ["wifi-reset"])
        self.assertEqual(board.posts[0]["path"], "/api/recovery/wifi_reset")

    def test_fails_when_no_restart(self):
        board = FakeBoard()
        out = self.run_tool(mr.recovery_wifi_reset, board, confirm=True, wait_s=4.0)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_unverified_when_silent_mentions_ap(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable()])
        out = self.run_tool(mr.recovery_wifi_reset, board, confirm=True, wait_s=4.0)
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertIn("192.168.4.1", out)

    def test_lost_reply_with_restart_is_unverified(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _status()],
                          post_error=roac.RecoveryOtaAuthError("reset", None, stage="post"))
        out = self.run_tool(mr.recovery_wifi_reset, board, confirm=True)
        self.assertTrue(out.startswith("UNVERIFIED"), out)

    def test_refuses_when_pico_busy(self):
        board = FakeBoard(pico=[_pico(busy=True, phase="erasing")])
        out = self.run_tool(mr.recovery_wifi_reset, board, confirm=True)
        self.assertTrue(out.startswith("REFUSED"))
        self.assertEqual(board.posts, [])


class BootGuardResetTest(_Base):
    def test_ok_when_record_gone(self):
        board = FakeBoard(status=[_status(), _status(record_present=False, record_len=0)])
        out = self.run_tool(mr.recovery_boot_guard_reset, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertEqual([p["context"] for p in board.posts], ["boot-guard-reset"])

    def test_fails_when_readback_disagrees(self):
        board = FakeBoard(status=[_status(), _status(record_present=True)],
                          post_reply={"status": 200, "text": "boot_guard cleared and verified"})
        out = self.run_tool(mr.recovery_boot_guard_reset, board, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("record_present=True", out)

    def test_unverified_when_readback_unreachable(self):
        board = FakeBoard(status=[_status(), _unreachable()])
        out = self.run_tool(mr.recovery_boot_guard_reset, board, confirm=True)
        self.assertTrue(out.startswith("UNVERIFIED"), out)

    def test_absent_current_record_still_posts(self):
        # recovery_status's record_present covers only the CURRENT location;
        # the board's clear_boot_guard() also erases the legacy
        # "boot_guard"/"count" record the main app still reads as a
        # fallback, so the tool must not skip the POST on record_present=False.
        board = FakeBoard(status=[_status(record_present=False, record_len=0)],
                          post_reply={"status": 200, "text": "boot_guard cleared and verified"})
        out = self.run_tool(mr.recovery_boot_guard_reset, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertEqual([p["context"] for p in board.posts], ["boot-guard-reset"])

    def test_lost_reply_is_unverified(self):
        board = FakeBoard(post_error=roac.RecoveryOtaAuthError("timed out", None, stage="post"))
        out = self.run_tool(mr.recovery_boot_guard_reset, board, confirm=True)
        self.assertTrue(out.startswith("UNVERIFIED"), out)

    def test_challenge_failure_is_failed(self):
        board = FakeBoard(post_error=roac.RecoveryOtaAuthError("challenge unreachable", None))
        out = self.run_tool(mr.recovery_boot_guard_reset, board, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)


class PicoUploadTest(_Base):
    IMAGE = bytes(range(256)) * 4

    def setUp(self):
        f = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        f.write(self.IMAGE)
        f.close()
        self.path = f.name
        self.addCleanup(os.unlink, self.path)
        self.crc = zlib.crc32(self.IMAGE) & 0xFFFFFFFF
        self.started = {"status": 202, "text": '{"started":true,"image_slot":"A"}'}

    def _board(self, final, **kw):
        n = len(self.IMAGE)
        mid = _pico(phase="sending", busy=True, bytes_sent=n // 2, total_bytes=n)
        return FakeBoard(pico=[_pico(), mid, final], post_reply=self.started, **kw)

    def _done(self, **kw):
        n = len(self.IMAGE)
        return _pico(phase="done", busy=False, bytes_sent=n, total_bytes=n, **kw)

    def test_success_signs_crc_and_slot_in_query(self):
        board = self._board(self._done())
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True, slot="B")
        self.assertTrue(out.startswith("ok - "), out)
        post = board.posts[0]
        self.assertEqual(post["context"], "pico-upload")
        self.assertEqual(post["path"], "/api/recovery/pico/upload")
        self.assertEqual(post["query"], f"crc={self.crc:08x}&slot=B")
        self.assertEqual(post["data"], self.IMAGE)
        self.assertNoSecret(out)

    def test_auto_slot_has_no_slot_in_query(self):
        board = self._board(self._done())
        self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertEqual(board.posts[0]["query"], f"crc={self.crc:08x}")

    def test_refuses_unless_relay_idle(self):
        for busy_pico in (_pico(busy=True, phase="sending"), _pico(busy="maybe"), _pico(psram=False)):
            board = FakeBoard(pico=[busy_pico], post_reply=self.started)
            out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
            self.assertTrue(out.startswith("REFUSED"), out)
            self.assertEqual(board.posts, [])

    def test_outcome_unknown_is_never_success(self):
        board = self._board(_pico(phase="outcome_unknown", busy=False, bytes_sent=len(self.IMAGE),
                                   total_bytes=len(self.IMAGE),
                                   message="stopped after END was sent - outcome unknown"))
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("OUTCOME UNKNOWN - NOT success"), out)
        self.assertNotIn("ok -", out)
        self.assertIn("do NOT retry blindly", out)

    def test_failed_reports_board_error(self):
        board = self._board(_pico(phase="failed", busy=False, error="erase timeout"))
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("erase timeout", out)

    def test_aborted(self):
        board = self._board(_pico(phase="aborted", busy=False))
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("ABORTED"), out)

    def test_done_with_wrong_byte_count_fails(self):
        board = self._board(_pico(phase="done", busy=False, bytes_sent=10, total_bytes=len(self.IMAGE)))
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_done_with_self_consistent_but_wrong_length_fails(self):
        n = len(self.IMAGE) - 1
        board = self._board(_pico(phase="done", busy=False, bytes_sent=n, total_bytes=n))
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_torn_done_read_is_reread_not_failed(self):
        # publish_done(): set_phase(DONE) and s_bytes_sent = s_len are two
        # separate lock holds; a poll between them sees done + short bytes.
        n = len(self.IMAGE)
        torn = _pico(phase="done", busy=True, bytes_sent=n - 64, total_bytes=n)
        board = FakeBoard(pico=[_pico(), torn, self._done()], post_reply=self.started)
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)

    def test_idle_after_start_is_unknown(self):
        board = self._board(_pico())  # relay back to idle: the board restarted
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_lost_reply_keeps_polling_and_is_never_ok(self):
        lost = roac.RecoveryOtaAuthError("timed out", None, stage="post")
        board = self._board(self._done(), post_error=lost)
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertNotIn("ok -", out)
        self.assertGreaterEqual(board.pico_gets, 3)

    def test_lost_reply_and_idle_relay_is_not_started(self):
        lost = roac.RecoveryOtaAuthError("reset", None, stage="post")
        board = FakeBoard(pico=[_pico()], post_error=lost)
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("did not start", out)

    def test_challenge_failure_does_not_poll(self):
        board = FakeBoard(post_error=roac.RecoveryOtaAuthError("challenge unreachable", None))
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertEqual(board.pico_gets, 1)  # the preflight read only

    def test_image_larger_than_a_slot_refused_locally(self):
        f = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        f.write(b"\0" * (mr.MAX_PICO_IMAGE_BYTES + 1))
        f.close()
        self.addCleanup(os.unlink, f.name)
        self.assertEqual(mr.MAX_PICO_IMAGE_BYTES, 0x000D0000)  # recovery_pico_proto.h RPP_SLOT_SIZE
        board = FakeBoard(post_reply=self.started)
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=f.name, confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertEqual(board.posts, [])

    def test_timeout_while_running_is_unknown(self):
        mid = _pico(phase="sending", busy=True, bytes_sent=1, total_bytes=len(self.IMAGE))
        board = FakeBoard(pico=[_pico(), mid], post_reply=self.started)
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True, wait_s=5.0)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_lost_contact_is_unknown(self):
        board = FakeBoard(pico=[_pico(), _unreachable()], post_reply=self.started)
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_post_not_202_fails(self):
        board = FakeBoard(post_reply={"status": 200, "text": "ok"})
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_422_from_board_is_failure(self):
        board = FakeBoard(post_error=roac.RecoveryOtaAuthError("HTTP 422: bad vectors", 422, "bad vectors"))
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("422", out)

    def test_bad_arguments_refuse(self):
        for kw in ({"slot": "C"}, {"image_path": "relative.bin"},
                   {"image_path": os.path.join(tempfile.gettempdir(), "no_such_image_xyz.bin")}):
            board = FakeBoard()
            args = {"image_path": self.path, "confirm": True}
            args.update(kw)
            out = self.run_tool(mr.recovery_pico_upload, board, **args)
            self.assertTrue(out.startswith("REFUSED"), (kw, out))
            self.assertEqual(board.posts, [])

    def test_empty_image_refused(self):
        f = tempfile.NamedTemporaryFile(delete=False)
        f.close()
        self.addCleanup(os.unlink, f.name)
        board = FakeBoard()
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=f.name, confirm=True)
        self.assertTrue(out.startswith("REFUSED"))


if __name__ == "__main__":
    unittest.main()
