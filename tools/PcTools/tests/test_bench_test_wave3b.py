#!/usr/bin/env python3
"""Unit tests for Wave 3 part B (docs/BENCH_TEST_SYSTEM_PLAN.md §8):
FL-10/FL-11 (opt-in JTAG flash round trips), WEB-SEC-05 (lockout, must run
last), WEB-WIFI-06, and the `--attended` operator-prompt mechanism used by
SP-08/SP-09/WEB-WIFI-06 (operator.py).

Per feedback_negative_test_every_check, every judgment function here gets
at least one test that feeds it a bad/regressed input and confirms it
reports something other than PASS -- and the attended-SKIP path gets its
own dedicated negative test (a case that forgets to check --attended would
otherwise hang or FAIL against a real board instead of SKIPping).

Run with: python -m pytest tools/PcTools/tests/test_bench_test_wave3b.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_fl as CFL  # noqa: E402
from kilnctrl.bench_test import cases_safety as CS  # noqa: E402
from kilnctrl.bench_test import cases_smoke as CSM  # noqa: E402
from kilnctrl.bench_test import cases_web as CW  # noqa: E402
from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test import operator as OP  # noqa: E402
from kilnctrl.bench_test.registry import Verdict, get_case  # noqa: E402


# ---------------------------------------------------------------------------
# operator.py -- the --attended mechanism itself.
# ---------------------------------------------------------------------------

class RequireAttendedTest(unittest.TestCase):
    def test_unattended_skips_with_reason(self):
        result = OP.require_attended({})
        self.assertIsNotNone(result)
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("--attended", result.reason)

    def test_attended_false_still_skips(self):
        result = OP.require_attended({"attended": False})
        self.assertIsNotNone(result)
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_attended_true_returns_none(self):
        self.assertIsNone(OP.require_attended({"attended": True}))

    def test_custom_reason_is_used(self):
        result = OP.require_attended({}, reason="requires --attended (custom)")
        self.assertEqual(result.reason, "requires --attended (custom)")


class AskOperatorTest(unittest.TestCase):
    def test_injected_prompt_fn_is_used(self):
        calls = []

        def fake_prompt(question, timeout_s):
            calls.append((question, timeout_s))
            return True

        ctx = {"operator_prompt_fn": fake_prompt}
        result = OP.ask_operator(ctx, "did it work?", timeout_s=30.0)
        self.assertTrue(result)
        self.assertEqual(calls, [("did it work?", 30.0)])

    def test_injected_prompt_fn_can_answer_no(self):
        ctx = {"operator_prompt_fn": lambda q, t: False}
        self.assertFalse(OP.ask_operator(ctx, "q"))

    def test_injected_prompt_fn_can_time_out(self):
        ctx = {"operator_prompt_fn": lambda q, t: None}
        self.assertIsNone(OP.ask_operator(ctx, "q"))

    def test_default_prompt_parses_yes(self):
        import io
        ctx = {}
        old_stdin = sys.stdin
        sys.stdin = io.StringIO("y\n")
        try:
            self.assertTrue(OP.ask_operator(ctx, "q", timeout_s=1.0))
        finally:
            sys.stdin = old_stdin

    def test_default_prompt_unparseable_is_none(self):
        import io
        ctx = {}
        old_stdin = sys.stdin
        sys.stdin = io.StringIO("banana\n")
        try:
            self.assertIsNone(OP.ask_operator(ctx, "q", timeout_s=1.0))
        finally:
            sys.stdin = old_stdin


# ---------------------------------------------------------------------------
# judgments.py -- new pure functions.
# ---------------------------------------------------------------------------

class SafetyTripMaskTest(unittest.TestCase):
    def test_mask_for_zero_is_zero(self):
        self.assertEqual(J.safety_trip_mask_for_reason(0), 0)

    def test_mask_for_s6a(self):
        self.assertEqual(J.safety_trip_mask_for_reason(6), 0x20)

    def test_mask_for_s6b(self):
        self.assertEqual(J.safety_trip_mask_for_reason(7), 0x40)

    def test_mask_for_estop_s7(self):
        self.assertEqual(J.safety_trip_mask_for_reason(8), 0x80)


class JudgeOperatorTripTest(unittest.TestCase):
    def test_matching_reason_and_cleared_passes(self):
        result = J.judge_operator_trip(8, 8, True)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_no_trip_reason_fails(self):
        result = J.judge_operator_trip(None, 8, True)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_wrong_reason_fails(self):
        """Negative test: a DIFFERENT guard trips (e.g. S6b instead of the
        expected S7 E-stop) -- must FAIL, never PASS just because
        *something* tripped."""
        result = J.judge_operator_trip(7, 8, True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("does not match", result.reason)

    def test_correct_reason_but_never_clears_fails(self):
        result = J.judge_operator_trip(8, 8, False)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("did not clear", result.reason)

    def test_correct_reason_unknown_clear_is_inconclusive(self):
        result = J.judge_operator_trip(8, 8, None)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


class JudgeWifiModeReturnedHomeTest(unittest.TestCase):
    def test_ap_seen_and_returned_home_passes(self):
        result = J.judge_wifi_mode_returned_home("ap", "home", True)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_board_reported_ap_even_without_operator_confirm_passes(self):
        result = J.judge_wifi_mode_returned_home("ap", "home", None)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_neither_signal_confirms_ap_fails(self):
        result = J.judge_wifi_mode_returned_home(None, "home", False)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_stuck_in_ap_fails_even_if_operator_confirmed(self):
        """Negative test: the board never came back to home -- a stuck-in-AP
        regression the operator's "yes I saw it" would not itself catch."""
        result = J.judge_wifi_mode_returned_home("ap", "ap", True)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("mode=home", result.reason)


