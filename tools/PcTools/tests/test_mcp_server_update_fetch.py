#!/usr/bin/env python3
"""Unit tests for the GitHub-fetch MCP tools (update_check, update_stage_release,
update_fetch_status, update_fetch_cancel) against a FAKE BOARD patched in at
http_auth.urlopen(): a small stateful stand-in for update_fetch.c's four routes
plus update_http.c's stage status. No socket, no real board, no sleeping.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_update_fetch.py -q
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.dirname(__file__))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_update as msu  # noqa: E402
from kilnctrl import update_http_client as uhc  # noqa: E402
from test_mcp_server_update import FakeBoard, _Resp, _image  # noqa: E402

RELEASE_IMAGE = _image(8192, fill=0x33)
RELEASE_SHA = hashlib.sha256(RELEASE_IMAGE).hexdigest()


class FetchFakeBoard(FakeBoard):
    """Adds the update_fetch.c routes. A job takes `polls_to_finish` status
    reads to leave busy (0 = already finished by the first read).
    `fail_error` makes the job end failed; `skip_stage` makes a download
    report done without staging; `stage_sha` overrides the staged hash."""

    def __init__(self):
        super().__init__()
        self.job = None  # dict while a job is pending/finished
        self.polls_to_finish = 1
        self.fail_error = ""
        self.skip_stage = False
        self.stage_sha = None
        self.start_refuse = None  # (status, name) for check/download POST
        self.cancel_replies_cancelling = True
        self.last_query = ""

    def _fetch_status(self) -> dict:
        j = self.job
        st = {"ok": True, "state": "idle", "kind": "check", "stage": "", "error": "", "http_status": 0,
              "bytes_done": 0, "bytes_total": 0, "busy": False, "repo": "owner/kiln", "unsigned": True,
              "tag": "", "prerelease": False, "app_size": 0, "running": "", "commit": "", "sha256": "",
              "verdict": "", "reason": "", "allowed": False, "needs_typed_confirm": False,
              "zones_cfg_lower": False}
        if j is None:
            return st
        finished = j["polls"] >= self.polls_to_finish
        st.update(kind=j["kind"], tag="v1.5.0", app_size=len(RELEASE_IMAGE), running="1.4.0",
                  sha256=RELEASE_SHA, verdict="upgrade", allowed=True)
        if finished:
            st["state"] = "failed" if self.fail_error else "done"
            st["error"] = self.fail_error
            st["http_status"] = 404 if self.fail_error else 200
            st["bytes_done"] = st["bytes_total"] = len(RELEASE_IMAGE) if j["kind"] == "download" else 0
        else:
            st.update(state="downloading" if j["kind"] == "download" else "checking", busy=True,
                      stage="body", bytes_done=100, bytes_total=len(RELEASE_IMAGE))
        return st

    def urlopen(self, req, timeout=None, **kw):
        path = req.full_url.split("/api", 1)[1]
        m = req.get_method()
        route = path.split("?", 1)[0]
        if route not in ("/update/check", "/update/download", "/update/fetch", "/update/fetch/cancel"):
            return super().urlopen(req, timeout=timeout, **kw)
        self.requests.append(req)
        if m == "GET" and route == "/update/fetch":
            if self.job is not None:
                self.job["polls"] += 1
            st = self._fetch_status()
            if self.job is not None and not st["busy"] and self.job["kind"] == "download" \
                    and not self.fail_error and not self.skip_stage and not self.job["staged"]:
                self.job["staged"] = True
                self.staged = {"len": len(RELEASE_IMAGE), "sha": self.stage_sha or RELEASE_SHA,
                               "semver": "1.5.0", "commit": "", "source": 2}
            return _Resp(json.dumps(st).encode())
        if m == "POST" and route in ("/update/check", "/update/download"):
            if self.start_refuse:
                raise self._http_error(req, self.start_refuse[0], {"ok": False, "error": self.start_refuse[1]})
            self.last_query = path.partition("?")[2]
            self.job = {"kind": "check" if route == "/update/check" else "download", "polls": 0,
                        "staged": False}
            return _Resp(b'{"ok":true,"started":true}')
        if m == "POST" and route == "/update/fetch/cancel":
            busy = self.job is not None and self.job["polls"] < self.polls_to_finish
            return _Resp(json.dumps({"ok": True, "cancelling": busy and self.cancel_replies_cancelling}).encode())
        raise AssertionError(f"unexpected request {m} {path}")

    def _status(self) -> dict:
        d = super()._status()
        if self.staged:
            d["source"] = self.staged.get("source", 1)
        return d

    def posts_to(self, route):
        return [r for r in self.posts() if r.full_url.split("/api", 1)[1].split("?", 1)[0] == route]


class _Base(unittest.TestCase):
    def setUp(self):
        self.board = FetchFakeBoard()
        for p in (
            unittest.mock.patch.object(uhc.http_auth, "urlopen", self.board.urlopen),
            unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"),
            unittest.mock.patch.object(msu.time, "sleep"),
        ):
            p.start()
            self.addCleanup(p.stop)


class ClientTest(_Base):
    def test_download_query_overrides(self):
        uhc.start_download("h", allow_prerelease=True, allow_downgrade=True,
                           confirm_downgrade="v1.0.0", force=True)
        self.assertEqual(self.board.last_query,
                         "allow_prerelease=1&force=1&allow_downgrade=1&confirm_downgrade=v1.0.0")
        uhc.start_download("h")
        self.assertEqual(self.board.last_query, "")

    def test_force_carries_typed_tag_without_downgrade(self):
        # A dev build (running version unknown) needs force=1 plus the typed tag; the
        # tag must reach the board even when allow_downgrade is not set.
        uhc.start_download("h", allow_prerelease=True, force=True, confirm_downgrade="v1.0.0-pre.1")
        self.assertEqual(self.board.last_query,
                         "allow_prerelease=1&force=1&confirm_downgrade=v1.0.0-pre.1")
        uhc.start_download("h", confirm_downgrade="v1.0.0")
        self.assertEqual(self.board.last_query, "", "a typed tag alone is never sent")

    def test_start_check_prerelease_flag(self):
        uhc.start_check("h", allow_prerelease=True)
        self.assertEqual(self.board.last_query, "allow_prerelease=1")
        uhc.start_check("h")
        self.assertEqual(self.board.last_query, "")

    def test_confirm_downgrade_is_url_quoted(self):
        uhc.start_download("h", allow_downgrade=True, confirm_downgrade="v1&force=1")
        self.assertNotIn("&force=1", self.board.last_query.split("confirm_downgrade=")[1])


class StatusTest(_Base):
    def test_idle_status_is_read_only(self):
        out = msu.update_fetch_status()
        self.assertTrue(out.startswith("ok - state=idle"), out)
        self.assertIn("UNSIGNED", out)
        self.assertEqual(self.board.posts(), [])

    def test_unreachable(self):
        with unittest.mock.patch.object(uhc.http_auth, "urlopen", side_effect=urllib.error.URLError("down")):
            self.assertTrue(msu.update_fetch_status().startswith("error:"))


class CheckTest(_Base):
    def test_check_polls_and_reports_release_without_touching_stage(self):
        self.board.polls_to_finish = 3
        out = msu.update_check()
        self.assertTrue(out.startswith("ok - state=done"), out)
        for needle in ("tag=v1.5.0", "running=1.4.0", RELEASE_SHA, "verdict=upgrade", "allowed=True"):
            self.assertIn(needle, out)
        self.assertEqual(len(self.board.posts_to("/update/check")), 1)
        self.assertEqual(self.board.posts_to("/update/stage"), [])
        self.assertIsNone(self.board.staged)

    def test_board_refusal_reported(self):
        self.board.start_refuse = (409, "clock_not_synced")
        out = msu.update_check()
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertIn("clock_not_synced", out)

    def test_failed_job_reported(self):
        self.board.fail_error = "http_status"
        out = msu.update_check()
        self.assertTrue(out.startswith("FAILED"))
        self.assertIn("http_status=404", out)
        self.assertIn("no release published", out)

    def test_timeout_is_unknown(self):
        self.board.polls_to_finish = 10 ** 6
        with unittest.mock.patch.object(msu.time, "monotonic", side_effect=[0.0, 0.0, 1000.0]):
            out = msu.update_check(wait_s=5.0)
        self.assertTrue(out.startswith("UNKNOWN"), out)


class DownloadTest(_Base):
    def test_unconfirmed_sends_nothing(self):
        for val in (False, "yes", 1, "true"):
            out = msu.update_stage_release(confirm=val)  # type: ignore[arg-type]
            self.assertTrue(out.startswith("DRY RUN"), out)
        self.assertEqual(self.board.posts(), [])

    def test_refused_when_job_already_busy(self):
        self.board.polls_to_finish = 5
        self.board.job = {"kind": "download", "polls": 0, "staged": False}
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertEqual(self.board.posts(), [])

    def test_confirmed_download_verified_by_stage_readback(self):
        self.board.polls_to_finish = 3
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("ok - release v1.5.0 staged and verified"), out)
        self.assertIn(RELEASE_SHA, out)
        self.assertIn("UNSIGNED", out)
        self.assertEqual(self.board.last_query, "")

    def test_overrides_passed_through(self):
        msu.update_stage_release(confirm=True, allow_downgrade=True, confirm_downgrade="v1.0.0")
        self.assertEqual(self.board.last_query, "allow_downgrade=1&confirm_downgrade=v1.0.0")

    def test_bench_prerelease_flow_query(self):
        # The owner's bench flow: a pre-release onto a dev build, force + typed tag.
        out = msu.update_stage_release(confirm=True, allow_prerelease=True, force=True,
                                       confirm_downgrade="v1.0.0-pre.1")
        self.assertTrue(out.startswith("ok - release"), out)
        self.assertEqual(self.board.last_query, "allow_prerelease=1&force=1&confirm_downgrade=v1.0.0-pre.1")

    def test_update_check_prerelease_passed_through(self):
        msu.update_check(allow_prerelease=True)
        self.assertEqual(self.board.last_query, "allow_prerelease=1")
        msu.update_check()
        self.assertEqual(self.board.last_query, "")

    def test_stage_sha_mismatch_fails_loud(self):
        self.board.stage_sha = "0" * 64
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("sha256", out)

    def test_done_but_nothing_staged_fails_loud(self):
        self.board.skip_stage = True
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("staged", out)

    def test_failed_job_reported(self):
        self.board.fail_error = "sha256_mismatch"
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("sha256_mismatch", out)

    def test_board_start_refusal_reported(self):
        self.board.start_refuse = (409, "system_mode_gate")
        out = msu.update_stage_release(confirm=True)
        self.assertTrue(out.startswith("REFUSED"), out)
        self.assertIn("system_mode_gate", out)

    def test_poll_timeout_is_unknown_not_failure(self):
        self.board.polls_to_finish = 10 ** 6
        with unittest.mock.patch.object(msu.time, "monotonic", side_effect=[0.0, 0.0, 1000.0]):
            out = msu.update_stage_release(confirm=True, wait_s=5.0)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertIn("update_fetch_cancel", out)


class CancelTest(_Base):
    def test_idle_needs_no_cancel(self):
        out = msu.update_fetch_cancel(confirm=True)
        self.assertIn("nothing to cancel", out)
        self.assertEqual(self.board.posts(), [])

    def test_unconfirmed_sends_nothing(self):
        self.board.polls_to_finish = 5
        self.board.job = {"kind": "download", "polls": 0, "staged": False}
        for val in (False, "yes", 1):
            self.assertTrue(msu.update_fetch_cancel(confirm=val).startswith("REFUSED"))  # type: ignore[arg-type]
        self.assertEqual(self.board.posts(), [])

    def test_confirmed_cancel_posts(self):
        self.board.polls_to_finish = 5
        self.board.job = {"kind": "download", "polls": 0, "staged": False}
        out = msu.update_fetch_cancel(confirm=True)
        self.assertTrue(out.startswith("ok - cancel requested"), out)
        self.assertEqual(len(self.board.posts_to("/update/fetch/cancel")), 1)


class NoCredentialTest(_Base):
    def test_results_never_carry_credentials(self):
        env = {"KILNCTL_WEB_USERNAME": "adminuser", "KILNCTL_WEB_PASSWORD": "s3cretpw"}
        with unittest.mock.patch.dict(os.environ, env):
            outs = [msu.update_fetch_status(), msu.update_check(), msu.update_stage_release(confirm=True),
                    msu.update_fetch_cancel(confirm=True)]
        for out in outs:
            self.assertNotIn("s3cretpw", out)
            self.assertNotIn("adminuser", out)


class RegistrationTest(unittest.TestCase):
    def test_tools_registered_and_findable(self):
        from kilnctrl import mcp_server  # noqa: F401
        from kilnctrl import mcp_facade
        names = ("update_check", "update_stage_release", "update_fetch_status", "update_fetch_cancel")
        registered = set(mcp_server.registry.by_name)
        for name in names:
            self.assertIn(name, mcp_facade.GROUP_OVERRIDES)
            self.assertIn(name, mcp_facade.KEYWORDS)
            self.assertIn(name, registered)


if __name__ == "__main__":
    unittest.main()
