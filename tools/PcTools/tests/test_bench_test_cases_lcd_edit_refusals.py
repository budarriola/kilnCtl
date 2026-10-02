#!/usr/bin/env python3
"""Unit tests for LCD-23 (Edit firing steppers + refusal paths) and LCD-24
(end-of-firing), cases_lcd._case_lcd23/_case_lcd24 and the three judgments
judge_lcd_edit_ramp_steppers / judge_lcd_edit_refusal / judge_lcd_edit_firing_end.
One fake board models the executor (driven by a fake clock), the live working
copy, the Edit page (positions of steppers/topbar icons, local working copy,
window check) and the relays -- never a real board.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_lcd_edit_refusals.py -q
"""
from __future__ import annotations

import copy
import math
import os
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_heat as CH  # noqa: E402,F401
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


ZONE_TEMP = 25.4
BASE = float(math.floor(ZONE_TEMP))
ROW_Y = {"target": 86, "ramp": 136, "dwell": 186}
MINUS_X, PLUS_X = 100, 443  # real layout: minus right after the caption
GLYPH = "�" * 3     # ui_test_client decodes LVGL symbol names as U+FFFD
NEXT_X, PREV_X = 453, 413
ADVANCE_AT = 30.0   # seconds after start: segment 0 -> 1
END_AT = 60.0       # seconds after start: firing done (LCD-24 profile)


class FakeLiveError(Exception):
    def __init__(self, status, detail=""):
        super().__init__(f"HTTP {status}")
        self.status = status
        self.detail = detail


class _Clock:
    def __init__(self):
        self.t = 0.0

    def now(self):
        return self.t

    def sleep(self, s):
        self.t += max(s, 0.01)


