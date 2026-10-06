#!/usr/bin/env python3
"""Unit tests for OT-G01..G06 (GitHub-release update cases) in
kilnctrl.bench_test.cases_ota and for kilnctrl.update_release_http_client.
Every board/HTTP call is faked; these bodies have NEVER been run on the
bench board, so this confirms case/judgment LOGIC only.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_ota_g.py -q
"""
from __future__ import annotations

import hashlib
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.dirname(__file__))

from kilnctrl import update_release_http_client as URC  # noqa: E402
from kilnctrl.bench_test import cases_ota as C  # noqa: E402
from kilnctrl.bench_test.registry import Verdict, get_case  # noqa: E402
from test_bench_test_cases_ota import (  # noqa: E402
    _Clock, _FakeOtaClient, _FakeSrv, _OtaHttpErr)

IMAGE = b"kiln-image-bytes" * 8


class _FakeUpdate:
    """Stands in for update_http_client."""

    def __init__(self, upload_exc=None, report_sha=None, accept_staging=True, clear_exc=None):
        self.stage = {"staged": False, "busy": False}
        self.upload_exc = upload_exc
        self.report_sha = report_sha
        self.accept_staging = accept_staging
        self.clear_exc = clear_exc
        self.uploads = 0
        self.clears = 0

    @staticmethod
    def sha256_hex(data):
        return hashlib.sha256(data).hexdigest()

    def upload_stage(self, host, image, **kw):
        self.uploads += 1
        if self.upload_exc:
            raise self.upload_exc
        if self.accept_staging:
            self.stage = {"staged": True, "busy": False, "state": "verified", "semver": "1.2.3",
                          "image_length": len(image),
                          "sha256": self.report_sha or hashlib.sha256(image).hexdigest()}
        return {"ok": True}

    def get_stage_status(self, host, **kw):
        return dict(self.stage)

    def clear_stage(self, host):
        self.clears += 1
        if self.clear_exc:
            raise self.clear_exc
        self.stage = {"staged": False, "busy": False}
        return {"ok": True}


class _FakeRelease:
    def __init__(self, repo="", is_default=True, statuses=None, start_exc=None, stage_to_mutate=None):
        self.repo = repo
        self.is_default = is_default
        self.statuses = list(statuses or [{"state": "failed"}])
        self.start_exc = start_exc
        self.set_calls = []
        self.started = []
        self.stage_to_mutate = stage_to_mutate

    def get_settings(self, host):
        return {"repo": self.repo, "is_default": self.is_default}

    def set_repo(self, host, repo):
        self.set_calls.append(repo)
        if not repo:
            self.repo, self.is_default = "", True
        else:
            self.repo, self.is_default = repo, False

    def start_check(self, host, **kw):
        self.started.append(("check", kw))
        if self.start_exc:
            raise self.start_exc

    def start_download(self, host, **kw):
        self.started.append(("download", kw))
        if self.start_exc:
            raise self.start_exc
        if self.stage_to_mutate is not None:
            self.stage_to_mutate["staged"] = True

    def get_fetch_status(self, host):
        return self.statuses.pop(0) if len(self.statuses) > 1 else self.statuses[0]

    @staticmethod
    def error_name(exc):
        return getattr(exc, "name", "")


def _base_ctx(**over):
    ctx = {
        "host": "10.0.0.5", "srv": _FakeSrv("idle"),
        "ota_image_path": "/x/image.bin", "_isfile_fn": lambda p: True,
        "_read_image_fn": lambda p: IMAGE,
        "ota_http_client": _FakeOtaClient(interlock_ok=True),
        "update_http_client": _FakeUpdate(),
        "update_release_http_client": _FakeRelease(),
        "_now": _Clock(1.0), "_sleep_fn": lambda s: None,
    }
    ctx.update(over)
    return ctx