class JudgeFlashRoundTripTest(unittest.TestCase):
    def test_verified_true_matching_partition_passes(self):
        result = J.judge_flash_round_trip(True, "app", "app", None)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_tool_error_fails(self):
        result = J.judge_flash_round_trip(None, None, "app", "error: refused")
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_verify_false_fails(self):
        result = J.judge_flash_round_trip(False, "recovery", "app", None)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_verified_but_wrong_partition_fails(self):
        """Negative test: verified=True must not be trusted blindly if the
        reported running partition disagrees with what was targeted."""
        result = J.judge_flash_round_trip(True, "recovery", "app", None)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_unknown_verification_is_inconclusive(self):
        result = J.judge_flash_round_trip(None, None, "app", None)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)


class JudgeLoginLockoutTest(unittest.TestCase):
    def test_a_429_anywhere_passes(self):
        result = J.judge_login_lockout([401, 401, 401, 429, 429, 429])
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_no_attempts_is_inconclusive(self):
        result = J.judge_login_lockout([])
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_all_401_never_locking_fails(self):
        """Negative test: lockout stopped engaging -- must FAIL, not PASS
        just because nothing crashed."""
        result = J.judge_login_lockout([401, 401, 401, 401, 401, 401])
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("no 429", result.reason)

    def test_a_successful_login_fails(self):
        result = J.judge_login_lockout([401, 401, 200, 401, 401, 401])
        self.assertEqual(result.verdict, Verdict.FAIL)


# ---------------------------------------------------------------------------
# cases_safety.py -- SP-08/SP-09.
# ---------------------------------------------------------------------------

class Sp08Sp09AttendedGateTest(unittest.TestCase):
    def test_sp08_skips_unattended(self):
        result = CS._case_sp08({})
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("--attended", result.reason)

    def test_sp09_skips_unattended(self):
        result = CS._case_sp09({})
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("--attended", result.reason)


def _diag_text(trip_reason, trip_mask=None):
    """The REAL shape of SafetyDiag.describe() (devices_safety.py) -- space
    separators, a bracketed reason description, a hex mask. The earlier
    fixture said "trip_reason=8", a string no device ever emits, which is
    the "idealized test input" bug class: it let SP-08/SP-09 read their trip
    off safety_get_status() (which carries no trip_reason at all) and still
    look green."""
    if trip_mask is None:
        trip_mask = (1 << (trip_reason - 1)) if trip_reason else 0
    return (
        f"boot reason: power_on | state armed | trip_reason {trip_reason} [desc] | "
        f"warn_mask 0x0000 | trip_mask 0x{trip_mask:04x} | uptime 1000 ms"
    )


class _FakeSafetySrv:
    """Injectable stand-in for ctx["srv"] -- returns canned diag text and
    records whether safety_clear_trip() was called."""

    def __init__(self, status_sequence):
        self._sequence = list(status_sequence)
        self.clear_called = 0

    def safety_get_diag(self):
        if len(self._sequence) > 1:
            return self._sequence.pop(0)
        return self._sequence[0]

    def safety_clear_trip(self):
        self.clear_called += 1


class _FakeFlashAndSafetySrv:
    """FL-11's shape: `debug_program(peer="pico")` plus a diag cache that
    can stay stale for a few reads after `safety_clear_trip()` (models
    LINK_DIAG_TX_PERIOD_MS, see cases_smoke.wait_for_trip_clear)."""

    def __init__(self, flash_text, diag_before_clear, post_clear_delay_polls=0):
        self._flash_text = flash_text
        self._diag_before_clear = diag_before_clear
        self._post_clear_delay_polls = post_clear_delay_polls
        self._polls_since_clear = None
        self.clear_called = 0

    def debug_program(self, peer=None):
        return self._flash_text

    def safety_get_diag(self):
        if self._polls_since_clear is not None:
            if self._polls_since_clear < self._post_clear_delay_polls:
                self._polls_since_clear += 1
                return self._diag_before_clear
            return _diag_text(0)
        return self._diag_before_clear

    def safety_clear_trip(self):
        self.clear_called += 1
        self._polls_since_clear = 0


