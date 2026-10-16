"""update_install_staged against a fake board (patched update_http_client)."""
import unittest
from unittest import mock

from kilnctrl import dashboard_http_client as dhc
from kilnctrl import mcp_server_update as msu
from kilnctrl import update_http_client as uhc


STAGED_SHA = "a0a1a2a3" + "00" * 28   # the staged image's sha256; identity = first 8 hex chars
STAGED_ID = "a0a1a2a3"


def _st(running="1.0.0", staged=True, sv="1.0.1", code="none", state="none", reason="none", busy=False,
        rid=None):
    li = {"state": state, "code": code, "reason": reason, "version": sv}
    if rid is not None:
        li["id"] = rid
    return {"running_version": running, "stage_busy": busy, "staged": staged, "staged_version": sv,
            "last_install": li}


def _ok(running="1.0.1", sv="1.0.1", rid=None):
    return _st(running=running, sv=sv, state="recorded", code="ok", rid=rid)


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
        msu._EXPIRED_INSTALL.clear()
        self.addCleanup(msu._PENDING_INSTALL.clear)
        self.addCleanup(msu._EXPIRED_INSTALL.clear)
        self.staged_sha = STAGED_SHA
        for p in (mock.patch.object(msu, "_resolve_host", return_value="h"), \
                     mock.patch.object(msu.time, "sleep", side_effect=self.clock.sleep), \
                     mock.patch.object(msu.time, "monotonic", side_effect=self.clock.monotonic), \
                     mock.patch.object(dhc, "get_status", side_effect=self._status), \
                     mock.patch.object(uhc, "get_stage_status", side_effect=lambda h, timeout=8.0: {"sha256": self.staged_sha})):
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
        self.assertIn("login refused or failed", out)  # AUTOFX2 INFO-3: not "authentication refused"
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


class SlowReadTests(_Base):
    """Request latency on the uptime reads must not look like a reboot."""

    def _run(self, delays, reboot):
        """delays: per uptime read, (seconds before sampling, seconds before the answer returns)."""
        calls = {"n": 0}

        def status(host, timeout=8.0):
            pre, post = delays[min(calls["n"], len(delays) - 1)]
            calls["n"] += 1
            self.clock.t += pre
            up = int(self.clock.t - self.boot_t)
            self.clock.t += post
            return {"uptime_s": up}
        stale = _ok()
        seq = [stale, uhc.UpdateHttpError("down")] + [stale] * 200

        def get(host, timeout=8.0):
            v = seq.pop(0)
            if isinstance(v, Exception):
                if reboot:
                    self.reboot()
                raise v
            return v
        with mock.patch.object(dhc, "get_status", side_effect=status), \
             mock.patch.object(uhc, "get_install_status", side_effect=get), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            return msu.update_install_staged(confirm=True, wait_s=60)

    def test_slow_reads_without_reboot_are_not_ok(self):
        cases = {"slow before-read request": [(3.0, 0.0), (0.0, 0.0)],
                 "slow after-read answer": [(0.0, 0.0), (0.0, 3.0)],
                 "slow both, 1 s each": [(1.0, 1.0), (1.0, 1.0)],
                 "slow both, 3 s each": [(3.0, 3.0), (3.0, 3.0)]}
        for name, d in cases.items():
            msu._PENDING_INSTALL.clear()
            self.boot_t = self.clock.t - 600.0
            out = self._run(d, False)
            self.assertTrue(out.startswith("UNKNOWN"), f"{name}: {out}")

    def test_slow_reads_with_real_reboot_are_ok(self):
        self.assertTrue(self._run([(1.0, 1.0)], True).startswith("ok"))


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

    def test_unproven_follow_up_is_pending_not_refused(self):
        self._timed_out_call()
        with mock.patch.object(uhc, "get_install_status", return_value=_ok()):
            out = msu.update_install_staged(confirm=False)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertNotIn("REFUSED", out)
        self.assertNotIn("confirm=True", out)
        self.assertIn("poll again with update_install_staged(confirm=False)", out)

    def test_stale_pending_record_does_not_yield_installed(self):
        self._timed_out_call()
        self.clock.sleep(86400)
        self.reboot()
        with mock.patch.object(uhc, "get_install_status", return_value=_ok()):
            out = msu.update_install_staged(confirm=False)
        self.assertFalse(out.startswith("ok"), out)
        self.assertEqual(msu._PENDING_INSTALL, {})

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


