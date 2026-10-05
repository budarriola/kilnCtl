#!/usr/bin/env python3
"""Unit tests for kilnctrl.update_http_client and kilnctrl.mcp_server_update
(WP6 of docs/GITHUB_RELEASE_UPDATE_PLAN.md) against a FAKE BOARD: a small
stateful stand-in for the three routes in update_http.c, patched in at
http_auth.urlopen(). No socket, no real board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_update.py -q
"""
from __future__ import annotations

import hashlib
import io
import json
import os
import sys
import tempfile
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_update as msu  # noqa: E402
from kilnctrl import update_http_client as uhc  # noqa: E402

CAPACITY = 0x3FF000


def _image(n: int = 4096, fill: int = 0x5A) -> bytes:
    return bytes([0xE9]) + bytes([fill]) * (n - 1)


class _Resp(io.BytesIO):
    status = 200

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        return False


class FakeBoard:
    """Mimics update_http.c. `corrupt_sha` makes the board store a wrong
    hash (the failure the read-back must catch); `refuse` makes the POST
    answer that (status, error-name)."""

    def __init__(self):
        self.staged = None  # dict or None
        self.busy = False
        self.refuse = None
        self.needs_ack = False  # safety link down: 428 unless the ack header is sent
        self.corrupt_sha = False
        self.drop_reply = False
        self.requests = []

    def _status(self) -> dict:
        s = self.staged
        return {
            "ok": True, "phase": "idle", "busy": self.busy, "bytes_done": 0, "bytes_total": 0,
            "staged": s is not None, "reason": "" if s else "blank", "header": "ok" if s else "blank",
            "capacity": CAPACITY, "image_length": s["len"] if s else 0,
            "state": "verified" if s else "", "semver": s["semver"] if s else "",
            "commit": s["commit"] if s else "", "sha256": s["sha"] if s else "",
            "source": 1 if s else 0,
        }

    @staticmethod
    def _http_error(req, code, body):
        return urllib.error.HTTPError(req.full_url, code, "x", {}, io.BytesIO(json.dumps(body).encode()))

    def urlopen(self, req, timeout=None, **kw):
        self.requests.append(req)
        path = req.full_url.split("/api", 1)[1]
        m = req.get_method()
        if m == "GET" and path == "/update/stage":
            return _Resp(json.dumps(self._status()).encode())
        if m == "POST" and path == "/update/stage":
            if self.needs_ack and not req.has_header("X-ota-ack-no-safety"):
                raise self._http_error(req, 428, {"ok": False, "error": "safety_not_answering"})
            if self.refuse:
                raise self._http_error(req, self.refuse[0], {"ok": False, "error": self.refuse[1]})
            if self.drop_reply:
                raise urllib.error.URLError("connection reset")
            data = req.data
            sha = hashlib.sha256(data).hexdigest()
            if self.corrupt_sha:
                sha = "0" * 64
            self.staged = {"len": len(data), "sha": sha,
                           "semver": req.get_header("X-stage-version") or "1.2.3",
                           "commit": req.get_header("X-stage-commit") or ""}
            return _Resp(b'{"ok":true,"staged":true}')
        if m == "POST" and path == "/update/stage/clear":
            if self.needs_ack and not req.has_header("X-ota-ack-no-safety"):
                raise self._http_error(req, 428, {"ok": False, "error": "safety_not_answering"})
            if self.refuse:
                raise self._http_error(req, self.refuse[0], {"ok": False, "error": self.refuse[1]})
            self.staged = None
            return _Resp(b'{"ok":true,"staged":false}')
        raise AssertionError(f"unexpected request {m} {path}")

    def posts(self):
        return [r for r in self.requests if r.get_method() == "POST"]


class _Base(unittest.TestCase):
    def setUp(self):
        self.board = FakeBoard()
        for p in (
            unittest.mock.patch.object(uhc.http_auth, "urlopen", self.board.urlopen),
            unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"),
        ):
            p.start()
            self.addCleanup(p.stop)
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)

    def write_image(self, image: bytes, name: str = "KilnCtrl.bin") -> str:
        path = os.path.join(self.tmp.name, name)
        with open(path, "wb") as fh:
            fh.write(image)
        return path