class FakeBoard:
    def __init__(self, clock, kind="lcd23", **kw):
        self.clock = clock
        self.kind = kind
        self.exec_forced = None
        self.started_at = None
        self.stopped = False
        self.page = "home"
        self.keypad = False
        self.calls = []
        self.presses = []
        self.origin = None          # saved profile segments (list of dicts)
        self.working = None         # adopted working copy (list of dicts) or None
        self.working_id = 9
        self.last_refusal = None
        self.page_segs = None       # the Edit page's local working copy
        self.cur_seg = 0
        self.page_released = False
        # behaviour switches
        self.locked = kw.get("locked", False)
        self.keypad_on_nav = kw.get("keypad_on_nav", False)
        self.apply_works = kw.get("apply_works", True)
        self.stale_apply_adopts = kw.get("stale_apply_adopts", False)
        self.steppers_stay_when_locked = kw.get("steppers_stay_when_locked", False)
        self.http_window_accepts = kw.get("http_window_accepts", False)
        self.http_bound_status = kw.get("http_bound_status", 400)
        self.never_advances = kw.get("never_advances", False)
        self.page_ignores_end = kw.get("page_ignores_end", False)
        self.no_pending_decision = kw.get("no_pending_decision", False)
        self.discard_works = kw.get("discard_works", True)
        self.foreign_after_stop = kw.get("foreign_after_stop", False)
        self.relays_energized = kw.get("relays_energized", False)
        self.preexisting_working = kw.get("preexisting_working", False)
        self.ends_as = kw.get("ends_as", "done")
        self.max_temp = kw.get("max_temp", 1300.0)
        self.max_ramp = kw.get("max_ramp", 1000.0)
        self.ceiling_accepts = kw.get("ceiling_accepts", False)
        self.foreign_wid_at_end = kw.get("foreign_wid_at_end", False)
        if self.preexisting_working:
            self.working = [{"target_c": 99.0, "ramp_c_per_hr": 1.0, "dwell_min": 1.0}]
            self.working_id = 5

    # -- executor, a function of the fake clock
    def exec_state(self):
        if self.exec_forced is not None:
            return self.exec_forced, 0
        if self.started_at is None:
            return "idle", 0
        el = self.clock.now() - self.started_at
        if self.kind == "lcd24" and not self.never_advances and el >= END_AT:
            return self.ends_as, 1
        if self.kind == "lcd23" and not self.never_advances and el >= ADVANCE_AT:
            return "running", 1
        return "running", 0

    def seg_index(self):
        return self.exec_state()[1]

    def running(self):
        return self.exec_state()[0] == "running"

    def window_violation(self, segs):
        idx = self.seg_index() if self.running() else 0
        return any(segs[i] != self.origin[i] for i in range(min(idx, len(self.origin))))

    # -- page
    def open_page(self):
        self.page = "edit_firing"
        src = self.working if self.working is not None else self.origin
        self.page_segs = copy.deepcopy(src)
        self.cur_seg = self.seg_index()
        self.page_released = False

    def page_active(self):
        if self.page_released:
            return False
        if not self.running() and not self.page_ignores_end:
            self.page_released = True
            return False
        return True

    def editable(self):
        if not self.page_active():
            return False
        return self.steppers_stay_when_locked or self.cur_seg >= self.seg_index()

    def targets(self):
        t = [{"name": "back", "cx": 333, "cy": 20, "hidden": False},
             {"name": "home", "cx": 373, "cy": 20, "hidden": False}]
        if self.page_active():
            if self.cur_seg > 0:
                t.append({"name": GLYPH, "cx": PREV_X, "cy": 20, "hidden": False})
            if self.cur_seg + 1 < len(self.page_segs):
                t.append({"name": GLYPH, "cx": NEXT_X, "cy": 20, "hidden": False})
            t.append({"name": "Apply", "cx": 239, "cy": 255, "hidden": False})
            # kiln_ui.c's tap walk lists each (CLICKABLE) row container too, its
            # name borrowed from the caption label, centred mid-row.
            for caption, y in zip(("Target", "Ramp", "Dwell"), ROW_Y.values()):
                t.append({"name": caption, "cx": 240, "cy": y, "hidden": False})
            if self.editable():
                for y in ROW_Y.values():
                    t.append({"name": GLYPH, "cx": MINUS_X, "cy": y, "hidden": False})
                    t.append({"name": GLYPH, "cx": PLUS_X, "cy": y, "hidden": False})
        return t

    def press(self, x, y):
        if self.page != "edit_firing":
            return
        if self.keypad_on_nav and (x, y) == (NEXT_X, 20):
            self.keypad = True
            return
        if (x, y) == (NEXT_X, 20) and self.page_active():
            self.cur_seg = min(self.cur_seg + 1, len(self.page_segs) - 1)
        elif (x, y) == (PREV_X, 20) and self.page_active():
            self.cur_seg = max(self.cur_seg - 1, 0)
        else:
            for field, ry in ROW_Y.items():
                if y == ry and x in (MINUS_X, PLUS_X) and self.editable():
                    key = {"target": "target_c", "ramp": "ramp_c_per_hr", "dwell": "dwell_min"}[field]
                    step = 5.0 if x == PLUS_X else -5.0
                    seg = self.page_segs[self.cur_seg]
                    seg[key] = max(0.0, seg[key] + step)

    def apply(self):
        if not self.page_active():
            return
        if self.window_violation(self.page_segs) and not self.stale_apply_adopts:
            return  # the LCD's own window check refuses; nothing saved
        if self.apply_works:
            self.working = copy.deepcopy(self.page_segs)