class IdentityTests(_Base):
    """LOW-1: the result record carries the first 4 bytes of the staged sha256; the tool matches it."""

    def _run(self, seq, start_exc=None, **kw):
        with mock.patch.object(uhc, "get_install_status", side_effect=seq), \
             mock.patch.object(uhc, "start_install", side_effect=start_exc, return_value={}):
            return msu.update_install_staged(confirm=True, wait_s=60, **kw)

    def test_matching_id_proves_a_new_record_without_any_uptime_proof(self):
        # Same version already running, POST lost, uptime unreadable: only the id (differs from the
        # ok record seen before the POST) can show the record is new.
        self.uptime_readable = False
        before = _ok(rid="deadbeef")
        out = self._run([before] + [_ok(rid=STAGED_ID)] * 50, uhc.UpdateHttpError("lost"))
        self.assertTrue(out.startswith("ok"), out)

    def test_mismatching_id_is_stale_even_when_the_post_was_answered(self):
        seq = [_st()] + [_ok(rid="deadbeef")] * 50
        out = self._run(seq)
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_mismatching_id_after_running_version_change_is_not_ok(self):
        seq = [_st()] + [_ok(rid="deadbeef")] * 50
        out = self._run(seq, uhc.UpdateHttpError("lost"))
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_stale_failure_with_another_id_is_ignored(self):
        bad = _st(running="1.0.0", state="recorded", code="fail", reason="apply_failed", rid="deadbeef")
        out = self._run([_st(), bad, bad, _ok(rid=STAGED_ID)])
        self.assertTrue(out.startswith("ok"), out)

    def test_failure_with_matching_id_is_failed(self):
        bad = _st(running="1.0.0", state="recorded", code="fail", reason="apply_failed", rid=STAGED_ID)
        out = self._run([_st(), bad])
        self.assertTrue(out.startswith("FAILED"), out)

    def test_failure_without_an_id_is_still_failed(self):
        bad = _st(running="1.0.0", state="recorded", code="fail", reason="apply_failed", rid="00000000")
        out = self._run([_st(), bad])
        self.assertTrue(out.startswith("FAILED"), out)

    def test_zero_id_falls_back_to_the_old_proof(self):
        out = self._run([_st(), _ok(rid="00000000")])
        self.assertTrue(out.startswith("ok"), out)  # POST answered: accepted as before
        msu._PENDING_INSTALL.clear()
        out = self._run([_ok(rid="00000000")] * 60, uhc.UpdateHttpError("lost"))
        self.assertTrue(out.startswith("UNKNOWN"), out)  # POST lost, same version, no reboot proof

    def test_same_id_as_the_record_before_the_post_is_not_new(self):
        # Reinstalling the same image: the old ok record has the same id, so the id alone proves nothing.
        out = self._run([_ok(rid=STAGED_ID)] * 60, uhc.UpdateHttpError("lost"))
        self.assertTrue(out.startswith("UNKNOWN"), out)

    def test_unreadable_stage_sha_falls_back_to_the_old_proof(self):
        self.staged_sha = ""
        out = self._run([_st(), _ok(rid="deadbeef")])
        self.assertTrue(out.startswith("ok"), out)

    def test_follow_up_uses_the_id_too(self):
        self.uptime_readable = False
        with mock.patch.object(uhc, "get_install_status", side_effect=[_ok(rid="deadbeef")] + [uhc.UpdateHttpError("down")] * 300), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            self.assertTrue(msu.update_install_staged(confirm=True, wait_s=9999).startswith("UNKNOWN"))
        with mock.patch.object(uhc, "get_install_status", return_value=_ok(rid="deadbeef")):
            self.assertFalse(msu.update_install_staged(confirm=False).startswith("ok"))
        with mock.patch.object(uhc, "get_install_status", return_value=_ok(rid=STAGED_ID)):
            self.assertTrue(msu.update_install_staged(confirm=False).startswith("ok"))


