#!/usr/bin/env python3
"""Unit tests for mcp_server_web_auth.web_auth_setup() -- the MCP tool that
bootstraps the board's administrator web credential and (by default) turns
web auth on. All against mocked web_auth_setup_http_client calls; no real
socket, no live board, and no credential value is ever asserted to be
ABSENT from a call -- only asserted absent from every LOGGED/RETURNED
string, per this tool's own no-echo contract.

Run with:
  python -m pytest tools/PcTools/tests/test_mcp_server_web_auth.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import mcp_server_web_auth as msw  # noqa: E402
from kilnctrl import web_auth_setup_http_client as wac  # noqa: E402
from kilnctrl.http_auth import PASSWORD_ENV, USERNAME_ENV  # noqa: E402

_SECRET_PASSWORD = "s3cret-value-42"
_USERNAME = "admin"

_OFF_NO_ADMIN = {
    "web_enabled": False, "lcd_enabled": True,
    "web_timeout_min": 30, "lcd_timeout_min": 5,
    "admin_password_set": False, "user_password_set": False,
    "admin_username": "",
}
_ON_NO_ADMIN = dict(_OFF_NO_ADMIN, web_enabled=True)
_ON_WITH_ADMIN = dict(_OFF_NO_ADMIN, web_enabled=True, admin_password_set=True, admin_username=_USERNAME)
_OFF_WITH_ADMIN = dict(_OFF_NO_ADMIN, admin_password_set=True, admin_username=_USERNAME)


class _Base(unittest.TestCase):
    def setUp(self):
        self._env_patch = unittest.mock.patch.dict(
            os.environ, {USERNAME_ENV: _USERNAME, PASSWORD_ENV: _SECRET_PASSWORD})
        self._env_patch.start()
        self._resolve_patch = unittest.mock.patch.object(
            mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")
        self._resolve_patch.start()

    def tearDown(self):
        self._env_patch.stop()
        self._resolve_patch.stop()

    def _assertNoSecretLeak(self, result: str):
        self.assertNotIn(_SECRET_PASSWORD, result)


class MissingCredentialTest(unittest.TestCase):
    def test_missing_both_refuses_before_any_http_call(self):
        with unittest.mock.patch.dict(os.environ, {}, clear=True), \
             unittest.mock.patch.object(wac, "get_auth_config") as get_mock:
            result = msw.web_auth_setup(confirm=True)
        self.assertIn("refused", result)
        self.assertIn("present=False", result)
        get_mock.assert_not_called()

    def test_missing_password_only_is_named(self):
        with unittest.mock.patch.dict(os.environ, {USERNAME_ENV: "admin"}, clear=True), \
             unittest.mock.patch.object(wac, "get_auth_config") as get_mock:
            result = msw.web_auth_setup(confirm=True)
        self.assertIn(f"{PASSWORD_ENV} present=False", result)
        self.assertIn(f"{USERNAME_ENV} present=True", result)
        get_mock.assert_not_called()


class AlreadyConfiguredTest(_Base):
    """Case 3: an admin record already exists. Only a login check happens;
    nothing is ever posted."""

    def test_valid_credentials_report_already_configured(self):
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_ON_WITH_ADMIN), \
             unittest.mock.patch.object(wac, "try_login", return_value=True) as login_mock, \
             unittest.mock.patch.object(wac, "post_security") as post_mock, \
             unittest.mock.patch.object(wac, "post_bootstrap_password") as boot_mock:
            result = msw.web_auth_setup(confirm=True)
        login_mock.assert_called_once_with("10.0.0.5", _USERNAME, _SECRET_PASSWORD)
        post_mock.assert_not_called()
        boot_mock.assert_not_called()
        self.assertIn("already configured, credentials valid", result)
        self._assertNoSecretLeak(result)

    def test_valid_credentials_checked_even_without_confirm(self):
        """A login is a read, not a write -- it must not be gated on confirm."""
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_ON_WITH_ADMIN), \
             unittest.mock.patch.object(wac, "try_login", return_value=True) as login_mock:
            result = msw.web_auth_setup(confirm=False)
        login_mock.assert_called_once()
        self.assertIn("already configured, credentials valid", result)

    def test_invalid_credentials_report_401_and_never_retry(self):
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_ON_WITH_ADMIN), \
             unittest.mock.patch.object(wac, "try_login", return_value=False) as login_mock:
            result = msw.web_auth_setup(confirm=True)
        login_mock.assert_called_once()
        self.assertIn("401", result)
        self.assertIn("refused", result)
        self._assertNoSecretLeak(result)

    def test_admin_exists_but_auth_off_still_only_logs_in(self):
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_OFF_WITH_ADMIN), \
             unittest.mock.patch.object(wac, "try_login", return_value=True) as login_mock, \
             unittest.mock.patch.object(wac, "post_security") as post_mock:
            result = msw.web_auth_setup(confirm=True)
        login_mock.assert_called_once()
        post_mock.assert_not_called()
        self.assertIn("already configured, credentials valid", result)


class DryRunTest(_Base):
    def test_dry_run_bootstrap_branch_does_not_post(self):
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_ON_NO_ADMIN), \
             unittest.mock.patch.object(wac, "post_bootstrap_password") as boot_mock:
            result = msw.web_auth_setup(confirm=False)
        self.assertIn("DRY RUN", result)
        self.assertIn("bootstrap_password", result)
        boot_mock.assert_not_called()

    def test_dry_run_off_branch_does_not_post(self):
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_OFF_NO_ADMIN), \
             unittest.mock.patch.object(wac, "post_security") as post_mock:
            result = msw.web_auth_setup(confirm=False)
        self.assertIn("DRY RUN", result)
        self.assertIn("set_web_password", result)
        post_mock.assert_not_called()


class BootstrapBranchTest(_Base):
    """Case 1: web auth already ON, no admin yet -- POST
    /api/auth/bootstrap_password."""

    def test_confirmed_bootstrap_succeeds_and_verifies(self):
        with unittest.mock.patch.object(
                wac, "get_auth_config", side_effect=[_ON_NO_ADMIN, _ON_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_bootstrap_password",
                                         return_value={"ok": True}) as boot_mock:
            result = msw.web_auth_setup(confirm=True)
        boot_mock.assert_called_once_with("10.0.0.5", _USERNAME, _SECRET_PASSWORD)
        self.assertIn("ok:", result)
        self._assertNoSecretLeak(result)

    def test_409_conflict_reported_distinctly(self):
        err = wac.WebAuthSetupHttpError("refused", status=409, detail="already configured")
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_ON_NO_ADMIN), \
             unittest.mock.patch.object(wac, "post_bootstrap_password", side_effect=err):
            result = msw.web_auth_setup(confirm=True)
        self.assertIn("409", result)

    def test_400_weak_password_reported_distinctly(self):
        err = wac.WebAuthSetupHttpError("refused", status=400, detail="password rejected")
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_ON_NO_ADMIN), \
             unittest.mock.patch.object(wac, "post_bootstrap_password", side_effect=err):
            result = msw.web_auth_setup(confirm=True)
        self.assertIn("400", result)
        self.assertIn("weak", result.lower())

    def test_unverified_success_fails_loud(self):
        """{"ok":true} on the POST but a read-back that still shows no
        admin record must never be reported as success -- same class as
        boot_guard's write-lies bug per CLAUDE.md."""
        with unittest.mock.patch.object(
                wac, "get_auth_config", side_effect=[_ON_NO_ADMIN, _ON_NO_ADMIN]), \
             unittest.mock.patch.object(wac, "post_bootstrap_password", return_value={"ok": True}):
            result = msw.web_auth_setup(confirm=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok:", result)


class OffBranchTest(_Base):
    """Case 2: web auth OFF, no admin yet -- set_web_password then, if
    enable_web_auth, set_policy, preserving the existing lcd_enabled/
    timeouts read back in step 1."""

    def test_confirmed_sets_password_and_enables_auth(self):
        with unittest.mock.patch.object(
                wac, "get_auth_config", side_effect=[_OFF_NO_ADMIN, _ON_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_security", return_value={"ok": True}) as post_mock:
            result = msw.web_auth_setup(confirm=True, enable_web_auth=True)
        self.assertEqual(post_mock.call_count, 2)
        first_call = post_mock.call_args_list[0]
        self.assertEqual(first_call.args[1]["cmd"], "set_web_password")
        self.assertEqual(first_call.args[1]["role"], "admin")
        self.assertEqual(first_call.args[1]["username"], _USERNAME)
        self.assertEqual(first_call.args[1]["password"], _SECRET_PASSWORD)
        second_call = post_mock.call_args_list[1]
        self.assertEqual(second_call.args[1]["cmd"], "set_policy")
        self.assertEqual(second_call.args[1]["web_enabled"], "1")
        # Preserves the board's existing lcd policy exactly as read back.
        self.assertEqual(second_call.args[1]["lcd_enabled"], "1")
        self.assertEqual(second_call.args[1]["lcd_timeout_min"], "5")
        self.assertEqual(second_call.args[1]["web_timeout_min"], "30")
        self.assertIn("ok:", result)
        self.assertIn("web auth enabled", result)
        self._assertNoSecretLeak(result)

    def test_enable_web_auth_false_only_sets_password(self):
        with unittest.mock.patch.object(
                wac, "get_auth_config", side_effect=[_OFF_NO_ADMIN, _OFF_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_security", return_value={"ok": True}) as post_mock:
            result = msw.web_auth_setup(confirm=True, enable_web_auth=False)
        self.assertEqual(post_mock.call_count, 1)
        self.assertEqual(post_mock.call_args_list[0].args[1]["cmd"], "set_web_password")
        self.assertIn("ok:", result)
        self.assertIn("left as-is", result)

    def test_set_web_password_rejected_stops_before_set_policy(self):
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_OFF_NO_ADMIN), \
             unittest.mock.patch.object(
                 wac, "post_security",
                 return_value={"ok": False, "error": "password too weak"}) as post_mock:
            result = msw.web_auth_setup(confirm=True)
        post_mock.assert_called_once()
        self.assertIn("rejected", result)
        self.assertIn("password too weak", result)

    def test_set_policy_rejected_is_reported_after_password_set(self):
        def fake_post(host, fields, **kw):
            if fields["cmd"] == "set_web_password":
                return {"ok": True}
            return {"ok": False, "error": "invalid transition"}

        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_OFF_NO_ADMIN), \
             unittest.mock.patch.object(wac, "post_security", side_effect=fake_post):
            result = msw.web_auth_setup(confirm=True)
        self.assertIn("password set", result)
        self.assertIn("invalid transition", result)

    def test_unverified_enable_fails_loud(self):
        """POST set_policy answers {"ok":true} but the read-back still
        shows web_enabled false -- must fail loud, not report success."""
        with unittest.mock.patch.object(
                wac, "get_auth_config", side_effect=[_OFF_NO_ADMIN, _OFF_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_security", return_value={"ok": True}):
            result = msw.web_auth_setup(confirm=True, enable_web_auth=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok:", result)


class NoSecretLeakTest(_Base):
    """Across every branch above, the raw password value must never appear
    in any string this tool returns."""

    def test_no_leak_in_every_branch(self):
        branches = [
            (_ON_WITH_ADMIN, {"try_login": True}),
            (_ON_NO_ADMIN, {"post_bootstrap_password": {"ok": True}}),
            (_OFF_NO_ADMIN, {"post_security": {"ok": True}}),
        ]
        for before, patches in branches:
            with self.subTest(before=before):
                configs = [before, dict(before, admin_password_set=True, web_enabled=True)]
                with unittest.mock.patch.object(wac, "get_auth_config", side_effect=configs), \
                     unittest.mock.patch.object(wac, "try_login", return_value=patches.get("try_login", True)), \
                     unittest.mock.patch.object(wac, "post_bootstrap_password",
                                                 return_value=patches.get("post_bootstrap_password", {"ok": True})), \
                     unittest.mock.patch.object(wac, "post_security",
                                                 return_value=patches.get("post_security", {"ok": True})):
                    result = msw.web_auth_setup(confirm=True)
                self.assertNotIn(_SECRET_PASSWORD, result)


if __name__ == "__main__":
    unittest.main()