class FakeProfiles:
    def __init__(self, b):
        self.b = b

    def save(self, slot, name, zone_mask, segments):
        self.b.calls.append("save")
        self.b.origin = [{"target_c": float(s.target_c), "ramp_c_per_hr": float(s.ramp_c_per_hr),
                          "dwell_min": float(s.dwell_min)} for s in segments]
        return SimpleNamespace(ok=True, error="")

    def start(self, slot):
        self.b.calls.append("start")
        self.b.started_at = self.b.clock.now()
        return SimpleNamespace(ok=True, error="")

    def stop(self):
        self.b.calls.append("stop")
        self.b.exec_forced = "idle"
        if self.b.foreign_after_stop:
            self.b.working_id = 11
            self.b.working = [{"target_c": 1.0, "ramp_c_per_hr": 1.0, "dwell_min": 1.0}]

    def delete(self, slot):
        self.b.calls.append("delete")

    def get_exec_status(self):
        st, idx = self.b.exec_state()
        return SimpleNamespace(state_name=st, profile_id=7, segment_count=len(self.b.origin or []),
                               segment_index=idx)


class FakeTouch:
    def __init__(self, b):
        self.b = b

    def get_state(self):
        return SimpleNamespace(screen_on=True, idle_ms=0)

    def inject(self, x, y, pressed):
        if pressed:
            self.b.presses.append((x, y))
            self.b.press(x, y)
        return SimpleNamespace(ok=True)


_KEYPAD = [{"name": n, "cx": 10, "cy": 10, "hidden": False}
           for n in ["OK", "Cancel"] + [str(i) for i in range(10)]]


class FakeUi:
    def __init__(self, b):
        self.b = b

    def get_current_page(self):
        return self.b.page

    def list_tap_targets(self):
        b = self.b
        if b.keypad:
            return {"targets": _KEYPAD, "truncated": False}
        if b.page == "edit_firing":
            return {"targets": b.targets(), "truncated": False}
        return {"targets": [{"name": "Edit", "cx": 240, "cy": 280, "hidden": False}], "truncated": False}

    def click_by_name(self, name):
        b = self.b
        b.calls.append(f"click:{name}")
        if name == "Edit" and b.page == "home":
            if not b.running():
                return {"result": "not_found"}
            if b.locked:
                b.keypad = True
            else:
                b.open_page()
            return {"result": "ok"}
        if name == "Cancel" and b.keypad:
            b.keypad = False
            return {"result": "ok"}
        if name in ("home", "back"):
            b.page = "home"
            return {"result": "ok"}
        if name == "Apply" and b.page == "edit_firing":
            b.apply()
            return {"result": "ok"}
        return {"result": "not_found"}


class FakeLiveClient:
    def __init__(self, b):
        self.b = b

    def get_live_status(self, host):
        b = self.b
        wid = b.working_id if b.working is not None else -1
        if b.foreign_wid_at_end and b.working is not None and not b.running():
            wid = 11
        return {"active": b.running(), "working_id": wid,
                "pending_decision": (b.working is not None and not b.running() and not b.no_pending_decision),
                "last_refusal": b.last_refusal}

    def get_live_content(self, host):
        if self.b.working is None:
            raise FakeLiveError(409)
        return {"id": self.b.working_id, "name": "bench", "segments": copy.deepcopy(self.b.working)}

    def edit_live(self, host, name, zone_mask, segments):
        b = self.b
        b.calls.append("edit_live")
        segs = [{"target_c": s["target_c"], "ramp_c_per_hr": s["ramp_c_per_hr"], "dwell_min": s["dwell_min"]}
                for s in segments]
        if any(s["target_c"] > 2015 for s in segs):
            if b.http_bound_status == 400:
                raise FakeLiveError(400, "target_c out of range")
        elif b.max_temp > 0 and any(s["target_c"] > b.max_temp for s in segs) and not b.ceiling_accepts:
            raise FakeLiveError(400, "target exceeds zone ceiling")
        elif b.window_violation(segs) and not b.http_window_accepts:
            raise FakeLiveError(409, "segment 1 has already run")
        b.working = segs
        return {"ok": True}

    def decide_live_discard(self, host):
        self.b.calls.append("discard")
        if self.b.working is None:
            raise FakeLiveError(409)
        if self.b.discard_works:
            self.b.working = None
        return {"ok": True}