class LostRecordTests(_Base):
    """LOW-2: no pending record (expired / MCP restart) is UNKNOWN 'cannot confirm', not a confirm=True offer."""

    def _pending_then_expire(self):
        with mock.patch.object(uhc, "get_install_status", side_effect=[_ok()] + [uhc.UpdateHttpError("down")] * 300), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            msu.update_install_staged(confirm=True, wait_s=9999)
        self.clock.sleep(msu._PENDING_TTL_S + 5)

    def test_expired_record_says_cannot_confirm(self):
        self._pending_then_expire()
        with mock.patch.object(uhc, "get_install_status", return_value=_ok()), \
             mock.patch.object(uhc, "start_install") as post:
            out = msu.update_install_staged(confirm=False)
        post.assert_not_called()
        self.assertTrue(out.startswith("UNKNOWN: cannot confirm"), out)
        self.assertIn("expired", out)
        self.assertIn(str(int(msu._PENDING_TTL_S)), out)
        self.assertIn("update_status", out)
        self.assertNotIn("confirm=True", out)
        self.assertNotIn("REFUSED", out)

    def test_expired_record_with_a_stage_present_is_still_cannot_confirm(self):
        self._pending_then_expire()
        with mock.patch.object(uhc, "get_install_status", return_value=_st()):
            out = msu.update_install_staged(confirm=False)
        self.assertTrue(out.startswith("UNKNOWN: cannot confirm"), out)

    def test_nothing_staged_and_no_record_says_cannot_confirm(self):
        # After a successful install the stage is gone; a restarted server has no record.
        with mock.patch.object(uhc, "get_install_status", return_value=_st(running="1.0.1", staged=False, state="recorded", code="ok")):
            out = msu.update_install_staged(confirm=False)
        self.assertTrue(out.startswith("UNKNOWN: cannot confirm"), out)
        self.assertNotIn("confirm=True", out)

    def test_new_confirmed_install_clears_the_tombstone(self):
        self._pending_then_expire()
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st(), _ok()]), \
             mock.patch.object(uhc, "start_install", return_value={}):
            self.assertTrue(msu.update_install_staged(confirm=True, wait_s=60).startswith("ok"))
        self.assertEqual(msu._EXPIRED_INSTALL, {})


class UptimeSlackTests(_Base):
    """LOW-3: uptime_s is a floored whole second; the slack must cover both truncations (>= ~1 s)."""

    def test_worst_case_truncation_is_not_a_reboot(self):
        # before: true 600.00 reads 600. 10.99 s later the true uptime is 610.99 and reads 610.
        # Without enough slack 610 + slack < 600 + 10.99 would claim a reboot that never happened.
        pend = {"before_up": 600.0, "t_pre": 1000.0}
        with mock.patch.object(msu.time, "monotonic", return_value=1010.99), \
             mock.patch.object(msu, "_read_uptime", return_value=610.0):
            self.assertFalse(msu._reboot_proven("h", pend))

    def test_a_real_reboot_just_past_the_slack_is_proven(self):
        pend = {"before_up": 600.0, "t_pre": 1000.0}
        with mock.patch.object(msu.time, "monotonic", return_value=1010.0), \
             mock.patch.object(msu, "_read_uptime", return_value=7.0):
            self.assertTrue(msu._reboot_proven("h", pend))