class ClientTest(_Base):
    def test_validate_rejects_bad_inputs(self):
        self.assertIsNotNone(uhc.validate_upload_args(b""))
        self.assertIsNotNone(uhc.validate_upload_args(b"\x00abc"))
        self.assertIsNotNone(uhc.validate_upload_args(_image(), version="1.2 3"))
        self.assertIsNotNone(uhc.validate_upload_args(_image(), version="x" * 40))
        self.assertIsNotNone(uhc.validate_upload_args(_image(), commit="ABC"))
        self.assertIsNone(uhc.validate_upload_args(_image(), version="v1.0.0", commit="a" * 40))

    def test_upload_sends_raw_body_and_headers(self):
        uhc.upload_stage("h", _image(), version="1.0.0", commit="b" * 40)
        req = self.board.posts()[0]
        self.assertEqual(req.get_header("Content-type"), "application/octet-stream")
        self.assertEqual(req.get_header("X-stage-version"), "1.0.0")
        self.assertEqual(req.get_header("X-stage-commit"), "b" * 40)
        self.assertEqual(req.data, _image())

    def test_invalid_upload_makes_no_request(self):
        with self.assertRaises(uhc.UpdateHttpError):
            uhc.upload_stage("h", b"\x00" * 10)
        self.assertEqual(self.board.requests, [])

    def test_error_name_parsed(self):
        self.board.refuse = (409, "update_in_progress")
        with self.assertRaises(uhc.UpdateHttpError) as cm:
            uhc.upload_stage("h", _image())
        self.assertEqual(cm.exception.status, 409)
        self.assertEqual(uhc.error_name(cm.exception), "update_in_progress")


class StatusTest(_Base):
    def test_status_reports_blank_then_staged(self):
        self.assertIn("staged=False", msu.update_status())
        uhc.upload_stage("h", _image())
        out = msu.update_status()
        self.assertIn("staged=True", out)
        self.assertIn(hashlib.sha256(_image()).hexdigest(), out)

    def test_status_error_when_unreachable(self):
        with unittest.mock.patch.object(uhc.http_auth, "urlopen", side_effect=urllib.error.URLError("down")):
            self.assertTrue(msu.update_status().startswith("error:"))


