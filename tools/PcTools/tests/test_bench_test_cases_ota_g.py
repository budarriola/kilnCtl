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
import types
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.dirname(__file__))

from kilnctrl import update_release_http_client as URC  # noqa: E402
from kilnctrl.bench_test import cases_ota as C  # noqa: E402
from kilnctrl.bench_test.registry import Verdict, get_case  # noqa: E402
from test_bench_test_cases_ota import (  # noqa: E402
    _Clock, _FakeDashboardClient, _FakeOtaClient, _FakeSrv, _OtaHttpErr)

IMAGE = b"kiln-image-bytes" * 8

RUNNING_BUILD = "Oct  6 2026 12:34:56"


def _image_with_build(date="Oct  6 2026", time_s="12:34:56"):
    """A minimal ESP app image carrying a real esp_app_desc_t (magic, time and
    date at the offsets esp_app_desc.parse_app_desc reads)."""
    import struct
    from kilnctrl import esp_app_desc as D
    img = bytearray(2048)
    img[0] = 0xE9
    base = D.APP_DESC_OFFSET
    struct.pack_into("<I", img, base, D.ESP_APP_DESC_MAGIC_WORD)
    img[base + 80:base + 80 + len(time_s)] = time_s.encode()
    img[base + 96:base + 96 + len(date)] = date.encode()
    return bytes(img)


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
        self.cancels = 0
        self.stage_to_mutate = stage_to_mutate

    def get_settings(self, host):
        return {"repo": self.repo, "is_default": self.is_default}

    def set_settings(self, host, repo):
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

    def cancel_fetch(self, host):
        self.cancels += 1
        self.statuses = [{"state": "failed", "error": "cancelled", "busy": False}]
        return {"ok": True, "cancelling": True}

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

    def test_clear_failure_fails_never_passes(self):
        # A PASS that leaves a verified image staged is not a pass.
        r = C._case_otg01(_base_ctx(update_http_client=_FakeUpdate(clear_exc=RuntimeError("nope"))))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("NOT confirmed cleared", r.reason)

    def test_upload_error_after_board_staged_clears_leftover(self):
        # Client timeout while the board still finished staging.
        upd = _FakeUpdate()
        orig = upd.upload_stage

        def staged_then_timeout(host, image, **kw):
            orig(host, image, **kw)
            raise TimeoutError("client gave up")

        upd.upload_stage = staged_then_timeout
        r = C._case_otg01(_base_ctx(update_http_client=upd))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(upd.clears, 1)
        self.assertFalse(upd.stage["staged"])


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

    def test_missing_busy_field_fails_never_passes(self):
        r = C._case_otg02(self._ctx(stage={"staged": False, "phase": "idle"}))
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_missing_staged_field_fails_never_passes(self):
        r = C._case_otg02(self._ctx(stage={"busy": False, "phase": "idle"}))
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_staged_after_truncation_is_cleared(self):
        ctx = self._ctx(stage={"staged": True, "busy": False})
        C._case_otg02(ctx)
        self.assertEqual(ctx["update_http_client"].clears, 1)
        self.assertFalse(ctx["update_http_client"].stage["staged"])

    def test_2xx_answer_clears_leftover_stage(self):
        ctx = self._ctx(status=200, stage={"staged": True, "busy": False})
        self.assertEqual(C._case_otg02(ctx).verdict, Verdict.FAIL)
        self.assertEqual(ctx["update_http_client"].clears, 1)

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
        orig = rel.set_settings

        def flaky(host, repo):
            if not repo:
                raise OSError("link down")
            orig(host, repo)

        rel.set_settings = flaky
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

    def test_job_never_finishes_is_cancelled(self):
        r, rel, ctx = self._run({"state": "downloading", "busy": True})
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(rel.cancels, 1)
        self.assertNotIn("_tainted", ctx)

    def test_job_that_will_not_end_taints(self):
        rel = _FakeRelease(statuses=[{"state": "downloading", "busy": True}])
        rel.cancel_fetch = lambda host: {"ok": True}
        ctx = _base_ctx(update_release_http_client=rel, update_downgrade_repo="o/old")
        r = C._case_otg03(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(ctx["_tainted"])

    def test_indefinite_is_default_skips_without_touching_repo(self):
        for flag in (None, "yes", 1):
            rel = _FakeRelease(is_default=flag)
            r = C._case_otg03(_base_ctx(update_release_http_client=rel, update_downgrade_repo="o/old"))
            self.assertEqual(r.verdict, Verdict.SKIP, flag)
            self.assertEqual(rel.set_calls, [], flag)

    def test_download_that_staged_is_cleared(self):
        upd = _FakeUpdate()
        rel = _FakeRelease(statuses=[{"state": "done", "allowed": True}])
        orig = rel.start_download

        def stage_it(host, **kw):
            orig(host, **kw)
            upd.stage = {"staged": True, "busy": False, "sha256": "ab"}

        rel.start_download = stage_it
        r = C._case_otg03(_base_ctx(update_http_client=upd, update_release_http_client=rel,
                                    update_downgrade_repo="o/old"))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(upd.clears, 1)
        self.assertFalse(upd.stage["staged"])


class Otg05Test(unittest.TestCase):
    def _run(self, status):
        rel = _FakeRelease(statuses=[status])
        return C._case_otg05(_base_ctx(update_release_http_client=rel, update_wrong_repo="o/none")), rel

    def test_unusable_release_passes(self):
        self.assertEqual(self._run({"state": "failed", "error": "no_app_asset"})[0].verdict, Verdict.PASS)

    def test_transport_failure_is_not_a_pass(self):
        for st in ({"state": "failed", "error": "connect_failed"},
                   {"state": "failed", "error": "timeout"},
                   {"state": "failed", "error": "http_status", "http_status": 403},
                   {"state": "failed"}):
            self.assertEqual(self._run(st)[0].verdict, Verdict.INCONCLUSIVE, st)

    def test_clean_failure_passes(self):
        r, rel = self._run({"state": "failed", "error": "http_status", "http_status": 404})
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

    def test_accepted_upload_is_cleared_after_the_run(self):
        upd = _FakeUpdate()
        r = C._case_otg04(self._ctx(upd))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(upd.clears, 1)
        self.assertFalse(upd.stage["staged"])

    def test_refused_upload_does_not_clear(self):
        upd = _FakeUpdate(upload_exc=_OtaHttpErr(409, "busy"))
        C._case_otg04(self._ctx(upd))
        self.assertEqual(upd.clears, 0)

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


def _safety_seams(trip_reason=0, trip_mask=0, link_up=True, received=True, enabled=True, status_raises=False):
    """Fake safety seams for OT-G06's post-reset check. Returns (kwargs, clears)."""
    clears = []

    def status():
        if status_raises:
            raise OSError("no link")
        return types.SimpleNamespace(link_up=link_up, enabled=enabled)

    def diag():
        return types.SimpleNamespace(trip_reason=trip_reason, trip_mask=trip_mask, ever_received=received)

    return dict(_get_safety_status_fn=status, _get_safety_diag_fn=diag,
                _clear_trip_fn=lambda: clears.append(1)), clears


class Otg06Test(unittest.TestCase):
    def _ctx(self, behaviour, upd=None, **over):
        upd = upd or _FakeUpdate()
        kw = dict(update_http_client=upd, ota_http_client=_Ota06(upd, behaviour, interlock_ok=True),
                  _read_image_fn=lambda p: _image_with_build(),
                  dashboard_http_client=_FakeDashboardClient(fw_build=RUNNING_BUILD))
        kw.update(_safety_seams()[0])
        kw.update(over)
        return _base_ctx(**kw)

    def test_not_running_build_skips_before_staging(self):
        ctx = self._ctx("clear", dashboard_http_client=_FakeDashboardClient(fw_build="Oct  5 2026 01:02:03"))
        r = C._case_otg06(ctx)
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertEqual(ctx["update_http_client"].uploads, 0)
        self.assertEqual(ctx["ota_http_client"].resets, 0)

    def test_unreadable_fw_build_skips_before_staging(self):
        class _Dead:
            def get_status(self, host):
                raise OSError("down")

        ctx = self._ctx("clear", dashboard_http_client=_Dead())
        self.assertEqual(C._case_otg06(ctx).verdict, Verdict.SKIP)
        self.assertEqual(ctx["update_http_client"].uploads, 0)

    def test_image_without_app_desc_skips(self):
        ctx = self._ctx("clear", _read_image_fn=lambda p: IMAGE)
        self.assertEqual(C._case_otg06(ctx).verdict, Verdict.SKIP)
        self.assertEqual(ctx["update_http_client"].uploads, 0)

    def test_relay_energized_or_unreadable_skips_reset_and_clears(self):
        for energized in (True, None):
            ctx = self._ctx("clear", dashboard_http_client=_FakeDashboardClient(
                fw_build=RUNNING_BUILD, relay_energized=energized))
            r = C._case_otg06(ctx)
            self.assertEqual(r.verdict, Verdict.SKIP, r.reason)
            self.assertEqual(ctx["ota_http_client"].resets, 0)
            self.assertEqual(ctx["update_http_client"].clears, 1)
            self.assertFalse(ctx["update_http_client"].stage["staged"])

    def test_survived_and_uncleared_fails(self):
        upd = _FakeUpdate()
        ctx = self._ctx("keep", upd=upd)

        def no_clear(host):
            upd.clears += 1
            return {"ok": True}  # claims success, stage stays

        upd.clear_stage = no_clear
        r = C._case_otg06(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("could NOT be cleared", r.reason)

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
        ctx = self._ctx("clear", upd=upd)
        r = C._case_otg06(ctx)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(ctx["ota_http_client"].resets, 0)

    def test_board_never_answers_after_reset_fails(self):
        # Answers until the reset, then never again: exercises the post-reset
        # poll, not the post-upload read.
        ctx = self._ctx("clear")
        upd, ota = ctx["update_http_client"], ctx["ota_http_client"]
        orig = upd.get_stage_status

        def status(host, **kw):
            if ota.resets:
                raise TimeoutError("gone")
            return orig(host, **kw)

        upd.get_stage_status = status
        r = C._case_otg06(ctx)
        self.assertEqual(ota.resets, 1)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("did not answer", r.reason)


class Otg06SafetyAfterResetTest(unittest.TestCase):
    """The dual reset can latch S6a: OT-G06 must look, clear only S6a alone,
    and never pass over anything else."""

    def _run(self, **seam_kw):
        seams, clears = _safety_seams(**seam_kw)
        ctx = Otg06Test._ctx(Otg06Test(), "clear", **seams)
        return C._case_otg06(ctx), clears

    def test_link_up_no_trip_passes(self):
        r, clears = self._run()
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(clears, [])
        self.assertIn("no trip", r.reason)

    def test_s6a_alone_is_cleared_and_passes(self):
        mask = 1 << (6 - 1)
        self.assertEqual(mask, 0x0020)
        r, clears = self._run(trip_reason=6, trip_mask=mask)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(clears, [1])
        self.assertIn("was cleared", r.reason)

    def test_s6a_clear_not_confirmed_fails(self):
        r, clears = self._run(trip_reason=6, trip_mask=0x0020, enabled=False)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(clears, [1])

    def test_s6a_with_extra_mask_bit_fails_and_never_clears(self):
        r, clears = self._run(trip_reason=6, trip_mask=0x0020 | 0x0001)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(clears, [])

    def test_s6b_link_dead_trip_fails_and_never_clears(self):
        # 0x0040 is bit 6 (trip_reason 7, S6b), not S6a.
        r, clears = self._run(trip_reason=7, trip_mask=0x0040)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(clears, [])

    def test_wrong_mask_for_reason_6_fails_and_never_clears(self):
        r, clears = self._run(trip_reason=6, trip_mask=0x0040)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(clears, [])

    def test_link_down_fails(self):
        r, clears = self._run(link_up=False)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(clears, [])

    def test_status_unreadable_fails(self):
        r, clears = self._run(status_raises=True)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(clears, [])

    def test_no_diag_frame_fails(self):
        r, clears = self._run(received=False)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(clears, [])

    def test_trip_reason_unreadable_fails(self):
        r, clears = self._run(trip_reason=None, trip_mask=None)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(clears, [])

    def test_no_reset_sent_skips_safety_check(self):
        # Relay energized: SKIP before the reset; safety unreadable must not turn it into FAIL.
        seams, _ = _safety_seams(status_raises=True)
        ctx = Otg06Test._ctx(Otg06Test(), "clear", dashboard_http_client=_FakeDashboardClient(
            fw_build=RUNNING_BUILD, relay_energized=True), **seams)
        self.assertEqual(C._case_otg06(ctx).verdict, Verdict.SKIP)


class WiringTest(unittest.TestCase):
    def test_wired_and_registered(self):
        for i in range(1, 7):
            cid = "OT-G0%d" % i
            self.assertIs(get_case(cid).judge, getattr(C, "_case_otg0%d" % i))
        self.assertTrue(get_case("OT-G04").heat)
        self.assertFalse(get_case("OT-G01").heat)


class TruncatedUploadClientTest(unittest.TestCase):
    """upload_stage_truncated against a real loopback socket (no board)."""

    def _serve(self, reply):
        import socket
        import threading
        srv = socket.socket()
        srv.bind(("127.0.0.1", 0))
        srv.listen(1)
        got = {"body": b"", "head": b""}

        def run():
            conn, _ = srv.accept()
            data = b""
            sep = bytes([13, 10, 13, 10])
            while sep not in data:
                data += conn.recv(4096)
            head, _, rest = data.partition(sep)
            got["head"] = head
            body = rest
            while True:
                chunk = conn.recv(4096)
                if not chunk:
                    break
                body += chunk
            got["body"] = body
            if reply:
                conn.sendall(reply)
            conn.close()
            srv.close()

        t = threading.Thread(target=run, daemon=True)
        t.start()
        return "127.0.0.1:%d" % srv.getsockname()[1], got, t

    def test_declares_full_length_sends_part_and_reports_answer(self):
        image = bytes([0xE9]) + b"A" * 99_999
        crlf = bytes([13, 10])
        host, got, t = self._serve(b"HTTP/1.1 400 Bad Request" + crlf + b"Content-Length: 2" + crlf + crlf + b"no")
        with unittest.mock.patch("kilnctrl.http_auth.login", return_value="sid123"):
            status, detail = URC.upload_stage_truncated(host, image, 0.6, timeout=5.0)
        t.join(5)
        self.assertEqual(status, 400)
        self.assertEqual(detail, "no")
        self.assertIn(b"Content-Length: 100000", got["head"])
        self.assertIn(b"kiln_sid=sid123", got["head"])
        self.assertEqual(len(got["body"]), 60_000)

    def test_dropped_connection_reports_none(self):
        host, _got, t = self._serve(b"")
        with unittest.mock.patch("kilnctrl.http_auth.login", return_value="sid"):
            status, detail = URC.upload_stage_truncated(host, bytes([0xE9]) + b"B" * 99_999, 0.6, timeout=5.0)
        t.join(5)
        self.assertIsNone(status)
        self.assertTrue(detail)


if __name__ == "__main__":
    unittest.main()