class DeadlineChargeTests(_Base):
    """INFO-2: the in-loop uptime read is charged to the call's deadline."""

    def test_slow_uptime_reads_do_not_push_the_call_past_the_deadline(self):
        # Vary the budget so some poll always starts within 5 s of the deadline, whatever the poll phase.
        for wait_s in range(50, 66):  # no subTest: negtest needs a plain FAILED line
            if True:
                msu._PENDING_INSTALL.clear()
                t0 = self.clock.t

                reads = {"n": 0}

                def slow_status(host, timeout=8.0):
                    reads["n"] += 1
                    if reads["n"] == 1:  # the before-POST read works, so later reads are needed for proof
                        return {"uptime_s": 500}
                    self.clock.t += timeout  # the read takes as long as its timeout allows
                    raise dhc.DashboardHttpError("timed out")
                with mock.patch.object(dhc, "get_status", side_effect=slow_status), \
                     mock.patch.object(uhc, "get_install_status", return_value=_ok()), \
                     mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
                    out = msu.update_install_staged(confirm=True, wait_s=wait_s)
                self.assertTrue(out.startswith("UNKNOWN"), f"wait_s={wait_s}: {out}")
                self.assertLess(self.clock.t - t0, wait_s + 0.5, f"wait_s={wait_s}")


class RecoveryApplyingTests(_Base):
    """A 404 on /api/update/status is the recovery image still applying, not absence."""

    def test_404_polls_then_ok(self):
        e404 = uhc.UpdateHttpError("GET failed: HTTP 404", 404, "not found")
        seq = [_st()] + [e404] * 5 + [_ok()]
        with mock.patch.object(uhc, "get_install_status", side_effect=seq), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            self.assertTrue(msu.update_install_staged(confirm=True, wait_s=60).startswith("ok"))

    def test_only_404s_is_unknown_naming_the_recovery_image_and_the_background_tool(self):
        e404 = uhc.UpdateHttpError("GET failed: HTTP 404", 404, "not found")
        self.uptime_readable = False  # the recovery image has no /api/status either
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()] + [e404] * 300), \
             mock.patch.object(uhc, "start_install", return_value={}):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertIn("recovery image is still applying", out)
        self.assertNotIn("never answered", out)
        self.assertIn("update_install_start", out)


    def test_404_while_app_status_answers_is_an_app_without_the_install_route(self):
        """AUTOFX5 LOW-1."""
        e404 = uhc.UpdateHttpError("GET failed: HTTP 404", 404, "not found")
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()] + [e404] * 300), \
             mock.patch.object(uhc, "start_install", return_value={}):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertIn("WITHOUT the install route", out)
        self.assertNotIn("recovery image is still applying", out)

    def test_app_that_answers_between_last_poll_and_probe_is_ok_not_a_downgrade(self):
        """UPDLOWFX LOW-3: a good install landing right after the last poll must not read as a downgrade."""
        e404 = uhc.UpdateHttpError("GET failed: HTTP 404", 404, "not found")
        t0 = self.clock.t

        def status(host, timeout=8.0):
            if self.clock.t > t0 + 60:  # only the post-loop re-read runs past the deadline
                return _ok()
            if self.clock.t < t0 + 3:
                return _st()
            raise e404

        with mock.patch.object(uhc, "get_install_status", side_effect=status),              mock.patch.object(uhc, "start_install", return_value={}):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("ok"), out)
        self.assertNotIn("downgrade", out)

    def test_non_404_tail_read_error_is_not_called_a_downgrade(self):
        """REVIEW_LOWFX10 LOW-1: a timeout, failed relogin or 5xx on the follow-up read says nothing
        about the install route, so the text must not claim "a downgrade?"."""
        e404 = uhc.UpdateHttpError("GET failed: HTTP 404", 404, "not found")
        for label, err in (("timeout", uhc.UpdateHttpError("timed out")),
                           ("5xx", uhc.UpdateHttpError("GET failed: HTTP 502", 502, "bad gateway"))):
            with self.subTest(label):
                msu._PENDING_INSTALL.clear()
                t0 = self.clock.t

                def status(host, timeout=8.0, err=err):
                    if self.clock.t < t0 + 3:
                        return _st()
                    if self.clock.t > t0 + 60:  # only the post-loop re-read runs past the deadline
                        raise err
                    raise e404

                with mock.patch.object(uhc, "get_install_status", side_effect=status),                      mock.patch.object(uhc, "start_install", return_value={}):
                    out = msu.update_install_staged(confirm=True, wait_s=60)
                self.assertTrue(out.startswith("UNKNOWN"), out)
                self.assertNotIn("downgrade", out)
                self.assertNotIn("WITHOUT the install route", out)
                self.assertIn("follow-up read", out)

    def test_tail_reads_are_short_and_bounded(self):
        """UPDLOWFX LOW-3: the post-loop probe and re-read each use the short timeout."""
        e404 = uhc.UpdateHttpError("GET failed: HTTP 404", 404, "not found")
        seen = []

        def status(host, timeout=8.0):
            seen.append(timeout)
            if len(seen) == 1:
                return _st()
            raise e404

        probes = []
        orig = msu._read_uptime
        with mock.patch.object(uhc, "get_install_status", side_effect=status),              mock.patch.object(msu, "_read_uptime", side_effect=lambda h, timeout=5.0: probes.append(timeout) or orig(h, timeout)),              mock.patch.object(uhc, "start_install", return_value={}):
            msu.update_install_staged(confirm=True, wait_s=60)
        self.assertEqual(probes[-1], msu._TAIL_PROBE_S)
        self.assertEqual(seen[-1], msu._TAIL_PROBE_S)
        self.assertLessEqual(msu._TAIL_PROBE_S, 3.0)

    def test_default_wait_leaves_margin_over_the_bench_return_time(self):
        """UPDLOWFX LOW-3: the app came back at ~231-235 s after the POST, 15 s of it spent in the settle sleep."""
        import inspect
        default = inspect.signature(msu.update_install_staged).parameters["wait_s"].default
        self.assertGreaterEqual(default, 265.0)
        self.assertLessEqual(default, msu._SYNC_WAIT_CAP_S)

    def test_unreadable_install_record_is_unknown_not_ok(self):
        """UPDLOWFX LOW-2."""
        st = _st(staged=False, state="unreadable", code="none")
        with mock.patch.object(uhc, "get_install_status", return_value=st),              mock.patch.object(uhc, "start_install") as post:
            out = msu.update_install_staged(confirm=False)
        post.assert_not_called()
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertIn("unreadable", out)
        self.assertNotIn("ok - nothing", out)

    def test_no_record_and_nothing_staged_is_still_plain_ok(self):
        with mock.patch.object(uhc, "get_install_status", return_value=_st(staged=False)):
            out = msu.update_install_staged(confirm=False)
        self.assertTrue(out.startswith("ok - nothing staged"), out)


