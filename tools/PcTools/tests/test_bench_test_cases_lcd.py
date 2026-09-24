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

    def test_start_sample_uses_label_avoid_offset_not_raw_centre(self):
        # 2026-09-24 round 5 regression: the Start button's tap-target centre
        # sits on its own centred label (lv_obj_center(label)), so the case
        # must sample cy + LABEL_AVOID_OFFSET_PX, never the raw cy, or the
        # sample lands on the label glyph. Assert the actual (x, y) passed to
        # sample_widget by way of sample_widget_body.
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        fake_sample = lcd_sampler.RegionSample(region=(0x5C, 0xC0, 0x6E), bezel=(26, 31, 43))
        seen_xy = []

        def fake_sample_widget(image_path, x, y, *args, **kwargs):
            seen_xy.append((x, y))
            return fake_sample

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget):
            C._case_lcd01({"srv": srv})
        start_target = _HOME_TARGETS[0]
        # Pause legitimately shares Start's raw (cx, cy) in this fixture and
        # is sampled unoffset by unchanged code, so a bare assertNotIn on the
        # raw coordinate would collide with that legitimate call. Assert
        # precisely what changed instead: Start's OWN sample used the offset.
        self.assertIn(
            (start_target["cx"], start_target["cy"] + lcd_sampler.LABEL_AVOID_OFFSET_PX),
            seen_xy,
        )

    def test_capture_lands_under_run_dir_captures_and_evidence_recorded(self, ):
        # 2026-09-24 fix: ctx["_lcd_capture_dir"] was never set anywhere, so
        # a captured frame always fell back to the OS tempdir and never
        # landed in the run's own captures/ directory, and CaseResult.evidence
        # stayed empty. A run_dir in ctx must now make the capture land under
        # <run_dir>/captures with its path recorded in evidence.
        import shutil
        import tempfile as _tempfile
        run_dir = _tempfile.mkdtemp(prefix="lcd01_run_")
        try:
            srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
            fake_sample = lcd_sampler.RegionSample(region=(0x5C, 0xC0, 0x6E), bezel=(26, 31, 43))
            captured_paths = []

            def fake_capture(path, repo_root=None):
                captured_paths.append(path)
                with open(path, "wb") as f:
                    f.write(b"\x00")

            with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=fake_capture), \
                 mock.patch.object(lcd_sampler, "sample_widget", return_value=fake_sample):
                result = C._case_lcd01({"srv": srv, "run_dir": run_dir})
            self.assertEqual(result.verdict, Verdict.PASS)
            self.assertTrue(captured_paths)
            self.assertTrue(captured_paths[0].startswith(os.path.join(run_dir, "captures")))
            self.assertTrue(os.path.isdir(os.path.join(run_dir, "captures")))
            self.assertEqual(result.evidence, [captured_paths[0]])
            self.assertIn("color_debug", result.observed)
            self.assertIn("start", result.observed["color_debug"])
            self.assertEqual(result.observed["color_debug"]["start"]["sampled_rgb"], fake_sample.region)
        finally:
            shutil.rmtree(run_dir, ignore_errors=True)

    def test_wrong_color_still_fails_with_numeric_evidence(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        # Bezel is a realistic dark reference (the measured bezel on both
        # 2026-09-24 captures reads ~(6,11,15)), distinctly darker than the
        # theme BG -- a fake bezel equal to _BG_RGB would make every
        # background point, including CORNER_CHECK_POINTS[0] == (5, 5),
        # read as bezel and downgrade this FAIL to INCONCLUSIVE.
        bezel = (6, 11, 15)
        red = lcd_sampler.RegionSample(region=(0xD6, 0x20, 0x20), bezel=bezel)
        bg = lcd_sampler.RegionSample(region=C._BG_RGB, bezel=bezel)

        def fake_sample_widget(image_path, x, y, *args, **kwargs):
            # No color cast in this capture: only the widget itself reads
            # wrong, so the bg-reference sanity check must not fire and
            # mask a genuine defect as INCONCLUSIVE. Background points
            # (the bg reference and every corner-check point) read as lit
            # theme BG; everything else is the wrong-colored widget.
            if (x, y) == C._BG_REFERENCE_XY or (x, y) in lcd_sampler.CORNER_CHECK_POINTS:
                return bg
            return red

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="x.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget):
            result = C._case_lcd01({"srv": srv, "_lcd_capture_dir": "/cap"})
        # The real _downgrade_if_corners_stale() ran and found sound corners.
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertNotIn("frame_corners_stale", result.observed)
        self.assertFalse(result.observed["color_debug"]["bg_reference"]["reads_as_bezel"])
        dbg = result.observed["color_debug"]["start"]
        self.assertFalse(dbg["matches"])
        self.assertEqual(dbg["sampled_rgb"], (0xD6, 0x20, 0x20))
        self.assertGreater(dbg["distance"], dbg["distance_tolerance"])
        self.assertEqual(len(result.evidence), 1)

    def test_readonly_run_dir_never_fails_the_case(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        with mock.patch.object(C.os, "makedirs", side_effect=PermissionError("ro")), \
             mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")) as cap:
            result = C._case_lcd01({"srv": srv, "run_dir": "/ro/run"})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertFalse(cap.call_args[0][0].startswith(os.path.join("/ro/run", "captures")))

    def test_explicit_lcd_capture_dir_overrides_run_dir(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_HOME_TARGETS))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")) as cap:
            C._case_lcd01({"srv": srv, "run_dir": "/should/not/be/used", "_lcd_capture_dir": "/explicit/dir"})
        called_path = cap.call_args[0][0]
        self.assertTrue(called_path.startswith("/explicit/dir") or called_path.startswith(os.path.normpath("/explicit/dir")))


_CONFIG_TARGETS = [
    {"name": "Profiles", "cx": 100, "cy": 100, "hidden": False},
    {"name": "Temperature", "cx": 200, "cy": 100, "hidden": False},
    {"name": "Network / Wi-Fi", "cx": 300, "cy": 100, "hidden": False},
    {"name": "Diagnostics", "cx": 100, "cy": 200, "hidden": False},
]


class Lcd08Test(unittest.TestCase):
    def test_menu_tap_reaches_config_hub(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_menu_tap_not_found_fails(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="not_found"))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("not_found", result.observed.get("attribution", ""))

    def test_records_visited_page_in_ctx(self):
        ctx = {"srv": FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))}
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            C._case_lcd08(ctx)
        self.assertIn("config", ctx["_lcd_pages_visited"])

    def test_retries_once_on_swallowed_tap_then_passes(self):
        # 2026-09-24 fix: _case_lcd08 used to call click_by_name('settings')
        # directly with no retry -- migrated to _click_then_page(), which
        # retries once when the click said 'ok' but the page never moved
        # (the screen_idle swallow race).
        class SwallowOnceUi(FakeUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.calls = 0
                self.settings_calls = 0

            def click_by_name(self, name):
                self.calls += 1
                if name == "settings":
                    self.settings_calls += 1
                    if self.settings_calls == 1:
                        return {"result": "ok", "cx": 0, "cy": 0}  # swallowed: no page change
                    self._page = "config"
                    return {"result": "ok", "cx": 0, "cy": 0}
                # Any other click (e.g. _navigate_home's "home"/"back") is a
                # normal, un-swallowed navigation back to home.
                self._page = "home"
                return {"result": "ok", "cx": 0, "cy": 0}

        ui = SwallowOnceUi(page="home", targets=_CONFIG_TARGETS)
        srv = FakeSrv(ui)
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(ui.settings_calls, 2)

    def test_saves_a_capture_as_evidence(self):
        srv = FakeSrv(FakeUiTest(page="home", targets=_CONFIG_TARGETS, click_result="ok"))
        captured = {}

        def fake_capture(path, **kw):
            captured["path"] = path

        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=fake_capture):
            result = C._case_lcd08({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.evidence, [captured["path"]])
        self.assertTrue(captured["path"].endswith("lcd08_config_hub.jpg"))


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
    def __init__(self, status=None, summaries=None, list_all_raises=None):
        self._status = status or FakeExecStatus()
        self._summaries = summaries if summaries is not None else []
        self._list_all_raises = list_all_raises

    def get_exec_status(self):
        return self._status

    def list_all(self):
        if self._list_all_raises is not None:
            raise self._list_all_raises
        return self._summaries


class FakeThermoReading:
    """Stand-in for kilnctrl.devices_thermo.ThermoReading -- just enough
    surface (.channel/.temperature_c/.valid) for cases_lcd._case_lcd14's
    srv._thermo.read(THERMO_CHANNEL_ALL) call."""

    def __init__(self, channel, temperature_c, valid=True):
        self.channel = channel
        self.temperature_c = temperature_c
        self.valid = valid


class FakeThermo:
    """Real ThermoClient has only .read(channel), never a .read_all() --
    2026-09-24 bench root cause for LCD-14 (cases_lcd.py called a method
    that never existed, always raising AttributeError, silently swallowed).
    `readings` is {channel: temperature_c}, same convenient shape the old
    (nonexistent) read_all() would have returned, translated here into the
    list[ThermoReading]-shaped shape .read() actually returns."""

    def __init__(self, readings=None, raises=None):
        self._readings = readings or {}
        self._raises = raises

    def read(self, channel=0xFF, timeout=None):
        if self._raises is not None:
            raise self._raises
        return [FakeThermoReading(ch, temp) for ch, temp in self._readings.items()]


class FakeControl:
    """Stand-in for kilnctrl.control.ControlClient -- just enough surface
    (.get_zones()) for cases_lcd._configured_zone_count()'s
    srv._control.get_zones() call. Real get_zones() returns
    (thermo_count, relay_count, zones: list[ZoneConfig]); only len(zones)
    is used by _configured_zone_count(), so `zone_count` here builds a
    same-length placeholder list. `raises`, if set, is raised instead --
    the "zone count unreadable" case, which must produce None (never a
    fabricated/guessed count), per the opus review of d66ba612."""

    def __init__(self, zone_count=3, raises=None):
        self._zone_count = zone_count
        self._raises = raises

    def get_zones(self):
        if self._raises is not None:
            raise self._raises
        return (self._zone_count, self._zone_count, [object()] * self._zone_count)


class FakeSrvFull(FakeSrv):
    """Extends FakeSrv with the extra client attributes LCD-02/03/04/09/14/16/19
    read (safety/profiles/thermo, and safety_clear_trip() at module scope)."""

    def __init__(self, ui_test, safety=None, profiles=None, thermo=None, clear_trip_raises=False,
                 control=None):
        super().__init__(ui_test)
        self._safety = safety or FakeSafety()
        self._profiles = profiles or FakeProfiles()
        self._thermo = thermo or FakeThermo()
        self._control = control or FakeControl()
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
        # _case_lcd02 calls _capture() unconditionally (before the page check
        # is judged), so this test must mock lcd_sampler.capture_full_frame --
        # otherwise it shells out to capture_lcd.ps1 and depends on the real
        # bench webcam. Page mismatch fails immediately regardless of what
        # the (mocked-away) camera returns (judge_lcd_home_firing's first
        # check), so the busy/unavailable side effect used elsewhere in this
        # class is sufficient here too.
        srv = FakeSrvFull(FakeUiTest(page="profiles", targets=_HOME_TARGETS))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
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

    def test_pause_sample_uses_label_avoid_offset_not_raw_centre(self):
        # Same class of regression as Lcd01Test's Start check: Pause is also
        # a ui_home_build_button() button with a centred caption, and now
        # goes through _sample_button_bool -> sample_widget_body.
        targets = [
            {"name": "start", "cx": 240, "cy": 280, "hidden": False, "label": "Stop"},
            {"name": "pause", "cx": 200, "cy": 280, "hidden": False},
            {"name": "profile_name", "cx": 240, "cy": 30, "hidden": False},
        ]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=targets))
        hp01 = {"progress_samples": [1.0, 90.0], "profile_name_greyed": True, "profile_name_tap_noop": True}
        fake_sample = lcd_sampler.RegionSample(region=(0xE8, 0x97, 0x4E), bezel=(26, 31, 43))
        seen_xy = []

        def fake_sample_widget(image_path, x, y, *args, **kwargs):
            seen_xy.append((x, y))
            return fake_sample

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget):
            C._case_lcd02({"srv": srv, "_hp01": hp01})
        self.assertIn((200, 280 + lcd_sampler.LABEL_AVOID_OFFSET_PX), seen_xy)
        self.assertNotIn((200, 280), seen_xy)


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
        # _case_lcd04 captures its "before" frame unconditionally once a trip
        # is latched, before it ever reaches the mask check below, so this
        # test must mock the camera too or it shells out for real.
        srv = FakeSrvFull(FakeUiTest(page="home"), safety=FakeSafety(FakeSafetyDiag(trip_reason=6, trip_mask=0x0040)))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
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

    def test_before_and_after_frames_get_distinct_paths(self):
        # Both frames land in evidence; a shared filename would make the
        # after-clear frame overwrite the before-clear one.
        strip_targets = [{"name": "trip_strip", "cx": 240, "cy": 10, "hidden": False}]
        srv = FakeSrvFull(FakeUiTest(page="home", targets=strip_targets), safety=FakeSafety(FakeSafetyDiag(trip_reason=6)))
        region_sample = lcd_sampler.RegionSample(region=(0xD6, 0x55, 0x5F), bezel=(26, 31, 43))
        off_sample = lcd_sampler.RegionSample(region=(26, 31, 43), bezel=(26, 31, 43))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value="fake.jpg"), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=[region_sample, off_sample]):
            result = C._case_lcd04({"srv": srv, "_lcd_capture_dir": "/cap"})
        self.assertEqual(len(result.evidence), 2)
        self.assertEqual(len(set(result.evidence)), 2)


