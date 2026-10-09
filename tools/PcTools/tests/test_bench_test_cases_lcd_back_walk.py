#!/usr/bin/env python3
"""Unit tests for LCD-26 (cases_lcd._case_lcd26), the Back-button walk.

A coordinate-driven fake board stands in for the panel: a page graph keyed
by (page, tap xy), a touch client that models screen_idle's wake swallow
(power_state / swallow_count / last_swallow_reason), and a ui_test client
that reads the page. Nothing here talks to a real board.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_lcd_back_walk.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_lcd as C  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402
from kilnctrl.devices_touch import (  # noqa: E402
    TOUCH_POWER_STATE_OFF,
    TOUCH_POWER_STATE_ON,
    TOUCH_SWALLOW_REASON_ERROR_HOLD,
    TOUCH_SWALLOW_REASON_NONE,
    TOUCH_SWALLOW_REASON_WAKE,
)

_START_XY = (424, 294)  # home Start button: must never be tapped

_PATCHERS: list = []


def setUpModule():
    for name, val in (
        ("_PAGE_POLL_TIMEOUT_S", 0.03), ("_PAGE_POLL_INTERVAL_S", 0.005),
        ("_WAKE_SCREEN_ON_TIMEOUT_S", 0.03), ("_WAKE_SCREEN_ON_POLL_S", 0.005),
        ("_LCD26_TAP_SETTLE_S", 0.0),
    ):
        p = mock.patch.object(C, name, val)
        p.start()
        _PATCHERS.append(p)


def tearDownModule():
    for p in _PATCHERS:
        p.stop()
    _PATCHERS.clear()


def _t(name, cx, cy):
    return {"name": name, "cx": cx, "cy": cy, "hidden": False}


class FakeBoard:
    """Page graph. ``back_dest`` overrides where a page's Back lands."""

    def __init__(self, safety=False, profiles=True, back_dest=None, dead_back=None):
        self.page = "home"
        self.asleep = False
        self.swallow_count = 0
        self.last_reason = TOUCH_SWALLOW_REASON_NONE
        self.swallow_next = 0          # swallow this many upcoming real taps (counted)
        self.silent_swallow_next = 0   # swallow taps WITHOUT bumping swallow_count
        self.wake_reports_reason = True
        self.taps = []
        self.safety = safety
        self.profiles = profiles
        self.dead_back = dead_back or set()
        back = {
            "config": "home", "temperature": "config", "diagnostics": "config",
            "network": "config", "network_manage": "network", "profiles": "config",
            "profile_detail": "profiles", "profile_segments": "profile_detail",
            "profile_builder_zones": "profiles", "safety": "config",
        }
        back.update(back_dest or {})
        bx = C._LCD26_BACK_X
        self.graph = {"home": [(C._LCD26_GEAR, "config")]}
        cfg = [(xy, p) for p, xy in C._LCD26_HUB.items()]
        if safety:
            cfg.append(((119, 232), "safety"))
        self.graph["config"] = cfg
        self.graph["network"] = [((240, 120), "network_manage")]
        self.graph["profiles"] = [((240, 76), "profile_detail"), (C._LCD26_PROFILES_ADD, "profile_builder_zones")]
        self.graph["profile_detail"] = [((100, 250), "profile_segments")]
        for page, parent in back.items():
            if page in self.dead_back:
                continue
            x = bx.get(page) or 414
            self.graph.setdefault(page, []).append(((x, 21), parent))

    # --- ui_test surface
    def get_current_page(self):
        return self.page

    def list_tap_targets(self):
        t = {
            "home": [_t("settings", 454, 21), _t("Start", *_START_XY)],
            "config": [_t("Profiles", 119, 80)] + ([_t("safety", 119, 232)] if self.safety else []),
            "network": [_t("Manage networks", 240, 120)],
            "profiles": ([_t("Prof A", 240, 76)] if self.profiles else []) + [_t("back", 294, 21)],
            "profile_detail": [_t("Segments", 100, 250), _t("Start", 300, 250)],
            "safety": [_t("back", 414, 21)],
        }.get(self.page, [])
        return {"targets": t, "truncated": False, "busy": False}

    def click_by_name(self, name):
        if name == "home":
            self.page = "home"
            return {"result": "ok"}
        return {"result": "not_found"}

    # --- touch surface
    def get_state(self):
        return SimpleNamespace(
            screen_on=not self.asleep, idle_ms=0,
            power_state=TOUCH_POWER_STATE_OFF if self.asleep else TOUCH_POWER_STATE_ON,
            swallow_count=self.swallow_count, last_swallow_reason=self.last_reason,
        )

    def inject(self, x, y, pressed):
        if not pressed:
            self.taps.append((x, y))
            self._release(x, y)
        return SimpleNamespace(ok=True)

    def _release(self, x, y):
        if self.asleep:
            self.asleep = False
            self.swallow_count += 1
            self.last_reason = TOUCH_SWALLOW_REASON_WAKE if self.wake_reports_reason else TOUCH_SWALLOW_REASON_NONE
            return
        if self.silent_swallow_next:
            self.silent_swallow_next -= 1
            return
        if self.swallow_next:
            self.swallow_next -= 1
            self.swallow_count += 1
            self.last_reason = TOUCH_SWALLOW_REASON_ERROR_HOLD
            return
        for (tx, ty), dest in self.graph.get(self.page, []):
            if abs(tx - x) <= 20 and abs(ty - y) <= 20:
                self.page = dest
                return


