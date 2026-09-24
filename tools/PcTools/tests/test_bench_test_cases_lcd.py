#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_lcd -- the fetch half of
LCD-01/08/21. srv._ui_test and lcd_sampler are both mocked; these tests
confirm each _case_XXX fetches the right thing, degrades gracefully when
the camera is unavailable, and hands the right data to judgments.py --
never that a real board or webcam answers a certain way.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_lcd.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_lcd as C  # noqa: E402
from kilnctrl.bench_test import cases_web_rw as CW  # noqa: E402
from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test import lcd_sampler  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


class FakeUiTest:
    def __init__(self, page="home", targets=None, click_result="ok"):
        self._page = page
        self._targets = targets if targets is not None else []
        self._click_result = click_result

    def get_current_page(self):
        return self._page

    def list_tap_targets(self):
        return {"targets": self._targets, "truncated": False}

    def click_by_name(self, name):
        if self._click_result == "ok":
            self._page = "config"
        return {"result": self._click_result, "cx": 0, "cy": 0}


class FakeSrv:
    def __init__(self, ui_test):
        self._ui_test = ui_test


_HOME_TARGETS = [
    {"name": "start", "cx": 240, "cy": 280, "hidden": False},
    {"name": "pause", "cx": 240, "cy": 280, "hidden": True},
]