_PROFILES_NAV = {"settings": "config", "Profiles": "profiles", "profile_row_0": "profile_detail"}
# Round 3 (item 4): "paging"/"new_profile" were fabricated names no
# firmware target has ever used -- ui_page_profile_picker.c's topbar
# icons are untagged glyphs, located only by position via
# _profiles_topbar_icons() (back/home anchors + a glyph in the New slot).
_PROFILES_GLYPH = "���"
_PROFILES_PAGE_TARGETS = {
    "home": [{"name": "settings", "cx": 10, "cy": 10, "hidden": False}],
    "config": [{"name": "Profiles", "cx": 100, "cy": 100, "hidden": False}],
    "profiles": [
        {"name": "profile_row_0", "cx": 50, "cy": 150, "hidden": False, "starred": False},
        {"name": "back", "cx": 140, "cy": 26, "hidden": False},
        {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        {"name": _PROFILES_GLYPH, "cx": 180 + 40, "cy": 26, "hidden": False},   # Prev
        {"name": _PROFILES_GLYPH, "cx": 180 + 80, "cy": 26, "hidden": False},   # Next
        {"name": _PROFILES_GLYPH, "cx": 180 + 120, "cy": 26, "hidden": False},  # New
    ],
    "profile_detail": [],
}


class ClickThenPageTest(unittest.TestCase):
    """Direct unit tests for _click_then_page() itself (2026-09-24 LCD-09/
    14/16 fix): a bare click_by_name() + _wait_for_page() with the wait's
    return value discarded let a case march on to click a target that could
    not exist on whatever page the board was really parked on. These tests
    exercise the helper directly, independent of any one case, including
    the timeout-not-checked regression the fix specifically closes."""

    def test_success_returns_none_fail_and_the_arrived_page(self):
        ui = PageNavUiTest(page="home", page_targets={"home": [], "config": []},
                            nav_map={"settings": "config"})
        fail, page, waited_s = C._click_then_page(ui, "settings", "config")
        self.assertIsNone(fail)
        self.assertEqual(page, "config")

    def test_click_itself_not_found_fails_without_polling(self):
        ui = PageNavUiTest(page="home", page_targets={"home": []}, nav_map={}, click_result="not_found")
        fail, page, waited_s = C._click_then_page(ui, "settings", "config")
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertIn("not_found", fail.reason)

    def test_click_ok_but_page_never_arrives_fails(self):
        # This is the exact regression the 2026-09-24 fix closes: without
        # checking _wait_for_page()'s return value, a click that replies
        # 'ok' but never actually lands on the destination page would be
        # treated as a successful hop -- and a caller would go on to click
        # a target that cannot exist on the page the board is really on.
        ui = PageNavUiTest(page="home", page_targets={"home": []}, nav_map={})  # "settings" click has no nav_map entry
        fail, page, waited_s = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertIn("config", fail.reason)
        self.assertEqual(page, "home")

    def test_retries_once_and_recovers_on_second_click(self):
        # A click that says 'ok' but the page swallowed the tap on the
        # first attempt must be retried once before failing outright -- the
        # retry lands on an already-awake panel (no wake edge involved), so
        # a second click that actually reaches the target must recover.
        class _RecoversOnSecondClick(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.calls = 0

            def click_by_name(self, name):
                self.calls += 1
                if self.calls == 1:
                    return {"result": "ok"}  # swallowed: no page change
                return super().click_by_name(name)

        ui = _RecoversOnSecondClick(page="home", page_targets={"home": [], "config": []},
                                     nav_map={"settings": "config"})
        fail, page, waited_s = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNone(fail)
        self.assertEqual(page, "config")
        self.assertEqual(ui.calls, 2)

    def test_retry_exhausted_fails_naming_the_retry(self):
        ui = _CountingNavUi(page="home", page_targets={"home": []}, nav_map={})
        fail, page, waited_s = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(fail.verdict, Verdict.FAIL)
        self.assertIn("retried once", fail.reason)
        self.assertEqual(fail.observed.get("attribution"), "swallowed_or_wrong_page")
        # Exactly one retry: two real taps, never more.
        self.assertEqual(ui.clicks, ["settings", "settings"])
        self.assertIn("retry_click", fail.observed)

    def test_not_found_attribution_never_retries(self):
        ui = _CountingNavUi(page="home", page_targets={"home": []}, nav_map={}, click_result="not_found")
        fail, page, waited_s = C._click_then_page(ui, "settings", "config")
        self.assertIsNotNone(fail)
        self.assertEqual(fail.observed.get("attribution"), "not_found")
        self.assertEqual(ui.clicks, ["settings"])

    def test_page_moved_elsewhere_is_never_retried(self):
        # The first tap DID move the board, just not to the expected page.
        # A second tap by the same name would land on a different page's
        # widget, so it must never be sent.
        ui = _CountingNavUi(page="home", page_targets={"home": [], "diagnostics": []},
                            nav_map={"settings": "diagnostics"})
        fail, page, waited_s = C._click_then_page(ui, "settings", "config", timeout_s=0.05)
        self.assertIsNotNone(fail)
        self.assertEqual(ui.clicks, ["settings"])
        self.assertEqual(fail.observed.get("attribution"), "wrong_page")
        self.assertEqual(page, "diagnostics")

    def test_every_call_site_is_a_pure_navigation_target(self):
        # A retried click is a second real tap. Pin EVERY call site of both
        # name-based retrying helpers (_click_then_page and
        # _click_then_targets_change -- the latter now unused by any case
        # since round 3's LCD-16 rewrite moved to a raw-touch retry instead,
        # but is checked here too in case a future case reintroduces it) so
        # a Start/Stop/Confirm/PIN/toggle target can't be added without
        # revisiting the retry. Every argument here must be a literal name
        # in the allowlist -- round 3 removed the one non-literal case
        # (`title` iterating J._DIAG_TITLES). LCD-09's fix (2026-09-24)
        # reintroduced one non-literal call site: `rows[0]["name"]`, the
        # profile-picker's row label read live off the board. It is
        # explicitly allowed here rather than added to `literals` (which
        # must stay a set of exact strings) because the row name is a
        # profile name chosen by whatever profiles exist on the bench, never
        # a fixed string -- but it is still reviewed and safe: it names a
        # row on the `profiles` list page, structurally never the same
        # widget as Start/Stop/Confirm/PIN/toggle, which all live on other
        # pages entirely (home / settings), so the `unsafe`-word check below
        # does not apply to it.
        import inspect
        import re
        src = inspect.getsource(C)
        calls = re.findall(r'(?<!def )\b(_click_then_page|_click_then_targets_change)\(\s*ui\s*,\s*([^,)]+)', src)
        self.assertTrue(calls, "no call sites found -- the regex no longer matches the source")
        allowed_nonliteral = {'rows[0]["name"]'}
        literals = set()
        for helper, arg in calls:
            arg = arg.strip()
            if arg in allowed_nonliteral:
                continue
            m = re.fullmatch(r'"([^"]+)"', arg)
            self.assertIsNotNone(m, f"non-literal retried click target {arg!r} in {helper}()")
            literals.add(m.group(1))
        self.assertEqual(literals, {"settings", "Profiles", "Temperature", "Diagnostics"})
        unsafe = ("start", "stop", "confirm", "pin", "toggle", "cancel", "ack", "reset", "clear", "ok")
        for title in literals:
            words = re.findall(r"[a-z]+", title.lower())
            for word in unsafe:
                self.assertNotIn(word, words, f"retried click target {title!r} contains {word!r}")

    def test_next_touch_retry_never_uses_click_by_name(self):
        # Round 3 (item 5): the diagnostics topbar's Next icon is paged via
        # a raw coordinate touch (_tap_next_then_targets_change()), never
        # click_by_name() -- Prev/Next share one undecodable glyph name
        # (_is_glyph_name()) and click_by_name() answers AMBIGUOUS for it on
        # a live board, so this call site must never be added to (or
        # confused with) the name-based retry allowlist above.
        import inspect
        import re
        src = inspect.getsource(C._tap_next_then_targets_change)
        # Match a call/attribute-access, not the docstring's prose mentions
        # of click_by_name() explaining why it is deliberately avoided.
        self.assertNotRegex(src, r'\.click_by_name\s*\(')
        self.assertIn("touch.inject", src)


class ThemeMirrorDriftTest(unittest.TestCase):
    """cases_lcd.py mirrors four ui_theme.h colors as reference values.
    Fails (never skips) if the header moves or a value drifts."""

    HEADER = os.path.join(os.path.dirname(__file__), "..", "..", "..", "firmware", "KilnFW",
                          "App", "drivers", "ui", "ui_theme.h")

    def _hex(self, text, macro):
        import re
        m = re.search(r"#define\s+" + macro + r"\s+0x([0-9a-fA-F]{6})\b", text)
        self.assertIsNotNone(m, f"{macro} not found in ui_theme.h")
        v = int(m.group(1), 16)
        return ((v >> 16) & 0xFF, (v >> 8) & 0xFF, v & 0xFF)

    def test_mirrors_match_ui_theme_h(self):
        self.assertTrue(os.path.isfile(self.HEADER), f"missing {os.path.abspath(self.HEADER)}")
        with open(self.HEADER, encoding="utf-8") as fh:
            text = fh.read()
        self.assertEqual(C._BG_RGB, self._hex(text, "UI_THEME_COLOR_BG_HEX"))
        self.assertEqual(C._ACCENT_1_RGB, self._hex(text, "UI_THEME_ACCENT_1_HEX"))
        self.assertEqual(C._ACCENT_4_RGB, self._hex(text, "UI_THEME_ACCENT_4_HEX"))
        self.assertEqual(C._ACCENT_5_RGB, self._hex(text, "UI_THEME_ACCENT_5_HEX"))
        self.assertEqual(C._CARD_RGB, self._hex(text, "UI_THEME_COLOR_CARD_HEX"))
        self.assertEqual(C._TEXT_PRIMARY_RGB, self._hex(text, "UI_THEME_COLOR_TEXT_PRIMARY_HEX"))


class _CountingNavUi(PageNavUiTest):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.clicks = []

    def click_by_name(self, name):
        self.clicks.append(name)
        return super().click_by_name(name)


class _RecordingNavUi(PageNavUiTest):
    def __init__(self, *a, **kw):
        super().__init__(*a, **kw)
        self.clicked = []

    def click_by_name(self, name):
        self.clicked.append(name)
        return super().click_by_name(name)


class FirstHopTimeoutCaseTest(unittest.TestCase):
    """Case-level negative test for the 2026-09-24 LCD-09/14/16 fix: the
    "settings" click replies 'ok' but the board never leaves home. Each
    case must FAIL naming that first hop and must never go on to click its
    second target -- even though (to make the regression observable) the
    second target is also offered on home and would navigate if clicked."""

    def _run(self, case_fn, second, dest):
        page_targets = {
            "home": [{"name": "settings", "hidden": False}, {"name": second, "hidden": False}],
            dest: [],
        }
        ui = _RecordingNavUi(page="home", page_targets=page_targets, nav_map={second: dest})
        srv = FakeSrvFull(ui)
        with mock.patch.object(C._click_then_page, "__defaults__", (0.05,)):
            result = case_fn({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("'settings'", result.reason)
        self.assertIn("'config'", result.reason)
        self.assertNotIn(second, ui.clicked)

    def test_lcd09(self):
        self._run(C._case_lcd09, "Profiles", "profiles")

    def test_lcd14(self):
        self._run(C._case_lcd14, "Temperature", "temperature")

    def test_lcd16(self):
        self._run(C._case_lcd16, "Diagnostics", "diagnostics")


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

    def test_row_tap_retries_once_on_swallowed_tap_then_passes(self):
        # 2026-09-24 bench root cause (LCD-09, three identical runs:
        # 20260924T191429Z/203338Z/221915Z): the row tap used to go through
        # a bare click_by_name()+_wait_for_page_change() with no retry,
        # unlike every other hop in this module -- the same
        # screen_idle/touch-poll swallow race _click_then_page() already
        # guards every other case against (see Lcd08Test's identical
        # scenario above). The first tap on the picker's row says "ok" but
        # the page never actually moves off "profiles"; only the retried
        # second tap lands on "profile_detail".
        class SwallowRowOnceUi(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.row_calls = 0

            def click_by_name(self, name):
                if name == "profile_row_0":
                    self.row_calls += 1
                    if self.row_calls == 1:
                        return {"result": "ok", "cx": 0, "cy": 0}  # swallowed
                    self._page = "profile_detail"
                    return {"result": "ok", "cx": 0, "cy": 0}
                return super().click_by_name(name)

        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "profile_row_0", "cx": 50, "cy": 150, "hidden": False},
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        nav = dict(_PROFILES_NAV)
        ui = SwallowRowOnceUi(page="home", page_targets=page, nav_map=nav)
        result = C._case_lcd09({"srv": FakeSrvFull(ui)})
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(ui.row_calls, 2)

    def test_armed_delete_confirm_button_is_never_a_row_or_tapped(self):
        # Review of ebec7d06: the row tap now goes through the retrying
        # _click_then_page(), so rows[0] must never be a destructive
        # target. ui_page_profile_picker.c relabels an armed Delete button
        # "Confirm?" (and never resets the label when the window lapses);
        # a stale "Confirm?" sorting above the first row's name button must
        # be excluded from rows, never clicked -- let alone twice.
        class RecordingUi(PageNavUiTest):
            def __init__(self, *a, **kw):
                super().__init__(*a, **kw)
                self.clicks = []

            def click_by_name(self, name):
                self.clicks.append(name)
                return super().click_by_name(name)

        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "Confirm?", "cx": 400, "cy": 148, "hidden": False},
            {"name": "Cone 6 Bisque", "cx": 50, "cy": 150, "hidden": False},
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        nav = dict(_PROFILES_NAV)
        nav["Cone 6 Bisque"] = "profile_detail"
        ui = RecordingUi(page="home", page_targets=page, nav_map=nav)
        result = C._case_lcd09({"srv": FakeSrvFull(ui)})
        self.assertNotIn("Confirm?", ui.clicks)
        self.assertIn("Cone 6 Bisque", ui.clicks)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_finally_restores_home_on_exception(self):
        ui = RaisingUiTest(page="home", page_targets=_PROFILES_PAGE_TARGETS, nav_map=_PROFILES_NAV, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd09({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)

    # Firmware-shaped profiles page: ui_topbar.c's untagged Prev/Next/New
    # icons are listed under their glyph label text, which the PC decodes
    # as U+FFFD bytes; only "back"/"home" carry tag names. Row 0 is kept so
    # the row-tap half of the judge is exercised.
    _GLYPH = "\ufffd\ufffd\ufffd"

    def _profiles_targets(self, slots):
        back = {"name": "back", "cx": 140, "cy": 26, "hidden": False}
        home = {"name": "home", "cx": 180, "cy": 26, "hidden": False}
        glyphs = [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in slots]
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [{"name": "profile_row_0", "cx": 50, "cy": 150, "hidden": False, "starred": False},
                            back, home] + glyphs
        return page

    def _run_firmware_shaped(self, slots):
        ui = PageNavUiTest(page="home", page_targets=self._profiles_targets(slots), nav_map=_PROFILES_NAV)
        return C._case_lcd09({"srv": FakeSrvFull(ui)})

    def test_glyph_icons_by_position_pass(self):
        self.assertEqual(self._run_firmware_shaped([1, 2, 3]).verdict, Verdict.PASS)

    def test_only_next_enabled_passes(self):
        self.assertEqual(self._run_firmware_shaped([2, 3]).verdict, Verdict.PASS)

    def test_single_page_both_paging_disabled_is_inconclusive(self):
        self.assertEqual(self._run_firmware_shaped([3]).verdict, Verdict.INCONCLUSIVE)

    def test_no_topbar_icons_fails(self):
        result = self._run_firmware_shaped([])
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("Prev/Next", result.reason)

    def test_new_icon_missing_fails(self):
        self.assertEqual(self._run_firmware_shaped([1, 2]).verdict, Verdict.FAIL)

    def test_row_located_by_real_firmware_name_not_fixed_prefix(self):
        # build_row() (ui_page_profile_picker.c) tags no fixed name on a
        # row's own button -- kiln_ui.c falls back to the button's label
        # text, i.e. the profile's real name. A row named "Cone 6 Bisque"
        # (never matching a "profile_row_" prefix) must still be found by
        # _profile_rows_by_position() and drive a PASS.
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "Cone 6 Bisque", "cx": 50, "cy": 150, "hidden": False},
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        nav = dict(_PROFILES_NAV)
        nav["Cone 6 Bisque"] = "profile_detail"
        ui = PageNavUiTest(page="home", page_targets=page, nav_map=nav)
        result = C._case_lcd09({"srv": FakeSrvFull(ui)})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_full_page_with_unnamed_rows_col_container_passes(self):
        # Review 2026-09-24: the list's own `rows_col` container
        # (ui_page_profile_picker.c, plain lv_obj_create -> CLICKABLE by
        # default, walked with name "") sits at cy ~= 178, inside the list
        # band. A full 4-row page plus that container must read as 4 rows,
        # not 5 (a false max_rows FAIL), and never tap the container.
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "", "cx": 240, "cy": 178, "hidden": False},
        ] + [
            {"name": "Profile %d" % k, "cx": 50, "cy": 74 + 32 * k, "hidden": False}
            for k in range(4)
        ] + [
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        nav = dict(_PROFILES_NAV)
        nav["Profile 0"] = "profile_detail"
        ui = PageNavUiTest(page="home", page_targets=page, nav_map=nav)
        result = C._case_lcd09({"srv": FakeSrvFull(ui)})
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_no_rows_with_known_positive_profiles_count_fails_not_inconclusive(self):
        # 2026-09-24 coordinator follow-up: an empty list area used to read
        # as merely undecidable even when the board itself reports profiles
        # exist -- that combination is a real stuck/empty-list defect.
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        ui = PageNavUiTest(page="home", page_targets=page, nav_map=_PROFILES_NAV)
        srv = FakeSrvFull(ui, profiles=FakeProfiles(summaries=["cone6", "cone10"]))
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("profiles_count", result.reason)

    def test_no_rows_with_unknown_profiles_count_stays_inconclusive(self):
        # list_all() raising (e.g. link busy) must never fabricate a FAIL --
        # profiles_count stays None and the original undecidable verdict
        # is preserved.
        page = dict(_PROFILES_PAGE_TARGETS)
        page["profiles"] = [
            {"name": "back", "cx": 140, "cy": 26, "hidden": False},
            {"name": "home", "cx": 180, "cy": 26, "hidden": False},
        ] + [{"name": self._GLYPH, "cx": 180 + 40 * k, "cy": 26, "hidden": False} for k in (1, 2, 3)]
        ui = PageNavUiTest(page="home", page_targets=page, nav_map=_PROFILES_NAV)
        srv = FakeSrvFull(ui, profiles=FakeProfiles(list_all_raises=RuntimeError("busy")))
        result = C._case_lcd09({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


_TEMP_NAV = {"settings": "config", "Temperature": "temperature"}
_TEMP_PAGE_TARGETS = {
    "home": [{"name": "settings", "hidden": False}],
    "config": [{"name": "Temperature", "hidden": False}],
    "temperature": [],
}


class Lcd14Test(unittest.TestCase):
    # Round 3 rewrite (item 3): zone_temp_/safety_line are not real tap
    # targets -- ui_page_temperature.c's zone rows and the Safety (K4) line
    # are plain, non-clickable labels, so LIST_TAP_TARGETS never reports
    # them. The judge is now driven by capture-based region sampling
    # (_sample_widget_off) against the fixed zone-row geometry derived from
    # ui_page_temperature.c's layout constants, not by named-target lookup.
    def _run_with_rows(self, rendered_zones, thermo=None, configured_zones=3,
                        header_rendered=True, control=None):
        ui = PageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=thermo or FakeThermo({0: 100.0, 1: 100.0, 2: 100.0}),
                           profiles=FakeProfiles(FakeExecStatus("idle")),
                           control=control or FakeControl(zone_count=configured_zones))
        # The Relays card header samples at the SAME formula as a
        # (configured_zones)'th zone row (see cases_lcd.py's header_y
        # comment) -- model it as one more "row" slot in the same dict so
        # this fake doesn't need a second code path.
        rendered = dict(rendered_zones)
        rendered[configured_zones] = header_rendered

        def fake_sample_widget(image_path, cx, cy, repo_root=None):
            # Realistic model (2026-09-24 contrast-judge follow-up): a
            # rendered row's TEXT point (near _LCD14_ZONE_TEXT_X) reads
            # UI_THEME_COLOR_TEXT_PRIMARY against its own CARD background at
            # the BG-reference point (_LCD14_ROW_X); a MISSING row makes
            # BOTH points read the page's own BG -- never the bezel, which
            # neither point is anywhere near in real geometry.
            zone = round((cy - C._LCD14_ZONE_ROW_Y0) / C._LCD14_ZONE_ROW_PITCH)
            on = rendered.get(zone, False)
            is_text_point = abs(cx - C._LCD14_ZONE_TEXT_X) < abs(cx - C._LCD14_ROW_X)
            if zone == configured_zones and on:
                # The Relays header is ONE left-aligned label spanning
                # x ~14..256 (not a name/value pair with a gap at x=240 like
                # a zone row) -- any point in that span reads text.
                is_text_point = 14.0 <= cx <= 256.0
            if not on:
                region = C._BG_RGB
            else:
                region = C._TEXT_PRIMARY_RGB if is_text_point else C._CARD_RGB
            return lcd_sampler.RegionSample(region=region, bezel=(6, 13, 22))

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value=None), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget), \
             mock.patch.object(lcd_sampler, "frame_corners_look_stale", return_value=False):
            return C._case_lcd14({"srv": srv})

    def test_all_rows_rendered_pass(self):
        result = self._run_with_rows({0: True, 1: True, 2: True})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_missing_row_fails(self):
        result = self._run_with_rows({0: True, 1: False, 2: True})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("1", result.reason)

    # -- Round 4: configured-zone-count cross-check (opus review of d66ba612) --

    def test_header_shifted_up_when_row_missing_fails(self):
        # The exact bug shape the review found: the board actually rendered
        # only 2 zone rows (0, 1) while 3 are configured -- rows 0/1 read as
        # rendered, but the Relays header is one row pitch higher than
        # header_y (58 + 3*32 = 154), so sampling AT header_y reads
        # background, not card content. A per-row-only check (the old code)
        # could not tell this apart from a genuine 3rd row landing there.
        result = self._run_with_rows({0: True, 1: True, 2: True}, configured_zones=3,
                                      header_rendered=False)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_zero_zones_configured_label_rendered_passes(self):
        ui = PageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({}), profiles=FakeProfiles(FakeExecStatus("idle")),
                           control=FakeControl(zone_count=0))

        def fake_sample_widget(image_path, cx, cy, repo_root=None):
            is_text_point = abs(cx - C._LCD14_ZONE_TEXT_X) < abs(cx - C._LCD14_ROW_X)
            region = C._TEXT_PRIMARY_RGB if is_text_point else C._BG_RGB
            return lcd_sampler.RegionSample(region=region, bezel=(6, 13, 22))

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value=None), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget), \
             mock.patch.object(lcd_sampler, "frame_corners_look_stale", return_value=False):
            result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_zone_count_unreadable_is_inconclusive(self):
        ui = PageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 100.0}), profiles=FakeProfiles(FakeExecStatus("idle")),
                           control=FakeControl(raises=RuntimeError("no reply")))
        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value=None), \
             mock.patch.object(lcd_sampler, "sample_widget", return_value=lcd_sampler.RegionSample(
                 region=C._TEXT_PRIMARY_RGB, bezel=(6, 13, 22))), \
             mock.patch.object(lcd_sampler, "frame_corners_look_stale", return_value=False):
            result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertIn("zones_count_error", result.observed)

    def test_no_capture_is_inconclusive(self):
        ui = PageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 100.0}), profiles=FakeProfiles(FakeExecStatus("idle")))
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
            result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

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