def _http(b):
    def get(h, p):
        if p != "/api/profile_exec":
            return 404, {}
        if b.running():
            return 200, {"zones": [{"zone": 0, "relay_on": False}]}
        return 200, {"zones": []}
    return get


def _run(case, kind, edit_heat=True, allow_heat=True, ambient=ZONE_TEMP, **kw):
    clock = _Clock()
    b = FakeBoard(clock, kind=kind, **kw)
    srv = SimpleNamespace(
        _ui_test=FakeUi(b), _profiles=FakeProfiles(b), _touch=FakeTouch(b),
        _thermo=SimpleNamespace(read=lambda *a, **k: [
            SimpleNamespace(channel=0, temperature_c=ambient, valid=True)]),
    )
    ctx = {
        "srv": srv, "host": "1.2.3.4", "allow_heat": allow_heat, "lcd22_allow_heat": edit_heat,
        "_now": clock.now, "_sleep": clock.sleep, "_profile_live_client": FakeLiveClient(b),
        "capability_preflight_run": lambda *_a, **_k: SimpleNamespace(ok=True),
        "_get_zones_config": lambda h: {"zones": [
            {"index": 0, "max_temp_c": b.max_temp, "max_ramp_c_per_hr": b.max_ramp}]},
        "_http_get_json": _http(b),
    }
    with mock.patch("kilnctrl.dashboard_http_client.get_status", side_effect=lambda h: dict(
            safety_relay_energized=b.relays_energized)):
        result = case(ctx)
    return result, b


def run23(**kw):
    return _run(C._case_lcd23, "lcd23", **kw)


def run24(**kw):
    return _run(C._case_lcd24, "lcd24", **kw)


class RegistryTest(unittest.TestCase):
    def test_registered_heat_cases_with_judge(self):
        for cid, fn in (("LCD-23", C._case_lcd23), ("LCD-24", C._case_lcd24)):
            spec = R.get_case(cid)
            self.assertTrue(spec.heat, cid)
            self.assertIs(spec.judge, fn)
            self.assertIn(cid, R.SUITES["lcd"])


class GateTest(unittest.TestCase):
    def test_not_run_without_lcd_edit_heat(self):
        for run in (run23, run24):
            result, b = run(edit_heat=False)
            self.assertEqual(result.verdict, Verdict.NOT_RUN)
            self.assertIn("lcd_edit_heat", result.reason)
            self.assertEqual(b.calls, [])

    def test_merely_truthy_opt_in_is_not_run(self):
        for run in (run23, run24):
            result, b = run(edit_heat=1)
            self.assertEqual(result.verdict, Verdict.NOT_RUN)
            self.assertEqual(b.calls, [])

    def test_allow_heat_false_is_not_run(self):
        for run in (run23, run24):
            result, b = run(allow_heat=False)
            self.assertEqual(result.verdict, Verdict.NOT_RUN)
            self.assertIn("allow_heat", result.reason)
            self.assertEqual(b.calls, [])

    def test_allow_heat_truthy_is_not_enough(self):
        result, b = run23(allow_heat=1)
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_preexisting_live_edit_is_inconclusive_untouched(self):
        for run in (run23, run24):
            result, b = run(preexisting_working=True)
            self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
            self.assertIn("pending", result.reason)
            self.assertEqual(b.calls, [])
            self.assertIsNotNone(b.working)