class FlightAndStaleTests(_Base):
    """AUTOFX5 LOW-2/3/4."""

    def test_second_confirmed_call_while_one_is_in_flight_is_refused(self):
        msu._INSTALL_INFLIGHT.add("h")
        try:
            with mock.patch.object(uhc, "get_install_status", return_value=_st()), \
                 mock.patch.object(uhc, "start_install") as post:
                out = msu.update_install_staged(confirm=True, wait_s=60)
            post.assert_not_called()
            self.assertTrue(out.startswith("BUSY") and "in flight" in out, out)
        finally:
            msu._INSTALL_INFLIGHT.discard("h")

    def test_marker_is_held_during_the_call_so_a_concurrent_call_is_refused(self):
        seen = []

        def post(*a, **k):
            seen.append(msu.update_install_staged(confirm=True, wait_s=60))
            return {}

        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()] + [_ok()] * 5),              mock.patch.object(uhc, "start_install", side_effect=post):
            msu.update_install_staged(confirm=True, wait_s=60)
        self.assertEqual(len(seen), 1)
        self.assertTrue(seen[0].startswith("BUSY") and "in flight" in seen[0], seen[0])

    def test_inflight_marker_is_released_after_success_and_failure(self):
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st(), _ok()]), \
             mock.patch.object(uhc, "start_install", return_value={}):
            self.assertTrue(msu.update_install_staged(confirm=True, wait_s=60).startswith("ok"))
        self.assertEqual(msu._INSTALL_INFLIGHT, set())
        with mock.patch.object(uhc, "get_install_status", return_value=_st()), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("x", 409, "busy")):
            msu.update_install_staged(confirm=True, wait_s=60)
        self.assertEqual(msu._INSTALL_INFLIGHT, set())

    def test_stale_failure_record_does_not_end_a_lost_post_install(self):
        stale = _st(state="recorded", code="apply_failed", reason="old")
        with mock.patch.object(uhc, "get_install_status", side_effect=[stale, stale, stale, _ok()] + [_ok()] * 5), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertFalse(out.startswith("FAILED"), out)

    def test_new_failure_record_still_ends_a_lost_post_install(self):
        stale = _st(state="recorded", code="apply_failed", reason="old")
        newer = _st(state="recorded", code="apply_failed", reason="new")
        with mock.patch.object(uhc, "get_install_status", side_effect=[stale, newer] + [newer] * 5), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("FAILED"), out)

    def test_plain_status_read_with_nothing_recorded_is_not_cannot_confirm(self):
        with mock.patch.object(uhc, "get_install_status", return_value=_st(running="1.0.1", staged=False)):
            out = msu.update_install_staged(confirm=False)
        self.assertTrue(out.startswith("ok - nothing staged"), out)
        self.assertNotIn("cannot confirm", out)

    def test_unexpected_exception_after_the_post_is_unknown_not_error(self):
        """RELSUITEFX4 LOW-1: the POST was sent, so the bench case must not read it as never started."""
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st(), RuntimeError("boom")]),              mock.patch.object(uhc, "start_install", return_value={}):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertTrue(out.startswith("UNKNOWN") and "RuntimeError" in out, out)
        self.assertEqual(msu._INSTALL_INFLIGHT, set())

    def test_unexpected_exception_before_the_post_is_still_an_error(self):
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()]), \
             mock.patch.object(msu, "_staged_id", side_effect=RuntimeError("boom")):
            out = msu.update_install_staged(confirm=True, wait_s=60)  # _tool wrapper turns it into error:
        self.assertTrue(out.startswith("error:"), out)


