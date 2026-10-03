#!/usr/bin/env python3
"""Unit tests for mcp_server_recovery -- the MCP tools for the standalone
recovery image (firmware/KilnFW_recovery/). A FAKE board only: both HTTP
layers (recovery_http_client GETs and recovery_post_client.post)
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
from kilnctrl import recovery_post_client as rpc  # noqa: E402

HOST = "192.0.2.7"
BG_OK_REPLY = "ok, rebooting into new application image; boot_guard cleared and verified"


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

    def post(self, host, path, *, data=b"", query="", timeout=None):
        self.posts.append({"host": host, "path": path, "data": data, "query": query})
        if self.post_error:
            raise self.post_error
        return self.post_reply


def _unreachable():
    return rhc.RecoveryHttpError("unreachable", None)


def _not_found():
    return rhc.RecoveryHttpError("404", 404)


class _Base(unittest.TestCase):
    #: None -> candidate list is just [HOST] (the legacy single-address poll);
    #: the multi-address tests set this to a list.
    candidates = None

    def run_tool(self, fn, board, **kw):
        clock = {"t": 0.0}
        cands = self.candidates or [HOST]

        def fake_sleep(s):
            clock["t"] += s

        with unittest.mock.patch.object(mr, "_resolve_host", return_value=HOST), \
             unittest.mock.patch.object(mr, "_candidate_hosts", lambda h, a: list(cands)), \
             unittest.mock.patch.object(mr, "_app_identity", lambda h: ("running partition='app'; boot_guard fake", getattr(board, "fw_build", None))), \
             unittest.mock.patch.object(mr, "_sleep", fake_sleep), \
             unittest.mock.patch.object(mr, "_monotonic", lambda: clock["t"]), \
             unittest.mock.patch.object(rhc, "get_status", board.get_status), \
             unittest.mock.patch.object(rhc, "get_pico_status", board.get_pico_status), \
             unittest.mock.patch.object(rpc, "post", board.post):
            return fn(**kw)


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

    def test_renders_app_image_size_against_partition(self):
        board = FakeBoard(status=[_status(app_size=8388608, app_image_size=2569344)])
        out = self.run_tool(mr.recovery_status, board)
        self.assertIn("app image 2569344 bytes of 8388608 partition", out)

    def test_null_app_image_size_is_unknown_not_zero(self):
        board = FakeBoard(status=[_status(app_size=8388608, app_image_size=None)])
        out = self.run_tool(mr.recovery_status, board)
        self.assertIn("app image size unknown (not verified) of 8388608 partition", out)
        self.assertNotIn("app image 0 bytes", out)

    def test_older_image_without_app_image_size_renders_no_image_line(self):
        out = self.run_tool(mr.recovery_status, FakeBoard())
        self.assertNotIn("app image", out)

    def test_needs_no_credential_environment(self):
        # The recovery image is unauthenticated (owner decision 2026-10-02).
        board = FakeBoard()
        with unittest.mock.patch.dict(os.environ, {}, clear=True):
            out = self.run_tool(mr.recovery_status, board)
        self.assertIn("running=recovery", out)


def _diag_status(**kw):
    """A healthy status carrying every new diagnostic key."""
    d = _status(auth_mode="lcd_passphrase", uptime_s=123, reset_reason=1,
                reset_reason_name="poweron", app_ota_state="valid", coredump_present=False,
                otadata_blank=False, wifi_up=True, ap_start_count=1, ap_stop_count=0,
                ap_stations=2, ap_connect_total=5, wifi_last_event="ap_staconnected",
                wifi_last_event_age_s=7, relay_hold_task=True, relay_hold_fault=False,
                relay_hold_fault_s=None, relay_hold_last_ok_s=120, relay_hold_mismatches=0,
                relay_hold_reassert_fails=0, relay_hold_stack_free=2048)
    d.update(kw)
    return d


class StatusDiagnosticsTest(_Base):
    def _out(self, **kw):
        return self.run_tool(mr.recovery_status, FakeBoard(status=[_diag_status(**kw)]))

    def test_every_new_key_is_rendered_with_its_value(self):
        out = self._out()
        for k in ("auth_mode='lcd_passphrase'", "uptime_s=123", "reset_reason=1",
                  "reset_reason_name='poweron'", "app_ota_state='valid'", "coredump_present=False",
                  "otadata_blank=False", "wifi_up=True", "ap_start_count=1", "ap_stop_count=0",
                  "ap_stations=2", "ap_connect_total=5", "wifi_last_event='ap_staconnected'",
                  "wifi_last_event_age_s=7", "relay_hold_task=True", "relay_hold_fault=False",
                  "relay_hold_fault_s=null (board could not read it)", "relay_hold_last_ok_s=120",
                  "relay_hold_mismatches=0", "relay_hold_reassert_fails=0", "relay_hold_stack_free=2048"):
            self.assertIn(k, out)
        self.assertNotIn("WARNING", out)
        self.assertNotIn(mr.NOT_REPORTED, out)

    def test_older_image_says_not_reported_and_fabricates_nothing(self):
        out = self.run_tool(mr.recovery_status, FakeBoard())
        keys = [k for _, ks in mr._DIAG_GROUPS for k in ks]
        self.assertEqual(len(keys), 21)
        for k in keys:
            self.assertIn(f"{k}={mr.NOT_REPORTED}", out)
        self.assertNotIn("WARNING", out)
        # Existing lines are unchanged.
        self.assertTrue(out.startswith(f"recovery image (host={HOST}): running=recovery, app_present=True"))
        self.assertIn("pico relay: phase='idle'", out)

    def test_warn_ap_stopped(self):
        out = self._out(ap_stop_count=2)
        self.assertIn("WARNING: ap_stop_count=2", out)
        self.assertNotIn("WARNING: ap_stop_count", self._out(ap_stop_count=0))

    def test_warn_relay_hold_fault(self):
        self.assertIn("WARNING: relay_hold_fault=true", self._out(relay_hold_fault=True, relay_hold_fault_s=9))

    def test_warn_relay_hold_task_not_running(self):
        out = self._out(relay_hold_task=False)
        self.assertIn("WARNING: relay_hold_task=false", out)

    def test_warn_otadata_blank(self):
        self.assertIn("WARNING: otadata_blank=true", self._out(otadata_blank=True))

    def test_warn_coredump_present(self):
        self.assertIn("WARNING: coredump_present=true", self._out(coredump_present=True))

    def test_null_tristate_does_not_warn(self):
        out = self._out(coredump_present=None, otadata_blank=None)
        self.assertNotIn("WARNING", out)
        self.assertIn("coredump_present=null (board could not read it)", out)

    def test_existing_warning_line_is_kept_alongside(self):
        out = self._out(relay_fault=True, otadata_blank=True)
        self.assertIn("WARNING: RELAY FAULT", out)
        self.assertIn("WARNING: otadata_blank=true", out)


class ConfirmGateTest(_Base):
    """Every mutating tool: anything but the literal True refuses before ANY
    network access (zero GETs, zero POSTs)."""

    def _tools(self):
        img = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        # 0xE9 first: an image every LATER local check accepts, so the
        # confirm gate is the only thing that can refuse here (a bad-magic
        # image let push_esp_image's gate test pass with the gate removed).
        img.write(b"\xe9" + b"x" * 63)
        img.close()
        self.addCleanup(os.unlink, img.name)
        return [
            ("exit", mr.recovery_exit, {}),
            ("wifi_reset", mr.recovery_wifi_reset, {}),
            ("boot_guard_reset", mr.recovery_boot_guard_reset, {}),
            ("pico_upload", mr.recovery_pico_upload, {"image_path": img.name}),
            ("pico_abort", mr.recovery_pico_abort, {}),
            ("sw_reset", mr.recovery_sw_reset, {}),
            ("push_esp_image", mr.recovery_push_esp_image, {"image_path": img.name}),
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


class ExitTest(_Base):
    def test_success_when_app_answers_404(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _not_found()])
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertEqual(board.posts[0]["path"], "/api/recovery/exit")
        self.assertEqual(board.posts[0]["query"], "")
        self.assertEqual(len(board.posts), 1)

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
        board = FakeBoard(post_error=rpc.RecoveryPostError("refused: HTTP 409: busy", 409, "busy"))
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("FAILED"))
        self.assertIn("409", out)

    def test_lost_reply_still_observes_the_restart(self):
        lost = rpc.RecoveryPostError("timed out", None, stage="post")
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
                          post_error=rpc.RecoveryPostError("reset", None, stage="post"))
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
        self.assertEqual([p["path"] for p in board.posts], ["/api/ota/esp/boot_guard_reset"])

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
        self.assertEqual([p["path"] for p in board.posts], ["/api/ota/esp/boot_guard_reset"])

    def test_lost_reply_is_unverified(self):
        board = FakeBoard(post_error=rpc.RecoveryPostError("timed out", None, stage="post"))
        out = self.run_tool(mr.recovery_boot_guard_reset, board, confirm=True)
        self.assertTrue(out.startswith("UNVERIFIED"), out)


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

    def test_success_sends_crc_and_slot_in_query(self):
        board = self._board(self._done())
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True, slot="B")
        self.assertTrue(out.startswith("ok - "), out)
        post = board.posts[0]
        self.assertEqual(post["path"], "/api/recovery/pico/upload")
        self.assertEqual(post["query"], f"crc={self.crc:08x}&slot=B")
        self.assertEqual(post["data"], self.IMAGE)

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
        lost = rpc.RecoveryPostError("timed out", None, stage="post")
        board = self._board(self._done(), post_error=lost)
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertNotIn("ok -", out)
        self.assertGreaterEqual(board.pico_gets, 3)

    def test_lost_reply_and_idle_relay_is_not_started(self):
        lost = rpc.RecoveryPostError("reset", None, stage="post")
        board = FakeBoard(pico=[_pico()], post_error=lost)
        out = self.run_tool(mr.recovery_pico_upload, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("did not start", out)

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
        board = FakeBoard(post_error=rpc.RecoveryPostError("HTTP 422: bad vectors", 422, "bad vectors"))
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


class PicoAbortTest(_Base):
    def test_ok_when_relay_reports_aborted(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True), _pico(phase="sending", busy=True),
                                _pico(phase="aborted", busy=False)],
                          post_reply={"status": 200, "text": "abort requested"})
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertEqual([(p["path"], p["query"]) for p in board.posts],
                         [("/api/recovery/pico/abort", "")])

    def test_relay_self_abort_is_unverified_not_ok(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True),
                                _pico(phase="aborted", busy=False,
                                      error="browser stopped polling status: update aborted")],
                          post_reply={"status": 200, "text": "abort requested"})
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertIn("browser stopped polling", out)

    def test_operator_abort_text_still_ok(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True),
                                _pico(phase="aborted", busy=False, error="update aborted")],
                          post_reply={"status": 200, "text": "abort requested"})
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)

    def test_idle_relay_sends_no_post(self):
        board = FakeBoard()
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertIn("nothing to abort", out)
        self.assertEqual(board.posts, [])

    def test_done_before_abort_is_not_ok(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True), _pico(phase="done", busy=False)])
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertTrue(out.startswith("NOT ABORTED"), out)

    def test_outcome_unknown_is_unknown(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True), _pico(phase="outcome_unknown", busy=False)])
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_still_running_at_deadline_is_unknown(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True)])
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True, wait_s=5.0)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_lost_contact_is_unknown(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True), _unreachable()])
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_lost_reply_with_aborted_is_unknown_not_ok(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True), _pico(phase="aborted")],
                          post_error=rpc.RecoveryPostError("timed out", None, stage="post"))
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_post_refusal_is_failed(self):
        board = FakeBoard(pico=[_pico(phase="sending", busy=True)],
                          post_error=rpc.RecoveryPostError("HTTP 409", 409, "busy"))
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_refuses_against_main_app(self):
        board = FakeBoard(status=[_not_found()])
        out = self.run_tool(mr.recovery_pico_abort, board, confirm=True)
        self.assertIn("not the recovery image", out)
        self.assertEqual(board.posts, [])


class SwResetTest(_Base):
    def test_ok_when_restart_observed(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _status()],
                          post_reply={"status": 200, "text": "resetting"})
        out = self.run_tool(mr.recovery_sw_reset, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertEqual([p["path"] for p in board.posts], ["/api/sw_reset"])

    def test_app_answering_after_reset_is_reported(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _not_found()])
        out = self.run_tool(mr.recovery_sw_reset, board, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertIn("normal application", out)

    def test_fails_when_no_restart(self):
        board = FakeBoard()
        out = self.run_tool(mr.recovery_sw_reset, board, confirm=True, wait_s=4.0)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_silent_is_unverified(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable()])
        out = self.run_tool(mr.recovery_sw_reset, board, confirm=True, wait_s=4.0)
        self.assertTrue(out.startswith("UNVERIFIED"), out)

    def test_lost_reply_is_unknown_never_ok(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _status()],
                          post_error=rpc.RecoveryPostError("reset", None, stage="post"))
        out = self.run_tool(mr.recovery_sw_reset, board, confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_refuses_when_pico_busy(self):
        board = FakeBoard(pico=[_pico(busy=True, phase="sending")])
        out = self.run_tool(mr.recovery_sw_reset, board, confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertEqual(board.posts, [])

    def test_refuses_against_main_app(self):
        board = FakeBoard(status=[_not_found()])
        out = self.run_tool(mr.recovery_sw_reset, board, confirm=True)
        self.assertIn("not the recovery image", out)
        self.assertEqual(board.posts, [])

    def test_wrong_running_label_refuses(self):
        board = FakeBoard(status=[_status(running="app")])
        out = self.run_tool(mr.recovery_sw_reset, board, confirm=True)
        self.assertIn("not 'recovery'", out)
        self.assertEqual(board.posts, [])


class PushEspImageTest(_Base):
    def setUp(self):
        self.path = self._img(b"\xe9" + b"x" * 99)

    def _img(self, data):
        f = tempfile.NamedTemporaryFile(delete=False, suffix=".bin")
        f.write(data)
        f.close()
        self.addCleanup(os.unlink, f.name)
        return f.name

    def test_ok_when_application_answers(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _not_found()],
                          post_reply={"status": 200, "text": BG_OK_REPLY})
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("ok - "), out)
        self.assertEqual(len(board.posts), 1)
        p = board.posts[0]
        self.assertEqual((p["path"], p["query"]), ("/api/ota/esp", ""))
        self.assertEqual(p["data"], b"\xe9" + b"x" * 99)

    def test_image_larger_than_partition_refused_before_post(self):
        board = FakeBoard(status=[_status(max_upload=50, app_size=50)])
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertIn("larger than the app partition", out)
        self.assertEqual(board.posts, [])

    def test_image_exactly_partition_size_is_allowed(self):
        board = FakeBoard(status=[_status(max_upload=100), _status(), _not_found()])
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertEqual(len(board.posts), 1, out)

    def test_falls_back_to_app_size_and_refuses_when_unknown(self):
        board = FakeBoard(status=[_status(max_upload=0, app_size=50)])
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertIn("larger than the app partition", out)
        board = FakeBoard(status=[_status(max_upload=0, app_size=0)])
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertEqual(board.posts, [])

    def test_refuses_while_pico_busy(self):
        board = FakeBoard(pico=[_pico(busy=True, phase="sending")])
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertEqual(board.posts, [])

    def test_bad_arguments_refuse_with_no_io(self):
        bad_magic = self._img(b"\x00" + b"x" * 10)
        empty = self._img(b"")
        for p in ("relative.bin", os.path.join(tempfile.gettempdir(), "no_such_esp_image_xyz.bin"),
                  bad_magic, empty):
            board = FakeBoard()
            out = self.run_tool(mr.recovery_push_esp_image, board, image_path=p, confirm=True)
            self.assertTrue(out.startswith("REFUSED"), (p, out))
            self.assertEqual((board.status_gets, board.posts), (0, []))

    def test_refuses_against_main_app(self):
        board = FakeBoard(status=[_not_found()])
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertIn("not the recovery image", out)
        self.assertEqual(board.posts, [])

    def test_board_rejection_is_failed(self):
        board = FakeBoard(post_error=rpc.RecoveryPostError("HTTP 422: bad image", 422, "bad image"))
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("422", out)

    def test_boot_guard_not_cleared_is_ok_with_warning(self):
        for text in ("ok, rebooting into new application image; boot_guard clear failed (erased but "
                     "read-back not verified)", "ok, rebooting into new application image"):
            board = FakeBoard(status=[_status(), _status(), _unreachable(), _not_found()],
                              post_reply={"status": 200, "text": text})
            out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
            self.assertTrue(out.startswith("ok-with-warning"), out)
            self.assertIn("boot_guard", out)
            self.assertNotIn("ok - ", out)
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _not_found()],
                          post_reply={"status": 200, "text": "ok, rebooting into new application "
                                      "image; boot_guard clear failed (erased but read-back not verified)"})
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertIn("clear failed (erased but read-back not verified)", out)

    def test_mid_stream_rejection_warns_app_may_be_erased_with_app_valid(self):
        for code in (400, 422, 500):
            board = FakeBoard(status=[_status(), _status(app_valid=False, app_present=False)],
                              post_error=rpc.RecoveryPostError(f"HTTP {code}", code, "x", stage="post"))
            out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
            self.assertTrue(out.startswith("FAILED"), out)
            self.assertIn("partition", out)
            self.assertIn("recovery_status", out)
            self.assertIn("app_valid=False", out)

    def test_mid_stream_rejection_status_reread_failure_still_warns(self):
        board = FakeBoard(status=[_status(), _unreachable()],
                          post_error=rpc.RecoveryPostError("HTTP 400", 400, "lost", stage="post"))
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertIn("do NOT trust a reboot", out)
        self.assertIn("could not re-read status", out)

    def test_pre_erase_refusal_has_no_erase_warning(self):
        # recovery_http.c/recovery_upload.c answer these only before the first
        # esp_ota_write(): Pico busy, length/oversize gate, no buffer.
        for code in (409, 413, 503):
            board = FakeBoard(post_error=rpc.RecoveryPostError(f"HTTP {code}", code, "x", stage="post"))
            out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
            self.assertTrue(out.startswith("FAILED"), out)
            self.assertNotIn("erased", out)
            self.assertEqual(board.status_gets, 1)  # preflight only, no re-read

    def test_back_in_recovery_is_failed(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _status()])
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_never_restarted_is_failed(self):
        board = FakeBoard()
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True, wait_s=4.0)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_silent_is_unverified(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable()])
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True, wait_s=4.0)
        self.assertTrue(out.startswith("UNVERIFIED"), out)

    def test_lost_reply_is_unknown_even_if_app_answers(self):
        board = FakeBoard(status=[_status(), _status(), _unreachable(), _not_found()],
                          post_error=rpc.RecoveryPostError("reset", None, stage="post"))
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self.path, confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)


if __name__ == "__main__":
    unittest.main()


LAN = "192.0.2.156"


class HostAwareBoard(FakeBoard):
    """Per-host scripted status GETs: `hosts` maps host -> script."""

    def __init__(self, hosts, **kw):
        super().__init__(**kw)
        self.hosts = {h: list(v) for h, v in hosts.items()}

    def get_status(self, host, timeout=None):
        self.status_gets += 1
        return self._next(self.hosts.get(host, [_unreachable()]))


class LanVerifyTest(_Base):
    """The application boots onto the LAN and the recovery AP disappears, so
    verification must also poll the application's LAN address (2026-10-03)."""
    candidates = [HOST, LAN]

    def _push_board(self):
        return HostAwareBoard({HOST: [_status(max_upload=1000), _status(max_upload=1000), _unreachable()],
                               LAN: [_unreachable(), _not_found()]},
                              post_reply={"status": 200, "text": BG_OK_REPLY})

    def _image(self):
        with tempfile.NamedTemporaryFile(delete=False, suffix=".bin") as fh:
            fh.write(bytes([0xE9]) + b"\0" * 99)
        self.addCleanup(os.unlink, fh.name)
        return fh.name

    def test_exit_verified_on_lan_candidate(self):
        board = HostAwareBoard({HOST: [_status(), _status(), _unreachable()], LAN: [_unreachable(), _not_found()]})
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("ok"), out)
        self.assertIn("answering at " + LAN, out)
        self.assertIn("running partition='app'", out)

    def test_exit_only_recovery_answers_is_not_ok(self):
        board = HostAwareBoard({HOST: [_status()], LAN: [_unreachable()]})
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertFalse(out.startswith("ok"), out)
        self.assertIn("still answers as the recovery image", out)

    def test_exit_nothing_answers_is_unverified_and_lists_candidates(self):
        board = HostAwareBoard({HOST: [_status(), _status(), _unreachable()], LAN: [_unreachable()]})
        out = self.run_tool(mr.recovery_exit, board, confirm=True)
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertIn(HOST, out)
        self.assertIn(LAN, out)

    def test_push_verified_on_lan_candidate(self):
        out = self.run_tool(mr.recovery_push_esp_image, self._push_board(), image_path=self._image(), confirm=True)
        self.assertTrue(out.startswith("ok"), out)
        self.assertIn("answered at " + LAN, out)

    def test_push_build_mismatch_fails(self):
        board = self._push_board()
        board.fw_build = "Jan  1 2020 00:00:00"
        desc = unittest.mock.Mock(build_timestamp="Oct  3 2026 10:00:00")
        with unittest.mock.patch("kilnctrl.esp_app_desc.parse_app_desc_file", return_value=desc), \
             unittest.mock.patch("kilnctrl.esp_app_desc.build_timestamps_match", return_value=False):
            out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self._image(), confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("different build", out)

    def test_push_nothing_answers_is_unverified_and_lists_candidates(self):
        board = HostAwareBoard({HOST: [_status(max_upload=1000), _status(max_upload=1000), _unreachable()],
                                LAN: [_unreachable()]}, post_reply={"status": 200, "text": BG_OK_REPLY})
        out = self.run_tool(mr.recovery_push_esp_image, board, image_path=self._image(), confirm=True)
        self.assertTrue(out.startswith("UNVERIFIED"), out)
        self.assertIn(LAN, out)


class CandidateHostsTest(unittest.TestCase):
    def test_explicit_app_host_is_exactly_host_plus_app_host(self):
        self.assertEqual(mr._candidate_hosts("192.168.4.1", LAN), ["192.168.4.1", LAN])

    def test_env_host_added_and_deduped(self):
        with unittest.mock.patch.dict(os.environ, {"KILNCTL_HOST": LAN}), \
             unittest.mock.patch("kilnctrl.mcp_server_flash._resolve_verify_hosts", return_value=[LAN, "192.168.4.1"]):
            self.assertEqual(mr._candidate_hosts("192.168.4.1", None), ["192.168.4.1", LAN])
