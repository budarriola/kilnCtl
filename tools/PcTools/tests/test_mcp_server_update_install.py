"""update_install_staged against a fake board (patched update_http_client)."""
import unittest
from unittest import mock

from kilnctrl import dashboard_http_client as dhc
from kilnctrl import mcp_server_update as msu
from kilnctrl import update_http_client as uhc


def _st(running="1.0.0", staged=True, sv="1.0.1", code="none", state="none", reason="none", busy=False):
    return {"running_version": running, "stage_busy": busy, "staged": staged, "staged_version": sv,
            "last_install": {"state": state, "code": code, "reason": reason, "version": sv}}


def _ok(running="1.0.1", sv="1.0.1"):
    return _st(running=running, sv=sv, state="recorded", code="ok")


class FakeClock:
    """time.monotonic()/sleep() on a virtual clock; sleep advances it."""

    def __init__(self):
        self.t = 1000.0

    def monotonic(self):
        self.t += 0.01
        return self.t

    def sleep(self, s):
        self.t += s


class _Base(unittest.TestCase):
    def setUp(self):
        self.clock = FakeClock()
        # Board boot instant on the virtual clock: uptime_s = clock - boot_t. Moving it forward = reboot.
        self.boot_t = self.clock.t - 600.0
        self.uptime_readable = True
        msu._PENDING_INSTALL.clear()
        self.addCleanup(msu._PENDING_INSTALL.clear)
        for p in (mock.patch.object(msu, "_resolve_host", return_value="h"),
                  mock.patch.object(msu.time, "sleep", side_effect=self.clock.sleep),
                  mock.patch.object(msu.time, "monotonic", side_effect=self.clock.monotonic),
                  mock.patch.object(dhc, "get_status", side_effect=self._status)):
            p.start()
            self.addCleanup(p.stop)

    def _status(self, host, timeout=8.0):
        if not self.uptime_readable:
            raise dhc.DashboardHttpError("GET /api/status failed")
        return {"uptime_s": int(self.clock.t - self.boot_t)}

    def reboot(self):
        self.boot_t = self.clock.t - 20.0


def _poll_errors():
    """The three kinds of failed poll that must not count as proof of a reboot."""
    auth = uhc.UpdateHttpError("GET failed: relogin refused")
    auth.__cause__ = msu.http_auth.HttpAuthError("relogin refused")
    return {"outage": uhc.UpdateHttpError("connection refused"),
            "http_503": uhc.UpdateHttpError("busy", 503, "busy"),
            "auth": auth}


class InstallTests(_Base):
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
        seq = [_st(), uhc.UpdateHttpError("down"), _st(running="1.0.0"), _ok()]
        with mock.patch.object(uhc, "get_install_status", side_effect=seq), \
             mock.patch.object(uhc, "start_install", return_value={}):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("ok"), out)

    def test_answered_post_is_accepted_without_a_down_poll(self):
        # Mutant P3: a POST that got an answer must not need an unreachable poll or an uptime proof.
        self.uptime_readable = False
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st(), _ok()]), \
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
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()] + [s] * 100), \
             mock.patch.object(uhc, "start_install", return_value={}):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("UNKNOWN"), out)


class InstallBudgetTests(_Base):
    def test_post_timeout_different_version_is_ok_once_new_version_runs(self):
        err = uhc.UpdateHttpError("timed out")  # status None: transport failure
        seq = [_st(), uhc.UpdateHttpError("down"), _ok()]
        with mock.patch.object(uhc, "get_install_status", side_effect=seq), \
             mock.patch.object(uhc, "start_install", side_effect=err):
            self.assertTrue(msu.update_install_staged(confirm=True).startswith("ok"))

    def test_post_timeout_never_confirmed_is_unknown_not_failed(self):
        err = uhc.UpdateHttpError("timed out")
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()] + [uhc.UpdateHttpError("down")] * 200), \
             mock.patch.object(uhc, "start_install", side_effect=err):
            out = msu.update_install_staged(confirm=True, wait_s=9999)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertIn("possibly applied", out)
        self.assertIn("may still be running", out)
        self.assertIn("update_install_staged(confirm=False)", out)

    def test_wait_is_capped_below_client_limit(self):
        t0 = self.clock.t
        seen = []

        def get(host, timeout=8.0):
            seen.append(self.clock.t - t0)
            return _st()
        with mock.patch.object(uhc, "get_install_status", side_effect=get), \
             mock.patch.object(uhc, "start_install"):
            out = msu.update_install_staged(confirm=True, wait_s=9999)
        self.assertTrue(out.startswith("UNKNOWN"))
        self.assertGreater(len(seen), 2)
        self.assertLess(max(seen), 270.0)
        self.assertLess(self.clock.t - t0, 280.0)

    def test_refusal_with_status_still_failed(self):
        with mock.patch.object(uhc, "get_install_status", return_value=_st()), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("x", 409, "busy")):
            self.assertTrue(msu.update_install_staged(confirm=True).startswith("FAILED"))

    def test_auth_refusal_without_status_is_failed_and_does_not_poll(self):
        try:
            try:
                raise msu.http_auth.HttpAuthError("relogin refused")
            except msu.http_auth.HttpAuthError as inner:
                raise uhc.UpdateHttpError("POST failed: relogin refused") from inner
        except uhc.UpdateHttpError as err:
            auth_err = err
        with mock.patch.object(uhc, "get_install_status", return_value=_st()) as g, \
             mock.patch.object(uhc, "start_install", side_effect=auth_err):
            out = msu.update_install_staged(confirm=True)
        self.assertTrue(out.startswith("FAILED"), out)
        self.assertNotIn("possibly applied", out)
        self.assertEqual(g.call_count, 1)  # initial GET only, no polling