_DIAG_GLYPH = "���"
_DIAG_BACK = {"name": "back", "cx": 140, "cy": 26, "hidden": False}
_DIAG_HOME = {"name": "home", "cx": 180, "cy": 26, "hidden": False}
_DIAG_PITCH = 40.0
_DIAG_NEXT = {"name": _DIAG_GLYPH, "cx": 180 + 2 * _DIAG_PITCH, "cy": 26, "hidden": False}


class DowngradeIfCornersStaleTest(unittest.TestCase):
    """_downgrade_if_corners_stale() may only ever lower a FAIL to
    INCONCLUSIVE, and only on an explicit True from the self-check."""

    def _fail(self):
        return C.CaseResult(Verdict.FAIL, reason="wrong color", observed={"x": 1}, evidence=["e.jpg"])

    def _run(self, result, stale=None, raises=False):
        def fake(*a, **k):
            if raises:
                raise RuntimeError("boom")
            return stale
        with mock.patch.object(lcd_sampler, "frame_corners_look_stale", fake):
            return C._downgrade_if_corners_stale({}, result, "img.jpg")

    def test_pass_is_never_touched(self):
        r = C.CaseResult(Verdict.PASS)
        self.assertIs(self._run(r, stale=True), r)

    def test_fail_with_stale_corners_becomes_inconclusive_keeping_reason(self):
        out = self._run(self._fail(), stale=True)
        self.assertEqual(out.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(out.observed["original_fail_reason"], "wrong color")
        self.assertTrue(out.observed["frame_corners_stale"])
        self.assertEqual(out.evidence, ["e.jpg"])

    def test_fail_with_sound_corners_stays_fail(self):
        self.assertEqual(self._run(self._fail(), stale=False).verdict, Verdict.FAIL)

    def test_fail_with_undeterminable_corners_stays_fail(self):
        self.assertEqual(self._run(self._fail(), stale=None).verdict, Verdict.FAIL)

    def test_self_check_exception_stays_fail(self):
        self.assertEqual(self._run(self._fail(), raises=True).verdict, Verdict.FAIL)

    def test_no_image_stays_fail(self):
        with mock.patch.object(lcd_sampler, "frame_corners_look_stale", lambda *a, **k: True):
            self.assertEqual(C._downgrade_if_corners_stale({}, self._fail(), None).verdict, Verdict.FAIL)


class DiagPagingUi(PageNavUiTest):
    """Models ui_page_diagnostics.c's 8 sub-pages, reached only by the
    topbar's untagged Next icon (position-located, never click_by_name --
    see _diagnostics_next_target()'s docstring). ``step_content`` maps a
    sub-page index to the extra targets that page shows (e.g. an
    Acknowledge button on Crash Report, a still-present Reset on Relay
    Life for the regression check)."""

    def __init__(self, *a, step_content=None, page_count=8, swallow_step=None, **kw):
        super().__init__(*a, **kw)
        self.step = 0
        self._step_content = step_content or {}
        self._page_count = page_count
        self._swallow_step = swallow_step
        self._swallowed_once = set()

    def list_tap_targets(self):
        if self._page != "diagnostics":
            return super().list_tap_targets()
        targets = [_DIAG_BACK, _DIAG_HOME]
        if self.step > 0:
            targets.append({"name": _DIAG_GLYPH, "cx": 180 + 1 * _DIAG_PITCH, "cy": 26, "hidden": False})
        if self.step + 1 < self._page_count:
            targets.append(_DIAG_NEXT)
        targets.extend(self._step_content.get(self.step, []))
        return {"targets": targets, "truncated": False}


class FakeTouchAdvancesDiag:
    """A TouchClient double: inject()ing a press+release at Next's own
    cx/cy advances DiagPagingUi.step, exactly the raw-coordinate path
    _tap_next_then_targets_change() uses (click_by_name() cannot target
    Next -- Prev/Next share one undecodable glyph name)."""

    def __init__(self, ui):
        self._ui = ui
        self.injected = []

    def inject(self, x, y, pressed):
        self.injected.append((x, y, pressed))
        if not pressed and abs(x - _DIAG_NEXT["cx"]) < 1 and abs(y - _DIAG_NEXT["cy"]) < 1:
            if self._ui._swallow_step == self._ui.step and self._ui.step not in self._ui._swallowed_once:
                self._ui._swallowed_once.add(self._ui.step)
                return  # swallowed once: step does not advance
            if self._ui.step + 1 < self._ui._page_count:
                self._ui.step += 1


def _diag_srv(ui):
    srv = FakeSrvFull(ui)
    srv._touch = FakeTouchAdvancesDiag(ui)
    return srv


_DIAG_NAV = {"settings": "config", "Diagnostics": "diagnostics"}
_DIAG_PAGE_TARGETS = {
    "home": [{"name": "settings", "hidden": False}],
    "config": [{"name": "Diagnostics", "hidden": False}],
}


class DiagPagingUiTransientRead(DiagPagingUi):
    """Models the 2026-09-24 race: a hop's own ENTRY read -- used to locate
    Next before tapping it -- lands right after the PREVIOUS hop's tap and
    can come back with the topbar's own back/home anchors transiently
    missing (board mid-redraw). The previous hop's own post-tap read (its
    boundary wait-for-change poll, or an interior hop's single trust read)
    is always the FIRST list_tap_targets() call observed at a given
    ``self.step`` value; the following hop's entry read is the SECOND. This
    blanks exactly that second read, once, for a chosen step."""

    def __init__(self, *a, blank_once_at_step=None, **kw):
        super().__init__(*a, **kw)
        self._blank_once_at_step = blank_once_at_step
        self._blanked = set()
        self._step_call_counts: dict = {}

    def list_tap_targets(self):
        if self._page == "diagnostics":
            count = self._step_call_counts.get(self.step, 0) + 1
            self._step_call_counts[self.step] = count
            if (self.step == self._blank_once_at_step and count == 2
                    and self.step not in self._blanked):
                self._blanked.add(self.step)
                return {"targets": [], "truncated": False}
        return super().list_tap_targets()


class Lcd16Test(unittest.TestCase):
    def test_finally_restores_home_on_exception(self):
        nav_map = {"settings": "config", "Diagnostics": "diag_hub"}
        ui = RaisingUiTest(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=nav_map, raise_on_call=2)
        srv = FakeSrvFull(ui)
        with self.assertRaises(RuntimeError):
            C._case_lcd16({"srv": srv})
        self.assertGreaterEqual(ui.home_calls, 1)

    def test_menu_tap_failure_fails(self):
        ui = PageNavUiTest(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV, click_result="not_found")
        srv = FakeSrvFull(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_full_paging_with_ack_at_last_step_passes(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV,
                           step_content={7: [{"name": "Acknowledge", "cx": 240, "cy": 280, "hidden": False}]})
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("pages_paged"), 7)
        self.assertEqual(result.observed.get("crash_report_step"), 7)

    def test_relay_life_reset_still_present_fails(self):
        # Relay Life's own Reset control was fully removed
        # (UI_PLAN.md section 6.4) -- a reappearing Reset target is a
        # regression, not a pass.
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV,
                           step_content={3: [{"name": "Reset", "cx": 240, "cy": 280, "hidden": False}]})
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("Reset", result.reason)

    def test_swallowed_next_tap_retries_once_then_passes(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV, swallow_step=0)
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("pages_paged"), 7)

    def test_no_retry_recorded_on_a_clean_run(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("retried_hops"), [])

    def test_swallowed_interior_tap_is_recorded_not_silent(self):
        # Interior hops are single, unconfirmed taps, so a tap dropped at
        # step 3 is only rescued by the LAST boundary hop's retry. The
        # final state is still correct (verdict unchanged), but the retry
        # must be visible in observed rather than silently absorbed.
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV, swallow_step=3)
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertEqual(ui.step, 7)
        self.assertEqual(result.observed.get("retried_hops"), [6])

    def test_next_stuck_after_retry_is_inconclusive(self):
        class StuckDiagUi(DiagPagingUi):
            pass

        ui = StuckDiagUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        srv = FakeSrvFull(ui)

        class StuckTouch:
            def __init__(self):
                self.injected = []

            def inject(self, x, y, pressed):
                self.injected.append((x, y, pressed))  # never advances ui.step

        srv._touch = StuckTouch()
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(result.observed.get("pages_paged"), 0)

    def test_truncated_tap_list_is_named_in_the_reason(self):
        # Review of ebec7d06: the LIST_TAP_TARGETS reply's 253 B wire cap
        # can cut the topbar icons from a sub-page with long row names
        # (Internal RAM); that read carries truncated=True. LCD-16 must
        # say so instead of only "Next may be broken".
        class TruncatingDiagUi(DiagPagingUi):
            def list_tap_targets(self):
                tap = super().list_tap_targets()
                if self._page == "diagnostics" and self.step >= 1:
                    return {"targets": [{"name": "Internal RAM free", "cx": 240, "cy": 120, "hidden": False}],
                            "truncated": True}
                return tap

        ui = TruncatingDiagUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertNotEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("tap_list_truncated_steps"), [1])
        self.assertIn("truncated", result.reason)

    def test_untruncated_run_records_no_truncation(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        result = C._case_lcd16({"srv": _diag_srv(ui)})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("tap_list_truncated_steps"), [])
        self.assertNotIn("truncated", result.reason or "")

    def test_interior_hop_transient_blank_read_recovers_without_extra_tap(self):
        # 2026-09-24 coordinator follow-up: an interior hop's own ENTRY read
        # (locating Next before tapping it) lands right after the PREVIOUS
        # hop's tap and raced the board's redraw, coming back with no
        # anchors at all -- previously misread as "Next is gone" and
        # stopped paging after the first hop. The stabilization poll must
        # recover WITHOUT sending any touch.
        #
        # blank_once_at_step=1 blanks step-1 hop's own entry read (step 1 is
        # interior: not step 0 or step expected_hops-1=6, so it uses
        # confirm=False and this fix's poll) -- the SECOND
        # list_tap_targets() call observed at step==1 (the first is step-0
        # hop's own post-tap wait-for-change read, which already tolerates
        # a blank reading on its own and must not be the one under test).
        ui = DiagPagingUiTransientRead(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV,
                                        blank_once_at_step=1)
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed.get("pages_paged"), 7)
        # Exactly one press+release pair per real hop, plus the one
        # _wake_and_home() wake tap this fake touch double has no
        # get_state() to satisfy (falls to need_wake=True) -- never a
        # doubled tap for the interior hop whose read was transiently blank.
        presses = [t for t in srv._touch.injected if t[2] is True]
        self.assertEqual(len(presses), 8)

    def test_interior_hop_still_blank_at_timeout_reports_next_missing(self):
        # Negative-test companion: if the anchors never come back at all
        # (a genuinely broken page, not merely a transient race), the poll
        # must give up at timeout_s and report found_next=False (paging
        # stops) rather than hang or fabricate a recovery.
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)

        class _AlwaysBlankAfterFirstTap(FakeTouchAdvancesDiag):
            def inject(self, x, y, pressed):
                super().inject(x, y, pressed)
                if not pressed:
                    self._ui._force_blank = True

        srv = FakeSrvFull(ui)
        srv._touch = _AlwaysBlankAfterFirstTap(ui)
        ui._force_blank = False
        orig_list = ui.list_tap_targets

        def list_tap_targets():
            if ui._page == "diagnostics" and getattr(ui, "_force_blank", False) and ui.step >= 1:
                return {"targets": [], "truncated": False}
            return orig_list()

        ui.list_tap_targets = list_tap_targets
        with mock.patch.object(C._tap_next_then_targets_change, "__defaults__", (0.05, True)):
            result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.observed.get("pages_paged"), 1)
        presses = [t for t in srv._touch.injected if t[2] is True]
        self.assertEqual(len(presses), 2)

    def test_no_touch_transport_is_inconclusive(self):
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV)
        srv = FakeSrvFull(ui)  # no _touch attribute at all
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


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

    def __init__(self, right_pin: str, wrong_pin: str, policy_fn=None, cancel_raises=False):
        super().__init__(page="home", targets=[])
        self._right_pin = right_pin
        self._wrong_pin = wrong_pin
        # policy_fn() -> lcd_enabled: when given, Start with the policy off
        # goes straight to Confirm Start with no keypad, as
        # ui_lcd_lock_has_role() does on the real board.
        self._policy_fn = policy_fn
        self._cancel_raises = cancel_raises
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
        if name == "Cancel" and self._cancel_raises:
            raise RuntimeError("simulated UI_TEST reply lost")
        if self._state == "idle":
            if name == "Start":
                if self._policy_fn is not None and not self._policy_fn():
                    self._state = "confirm"
                    return {"result": "ok"}
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


