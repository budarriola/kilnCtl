#!/usr/bin/env python3
"""Unit tests for mcp_server_totp.{totp_enroll_status,totp_reset_password}
-- all against mocked totp_http_client/http_auth calls; no real socket, no
live board (WT-A's firmware routes do not exist yet).

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_totp.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import http_auth  # noqa: E402
from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_totp as mst  # noqa: E402
from kilnctrl import totp_http_client as thc  # noqa: E402


class _Base(unittest.TestCase):
    def setUp(self):
        http_auth.clear_sessions()
        self._env_backup = dict(os.environ)
        for var in (http_auth.USERNAME_ENV, mst.TOTP_CODE_ENV, mst.NEW_PASSWORD_ENV,
                    http_auth.PASSWORD_ENV):
            os.environ.pop(var, None)
        self._resolve_patch = unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")
        self._resolve_patch.start()

    def tearDown(self):
        self._resolve_patch.stop()
        os.environ.clear()
        os.environ.update(self._env_backup)
        http_auth.clear_sessions()

    def _set_env(self, username="bench", code="123456", new_password="N3wPassw0rd!"):
        if username is not None:
            os.environ[http_auth.USERNAME_ENV] = username
        if code is not None:
            os.environ[mst.TOTP_CODE_ENV] = code
        if new_password is not None:
            os.environ[mst.NEW_PASSWORD_ENV] = new_password


class EnrollStatusTest(_Base):
    def test_reports_enrolled_and_never_a_secret(self):
        with unittest.mock.patch.object(
                thc, "get_totp_status",
                return_value={"enrolled": True, "sntp_synced": True,
                              "board_time_utc": "2026-09-24T12:00:00Z"}):
            result = mst.totp_enroll_status()
        self.assertIn("enrolled=True", result)
        self.assertIn("sntp_synced=True", result)
        self.assertNotIn("secret", result.lower())

    def test_warns_when_clock_unsynced(self):
        with unittest.mock.patch.object(
                thc, "get_totp_status", return_value={"enrolled": False, "sntp_synced": False}):
            result = mst.totp_enroll_status()
        self.assertIn("WARNING", result)
        self.assertIn("not SNTP-synced", result)

    def test_http_error_is_reported(self):
        with unittest.mock.patch.object(
                thc, "get_totp_status", side_effect=thc.TotpHttpError("boom")):
            result = mst.totp_enroll_status()
        self.assertIn("error", result.lower())


class ResetRefusalTest(_Base):
    def test_refuses_without_confirm_and_sends_no_request(self):
        self._set_env()
        with unittest.mock.patch.object(thc, "post_forgot") as forgot_mock:
            result = mst.totp_reset_password(confirm=False)
        self.assertIn("DRY RUN", result)
        forgot_mock.assert_not_called()

    def test_confirm_not_exactly_true_is_refused(self):
        self._set_env()
        with unittest.mock.patch.object(thc, "post_forgot") as forgot_mock:
            result = mst.totp_reset_password(confirm=1)  # truthy, not True
        self.assertIn("DRY RUN", result)
        forgot_mock.assert_not_called()

    def test_missing_code_env_refuses_naming_only_that_variable(self):
        self._set_env(code=None)
        with unittest.mock.patch.object(thc, "post_forgot") as forgot_mock:
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("refused", result)
        self.assertIn(mst.TOTP_CODE_ENV, result)
        self.assertNotIn(mst.NEW_PASSWORD_ENV, result)
        self.assertNotIn(http_auth.USERNAME_ENV, result)
        forgot_mock.assert_not_called()

    def test_missing_new_password_env_refuses_naming_only_that_variable(self):
        self._set_env(new_password=None)
        with unittest.mock.patch.object(thc, "post_forgot") as forgot_mock:
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("refused", result)
        self.assertIn(mst.NEW_PASSWORD_ENV, result)
        self.assertNotIn(mst.TOTP_CODE_ENV, result)
        forgot_mock.assert_not_called()

    def test_missing_username_env_refuses_naming_only_that_variable(self):
        self._set_env(username=None)
        with unittest.mock.patch.object(thc, "post_forgot") as forgot_mock:
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("refused", result)
        self.assertIn(http_auth.USERNAME_ENV, result)
        forgot_mock.assert_not_called()

    def test_both_missing_names_both(self):
        self._set_env(code=None, new_password=None)
        result = mst.totp_reset_password(confirm=True)
        self.assertIn(mst.TOTP_CODE_ENV, result)
        self.assertIn(mst.NEW_PASSWORD_ENV, result)


class ResetSuccessPathTest(_Base):
    def test_full_success_reports_ok_and_never_echoes_secrets(self):
        self._set_env(code="654321", new_password="Sup3rSecretPW!")
        with unittest.mock.patch.object(thc, "post_forgot",
                                         return_value=(202, {"reset_token": "realtok"})) as forgot_mock, \
             unittest.mock.patch.object(thc, "post_reset",
                                         return_value=(200, {"ok": True})) as reset_mock, \
             unittest.mock.patch.object(http_auth, "login", return_value="cookie123") as login_mock:
            result = mst.totp_reset_password(confirm=True)
        forgot_mock.assert_called_once_with("10.0.0.5", "bench", "654321")
        reset_mock.assert_called_once_with("10.0.0.5", "bench", "realtok", "Sup3rSecretPW!")
        login_mock.assert_called_once()
        self.assertIn("ok - password reset via totp and verified by login", result.lower())
        self.assertNotIn("654321", result)
        self.assertNotIn("Sup3rSecretPW!", result)
        self.assertNotIn("realtok", result)

    def test_login_verification_failure_fails_loud(self):
        """A 200 from /api/auth/reset must not be trusted alone -- same
        write-lies discipline as estop_verify()/crash_report_ack()."""
        self._set_env()
        with unittest.mock.patch.object(thc, "post_forgot",
                                         return_value=(202, {"reset_token": "realtok"})), \
             unittest.mock.patch.object(thc, "post_reset", return_value=(200, {"ok": True})), \
             unittest.mock.patch.object(http_auth, "login",
                                         side_effect=http_auth.HttpAuthError("401")):
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok - password reset", result)

    def test_forgot_rate_limited_stops_before_reset(self):
        self._set_env()
        with unittest.mock.patch.object(thc, "post_forgot", return_value=(429, {"ok": False})), \
             unittest.mock.patch.object(thc, "post_reset") as reset_mock:
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("failed", result.lower())
        self.assertIn("429", result)
        reset_mock.assert_not_called()

    def test_forgot_unsynced_clock_503_is_named_and_stops_before_reset(self):
        self._set_env()
        with unittest.mock.patch.object(thc, "post_forgot", return_value=(503, {})),              unittest.mock.patch.object(thc, "post_reset") as reset_mock:
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("503", result)
        self.assertIn("not SNTP-synced", result)
        reset_mock.assert_not_called()

    def test_reset_unsynced_clock_503_is_named(self):
        self._set_env()
        with unittest.mock.patch.object(thc, "post_forgot",
                                         return_value=(202, {"reset_token": "realtok"})),              unittest.mock.patch.object(thc, "post_reset", return_value=(503, {})),              unittest.mock.patch.object(http_auth, "login") as login_mock:
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("not SNTP-synced", result)
        login_mock.assert_not_called()

    def test_forgot_missing_reset_token_stops_before_reset(self):
        self._set_env()
        with unittest.mock.patch.object(thc, "post_forgot", return_value=(202, {})), \
             unittest.mock.patch.object(thc, "post_reset") as reset_mock:
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("failed", result.lower())
        reset_mock.assert_not_called()

    def test_reset_generic_400_is_reported_without_details(self):
        self._set_env()
        with unittest.mock.patch.object(thc, "post_forgot",
                                         return_value=(202, {"reset_token": "realtok"})), \
             unittest.mock.patch.object(thc, "post_reset", return_value=(400, {"ok": False})), \
             unittest.mock.patch.object(http_auth, "login") as login_mock:
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("failed", result.lower())
        self.assertIn("400", result)
        login_mock.assert_not_called()

    def test_forgot_transport_error_is_reported(self):
        self._set_env()
        with unittest.mock.patch.object(thc, "post_forgot",
                                         side_effect=thc.TotpHttpError("unreachable")):
            result = mst.totp_reset_password(confirm=True)
        self.assertIn("failed", result.lower())


if __name__ == "__main__":
    unittest.main()
