#!/usr/bin/env python3
"""Unit tests for kilnctrl.ui_test_runner: script loading/validation and
run_ui_script()'s compact pass/fail reporting, against mocked lcd/web
clients (no real link, no real socket).

Run with: python -m pytest tools/PcTools/tests/test_ui_test_runner.py -q
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import ui_test_runner as runner  # noqa: E402


class _FakeUiTestClient:
    def __init__(self):
        self.clicked = []

    def click_by_name(self, name):
        self.clicked.append(name)
        return {"result": "ok", "cx": 1, "cy": 2}

    def list_tap_targets(self):
        return {"targets": [{"name": "Start", "cx": 1, "cy": 2, "hidden": False}], "truncated": False}

    def get_current_page(self):
        return "home"


class _FakeWebClient:
    def __init__(self):
        self.current_path = "/"
        self.clicked = []

    def goto(self, path):
        self.current_path = path
        return "<html></html>"

    def click(self, target, body=None):
        self.clicked.append((target, body))
        return "ok"

    def find_element(self, target):
        if target == "runBtn":
            return {"tag": "button", "text": "Start", "hidden": False}
        return None


class ListLoadScriptTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self._patch = unittest.mock.patch.object(runner, "ui_scripts_dir", return_value=self._tmp.name)
        self._patch.start()

    def tearDown(self):
        self._patch.stop()
        self._tmp.cleanup()

    def _write(self, name, data):
        with open(os.path.join(self._tmp.name, f"{name}.json"), "w", encoding="utf-8") as f:
            json.dump(data, f)

    def test_list_empty_dir(self):
        self.assertEqual(runner.list_ui_scripts(), [])

    def test_load_valid_script(self):
        self._write("ok_script", {
            "name": "ok_script", "description": "d", "backend": "lcd",
            "steps": [{"action": "click", "target": "Start"}],
        })
        script = runner.load_ui_script("ok_script")
        self.assertEqual(script["backend"], "lcd")

    def test_name_mismatch_raises(self):
        self._write("bad_name", {"name": "other", "backend": "lcd",
                                  "steps": [{"action": "click", "target": "x"}]})
        with self.assertRaises(runner.UiScriptError):
            runner.load_ui_script("bad_name")

    def test_bad_backend_raises(self):
        self._write("bad_backend", {"name": "bad_backend", "backend": "carrier_pigeon",
                                     "steps": [{"action": "click", "target": "x"}]})
        with self.assertRaises(runner.UiScriptError):
            runner.load_ui_script("bad_backend")

    def test_empty_steps_raises(self):
        self._write("no_steps", {"name": "no_steps", "backend": "lcd", "steps": []})
        with self.assertRaises(runner.UiScriptError):
            runner.load_ui_script("no_steps")

    def test_bad_action_raises(self):
        self._write("bad_action", {"name": "bad_action", "backend": "lcd",
                                    "steps": [{"action": "teleport", "target": "x"}]})
        with self.assertRaises(runner.UiScriptError):
            runner.load_ui_script("bad_action")

    def test_path_traversal_name_rejected(self):
        for bad in ("../etc", "a/b", "a\\b", ".."):
            with self.assertRaises(runner.UiScriptError):
                runner.load_ui_script(bad)

    def test_missing_script_raises(self):
        with self.assertRaises(runner.UiScriptError):
            runner.load_ui_script("does_not_exist")

    def test_list_reports_broken_script_without_raising(self):
        self._write("broken", {"name": "wrong_name", "backend": "lcd",
                                "steps": [{"action": "click", "target": "x"}]})
        listed = runner.list_ui_scripts()
        self.assertEqual(len(listed), 1)
        self.assertIn("error:", listed[0]["description"])


class RunUiStepTest(unittest.TestCase):
    def test_lcd_click_ok(self):
        client = _FakeUiTestClient()
        result = runner.run_ui_step("lcd", client, None, {"action": "click", "target": "Start"})
        self.assertTrue(result["ok"])
        self.assertEqual(client.clicked, ["Start"])

    def test_lcd_wait_for_finds_target_immediately(self):
        client = _FakeUiTestClient()
        result = runner.run_ui_step("lcd", client, None,
                                     {"action": "wait_for", "target": "Start", "timeout_ms": 500})
        self.assertTrue(result["ok"])

    def test_lcd_wait_for_times_out(self):
        client = _FakeUiTestClient()
        result = runner.run_ui_step("lcd", client, None,
                                     {"action": "wait_for", "target": "NeverThere", "timeout_ms": 300})
        self.assertFalse(result["ok"])
        self.assertIn("timed out", result["detail"])

    def test_web_assert_text_pass(self):
        client = _FakeWebClient()
        result = runner.run_ui_step("web", None, client,
                                     {"action": "assert_text", "target": "runBtn", "contains": "Start"})
        self.assertTrue(result["ok"])

    def test_web_assert_text_fail(self):
        client = _FakeWebClient()
        result = runner.run_ui_step("web", None, client,
                                     {"action": "assert_text", "target": "runBtn", "contains": "Nope"})
        self.assertFalse(result["ok"])

    def test_web_assert_text_missing_element(self):
        client = _FakeWebClient()
        result = runner.run_ui_step("web", None, client,
                                     {"action": "assert_text", "target": "missing", "contains": "x"})
        self.assertFalse(result["ok"])
        self.assertIn("not found", result["detail"])

    def test_web_goto(self):
        client = _FakeWebClient()
        result = runner.run_ui_step("web", None, client, {"action": "goto", "target": "/"})
        self.assertTrue(result["ok"])

    def test_web_click(self):
        client = _FakeWebClient()
        result = runner.run_ui_step("web", None, client, {"action": "click", "target": "runBtn"})
        self.assertTrue(result["ok"])
        self.assertEqual(client.clicked, [("runBtn", None)])


class RunUiScriptCompactnessTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self._patch = unittest.mock.patch.object(runner, "ui_scripts_dir", return_value=self._tmp.name)
        self._patch.start()

    def tearDown(self):
        self._patch.stop()
        self._tmp.cleanup()

    def _write(self, name, data):
        with open(os.path.join(self._tmp.name, f"{name}.json"), "w", encoding="utf-8") as f:
            json.dump(data, f)

    def test_all_steps_pass(self):
        self._write("pass_script", {
            "name": "pass_script", "backend": "lcd",
            "steps": [{"action": "click", "target": "Start"}, {"action": "assert_text", "target": "home"}],
        })
        result = runner.run_ui_script("pass_script", ui_test_client=_FakeUiTestClient(), web_client=None)
        self.assertTrue(result["ok"])
        self.assertIsNone(result["failed_at"])
        self.assertEqual(len(result["steps"]), 2)
        for step in result["steps"]:
            self.assertTrue(step["ok"])

    def test_first_failure_stops_remaining_steps_without_running_them(self):
        client = _FakeUiTestClient()
        self._write("fail_script", {
            "name": "fail_script", "backend": "lcd",
            "steps": [
                {"action": "click", "target": "Nope"},  # fails: not "ok" -> _FakeUiTestClient always ok though
                {"action": "click", "target": "Start"},
                {"action": "click", "target": "Start"},
            ],
        })

        class _FailFirstClient(_FakeUiTestClient):
            def click_by_name(self, name):
                if name == "Nope":
                    return {"result": "not_found", "cx": 0, "cy": 0}
                return super().click_by_name(name)

        result = runner.run_ui_script("fail_script", ui_test_client=_FailFirstClient(), web_client=None)
        self.assertFalse(result["ok"])
        self.assertEqual(result["failed_at"], 0)
        self.assertFalse(result["steps"][0]["ok"])
        self.assertIsNotNone(result["steps"][0]["detail"])
        # steps after the failure are reported but never actually run
        self.assertFalse(result["steps"][1]["ok"])
        self.assertIsNone(result["steps"][1]["detail"])
        self.assertFalse(result["steps"][2]["ok"])
        self.assertIsNone(result["steps"][2]["detail"])
        self.assertEqual(client.clicked, [])  # the original client passed in was never used past construction

    def test_passing_step_detail_stays_short(self):
        self._write("terse_script", {
            "name": "terse_script", "backend": "lcd",
            "steps": [{"action": "click", "target": "Start"}],
        })
        result = runner.run_ui_script("terse_script", ui_test_client=_FakeUiTestClient(), web_client=None)
        self.assertIsNone(result["steps"][0]["detail"])


if __name__ == "__main__":
    unittest.main()