class CommonGateTest(unittest.TestCase):
    CASES = (("OT-G01", C._case_otg01, {}),
             ("OT-G02", C._case_otg02, {"_truncated_upload_fn": lambda: (400, "x")}),
             ("OT-G03", C._case_otg03, {"update_downgrade_repo": "o/old"}),
             ("OT-G05", C._case_otg05, {"update_wrong_repo": "o/none"}),
             ("OT-G06", C._case_otg06, {}))

    def test_not_idle_skips_and_does_not_touch_board(self):
        for cid, fn, extra in self.CASES:
            ctx = _base_ctx(srv=_FakeSrv("running"), **extra)
            r = fn(ctx)
            self.assertEqual(r.verdict, Verdict.SKIP, cid)
            self.assertEqual(ctx["update_http_client"].uploads, 0, cid)
            self.assertEqual(ctx["update_release_http_client"].set_calls, [], cid)

    def test_interlock_not_ok_skips(self):
        for cid, fn, extra in self.CASES:
            ctx = _base_ctx(ota_http_client=_FakeOtaClient(interlock_ok=False, interlock_reason="busy"), **extra)
            r = fn(ctx)
            self.assertEqual(r.verdict, Verdict.SKIP, cid)
            self.assertIn("interlock", r.reason, cid)
            self.assertEqual(ctx["update_http_client"].uploads, 0, cid)
            self.assertEqual(ctx["update_release_http_client"].set_calls, [], cid)

    def test_missing_parameter_skips(self):
        self.assertEqual(C._case_otg03(_base_ctx()).verdict, Verdict.SKIP)
        self.assertEqual(C._case_otg05(_base_ctx()).verdict, Verdict.SKIP)
        for fn in (C._case_otg01, C._case_otg02, C._case_otg04, C._case_otg06):
            self.assertEqual(fn(_base_ctx(ota_image_path=None)).verdict, Verdict.SKIP, fn.__name__)

    def test_unreadable_image_skips(self):
        for fn in (C._case_otg01, C._case_otg02, C._case_otg04, C._case_otg06):
            r = fn(_base_ctx(_isfile_fn=lambda p: False))
            self.assertEqual(r.verdict, Verdict.SKIP, fn.__name__)


