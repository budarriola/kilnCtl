#!/usr/bin/env python3
"""Unit tests for kilnctrl.capability_preflight -- all against MOCKED
urllib responses. No real socket and no live board (192.168.1.156 is
mid-campaign and off limits) is used or required.

Run with: tools/PcTools/.venv/Scripts/python.exe -m pytest tools/PcTools/tests
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import capability_preflight as cp  # noqa: E402
from kilnctrl import config_presets  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


_STATUS_BODY = json.dumps({
    "fw_version": "1.4.2",
    "fw_build": "Aug 30 2026 10:00:00",
    "self_protocol_version": 3,
}).encode()

_RAMP_ASSIST_PRESENT_BODY = json.dumps({"enabled": False}).encode()
_RAMP_ASSIST_ABSENT_BODY = json.dumps({"ok": False, "error": "no such endpoint"}).encode()


def _preset(ramp_assist_enabled: bool) -> dict:
    return {
        "name": "test-preset",
        "ramp_assist_enabled": ramp_assist_enabled,
        "zones": [
            {
                "index": 0, "relay_mask": 1, "control_mode": 1, "cal_offset_c": 0.0,
                "pid_kp": 1.0, "pid_ki": 0.1, "pid_kd": 0.0, "max_ramp_c_per_hr": 100.0,
                "max_temp_c": 200.0, "min_temp_c": 0.0,
            }
        ],
    }


def _urlopen_router(responses: dict):
    """responses: {path_substring: body_bytes_or_exception}. First match by
    substring wins; used to route GET /api/status vs GET /api/ramp_assist to
    different canned bodies within one test."""

    def _fake_urlopen(req, timeout=None):
        url = req.full_url if hasattr(req, "full_url") else req
        for key, value in responses.items():
            if key in url:
                if isinstance(value, Exception):
                    raise value
                return _fake_response(value)
        raise AssertionError(f"unexpected URL in test: {url}")

    return _fake_urlopen


class DeriveRequiredCapabilitiesTest(unittest.TestCase):
    def test_ramp_assist_required_when_zones_host_given(self):
        preset = _preset(False)
        required = cp.derive_required_capabilities(preset, zones_host="192.168.1.50")
        names = [c.name for c, _v in required]
        self.assertIn("ramp_assist", names)

    def test_ramp_assist_not_required_without_zones_host(self):
        # NEGATIVE-TESTED below (see test_manifest_precondition_mutation).
        preset = _preset(False)
        required = cp.derive_required_capabilities(preset, zones_host=None)
        names = [c.name for c, _v in required]
        self.assertNotIn("ramp_assist", names)

    def test_field_absent_from_preset_yields_no_requirement(self):
        preset = {"name": "x", "zones": []}  # no ramp_assist_enabled key at all
        required = cp.derive_required_capabilities(preset, zones_host="192.168.1.50")
        self.assertEqual(required, [])


class AllPresentTest(unittest.TestCase):
    """All required capabilities present -> ok, no fatal checks."""

    def test_all_present_is_ok(self):
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_PRESENT_BODY,
        }
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
        self.assertTrue(report.ok)
        self.assertEqual(report.fatal_checks, [])
        self.assertTrue(report.board.reachable)
        self.assertEqual(report.board.fw_version, "1.4.2")
        text = report.describe()
        self.assertIn("[ok]", text)
        self.assertIn("ok to start", text)


class FatalMissingTest(unittest.TestCase):
    """Preset pins ramp_assist_enabled=True, firmware lacks the endpoint ->
    FATAL, exactly the 906d026 incident shape."""

    def test_fatal_missing_capability(self):
        preset = _preset(True)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
            self.assertFalse(report.ok)
            self.assertEqual(len(report.fatal_checks), 1)
            self.assertEqual(report.fatal_checks[0].capability.name, "ramp_assist")
            text = report.describe()
            self.assertIn("FATAL", text)
            self.assertIn("reflash", text)
            self.assertIn("do not start this run", text)

            with self.assertRaises(cp.PreflightFailed) as ctx:
                cp.preflight_or_raise(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
            self.assertIn("FATAL", str(ctx.exception))


class BenignMissingTest(unittest.TestCase):
    """Preset pins ramp_assist_enabled=False, firmware lacks the endpoint ->
    BENIGN (firmware without the feature already behaves as "off")."""

    def test_benign_missing_capability(self):
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
            self.assertTrue(report.ok)
            self.assertEqual(report.fatal_checks, [])
            check = report.checks[0]
            self.assertFalse(check.present)
            self.assertFalse(check.fatal)
            text = report.describe()
            self.assertIn("[benign]", text)
            self.assertIn("ok to start", text)
            # preflight_or_raise must NOT raise for a benign-only report.
            result = cp.preflight_or_raise(preset, "192.168.1.50", zones_host="192.168.1.50",
                                            preset_name="test-preset")
            self.assertTrue(result.ok)


class BoardUnreachableTest(unittest.TestCase):
    """A board that never answers must NOT be reported as fine -- this is
    the exact "must not report everything fine" requirement."""

    def test_unreachable_board_is_not_ok(self):
        preset = _preset(True)

        def _refuse(req, timeout=None):
            raise urllib.error.URLError("connection refused")

        with unittest.mock.patch.object(cp.urllib.request, "urlopen", side_effect=_refuse):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
        self.assertFalse(report.ok)
        self.assertFalse(report.board.reachable)
        # every required capability is reported fatal-and-unresolved, not silently dropped
        self.assertEqual(len(report.checks), 1)
        self.assertTrue(report.checks[0].fatal)
        text = report.describe()
        self.assertIn("BOARD UNREACHABLE", text)
        self.assertIn("do not start this run", text)
        self.assertNotIn("ok to start", text)


_CRASH_UNACK_BODY = json.dumps({
    "present": True, "acknowledged": False, "exc_cause": 6, "exc_cause_str": "IllegalInstruction",
    "exc_pc": "0x4008abcd", "exc_addr": "0x00000000", "exc_task": "safety_poll",
    "found_on_boot_reset_reason": "panic/exception", "backtrace": [], "backtrace_corrupted": False,
}).encode()
_CRASH_ACK_BODY = json.dumps({
    "present": True, "acknowledged": True, "exc_cause": 6, "exc_cause_str": "IllegalInstruction",
    "exc_pc": "0x4008abcd", "exc_addr": "0x00000000", "exc_task": "safety_poll",
    "found_on_boot_reset_reason": "panic/exception", "backtrace": [], "backtrace_corrupted": False,
}).encode()
_CRASH_NONE_BODY = json.dumps({"present": False}).encode()


class UnacknowledgedCrashReportTest(unittest.TestCase):
    """MANDATORY negative test (task instructions): an unacknowledged crash
    report must fail the preflight regardless of whether the preset needs
    any HTTP capability at all -- the 2026-08-31 incident (board panicked,
    ran 5 hours unnoticed, including through a preflight-shaped check) is
    exactly the scenario this guards. Constructed with SYNTHETIC data only;
    no real board is touched."""

    def test_unacknowledged_crash_blocks_a_run_that_needs_no_capability(self):
        preset = _preset(False)  # benign/no-fatal-capability preset on purpose
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/crash_report": _CRASH_UNACK_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
            self.assertFalse(report.ok, "unacknowledged crash must fail the preflight")
            self.assertTrue(report.board.crash_unacknowledged)
            self.assertIn("safety_poll", report.board.crash_summary)
            text = report.describe()
            self.assertIn("UNACKNOWLEDGED CRASH REPORT", text)
            self.assertIn("do not start this run", text)
            self.assertNotIn("ok to start", text)

            with self.assertRaises(cp.PreflightFailed) as ctx:
                cp.preflight_or_raise(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
            self.assertIn("UNACKNOWLEDGED CRASH REPORT", str(ctx.exception))

    def test_acknowledged_crash_does_not_block(self):
        """Same crash record, but acknowledged=true -- an operator has
        reviewed it, so this must NOT be fatal (the run stays gate-able only
        by an actual unresolved capability gap)."""
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/crash_report": _CRASH_ACK_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
        self.assertTrue(report.ok)
        self.assertFalse(report.board.crash_unacknowledged)

    def test_clean_board_no_crash_record_passes(self):
        """PROOF a clean board still passes: {"present": false} is the
        common case and must never itself be treated as a problem."""
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
        self.assertTrue(report.ok)
        self.assertFalse(report.board.crash_unacknowledged)
        self.assertNotIn("UNACKNOWLEDGED CRASH REPORT", report.describe())

        with self.assertRaises(cp.PreflightFailed):
            cp.preflight_or_raise(preset, "192.168.1.50", zones_host="192.168.1.50",
                                   preset_name="test-preset")

    def test_unreachable_board_not_ok_even_with_zero_required_capabilities(self):
        # Isolates PreflightReport.ok's own "board must have answered"
        # check from the "no fatal checks" check: a preset that (with these
        # apply args) requires NO HTTP capability at all must still not be
        # reported ok when the board never answered anything, including
        # /api/status -- there being nothing to preflight is not the same
        # as everything being fine.
        preset = {"name": "x", "zones": []}

        def _refuse(req, timeout=None):
            raise urllib.error.URLError("connection refused")

        with unittest.mock.patch.object(cp.urllib.request, "urlopen", side_effect=_refuse):
            report = cp.run_preflight(preset, "192.168.1.50", preset_name="x")
        self.assertEqual(report.checks, [])
        self.assertFalse(report.board.reachable)
        self.assertFalse(report.ok)

    def test_status_reachable_but_capability_probe_fails_is_fatal_unresolved(self):
        # Board answers /api/status fine but the specific capability probe
        # times out / connection resets mid-run -- distinct from "board
        # totally unreachable", still must not be reported as present.
        preset = _preset(False)

        def _router(req, timeout=None):
            url = req.full_url
            if "/api/status" in url:
                return _fake_response(_STATUS_BODY)
            raise urllib.error.URLError("connection reset")

        with unittest.mock.patch.object(cp.urllib.request, "urlopen", side_effect=_router):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
        self.assertFalse(report.ok)
        self.assertTrue(report.board.reachable)
        self.assertIsNone(report.checks[0].present)
        self.assertTrue(report.checks[0].fatal)


class NoSuchEndpointDetectionTest(unittest.TestCase):
    def test_unrelated_4xx_is_not_treated_as_absent(self):
        """A 4xx that is NOT the board's precise {"ok":false,"error":"no
        such endpoint"} body must surface as a transport failure (fatal,
        unresolved), never as a silent 'capability absent, benign'."""
        preset = _preset(False)

        def _router(req, timeout=None):
            url = req.full_url
            if "/api/status" in url:
                return _fake_response(_STATUS_BODY)
            raise urllib.error.HTTPError(url, 403, "forbidden", hdrs=None,
                                          fp=io.BytesIO(b'{"ok":false,"error":"forbidden"}'))

        with unittest.mock.patch.object(cp.urllib.request, "urlopen", side_effect=_router):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
        self.assertFalse(report.ok)
        self.assertIsNone(report.checks[0].present)
        self.assertTrue(report.checks[0].fatal)


class ManifestMatchesApplyPresetTest(unittest.TestCase):
    """Tripwire: this module's manifest asserts that apply_preset() only
    pins ramp_assist_enabled over HTTP when zones_host is given. If
    apply_preset() changes that gating, this preflight would silently
    become wrong about whether ramp_assist is required -- this test fails
    loudly instead. See capability_preflight.py's docstring, "THE ONE
    MANUAL STEP THAT REMAINS"."""

    def test_manifest_matches_apply_preset_ramp_assist_pin(self):
        import inspect
        src = inspect.getsource(config_presets.apply_preset)
        # apply_preset() only calls ramp_assist_http_client.set_enabled(...)
        # inside its `if zones_host:` branch -- confirm that call site is
        # still there and still gated the way this manifest assumes.
        if_zones_idx = src.index("if zones_host:")
        set_enabled_idx = src.index("ramp_assist_http_client.set_enabled")
        self.assertGreater(
            set_enabled_idx, if_zones_idx,
            "apply_preset() no longer pins ramp_assist_enabled inside its "
            "'if zones_host:' branch -- capability_preflight.py's "
            "_CAPABILITY_MANIFEST precondition for 'ramp_assist' is now "
            "stale and must be updated to match (see this module's "
            "docstring, THE ONE MANUAL STEP THAT REMAINS)",
        )


