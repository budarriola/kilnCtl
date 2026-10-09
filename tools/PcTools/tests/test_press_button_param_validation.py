#!/usr/bin/env python3
"""Unit tests for press_button()'s param type validation (mcp_server_actions.py).

History: Action.params was descriptive-only -- press_button splatted the
caller's dict straight into action.run(**params) uncoerced. A gated action's
own lambda reads `confirm is True` (or, before that fix, `not confirm`), so a
JSON/RPC client sending confirm="false" (a non-empty string, therefore truthy
in Python) -- or "true", or the integer 1 -- slipped straight past the gate
and reached the device. press_button must now reject any bool/int-declared
value that isn't exactly that type (rejecting bool for an int-typed param
too, since True/False are int subclasses in Python) before ever calling
action.run.

SAFETY NOTE, load-bearing: this module MUST NEVER call press_button/action.run
for a real gated action (e.g. "System: Factory Reset") with confirm=True.
mcp_server_actions.py's press_button dispatches through
kilnctrl.mcp_server's module-level `_action_ctx`, which is built against
whatever physical link this process's link_hub finds on import -- including a
live bench board over a shared link_hub connection. A prior version of this
file called the real "System: Factory Reset" action with confirm=True to
prove validation "passes through" to run(); that reaches
actions.py's ``_send(ctx, UART_TASK_ID_SYSTEM, devices.system_factory_reset(0))``
for real once ``ctx.info.compatible is True`` (i.e. once *anything* in the
same process has already talked to a real board), which is a genuine NVS
erase on whatever board is attached. Every "validation clears, action.run
would be reached" case below is proven with a temporary fake action
registered under a throwaway name (removed in a finally block) whose `run`
only records that it was called and returns a fixed string -- it never
touches devices.py, UART_TASK_ID_SYSTEM, or the shared link.

Run with: python -m pytest tools/PcTools/tests/test_press_button_param_validation.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import actions  # noqa: E402
from kilnctrl import mcp_server_actions as ma  # noqa: E402

_FAKE_NAME = "Test: Fake Gated Action (validation harness only)"


class PressButtonParamValidationTest(unittest.TestCase):
    """Exercises real, currently-registered gated actions -- but only with
    inputs that validation must refuse BEFORE action.run is ever reached.
    None of these can dispatch to hardware: an invalid type is caught in
    press_button's validation loop, well before ``action.run(_srv._action_ctx,
    **params)`` is called."""

    def test_confirm_string_false_is_rejected(self):
        """The exact bug: "false" is a truthy Python string -- `if not confirm`
        (and even `confirm is True`) must never see it as anything but wrong."""
        result = ma.press_button("System: Factory Reset", {"scope": 0, "confirm": "false"})
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("confirm", result)
        self.assertIn("boolean", result)

    def test_confirm_string_true_is_rejected(self):
        result = ma.press_button("System: Factory Reset", {"scope": 0, "confirm": "true"})
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("confirm", result)

    def test_confirm_int_one_is_rejected(self):
        result = ma.press_button("System: Factory Reset", {"scope": 0, "confirm": 1})
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("confirm", result)

    def test_confirm_int_zero_is_rejected(self):
        result = ma.press_button("System: Set Watchdog Panic Disabled", {"disabled": True, "confirm": 0})
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("confirm", result)

    def test_scope_bool_is_rejected_for_int_param(self):
        """bool is an int subclass in Python -- an int-typed param (scope)
        must reject True/False too, not silently read them as 1/0. confirm is
        also invalid here (not True) but scope is checked/reported first in
        params dict iteration order, which is what this test pins down."""
        result = ma.press_button("System: Factory Reset", {"scope": True, "confirm": False})
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("scope", result)
        self.assertIn("integer", result)

    def test_unknown_action_still_reported_plainly(self):
        result = ma.press_button("System: Not A Real Button")
        self.assertTrue(result.startswith("error: unknown action"), result)


class PressButtonReachesRunOnlyViaFakeActionTest(unittest.TestCase):
    """Proves a real bool/int clears validation and reaches action.run --
    against a throwaway fake action, never a real gated one, so this can
    never touch a live board regardless of link_hub/compatibility state."""

    def setUp(self):
        self.calls: list[dict] = []

        def _fake_run(ctx, **kwargs):
            self.calls.append(kwargs)
            return "ok (fake action, no hardware touched)"

        assert _FAKE_NAME not in actions.ACTIONS, "leftover fake action from a prior failed run"
        actions.ACTIONS[_FAKE_NAME] = actions.Action(
            name=_FAKE_NAME,
            description="Test-only stand-in for a gated action; never touches hardware.",
            params={"scope": int, "confirm": bool},
            required=frozenset({"scope", "confirm"}),
            run=_fake_run,
        )

    def tearDown(self):
        actions.ACTIONS.pop(_FAKE_NAME, None)

    def test_real_bool_and_int_pass_validation_and_reach_run(self):
        result = ma.press_button(_FAKE_NAME, {"scope": 0, "confirm": True})
        self.assertEqual(result, "ok (fake action, no hardware touched)")
        self.assertEqual(self.calls, [{"scope": 0, "confirm": True}])

    def test_invalid_confirm_never_reaches_run(self):
        result = ma.press_button(_FAKE_NAME, {"scope": 0, "confirm": "true"})
        self.assertTrue(result.startswith("error:"), result)
        self.assertEqual(self.calls, [], "validation failure must not have called run")


if __name__ == "__main__":
    unittest.main()
