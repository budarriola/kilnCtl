#!/usr/bin/env python3
"""Unit tests for LCD-05 (brightness), LCD-06 (blank timeout) and LCD-13
(segments paging) in cases_lcd.py. Fake sampler and fake board only.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_lcd_display.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_lcd as C  # noqa: E402
from kilnctrl.bench_test import lcd_sampler  # noqa: E402
from kilnctrl.bench_test.registry import Verdict, get_case  # noqa: E402

_ORIG = {"brightness_percent": 80, "timeout_setting": 5, "keep_on_while_firing": True,
         "display_on_error": False, "brightness_inert": False}
_TARGETS = [{"name": "start", "cx": 240, "cy": 280, "hidden": False}]


class FakeTouch:
    def __init__(self, wakes=True):
        self.wakes = wakes
        self.injected = []

    def inject(self, x, y, pressed):
        self.injected.append((x, y, pressed))

    def get_state(self):
        class S:
            pass
        s = S()
        s.screen_on = self.wakes and bool(self.injected)
        return s


class FakeUi:
    def list_tap_targets(self):
        return {"targets": _TARGETS}


class FakeSrv:
    def __init__(self, touch=None):
        self._ui_test = FakeUi()
        self._touch = touch or FakeTouch()


class FakeBoard:
    def __init__(self, inert=False):
        self.cfg = dict(_ORIG, brightness_inert=inert)
        self.posts = []

    def get(self, path):
        return 200, dict(self.cfg)

    def post(self, path, fields):
        self.posts.append(dict(fields))
        return 200, {"ok": True}

    def ctx(self, srv):
        return {"srv": srv, "http_get_json": self.get, "http_post_json": self.post,
                "_sleep": lambda s: None}


def _run(case, board, srv, samples, bezel=(0, 0, 0)):
    """samples: iterator of RGB tuples returned by the fake sampler in order."""
    it = iter(samples)

    def fake_widget_body(path, cx, cy, *a, **k):
        return lcd_sampler.RegionSample(region=next(it), bezel=bezel)

    def fake_region(path, *a, **k):
        return lcd_sampler.RegionSample(region=next(it), bezel=bezel)

    with mock.patch.object(C, "_wake_and_home", return_value=None), \
            mock.patch.object(C, "_capture", return_value="x.jpg"), \
            mock.patch.object(lcd_sampler, "sample_widget_body", side_effect=fake_widget_body), \
            mock.patch.object(lcd_sampler, "sample_region", side_effect=fake_region):
        return case(board.ctx(srv))


def _restored(board):
    last = board.posts[-1]
    return (last["brightness"] == "80" and last["timeout"] == "5"
            and last["keep_on_while_firing"] == "1" and last["display_on_error"] == "0")


class Lcd05Test(unittest.TestCase):
    def test_registered(self):
        self.assertIs(get_case("LCD-05").judge, C._case_lcd05)
        self.assertIs(get_case("LCD-06").judge, C._case_lcd06)
        self.assertIs(get_case("LCD-13").judge, C._case_lcd13)

    def test_pass_on_big_drop(self):
        b = FakeBoard()
        r = _run(C._case_lcd05, b, FakeSrv(), [(100, 100, 100), (60, 60, 60)])
        self.assertEqual(r.verdict, Verdict.PASS)
        self.assertTrue(_restored(b))

    def test_fail_on_small_drop(self):
        b = FakeBoard()
        r = _run(C._case_lcd05, b, FakeSrv(), [(100, 100, 100), (90, 90, 90)])
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(_restored(b))

    def test_inert_is_inconclusive(self):
        b = FakeBoard(inert=True)
        r = _run(C._case_lcd05, b, FakeSrv(), [])
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)

    def test_restore_when_judge_raises(self):
        b = FakeBoard()
        with mock.patch.object(C, "_judge_lcd05", side_effect=RuntimeError("boom")):
            with self.assertRaises(RuntimeError):
                _run(C._case_lcd05, b, FakeSrv(), [(100, 100, 100), (60, 60, 60)])
        self.assertTrue(_restored(b))


class Lcd06Test(unittest.TestCase):
    def test_pass_dark_then_wake(self):
        b = FakeBoard()
        r = _run(C._case_lcd06, b, FakeSrv(), [(2, 2, 2)])
        self.assertEqual(r.verdict, Verdict.PASS)
        self.assertEqual(b.posts[0]["timeout"], "0")
        self.assertTrue(_restored(b))

    def test_fail_panel_still_lit(self):
        b = FakeBoard()
        r = _run(C._case_lcd06, b, FakeSrv(), [(120, 130, 140)])
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(_restored(b))

    def test_fail_no_wake(self):
        b = FakeBoard()
        r = _run(C._case_lcd06, b, FakeSrv(FakeTouch(wakes=False)), [(2, 2, 2)])
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_restore_when_judge_raises(self):
        b = FakeBoard()
        with mock.patch.object(C, "_judge_lcd06", side_effect=RuntimeError("boom")):
            with self.assertRaises(RuntimeError):
                _run(C._case_lcd06, b, FakeSrv(), [(2, 2, 2)])
        self.assertTrue(_restored(b))


class Lcd13JudgeTest(unittest.TestCase):
    def test_pass(self):
        self.assertEqual(C._judge_lcd13(3, 3, True).verdict, Verdict.PASS)

    def test_fail_wrong_page_count(self):
        self.assertEqual(C._judge_lcd13(3, 2, True).verdict, Verdict.FAIL)

    def test_fail_no_next(self):
        self.assertEqual(C._judge_lcd13(2, 1, False).verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