class TaskLivenessGatingTest(unittest.TestCase):
    """MANDATORY negative test (task instructions): a task_liveness result
    with a dead or absent required task must fail the preflight regardless
    of whether the preset needs any HTTP capability at all, unless
    allow_missing_tasks=True -- same shape as
    UnacknowledgedCrashReportTest above. task_liveness.py has no board/link
    access of its own, so it is handed to run_preflight() pre-computed,
    exactly as mcp_server_capability_preflight.py's tool does."""

    def _tl_report(self, dead=(), absent=()):
        from kilnctrl import task_liveness
        from kilnctrl.devices_info import StackMarginEntry
        from kilnctrl.protocol import StackMarginLevel

        expected = ("kiln_io_owner", "profile_executor", "httpd_worker")
        entries = []
        for name in expected:
            if name in dead:
                entries.append(StackMarginEntry(name=name, configured_stack_bytes=4096,
                                                 hwm_bytes=0, alive=False,
                                                 level=StackMarginLevel.OK))
            elif name in absent:
                continue
            else:
                entries.append(StackMarginEntry(name=name, configured_stack_bytes=4096,
                                                 hwm_bytes=2000, alive=True,
                                                 level=StackMarginLevel.OK))
        return task_liveness.check_task_liveness(entries, expected)

    def test_dead_task_blocks_a_run_that_needs_no_capability(self):
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        tl = self._tl_report(dead=("profile_executor",))
        self.assertFalse(tl.ok)
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset", task_liveness=tl)
        self.assertFalse(report.ok, "a dead required task must fail the preflight")
        text = report.describe()
        self.assertIn("DEAD", text)
        self.assertIn("profile_executor", text)
        self.assertIn("do not start this run", text)

    def test_absent_task_blocks_a_run(self):
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        tl = self._tl_report(absent=("httpd_worker",))
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset", task_liveness=tl)
        self.assertFalse(report.ok)
        self.assertIn("ABSENT", report.describe())
        self.assertIn("httpd_worker", report.describe())

    def test_allow_missing_tasks_overrides_the_refusal(self):
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        tl = self._tl_report(dead=("profile_executor",))
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset", task_liveness=tl,
                                       allow_missing_tasks=True)
        self.assertTrue(report.ok, "allow_missing_tasks=True must override the refusal")
        self.assertIn("[allowed]", report.describe())

    def test_all_alive_task_liveness_does_not_block(self):
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        tl = self._tl_report()
        self.assertTrue(tl.ok)
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset", task_liveness=tl)
        self.assertTrue(report.ok)
        self.assertIn("all 3 expected task(s) alive", report.describe())

    def test_no_task_liveness_supplied_does_not_block(self):
        """None (not checked) must never itself fail a preflight -- e.g. no
        live link available for this host."""
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
        self.assertTrue(report.ok)

    def test_no_task_liveness_supplied_shows_explicit_skip_line(self):
        """task_liveness=None must not read as 'checked and fine' in
        describe() -- an explicit [skip] line makes the blind spot visible
        rather than silently omitting any mention of task liveness."""
        preset = _preset(False)
        responses = {
            "/api/status": _STATUS_BODY,
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/ramp_assist": _RAMP_ASSIST_ABSENT_BODY,
        }
        with unittest.mock.patch.object(cp.urllib.request, "urlopen",
                                         side_effect=_urlopen_router(responses)):
            report = cp.run_preflight(preset, "192.168.1.50", zones_host="192.168.1.50",
                                       preset_name="test-preset")
        text = report.describe()
        self.assertIn("[skip]", text)
        self.assertIn("task liveness: not checked (no link)", text)


if __name__ == "__main__":
    unittest.main()