class Lcd23Test(unittest.TestCase):
    def test_pass(self):
        result, b = run23()
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertTrue(result.observed["cleanup"]["verified"])
        self.assertIsNone(b.working)
        self.assertIn("stop", b.calls)
        self.assertEqual(b.calls.count("click:Edit"), 1)
        self.assertEqual(b.calls.count("click:Apply"), 2)
        self.assertNotIn("click:Confirm Stop", b.calls)
        self.assertEqual(result.observed["cleanup"]["own_working_id"], 9)
        # net edit: target -5, ramp +5, dwell -5 on segment index 1
        self.assertEqual(result.observed["expected"][1],
                         {"target_c": BASE + 5.0, "ramp_c_per_hr": 605.0, "dwell_min": 25.0})

    def test_stepper_taps_cover_ramp_minus_and_plus(self):
        result, b = run23()
        taps = [t for t in result.observed["taps"] if "field" in t]
        self.assertEqual([(t["field"], t["sign"]) for t in taps[:5]],
                         [("target", "-"), ("ramp", "+"), ("ramp", "+"), ("ramp", "-"), ("dwell", "-")])
        ramp_y = ROW_Y["ramp"]
        self.assertIn((MINUS_X, ramp_y), b.presses)
        self.assertIn((PLUS_X, ramp_y), b.presses)

    def test_stale_apply_that_adopts_is_fail(self):
        result, b = run23(stale_apply_adopts=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("after the LCD Apply", result.reason)

    def test_steppers_not_locked_is_fail(self):
        result, b = run23(steppers_stay_when_locked=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("not locked", result.reason)

    def test_http_window_accepted_is_fail(self):
        result, b = run23(http_window_accepts=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("window violation answered None", result.reason)

    def test_http_bound_accepted_is_fail(self):
        result, b = run23(http_bound_status=200)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("bound violation", result.reason)

    def test_step_edit_not_adopted_is_fail_and_never_reaches_part_two(self):
        result, b = run23(apply_works=False)
        # nothing adopted: the judge reads no working copy at all
        self.assertIn(result.verdict, (Verdict.FAIL, Verdict.INCONCLUSIVE))
        self.assertEqual(b.calls.count("click:Apply"), 1)
        self.assertNotIn("edit_live", b.calls)

    def test_firing_never_leaves_segment_zero_is_inconclusive(self):
        result, b = run23(never_advances=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("did not leave segment 0", result.reason)
        self.assertNotIn("edit_live", b.calls)
        self.assertTrue(result.observed["cleanup"]["verified"])

    def test_keypad_on_nav_tap_is_dismissed_and_nothing_else_tapped(self):
        result, b = run23(keypad_on_nav=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("keypad", result.reason)
        # only the nav tap, then the keypad backdrop dismissal -- no digit/OK
        self.assertEqual(b.presses, [(NEXT_X, 20), C._KEYPAD_BACKDROP_XY])
        self.assertNotIn("click:Apply", b.calls)
        self.assertEqual(b.exec_state()[0], "idle")

    def test_locked_panel_never_gets_a_tap(self):
        result, b = run23(locked=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(b.calls.count("click:Edit"), 1)
        self.assertEqual(b.presses, [C._KEYPAD_BACKDROP_XY])  # dismissal only, never a digit
        self.assertTrue(all(c in ("save", "start", "stop", "delete", "click:Edit", "click:Cancel", "click:home",
                                  "click:back") for c in b.calls), b.calls)

    def test_foreign_working_copy_after_stop_is_never_discarded(self):
        result, b = run23(foreign_after_stop=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("cleanup could not be verified", result.reason)
        self.assertNotIn("discard", b.calls)

    def test_relays_still_energized_forces_fail(self):
        result, b = run23(relays_energized=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("cleanup could not be verified", result.reason)

    def test_zone_temperature_too_high_starts_nothing(self):
        result, b = run23(ambient=45.0)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(b.calls, [])


class Lcd23GeometryAndLimitsTest(unittest.TestCase):
    def test_minus_taps_hit_the_minus_stepper_not_the_row_container(self):
        result, b = run23()
        ramp_y = ROW_Y["ramp"]
        self.assertIn((MINUS_X, ramp_y), b.presses)
        self.assertNotIn((240, ramp_y), b.presses)   # the row container's centre
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_ceiling_probe_posts_max_temp_plus_10_and_passes(self):
        result, b = run23(max_temp=70.0)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(result.observed["ceiling_probe_target_c"], 80.0)
        self.assertEqual(result.observed["http_ceiling"][0], 400)
        self.assertEqual(b.calls.count("edit_live"), 3)

    def test_ceiling_probe_accepted_is_fail_and_stops_the_firing_at_once(self):
        result, b = run23(max_temp=70.0, ceiling_accepts=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("zone-ceiling violation", result.reason)
        last_edit = len(b.calls) - 1 - b.calls[::-1].index("edit_live")
        self.assertEqual(b.calls.index("stop"), last_edit + 1)
        # one stop sent immediately on the accepted probe, one by cleanup
        self.assertEqual(b.calls.count("stop"), 2)
        self.assertEqual(b.exec_state()[0], "idle")

    def test_ceiling_probe_skipped_when_ceiling_unusable(self):
        for mt in (0.0, 2010.0):
            result, b = run23(max_temp=mt)
            self.assertEqual(result.verdict, Verdict.PASS, result.reason)
            self.assertIsNone(result.observed["http_ceiling"])
            self.assertEqual(b.calls.count("edit_live"), 2)

    def test_low_ramp_ceiling_lowers_the_base_ramp(self):
        result, b = run23(max_ramp=100.0)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(result.observed["orig"][1]["ramp_c_per_hr"], 90.0)
        self.assertEqual(result.observed["expected"][1]["ramp_c_per_hr"], 95.0)

    def test_ramp_ceiling_with_no_room_is_inconclusive_no_action(self):
        for run in (run23, run24):
            result, b = run(max_ramp=15.0)
            self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
            self.assertIn("ramp ceiling", result.reason)
            self.assertEqual(b.calls, [])

    def test_ramp_ceiling_edit_never_exceeds_ceiling(self):
        for ceiling in (20.0, 37.5, 100.0, 605.0, 614.0, 700.0):
            result, b = run23(max_ramp=ceiling)
            self.assertEqual(result.verdict, Verdict.PASS, (ceiling, result.reason))
            self.assertLessEqual(result.observed["expected"][1]["ramp_c_per_hr"], ceiling)


class NavSlotTest(unittest.TestCase):
    """_lcd_edit_nav derives Prev/Next from computed topbar slots, so a disabled
    Next (absent from the listing) is never mistaken for the rightmost Prev."""

    def _state(self, cur_seg):
        clock = _Clock()
        b = FakeBoard(clock, kind="lcd23")
        b.origin = [{"target_c": 1.0, "ramp_c_per_hr": 1.0, "dwell_min": 1.0}] * 3
        b.started_at = 0.0
        b.open_page()
        b.cur_seg = cur_seg
        return C._lcd_edit_page_state(FakeUi(b))

    def test_middle_segment_has_both(self):
        st = self._state(1)
        self.assertEqual(C._lcd_edit_nav(st, "prev")[0], PREV_X)
        self.assertEqual(C._lcd_edit_nav(st, "next")[0], NEXT_X)

    def test_last_segment_next_is_disabled_not_prev(self):
        st = self._state(2)
        self.assertIsNone(C._lcd_edit_nav(st, "next"))
        self.assertEqual(C._lcd_edit_nav(st, "prev")[0], PREV_X)

    def test_first_segment_prev_is_disabled(self):
        st = self._state(0)
        self.assertIsNone(C._lcd_edit_nav(st, "prev"))
        self.assertEqual(C._lcd_edit_nav(st, "next")[0], NEXT_X)

    def test_missing_anchors_means_no_nav(self):
        self.assertIsNone(C._lcd_edit_nav({"topbar": {"prev": None, "next": None}}, "next"))
        self.assertIsNone(C._lcd_edit_nav({}, "prev"))


class Lcd24Test(unittest.TestCase):
    def test_pass(self):
        result, b = run24()
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertTrue(result.observed["cleanup"]["verified"])
        self.assertEqual(b.calls.count("discard"), 1)
        self.assertEqual(result.observed["end_state"], "done")
        self.assertTrue(result.observed["lcd_ended"])
        self.assertIsNone(b.working)

    def test_page_still_offering_apply_after_end_is_fail(self):
        result, b = run24(page_ignores_end=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("still offered Apply", result.reason)

    def test_no_pending_decision_is_fail(self):
        result, b = run24(no_pending_decision=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("pending_decision", result.reason)

    def test_discard_that_leaves_working_copy_is_fail(self):
        result, b = run24(discard_works=False)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("remains after decide discard", result.reason)

    def test_firing_ending_faulted_is_fail(self):
        result, b = run24(ends_as="faulted")
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("expected 'done'", result.reason)

    def test_firing_never_ends_is_inconclusive_and_stopped(self):
        result, b = run24(never_advances=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("did not end", result.reason)
        self.assertIn("stop", b.calls)
        self.assertTrue(result.observed["cleanup"]["verified"])
        self.assertIsNone(b.working)

    def test_discard_not_attempted_for_a_foreign_working_id(self):
        result, b = run24(foreign_wid_at_end=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("not attempted", result.reason)
        self.assertEqual(b.calls.count("discard"), 0)

    def test_only_one_edit_click_and_no_pin_taps(self):
        result, b = run24()
        self.assertEqual(b.calls.count("click:Edit"), 1)
        self.assertEqual({p for p in b.presses} - {(NEXT_X, 20), (PLUS_X, ROW_Y["ramp"])}, set())

    def test_keypad_after_edit_click_is_inconclusive_no_taps(self):
        result, b = run24(locked=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(b.presses, [C._KEYPAD_BACKDROP_XY])  # dismissal only, never a digit


class JudgeTest(unittest.TestCase):
    EXP = [{"target_c": 25.0, "ramp_c_per_hr": 600.0, "dwell_min": 1.0},
           {"target_c": 30.0, "ramp_c_per_hr": 605.0, "dwell_min": 25.0}]
    RUN = {"state_name": "running"}

    def _status(self, **kw):
        s = {"active": True, "working_id": 9, "last_refusal": None, "pending_decision": False}
        s.update(kw)
        return s

    def test_ramp_judge_pass_and_each_field_fails(self):
        ok = J.judge_lcd_edit_ramp_steppers(self.EXP, self._status(), {"segments": copy.deepcopy(self.EXP)}, self.RUN)
        self.assertEqual(ok.verdict, Verdict.PASS)
        for key in ("target_c", "ramp_c_per_hr", "dwell_min"):
            segs = copy.deepcopy(self.EXP)
            segs[1][key] += 5.0
            bad = J.judge_lcd_edit_ramp_steppers(self.EXP, self._status(), {"segments": segs}, self.RUN)
            self.assertEqual(bad.verdict, Verdict.FAIL, key)
            self.assertIn(key, bad.reason)
        segs = copy.deepcopy(self.EXP)
        segs[0]["dwell_min"] = 9.0
        self.assertEqual(J.judge_lcd_edit_ramp_steppers(self.EXP, self._status(), {"segments": segs},
                                                        self.RUN).verdict, Verdict.FAIL)

    def test_ramp_judge_refusal_and_not_running(self):
        c = {"segments": copy.deepcopy(self.EXP)}
        r = J.judge_lcd_edit_ramp_steppers(self.EXP, self._status(last_refusal={"message": "x"}), c, self.RUN)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertIn("refused", r.reason)
        r = J.judge_lcd_edit_ramp_steppers(self.EXP, self._status(), c, {"state_name": "idle"})
        self.assertEqual(r.verdict, Verdict.FAIL)
        r = J.judge_lcd_edit_ramp_steppers(self.EXP, None, None, self.RUN)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_ramp_judge_preexisting_refusal_is_not_blamed(self):
        c = {"segments": copy.deepcopy(self.EXP)}
        old = {"message": "old"}
        r = J.judge_lcd_edit_ramp_steppers(self.EXP, self._status(last_refusal=old), c, self.RUN,
                                           refusal_before=old)
        self.assertEqual(r.verdict, Verdict.PASS)

    def _refusal(self, **kw):
        args = dict(adopted=self.EXP, wid_before=9, status_after=self._status(), content_after={"segments": copy.deepcopy(self.EXP)},
                    exec_after=self.RUN, refusal_before=None, locked_seen=True, http_window=(409, "x"),
                    http_bound=(400, "y"), content_after_http={"segments": copy.deepcopy(self.EXP)})
        args.update(kw)
        return J.judge_lcd_edit_refusal(**args)

    def test_refusal_judge(self):
        self.assertEqual(self._refusal().verdict, Verdict.PASS)
        self.assertEqual(self._refusal(locked_seen=False).verdict, Verdict.FAIL)
        self.assertEqual(self._refusal(http_window=(None, "accepted")).verdict, Verdict.FAIL)
        self.assertEqual(self._refusal(http_window=(400, "x")).verdict, Verdict.FAIL)
        self.assertEqual(self._refusal(http_bound=(409, "y")).verdict, Verdict.FAIL)
        self.assertEqual(self._refusal(status_after=self._status(working_id=10)).verdict, Verdict.FAIL)
        self.assertEqual(self._refusal(status_after=self._status(last_refusal={"m": 1})).verdict, Verdict.FAIL)
        segs = copy.deepcopy(self.EXP)
        segs[0]["target_c"] += 5.0
        self.assertEqual(self._refusal(content_after={"segments": segs}).verdict, Verdict.FAIL)
        self.assertEqual(self._refusal(content_after_http={"segments": segs}).verdict, Verdict.FAIL)
        self.assertEqual(self._refusal(status_after=None).verdict, Verdict.INCONCLUSIVE)

    def test_refusal_judge_ceiling_probe(self):
        self.assertEqual(self._refusal(http_ceiling=(400, "z")).verdict, Verdict.PASS)
        self.assertEqual(self._refusal(http_ceiling=None).verdict, Verdict.PASS)
        bad = self._refusal(http_ceiling=(None, "accepted"))
        self.assertEqual(bad.verdict, Verdict.FAIL)
        self.assertIn("zone-ceiling violation", bad.reason)
        self.assertEqual(self._refusal(http_ceiling=(409, "z")).verdict, Verdict.FAIL)

    def _end(self, **kw):
        args = dict(adopted=self.EXP, own_wid=9, end_state="done", lcd_ended=True,
                    status_ended=self._status(active=False, pending_decision=True),
                    content_ended={"segments": copy.deepcopy(self.EXP)}, discard_error=None,
                    status_discarded=self._status(active=False, working_id=-1))
        args.update(kw)
        return J.judge_lcd_edit_firing_end(**args)

    def test_end_judge(self):
        self.assertEqual(self._end().verdict, Verdict.PASS)
        self.assertEqual(self._end(end_state=None).verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(self._end(end_state="faulted").verdict, Verdict.FAIL)
        self.assertEqual(self._end(lcd_ended=False).verdict, Verdict.FAIL)
        self.assertEqual(self._end(lcd_ended=None).verdict, Verdict.FAIL)
        self.assertEqual(self._end(status_ended=self._status(active=False, pending_decision=False)).verdict,
                         Verdict.FAIL)
        self.assertEqual(self._end(status_ended=self._status(active=False, pending_decision=True,
                                                             working_id=4)).verdict, Verdict.FAIL)
        self.assertEqual(self._end(discard_error="X:500").verdict, Verdict.FAIL)
        self.assertEqual(self._end(status_discarded=self._status(working_id=9)).verdict, Verdict.FAIL)
        self.assertEqual(self._end(status_discarded=self._status(working_id=-1, pending_decision=True)).verdict,
                         Verdict.FAIL)
        segs = copy.deepcopy(self.EXP)
        segs[1]["ramp_c_per_hr"] = 600.0
        self.assertEqual(self._end(content_ended={"segments": segs}).verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