class Lcd01Test(unittest.TestCase):
    def test_camera_unavailable_degrades_to_inconclusive(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd01({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_wrong_page_fails_without_touching_the_camera(self):
        srv = FakeSrv(FakeUiTest(page="profiles", targets=_HOME_TARGETS))
        with mock.patch.object(lcd_sampler, "capture_full_frame") as cap:
            result = C._case_lcd01({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        cap.assert_not_called()

    def test_successful_capture_feeds_camera_data_through(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        fake_sample = lcd_sampler.RegionSample(region=(0x5C, 0xC0, 0x6E), bezel=(26, 31, 43))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", return_value=fake_sample):
            result = C._case_lcd01({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_records_visited_page_in_ctx(self):
        ctx = {"srv": FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))}
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            C._case_lcd01(ctx)
        self.assertIn("home", ctx["_lcd_pages_visited"])


_CONFIG_TARGETS = [
    {"name": "Profiles", "cx": 100, "cy": 100, "hidden": False},
    {"name": "Temperature", "cx": 200, "cy": 100, "hidden": False},
    {"name": "Network", "cx": 300, "cy": 100, "hidden": False},
    {"name": "Diagnostics", "cx": 100, "cy": 200, "hidden": False},
]


class Lcd08Test(unittest.TestCase):
    def test_menu_tap_reaches_config_hub(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))
        result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_menu_tap_not_found_fails(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="not_found"))
        result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("not_found", result.reason)

    def test_records_visited_page_in_ctx(self):
        ctx = {"srv": FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))}
        C._case_lcd08(ctx)
        self.assertIn("config", ctx["_lcd_pages_visited"])


class FakeSafetyDiag:
    def __init__(self, trip_reason=0, trip_mask=None):
        self.trip_reason = trip_reason
        self.trip_mask = trip_mask if trip_mask is not None else (1 << (trip_reason - 1) if trip_reason else 0)


class FakeSafety:
    def __init__(self, diag=None):
        self._diag = diag or FakeSafetyDiag()

    def get_diag(self):
        return self._diag


class FakeExecStatus:
    def __init__(self, state_name="idle", zones=None):
        self.state_name = state_name
        self.zones = zones or []


class FakeProfiles:
    def __init__(self, status=None):
        self._status = status or FakeExecStatus()

    def get_exec_status(self):
        return self._status


class FakeThermo:
    def __init__(self, readings=None):
        self._readings = readings or {}

    def read_all(self):
        return self._readings


class FakeSrvFull(FakeSrv):
    """Extends FakeSrv with the extra client attributes LCD-02/03/04/09/14/16/19
    read (safety/profiles/thermo, and safety_clear_trip() at module scope)."""

    def __init__(self, ui_test, safety=None, profiles=None, thermo=None, clear_trip_raises=False):
        super().__init__(ui_test)
        self._safety = safety or FakeSafety()
        self._profiles = profiles or FakeProfiles()
        self._thermo = thermo or FakeThermo()
        self._clear_trip_raises = clear_trip_raises
        self.clear_trip_called = False

    def safety_clear_trip(self):
        self.clear_trip_called = True
        if self._clear_trip_raises:
            raise RuntimeError("clear_trip failed")

    def get_heap_status(self):
        return "heap free: 100000 bytes"


class PageNavUiTest(FakeUiTest):
    """A UI double that actually tracks which page a named click lands on,
    via an explicit name->page map, and a per-page targets map -- unlike
    the base FakeUiTest (used by LCD-01/08/21) which always lands on a
    single fixed page regardless of which name was clicked."""

    def __init__(self, page="home", page_targets=None, nav_map=None, click_result="ok"):
        super().__init__(page=page, targets=(page_targets or {}).get(page, []), click_result=click_result)
        self._page_targets = page_targets or {}
        self._nav_map = nav_map or {}

    def list_tap_targets(self):
        return {"targets": self._page_targets.get(self._page, []), "truncated": False}

    def click_by_name(self, name):
        if self._click_result != "ok":
            return {"result": self._click_result, "cx": 0, "cy": 0}
        dest = self._nav_map.get(name)
        if dest is not None:
            self._page = dest
        return {"result": "ok", "cx": 0, "cy": 0}


class RaisingUiTest(PageNavUiTest):
    """A UI double whose click_by_name raises on the Nth call (after any
    earlier calls have already navigated), to exercise the
    finally-restore-on-exception path with the board actually having left
    'home' by the time the exception hits."""

    def __init__(self, *a, raise_on_call=2, **kw):
        super().__init__(*a, **kw)
        self._call_count = 0
        self._raise_on_call = raise_on_call
        self.home_calls = 0

    def click_by_name(self, name):
        self._call_count += 1
        if name == "home":
            self.home_calls += 1
            self._page = "home"
            return {"result": "ok"}
        if name == "back":
            return {"result": "not_found"}
        if self._call_count == self._raise_on_call:
            raise RuntimeError("board went unresponsive")
        return super().click_by_name(name)


class BackThenNoHomeUiTest(PageNavUiTest):
    """Mirrors the real topbar's per-page icon set for _navigate_home's
    home-first ordering: 'back' walks a page toward home one hop at a time
    (diagnostics -> config -> home, config's own Back), while 'home' is a
    no-op (not_found) on config specifically -- config hub has no Home icon
    (show_home=false) -- and otherwise jumps straight to home."""

    def click_by_name(self, name):
        if name == "home":
            if self._page == "config":
                return {"result": "not_found", "cx": 0, "cy": 0}
            self._page = "home"
            return {"result": "ok", "cx": 0, "cy": 0}
        if name == "back":
            dest = self._nav_map.get(self._page)
            if dest is not None:
                self._page = dest
            return {"result": "ok", "cx": 0, "cy": 0}
        return super().click_by_name(name)


class NavigateHomeTest(unittest.TestCase):
    def test_back_then_home_from_diagnostics_via_config(self):
        # diagnostics has no direct Home icon target in this double's map,
        # only a Back that lands on config; config itself has no Home icon
        # (show_home=false) and its own Back reaches home. A "back"-first
        # ordering would click "back" (diagnostics -> config), then "home"
        # (not_found on config) and strand on config; "home"-first either
        # jumps straight home from a page that has it, or falls through to
        # "back" from config, which does reach home.
        ui = BackThenNoHomeUiTest(page="diagnostics", nav_map={"diagnostics": "config", "config": "home"})
        C._navigate_home(ui)
        self.assertEqual(ui.get_current_page(), "home")


class Lcd02Test(unittest.TestCase):
    def test_not_run_when_hp01_absent(self):
        srv = FakeSrvFull(FakeUiTest(page="home", targets=_HOME_TARGETS))
        result = C._case_lcd02({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_wrong_page_fails(self):
        srv = FakeSrvFull(FakeUiTest(page="profiles", targets=_HOME_TARGETS))
        result = C._case_lcd02({"srv": srv, "_hp01": {}})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_camera_unavailable_degrades_to_inconclusive(self):
        targets = [dict(t, label="Stop") if t["name"] == "start" else t for t in _HOME_TARGETS]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=targets))
        hp01 = {"progress_samples": [], "profile_name_greyed": None, "profile_name_tap_noop": None}
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd02({"srv": srv, "_hp01": hp01})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_full_data_passes(self):
        targets = [
            {"name": "start", "cx": 240, "cy": 280, "hidden": False, "label": "Stop"},
            {"name": "pause", "cx": 200, "cy": 280, "hidden": False},
            {"name": "profile_name", "cx": 240, "cy": 30, "hidden": False},
        ]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=targets))
        hp01 = {"progress_samples": [1.0, 90.0], "profile_name_greyed": True, "profile_name_tap_noop": True}
        fake_sample = lcd_sampler.RegionSample(region=(0xE8, 0x97, 0x4E), bezel=(26, 31, 43))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", return_value=fake_sample):
            result = C._case_lcd02({"srv": srv, "_hp01": hp01})
        self.assertEqual(result.verdict, Verdict.PASS)


