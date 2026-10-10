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
import urllib.parse

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
                body = {"ok": False, "error": self.refuse[1]}
                if len(self.refuse) > 2:
                    body.update(self.refuse[2])
                raise self._http_error(req, self.refuse[0], body)
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


class DowngradeGateTest(_Base):
    def test_override_headers_sent_only_when_asked(self):
        uhc.upload_stage("h", _image(), version="1.0.0")
        req = self.board.posts()[0]
        for h in ("X-stage-force", "X-stage-allow-downgrade", "X-stage-confirm"):
            self.assertFalse(req.has_header(h), h)
        uhc.upload_stage("h", _image(), version="1.0.0", force=True, allow_downgrade=True,
                         confirm_downgrade="1.0.0")
        req = self.board.posts()[1]
        self.assertEqual(req.get_header("X-stage-force"), "1")
        self.assertEqual(req.get_header("X-stage-allow-downgrade"), "1")
        self.assertEqual(req.get_header("X-stage-confirm"), "1.0.0")

    def test_tool_passes_overrides_through(self):
        path = self.write_image(_image())
        out = msu.update_stage_upload(path, version="1.0.0", confirm=True, allow_downgrade=True,
                                      confirm_downgrade="1.0.0", force=True)
        self.assertTrue(out.startswith("ok"), out)
        req = self.board.posts()[0]
        self.assertEqual(req.get_header("X-stage-allow-downgrade"), "1")
        self.assertEqual(req.get_header("X-stage-confirm"), "1.0.0")
        self.assertEqual(req.get_header("X-stage-force"), "1")

    def test_truthy_overrides_are_not_sent(self):
        path = self.write_image(_image())
        msu.update_stage_upload(path, version="1.0.0", confirm=True, force="yes", allow_downgrade=1)  # type: ignore[arg-type]
        req = self.board.posts()[0]
        self.assertFalse(req.has_header("X-stage-force"))
        self.assertFalse(req.has_header("X-stage-allow-downgrade"))

    def test_allow_downgrade_without_typed_confirm_refused_locally(self):
        out = msu.update_stage_upload(self.write_image(_image()), confirm=True, allow_downgrade=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertIn("confirm_downgrade", out)
        self.assertEqual(self.board.posts(), [])

    def test_gate_409_reports_hazard_and_override(self):
        self.board.refuse = (409, "downgrade_refused",
                             {"reason": "candidate is older than the running version; downgrade refused",
                              "needs_typed_confirm": True})
        out = msu.update_stage_upload(self.write_image(_image()), version="0.9.0", confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("downgrade_refused", out)
        self.assertIn("older than the running version", out)
        self.assertIn("control_get_zones", out)
        self.assertIn("allow_downgrade=True", out)

    def test_needs_force_409_reported(self):
        self.board.refuse = (409, "needs_force", {"reason": "already up to date"})
        out = msu.update_stage_upload(self.write_image(_image()), version="1.0.0", confirm=True)
        self.assertIn("force=True", out)
        self.assertIn("already up to date", out)

    def test_needs_force_with_typed_confirm_names_versions(self):
        self.board.refuse = (409, "needs_force",
                             {"reason": "image carries no schema identity record", "needs_typed_confirm": True,
                              "candidate_version": "1.3.0", "running_version": "1.2.0"})
        out = msu.update_stage_upload(self.write_image(_image()), version="1.3.0", confirm=True)
        self.assertIn("confirm_downgrade=1.3.0", out)
        self.assertIn("candidate 1.3.0, running 1.2.0", out)
        self.assertIn("force=True", out)

    def test_version_mismatch_409_reported(self):
        self.board.refuse = (409, "version_mismatch", {})
        out = msu.update_stage_upload(self.write_image(_image()), version="99.0.0", confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("differs from the version inside the image", out)


class WedgeShownTest(_Base):
    """Review 7 L1: fetch_writer_wedged is surfaced by update_status and update_fetch_status."""

    _STAGE = {"phase": "uploading", "busy": True, "staged": False, "header": "blank",
              "reason": "writer_wedged_reboot_required", "capacity": 1, "fetch_writer_wedged": True}

    def test_update_status_shows_wedge(self):
        with unittest.mock.patch.object(uhc, "get_stage_status", return_value=dict(self._STAGE)):
            out = msu.update_status()
        self.assertIn("fetch_writer_wedged=True", out)
        self.assertIn("reboot required", out)
        with unittest.mock.patch.object(uhc, "get_stage_status",
                                        return_value=dict(self._STAGE, fetch_writer_wedged=False)):
            self.assertIn("fetch_writer_wedged=False", msu.update_status())

    def test_update_fetch_status_shows_wedge(self):
        fetch = {"state": "failed", "kind": "download", "repo": "a/b", "busy": False}
        with unittest.mock.patch.object(uhc, "get_fetch_status", return_value=fetch),                 unittest.mock.patch.object(uhc, "get_stage_status", return_value=dict(self._STAGE)):
            out = msu.update_fetch_status()
        self.assertIn("fetch_writer_wedged=True", out)


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
        self.assertNotIn("UNSIGNED", out)
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


class GhBoard(FakeBoard):
    """FakeBoard plus the WP8/WP9 routes: check, download, fetch status, settings."""

    DEFAULT = "budarriola/kilnCtl"

    def __init__(self):
        super().__init__()
        self.repo = self.DEFAULT
        self.job = None            # None | "check" | "download"
        self.polls_busy = 0        # GET /update/fetch answers busy this many times first
        self.last = {}             # fields overriding the finished-job status
        self.verdict = ("allow_upgrade", True)
        self.release_sha = "ab" * 32
        self.release_len = 4096
        self.stage_corrupt = False
        self.refuse_gh = None      # (status, name) for check/download
        self.settings_ignored = False

    def _fetch(self) -> dict:
        if self.job is not None and self.polls_busy > 0:
            self.polls_busy -= 1
            return {"ok": True, "state": "downloading", "kind": self.job, "busy": True, "repo": self.repo,
                    "bytes_done": 10, "bytes_total": 100}
        if self.job is None:
            return {"ok": True, "state": "idle", "kind": "none", "busy": False, "repo": self.repo}
        d = {"ok": True, "state": "done", "kind": self.job, "busy": False, "repo": self.repo,
             "tag": "v1.0.1", "prerelease": False, "app_size": self.release_len, "running": "1.0.0",
             "sha256": self.release_sha, "verdict": self.verdict[0], "allowed": self.verdict[1],
             "needs_typed_confirm": False, "zones_cfg_lower": False}
        d.update(self.last)
        return d

    def urlopen(self, req, timeout=None, **kw):
        path = req.full_url.split("/api", 1)[1]
        m = req.get_method()
        base = path.split("?", 1)[0]
        if base in ("/update/fetch", "/update/check", "/update/download", "/update/settings"):
            self.requests.append(req)
            if m == "GET" and base == "/update/fetch":
                return _Resp(json.dumps(self._fetch()).encode())
            if m == "GET" and base == "/update/settings":
                return _Resp(json.dumps({"ok": True, "repo": self.repo, "default_repo": self.DEFAULT,
                                         "is_default": self.repo == self.DEFAULT}).encode())
            if m == "POST" and base == "/update/settings":
                body = urllib.parse.parse_qs(req.data.decode())
                if not self.settings_ignored:
                    self.repo = (body.get("repo") or [""])[0] or self.DEFAULT
                return _Resp(b'{"ok":true}')
            if m == "POST" and base in ("/update/check", "/update/download"):
                if self.needs_ack and not req.has_header("X-ota-ack-no-safety"):
                    raise self._http_error(req, 428, {"ok": False, "error": "safety_not_answering"})
                if self.refuse_gh:
                    raise self._http_error(req, self.refuse_gh[0], {"ok": False, "error": self.refuse_gh[1]})
                self.job = "check" if base == "/update/check" else "download"
                if self.job == "download":
                    self.staged = {"len": self.release_len,
                                   "sha": "0" * 64 if self.stage_corrupt else self.release_sha,
                                   "semver": "1.0.1", "commit": "c" * 40, "source": self.stage_source}
                return _Resp(b'{"ok":true,"started":true}')
        return super().urlopen(req, timeout, **kw)

    stage_source = 2

    def _status(self) -> dict:
        d = super()._status()
        if self.staged:
            d["source"] = self.staged.get("source", 1)
        return d


class GhBase(_Base):
    def setUp(self):
        super().setUp()
        self.board = GhBoard()
        p = unittest.mock.patch.object(uhc.http_auth, "urlopen", self.board.urlopen)
        p.start()
        self.addCleanup(p.stop)
        q = unittest.mock.patch.object(msu, "_POLL_S", 0)
        q.start()
        self.addCleanup(q.stop)


class GhCheckTest(GhBase):
    def test_check_reports_verdict_and_sha(self):
        self.board.polls_busy = 2
        out = msu.update_check()
        self.assertTrue(out.startswith("ok"), out)
        for s in ("v1.0.1", "allow_upgrade", self.board.release_sha):
            self.assertIn(s, out)
        self.assertIsNone(self.board.staged)

    def test_check_refusal_names_error(self):
        self.board.refuse_gh = (409, "clock_not_synced")
        out = msu.update_check()
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertIn("clock_not_synced", out)

    def test_check_failed_job_not_ok(self):
        self.board.job = "check"
        self.board.last = {"state": "failed", "error": "connect_failed"}
        out = msu.update_check()
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("connect_failed", out)

    def test_check_wait_expiry_is_unknown(self):
        self.board.polls_busy = 10 ** 6
        out = msu.update_check(wait_s=0)
        self.assertTrue(out.startswith("UNKNOWN"), out)


class UpdateCheckClassificationTest(unittest.TestCase):
    def test_update_check_documented_as_not_read_only(self):
        # update_check POSTs /api/update/check and starts a board job; the module
        # header and tool doc must not call it read-only (review 2026-10-10 MED).
        head = msu.__doc__.split("Mutating:")[0]
        read_only_line = [l for l in head.splitlines() if l.startswith("Read-only:")][0]
        self.assertNotIn("update_check", read_only_line)
        self.assertIn("NOT read-only", msu.update_check.__doc__)
        import inspect
        self.assertIn("start_check", inspect.getsource(msu.update_check))


class WaitClampTest(unittest.TestCase):
    def test_wait_job_clamped(self):
        clock = [0.0]
        with unittest.mock.patch.object(msu.time, "monotonic", lambda: clock[0]),                 unittest.mock.patch.object(msu.time, "sleep", lambda s: clock.__setitem__(0, clock[0] + 30.0)),                 unittest.mock.patch.object(msu.uhc, "get_fetch_status", lambda h: {"busy": True}):
            st, err = msu._wait_job("h", 10 ** 6)
        self.assertIsNotNone(err)
        self.assertLessEqual(clock[0], 270.0)


class GhStageReleaseTest(GhBase):
    def test_dry_run_posts_nothing(self):
        for val in (False, 1, "yes", None):
            out = msu.update_stage_release(confirm=val, force=True)  # type: ignore[arg-type]
            self.assertTrue(out.startswith("DRY RUN"), out)
            self.assertIn("force=1", out)
        self.assertEqual(self.board.posts(), [])

    def test_confirmed_download_verified_by_readback(self):
        self.board.polls_busy = 1
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("ok - release v1.0.1 staged and verified"), out)
        self.assertNotIn("UNSIGNED", out)
        self.assertEqual(len(self.board.posts()), 1)

    def test_query_carries_flags_and_typed_tag(self):
        msu.update_stage_release(confirm=True, allow_downgrade=True, confirm_downgrade="v0.9.0",
                                 allow_prerelease=True)
        url = self.board.posts()[0].full_url
        self.assertIn("allow_prerelease=1", url)
        self.assertIn("allow_downgrade=1&confirm_downgrade=v0.9.0", url)

    def test_stage_sha_mismatch_fails_loud(self):
        self.board.stage_corrupt = True
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("sha256", out)

    def test_stage_wrong_source_fails(self):
        self.board.stage_source = 1
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("expected github", out)

    def test_refusal_409_final(self):
        self.board.refuse_gh = (409, "heat_run_active")
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertIn("heat_run_active", out)

    def test_in_flight_refused_before_post(self):
        self.board.busy = True
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertEqual(self.board.posts(), [])

    def test_428_needs_exact_ack(self):
        self.board.needs_ack = True
        self.assertTrue(msu.update_stage_release(confirm=True, ack_no_safety=1).startswith("REFUSED"))  # type: ignore[arg-type]
        self.assertTrue(msu.update_stage_release(confirm=True, ack_no_safety=True).startswith("ok"))
        self.assertTrue(msu.update_stage_release(ack_no_safety=True).startswith("DRY RUN"))

    def test_replaced_stage_noted(self):
        uhc.upload_stage("h", _image(), version="1.0.0")
        out = msu.update_stage_release(confirm=True)
        self.assertIn("previously staged image", out)


class GhSettingsTest(GhBase):
    def test_get(self):
        out = msu.update_get_settings()
        self.assertIn("repo=budarriola/kilnCtl", out)
        self.assertIn("is_default=True", out)

    def test_set_needs_exact_confirm(self):
        for val in (False, 1, "yes"):
            out = msu.update_set_settings("a/b", confirm=val)  # type: ignore[arg-type]
            self.assertTrue(out.startswith("REFUSED"), out)
        self.assertEqual(self.board.posts(), [])
        self.assertEqual(self.board.repo, GhBoard.DEFAULT)

    def test_set_verified_by_readback(self):
        out = msu.update_set_settings("someone/else", confirm=True)
        self.assertTrue(out.startswith("ok"), out)
        self.assertEqual(self.board.repo, "someone/else")

    def test_empty_resets_default(self):
        self.board.repo = "x/y"
        out = msu.update_set_settings("", confirm=True)
        self.assertTrue(out.startswith("ok"), out)
        self.assertEqual(self.board.repo, GhBoard.DEFAULT)

    def test_ignored_write_fails_loud(self):
        self.board.settings_ignored = True
        out = msu.update_set_settings("someone/else", confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_board_400_reported(self):
        orig = self.board.urlopen

        def bad(req, timeout=None, **kw):
            if req.get_method() == "POST" and req.full_url.endswith("/update/settings"):
                raise self.board._http_error(req, 400, {"ok": False, "error": "bad_repo"})
            return orig(req, timeout, **kw)
        with unittest.mock.patch.object(uhc.http_auth, "urlopen", bad):
            out = msu.update_set_settings("nonsense", confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("bad_repo", out)


class GhRegistrationTest(unittest.TestCase):
    def test_registered_and_findable(self):
        from kilnctrl import mcp_server
        from kilnctrl import mcp_facade
        for name in ("update_check", "update_stage_release", "update_get_settings", "update_set_settings"):
            self.assertIn(name, mcp_facade.GROUP_OVERRIDES)
            self.assertIn(name, mcp_facade.KEYWORDS)
            self.assertIn(name, set(mcp_server.registry.by_name))


if __name__ == "__main__":
    unittest.main()