class UploadToolTest(_Base):
    def test_unconfirmed_is_dry_run_and_posts_nothing(self):
        path = self.write_image(_image())
        out = msu.update_stage_upload(path)
        self.assertIn("DRY RUN", out)
        self.assertIn(hashlib.sha256(_image()).hexdigest(), out)
        self.assertEqual(self.board.posts(), [])

    def test_truthy_but_not_true_is_refused_as_dry_run(self):
        path = self.write_image(_image())
        for val in ("yes", 1, "true"):
            out = msu.update_stage_upload(path, confirm=val)  # type: ignore[arg-type]
            self.assertIn("DRY RUN", out)
        self.assertEqual(self.board.posts(), [])

    def test_confirmed_upload_verified_by_readback(self):
        path = self.write_image(_image(8192))
        out = msu.update_stage_upload(path, version="1.4.0", confirm=True)
        self.assertTrue(out.startswith("ok - staged and verified"), out)
        self.assertIn("UNSIGNED", out)
        self.assertEqual(self.board.staged["semver"], "1.4.0")

    def test_sha_mismatch_on_readback_fails_loud(self):
        self.board.corrupt_sha = True
        out = msu.update_stage_upload(self.write_image(_image()), confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("sha256", out)

    def test_board_refusal_reported_with_name(self):
        self.board.refuse = (409, "update_in_progress")
        out = msu.update_stage_upload(self.write_image(_image()), confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("update_in_progress", out)

    def test_bad_version_400_explained_and_no_version_invented(self):
        self.board.refuse = (400, "bad_version")
        out = msu.update_stage_upload(self.write_image(_image()), confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("bad_version", out)
        self.assertIn("not valid semver", out)
        self.assertIn('version="x.y.z"', out)
        self.assertIn("Nothing was invented", out)
        # the single POST carried no version header: nothing was made up
        self.assertEqual(len(self.board.posts()), 1)
        self.assertIsNone(self.board.posts()[0].get_header("X-stage-version"))

    def test_bad_version_400_with_explicit_version_names_it(self):
        self.board.refuse = (400, "bad_version")
        out = msu.update_stage_upload(self.write_image(_image()), version="1.0", confirm=True)
        self.assertIn("'1.0'", out)
        self.assertIn('version="x.y.z"', out)

    def test_other_400_keeps_generic_message(self):
        self.board.refuse = (400, "bad_argument")
        out = msu.update_stage_upload(self.write_image(_image()), confirm=True)
        self.assertIn("bad_argument", out)
        self.assertNotIn("semver", out)

    def test_lost_reply_is_unknown_not_ok(self):
        self.board.drop_reply = True
        out = msu.update_stage_upload(self.write_image(_image()), confirm=True)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_busy_board_refused_before_posting(self):
        self.board.busy = True
        out = msu.update_stage_upload(self.write_image(_image()), confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertEqual(self.board.posts(), [])

    def test_local_refusals_make_no_request(self):
        self.assertTrue(msu.update_stage_upload("relative.bin", confirm=True).startswith("REFUSED"))
        self.assertTrue(msu.update_stage_upload(os.path.join(self.tmp.name, "nope.bin"),
                                                confirm=True).startswith("REFUSED"))
        self.assertTrue(msu.update_stage_upload(self.write_image(b"\x00" * 64),
                                                confirm=True).startswith("REFUSED"))
        self.assertTrue(msu.update_stage_upload(self.write_image(_image()), version="bad version",
                                                confirm=True).startswith("REFUSED"))
        self.assertEqual(self.board.requests, [])

    def test_oversize_refused(self):
        big = self.write_image(_image(CAPACITY + 1))
        self.assertTrue(msu.update_stage_upload(big, confirm=True).startswith("REFUSED"))
        self.assertEqual(self.board.posts(), [])

    def test_replaced_stage_is_noted(self):
        uhc.upload_stage("h", _image(4096, 1), version="1.0.0")
        out = msu.update_stage_upload(self.write_image(_image(4096, 2)), version="1.1.0", confirm=True)
        self.assertIn("previously staged image", out)
        self.assertIn("1.0.0", out)


class AckNoSafetyTest(_Base):
    def test_428_refused_without_ack_and_header_never_sent(self):
        self.board.needs_ack = True
        out = msu.update_stage_upload(self.write_image(_image()), confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("428", out)
        for r in self.board.posts():
            self.assertFalse(r.has_header("X-ota-ack-no-safety"))
        self.assertIsNone(self.board.staged)

    def test_ack_sent_only_when_exactly_true(self):
        self.board.needs_ack = True
        path = self.write_image(_image())
        for val in ("yes", 1):
            out = msu.update_stage_upload(path, confirm=True, ack_no_safety=val)  # type: ignore[arg-type]
            self.assertTrue(out.startswith("FAILED"), out)
        out = msu.update_stage_upload(path, confirm=True, ack_no_safety=True)
        self.assertTrue(out.startswith("ok - staged and verified"), out)

    def test_ack_does_not_bypass_confirm(self):
        self.board.needs_ack = True
        out = msu.update_stage_upload(self.write_image(_image()), ack_no_safety=True)
        self.assertIn("DRY RUN", out)
        self.assertEqual(self.board.posts(), [])

    def test_clear_428_then_ack(self):
        uhc.upload_stage("h", _image(), ack_no_safety=True)
        self.board.needs_ack = True
        self.assertTrue(msu.update_stage_clear(confirm=True).startswith("FAILED"))
        self.assertIsNotNone(self.board.staged)
        self.assertTrue(msu.update_stage_clear(confirm=True, ack_no_safety=True).startswith("ok - stage cleared"))


class ClearToolTest(_Base):
    def test_unconfirmed_sends_nothing(self):
        uhc.upload_stage("h", _image())
        n = len(self.board.posts())
        out = msu.update_stage_clear()
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertEqual(len(self.board.posts()), n)
        self.assertIsNotNone(self.board.staged)

    def test_confirmed_clear_verified(self):
        uhc.upload_stage("h", _image())
        out = msu.update_stage_clear(confirm=True)
        self.assertTrue(out.startswith("ok - stage cleared"), out)
        self.assertIsNone(self.board.staged)

    def test_clear_that_did_not_take_fails(self):
        uhc.upload_stage("h", _image())
        real = self.board.urlopen

        def lying(req, timeout=None, **kw):
            if req.get_method() == "POST" and req.full_url.endswith("/clear"):
                self.board.requests.append(req)
                return _Resp(b'{"ok":true,"staged":false}')  # claims success, changes nothing
            return real(req, timeout)

        with unittest.mock.patch.object(uhc.http_auth, "urlopen", lying):
            out = msu.update_stage_clear(confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_board_refusal_reported(self):
        self.board.refuse = (409, "update_in_progress")
        self.assertTrue(msu.update_stage_clear(confirm=True).startswith("FAILED"))


class NoCredentialTest(_Base):
    def test_results_never_carry_credentials(self):
        env = {"KILNCTL_WEB_USERNAME": "adminuser", "KILNCTL_WEB_PASSWORD": "s3cretpw"}
        with unittest.mock.patch.dict(os.environ, env):
            outs = [
                msu.update_status(),
                msu.update_stage_upload(self.write_image(_image()), confirm=True),
                msu.update_stage_clear(confirm=True),
            ]
        for out in outs:
            self.assertNotIn("s3cretpw", out)
            self.assertNotIn("adminuser", out)


class RegistrationTest(unittest.TestCase):
    def test_tools_registered_and_findable(self):
        from kilnctrl import mcp_server  # noqa: F401  (imports register every tool)
        from kilnctrl import mcp_facade
        for name in ("update_status", "update_stage_upload", "update_stage_clear"):
            self.assertIn(name, mcp_facade.GROUP_OVERRIDES)
            self.assertIn(name, mcp_facade.KEYWORDS)
        self.assertNotIn("update_apply", mcp_facade.GROUP_OVERRIDES)
        registered = set(mcp_server.registry.by_name)
        for name in ("update_status", "update_stage_upload", "update_stage_clear"):
            self.assertIn(name, registered)


if __name__ == "__main__":
    unittest.main()