class DelayedOverlayUiTest(FakeUiTest):
    """``list_tap_targets`` returns ``pre`` for the first ``stale_reads``
    reads, then ``post`` -- models the real LCD-19 click-then-read race:
    home's own Start/nav buttons are non-empty (``pre``) even before LVGL
    has processed the click, and the popup's own buttons only replace them
    a couple of polls later (``post``)."""

    def __init__(self, pre, post, stale_reads=1):
        super().__init__(page="home", targets=[])
        self._pre = pre
        self._post = post
        self._stale_reads = stale_reads
        self._reads = 0

    def list_tap_targets(self):
        self._reads += 1
        names = self._pre if self._reads <= self._stale_reads else self._post
        return {"targets": [{"name": n, "hidden": False} for n in names], "truncated": False}


class WaitForOverlayNamesBaselineTest(unittest.TestCase):
    """Unit tests for the LCD-19 baseline-wait fix: _wait_for_overlay_names
    used to treat `present=True` as "wait for a non-empty set", which the
    home page's own Start/nav buttons already satisfy before the popup
    ever appears -- a real bug found from a suspiciously fast (0.92s vs a
    2.0s timeout) LCD-19 FAIL. It must instead wait for the set to differ
    from a caller-supplied pre-click baseline."""

    def test_present_true_waits_past_stale_nonempty_baseline(self):
        ui = DelayedOverlayUiTest(pre={"Start", "settings"},
                                   post={"Start", "settings", "OK", "Cancel"},
                                   stale_reads=2)
        baseline = C._lcd19_overlay_names(ui)  # consumes one read, itself "pre"
        names, _waited = C._wait_for_overlay_names(ui, present=True, baseline=baseline,
                                                     timeout_s=1.0, interval_s=0.01)
        self.assertIsNotNone(names)
        self.assertIn("OK", names)
        self.assertIn("Cancel", names)

    def test_present_true_never_diverging_from_baseline_times_out_honestly(self):
        # A popup that never actually appears (baseline == post) must not
        # be reported as "present" just because the timeout elapsed --
        # the function returns the last (unsatisfying) read, not a lie.
        same = {"Start", "settings"}
        ui = DelayedOverlayUiTest(pre=same, post=same, stale_reads=0)
        baseline = C._lcd19_overlay_names(ui)
        names, waited = C._wait_for_overlay_names(ui, present=True, baseline=baseline,
                                                    timeout_s=0.05, interval_s=0.01)
        self.assertEqual(names, same)
        self.assertGreaterEqual(waited, 0.05)

    def test_present_true_without_baseline_falls_back_to_bare_nonempty(self):
        # Documents the fallback for a caller that omits `baseline`: the
        # OLD, buggy-for-LCD-19 bare-non-empty behavior. _case_lcd19 itself
        # always passes a baseline now; this is not an endorsement.
        ui = DelayedOverlayUiTest(pre={"Start", "settings"},
                                   post={"Start", "settings", "OK", "Cancel"})
        names, _waited = C._wait_for_overlay_names(ui, present=True, timeout_s=1.0, interval_s=0.01)
        self.assertEqual(names, {"Start", "settings"})  # returns immediately, stale


