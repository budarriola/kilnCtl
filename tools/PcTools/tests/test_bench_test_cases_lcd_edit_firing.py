#!/usr/bin/env python3
"""Unit tests for LCD-22 (Edit firing page live edit), cases_lcd._case_lcd22
and judgments.judge_lcd_edit_firing. A single fake board models the executor,
the live working copy, the panel and the relays -- never a real board.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_lcd_edit_firing.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_heat as CH  # noqa: E402
from kilnctrl.bench_test import cases_lcd as C  # noqa: E402
from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402

_TIMING_PATCHERS: "list" = []


def setUpModule():
    for p in (
        mock.patch.object(C, "_PAGE_POLL_TIMEOUT_S", 0.05),
        mock.patch.object(C, "_PAGE_POLL_INTERVAL_S", 0.01),
        mock.patch.object(C, "_WAKE_SCREEN_ON_TIMEOUT_S", 0.05),
        mock.patch.object(C, "_WAKE_SCREEN_ON_POLL_S", 0.01),
    ):
        p.start()
        _TIMING_PATCHERS.append(p)


def tearDownModule():
    for p in _TIMING_PATCHERS:
        p.stop()
    _TIMING_PATCHERS.clear()


AMBIENT = 25.0
SEG0_T = AMBIENT + 10.0
SEG1_T = AMBIENT + 20.0


class FakeLiveError(Exception):
    def __init__(self, status):
        super().__init__(f"HTTP {status}")
        self.status = status


class FakeBoard:
    """Executor + live working copy + panel + relays in one object."""

    def __init__(self, exec_state="idle", apply_works=True, apply_refused=False, locked=False,
                 stop_works=True, relays_energized=False, discard_works=True,
                 apply_changes_target_only=False, extra_edit_target=None):
        self.exec_state = exec_state
        self.apply_works = apply_works
        self.apply_refused = apply_refused
        self.locked = locked
        self.keypad = False
        self.stop_works = stop_works
        self.relays_energized = relays_energized
        self.discard_works = discard_works
        self.apply_changes_target_only = apply_changes_target_only
        self.extra_edit_target = extra_edit_target
        self.page = "home"
        self.segments = []
        self.saved = None
        self.started = False
        self.pending_target_steps = 0
        self.pending_dwell_steps = 0
        self.working = None  # list of segment dicts once Apply took
        self.calls = []

    # -- executor
    def exec_status(self):
        return SimpleNamespace(state_name=self.exec_state, profile_id=7, segment_count=2, segment_index=0)


class FakeProfiles:
    def __init__(self, board):
        self.b = board

    def save(self, slot, name, zone_mask, segments):
        self.b.calls.append("save")
        self.b.saved = [(s.target_c, s.ramp_c_per_hr, s.dwell_min) for s in segments]
        return SimpleNamespace(ok=True, error="")

    def start(self, slot):
        self.b.calls.append("start")
        self.b.started = True
        self.b.exec_state = "running"
        return SimpleNamespace(ok=True, error="")

    def stop(self):
        self.b.calls.append("stop")
        if self.b.stop_works:
            self.b.exec_state = "idle"

    def delete(self, slot):
        self.b.calls.append("delete")

    def get_exec_status(self):
        return self.b.exec_status()


class FakeTouch:
    def __init__(self, board):
        self.b = board
        self.presses = []

    def get_state(self):
        return SimpleNamespace(screen_on=True, idle_ms=0)

    def inject(self, x, y, pressed):
        if pressed:
            self.presses.append((x, y))
            if self.b.page == "edit_firing":
                if (x, y) == C._LCD22_TARGET_PLUS_XY:
                    self.b.pending_target_steps += 1
                elif (x, y) == C._LCD22_DWELL_PLUS_XY:
                    self.b.pending_dwell_steps += 1
        return SimpleNamespace(ok=True)


_EDIT_TARGETS = [
    {"name": "back", "cx": 30, "cy": 20, "hidden": False},
    {"name": "home", "cx": 90, "cy": 20, "hidden": False},
    {"name": "Apply", "cx": 239, "cy": 255, "hidden": False},
]
_HOME_TARGETS = [{"name": "Edit", "cx": 240, "cy": 280, "hidden": False}]
_KEYPAD_TARGETS = [{"name": n, "cx": 10, "cy": 10, "hidden": False}
                   for n in ["OK", "Cancel"] + [str(i) for i in range(10)]]


class FakeUi:
    def __init__(self, board):
        self.b = board

    def get_current_page(self):
        return self.b.page

    def list_tap_targets(self):
        if self.b.keypad:
            return {"targets": _KEYPAD_TARGETS, "truncated": False}
        if self.b.page == "edit_firing":
            extra = [self.b.extra_edit_target] if self.b.extra_edit_target else []
            return {"targets": _EDIT_TARGETS + extra, "truncated": False}
        return {"targets": _HOME_TARGETS, "truncated": False}

    def click_by_name(self, name):
        b = self.b
        b.calls.append(f"click:{name}")
        if name == "Edit" and b.page == "home":
            if b.exec_state not in ("running", "paused"):
                return {"result": "not_found"}
            if b.locked:
                b.keypad = True  # PIN-locked: the keypad rises, the page stays home
            else:
                b.page = "edit_firing"
            return {"result": "ok"}
        if name == "Cancel" and b.keypad:
            b.keypad = False
            return {"result": "ok"}
        if name in ("home", "back"):
            b.page = "home"
            return {"result": "ok"}
        if name == "Apply" and b.page == "edit_firing":
            if b.apply_works:
                t = SEG1_T + 5.0 * b.pending_target_steps
                d = 5.0 + (0.0 if b.apply_changes_target_only else 5.0 * b.pending_dwell_steps)
                b.working = [
                    {"seg_kind": 0, "target_c": SEG0_T, "ramp_c_per_hr": 600.0, "dwell_min": 10.0},
                    {"seg_kind": 0, "target_c": t, "ramp_c_per_hr": 600.0, "dwell_min": d},
                ]
            return {"result": "ok"}
        return {"result": "not_found"}


class FakeLiveClient:
    def __init__(self, board):
        self.b = board

    def get_live_status(self, host):
        b = self.b
        return {"active": b.working is not None, "working_id": 9 if b.working is not None else -1,
                "last_refusal": "window" if (b.apply_refused and b.working is not None) else None}

    def get_live_content(self, host):
        if self.b.working is None:
            raise FakeLiveError(409)
        return {"id": 9, "segment_count": 2, "segments": self.b.working}

    def decide_live_discard(self, host):
        if self.b.working is None:
            raise FakeLiveError(409)
        if self.b.discard_works:
            self.b.working = None
        return {"ok": True}


class _Clock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += max(s, 0.01)


def _run(board, allow_heat=True, ambient=AMBIENT, host="1.2.3.4"):
    clock = _Clock()
    srv = SimpleNamespace(
        _ui_test=FakeUi(board), _profiles=FakeProfiles(board), _touch=FakeTouch(board),
        _thermo=SimpleNamespace(read=lambda *a, **k: [
            SimpleNamespace(channel=0, temperature_c=ambient, valid=True)]),
    )
    ctx = {
        "srv": srv, "host": host, "allow_heat": allow_heat,
        "_now": clock.now, "_sleep": clock.sleep,
        "_profile_live_client": FakeLiveClient(board),
        "capability_preflight_run": lambda *_a, **_k: SimpleNamespace(ok=True),
        "_get_zones_config": lambda h: {"zones": []},
        "_http_get_json": lambda h, p: (404, {}),
    }
    with mock.patch("kilnctrl.dashboard_http_client.get_status", side_effect=lambda h: dict(
            safety_relay_energized=board.relays_energized)):
        result = C._case_lcd22(ctx)
    return result, srv


class RegistryTest(unittest.TestCase):
    def test_registered_as_heat_case_with_judge(self):
        spec = R.get_case("LCD-22")
        self.assertTrue(spec.heat)
        self.assertIs(spec.judge, C._case_lcd22)
        self.assertIn("LCD-22", R.SUITES["lcd"])


class Lcd22Test(unittest.TestCase):
    def test_pass_when_both_edits_land_and_cleanup_verifies(self):
        board = FakeBoard()
        result, srv = _run(board)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual([p for p in srv._touch.presses
                          if p in (C._LCD22_NEXT_XY, C._LCD22_TARGET_PLUS_XY, C._LCD22_DWELL_PLUS_XY)],
                         [C._LCD22_NEXT_XY, C._LCD22_TARGET_PLUS_XY, C._LCD22_DWELL_PLUS_XY])
        self.assertTrue(result.observed["cleanup"]["verified"])
        self.assertEqual(board.exec_state, "idle")
        self.assertIsNone(board.working)
        self.assertIn("stop", board.calls)
        self.assertNotIn("click:Confirm Stop", board.calls)

    def test_allow_heat_false_does_nothing(self):
        board = FakeBoard()
        result, _ = _run(board, allow_heat=False)
        self.assertEqual(result.verdict, Verdict.NOT_RUN)
        self.assertEqual(board.calls, [])

    def test_busy_executor_is_inconclusive_with_no_action(self):
        board = FakeBoard(exec_state="running")
        result, srv = _run(board)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(board.calls, [])
        self.assertEqual(srv._touch.presses, [])

    def test_ambient_too_high_is_inconclusive_with_no_action(self):
        board = FakeBoard()
        result, _ = _run(board, ambient=45.0)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(board.calls, [])

    def test_trip_blocks_via_preflight(self):
        board = FakeBoard()
        clock = _Clock()
        srv = SimpleNamespace(_ui_test=FakeUi(board), _profiles=FakeProfiles(board), _touch=FakeTouch(board),
                              _thermo=SimpleNamespace(read=lambda *a, **k: []))
        board_ctx = {
            "srv": srv, "host": "h", "allow_heat": True, "_now": clock.now, "_sleep": clock.sleep,
            "_profile_live_client": FakeLiveClient(board),
            "capability_preflight_run": lambda *_a, **_k: SimpleNamespace(
                ok=False, board=SimpleNamespace(crash_unacknowledged=False, readiness_blocked=True)),
        }
        result = C._case_lcd22(board_ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(board.calls, [])

    def test_change_not_picked_up_is_fail(self):
        board = FakeBoard(apply_works=False)
        result, _ = _run(board)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("change not picked up", result.reason)
        self.assertEqual(board.exec_state, "idle")  # still cleaned up

    def test_only_one_of_two_edits_landing_is_fail(self):
        board = FakeBoard(apply_changes_target_only=True)
        result, _ = _run(board)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("dwell", result.reason)

    def test_executor_refusal_is_fail(self):
        board = FakeBoard(apply_refused=True)
        result, _ = _run(board)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("refused", result.reason)

    def test_cleanup_stop_failure_forces_fail_even_when_edits_landed(self):
        board = FakeBoard(stop_works=False)
        result, _ = _run(board)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("cleanup could not be verified", result.reason)
        self.assertIn("E-stop", result.reason)

    def test_cleanup_discard_failure_forces_fail(self):
        board = FakeBoard(discard_works=False)
        result, _ = _run(board)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("cleanup could not be verified", result.reason)

    def test_energized_relay_after_stop_forces_fail(self):
        board = FakeBoard(relays_energized=True)
        result, _ = _run(board)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("cleanup could not be verified", result.reason)

    def test_pin_keypad_is_inconclusive_cancelled_and_never_typed(self):
        board = FakeBoard(locked=True)
        result, srv = _run(board)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("PIN", result.reason)
        digits = [c for c in board.calls if c.startswith("click:") and c[6:].isdigit()]
        self.assertEqual(digits, [])
        self.assertNotIn("click:OK", board.calls)
        self.assertEqual(board.exec_state, "idle")

    def test_target_below_320_px_budget_fails(self):
        board = FakeBoard(extra_edit_target={"name": "low", "cx": 10, "cy": 400, "hidden": False})
        result, _ = _run(board)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("320", result.reason)


class JudgeTest(unittest.TestCase):
    ORIG = {"seg0_target_c": 35.0, "seg0_dwell_min": 10.0, "seg1_target_c": 45.0, "seg1_dwell_min": 5.0}
    EXP = {"seg0_target_c": 35.0, "seg0_dwell_min": 10.0, "seg1_target_c": 50.0, "seg1_dwell_min": 10.0}

    def _judge(self, seg1_target=50.0, seg1_dwell=10.0):
        status = {"active": True, "working_id": 9, "last_refusal": None}
        content = {"segments": [
            {"target_c": 35.0, "dwell_min": 10.0},
            {"target_c": seg1_target, "dwell_min": seg1_dwell}]}
        ex = {"state_name": "running", "profile_id": 7, "segment_count": 2}
        return J.judge_lcd_edit_firing(self.ORIG, self.EXP, status, content, ex, ex,
                                       {"targets": [{"name": "a", "cx": 1, "cy": 100}], "truncated": False})

    def test_pass(self):
        self.assertEqual(self._judge().verdict, Verdict.PASS)

    def test_unchanged_target_fails(self):
        self.assertEqual(self._judge(seg1_target=45.0).verdict, Verdict.FAIL)

    def test_unchanged_dwell_fails(self):
        self.assertEqual(self._judge(seg1_dwell=5.0).verdict, Verdict.FAIL)

    def test_unreadable_is_inconclusive(self):
        r = J.judge_lcd_edit_firing(self.ORIG, self.EXP, None, None, None, None, None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


if __name__ == "__main__":
    unittest.main()