class InstallStartTests(_Base):
    """The background twin: same routine, confirm unchanged, long bound, job classification."""

    def test_unconfirmed_starts_no_job_and_posts_nothing(self):
        with mock.patch.object(msu.build_jobs, "start_job") as sj, \
             mock.patch.object(uhc, "get_install_status", return_value=_st()), \
             mock.patch.object(uhc, "start_install") as post:
            for v in (False, 1, "yes", None):
                self.assertTrue(msu.update_install_start(confirm=v).startswith("REFUSED"))
            sj.assert_not_called()
            post.assert_not_called()

    def _runner(self):
        with mock.patch.object(msu.build_jobs, "start_job", return_value="abcd1234") as sj:
            out = msu.update_install_start(confirm=True)
        self.assertIn("abcd1234", out)
        self.assertIn("bench_test_job_status", out)
        args, kw = sj.call_args
        return args[1], kw["classify"]

    def test_job_runs_the_real_install_and_waits_much_longer_than_one_call(self):
        runner, classify = self._runner()
        t0 = self.clock.t
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()] + [uhc.UpdateHttpError("down")] * 500), \
             mock.patch.object(uhc, "start_install", return_value={}) as post:
            out = runner()
        post.assert_called_once()
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.assertEqual(classify(out), "unknown")
        self.assertGreater(self.clock.t - t0, 500.0)   # the synchronous tool stops below 280 s
        self.assertLess(self.clock.t - t0, 640.0)

    def test_job_reports_ok_after_the_late_return_seen_on_the_bench(self):
        # benchupd4/5: the application answered again ~231-235 s after the POST.
        runner, classify = self._runner()
        t0 = self.clock.t
        state = {"posted": None}

        def get(host, timeout=8.0):
            if state["posted"] is None:
                return _st()
            if self.clock.t - state["posted"] < 235.0:
                raise uhc.UpdateHttpError("GET failed: HTTP 404", 404, "not found")
            return _ok(rid=STAGED_ID)

        def post(host, *a, **k):
            state["posted"] = self.clock.t
            return {}
        with mock.patch.object(uhc, "get_install_status", side_effect=get), \
             mock.patch.object(uhc, "start_install", side_effect=post):
            out = runner()
        self.assertTrue(out.startswith("ok"), out)
        self.assertEqual(classify(out), "ok")
        self.assertGreater(self.clock.t - t0, 235.0)

    def test_pending_record_outlives_the_background_wait(self):
        # REVIEW_AUTOFX5: the job may give up after its full 600 s; a follow-up read shortly after
        # must still find the pending record (TTL longer than the job's wait), not "cannot confirm".
        runner, _ = self._runner()
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()] + [uhc.UpdateHttpError("down")] * 500), \
             mock.patch.object(uhc, "start_install", side_effect=uhc.UpdateHttpError("lost")):
            out = runner()
        self.assertTrue(out.startswith("UNKNOWN"), out)
        self.clock.sleep(30.0)
        self.reboot()
        with mock.patch.object(uhc, "get_install_status", return_value=_ok(rid=STAGED_ID)), \
             mock.patch.object(uhc, "start_install") as post:
            out = msu.update_install_staged(confirm=False)
        post.assert_not_called()
        self.assertTrue(out.startswith("ok"), out)

    def test_classification(self):
        _, classify = self._runner()
        self.assertEqual(classify("ok - installed"), "ok")
        self.assertEqual(classify("UNKNOWN: install may still be running"), "unknown")
        for bad in ("FAILED: x", "REFUSED: x", "error: x", ""):
            self.assertEqual(classify(bad), "failed")

    def test_job_refuses_inside_when_there_is_no_stage(self):
        runner, classify = self._runner()
        with mock.patch.object(uhc, "get_install_status", return_value=_st(staged=False)), \
             mock.patch.object(uhc, "start_install") as post:
            out = runner()
        post.assert_not_called()
        self.assertEqual(classify(out), "failed")

    def test_sync_unknown_text_points_at_the_background_tool(self):
        with mock.patch.object(uhc, "get_install_status", side_effect=[_st()] + [uhc.UpdateHttpError("down")] * 300), \
             mock.patch.object(uhc, "start_install", return_value={}):
            out = msu.update_install_staged(confirm=True, wait_s=60)
        self.assertIn("update_install_start(confirm=True)", out)
        self.assertIn("600", out)


