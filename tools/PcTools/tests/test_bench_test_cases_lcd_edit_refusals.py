#!/usr/bin/env python3
"""Unit tests for LCD-23 (Edit firing steppers + refusal paths), LCD-24
(end-of-firing) and LCD-25 (the PIN-gated Keep? decision page),
cases_lcd._case_lcd23/_case_lcd24/_case_lcd25 and the judgments
judge_lcd_edit_ramp_steppers / judge_lcd_edit_refusal / judge_lcd_edit_firing_end /
judge_lcd_keep_discard.
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
# the cases start segment 0 above the reading so the warm start cannot skip it
BASE = float(math.ceil(ZONE_TEMP)) + 2.0
BASE_FLOOR = float(math.floor(ZONE_TEMP))   # the old, warm-start-skipped base
ROW_Y = {"target": 86, "ramp": 136, "dwell": 186}
MINUS_X, PLUS_X = 100, 443  # real layout: minus right after the caption
GLYPH = "�" * 3     # ui_test_client decodes LVGL symbol names as U+FFFD
NEXT_X, PREV_X = 453, 413
ADVANCE_AT = 30.0   # seconds after start: segment 0 -> 1


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
        self.adopted_log = []       # every working copy ever adopted
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
        self.prev_ignored = kw.get("prev_ignored", False)
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
        # Segment the executor is ALREADY in when the page opens (hardware run
        # 20261002T013957Z: the firing was at segment_index 1 by the time Edit
        # was tapped, and ui_page_edit_firing.c:214 opens the page on the
        # RUNNING segment, not segment 0).
        self.start_seg = kw.get("start_seg", 0)   # extra segments already run past the warm-start entry
        self.zone_temp = kw.get("zone_temp", ZONE_TEMP)
        self.edit_never_listed = kw.get("edit_never_listed", False)
        # first N edit-page tap lists come back truncated, with the Prev/Next glyphs missing
        self.truncated_lists = kw.get("truncated_lists", 0)
        # truncate every edit-page list once the firing has advanced (LCD-23 lock wait)
        self.truncated_lists_late = kw.get("truncated_lists_late", False)
        # LCD-25: the home Keep? button, the PIN gate, the live_decide page and its confirm dialog
        self.keep_never_listed = kw.get("keep_never_listed", False)
        self.decide_locked = kw.get("decide_locked", False)
        self.pin_accepts = kw.get("pin_accepts", True)
        self.pin_incomplete = kw.get("pin_incomplete", False)
        self.origin_builtin = kw.get("origin_builtin", False)
        self.overwrite_shown_wrong = kw.get("overwrite_shown_wrong", False)
        self.decide_truncated = kw.get("decide_truncated", False)
        # names a TRUNCATED decide-page read silently drops (a real partial walk loses entries)
        self.decide_truncated_drops = set(kw.get("decide_truncated_drops", ()))
        self.decide_unreadable = kw.get("decide_unreadable", False)
        # the first N decide-page reads are flagged truncated (names all present), then clean
        self.decide_truncated_reads = kw.get("decide_truncated_reads", 0)
        # confirm dialog read: empty list, or truncated (dropping the named entries)
        self.confirm_unreadable = kw.get("confirm_unreadable", False)
        self.confirm_truncated = kw.get("confirm_truncated", False)
        self.confirm_truncated_drops = set(kw.get("confirm_truncated_drops", ()))
        self.confirm_tap_unknown = kw.get("confirm_tap_unknown", False)
        self.no_confirm_dialog = kw.get("no_confirm_dialog", False)
        self.lcd_discard_works = kw.get("lcd_discard_works", True)
        self.orig_changed_on_discard = kw.get("orig_changed_on_discard", False)
        self.stray_profile_on_discard = kw.get("stray_profile_on_discard", False)
        self.confirm = False
        self.decide_unlocked = False
        self.pin_entries = 0
        self.profile_ids = [7]
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
        segs = self.origin or []
        last = max(len(segs) - 1, 0)
        entry = self.entry_seg()
        first = min(entry + self.start_seg, last)
        if self.kind in ("lcd24", "lcd25") and not self.never_advances:
            end_at = sum(float(s["dwell_min"]) * 60.0 for s in segs[entry:])
            if el >= end_at:
                return self.ends_as, first
        if self.kind == "lcd23" and not self.never_advances and el >= ADVANCE_AT:
            return "running", min(first + 1, last)
        return "running", first

    def entry_seg(self):
        """Mirror of profile_executor_plan_warm_start() (profile_executor_start.c:
        371-479): over the leading non-descending ascent, enter at the first
        segment with zone_temp <= target (a target equal to the reading is NOT
        skipped); if the reading is above the whole ascent, land on the last
        ascent segment already dwelling (lines 470-477), so it runs only its
        dwell."""
        segs = self.origin or []
        ascent_end, last = len(segs), None
        for i, sg in enumerate(segs):
            t = float(sg["target_c"])
            if last is not None and t < last:
                ascent_end = i
                break
            last = t
        for i in range(ascent_end):
            if self.zone_temp <= float(segs[i]["target_c"]):
                return i
        return max(ascent_end - 1, 0)

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

    def keep_owed(self):
        return (self.working is not None and not self.running() and not self.no_pending_decision
                and not self.keep_never_listed)

    def decide_targets(self):
        if self.confirm:
            names = [n for n in ("Cancel", "Discard")
                     if not (self.confirm_truncated and n in self.confirm_truncated_drops)]
            if self.confirm_unreadable:
                names = []
            return [{"name": n, "cx": 10, "cy": 10, "hidden": False} for n in names]
        names = ["back", "home", "Discard edit", "Save as new (auto-named)"]
        if (not self.origin_builtin) != self.overwrite_shown_wrong:
            names.append("Overwrite original")
        if self.decide_unreadable:
            names = []
        elif self.decide_truncated:
            names = [n for n in names if n not in self.decide_truncated_drops]
        return [{"name": n, "cx": 10, "cy": 10, "hidden": False} for n in names]

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
        if self.keypad and (x, y) == C._KEYPAD_BACKDROP_XY:
            self.keypad = False
            return
        if self.page != "edit_firing":
            return
        if self.keypad_on_nav and (x, y) == (NEXT_X, 20):
            self.keypad = True
            return
        if (x, y) == (NEXT_X, 20) and self.page_active():
            self.cur_seg = min(self.cur_seg + 1, len(self.page_segs) - 1)
        elif (x, y) == (PREV_X, 20) and self.page_active() and not self.prev_ignored:
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
            self.adopted_log.append(copy.deepcopy(self.working))


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

    def get(self, slot):
        if self.b.origin is None:
            return None
        return SimpleNamespace(id=slot, segments=[
            SimpleNamespace(target_c=s["target_c"], ramp_c_per_hr=s["ramp_c_per_hr"], dwell_min=s["dwell_min"])
            for s in self.b.origin])

    def list_all(self):
        return [SimpleNamespace(id=i) for i in self.b.profile_ids]

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
        if b.page == "live_decide":
            if b.decide_truncated_reads > 0 and not b.confirm:
                b.decide_truncated_reads -= 1
                return {"targets": b.decide_targets(), "truncated": True}
            return {"targets": b.decide_targets(),
                    "truncated": b.decide_truncated or (b.confirm and b.confirm_truncated)}
        if b.page == "home" and b.keep_owed():
            return {"targets": [{"name": "Keep?", "cx": 240, "cy": 280, "hidden": False}], "truncated": False}
        if b.page == "edit_firing":
            late = b.truncated_lists_late and b.exec_state()[1] > b.entry_seg() + b.start_seg
            if b.truncated_lists > 0 or late:
                if not late:
                    b.truncated_lists -= 1
                return {"targets": [t for t in b.targets() if t["cy"] != 20 or t["name"] in ("back", "home")],
                        "truncated": True}
            return {"targets": b.targets(), "truncated": False}
        if b.edit_never_listed:
            return {"targets": [{"name": "Start", "cx": 240, "cy": 280, "hidden": False}], "truncated": False}
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
        if name == "Keep?" and b.page == "home" and b.keep_owed():
            if b.decide_locked and not b.decide_unlocked:
                b.keypad = True
            else:
                b.page = "live_decide"
            return {"result": "ok"}
        if name == "Discard edit" and b.page == "live_decide" and not b.confirm:
            b.confirm = not b.no_confirm_dialog
            return {"result": "ok"}
        if name == "Cancel" and b.confirm:
            b.confirm = False
            return {"result": "ok"}
        if name == "Discard" and b.confirm:
            b.confirm = False
            if b.lcd_discard_works:
                b.working = None
            if b.orig_changed_on_discard:
                b.origin = [dict(b.origin[0], target_c=b.origin[0]["target_c"] + 5.0)] + b.origin[1:]
            if b.stray_profile_on_discard:
                b.profile_ids.append(40)
            return {"result": "verdict_unknown" if b.confirm_tap_unknown else "ok"}
        if name in ("home", "back"):
            b.page = "home"
            b.confirm = False
            return {"result": "ok"}
        if name == "Apply" and b.page == "edit_firing":
            b.apply()
            return {"result": "ok"}
        return {"result": "not_found"}


    def enter_pin_verified(self, pin):
        """Models UiTestClient.enter_pin_verified: records only that an entry happened."""
        b = self.b
        b.pin_entries += 1
        ok = {"result": "ok"}
        entry = {"digit_results": [dict(ok) for _ in pin], "ok_result": dict(ok)}
        if b.pin_incomplete:
            entry["ok_result"] = None
            entry["entry_incomplete"] = True
            return entry
        if b.pin_accepts:
            b.keypad = False
            b.decide_unlocked = True
            b.page = "live_decide"
        return entry


class FakeLiveClient:
    def __init__(self, b):
        self.b = b

    def get_live_status(self, host):
        b = self.b
        wid = b.working_id if b.working is not None else -1
        if b.foreign_wid_at_end and b.working is not None and not b.running():
            wid = 11
        return {"active": b.running(), "working_id": wid, "origin_id": 7,
                "origin_is_builtin": b.origin_builtin,
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


def _run(case, kind, edit_heat=True, allow_heat=True, ambient=ZONE_TEMP, ctx_extra=None, **kw):
    clock = _Clock()
    b = FakeBoard(clock, kind=kind, zone_temp=ambient, **kw)
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
    ctx.update(ctx_extra or {})
    with mock.patch("kilnctrl.dashboard_http_client.get_status", side_effect=lambda h: dict(
            safety_relay_energized=b.relays_energized)):
        result = case(ctx)
    return result, b


def run23(**kw):
    return _run(C._case_lcd23, "lcd23", **kw)


def run24(**kw):
    return _run(C._case_lcd24, "lcd24", **kw)


PIN = "7391"


def run25(pin=PIN, heap=20000, **kw):
    extra = {"lcd_admin_pin": pin} if pin else {}
    if heap == "raise":
        def _heap(host):
            raise OSError("unreachable")
    else:
        def _heap(host):
            return {"heap_internal": {"min_free": heap}}
    extra["_get_heap_status"] = _heap
    with mock.patch.dict(os.environ):
        os.environ.pop("KILNCTL_LCD_PIN", None)
        return _run(C._case_lcd25, "lcd25", ctx_extra=extra, **kw)


class RegistryTest(unittest.TestCase):
    def test_registered_heat_cases_with_judge(self):
        for cid, fn in (("LCD-23", C._case_lcd23), ("LCD-24", C._case_lcd24), ("LCD-25", C._case_lcd25)):
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
        # net edit: target -5, ramp +5, dwell -5 on segment index 2
        self.assertEqual(result.observed["expected"][2],
                         {"target_c": BASE + 15.0, "ramp_c_per_hr": 605.0, "dwell_min": 25.0})

    def test_pass_when_executor_is_already_past_segment_zero_at_open(self):
        # Hardware run 20261002T013957Z: the page opens on the RUNNING segment
        # (ui_page_edit_firing.c:214), which was already 1 when Edit was tapped.
        result, b = run23(start_seg=1)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        exp = result.observed["expected"]
        orig = result.observed["orig"]
        changed = [i for i in range(len(exp)) if exp[i] != orig[i]]
        self.assertEqual(changed, [2])
        # the segment actually adopted is the one the case intended to edit
        first = b.adopted_log[0]
        self.assertEqual(first[2]["target_c"], orig[2]["target_c"] - 5.0)
        self.assertEqual(first[1], orig[1])
        self.assertEqual(first[3], orig[3])

    def test_executor_already_at_the_edit_segment_is_inconclusive_no_edit(self):
        result, b = run23(start_seg=2)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertNotIn("click:Apply", b.calls)
        self.assertNotIn("edit_live", b.calls)
        self.assertTrue(result.observed["cleanup"]["verified"])

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

    def test_bound_probe_accepted_is_fail_and_stops_the_firing_at_once(self):
        result, b = run23(http_bound_status=200)
        self.assertEqual(result.verdict, Verdict.FAIL)
        first_edits = [i for i, c in enumerate(b.calls) if c == "edit_live"]
        # window probe, then bound probe; stop must directly follow the bound probe
        self.assertEqual(b.calls[first_edits[1] + 1], "stop")
        self.assertEqual(b.calls.count("stop"), 2)
        self.assertEqual(b.exec_state()[0], "idle")

    def test_prev_tap_not_moving_page_is_inconclusive_without_apply(self):
        result, b = run23(prev_ignored=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("page did not show segment", result.reason)
        # navigation is verified before any edit, so no Apply is ever sent
        self.assertEqual(b.calls.count("click:Apply"), 0)
        self.assertNotIn("edit_live", b.calls)

    def test_ceiling_probe_skipped_when_ceiling_unusable(self):
        for mt in (0.0, 2010.0):
            result, b = run23(max_temp=mt)
            self.assertEqual(result.verdict, Verdict.PASS, result.reason)
            self.assertTrue(result.observed["ceiling_probe_skipped"])
            self.assertIn("ceiling probe skipped", result.reason)
            self.assertIsNone(result.observed["http_ceiling"])
            self.assertEqual(b.calls.count("edit_live"), 2)

    def test_low_ramp_ceiling_lowers_the_base_ramp(self):
        result, b = run23(max_ramp=100.0)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(result.observed["orig"][2]["ramp_c_per_hr"], 90.0)
        self.assertEqual(result.observed["expected"][2]["ramp_c_per_hr"], 95.0)

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
            self.assertLessEqual(result.observed["expected"][2]["ramp_c_per_hr"], ceiling)


class TruncatedListTest(unittest.TestCase):
    def test_truncated_list_is_unreadable_not_icon_absent(self):
        clock = _Clock()
        b = FakeBoard(clock, kind="lcd23", zone_temp=0.0, truncated_lists=1)
        b.origin = [{"target_c": 1.0, "ramp_c_per_hr": 1.0, "dwell_min": 1.0}] * 3
        b.started_at = 0.0
        b.open_page()
        st = C._lcd_edit_page_state(FakeUi(b))
        self.assertFalse(st["readable"])
        self.assertTrue(st["truncated"])

    def test_navigation_repolls_through_truncated_lists(self):
        result, b = run23(truncated_lists=3)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(b.truncated_lists, 0)


class ReadCounterPresenceTest(unittest.TestCase):
    """The Edit-page read counters are recorded on every outcome, 0 when nothing was seen."""
    KEYS = ("edit_page_truncated_reads", "edit_page_unreadable_reads", "edit_settle_truncated_reads")

    def _zero(self, result):
        for k in self.KEYS:
            self.assertIn(k, result.observed)
            self.assertEqual(result.observed[k], 0, k)

    def test_lcd23_pass_records_zero_counters(self):
        result, _b = run23()
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self._zero(result)

    def test_lcd24_pass_records_zero_counters(self):
        result, _b = run24()
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self._zero(result)

    def test_lcd23_fail_records_zero_counters(self):
        result, _b = run23(steppers_stay_when_locked=True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self._zero(result)

    def test_inconclusive_before_the_page_opens_still_has_counters(self):
        result, _b = run24(page_ignores_end=True)
        for k in self.KEYS:
            self.assertIn(k, result.observed)

    def test_truncated_run_counts_are_nonzero_and_keys_unchanged(self):
        result, _b = run23(truncated_lists=1000)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        for k in self.KEYS:
            self.assertIn(k, result.observed)
        self.assertGreater(result.observed["edit_page_truncated_reads"], 0)


class LockWaitTruncationTest(unittest.TestCase):
    def test_truncated_reads_during_lock_wait_are_inconclusive_not_fail(self):
        # steppers never lock (fake) AND the lock-wait reads are truncated
        result, b = run23(steppers_stay_when_locked=True, truncated_lists_late=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("unreadable", result.reason)
        self.assertGreater(result.observed["edit_page_truncated_reads"], 0)

    def test_steppers_not_locked_with_clean_reads_is_still_fail(self):
        result, b = run23(steppers_stay_when_locked=True)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_navigation_failure_reason_reports_truncated_reads(self):
        result, b = run23(truncated_lists=1000)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("truncated", result.reason)
        self.assertGreater(result.observed["edit_page_truncated_reads"], 0)


class NavSlotTest(unittest.TestCase):
    """_lcd_edit_nav derives Prev/Next from computed topbar slots, so a disabled
    Next (absent from the listing) is never mistaken for the rightmost Prev."""

    def _state(self, cur_seg):
        clock = _Clock()
        b = FakeBoard(clock, kind="lcd23", zone_temp=0.0)
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


class WarmStartRegressionTest(unittest.TestCase):
    """The executor warm-starts every firing, skipping segments whose target is
    <= the zone temperature (profile_executor_start.c:371-479). The first
    hardware run used a floor(zone temp) segment-0 target, which was skipped.
    These tests use the OLD floor-based plan against the fake, which mirrors
    the warm start, to prove the fake would have caught it."""

    def _old_plan(self, kind):
        def plan(ctx, zone_temp):
            ramp = 600.0
            if kind == "lcd23":
                offs, dw = (0.0, 10.0, 20.0, 25.0), (1, 1, 30, 5)
            else:
                offs, dw = (0.0, 0.0, 0.0), (1, 1, 0)
            base = float(math.floor(zone_temp))
            orig = [C._lcd_edit_seg(base + o, ramp, d) for o, d in zip(offs, dw)]
            from kilnctrl import devices
            return ([C._lcd_edit_pstep(devices.ProfileSegment, s) for s in orig], orig, base + offs[-1], {"max_temp_c": 1300.0, "max_ramp_c_per_hr": 1000.0})
        return plan

    def test_fake_enters_past_segment_zero_for_a_floor_base(self):
        clock = _Clock()
        b = FakeBoard(clock, kind="lcd23", zone_temp=ZONE_TEMP)
        b.origin = [{"target_c": BASE0, "ramp_c_per_hr": 600.0, "dwell_min": 1.0}
                    for BASE0 in (BASE_FLOOR, BASE_FLOOR + 10)]
        b.started_at = 0.0
        self.assertEqual(b.exec_state(), ("running", 1))

    def test_fake_lands_on_the_last_ascent_segment_when_hotter_than_all(self):
        # firmware lines 470-477: hotter than the whole ascent -> last ascent
        # segment, already dwelling; for the old LCD-24 plan that is the
        # zero-dwell segment, so the firing ends at once.
        clock = _Clock()
        b = FakeBoard(clock, kind="lcd24", zone_temp=ZONE_TEMP)
        b.origin = [{"target_c": BASE_FLOOR, "ramp_c_per_hr": 600.0, "dwell_min": float(d)} for d in (1, 1, 0)]
        b.started_at = 0.0
        self.assertEqual(b.entry_seg(), 2)
        self.assertEqual(b.exec_state(), ("done", 2))

    def test_target_equal_to_the_reading_is_not_skipped(self):
        clock = _Clock()
        b = FakeBoard(clock, kind="lcd23", zone_temp=30.0)
        b.origin = [{"target_c": 30.0, "ramp_c_per_hr": 600.0, "dwell_min": 1.0},
                    {"target_c": 40.0, "ramp_c_per_hr": 600.0, "dwell_min": 1.0}]
        b.started_at = 0.0
        self.assertEqual(b.entry_seg(), 0)

    def test_current_plans_enter_segment_zero(self):
        for case, kind in ((C._case_lcd23, "lcd23"), (C._case_lcd24, "lcd24")):
            result, b = _run(case, kind)
            self.assertEqual(result.verdict, Verdict.PASS, result.reason)
            self.assertEqual(result.observed["exec_at_page_open"]["segment_index"], 0)
            self.assertGreater(result.observed["orig"][0]["target_c"], ZONE_TEMP)

    def test_old_floor_plan_makes_lcd24_inconclusive_in_the_fake(self):
        with mock.patch.object(C, "_lcd24_plan", self._old_plan("lcd24")):
            result, b = _run(lambda ctx: C._lcd_edit_run(ctx, "LCD-24", C._lcd24_plan, C._lcd24_body), "lcd24")
        self.assertNotEqual(result.verdict, Verdict.PASS, result.reason)

    def test_old_floor_plan_lcd23_opens_past_segment_zero(self):
        with mock.patch.object(C, "_lcd23_plan", self._old_plan("lcd23")):
            result, b = _run(lambda ctx: C._lcd_edit_run(ctx, "LCD-23", C._lcd23_plan, C._lcd23_body), "lcd23")
        self.assertEqual(result.observed["exec_at_page_open"]["segment_index"], 1)


class Lcd24Test(unittest.TestCase):
    def test_pass_when_executor_is_already_past_segment_zero_at_open(self):
        result, b = run24(start_seg=1)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(result.observed["edit_segment"], 2)
        orig = result.observed["orig"]
        first = b.adopted_log[0]
        self.assertEqual(first[2]["ramp_c_per_hr"], orig[2]["ramp_c_per_hr"] + 5.0)
        self.assertEqual(first[1], orig[1])

    def test_records_the_edit_wait(self):
        result, b = run24()
        self.assertIn("edit_wait_s", result.observed)

    def test_edit_never_listed_while_running_is_inconclusive_with_diagnostic(self):
        result, b = run24(edit_never_listed=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("still running", result.reason)
        self.assertIn("waited", result.reason)
        self.assertIn("edit_wait_s", result.observed)
        self.assertNotIn("click:Edit", b.calls)
        self.assertTrue(result.observed["cleanup"]["verified"])

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


class Lcd25Test(unittest.TestCase):
    def test_pass_via_lcd_discard_only(self):
        result, b = run25()
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(b.calls.count("click:Keep?"), 1)
        i = [c for c in b.calls if c in ("click:Keep?", "click:Discard edit", "click:Discard")]
        self.assertEqual(i, ["click:Keep?", "click:Discard edit", "click:Discard"])
        self.assertEqual(b.calls.count("discard"), 0)  # the HTTP discard was never needed
        self.assertIsNone(b.working)
        self.assertFalse(result.observed["origin_is_builtin"])
        self.assertTrue(result.observed["cleanup"]["verified"])
        self.assertEqual(b.pin_entries, 0)  # unlocked panel: no keypad, no PIN typed

    def test_locked_panel_enters_the_pin_once_and_never_logs_it(self):
        result, b = run25(decide_locked=True)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(b.pin_entries, 1)
        self.assertEqual(result.observed["pin_entry"]["digit_count"], len(PIN))
        self.assertNotIn(PIN, repr(result.observed))
        self.assertNotIn(PIN, repr(b.calls))

    def test_locked_panel_pin_from_the_environment(self):
        with mock.patch.dict(os.environ, {"KILNCTL_LCD_PIN": PIN}):
            result, b = _run(C._case_lcd25, "lcd25", decide_locked=True,
                            ctx_extra={"_get_heap_status": lambda h: {"heap_internal": {"min_free": 20000}}})
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_locked_without_a_pin_is_inconclusive_and_types_nothing(self):
        result, b = run25(pin=None, decide_locked=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(b.pin_entries, 0)
        self.assertFalse(result.observed["pin_available"])
        self.assertEqual(b.presses[-1:], [C._KEYPAD_BACKDROP_XY])  # dismissal only
        self.assertNotIn("click:Discard edit", b.calls)
        self.assertTrue(result.observed["cleanup"]["verified"])
        self.assertEqual(b.calls.count("discard"), 1)  # cleanup discarded only the case's own copy

    def test_malformed_pin_counts_as_no_pin(self):
        result, b = run25(pin="12ab", decide_locked=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(b.pin_entries, 0)

    def test_wrong_pin_keypad_stays_is_inconclusive_one_attempt(self):
        result, b = run25(decide_locked=True, pin_accepts=False)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(b.pin_entries, 1)
        self.assertIn("PIN rejected", result.reason)
        self.assertNotIn("click:Discard edit", b.calls)
        self.assertFalse(b.keypad)

    def test_incomplete_pin_entry_is_inconclusive(self):
        result, b = run25(decide_locked=True, pin_incomplete=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertNotIn("click:Discard edit", b.calls)

    def test_keep_never_appearing_is_inconclusive_with_no_tap(self):
        result, b = run25(keep_never_listed=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertNotIn("click:Keep?", b.calls)
        self.assertEqual(b.pin_entries, 0)
        self.assertTrue(result.observed["cleanup"]["verified"])
        self.assertIsNone(b.working)  # the HTTP fallback discard of its own working id
        self.assertEqual(b.calls.count("discard"), 1)

    def test_no_pending_decision_is_fail_before_any_lcd_tap(self):
        result, b = run25(no_pending_decision=True)
        self.assertEqual(result.verdict, Verdict.FAIL, result.reason)
        self.assertIn("no LCD tap", result.reason)
        self.assertNotIn("click:Keep?", b.calls)

    def test_firing_not_done_is_inconclusive_with_no_tap(self):
        result, b = run25(ends_as="faulted")
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertNotIn("click:Keep?", b.calls)

    def test_overwrite_missing_for_a_user_origin_is_fail(self):
        result, b = run25(overwrite_shown_wrong=True)
        self.assertEqual(result.verdict, Verdict.FAIL, result.reason)
        self.assertIn("'Overwrite original' is missing", result.reason)

    def test_overwrite_offered_for_a_builtin_origin_is_fail(self):
        result, b = run25(origin_builtin=True, overwrite_shown_wrong=True)
        self.assertEqual(result.verdict, Verdict.FAIL, result.reason)
        self.assertIn("'Overwrite original' is offered", result.reason)

    def test_builtin_origin_without_overwrite_passes(self):
        result, b = run25(origin_builtin=True)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertTrue(result.observed["origin_is_builtin"])

    def test_truncated_page_read_is_inconclusive_and_taps_nothing(self):
        result, b = run25(overwrite_shown_wrong=True, decide_truncated=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertTrue(result.observed["page_truncated"])
        self.assertNotIn("click:Discard edit", b.calls)
        self.assertTrue(result.observed["cleanup"]["verified"])
        self.assertEqual(b.calls.count("discard"), 1)  # cleanup only

    def test_truncated_read_that_drops_a_name_is_not_a_missing_button_fail(self):
        for dropped in ("Discard edit", "Save as new (auto-named)"):
            result, b = run25(decide_truncated=True, decide_truncated_drops={dropped})
            self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, (dropped, result.reason))
            self.assertNotIn("has no", result.reason)
            self.assertNotIn("click:Discard edit", b.calls)
            self.assertTrue(result.observed["page_truncated"])

    def test_transient_truncated_page_read_is_repolled_until_clean(self):
        result, b = run25(decide_truncated_reads=2)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(b.decide_truncated_reads, 0)

    def test_unreadable_page_read_is_inconclusive_and_taps_nothing(self):
        result, b = run25(decide_unreadable=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertTrue(result.observed["page_unreadable"])
        self.assertNotIn("click:Discard edit", b.calls)
        self.assertNotIn("raised no confirm dialog", result.reason)
        self.assertTrue(result.observed["cleanup"]["verified"])

    def test_unreadable_confirm_dialog_is_inconclusive_and_never_taps_discard(self):
        result, b = run25(confirm_unreadable=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertTrue(result.observed["confirm_unreadable"])
        self.assertNotIn("click:Discard", b.calls)
        self.assertNotIn("click:Cancel", b.calls)  # nothing readable to dismiss with
        self.assertIn("no 'Cancel' was readable", result.reason)

    def test_truncated_confirm_dialog_is_cancelled_never_discarded(self):
        result, b = run25(confirm_truncated=True, confirm_truncated_drops={"Discard"})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertNotIn("click:Discard", b.calls)
        self.assertIn("click:Cancel", b.calls)
        self.assertFalse(b.confirm)
        self.assertEqual(result.observed["confirm_cancel_click"], "ok")

    def test_truncated_confirm_dialog_without_cancel_taps_nothing(self):
        result, b = run25(confirm_truncated=True, confirm_truncated_drops={"Cancel", "Discard"})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertNotIn("click:Discard", b.calls)
        self.assertNotIn("click:Cancel", b.calls)

    def test_confirm_tap_verdict_unknown_with_working_copy_left_is_inconclusive(self):
        result, b = run25(confirm_tap_unknown=True, lcd_discard_works=False)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertIn("confirm tap outcome unknown", result.reason)

    def test_confirm_tap_verdict_unknown_that_cleared_the_copy_passes(self):
        result, b = run25(confirm_tap_unknown=True)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_confirm_dialog_never_appearing_is_fail_and_cleanup_discards_over_http(self):
        result, b = run25(no_confirm_dialog=True)
        self.assertEqual(result.verdict, Verdict.FAIL, result.reason)
        self.assertIn("no confirm dialog", result.reason)
        self.assertNotIn("click:Discard", b.calls)
        self.assertIsNone(b.working)
        self.assertEqual(b.calls.count("discard"), 1)
        self.assertTrue(result.observed["cleanup"]["verified"])

    def test_discard_that_leaves_the_working_copy_is_fail(self):
        result, b = run25(lcd_discard_works=False)
        self.assertEqual(result.verdict, Verdict.FAIL, result.reason)
        self.assertIn("remains after the LCD discard", result.reason)
        self.assertEqual(b.calls.count("discard"), 1)  # cleanup, own working id only
        self.assertIsNone(b.working)

    def test_changed_original_is_fail(self):
        result, b = run25(orig_changed_on_discard=True)
        self.assertEqual(result.verdict, Verdict.FAIL, result.reason)
        self.assertIn("original profile changed", result.reason)

    def test_stray_new_profile_is_fail(self):
        result, b = run25(stray_profile_on_discard=True)
        self.assertEqual(result.verdict, Verdict.FAIL, result.reason)
        self.assertIn("unexpected new profile", result.reason)

    def test_relays_still_energized_forces_fail(self):
        result, b = run25(relays_energized=True)
        self.assertEqual(result.verdict, Verdict.FAIL, result.reason)

    def test_gates(self):
        for kw in ({"edit_heat": False}, {"allow_heat": False}):
            result, b = run25(**kw)
            self.assertEqual(result.verdict, Verdict.NOT_RUN)
            self.assertEqual(b.calls, [])

    def test_preexisting_live_edit_is_inconclusive_untouched(self):
        result, b = run25(preexisting_working=True)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, result.reason)
        self.assertEqual(b.calls, [])

    def test_heap_floor(self):
        result, b = run25(heap=8192)
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(result.observed["heap_internal_min_free"], 8192)
        result, b = run25(heap=8191)
        self.assertEqual(result.verdict, Verdict.FAIL, result.reason)
        self.assertIn("below the 8192 B floor", result.reason)
        self.assertIsNone(b.working)

    def test_unreadable_heap_is_inconclusive(self):
        for heap in ("raise", None, "n/a"):
            result, b = run25(heap=heap)
            self.assertEqual(result.verdict, Verdict.INCONCLUSIVE, (heap, result.reason))

    def test_save_as_and_overwrite_are_never_tapped(self):
        result, b = run25()
        self.assertNotIn("click:Overwrite original", b.calls)
        self.assertFalse([c for c in b.calls if c.startswith("click:Save as new")])


class KeepDiscardJudgeTest(unittest.TestCase):
    NAMES = {"back", "home", "Discard edit", "Save as new (auto-named)", "Overwrite original"}
    DONE = {"active": False, "working_id": -1, "pending_decision": False}

    def _j(self, **kw):
        args = dict(origin_is_builtin=False, page_names=set(self.NAMES), page_truncated=False,
                    confirm_seen=True, status_after=dict(self.DONE), orig_unchanged=True,
                    stray_profile_ids=[], heap_internal_min_free=20000)
        args.update(kw)
        return J.judge_lcd_keep_discard(**args)

    def test_pass_and_each_failure(self):
        self.assertEqual(self._j().verdict, Verdict.PASS)
        self.assertEqual(self._j(page_names=self.NAMES - {"Discard edit"}).verdict, Verdict.FAIL)
        self.assertEqual(self._j(page_names=self.NAMES - {"Save as new (auto-named)"}).verdict, Verdict.FAIL)
        self.assertEqual(self._j(page_names=self.NAMES - {"Overwrite original"}).verdict, Verdict.FAIL)
        self.assertEqual(self._j(origin_is_builtin=True).verdict, Verdict.FAIL)
        self.assertEqual(self._j(origin_is_builtin=True, page_names=self.NAMES - {"Overwrite original"}).verdict,
                         Verdict.PASS)
        self.assertEqual(self._j(confirm_seen=False).verdict, Verdict.FAIL)
        self.assertEqual(self._j(status_after=dict(self.DONE, working_id=9)).verdict, Verdict.FAIL)
        self.assertEqual(self._j(status_after=dict(self.DONE, pending_decision=True)).verdict, Verdict.FAIL)
        self.assertEqual(self._j(orig_unchanged=False).verdict, Verdict.FAIL)
        self.assertEqual(self._j(stray_profile_ids=[40]).verdict, Verdict.FAIL)
        self.assertEqual(self._j(heap_internal_min_free=8192).verdict, Verdict.PASS)
        self.assertEqual(self._j(heap_internal_min_free=8191).verdict, Verdict.FAIL)
        self.assertEqual(self._j(heap_internal_min_free=True).verdict, Verdict.INCONCLUSIVE)

    def test_inconclusive_when_facts_are_missing(self):
        self.assertEqual(self._j(origin_is_builtin=None).verdict, Verdict.INCONCLUSIVE)
        # ... but a problem already collected is never discarded by the unknown origin
        self.assertEqual(self._j(origin_is_builtin=None, orig_unchanged=False).verdict, Verdict.FAIL)
        self.assertEqual(self._j(origin_is_builtin=None, confirm_seen=False).verdict, Verdict.FAIL)
        self.assertEqual(self._j(origin_is_builtin=None, page_names=self.NAMES - {"Discard edit"}).verdict,
                         Verdict.FAIL)
        self.assertEqual(self._j(status_after=None).verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(self._j(orig_unchanged=None).verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(self._j(heap_internal_min_free=None).verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(self._j(status_after=None, confirm_seen=False).verdict, Verdict.FAIL)


class KeepDiscardJudgeUnreadableTest(unittest.TestCase):
    """Unreadable / truncated page reads and an untapped Discard edit never read as a
    missing button or a missing confirm dialog."""
    _j = KeepDiscardJudgeTest._j
    NAMES = KeepDiscardJudgeTest.NAMES
    DONE = KeepDiscardJudgeTest.DONE

    def test_unreadable_page_is_inconclusive_not_missing_buttons(self):
        r = self._j(page_names=None, confirm_seen=None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE, r.reason)
        self.assertNotIn("has no", r.reason)
        self.assertNotIn("raised no confirm dialog", r.reason)
        self.assertIsNone(r.observed["page_names"])

    def test_truncated_page_dropping_a_name_is_inconclusive(self):
        for dropped in ("Discard edit", "Save as new (auto-named)", "Overwrite original"):
            r = self._j(page_truncated=True, page_names=self.NAMES - {dropped}, confirm_seen=None)
            self.assertEqual(r.verdict, Verdict.INCONCLUSIVE, (dropped, r.reason))
            self.assertNotIn("has no", r.reason)

    def test_truncated_page_with_everything_visible_still_not_a_pass(self):
        self.assertEqual(self._j(page_truncated=True).verdict, Verdict.INCONCLUSIVE)

    def test_untapped_discard_edit_is_not_a_no_dialog_fail(self):
        r = self._j(confirm_seen=None)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE, r.reason)
        self.assertNotIn("raised no confirm dialog", r.reason)

    def test_unreadable_page_still_fails_on_real_problems(self):
        r = self._j(page_names=None, confirm_seen=None, orig_unchanged=False)
        self.assertEqual(r.verdict, Verdict.FAIL, r.reason)
        self.assertNotIn("has no", r.reason)


class PlanCountTest(unittest.TestCase):
    def test_lcd_suite_and_plan_counts_match(self):
        ids = R.SUITES["lcd"]
        self.assertEqual(len(ids), 25)
        heat = [c for c in ids if R.get_case(c).heat]
        self.assertEqual(heat, ["LCD-22", "LCD-23", "LCD-24", "LCD-25"])
        plan = os.path.join(os.path.dirname(__file__), "..", "..", "..", "docs", "BENCH_TEST_SYSTEM_PLAN.md")
        with open(plan, encoding="utf-8") as fh:
            text = fh.read()
        self.assertIn("| LCD | 25 | 4 (LCD-22, LCD-23, LCD-24, LCD-25) |", text)
        self.assertIn("| LCD-25 |", text)


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
