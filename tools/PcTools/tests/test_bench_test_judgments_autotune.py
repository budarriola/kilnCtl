#!/usr/bin/env python3
"""Unit tests for the AT-*/HP-03/HP-07 pure judge functions added in Wave 3
part A -- see test_bench_test_judgments.py for the smoke-suite judges this
mirrors in style. No board access; every input is synthetic.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_judgments_autotune.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


class JudgeAutotuneFitTest(unittest.TestCase):
    def _ok_kwargs(self, **overrides):
        kwargs = dict(
            method="step", model_valid=True, baseline_c=24.0, ambient_ref=24.0,
            k_gain_c_per_duty=38.0, tau_s=265.0, max_temp_c=45.0, tripped=False,
        )
        kwargs.update(overrides)
        return kwargs

    def test_passes_on_a_good_fit(self):
        result = J.judge_autotune_fit(**self._ok_kwargs())
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_fails_on_a_trip_during_the_run(self):
        result = J.judge_autotune_fit(**self._ok_kwargs(tripped=True))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("trip", result.reason)

    def test_fails_at_the_max_temp_limit(self):
        result = J.judge_autotune_fit(**self._ok_kwargs(max_temp_c=70.0))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("70", result.reason)

    def test_fails_when_model_invalid(self):
        result = J.judge_autotune_fit(**self._ok_kwargs(model_valid=False))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("model_valid", result.reason)

    def test_fails_when_baseline_not_rested(self):
        result = J.judge_autotune_fit(**self._ok_kwargs(baseline_c=30.0, ambient_ref=24.0))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("rested reference", result.reason)

    def test_fails_when_k_gain_out_of_tolerance(self):
        result = J.judge_autotune_fit(**self._ok_kwargs(k_gain_c_per_duty=10.0))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("fitted K", result.reason)

    def test_fails_when_tau_out_of_tolerance(self):
        result = J.judge_autotune_fit(**self._ok_kwargs(tau_s=10.0))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("fitted tau", result.reason)

    def test_relay_insufficient_amplitude_is_inconclusive(self):
        result = J.judge_autotune_fit(**self._ok_kwargs(
            method="relay", relay_valid=False, relay_amplitude_c=0.5,
        ))
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_relay_invalid_for_other_reason_fails(self):
        result = J.judge_autotune_fit(**self._ok_kwargs(
            method="relay", relay_valid=False, relay_amplitude_c=5.0,
        ))
        self.assertEqual(result.verdict, Verdict.FAIL)


class JudgeAutotuneAbortImmediateTest(unittest.TestCase):
    def test_passes_when_idle_quickly_with_relays_off(self):
        result = J.judge_autotune_abort_immediate(
            state_name="idle", duties=[0.0, 0.0], relays_off=True, elapsed_since_abort_s=1.0,
        )
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_fails_on_timeout(self):
        result = J.judge_autotune_abort_immediate(
            state_name="idle", duties=[0.0], relays_off=True, elapsed_since_abort_s=10.0, timeout_s=5.0,
        )
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fails_when_not_idle(self):
        result = J.judge_autotune_abort_immediate(
            state_name="running", duties=[0.0], relays_off=True, elapsed_since_abort_s=1.0,
        )
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fails_on_nonzero_duty(self):
        result = J.judge_autotune_abort_immediate(
            state_name="idle", duties=[0.3], relays_off=True, elapsed_since_abort_s=1.0,
        )
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_inconclusive_when_relay_state_unreadable(self):
        result = J.judge_autotune_abort_immediate(
            state_name="idle", duties=[0.0], relays_off=None, elapsed_since_abort_s=1.0,
        )
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_fails_when_relay_still_on(self):
        result = J.judge_autotune_abort_immediate(
            state_name="idle", duties=[0.0], relays_off=False, elapsed_since_abort_s=1.0,
        )
        self.assertEqual(result.verdict, Verdict.FAIL)


class JudgeAutotuneAcceptGuardedTest(unittest.TestCase):
    def test_passes_when_refused_and_gains_unchanged(self):
        result = J.judge_autotune_accept_guarded(
            accept_ok=False, accept_reason="unsettled", gains_before=(1.0, 0.1, 0.0), gains_after=(1.0, 0.1, 0.0),
        )
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_fails_when_accept_succeeds(self):
        result = J.judge_autotune_accept_guarded(
            accept_ok=True, accept_reason="", gains_before=(1.0, 0.1, 0.0), gains_after=(1.0, 0.1, 0.0),
        )
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fails_when_gains_changed_despite_refusal(self):
        result = J.judge_autotune_accept_guarded(
            accept_ok=False, accept_reason="unsettled", gains_before=(1.0, 0.1, 0.0), gains_after=(2.0, 0.1, 0.0),
        )
        self.assertEqual(result.verdict, Verdict.FAIL)


class JudgeAutotuneMatrixTest(unittest.TestCase):
    def test_passes_on_a_well_formed_matrix(self):
        matrix = [[0, 0.1, 0.05], [0.1, 0, 0.1], [0.05, 0.1, 0]]
        result = J.judge_autotune_matrix(matrix, zone_row=0)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_fails_on_wrong_shape(self):
        result = J.judge_autotune_matrix([[0, 0.1], [0.1, 0]], zone_row=0)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fails_on_non_numeric_row(self):
        matrix = [[0, None, 0.05], [0.1, 0, 0.1], [0.05, 0.1, 0]]
        result = J.judge_autotune_matrix(matrix, zone_row=0)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fails_on_bool_disguised_as_number(self):
        matrix = [[0, True, 0.05], [0.1, 0, 0.1], [0.05, 0.1, 0]]
        result = J.judge_autotune_matrix(matrix, zone_row=0)
        self.assertEqual(result.verdict, Verdict.FAIL)


class JudgeOnOffZoneCyclingTest(unittest.TestCase):
    def test_passes_with_toggles_and_in_band_temps(self):
        result = J.judge_on_off_zone_cycling(
            relay_states=[True, False, True, False], temps_c=[34.0, 35.0, 33.5, 34.5],
            target_c=34.0, hyst_c=2.0,
        )
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_inconclusive_with_too_few_samples(self):
        result = J.judge_on_off_zone_cycling(
            relay_states=[True], temps_c=[34.0], target_c=34.0, hyst_c=2.0,
        )
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_fails_when_relay_never_toggles(self):
        result = J.judge_on_off_zone_cycling(
            relay_states=[True, True, True], temps_c=[34.0, 34.0, 34.0], target_c=34.0, hyst_c=2.0,
        )
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("toggled", result.reason)

    def test_fails_when_temperature_strays_out_of_band(self):
        result = J.judge_on_off_zone_cycling(
            relay_states=[True, False, True], temps_c=[34.0, 60.0, 34.0], target_c=34.0, hyst_c=2.0,
        )
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("strayed", result.reason)

    def test_passes_when_ramp_up_from_ambient_precedes_in_band_cycling(self):
        """HP-03 regression: the run starts at ambient, well outside the
        band, and only converges to cycle in-band later. Judging every
        sample from profile start (the pre-fix behaviour) failed this
        expected ramp-up; only samples from the first in-band arrival
        onward are judged for straying."""
        result = J.judge_on_off_zone_cycling(
            relay_states=[True, True, True, False, True, False],
            temps_c=[10.0, 20.0, 30.0, 35.0, 33.5, 34.5],
            target_c=34.0, hyst_c=2.0,
        )
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_fails_not_inconclusive_when_never_reaches_band(self):
        """A run that never arrives in band must still FAIL -- not pass
        vacuously because zero post-arrival samples were judged."""
        result = J.judge_on_off_zone_cycling(
            relay_states=[True, False, True, False],
            temps_c=[10.0, 15.0, 20.0, 25.0],
            target_c=34.0, hyst_c=2.0,
        )
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("never reached band", result.reason)


class JudgeFaultedRunTest(unittest.TestCase):
    def test_passes_on_a_clean_provoked_fault_and_clear(self):
        result = J.judge_faulted_run(
            state_name="faulted", faulted=True, fault_guard=5, ack_ok=True, post_ack_state="idle",
        )
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_fails_when_never_faulted(self):
        result = J.judge_faulted_run(
            state_name="running", faulted=False, fault_guard=None, ack_ok=True, post_ack_state="running",
        )
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fails_on_the_wrong_guard(self):
        result = J.judge_faulted_run(
            state_name="faulted", faulted=True, fault_guard=3, ack_ok=True, post_ack_state="idle",
        )
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("fault_guard", result.reason)

    def test_fails_when_ack_refused(self):
        result = J.judge_faulted_run(
            state_name="faulted", faulted=True, fault_guard=5, ack_ok=False, post_ack_state="faulted",
        )
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fails_when_still_faulted_after_ack(self):
        result = J.judge_faulted_run(
            state_name="faulted", faulted=True, fault_guard=5, ack_ok=True, post_ack_state="faulted",
        )
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("still", result.reason)


if __name__ == "__main__":
    unittest.main()