class RebootEvidenceTests(_Base):
    """MED-1: a poll error is not proof of a reboot; only the uptime comparison is."""

    def _same_version(self, poll_err, reboot_before_poll=False, **kw):
        stale = _ok()
        err = uhc.UpdateHttpError("connection refused")  # POST lost: no status

        def get(host, timeout=8.0):
            if get.calls == 0:
                get.calls += 1
                return stale
            get.calls += 1
            if get.calls == 2:
                if reboot_before_poll:
                    self.reboot()
                raise poll_err
            return stale
        get.calls = 0
        with mock.patch.object(uhc, "get_install_status", side_effect=get), \
             mock.patch.object(uhc, "start_install", side_effect=err):
            return msu.update_install_staged(confirm=True, wait_s=60, **kw)

    def test_failed_poll_without_reboot_is_not_success_for_each_error_kind(self):
        for name, perr in _poll_errors().items():
            with self.subTest(kind=name):
                msu._PENDING_INSTALL.clear()
                out = self._same_version(perr)
                self.assertTrue(out.startswith("UNKNOWN"), out)
                self.assertIn("stale", out)

    def test_same_version_ok_accepted_when_uptime_proves_reboot(self):
        for name, perr in _poll_errors().items():
            with self.subTest(kind=name):
                msu._PENDING_INSTALL.clear()
                self.boot_t = self.clock.t - 600.0
                out = self._same_version(perr, reboot_before_poll=True)
                self.assertTrue(out.startswith("ok"), out)

    def test_same_version_ok_not_accepted_when_uptime_unreadable_before(self):
        self.uptime_readable = False
        out = self._same_version(uhc.UpdateHttpError("down"))
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_same_version_ok_not_accepted_when_uptime_unreadable_after(self):
        stale = _ok()
        reads = {"n": 0}

        def status(host, timeout=8.0):
            reads["n"] += 1
            if reads["n"] > 1:
                raise dhc.DashboardHttpError("down")
            return {"uptime_s": 500}
        with mock.patch.object(dhc, "get_status", side_effect=status), \
             mock.patch.object(uhc, "get_install_status", return_value=stale), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_uptime_that_keeps_counting_is_not_a_reboot(self):
        # No poll error at all, uptime simply continues: stale ok, never success.
        stale = _ok()
        with mock.patch.object(uhc, "get_install_status", return_value=stale), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("UNKNOWN"), out)


class FollowUpTests(_Base):
    """Pooled bench finding: the call ran out of time; a follow-up decides with the same evidence."""

    def _timed_out_call(self):
        stale = _ok()
        with mock.patch.object(uhc, "get_install_status",
                               side_effect=[stale] + [uhc.UpdateHttpError("down")] * 300), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            out = msu.update_install_staged(confirm=True, wait_s=9999)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertIn("may still be running", out)
        self.assertIn("update_install_staged(confirm=False)", out)

    def test_follow_up_reports_ok_once_new_version_runs_and_reboot_proven(self):
        self._timed_out_call()
        self.reboot()
        self.clock.sleep(30)
        with mock.patch.object(uhc, "get_install_status", return_value=_ok()), \
             mock.patch.object(uhc, "start_install") as post:
            out = msu.update_install_staged(confirm=False)
        post.assert_not_called()
        self.assertTrue(out.startswith("ok"), out)
        self.assertEqual(msu._PENDING_INSTALL, {})

    def test_follow_up_without_reboot_proof_is_not_ok(self):
        self._timed_out_call()
        with mock.patch.object(uhc, "get_install_status", return_value=_ok()):
            out = msu.update_install_staged(confirm=False)
        self.assertFalse(out.startswith("ok"), out)

    def test_follow_up_reports_recorded_failure(self):
        self._timed_out_call()
        with mock.patch.object(uhc, "get_install_status",
                               return_value=_st(running="1.0.0", state="recorded", code="fail", reason="apply_failed")):
            out = msu.update_install_staged(confirm=False)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_plain_status_read_without_pending_install_stays_a_dry_run(self):
        with mock.patch.object(uhc, "get_install_status", return_value=_ok()), \
             mock.patch.object(uhc, "start_install") as post:
            out = msu.update_install_staged(confirm=False)
        post.assert_not_called()
        self.assertTrue(out.startswith("REFUSED"), out)


if __name__ == "__main__":
    unittest.main()
