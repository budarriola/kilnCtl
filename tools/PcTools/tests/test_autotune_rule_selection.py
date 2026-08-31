#!/usr/bin/env python3
"""Unit tests for kilnctrl.protocol.AUTOTUNE_RULES (the single source of
truth for tuning-rule names the firmware's autotune_rule_t enum defines,
pid_autotune.h) and mcp_server.autotune_start()'s use of it.

DEFECT under test: mcp_server.autotune_start() used to hardcode its own
rule_map = {"tl": ..., "zn": ...} -- a second, narrower copy of the rule set
that rejected "simc" and "cohen-coon" outright ("error: rule must be one of
['tl', 'zn'], got 'simc'") even though pid_autotune.h defines four rules
(AUTOTUNE_RULE_SIMC, AUTOTUNE_RULE_ZIEGLER_NICHOLS, AUTOTUNE_RULE_TYREUS_LUYBEN,
AUTOTUNE_RULE_COHEN_COON) and dashboard_http.c's POST /api/autotune/start
accepts simc/cohen-coon on the step method. This module keys off
protocol.AUTOTUNE_RULES instead.

No real UART/serial connection is used -- _autotune.start() is mocked out.

Run with: python -m pytest tools/PcTools/tests/test_autotune_rule_selection.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl import mcp_server  # noqa: E402
from kilnctrl.protocol import AUTOTUNE_RULES  # noqa: E402

# NOTE: the dict-shape checks that used to live here (test_all_four_firmware_
# rule_names_present, test_relay_rules_are_tl_and_zn_only,
# test_step_rules_are_simc_and_cohen_coon_only) were near-tautologies -- they
# checked AUTOTUNE_RULES against literals in this same file, so they could
# never fail on real drift between AUTOTUNE_RULES and the firmware. That
# constraint now lives in test_autotune_rules_drift_guard.py, which regexes
# pid_autotune.h's actual enum and dashboard_http.c's actual accepted-rule
# strings instead of trusting this file's own copy of "the right answer".


class AutotuneStartRuleValidationTests(unittest.TestCase):
    def _patched_start(self, **kwargs):
        with unittest.mock.patch.object(
                mcp_server._autotune, "start", return_value=(True, "")) as mock_start:
            result = mcp_server.autotune_start(**kwargs)
        return result, mock_start

    def test_relay_tl_accepted_and_wire_byte_is_tl(self):
        """Not just "accepted" -- the EXACT wire byte that reaches
        _autotune.start() must be AUTOTUNE_RULE_TL. An implementation that
        always sends 0 regardless of the requested rule would pass a looser
        assertion here; assert_called_once_with pins the whole call."""
        result, mock_start = self._patched_start(
            zone=0, method="relay", step_duty_or_setpoint_c=200.0,
            relay_d=0.3, relay_h_c=2.0, rule="tl")
        self.assertEqual(result, "ok - autotune started")
        mock_start.assert_called_once_with(
            0, devices.AUTOTUNE_METHOD_RELAY, 200.0, 0.3, 2.0, devices.AUTOTUNE_RULE_TL)

    def test_relay_zn_accepted_and_wire_byte_is_zn(self):
        """THE test that catches "always sends rule_val=0" -- ZN's wire
        byte (1) is different from TL's (0), so a hardcoded-0 implementation
        fails this one specifically even though it would have quietly
        passed the old test_relay_tl_accepted. See
        test_relay_zn_wire_byte_NEGATIVE below for proof."""
        result, mock_start = self._patched_start(
            zone=0, method="relay", step_duty_or_setpoint_c=200.0,
            relay_d=0.3, relay_h_c=2.0, rule="zn")
        self.assertEqual(result, "ok - autotune started")
        mock_start.assert_called_once_with(
            0, devices.AUTOTUNE_METHOD_RELAY, 200.0, 0.3, 2.0, devices.AUTOTUNE_RULE_ZN)

    def test_step_simc_accepted(self):
        """DEFECT 2: "simc" used to be rejected outright for EVERY method,
        even though it is the step path's own real default rule. The wire
        protocol has no rule byte for step at all (firmware always runs
        SIMC there), so only the call succeeding matters here, not which
        placeholder byte rides along -- see devices.AUTOTUNE_RULES'
        wire_byte="implicit" doc comment."""
        result, mock_start = self._patched_start(
            zone=0, method="step", step_duty_or_setpoint_c=0.4, rule="simc")
        self.assertEqual(result, "ok - autotune started")
        mock_start.assert_called_once()

    def test_step_with_no_rule_given_defaults_to_simc_and_succeeds(self):
        """BLOCKING regression: autotune_start(zone, "step", duty) --
        omitting `rule` entirely, the most common real call on this bench --
        used to fail with "error: rule 'tl' is not valid for method 'step'"
        once the default was a bare "tl" applied to every method. The
        default must resolve per-method instead."""
        result, mock_start = self._patched_start(
            zone=0, method="step", step_duty_or_setpoint_c=0.4)
        self.assertEqual(result, "ok - autotune started")
        mock_start.assert_called_once()

    def test_relay_with_no_rule_given_defaults_to_tl_and_wire_byte_is_tl(self):
        """Same per-method default, relay side: omitting `rule` on a relay
        call must still resolve to Tyreus-Luyben (dashboard_http.c's own
        relay-path default) and carry AUTOTUNE_RULE_TL on the wire."""
        result, mock_start = self._patched_start(
            zone=0, method="relay", step_duty_or_setpoint_c=200.0, relay_d=0.3, relay_h_c=2.0)
        self.assertEqual(result, "ok - autotune started")
        mock_start.assert_called_once_with(
            0, devices.AUTOTUNE_METHOD_RELAY, 200.0, 0.3, 2.0, devices.AUTOTUNE_RULE_TL)

    def test_relay_zn_wire_byte_asserts_the_exact_byte_not_just_any_call(self):
        """Sanity check on the assertion technique itself: assert_called_once_with
        against the WRONG expected byte (TL's, 0) must fail even though the
        call happened -- proving this suite's exact-byte assertions really
        do pin the byte and are not silently equivalent to
        assert_called_once(). The corresponding source-level negative test
        (mcp_server.autotune_start() patched to always send byte 0) is run
        manually per this task's stub-out/confirm-FAIL/restore/confirm-PASS
        requirement -- see the task report for that transcript, since
        committing a self-corrupting patch of production code into this
        test file is not an option (kept scope to one file at a time)."""
        result, mock_start = self._patched_start(
            zone=0, method="relay", step_duty_or_setpoint_c=200.0,
            relay_d=0.3, relay_h_c=2.0, rule="zn")
        self.assertEqual(result, "ok - autotune started")
        with self.assertRaises(AssertionError):
            mock_start.assert_called_once_with(
                0, devices.AUTOTUNE_METHOD_RELAY, 200.0, 0.3, 2.0, devices.AUTOTUNE_RULE_TL)

    def test_step_cohen_coon_is_a_known_rule_but_refused_not_silently_run_as_simc(self):
        """"cohen-coon" is a real firmware rule (dashboard_http.c's HTTP
        path accepts it) but the UART_TASK_ID_AUTOTUNE wire protocol this
        tool talks over has no rule byte for the step method at all --
        uart_bridge_ext.c always runs SIMC there regardless of what is
        sent. The tool must refuse this explicitly (loud failure) rather
        than silently starting a SIMC run while claiming Cohen-Coon was
        used -- the same "consumer without producer" trap as defect 1.
        """
        with unittest.mock.patch.object(mcp_server._autotune, "start") as mock_start:
            result = mcp_server.autotune_start(
                zone=0, method="step", step_duty_or_setpoint_c=0.4, rule="cohen-coon")
        self.assertTrue(result.startswith("error:"))
        self.assertIn("cohen-coon", result)
        mock_start.assert_not_called()  # never silently substitutes SIMC

    def test_relay_method_rejects_simc(self):
        result, mock_start = None, None
        with unittest.mock.patch.object(mcp_server._autotune, "start") as mock_start:
            result = mcp_server.autotune_start(
                zone=0, method="relay", step_duty_or_setpoint_c=200.0,
                relay_d=0.3, relay_h_c=2.0, rule="simc")
        self.assertTrue(result.startswith("error:"))
        mock_start.assert_not_called()

    def test_genuinely_invalid_rule_still_rejected(self):
        with unittest.mock.patch.object(mcp_server._autotune, "start") as mock_start:
            result = mcp_server.autotune_start(
                zone=0, method="relay", step_duty_or_setpoint_c=200.0,
                relay_d=0.3, relay_h_c=2.0, rule="not-a-real-rule")
        self.assertTrue(result.startswith("error:"))
        self.assertIn("not-a-real-rule", result)
        mock_start.assert_not_called()

    # ---- NEGATIVE TEST: prove this test file would catch the original bug ----

    def test_step_simc_acceptance_NEGATIVE(self):
        """With autotune_start() reverted to the old two-entry rule_map
        (simulating the pre-fix code), rule="simc" on the step method must
        be REJECTED -- reproducing the exact reported symptom, "error: rule
        must be one of ['tl', 'zn'], got 'simc'"."""
        old_rules = {"tl": AUTOTUNE_RULES["tl"], "zn": AUTOTUNE_RULES["zn"]}
        with unittest.mock.patch.object(mcp_server.devices, "AUTOTUNE_RULES", old_rules):
            with unittest.mock.patch.object(mcp_server._autotune, "start") as mock_start:
                result = mcp_server.autotune_start(
                    zone=0, method="step", step_duty_or_setpoint_c=0.4, rule="simc")
        self.assertTrue(result.startswith("error:"))
        self.assertIn("simc", result)
        mock_start.assert_not_called()


if __name__ == "__main__":
    unittest.main()
