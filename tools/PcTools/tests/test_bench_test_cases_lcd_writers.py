"""LCD-10/11/12 (writing LCD judges): fail-closed write gate, heat gate,
pure verdict logic, and fake-board happy path / teardown for LCD-11."""
import os
import sys
import unittest
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_lcd as L  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


class _Profiles:
    def __init__(self, ids=(0,)):
        self.ids = set(ids)
        self.calls = []
        self.state = "idle"

    def list_all(self):
        return [SimpleNamespace(id=i, name=f"p{i}") for i in sorted(self.ids)]

    def save(self, sid, name, mask, segs):
        self.calls.append(("save", sid))
        self.ids.add(sid)
        return SimpleNamespace(ok=True, error=None)

    def delete(self, sid):
        self.calls.append(("delete", sid))
        self.ids.discard(sid)

    def stop(self):
        self.calls.append(("stop",))

    def get_exec_status(self):
        return SimpleNamespace(state_name=self.state)


class _Srv:
    def __init__(self, profiles):
        self._profiles = profiles
        self._ui_test = object()
        self._touch = object()


def _ctx(srv, **kw):
    c = {"srv": srv, "suite": "lcd", "_now": lambda: 0.0, "_sleep": lambda s: None}
    c.update(kw)
    return c


class Gate(unittest.TestCase):
    def test_no_suite_skips_without_writes(self):
        for cid in ("LCD-10", "LCD-11", "LCD-12"):
            p = _Profiles()
            r = R.get_case(cid).judge({"srv": _Srv(p)})
            self.assertEqual(r.verdict, Verdict.SKIP, cid)
            self.assertIn("refusing to write", r.reason)
            self.assertEqual(p.calls, [])

    def test_read_only_suite_skips(self):
        for cid in ("LCD-10", "LCD-11", "LCD-12"):
            p = _Profiles()
            r = R.get_case(cid).judge({"srv": _Srv(p), "suite": "smoke"})
            self.assertEqual(r.verdict, Verdict.SKIP, cid)
            self.assertIn("is not a mutating suite", r.reason)
            self.assertEqual(p.calls, [])

    def test_lcd10_not_run_without_allow_heat(self):
        p = _Profiles()
        for heat in (None, False, 1):
            r = R.get_case("LCD-10").judge(_ctx(_Srv(p), allow_heat=heat))
            self.assertEqual(r.verdict, Verdict.NOT_RUN)
        self.assertEqual(p.calls, [])


class Verdicts(unittest.TestCase):
    def test_lcd10(self):
        self.assertEqual(L._judge_lcd10(True, True, True, True, True, True).verdict, Verdict.PASS)
        for i in range(6):
            args = [True] * 6
            args[i] = False
            self.assertEqual(L._judge_lcd10(*args).verdict, Verdict.FAIL, i)

    def test_lcd11(self):
        self.assertEqual(L._judge_lcd11(True, True, True, True).verdict, Verdict.PASS)
        for i in range(4):
            args = [True] * 4
            args[i] = False
            self.assertEqual(L._judge_lcd11(*args).verdict, Verdict.FAIL, i)

    def test_lcd12(self):
        self.assertEqual(L._judge_lcd12(True, True, True, "3 - free", True, True).verdict, Verdict.PASS)
        self.assertEqual(L._judge_lcd12(True, True, True, None, False, False).verdict, Verdict.FAIL)
        self.assertEqual(L._judge_lcd12(True, True, True, "3 - free", True, False).verdict, Verdict.FAIL)


class Lcd11Flow(unittest.TestCase):
    def _run(self, double_tap_deletes=True, single_tap_deletes=False):
        p = _Profiles({0})
        srv = _Srv(p)
        taps = []
        state = {"armed": False}
        row = {"name": "LCD11_TMP", "cy": 100.0, "cx": 10}
        btn = {"name": "Delete", "cx": 300, "cy": 100.0}

        def tap(touch, t):
            taps.append(t)
            sid = max(p.ids - {0}, default=None)
            if single_tap_deletes and sid is not None:
                p.ids.discard(sid)
            elif len(taps) in (1, 3):
                state["armed"] = True
            elif len(taps) == 4 and double_tap_deletes and sid is not None:
                p.ids.discard(sid)

        def targets(ui):
            nm = "Confirm?" if state["armed"] else "Delete"
            return {"targets": [dict(btn, name=nm)]}, False

        with mock.patch.object(L, "_wake_and_home"), \
                mock.patch.object(L, "_lcdwr_open_picker", return_value=None), \
                mock.patch.object(L, "_lcdwr_find_row", return_value=row), \
                mock.patch.object(L, "_list_tap_targets_resolving_busy", side_effect=targets), \
                mock.patch.object(L, "_lcd13_tap", side_effect=tap), \
                mock.patch.object(L, "_dismiss_lcd19_overlay", return_value={}), \
                mock.patch.object(L, "_navigate_home", return_value={}):
            r = L._case_lcd11(_ctx(srv))
        return r, p

    def test_pass_and_restored(self):
        r, p = self._run()
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(p.ids, {0})

    def test_single_tap_delete_fails_and_restored(self):
        r, p = self._run(single_tap_deletes=True)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(p.ids, {0})

    def test_unconfirmed_delete_cleaned_by_teardown(self):
        r, p = self._run(double_tap_deletes=False)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(p.ids, {0})
        self.assertIn(("delete", 1), p.calls)

    def test_never_uses_hidden_slot(self):
        p = _Profiles({0, 1, 2, 3, 4, 5, 6})
        slot, _ = L._lcdwr_free_slot(_Srv(p))
        self.assertEqual(slot, 8)


if __name__ == "__main__":
    unittest.main()
