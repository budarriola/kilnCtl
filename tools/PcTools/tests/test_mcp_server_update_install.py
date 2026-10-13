"""update_install_staged against a fake board (patched update_http_client)."""
import unittest
from unittest import mock

from kilnctrl import mcp_server_update as msu
from kilnctrl import update_http_client as uhc


def _st(running="1.0.0", staged=True, sv="1.0.1", code="none", state="none", reason="none", busy=False):
    return {"running_version": running, "stage_busy": busy, "staged": staged, "staged_version": sv,
            "last_install": {"state": state, "code": code, "reason": reason, "version": sv}}


class InstallTests(unittest.TestCase):
    def setUp(self):
        for p in (mock.patch.object(msu, "_resolve_host", return_value="h"),
                  mock.patch.object(msu.time, "sleep", return_value=None)):
            p.start()
            self.addCleanup(p.stop)

    def test_unconfirmed_posts_nothing(self):
        with mock.patch.object(uhc, "get_install_status", return_value=_st()), \
             mock.patch.object(uhc, "start_install") as post:
            for v in (False, 1, "yes", None):
                self.assertTrue(msu.update_install_staged(confirm=v).startswith("REFUSED"))
            post.assert_not_called()

    def test_no_stage_refused(self):
        with mock.patch.object(uhc, "get_install_status", return_value=_st(staged=False)), \
             mock.patch.object(uhc, "start_install") as post:
            self.assertTrue(msu.update_install_staged(confirm=True).startswith("REFUSED"))
            post.assert_not_called()

    def test_post_refused_409(self):
        err = uhc.UpdateHttpError("x", 409, "busy")
        with mock.patch.object(uhc, "get_install_status", return_value=_st()), \
             mock.patch.object(uhc, "start_install", side_effect=err):
            self.assertTrue(msu.update_install_staged(confirm=True).startswith("FAILED"))

    def test_success_needs_version_and_ok(self):
        seq = [uhc.UpdateHttpError("down"), _st(running="1.0.0"),
               _st(running="1.0.1", state="recorded", code="ok", reason="none")]

        def get(host, timeout=8.0):
            if len(seq) == 0:
                return _st(running="1.0.1", state="recorded", code="ok")
            v = seq.pop(0)
            if isinstance(v, Exception):
                raise v
            return v
        calls = {"n": 0}
        first = _st()

        def getter(host, timeout=8.0):
            calls["n"] += 1
            return first if calls["n"] == 1 else get(host, timeout)
        with mock.patch.object(uhc, "get_install_status", side_effect=getter), \
             mock.patch.object(uhc, "start_install", return_value={}):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("ok"), out)

    def test_recorded_failure_is_failed(self):
        seq = [_st(), _st(running="1.0.0", state="recorded", code="fail", reason="apply_failed")]
        with mock.patch.object(uhc, "get_install_status", side_effect=seq), \
             mock.patch.object(uhc, "start_install", return_value={}):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertIn("apply_failed", out)

    def test_running_version_alone_is_not_success(self):
        # version matches but no OK record: keep waiting, then UNKNOWN.
        s = _st(running="1.0.1", state="none")
        t = iter([0.0, 0.0, 1.0, 400.0, 400.0, 400.0])
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()] + [s] * 10), \
             mock.patch.object(uhc, "start_install", return_value={}), \
             mock.patch.object(msu.time, "monotonic", side_effect=lambda: next(t)):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("UNKNOWN"), out)


if __name__ == "__main__":
    unittest.main()


class InstallBudgetTests(unittest.TestCase):
    def setUp(self):
        for p in (mock.patch.object(msu, "_resolve_host", return_value="h"),
                  mock.patch.object(msu.time, "sleep", return_value=None)):
            p.start()
            self.addCleanup(p.stop)

    def test_post_timeout_is_unknown_then_follow_up_decides(self):
        err = uhc.UpdateHttpError("timed out")  # status None: transport failure
        seq = [_st(), uhc.UpdateHttpError("down"), _st(running="1.0.1", state="recorded", code="ok")]
        with mock.patch.object(uhc, "get_install_status", side_effect=seq),              mock.patch.object(uhc, "start_install", side_effect=err):
            self.assertTrue(msu.update_install_staged(confirm=True).startswith("ok"))

    def test_post_timeout_never_confirmed_is_unknown_not_failed(self):
        err = uhc.UpdateHttpError("timed out")
        clock = iter([0.0, 1.0, 400.0, 400.0, 400.0, 400.0])
        with mock.patch.object(uhc, "get_install_status", return_value=_st()),              mock.patch.object(uhc, "start_install", side_effect=err),              mock.patch.object(msu.time, "monotonic", side_effect=lambda: next(clock)):
            out = msu.update_install_staged(confirm=True, wait_s=9999)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertIn("possibly applied", out)
        self.assertIn("poll with", out)

    def test_wait_is_capped_below_client_limit(self):
        # deadline = t0 + 270 even for a huge wait_s: a poll at t=271 must not happen.
        times = iter([0.0, 5.0, 271.0, 271.0, 271.0])
        with mock.patch.object(uhc, "get_install_status", return_value=_st()) as g,              mock.patch.object(uhc, "start_install"),              mock.patch.object(msu.time, "monotonic", side_effect=lambda: next(times)):
            out = msu.update_install_staged(confirm=True, wait_s=9999)
        self.assertTrue(out.startswith("UNKNOWN"))
        self.assertEqual(g.call_count, 1)  # only the initial GET

    def test_refusal_with_status_still_failed(self):
        with mock.patch.object(uhc, "get_install_status", return_value=_st()),              mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("x", 409, "busy")):
            self.assertTrue(msu.update_install_staged(confirm=True).startswith("FAILED"))

    def test_stale_ok_after_post_transport_failure_is_not_success(self):
        # Same-version reinstall: board already runs 1.0.1 with a recorded ok. POST gets no answer and the
        # board never goes unreachable -> the ok is stale evidence, must not be reported as installed.
        stale = _st(running="1.0.1", sv="1.0.1", state="recorded", code="ok")
        err = uhc.UpdateHttpError("connection refused")
        clock = iter([0.0, 1.0, 2.0, 3.0, 400.0, 400.0, 400.0, 400.0])
        with mock.patch.object(uhc, "get_install_status", return_value=stale),              mock.patch.object(uhc, "start_install", side_effect=err),              mock.patch.object(msu.time, "monotonic", side_effect=lambda: next(clock)):
            out = msu.update_install_staged(confirm=True, wait_s=9999)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertIn("stale", out)

    def test_ok_after_post_failure_accepted_once_board_went_down(self):
        stale = _st(running="1.0.1", sv="1.0.1", state="recorded", code="ok")
        err = uhc.UpdateHttpError("connection refused")
        seq = [stale, uhc.UpdateHttpError("down"), stale]
        with mock.patch.object(uhc, "get_install_status", side_effect=seq),              mock.patch.object(uhc, "start_install", side_effect=err):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("ok"), out)

    def test_auth_refusal_without_status_is_failed_and_does_not_poll(self):
        try:
            try:
                raise msu.http_auth.HttpAuthError("relogin refused")
            except msu.http_auth.HttpAuthError as inner:
                raise uhc.UpdateHttpError("POST failed: relogin refused") from inner
        except uhc.UpdateHttpError as err:
            auth_err = err
        with mock.patch.object(uhc, "get_install_status", return_value=_st()) as g,              mock.patch.object(uhc, "start_install", side_effect=auth_err):
            out = msu.update_install_staged(confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertNotIn("possibly applied", out)
        self.assertEqual(g.call_count, 1)  # initial GET only, no polling