def _run(board):
    srv = SimpleNamespace(_ui_test=board, _touch=board)
    return C._case_lcd26({"srv": srv, "_sleep": lambda s: None})


class Lcd26Test(unittest.TestCase):
    def test_registered_with_judge(self):
        self.assertIn("LCD-26", R.SUITES["lcd"])
        self.assertIs(R.get_case("LCD-26").judge, C._case_lcd26)
        self.assertFalse(R.get_case("LCD-26").heat)

    def test_all_pages_pass_and_unsafe_targets_never_tapped(self):
        b = FakeBoard()
        r = _run(b)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        verified = r.observed["back_verified"]
        for page in ("config", "temperature", "diagnostics", "network", "network_manage",
                     "profiles", "profile_detail", "profile_segments", "profile_builder_zones"):
            self.assertIn(f"{page} back", verified)
        self.assertEqual(b.page, "home")
        self.assertNotIn(_START_XY, b.taps)
        self.assertNotIn((300, 250), b.taps)
        self.assertEqual(r.observed["skipped"], ["safety: page not offered on the config hub of this build"])

    def test_wrong_parent_fails_naming_the_page(self):
        b = FakeBoard(back_dest={"profile_segments": "home"})
        r = _run(b)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("profile_segments back", r.reason)
        self.assertIn("'home'", r.reason)

    def test_dead_back_button_fails(self):
        r = _run(FakeBoard(dead_back={"temperature"}))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("temperature back", r.reason)

    def test_blanked_panel_is_woken_first_and_confirmed(self):
        b = FakeBoard()
        orig = b._release
        state = {"n": 0}

        def blanking(x, y):
            state["n"] += 1
            orig(x, y)
            if state["n"] == 3:
                b.asleep = True  # blanks right after the 3rd real tap

        b._release = blanking
        r = _run(b)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        woken = [s for s in r.observed["steps"] if s.get("wake")]
        self.assertTrue(woken)
        self.assertEqual(woken[0]["wake"][0]["wake_swallow_reason"], TOUCH_SWALLOW_REASON_WAKE)
        self.assertIn((5, 5), b.taps)

    def test_wake_without_wake_reason_is_inconclusive_not_fail(self):
        b = FakeBoard()
        b.asleep = True
        b.wake_reports_reason = False
        b.last_reason = TOUCH_SWALLOW_REASON_ERROR_HOLD
        orig = b._release
        b._release = lambda x, y: (orig(x, y), setattr(b, "last_reason", TOUCH_SWALLOW_REASON_ERROR_HOLD))
        # _wake_and_home wakes it once; blank it again so the case itself must wake
        srv = SimpleNamespace(_ui_test=b, _touch=b)
        with mock.patch.object(C, "_wake_and_home", lambda ctx: b.__setattr__("asleep", True)):
            r = C._case_lcd26({"srv": srv, "_sleep": lambda s: None})
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertNotEqual(r.verdict, Verdict.FAIL)

    def test_one_swallowed_back_tap_is_retried_not_failed(self):
        b = FakeBoard()
        orig = b._release
        n = {"c": 0}

        def swallow_first_back(x, y):
            if b.page == "network_manage" and not n["c"]:
                n["c"] = 1
                b.swallow_next = 1
            orig(x, y)

        b._release = swallow_first_back
        r = _run(b)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        rec = next(s for s in r.observed["steps"] if s["label"] == "network_manage back")
        self.assertEqual(rec["swallowed_attempts"], [1])
        self.assertEqual(rec["attempts"], 2)

    def test_persistently_swallowed_back_is_inconclusive_not_fail(self):
        b = FakeBoard()
        orig = b._release

        def swallow_diag_back(x, y):
            if b.page == "diagnostics":
                b.swallow_next = 1
            orig(x, y)

        b._release = swallow_diag_back
        r = _run(b)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("diagnostics back: swallowed", r.reason)

    def test_unswallowed_dead_tap_still_fails_even_if_swallow_counter_exists(self):
        b = FakeBoard(dead_back={"network_manage"})
        r = _run(b)
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_no_profiles_is_inconclusive(self):
        r = _run(FakeBoard(profiles=False))
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("no profile row", r.reason)

    def test_safety_page_walked_only_when_offered(self):
        b = FakeBoard(safety=True)
        r = _run(b)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertIn("safety back", r.observed["back_verified"])
        self.assertEqual(r.observed["skipped"], [])

    def test_safety_wrong_parent_fails(self):
        r = _run(FakeBoard(safety=True, back_dest={"safety": "home"}))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("safety back", r.reason)

    def test_no_touch_client_is_inconclusive(self):
        b = FakeBoard()
        r = C._case_lcd26({"srv": SimpleNamespace(_ui_test=b), "_sleep": lambda s: None})
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)


if __name__ == "__main__":
    unittest.main()