class Lcd03Test(unittest.TestCase):
    def test_not_run_when_hp04_absent(self):
        result = C._case_lcd03({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_resume_label_zero_duty_passes(self):
        ctx = {"_hp04": {
            "pause_ui_targets": [{"name": "pause", "label": "Resume"}],
            "duties_while_paused": [0.0, 0.0],
        }}
        result = C._case_lcd03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_nonzero_duty_fails(self):
        ctx = {"_hp04": {
            "pause_ui_targets": [{"name": "pause", "label": "Resume"}],
            "duties_while_paused": [0.0, 0.5],
        }}
        result = C._case_lcd03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class Lcd04Test(unittest.TestCase):
    def test_not_run_when_no_trip_latched(self):
        srv = FakeSrvFull(FakeUiTest(page="home"), safety=FakeSafety(FakeSafetyDiag(trip_reason=0)))
        result = C._case_lcd04({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)
        self.assertFalse(srv.clear_trip_called)

    def test_camera_unavailable_degrades_to_inconclusive(self):
        strip_targets = [{"name": "trip_strip", "cx": 240, "cy": 10, "hidden": False}]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=strip_targets), safety=FakeSafety(FakeSafetyDiag(trip_reason=6)))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd04({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertTrue(srv.clear_trip_called)

    def test_unexpected_trip_mask_refuses_to_clear(self):
        # trip_reason=6 (S6a) implies mask 0x0020 by the formula in CLAUDE.md;
        # a board reporting a different mask must never be cleared blind.
        srv = FakeSrvFull(FakeUiTest(page="home"), safety=FakeSafety(FakeSafetyDiag(trip_reason=6, trip_mask=0x0040)))
        result = C._case_lcd04({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)
        self.assertFalse(srv.clear_trip_called)

    def test_full_data_passes_and_clears(self):
        strip_targets = [{"name": "trip_strip", "cx": 240, "cy": 10, "hidden": False}]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=strip_targets), safety=FakeSafety(FakeSafetyDiag(trip_reason=6)))
        region_sample = lcd_sampler.RegionSample(region=(0xD6, 0x55, 0x5F), bezel=(26, 31, 43))
        off_sample = lcd_sampler.RegionSample(region=(26, 31, 43), bezel=(26, 31, 43))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=[region_sample, off_sample]):
            result = C._case_lcd04({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertTrue(srv.clear_trip_called)


_PROFILES_NAV = {"settings": "config", "Profiles": "profiles", "profile_row_0": "profile_detail"}
_PROFILES_PAGE_TARGETS = {
    "home": [{"name": "settings", "cx": 10, "cy": 10, "hidden": False}],
    "config": [{"name": "Profiles", "cx": 100, "cy": 100, "hidden": False}],
    "profiles": [
        {"name": "profile_row_0", "cx": 50, "cy": 150, "hidden": False, "starred": False},
        {"name": "paging", "cx": 400, "cy": 300, "hidden": False},
        {"name": "new_profile", "cx": 400, "cy": 10, "hidden": False},
    ],
    "profile_detail": [],
}


class Lcd09Test(unittest.TestCase):
    def test_passes_with_valid_picker(self):
        ui = PageNavUiTest(page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV)
        srv = FakeSrvFull(ui)
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_menu_tap_failure_fails(self):
        ui = PageNavUiTest(page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV, click_result="not_found")
        srv = FakeSrvFull(ui)
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_finally_restores_home_on_exception(self):
        ui = RaisingUiTest(page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd09({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)


_TEMP_NAV = {"settings": "config", "Temperature": "temperature"}


class Lcd14Test(unittest.TestCase):
    def test_matching_values_pass(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Temperature", "hidden": False}],
            "temperature": [{"name": "zone_temp_0", "value": 100.0}, {"name": "safety_line", "on": False}],
        }
        ui = PageNavUiTest(page="home", page_targets=page_targets, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 100.0}), profiles=FakeProfiles(FakeExecStatus("idle")))
        result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_value_mismatch_fails(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Temperature", "hidden": False}],
            "temperature": [{"name": "zone_temp_0", "value": 100.0}, {"name": "safety_line", "on": False}],
        }
        ui = PageNavUiTest(page="home", page_targets=page_targets, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 150.0}), profiles=FakeProfiles(FakeExecStatus("idle")))
        result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_finally_restores_home_on_exception(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Temperature", "hidden": False}],
        }
        ui = RaisingUiTest(page="home", page_targets=page_targets, nav_map=_TEMP_NAV, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd14({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)


class Lcd16Test(unittest.TestCase):
    def test_finally_restores_home_on_exception(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Diagnostics", "hidden": False}],
        }
        nav_map = {"settings": "config", "Diagnostics": "diag_hub"}
        ui = RaisingUiTest(page="home", page_targets=page_targets, nav_map=nav_map, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd16({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)

    def test_menu_tap_failure_fails(self):
        targets = [{"name": "settings", "hidden": False}]
        ui = FakeUiTest(page="home", targets=targets, click_result="not_found")
        srv = FakeSrvFull(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)


class PopupUiTest(FakeUiTest):
    """A UiTestClient double for LCD-19: both the PIN keypad and the Confirm
    Start/Confirm Stop dialogs are top-layer popups that never change
    current_page (kiln_ui.c) -- this fake models that by keeping `_page`
    fixed at "home" and instead tracking an overlay's tap-target names,
    which appear only after `trigger_name` is clicked and disappear once
    "Cancel" is clicked (never any other button -- the case must never
    press the popup's own confirm button)."""

    def __init__(self, trigger_name, overlay_names):
        super().__init__(page="home", targets=[])
        self._trigger_name = trigger_name
        self._overlay_names = overlay_names
        self._overlay_open = False

    def list_tap_targets(self):
        names = self._overlay_names if self._overlay_open else []
        return {"targets": [{"name": n, "hidden": False} for n in names], "truncated": False}

    def click_by_name(self, name):
        if name == self._trigger_name and not self._overlay_open:
            self._overlay_open = True
            return {"result": "ok"}
        if name == "Cancel" and self._overlay_open:
            self._overlay_open = False
            return {"result": "ok"}
        return {"result": "not_found"}


class PinKeypadUiTest(FakeUiTest):
    """Models the real ui_lcd_keypad.c/ui_confirm.c state machine for
    LCD-19's idle-branch happy path, closely enough for enter_pin() to be
    exercised meaningfully: Start opens the keypad (digits 0-9 + "OK" +
    "Cancel"); submitting a wrong PIN resets the entry but leaves the
    keypad open ("OK" still present, same as lcd_keypad_state_submit()'s
    LCD_KEYPAD_SUBMIT_DENIED path); submitting the right PIN closes the
    keypad and opens the Confirm Start dialog ("OK" gone, "Start"/"Cancel"
    only)."""

    def __init__(self, right_pin: str, wrong_pin: str):
        super().__init__(page="home", targets=[])
        self._right_pin = right_pin
        self._wrong_pin = wrong_pin
        self._state = "idle"  # idle -> keypad -> confirm
        self._entry = ""

    def list_tap_targets(self):
        if self._state == "keypad":
            names = [str(d) for d in range(10)] + ["OK", "Cancel"]
        elif self._state == "confirm":
            names = ["Start", "Cancel"]
        else:
            names = []
        return {"targets": [{"name": n, "hidden": False} for n in names], "truncated": False}

    def click_by_name(self, name):
        if self._state == "idle":
            if name == "Start":
                self._state = "keypad"
                self._entry = ""
                return {"result": "ok"}
            return {"result": "not_found"}
        if self._state == "keypad":
            if name in "0123456789":
                self._entry += name
                return {"result": "ok"}
            if name == "OK":
                if self._entry == self._right_pin:
                    self._state = "confirm"
                else:
                    self._entry = ""
                return {"result": "ok"}
            if name == "Cancel":
                self._state = "idle"
                return {"result": "ok"}
            return {"result": "not_found"}
        if self._state == "confirm":
            if name == "Cancel":
                self._state = "idle"
                return {"result": "ok"}
            return {"result": "not_found"}
        return {"result": "not_found"}

    def enter_pin(self, pin):
        digit_results = [self.click_by_name(ch) for ch in pin]
        ok_result = self.click_by_name("OK")
        return {"digit_results": digit_results, "ok_result": ok_result}


class FakeLcd19SecClient:
    """Minimal fake for LCD-19's own lcd_enabled on/off toggle (fix 1):
    tracks policy state across get_config/set_policy so the enable-before,
    restore-after round trip is real, not trivially true. Defaults to the
    bench's normal off state (web_enabled=False, lcd_enabled=False)."""

    def __init__(self, web_enabled=False, lcd_enabled=False,
                 web_timeout_min=30, lcd_timeout_min=30,
                 enable_ok=True, restore_ok=True):
        self._cfg = {
            "web_enabled": web_enabled, "lcd_enabled": lcd_enabled,
            "web_timeout_min": web_timeout_min, "lcd_timeout_min": lcd_timeout_min,
        }
        self.enable_ok = enable_ok
        self.restore_ok = restore_ok
        self.set_policy_calls = []

    def get_config(self):
        return 200, dict(self._cfg)

    def set_policy(self, web_enabled, lcd_enabled, web_timeout_min, lcd_timeout_min):
        self.set_policy_calls.append(lcd_enabled)
        if lcd_enabled and not self.enable_ok:
            return 200, {"ok": False}
        if not lcd_enabled and not self.restore_ok:
            return 200, {"ok": False}
        self._cfg["web_enabled"] = web_enabled
        self._cfg["lcd_enabled"] = lcd_enabled
        self._cfg["web_timeout_min"] = web_timeout_min
        self._cfg["lcd_timeout_min"] = lcd_timeout_min
        return 200, {"ok": True}


class Lcd19Test(unittest.TestCase):
    def test_not_run_when_no_pin_configured(self):
        srv = FakeSrvFull(FakeUiTest(page="home"))
        result = C._case_lcd19({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_owns_its_own_lcd_enabled_toggle_and_restores_it_off(self):
        # Fix 1: LCD-19 must not depend on WEB-SEC-04 leaving lcd_enabled on
        # -- it turns the policy on itself before driving the keypad and
        # restores the original (off) value afterward, verified by readback.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        sec = FakeLcd19SecClient(lcd_enabled=False)
        ctx = {"srv": srv, "sec_client": sec, "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        # True while the keypad was being driven, then restored to False.
        self.assertIn(True, sec.set_policy_calls)
        self.assertEqual(sec.set_policy_calls[-1], False)
        self.assertEqual(sec._cfg["lcd_enabled"], False)
        self.assertTrue(result.observed["restore"]["readback_matches"])

    def test_enable_not_confirmed_fails_without_touching_keypad(self):
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        sec = FakeLcd19SecClient(enable_ok=False)
        ctx = {"srv": srv, "sec_client": sec, "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(ui, "click_by_name", wraps=ui.click_by_name) as spy:
            result = C._case_lcd19(ctx)
        spy.assert_not_called()
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_restore_failure_fails_even_if_keypad_flow_passed(self):
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        sec = FakeLcd19SecClient(restore_ok=False)
        ctx = {"srv": srv, "sec_client": sec, "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("restore", result.reason)

    def test_idle_click_uses_exact_start_label_and_detects_keypad(self):
        # kiln_ui_click_by_name() (firmware/KilnFW/App/drivers/ui/kiln_ui.c)
        # matches with an exact strcmp; the home fire button's label is
        # exactly "Start" when idle (ui_page_home.c). A lowercase "start"
        # never matches and always returns NOT_FOUND. Tapping "Start" opens
        # the PIN keypad (ui_lcd_keypad.c: digit buttons + "OK" + "Cancel"),
        # a top-layer popup -- current_page stays "home", so the keypad must
        # be detected from tap-target names, never a page name (no page is
        # ever named "pin_entry").
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        with mock.patch.object(ui, "click_by_name", wraps=ui.click_by_name) as spy:
            ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
            result = C._case_lcd19(ctx)
        spy.assert_any_call("Start")
        self.assertNotIn(mock.call("start"), spy.call_args_list)
        self.assertEqual(result.observed.get("keypad_raised"), True)
        # enter_pin() now drives the keypad: a wrong PIN is refused (keypad
        # stays open) and the right PIN opens Confirm Start. stop_not_gated
        # is never observed on the idle branch, so the case still tops out
        # at INCONCLUSIVE, not PASS -- see judge_lcd_pin_lock.
        self.assertEqual(result.observed.get("wrong_pin_refused"), True)
        self.assertEqual(result.observed.get("right_pin_started"), True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        # The Confirm Start dialog left open by the right PIN must not be
        # left open on the board afterward.
        self.assertEqual(result.observed["overlay_dismiss"]["present"], True)
        self.assertTrue(result.observed["overlay_dismiss"]["dismissed"])
        self.assertEqual(ui.list_tap_targets()["targets"], [])

    def test_wrong_pin_actually_granted_fails(self):
        # Negative test: if a wrong PIN were incorrectly accepted (keypad
        # closes to Confirm Start instead of staying open), this must FAIL,
        # not read as refused. Model that by making the fake's "right_pin"
        # equal to the configured wrong_pin -- i.e. the board grants entry
        # on the wrong PIN.
        ui = PinKeypadUiTest(right_pin="0000", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.observed.get("wrong_pin_refused"), False)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_stop_opens_confirm_dialog_directly_not_gated(self):
        # A Confirm Stop dialog (ui_confirm.c) has only its confirm_label
        # ("Stop") + "Cancel" -- no "OK" -- distinguishing it from the PIN
        # keypad. Stop landing directly on Confirm Stop (no keypad) means
        # Stop was correctly never gated.
        ui = PopupUiTest(trigger_name="Stop", overlay_names=["Stop", "Cancel"])
        srv = FakeSrvFull(ui)
        with mock.patch.object(ui, "click_by_name", wraps=ui.click_by_name) as spy:
            ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000", "firing_active_with_lock": True}}
            result = C._case_lcd19(ctx)
        self.assertEqual(spy.call_args_list[0], mock.call("Stop"))
        self.assertNotIn(mock.call("Start"), spy.call_args_list)
        # Dismissal in `finally` must use "Cancel" only, never the dialog's
        # own confirm button.
        self.assertEqual(spy.call_args_list[-1], mock.call("Cancel"))
        self.assertEqual(result.observed.get("stop_not_gated"), True)
        self.assertTrue(result.observed["overlay_dismiss"]["dismissed"])

    def test_stop_gated_behind_keypad_fails(self):
        # If the PIN keypad ("OK" present) appears instead of Confirm Stop
        # directly, Stop was gated -- a safety regression -- and this must
        # be a real FAIL, not vacuously always True.
        ui = PopupUiTest(trigger_name="Stop", overlay_names=["1", "2", "3", "OK", "Cancel"])
        srv = FakeSrvFull(ui)
        with mock.patch.object(ui, "click_by_name", wraps=ui.click_by_name) as spy:
            ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000", "firing_active_with_lock": True}}
            result = C._case_lcd19(ctx)
        self.assertEqual(spy.call_args_list[0], mock.call("Stop"))
        self.assertNotIn(mock.call("Start"), spy.call_args_list)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("Stop", result.reason)
        self.assertTrue(result.observed["overlay_dismiss"]["dismissed"])

    def test_dismiss_overlay_not_needed_when_nothing_open(self):
        ui = PopupUiTest(trigger_name="Start", overlay_names=[])
        # Simulate a click that reports ok but raises nothing (defensive):
        ui._trigger_name = None  # click never opens an overlay
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.observed["overlay_dismiss"], {"checked": True, "present": False})


class _SharedBoardSecClient:
    """One fake board's policy/PIN state, shared across both WEB-SEC-04 and
    LCD-19 in :class:`WebSec04ThenLcd19Test` -- proves the fix-1 sequencing
    actually works end to end: WEB-SEC-04 restores lcd_enabled to its
    original (off) value in its own finally, and LCD-19 must then be able to
    turn it back on for its own run and restore it off again afterward,
    against the SAME underlying state rather than two independent fakes."""

    def __init__(self):
        self._cfg = {
            "web_enabled": False, "lcd_enabled": False,
            "web_timeout_min": 30, "lcd_timeout_min": 30,
            "admin_pin_set": False,
        }
        self.set_lcd_pin_calls = []
        self.set_policy_calls = []

    def get_config(self):
        return 200, dict(self._cfg)

    def set_lcd_pin(self, role, pin):
        self.set_lcd_pin_calls.append((role, pin))
        self._cfg["admin_pin_set"] = True
        return 200, {"ok": True}

    def set_policy(self, web_enabled, lcd_enabled, web_timeout_min, lcd_timeout_min):
        self.set_policy_calls.append(lcd_enabled)
        self._cfg["web_enabled"] = web_enabled
        self._cfg["lcd_enabled"] = lcd_enabled
        self._cfg["web_timeout_min"] = web_timeout_min
        self._cfg["lcd_timeout_min"] = lcd_timeout_min
        return 200, {"ok": True}


class WebSec04ThenLcd19Test(unittest.TestCase):
    """Fix 1's required test: run WEB-SEC-04 then LCD-19 against ONE shared
    fake board state, proving lcd_enabled ends up off after WEB-SEC-04 (its
    own restore), gets turned on by LCD-19 for its own run, and ends up off
    again after LCD-19 (its own restore) -- never relying on one case's
    leftover policy state for the other's keypad to appear."""

    def test_lcd19_turns_policy_on_itself_after_web_sec04_restores_it_off(self):
        os.environ[CW._LCD_PIN_ENV] = "1234"
        try:
            sec = _SharedBoardSecClient()
            ctx = {"sec_client": sec}

            sec04_result = CW._case_web_sec04(ctx)
            self.assertEqual(sec04_result.verdict, Verdict.PASS)
            # WEB-SEC-04 must leave lcd_enabled exactly as it found it (off).
            self.assertEqual(sec._cfg["lcd_enabled"], False)
            self.assertIn("_lcd_pin", ctx)

            ui = PinKeypadUiTest(right_pin="1234", wrong_pin=ctx["_lcd_pin"]["wrong_pin"])
            ctx["srv"] = FakeSrvFull(ui)
            lcd19_result = C._case_lcd19(ctx)

            # LCD-19 must have turned lcd_enabled on for its own run (else
            # the keypad could never have appeared) and restored it off
            # again afterward -- against the SAME shared board state.
            self.assertIn(True, sec.set_policy_calls)
            self.assertEqual(sec._cfg["lcd_enabled"], False)
            self.assertTrue(lcd19_result.observed["restore"]["readback_matches"])
            self.assertEqual(lcd19_result.observed.get("wrong_pin_refused"), True)
            self.assertEqual(lcd19_result.observed.get("right_pin_started"), True)
        finally:
            os.environ.pop(CW._LCD_PIN_ENV, None)


class DelayedPageNavUiTest(PageNavUiTest):
    """Models the click_by_name()-then-read race (2026-09-24 bench finding):
    click_by_name() reports 'ok' immediately, but the destination page name
    only becomes visible after `flips_after` subsequent get_current_page()
    polls -- the real LVGL-task-poll delay collapsed to "N calls" so the
    test runs instantly. `flips_after=None` means the page never flips at
    all (the never-arrives case)."""

    def __init__(self, *a, flips_after=1, **kw):
        super().__init__(*a, **kw)
        self._flips_after = flips_after
        self._pending_dest = None
        self._poll_count = 0

    def click_by_name(self, name):
        if self._click_result != "ok":
            return {"result": self._click_result, "cx": 0, "cy": 0}
        dest = self._nav_map.get(name)
        if dest is not None:
            self._pending_dest = dest
            self._poll_count = 0
        return {"result": "ok", "cx": 0, "cy": 0}

    def get_current_page(self):
        if self._pending_dest is not None and self._flips_after is not None:
            self._poll_count += 1
            if self._poll_count > self._flips_after:
                self._page = self._pending_dest
                self._pending_dest = None
        return self._page

    def list_tap_targets(self):
        return {"targets": self._page_targets.get(self._page, []), "truncated": False}


class Lcd08PollTest(unittest.TestCase):
    """LCD-08 specifically reproduces the bench log: click_by_name('settings')
    succeeds but an immediate page read still says 'home'."""

    def test_page_flips_one_poll_after_click_still_passes(self):
        ui = DelayedPageNavUiTest(
            page="home",
            page_targets={"home": [], "config": _CONFIG_TARGETS},
            nav_map={"settings": "config"},
            flips_after=1,
        )
        srv = FakeSrv(ui)
        result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertIn("page_wait_s", result.observed)

    def test_page_never_flips_is_still_a_fail(self):
        ui = DelayedPageNavUiTest(
            page="home",
            page_targets={"home": [], "config": _CONFIG_TARGETS},
            nav_map={"settings": "config"},
            flips_after=None,
        )
        srv = FakeSrv(ui)
        result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("home", result.reason)


class Lcd09PollTest(unittest.TestCase):
    def test_page_flips_one_poll_after_each_click_still_passes(self):
        ui = DelayedPageNavUiTest(
            page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV, flips_after=1,
        )
        srv = FakeSrvFull(ui)
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)


class Lcd14PollTest(unittest.TestCase):
    def test_page_flips_one_poll_after_each_click_still_passes(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Temperature", "hidden": False}],
            "temperature": [{"name": "zone_temp_0", "value": 100.0}, {"name": "safety_line", "on": False}],
        }
        ui = DelayedPageNavUiTest(page="home", page_targets=page_targets, nav_map=_TEMP_NAV, flips_after=1)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 100.0}), profiles=FakeProfiles(FakeExecStatus("idle")))
        result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)


class DelayedDiagUiTest(PageNavUiTest):
    """LCD-16's diagnostics sub-tabs never change the top-level page name
    (ui_page_diagnostics.c's own internal s_pages[]), so the race there
    shows up in list_tap_targets(), not get_current_page(). Models a
    per-title tap-target set that only updates `flips_after` polls after
    the tab's click_by_name()."""

    def __init__(self, *a, tab_targets=None, flips_after=1, **kw):
        super().__init__(*a, **kw)
        self._tab_targets = tab_targets or {}
        self._flips_after = flips_after
        self._pending_tab = None
        self._current_tab_targets: "list" = []
        self._poll_count = 0

    def click_by_name(self, name):
        if self._click_result != "ok":
            return {"result": self._click_result, "cx": 0, "cy": 0}
        dest = self._nav_map.get(name)
        if dest is not None:
            self._page = dest
        if name in self._tab_targets:
            self._pending_tab = name
            self._poll_count = 0
        return {"result": "ok", "cx": 0, "cy": 0}

    def list_tap_targets(self):
        if self._pending_tab is not None and self._flips_after is not None:
            self._poll_count += 1
            if self._poll_count > self._flips_after:
                self._current_tab_targets = self._tab_targets[self._pending_tab]
                self._pending_tab = None
        if self._page != "config" and self._page != "home":
            return {"targets": self._current_tab_targets, "truncated": False}
        return {"targets": self._page_targets.get(self._page, []), "truncated": False}


class Lcd16PollTest(unittest.TestCase):
    def test_relay_life_tab_targets_arrive_one_poll_late(self):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}],
            "config": [{"name": "Diagnostics", "hidden": False}],
        }
        nav_map = {"settings": "config", "Diagnostics": "diagnostics"}
        tab_targets = {t: [] for t in J._DIAG_TITLES}
        tab_targets["Relay Life"] = [{"name": "reset", "hidden": False}]
        ui = DelayedDiagUiTest(page="home", page_targets=page_targets, nav_map=nav_map,
                                tab_targets=tab_targets, flips_after=1)
        srv = FakeSrvFull(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.observed.get("relay_life_has_reset"), True)


class FakeTouchThatWakes:
    """A TouchClient double: inject() records the call and wakes the
    SwallowingUiTest it is paired with, modeling `screen_idle.c` treating a
    real touch_inject press/release as a genuine touch for the idle timer
    and wake decision."""

    def __init__(self, ui, idle_ms=0, state_error=None):
        self._ui = ui
        self.injected = []
        self.idle_ms = idle_ms
        self.state_error = state_error

    def get_state(self):
        if self.state_error is not None:
            raise self.state_error
        return mock.Mock(screen_on=not self._ui._asleep, idle_ms=self.idle_ms)

    def inject(self, x, y, pressed):
        self.injected.append((x, y, pressed))
        self._ui.wake()


class SwallowingUiTest(PageNavUiTest):
    """Models `screen_idle.c`/`lvgl_port.c`'s touch-swallow: while asleep,
    `click_by_name()` still replies 'ok' (matching the real firmware's
    behavior -- it only reports whether a target by that name exists, not
    whether the tap reached it) but never actually navigates. The first
    real touch_inject (via `wake()`) clears the asleep flag, same as a real
    panel waking on a tap."""

    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self._asleep = True

    def wake(self):
        self._asleep = False

    def click_by_name(self, name):
        if self._asleep:
            return {"result": "ok", "cx": 0, "cy": 0}
        return super().click_by_name(name)


class FakeSrvWithTouch(FakeSrv):
    def __init__(self, ui_test):
        super().__init__(ui_test)
        self._touch = FakeTouchThatWakes(ui_test)


class WakeAndHomeTest(unittest.TestCase):
    """Covers `_wake_and_home()` itself, plus the negative test required
    alongside it: a UI double that swallows every click until woken must
    make the OLD path (no wake step) fail, and the NEW path (with the wake
    step) pass -- proving the fix actually closes the 2026-09-24 bench
    race rather than merely adding an unused helper."""

    def test_wake_and_home_injects_touch_before_navigating(self):
        ui = SwallowingUiTest(page="home", page_targets=_CONFIG_TARGETS and {
            "home": [], "config": _CONFIG_TARGETS,
        }, nav_map={"settings": "config"})
        srv = FakeSrvWithTouch(ui)
        ctx = {"srv": srv}
        C._wake_and_home(ctx)
        self.assertEqual(srv._touch.injected, [(5, 5, True), (5, 5, False)])
        self.assertFalse(ui._asleep)
        self.assertEqual(ui.get_current_page(), "home")

    def test_wake_runs_per_case_after_a_re_blank(self):
        """The panel can blank again between cases (shortest timeout is
        1 min): a second call after a re-blank must wake it again, not be
        skipped as already-done-this-run."""
        ui = SwallowingUiTest(page="home", page_targets={"home": []}, nav_map={})
        srv = FakeSrvWithTouch(ui)
        ctx = {"srv": srv}
        C._wake_and_home(ctx)
        self.assertEqual(len(srv._touch.injected), 2)
        ui._asleep = True  # re-blanked between cases
        C._wake_and_home(ctx)
        self.assertEqual(len(srv._touch.injected), 4)
        self.assertFalse(ui._asleep)

    def test_no_wake_tap_on_an_awake_recently_active_panel(self):
        """On an awake panel the (5,5) tap would be delivered for real
        (home's top-left auth-reset corner) -- skip it when not needed."""
        ui = SwallowingUiTest(page="home", page_targets={"home": []}, nav_map={})
        ui._asleep = False
        srv = FakeSrvWithTouch(ui)
        srv._touch.idle_ms = 1000
        C._wake_and_home({"srv": srv})
        self.assertEqual(srv._touch.injected, [])

    def test_wake_tap_sent_when_awake_but_idle_long(self):
        ui = SwallowingUiTest(page="home", page_targets={"home": []}, nav_map={})
        ui._asleep = False
        srv = FakeSrvWithTouch(ui)
        srv._touch.idle_ms = C._WAKE_IDLE_MS_THRESHOLD
        C._wake_and_home({"srv": srv})
        self.assertEqual(len(srv._touch.injected), 2)

    def test_wake_tap_sent_when_state_unreadable(self):
        ui = SwallowingUiTest(page="home", page_targets={"home": []}, nav_map={})
        srv = FakeSrvWithTouch(ui)
        srv._touch.state_error = RuntimeError("GET_STATE timed out")
        C._wake_and_home({"srv": srv})
        self.assertEqual(len(srv._touch.injected), 2)
        self.assertFalse(ui._asleep)

    def test_blanked_screen_swallow_old_path_fails_new_path_passes(self):
        page_targets = {"home": [], "config": _CONFIG_TARGETS}
        nav_map = {"settings": "config"}

        # OLD path: no wake step at all -- click_by_name('settings') reports
        # 'ok' (swallowed-but-ok, per screen_idle.c's real behavior) while
        # the page never actually changes, so LCD-08 must FAIL.
        ui_old = SwallowingUiTest(page="home", page_targets=page_targets, nav_map=nav_map)
        srv_old = FakeSrvWithTouch(ui_old)
        with mock.patch.object(C, "_wake_and_home", lambda ctx: None):
            result_old = C._case_lcd08({"srv": srv_old})
        self.assertEqual(result_old.verdict, Verdict.FAIL)

        # NEW path: the real _wake_and_home runs first, sends the wake tap,
        # and the same case now passes.
        ui_new = SwallowingUiTest(page="home", page_targets=page_targets, nav_map=nav_map)
        srv_new = FakeSrvWithTouch(ui_new)
        result_new = C._case_lcd08({"srv": srv_new})
        self.assertEqual(result_new.verdict, Verdict.PASS)


class Lcd21Test(unittest.TestCase):
    def test_uses_pages_visited_by_earlier_cases(self):
        ctx = {"_lcd_pages_visited": {
            "home": {"targets": [{"name": "start", "cx": 1, "cy": 300}], "truncated": False},
        }}
        result = C._case_lcd21(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_no_pages_visited_is_not_run(self):
        result = C._case_lcd21({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_offscreen_target_from_an_earlier_case_fails(self):
        ctx = {"_lcd_pages_visited": {
            "config": {"targets": [{"name": "ghost", "cx": 1, "cy": 500}], "truncated": False},
        }}
        result = C._case_lcd21(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