class Lcd19Test(unittest.TestCase):
    def test_not_run_when_no_pin_configured(self):
        srv = FakeSrvFull(FakeUiTest(page="home"))
        result = C._case_lcd19({"srv": srv})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_wakes_and_homes_before_driving_the_keypad(self):
        # 2026-09-24 bench root cause (LCD-19, three identical runs:
        # 20260924T191429Z/203338Z/221915Z): this case went straight to
        # click_by_name("Start")/("Stop") with no _wake_and_home() call --
        # the one exception among every click-driven LCD case. On a bench
        # run where the panel had blanked or drifted off `home` since the
        # previous case, that first click legitimately failed (a blanked
        # panel swallows the tap; a stale page has no "Start"/"Stop"
        # widget), leaving all four PIN-lock booleans at None -- the
        # observed INCONCLUSIVE. Pin that _wake_and_home() runs before the
        # click.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C, "_wake_and_home") as wake:
            C._case_lcd19(ctx)
        wake.assert_called_once_with(ctx)

    def test_wake_and_home_precedes_every_click(self):
        # Review of ebec7d06: the test above only pins that the wake ran,
        # not that it ran BEFORE the first click -- which is the claimed fix.
        order = []
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        real_click = ui.click_by_name

        def recording_click(name):
            order.append(("click", name))
            return real_click(name)

        ui.click_by_name = recording_click
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C, "_wake_and_home", side_effect=lambda c: order.append(("wake",))):
            C._case_lcd19(ctx)
        self.assertTrue(any(o[0] == "click" for o in order), order)
        self.assertEqual(order[0], ("wake",), order)

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

    def test_start_click_that_changes_nothing_fails_with_names_recorded(self):
        # The Start click is acknowledged but no popup appears: the
        # post-click set never diverges from the pre-click baseline, so the
        # keypad is not raised, the case FAILs, and the raw names it saw
        # are still recorded in observed for the operator.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000")
        real_click = ui.click_by_name
        ui.click_by_name = lambda name: {"result": "ok"} if name == "Start" else real_click(name)
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(), "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        with mock.patch.object(C._wait_for_overlay_names, "__defaults__",
                               tuple(0.05 if d == C._PAGE_POLL_TIMEOUT_S else d
                                     for d in C._wait_for_overlay_names.__defaults__)):
            result = C._case_lcd19(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIs(result.observed.get("keypad_raised"), False)
        self.assertIn("after_start_click_names", result.observed)
        self.assertEqual(result.observed["after_start_click_names"], [])

    def test_restore_runs_even_if_overlay_dismiss_raises(self):
        # The dismiss is a UART round trip that can raise; the HTTP policy
        # restore must still run so lcd_enabled is never left on.
        ui = PinKeypadUiTest(right_pin="1234", wrong_pin="0000", cancel_raises=True)
        srv = FakeSrvFull(ui)
        sec = FakeLcd19SecClient(lcd_enabled=False)
        ctx = {"srv": srv, "sec_client": sec, "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(sec._cfg["lcd_enabled"], False)
        self.assertTrue(result.observed["restore"]["readback_matches"])
        self.assertEqual(result.observed["overlay_dismiss"].get("checked"), False)

    def test_pins_never_in_result(self):
        import json
        right, wrong = "8642097", "8642098"
        for ui in (PinKeypadUiTest(right_pin=right, wrong_pin=wrong),
                   PinKeypadUiTest(right_pin=wrong, wrong_pin=wrong)):
            srv = FakeSrvFull(ui)
            ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
                   "_lcd_pin": {"right_pin": right, "wrong_pin": wrong}}
            result = C._case_lcd19(ctx)
            blob = (result.reason or "") + json.dumps(result.observed or {}, default=str)
            self.assertNotIn(right, blob)
            self.assertNotIn(wrong, blob)

    def test_keypad_that_ignores_ok_never_passes_right_pin(self):
        # Vacuity guard: a keypad that never reacts to OK leaves the button
        # set unchanged for BOTH PINs. The wrong-PIN read alone can't tell
        # that apart from a refusal, so right_pin_started must be False
        # (keypad still open), never True -- the right-PIN close is the
        # signal that distinguishes the two verdicts.
        ui = PinKeypadUiTest(right_pin="never-matches", wrong_pin="0000")
        srv = FakeSrvFull(ui)
        ctx = {"srv": srv, "sec_client": FakeLcd19SecClient(),
               "_lcd_pin": {"right_pin": "1234", "wrong_pin": "0000"}}
        result = C._case_lcd19(ctx)
        self.assertEqual(result.observed.get("wrong_pin_refused"), True)
        self.assertEqual(result.observed.get("right_pin_started"), False)
        self.assertEqual(result.verdict, Verdict.FAIL)

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

            # The fake only raises the keypad while the SHARED board state has
            # lcd_enabled on, so this passes only if LCD-19 enables it itself.
            ui = PinKeypadUiTest(right_pin="1234", wrong_pin=ctx["_lcd_pin"]["wrong_pin"],
                                 policy_fn=lambda: sec._cfg["lcd_enabled"])
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
        # page flip succeeds so _case_lcd08 falls through to its unconditional
        # _capture() call; mock the camera so this test never shells out.
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
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
        ui = DelayedPageNavUiTest(page="home", page_targets=_TEMP_PAGE_TARGETS, nav_map=_TEMP_NAV, flips_after=1)
        srv = FakeSrvFull(ui, thermo=FakeThermo({0: 100.0, 1: 100.0, 2: 100.0}), profiles=FakeProfiles(FakeExecStatus("idle")))

        def fake_sample_widget(image_path, cx, cy, repo_root=None):
            is_text_point = abs(cx - C._LCD14_ZONE_TEXT_X) < abs(cx - C._LCD14_ROW_X)
            region = C._TEXT_PRIMARY_RGB if is_text_point else C._CARD_RGB
            return lcd_sampler.RegionSample(region=region, bezel=(6, 13, 22))

        with mock.patch.object(lcd_sampler, "capture_full_frame", return_value=None), \
             mock.patch.object(lcd_sampler, "sample_widget", side_effect=fake_sample_widget), \
             mock.patch.object(lcd_sampler, "frame_corners_look_stale", return_value=False):
            result = C._case_lcd14({"srv": srv})
        self.assertEqual(result.verdict, Verdict.PASS)


class Lcd16PollTest(unittest.TestCase):
    def test_relay_life_reset_seen_after_a_swallowed_next_tap_still_fails(self):
        # Same "poll one step late" race as Lcd14PollTest, expressed through
        # DiagPagingUi's swallow_step: a Next tap on step 3 (Relay Life) is
        # swallowed once, so the retry path must still be the one that
        # observes the reappeared Reset control rather than silently
        # skipping past it.
        ui = DiagPagingUi(page="home", page_targets=_DIAG_PAGE_TARGETS, nav_map=_DIAG_NAV,
                           step_content={3: [{"name": "Reset", "cx": 240, "cy": 280, "hidden": False}]},
                           swallow_step=2)
        srv = _diag_srv(ui)
        result = C._case_lcd16({"srv": srv})
        self.assertEqual(result.observed.get("relay_life_has_reset"), True)
        self.assertEqual(result.verdict, Verdict.FAIL)


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
        # _case_lcd08 captures unconditionally on success (NEW path reaches
        # config, page-flip succeeded), so mock the camera for both calls.
        with mock.patch.object(lcd_sampler, "capture_full_frame", side_effect=lcd_sampler.LcdCaptureError("busy")):
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