if __name__ == "__main__":
    unittest.main()


class LegacyPrecheckTests(_Base):
    def _stage(self, **kw):
        d = {"ok": True, "phase": "idle", "busy": False, "staged": True, "state": "verified", "semver": "1.0.1",
             "sha256": STAGED_SHA}
        d.update(kw)
        return d

    def _run(self, stage, confirm, post_exc=None):
        e404 = uhc.UpdateHttpError("nf", 404, "nf")
        with mock.patch.object(uhc, "get_install_status", side_effect=e404),              mock.patch.object(uhc, "get_stage_status", side_effect=lambda h, timeout=8.0: stage),              mock.patch.object(uhc, "start_install", side_effect=post_exc) as post:
            return msu.update_install_staged(confirm=confirm, wait_s=20), post

    def test_verified_stage_unconfirmed_posts_nothing(self):
        out, post = self._run(self._stage(), False)
        self.assertTrue(out.startswith("REFUSED"), out)
        post.assert_not_called()

    def test_verified_stage_confirmed_posts(self):
        out, post = self._run(self._stage(), True, post_exc=uhc.UpdateHttpError("x", 409, "busy"))
        post.assert_called_once()
        self.assertTrue(out.startswith("FAILED"), out)

    def test_unverified_or_busy_stage_refused(self):
        for st in (self._stage(state="writing"), self._stage(staged=False), self._stage(busy=True)):
            out, post = self._run(st, True)
            self.assertTrue(out.startswith("REFUSED"), out)
            post.assert_not_called()

    def test_stage_read_failure_errors(self):
        e404 = uhc.UpdateHttpError("nf", 404, "nf")
        with mock.patch.object(uhc, "get_install_status", side_effect=e404),              mock.patch.object(uhc, "get_stage_status", side_effect=uhc.UpdateHttpError("down")),              mock.patch.object(uhc, "start_install") as post:
            self.assertTrue(msu.update_install_staged(confirm=True).startswith("error"))
            post.assert_not_called()
