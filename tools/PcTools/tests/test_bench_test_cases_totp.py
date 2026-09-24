#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_totp -- the TOTP password-reset
suite (TP-R01/R02/R03/M01, docs/TOTP_PASSWORD_RESET_PLAN.md sections 4/6a).
Fake board only: every board call is injected via ctx overrides
(``totp_status_fn``/``totp_forgot_fn``/``totp_reset_fn``/``totp_login_fn``);
nothing here touches urllib, http_auth, or hardware. Per this task's
constraint, no credential value ever appears in these tests -- only fixed,
obviously-fake literals and env vars set to non-secret test strings.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_totp.py -q
"""
from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_totp as C  # noqa: E402
from kilnctrl.bench_test import judgments as J  # noqa: E402
from kilnctrl.bench_test.registry import REGISTRY, Verdict  # noqa: E402
from kilnctrl import totp_http_client as thc  # noqa: E402


# ---------------------------------------------------------------------------
# Registry wiring
# ---------------------------------------------------------------------------

class RegistryWiringTest(unittest.TestCase):
    def test_all_four_ids_registered(self):
        for cid in ("TP-R01", "TP-R02", "TP-R03", "TP-M01"):
            self.assertIn(cid, REGISTRY, f"{cid} missing from REGISTRY")

    def test_totp_suite_contains_all_four(self):
        from kilnctrl.bench_test.registry import SUITES
        self.assertEqual(set(SUITES["totp"]), {"TP-R01", "TP-R02", "TP-R03", "TP-M01"})


# ---------------------------------------------------------------------------
# TP-R01 -- GET /api/auth/totp_status
# ---------------------------------------------------------------------------

class TpR01Test(unittest.TestCase):
    def test_pass_on_boolean_enrolled(self):
        ctx = {"totp_status_fn": lambda: {"enrolled": True}}
        result = C._case_tp_r01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_pass_on_enrolled_false(self):
        ctx = {"totp_status_fn": lambda: {"enrolled": False}}
        result = C._case_tp_r01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_inconclusive_on_404(self):
        def raise_404():
            raise thc.TotpHttpError("not found", status=404)
        ctx = {"totp_status_fn": raise_404}
        result = C._case_tp_r01(ctx)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_fail_on_other_error(self):
        def raise_500():
            raise thc.TotpHttpError("server error", status=500)
        ctx = {"totp_status_fn": raise_500}
        result = C._case_tp_r01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fail_on_missing_enrolled_field(self):
        # Negative test for judge_totp_status: a body with no 'enrolled'
        # boolean must never PASS.
        ctx = {"totp_status_fn": lambda: {"something_else": True}}
        result = C._case_tp_r01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fail_on_non_bool_enrolled(self):
        ctx = {"totp_status_fn": lambda: {"enrolled": "yes"}}
        result = C._case_tp_r01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


# ---------------------------------------------------------------------------
# TP-R02 -- POST /api/auth/forgot with a wrong code (never calls reset)
# ---------------------------------------------------------------------------

class TpR02Test(unittest.TestCase):
    def test_pass_on_202_with_reset_token(self):
        calls = []

        def fake_forgot(username, code):
            calls.append((username, code))
            return 202, {"reset_token": "opaque-token-value"}

        ctx = {"totp_forgot_fn": fake_forgot, "username": "bench"}
        result = C._case_tp_r02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        # Confirm the wrong-code literal was sent, never a real credential.
        self.assertEqual(calls, [("bench", C._TP_R02_WRONG_CODE)])

    def test_pass_on_503_clock_unsynced(self):
        ctx = {"totp_forgot_fn": lambda u, c: (503, {})}
        result = C._case_tp_r02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_pass_on_429_rate_limited(self):
        ctx = {"totp_forgot_fn": lambda u, c: (429, {})}
        result = C._case_tp_r02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_fail_on_202_missing_reset_token(self):
        ctx = {"totp_forgot_fn": lambda u, c: (202, {})}
        result = C._case_tp_r02(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fail_on_unexpected_status(self):
        ctx = {"totp_forgot_fn": lambda u, c: (500, {})}
        result = C._case_tp_r02(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fail_on_transport_error(self):
        def raise_err(u, c):
            raise thc.TotpHttpError("unreachable")
        ctx = {"totp_forgot_fn": raise_err}
        result = C._case_tp_r02(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_never_calls_reset(self):
        # TP-R02 is read-only: it must never touch /api/auth/reset even
        # though it holds a reset_token after a 202.
        def fail_if_called(*a, **kw):
            raise AssertionError("TP-R02 must never call reset")

        ctx = {
            "totp_forgot_fn": lambda u, c: (202, {"reset_token": "tok"}),
            "totp_reset_fn": fail_if_called,
        }
        result = C._case_tp_r02(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)


# ---------------------------------------------------------------------------
# TP-R03 -- OPEN tier: no session required
# ---------------------------------------------------------------------------

class TpR03Test(unittest.TestCase):
    def test_pass_when_neither_route_requires_a_session(self):
        ctx = {
            "totp_forgot_fn": lambda u, c: (202, {"reset_token": "tok"}),
            "totp_reset_fn": lambda u, t, p: (400, {"ok": False}),
        }
        result = C._case_tp_r03(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_fail_when_forgot_requires_a_session(self):
        ctx = {
            "totp_forgot_fn": lambda u, c: (401, {}),
            "totp_reset_fn": lambda u, t, p: (400, {}),
        }
        result = C._case_tp_r03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fail_when_reset_requires_a_session(self):
        ctx = {
            "totp_forgot_fn": lambda u, c: (202, {"reset_token": "tok"}),
            "totp_reset_fn": lambda u, t, p: (403, {}),
        }
        result = C._case_tp_r03(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_never_uses_a_real_token(self):
        # TP-R03 must never complete a real reset -- it always sends the
        # fixed wrong token/password literals, never a value derived from a
        # real forgot response.
        seen = {}

        def fake_reset(username, token, password):
            seen["token"] = token
            seen["password"] = password
            return 400, {}

        ctx = {
            "totp_forgot_fn": lambda u, c: (202, {"reset_token": "a-real-looking-token"}),
            "totp_reset_fn": fake_reset,
        }
        C._case_tp_r03(ctx)
        self.assertEqual(seen["token"], C._TP_R03_WRONG_TOKEN)
        self.assertEqual(seen["password"], C._TP_R03_WRONG_PASSWORD)


# ---------------------------------------------------------------------------
# TP-M01 -- full reset round trip, opt-in via env credentials
# ---------------------------------------------------------------------------

class TpM01Test(unittest.TestCase):
    def setUp(self):
        # Make sure ambient env vars from the real shell never leak into a
        # test that expects them absent, and clean up anything we set.
        self._saved = {
            k: os.environ.get(k) for k in (C.TOTP_CODE_ENV, C.NEW_PASSWORD_ENV, C._USERNAME_ENV)
        }
        for k in self._saved:
            os.environ.pop(k, None)

    def tearDown(self):
        for k, v in self._saved.items():
            if v is None:
                os.environ.pop(k, None)
            else:
                os.environ[k] = v

    def test_skip_when_code_env_missing(self):
        os.environ[C._USERNAME_ENV] = "bench"
        os.environ[C.NEW_PASSWORD_ENV] = "irrelevant-test-value"
        result = C._case_tp_m01({})
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn(C.TOTP_CODE_ENV, result.reason)

    def test_skip_when_new_password_env_missing(self):
        os.environ[C._USERNAME_ENV] = "bench"
        os.environ[C.TOTP_CODE_ENV] = "123456"
        result = C._case_tp_m01({})
        self.assertEqual(result.verdict, Verdict.SKIP)
        self.assertIn(C.NEW_PASSWORD_ENV, result.reason)

    def test_skip_when_username_missing(self):
        os.environ[C.TOTP_CODE_ENV] = "123456"
        os.environ[C.NEW_PASSWORD_ENV] = "irrelevant-test-value"
        result = C._case_tp_m01({})
        self.assertEqual(result.verdict, Verdict.SKIP)

    def test_full_round_trip_pass(self):
        ctx = {
            "totp_code": "123456",
            "totp_new_password": "irrelevant-test-value",
            "username": "bench",
            "totp_forgot_fn": lambda u, c: (202, {"reset_token": "tok"}),
            "totp_reset_fn": lambda u, t, p: (200, {"ok": True}),
            "totp_login_fn": lambda origin, pw: True,
            "host": "10.0.0.5",
        }
        result = C._case_tp_m01(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_fail_when_reset_reports_ok_but_login_fails(self):
        # Negative test for judge_totp_reset_roundtrip: a board-reported
        # success is never trusted alone.
        ctx = {
            "totp_code": "123456",
            "totp_new_password": "irrelevant-test-value",
            "username": "bench",
            "totp_forgot_fn": lambda u, c: (202, {"reset_token": "tok"}),
            "totp_reset_fn": lambda u, t, p: (200, {"ok": True}),
            "totp_login_fn": lambda origin, pw: False,
            "host": "10.0.0.5",
        }
        result = C._case_tp_m01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fail_when_forgot_not_202(self):
        ctx = {
            "totp_code": "123456",
            "totp_new_password": "irrelevant-test-value",
            "username": "bench",
            "totp_forgot_fn": lambda u, c: (500, {}),
            "totp_reset_fn": lambda u, t, p: (200, {"ok": True}),
            "totp_login_fn": lambda origin, pw: True,
        }
        result = C._case_tp_m01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_fail_when_reset_not_200(self):
        ctx = {
            "totp_code": "123456",
            "totp_new_password": "irrelevant-test-value",
            "username": "bench",
            "totp_forgot_fn": lambda u, c: (202, {"reset_token": "tok"}),
            "totp_reset_fn": lambda u, t, p: (400, {"ok": False}),
            "totp_login_fn": lambda origin, pw: True,
        }
        result = C._case_tp_m01(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_never_sends_credential_values_to_result(self):
        # No CaseResult field this case builds may contain the raw code or
        # new-password value anywhere.
        code = "999111"
        new_password = "must-never-appear-in-output"
        ctx = {
            "totp_code": code,
            "totp_new_password": new_password,
            "username": "bench",
            "totp_forgot_fn": lambda u, c: (202, {"reset_token": "tok"}),
            "totp_reset_fn": lambda u, t, p: (200, {"ok": True}),
            "totp_login_fn": lambda origin, pw: True,
        }
        result = C._case_tp_m01(ctx)
        dump = repr(result.observed) + repr(result.reason) + repr(result.evidence)
        self.assertNotIn(code, dump)
        self.assertNotIn(new_password, dump)


# ---------------------------------------------------------------------------
# Pure judge-function tests (no ctx involved) -- direct negative-input
# coverage for judgments.py's four new functions.
# ---------------------------------------------------------------------------

class JudgeFunctionsTest(unittest.TestCase):
    def test_judge_totp_status_fail_on_non_dict(self):
        result = J.judge_totp_status(None)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_judge_totp_forgot_probe_fail_on_none_status(self):
        result = J.judge_totp_forgot_probe(None, None)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_judge_totp_open_tier_fail_on_both_401(self):
        result = J.judge_totp_open_tier(401, 401)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("forgot=401", result.reason)
        self.assertIn("reset=401", result.reason)

    def test_judge_totp_reset_roundtrip_fail_on_no_token(self):
        result = J.judge_totp_reset_roundtrip(202, False, None, False)
        self.assertEqual(result.verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