class Otg01Test(unittest.TestCase):
    def test_pass_and_stage_cleared(self):
        ctx = _base_ctx()
        r = C._case_otg01(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(ctx["update_http_client"].clears, 1)

    def test_sha_mismatch_fails(self):
        r = C._case_otg01(_base_ctx(update_http_client=_FakeUpdate(report_sha="0" * 64)))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("!=", r.reason)

    def test_not_staged_fails(self):
        r = C._case_otg01(_base_ctx(update_http_client=_FakeUpdate(accept_staging=False)))
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_upload_refused_fails(self):
        r = C._case_otg01(_base_ctx(update_http_client=_FakeUpdate(upload_exc=_OtaHttpErr(400, "bad"))))
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_clear_failure_is_reported_not_hidden(self):
        r = C._case_otg01(_base_ctx(update_http_client=_FakeUpdate(clear_exc=RuntimeError("nope"))))
        self.assertEqual(r.verdict, Verdict.PASS)
        self.assertIn("stage clear afterwards failed", r.reason)


class Otg02Test(unittest.TestCase):
    def _ctx(self, status=400, stage=None, **over):
        upd = _FakeUpdate()
        if stage:
            upd.stage = stage
        return _base_ctx(update_http_client=upd, _truncated_upload_fn=lambda: (status, "short body"), **over)

    def test_refused_and_nothing_staged_passes(self):
        self.assertEqual(C._case_otg02(self._ctx()).verdict, Verdict.PASS)

    def test_connection_dropped_passes(self):
        ctx = _base_ctx(_truncated_upload_fn=lambda: (None, "ConnectionResetError: x"))
        self.assertEqual(C._case_otg02(ctx).verdict, Verdict.PASS)

    def test_2xx_answer_fails(self):
        self.assertEqual(C._case_otg02(self._ctx(status=200)).verdict, Verdict.FAIL)

    def test_staged_after_truncation_fails(self):
        r = C._case_otg02(self._ctx(stage={"staged": True, "busy": False}))
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_stuck_busy_fails(self):
        r = C._case_otg02(self._ctx(stage={"staged": False, "busy": True}))
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_unreadable_stage_fails_never_passes(self):
        upd = _FakeUpdate()

        def boom(host, **kw):
            raise TimeoutError("x")

        upd.get_stage_status = boom
        r = C._case_otg02(_base_ctx(update_http_client=upd, _truncated_upload_fn=lambda: (400, "x")))
        self.assertEqual(r.verdict, Verdict.FAIL)


class Otg03Test(unittest.TestCase):
    def _run(self, status, **rel_kw):
        rel = _FakeRelease(statuses=[status], **rel_kw)
        ctx = _base_ctx(update_release_http_client=rel, update_downgrade_repo="o/old")
        return C._case_otg03(ctx), rel, ctx

    def test_refused_downgrade_passes_and_repo_restored(self):
        r, rel, _ = self._run({"state": "failed", "verdict": "refuse_downgrade", "bytes_done": 0})
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(rel.set_calls, ["o/old", ""])
        self.assertTrue(rel.is_default)
        self.assertEqual(rel.started, [("download", {})])

    def test_done_without_allow_fails(self):
        r, rel, _ = self._run({"state": "done", "allowed": True})
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(rel.is_default)

    def test_other_failure_is_inconclusive(self):
        r, _, _ = self._run({"state": "failed", "error": "http_404"})
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_stage_changed_fails(self):
        upd = _FakeUpdate()
        rel = _FakeRelease(statuses=[{"state": "failed", "verdict": "refuse_downgrade"}], stage_to_mutate=upd.stage)
        r = C._case_otg03(_base_ctx(update_http_client=upd, update_release_http_client=rel,
                                    update_downgrade_repo="o/old"))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(rel.is_default, "repo must be restored even on FAIL")

    def test_previous_custom_repo_restored(self):
        rel = _FakeRelease(repo="me/mine", is_default=False,
                           statuses=[{"state": "failed", "verdict": "refuse_downgrade"}])
        r = C._case_otg03(_base_ctx(update_release_http_client=rel, update_downgrade_repo="o/old"))
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(rel.repo, "me/mine")

    def test_failed_restore_fails_and_taints(self):
        rel = _FakeRelease(statuses=[{"state": "failed", "verdict": "refuse_downgrade"}])
        orig = rel.set_repo

        def flaky(host, repo):
            if not repo:
                raise OSError("link down")
            orig(host, repo)

        rel.set_repo = flaky
        ctx = _base_ctx(update_release_http_client=rel, update_downgrade_repo="o/old")
        r = C._case_otg03(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("tainted", r.reason)
        self.assertTrue(ctx["_tainted"])

    def test_clock_not_synced_skips(self):
        exc = RuntimeError("409")
        exc.name = "clock_not_synced"
        r, rel, _ = self._run({"state": "failed"}, start_exc=exc)
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertTrue(rel.is_default)

    def test_job_never_finishes_fails(self):
        r, rel, _ = self._run({"state": "running"})
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(rel.is_default)


class Otg05Test(unittest.TestCase):
    def _run(self, status):
        rel = _FakeRelease(statuses=[status])
        return C._case_otg05(_base_ctx(update_release_http_client=rel, update_wrong_repo="o/none")), rel

    def test_clean_failure_passes(self):
        r, rel = self._run({"state": "failed", "error": "http_404", "http_status": 404})
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(rel.started, [("check", {})])
        self.assertTrue(rel.is_default)

    def test_release_found_is_inconclusive(self):
        self.assertEqual(self._run({"state": "done", "tag": "v9"})[0].verdict, Verdict.INCONCLUSIVE)

    def test_tag_but_failed_fails(self):
        self.assertEqual(self._run({"state": "failed", "tag": "v1", "allowed": True})[0].verdict, Verdict.FAIL)


class Otg04Test(unittest.TestCase):
    def _ctx(self, upd, **over):
        ctx = _base_ctx(update_http_client=upd,
                        ota_http_client=_FakeOtaClient(interlock_ok=False, interlock_reason="firing"))
        ctx["_exec_state_fn"] = lambda: "running"
        ctx.update(over)
        return ctx

    def test_refused_upload_passes(self):
        upd = _FakeUpdate(upload_exc=_OtaHttpErr(409, "busy"))
        r = C._case_otg04(self._ctx(upd))
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)

    def test_accepted_upload_fails(self):
        r = C._case_otg04(self._ctx(_FakeUpdate()))
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_refusal_that_still_changes_stage_fails(self):
        upd = _FakeUpdate(upload_exc=_OtaHttpErr(409, "busy"))
        orig = upd.upload_stage

        def sneaky(host, image, **kw):
            upd.stage = {"staged": True, "busy": False, "sha256": "ab"}
            return orig(host, image, **kw)

        upd.upload_stage = sneaky
        self.assertEqual(C._case_otg04(self._ctx(upd)).verdict, Verdict.FAIL)

    def test_wrong_state_without_start_skips(self):
        upd = _FakeUpdate(upload_exc=_OtaHttpErr(409, "busy"))
        r = C._case_otg04(self._ctx(upd, _exec_state_fn=lambda: "idle"))
        self.assertEqual(r.verdict, Verdict.SKIP)

    def test_non_http_error_fails(self):
        upd = _FakeUpdate(upload_exc=FileNotFoundError("x"))
        self.assertEqual(C._case_otg04(self._ctx(upd)).verdict, Verdict.FAIL)


class _Ota06(_FakeOtaClient):
    def __init__(self, upd, behaviour, **kw):
        super().__init__(**kw)
        self.upd, self.behaviour, self.resets = upd, behaviour, 0

    def sw_reset(self, host):
        self.resets += 1
        if self.behaviour == "clear":
            self.upd.stage = {"staged": False, "busy": False, "boot_auto_cleared": True}
        elif self.behaviour == "empty_unflagged":
            self.upd.stage = {"staged": False, "busy": False}
        elif self.behaviour == "keep":
            self.upd.stage = dict(self.upd.stage, boot_auto_clear="kept")
        return {"ok": True}


class Otg06Test(unittest.TestCase):
    def _ctx(self, behaviour):
        upd = _FakeUpdate()
        return _base_ctx(update_http_client=upd, ota_http_client=_Ota06(upd, behaviour, interlock_ok=True))

    def test_auto_cleared_passes(self):
        ctx = self._ctx("clear")
        r = C._case_otg06(ctx)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(ctx["ota_http_client"].resets, 1)

    def test_empty_without_boot_flag_fails(self):
        self.assertEqual(C._case_otg06(self._ctx("empty_unflagged")).verdict, Verdict.FAIL)

    def test_survived_reboot_is_inconclusive_and_cleared(self):
        ctx = self._ctx("keep")
        r = C._case_otg06(ctx)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(ctx["update_http_client"].clears, 1)

    def test_no_reset_when_upload_fails(self):
        upd = _FakeUpdate(upload_exc=_OtaHttpErr(400, "bad"))
        ota = _Ota06(upd, "clear", interlock_ok=True)
        r = C._case_otg06(_base_ctx(update_http_client=upd, ota_http_client=ota))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(ota.resets, 0)

    def test_board_never_answers_fails(self):
        ctx = self._ctx("clear")
        ctx["update_http_client"].get_stage_status = lambda host, **kw: None
        self.assertEqual(C._case_otg06(ctx).verdict, Verdict.FAIL)


class WiringTest(unittest.TestCase):
    def test_wired_and_registered(self):
        for i in range(1, 7):
            cid = "OT-G0%d" % i
            self.assertIs(get_case(cid).judge, getattr(C, "_case_otg0%d" % i))
        self.assertTrue(get_case("OT-G04").heat)
        self.assertFalse(get_case("OT-G01").heat)


class ReleaseClientTest(unittest.TestCase):
    def test_get_fetch_status_requires_state(self):
        with unittest.mock.patch.object(URC._u, "_request", return_value={"x": 1}):
            with self.assertRaises(Exception):
                URC.get_fetch_status("h")

    def test_get_fetch_status_ok(self):
        with unittest.mock.patch.object(URC._u, "_request", return_value={"state": "done"}):
            self.assertEqual(URC.get_fetch_status("h")["state"], "done")


if __name__ == "__main__":
    unittest.main()
