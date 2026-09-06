#!/usr/bin/env python3
"""Unit tests for press_button()'s param type validation (mcp_server_actions.py).

History: Action.params was descriptive-only -- press_button splatted the
caller's dict straight into action.run(**params) uncoerced. A gated action's
own lambda reads `confirm is True` (or, before that fix, `not confirm`), so a
JSON/RPC client sending confirm="false" (a non-empty string, therefore truthy
in Python) -- or "true", or the integer 1 -- slipped straight past the gate
and reached the device. press_button must now reject any value that isn't
exactly the declared type (bool for bool, int for int, rejecting bool for an
int-typed param too, since True/False are int subclasses in Python) before
ever calling action.run.

These exercise "System: Factory Reset" and "System: Set Watchdog Panic
Disabled" specifically -- the two confirm-gated actions -- but the validation
being tested lives in press_button/_validate_param, ahead of any device
access, so no real board or link is needed: an invalid type must be refused
before ctx is ever touched.

Run with: python -m pytest tools/PcTools/tests/test_press_button_param_validation.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_actions as ma  # noqa: E402


class PressButtonParamValidationTest(unittest.TestCase):
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
        must reject True/False too, not silently read them as 1/0."""
        result = ma.press_button("System: Factory Reset", {"scope": True, "confirm": True})
        self.assertTrue(result.startswith("error:"), result)
        self.assertIn("scope", result)
        self.assertIn("integer", result)

    def test_real_bool_true_passes_validation(self):
        """A real bool clears validation and reaches action.run -- proven by
        NOT getting the type-validation error text (it may still error for
        an unrelated reason, e.g. not connected to a board)."""
        result = ma.press_button("System: Factory Reset", {"scope": 0, "confirm": True})
        self.assertNotIn("must be a real boolean", result)
        self.assertNotIn("factory reset refused without confirm", result)

    def test_unknown_action_still_reported_plainly(self):
        result = ma.press_button("System: Not A Real Button")
        self.assertTrue(result.startswith("error: unknown action"), result)


if __name__ == "__main__":
    unittest.main()