class Sp08AttendedFlowTest(unittest.TestCase):
    def test_operator_says_no_fails(self):
        ctx = {"attended": True, "operator_prompt_fn": lambda q, t: False}
        result = CS._case_sp08(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_full_trip_and_clear_passes(self):
        srv = _FakeSafetySrv([_diag_text(8), _diag_text(0)])
        ctx = {"attended": True, "operator_prompt_fn": lambda q, t: True, "srv": srv}
        result = CS._case_sp08(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(srv.clear_called, 1)

    def test_wrong_trip_reason_fails(self):
        """Negative test: SP-08 (E-stop, expects 8) sees S6b (7) instead --
        must FAIL, not PASS on "something tripped"."""
        srv = _FakeSafetySrv([_diag_text(7), _diag_text(0)])
        ctx = {"attended": True, "operator_prompt_fn": lambda q, t: True, "srv": srv}
        result = CS._case_sp08(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_sp09_full_trip_and_clear_passes(self):
        srv = _FakeSafetySrv([_diag_text(7), _diag_text(0)])
        ctx = {"attended": True, "operator_prompt_fn": lambda q, t: True, "srv": srv}
        result = CS._case_sp09(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)


class Sp08Sp09RegistryWiringTest(unittest.TestCase):
    def test_judges_wired(self):
        self.assertIs(get_case("SP-08").judge, CS._case_sp08)
        self.assertIs(get_case("SP-09").judge, CS._case_sp09)

    def test_operator_only_flagged(self):
        self.assertTrue(get_case("SP-08").operator_only)
        self.assertTrue(get_case("SP-09").operator_only)


# ---------------------------------------------------------------------------
# cases_web.py -- WEB-WIFI-06 and WEB-SEC-05.
# ---------------------------------------------------------------------------

class WebWifi06Test(unittest.TestCase):
    def test_skips_unattended(self):
        result = CW._case_web_wifi06({})
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_not_run_without_host(self):
        result = CW._case_web_wifi06({"attended": True, "operator_prompt_fn": lambda q, t: True})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_registry_operator_only(self):
        self.assertTrue(get_case("WEB-WIFI-06").operator_only)
        self.assertIs(get_case("WEB-WIFI-06").judge, CW._case_web_wifi06)


class WebSec05Test(unittest.TestCase):
    def test_not_run_without_host(self):
        result = CW._case_web_sec05({})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_skips_without_username(self):
        old = os.environ.pop("KILNCTL_WEB_USERNAME", None)
        try:
            result = CW._case_web_sec05({"host": "10.0.0.5"})
            self.assertEqual(result.verdict, Verdict.SKIP)
        finally:
            if old is not None:
                os.environ["KILNCTL_WEB_USERNAME"] = old

    def test_registry_wired_last(self):
        self.assertIs(get_case("WEB-SEC-05").judge, CW._case_web_sec05)


# ---------------------------------------------------------------------------
# cases_fl.py -- FL-10/FL-11 opt-in gates.
# ---------------------------------------------------------------------------

class Fl10Fl11OptInTest(unittest.TestCase):
    def test_fl10_skips_without_allow_flash(self):
        result = CFL._case_fl10({})
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("allow_flash", result.reason)

    def test_fl10_skips_without_ap_password_even_with_allow_flash(self):
        result = CFL._case_fl10({"allow_flash": True})
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("ap_password", result.reason)

    def test_fl11_skips_without_allow_flash(self):
        result = CFL._case_fl11({})
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn("allow_flash", result.reason)

    def test_fl10_never_gated_by_attended(self):
        """Owner decision 10: FL-10/FL-11 must be runnable unattended once
        opted in -- attended=False alone must never turn this into a SKIP
        for an attended-mechanism reason (only the allow_flash gate above
        applies)."""
        result = CFL._case_fl10({"allow_flash": True, "ap_password": "x", "attended": False,
                                  "srv": _FakeFlashSrv(error="error: no board")})
        self.assertNotIn("--attended", result.reason)

    def test_registry_wired(self):
        self.assertIs(get_case("FL-10").judge, CFL._case_fl10)
        self.assertIs(get_case("FL-11").judge, CFL._case_fl11)


class _FakeFlashSrv:
    def __init__(self, text=None, error=None):
        self._text = text
        self._error = error

    def flash_firmware(self, **kwargs):
        if self._error:
            return self._error
        return self._text


class Fl10FlashOutcomeTest(unittest.TestCase):
    def test_success_text_passes(self):
        srv = _FakeFlashSrv(text="flashed and verified OK ..., and post-flash verification confirmed the board is running 'app' with the matching build")
        ctx = {"allow_flash": True, "ap_password": "pw", "srv": srv}
        result = CFL._case_fl10(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_error_text_fails(self):
        """Negative test: a refusal from the flashing tool itself must never
        be treated as anything but FAIL (plan §6 rule 3)."""
        srv = _FakeFlashSrv(error="error: sensitive dirty files detected: zones_config_json.h")
        ctx = {"allow_flash": True, "ap_password": "pw", "srv": srv}
        result = CFL._case_fl10(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_warning_text_fails(self):
        srv = _FakeFlashSrv(text="flashed OK\nWARNING: post-flash verification could not confirm the board")
        ctx = {"allow_flash": True, "ap_password": "pw", "srv": srv}
        result = CFL._case_fl10(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


class TripMaskRuleFiveTest(unittest.TestCase):
    """Plan section 6 rule 5: safety_clear_trip() only on an exactly-matched
    reason AND mask."""

    def test_extra_guard_in_the_mask_fails_and_never_clears(self):
        # reason 8 (E-stop) but the mask also carries bit 5 (S6a) -- a
        # second guard is latched, so this must FAIL and must NOT clear.
        srv = _FakeSafetySrv([_diag_text(8, trip_mask=0x0080 | 0x0020)])
        ctx = {"attended": True, "operator_prompt_fn": lambda q, t: True, "srv": srv}
        result = CS._case_sp08(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("trip_mask", result.reason)
        self.assertEqual(srv.clear_called, 0)

    def test_a_missing_mask_is_not_a_match(self):
        srv = _FakeSafetySrv(["boot reason: power_on | trip_reason 8 [desc] | uptime 1 ms"])
        ctx = {"attended": True, "operator_prompt_fn": lambda q, t: True, "srv": srv}
        result = CS._case_sp08(ctx)
        self.assertEqual(srv.clear_called, 0)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_judge_passes_on_an_exact_mask(self):
        result = J.judge_operator_trip(8, 8, True, trip_mask=1 << 7)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_fl11_expects_s6a_mask_0x0020(self):
        self.assertEqual(J.safety_trip_mask_for_reason(CFL._FL11_EXPECTED_TRIP_REASON), 0x0020)


class Fl11TripClearPollTest(unittest.TestCase):
    """FL-11's clear-then-read-back must poll rather than trust a single
    immediate read, same reasoning as HP-07 (cases_heat.py) --
    cases_smoke.wait_for_trip_clear is the shared helper both use."""

    def _ctx(self, srv):
        clock = {"t": 0.0}
        return {
            "allow_flash": True, "srv": srv,
            "_now": lambda: clock["t"],
            "_sleep": lambda s: clock.__setitem__("t", clock["t"] + s),
        }

    def test_clear_landing_on_second_poll_still_passes(self):
        srv = _FakeFlashAndSafetySrv(
            flash_text="flashed OK", diag_before_clear=_diag_text(6), post_clear_delay_polls=1,
        )
        result = CFL._case_fl11(self._ctx(srv))
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)
        self.assertEqual(srv.clear_called, 1)
        self.assertGreater(result.observed.get("trip_clear_elapsed_s"), 0.0)

    def test_clear_landing_on_third_poll_still_passes(self):
        srv = _FakeFlashAndSafetySrv(
            flash_text="flashed OK", diag_before_clear=_diag_text(6), post_clear_delay_polls=2,
        )
        result = CFL._case_fl11(self._ctx(srv))
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_never_clears_fails_with_last_reason_and_elapsed(self):
        srv = _FakeFlashAndSafetySrv(
            flash_text="flashed OK", diag_before_clear=_diag_text(6), post_clear_delay_polls=1000,
        )
        result = CFL._case_fl11(self._ctx(srv))
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertEqual(result.observed.get("trip_reason"), 6)
        self.assertGreaterEqual(result.observed.get("trip_clear_elapsed_s"), CSM._TRIP_CLEAR_POLL_TIMEOUT_S)


class AlwaysLastAcrossEverySuiteTest(unittest.TestCase):
    def test_web_sec_05_is_last_in_every_suite_containing_it(self):
        from kilnctrl.bench_test import registry as R
        containing = [n for n, ids in R.SUITES.items() if "WEB-SEC-05" in ids]
        self.assertIn("web", containing)
        self.assertIn("full", containing)
        for name in containing:
            self.assertEqual(R.SUITES[name][-1], "WEB-SEC-05", name)


if __name__ == "__main__":
    unittest.main()
