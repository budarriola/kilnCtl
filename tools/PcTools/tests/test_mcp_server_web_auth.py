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
        # web_enabled true + a successful pre-fetch read proves the session
        # is already authenticated (fix 2) -- no separate login POST fires.
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_ON_WITH_ADMIN), \
             unittest.mock.patch.object(wac, "try_login") as login_mock, \
             unittest.mock.patch.object(wac, "post_security") as post_mock, \
             unittest.mock.patch.object(wac, "post_bootstrap_password") as boot_mock:
            result = msw.web_auth_setup(confirm=True)
        login_mock.assert_not_called()
        post_mock.assert_not_called()
        boot_mock.assert_not_called()
        self.assertIn("already configured, credentials valid", result)
        self._assertNoSecretLeak(result)

    def test_valid_credentials_checked_even_without_confirm(self):
        """A dry run still must not fire a write, and the already-
        authenticated pre-fetch alone is enough to report validity."""
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_ON_WITH_ADMIN), \
             unittest.mock.patch.object(wac, "try_login") as login_mock:
            result = msw.web_auth_setup(confirm=False)
        login_mock.assert_not_called()
        self.assertIn("already configured, credentials valid", result)

    def test_invalid_credentials_report_401_and_never_retry(self):
        """When the pre-fetch itself needed no session (web auth off), a
        successful GET proves nothing about the credential -- the explicit
        login check still runs and can still fail."""
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_OFF_WITH_ADMIN), \
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

    def test_admin_exists_with_minus_one_board_timeouts_has_no_substitution_note(self):
        """Regression: case 3 writes nothing regardless of what the board's
        timeouts read, so a board reporting -1 for both must not make this
        tool claim it substituted a default."""
        config = dict(_ON_WITH_ADMIN, web_timeout_min=-1, lcd_timeout_min=-1)
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=config), \
             unittest.mock.patch.object(wac, "try_login") as login_mock:
            result = msw.web_auth_setup(confirm=True)
        login_mock.assert_not_called()
        self.assertIn("already configured, credentials valid", result)
        self.assertNotIn("substituted default", result)
        self.assertNotIn("NOTE:", result)


class UnreadableConfigTest(_Base):
    """Review fix 1: pre-fetch denied (401/HttpAuthError, not "unreachable")
    must proceed to bootstrap_password rather than bailing with "could not
    read"."""

    def test_denied_prefetch_proceeds_to_bootstrap(self):
        err = wac.WebAuthSetupHttpError("HTTP Error 401: Unauthorized", status=401)
        with unittest.mock.patch.object(wac, "get_auth_config",
                                         side_effect=[err, _ON_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_bootstrap_password",
                                         return_value={"ok": True}) as boot_mock:
            result = msw.web_auth_setup(confirm=True)
        boot_mock.assert_called_once_with("10.0.0.5", _USERNAME, _SECRET_PASSWORD)
        self.assertNotIn("could not read", result)
        self.assertIn("ok:", result)

    def test_denied_prefetch_dry_run_names_bootstrap_action(self):
        err = wac.WebAuthSetupHttpError("HTTP Error 403: Forbidden", status=403)
        with unittest.mock.patch.object(wac, "get_auth_config", side_effect=err), \
             unittest.mock.patch.object(wac, "post_bootstrap_password") as boot_mock:
            result = msw.web_auth_setup(confirm=False)
        boot_mock.assert_not_called()
        self.assertIn("DRY RUN", result)
        self.assertIn("bootstrap_password", result)

    def test_denied_prefetch_dry_run_has_no_substitution_note(self):
        """Regression: case 1 (config unreadable) must not claim a timeout
        substitution -- raw_* there is only the before.get(..., -1)
        fallback over an empty dict, and nothing is ever sent in this case
        regardless of it."""
        err = wac.WebAuthSetupHttpError("HTTP Error 403: Forbidden", status=403)
        with unittest.mock.patch.object(wac, "get_auth_config", side_effect=err):
            result = msw.web_auth_setup(confirm=False)
        self.assertNotIn("substituted default", result)
        self.assertNotIn("NOTE:", result)

    def test_genuinely_unreachable_board_still_hard_errors(self):
        err = wac.WebAuthSetupHttpError("board unreachable: [Errno 111] Connection refused")
        with unittest.mock.patch.object(wac, "get_auth_config", side_effect=err), \
             unittest.mock.patch.object(wac, "post_bootstrap_password") as boot_mock:
            result = msw.web_auth_setup(confirm=True)
        boot_mock.assert_not_called()
        self.assertIn("could not read", result)

    def test_denied_prefetch_409_falls_back_to_single_login(self):
        """The unreadable pre-fetch is ambiguous; bootstrap's 409 resolves it
        to "admin already exists" and exactly one login check follows."""
        prefetch_err = wac.WebAuthSetupHttpError("HTTP Error 401: Unauthorized", status=401)
        boot_err = wac.WebAuthSetupHttpError("refused", status=409, detail="already configured")
        with unittest.mock.patch.object(wac, "get_auth_config", side_effect=prefetch_err), \
             unittest.mock.patch.object(wac, "post_bootstrap_password", side_effect=boot_err), \
             unittest.mock.patch.object(wac, "try_login", return_value=True) as login_mock:
            result = msw.web_auth_setup(confirm=True)
        login_mock.assert_called_once_with("10.0.0.5", _USERNAME, _SECRET_PASSWORD)
        self.assertIn("already configured, credentials valid", result)


class SingleLoginTest(_Base):
    """Review fix 2: when the pre-fetch itself succeeded through an
    authenticated session (web_enabled true, admin configured), no second
    login POST may be sent."""

    def test_no_redundant_login_when_prefetch_already_authenticated(self):
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_ON_WITH_ADMIN), \
             unittest.mock.patch.object(wac, "try_login") as login_mock:
            result = msw.web_auth_setup(confirm=True)
        login_mock.assert_not_called()
        self.assertIn("already configured, credentials valid", result)
        self.assertIn("no separate login was sent", result)

    def test_login_still_used_when_admin_exists_but_auth_off(self):
        """web_enabled false means the successful GET proved nothing about
        the credential -- the explicit login check must still run."""
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=_OFF_WITH_ADMIN), \
             unittest.mock.patch.object(wac, "try_login", return_value=True) as login_mock:
            result = msw.web_auth_setup(confirm=True)
        login_mock.assert_called_once()
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
                wac, "get_auth_config",
                side_effect=[_OFF_NO_ADMIN, _OFF_WITH_ADMIN, _ON_WITH_ADMIN]), \
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

        with unittest.mock.patch.object(
                wac, "get_auth_config", side_effect=[_OFF_NO_ADMIN, _OFF_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_security", side_effect=fake_post):
            result = msw.web_auth_setup(confirm=True)
        self.assertIn("password set", result)
        self.assertIn("invalid transition", result)

    def test_unverified_enable_fails_loud(self):
        """POST set_policy answers {"ok":true} but the read-back still
        shows web_enabled false -- must fail loud, not report success."""
        with unittest.mock.patch.object(
                wac, "get_auth_config",
                side_effect=[_OFF_NO_ADMIN, _OFF_WITH_ADMIN, _OFF_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_security", return_value={"ok": True}):
            result = msw.web_auth_setup(confirm=True, enable_web_auth=True)
        self.assertIn("FAILED", result)
        self.assertNotIn("ok:", result)


class TimeoutSubstitutionTest(_Base):
    """After an NVS erase, web_auth_backend_get_config() reports -1 for both
    timeouts because no policy record exists yet -- the same sentinel that
    means "never expire". web_auth_setup() must not echo that -1 straight
    back into set_policy."""

    _NO_RECORD = dict(_OFF_NO_ADMIN, web_timeout_min=-1, lcd_timeout_min=-1)

    def test_minus_one_readback_is_substituted_with_defaults(self):
        with unittest.mock.patch.object(
                wac, "get_auth_config",
                side_effect=[self._NO_RECORD, _OFF_WITH_ADMIN, _ON_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_security", return_value={"ok": True}) as post_mock:
            result = msw.web_auth_setup(confirm=True, enable_web_auth=True)
        second_call = post_mock.call_args_list[1]
        self.assertEqual(second_call.args[1]["cmd"], "set_policy")
        self.assertEqual(second_call.args[1]["web_timeout_min"], "30")
        self.assertEqual(second_call.args[1]["lcd_timeout_min"], "10")
        self.assertIn("substituted default", result)
        self.assertIn("ok:", result)

    def test_explicit_minus_one_is_honored_with_warning(self):
        with unittest.mock.patch.object(
                wac, "get_auth_config",
                side_effect=[self._NO_RECORD, _OFF_WITH_ADMIN, _ON_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_security", return_value={"ok": True}) as post_mock:
            result = msw.web_auth_setup(confirm=True, enable_web_auth=True,
                                         web_timeout_min=-1, lcd_timeout_min=-1)
        second_call = post_mock.call_args_list[1]
        self.assertEqual(second_call.args[1]["web_timeout_min"], "-1")
        self.assertEqual(second_call.args[1]["lcd_timeout_min"], "-1")
        self.assertIn("requested explicitly", result)
        self.assertIn("NEVER expire", result)

    def test_out_of_range_override_refused_before_any_http_call(self):
        with unittest.mock.patch.object(wac, "get_auth_config") as get_mock:
            result = msw.web_auth_setup(confirm=True, web_timeout_min=0)
        self.assertIn("refused", result)
        self.assertIn("web_timeout_min=0", result)
        get_mock.assert_not_called()

    def test_out_of_range_lcd_override_refused_before_any_http_call(self):
        with unittest.mock.patch.object(wac, "get_auth_config") as get_mock:
            result = msw.web_auth_setup(confirm=True, lcd_timeout_min=61)
        self.assertIn("refused", result)
        self.assertIn("lcd_timeout_min=61", result)
        get_mock.assert_not_called()

    def test_dry_run_names_substituted_defaults_not_raw_minus_one(self):
        """A dry run never reaches the set_policy call, but its 'would:'
        line must still report the values this tool would actually send --
        not the board's raw -1 with no explanation of what's about to
        happen to it. (The separate 'before:' line legitimately still shows
        the board's raw -1 read; only the action line is asserted here.)"""
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=self._NO_RECORD):
            result = msw.web_auth_setup(confirm=False, enable_web_auth=True)
        self.assertIn("DRY RUN", result)
        would_line = result.splitlines()[0]
        self.assertIn("set_policy(web_enabled=1, web_timeout_min=30, lcd_timeout_min=10)", would_line)
        self.assertIn("substituted default", result)

    def test_dry_run_state_line_still_shows_raw_before_value(self):
        with unittest.mock.patch.object(wac, "get_auth_config", return_value=self._NO_RECORD):
            result = msw.web_auth_setup(confirm=False, enable_web_auth=True)
        self.assertIn("before: ", result)
        self.assertIn("web_timeout_min=-1 lcd_timeout_min=-1", result)

    def test_valid_override_is_used_verbatim_no_note(self):
        with unittest.mock.patch.object(
                wac, "get_auth_config",
                side_effect=[_OFF_NO_ADMIN, _OFF_WITH_ADMIN, _ON_WITH_ADMIN]), \
             unittest.mock.patch.object(wac, "post_security", return_value={"ok": True}) as post_mock:
            result = msw.web_auth_setup(confirm=True, enable_web_auth=True, web_timeout_min=45)
        second_call = post_mock.call_args_list[1]
        self.assertEqual(second_call.args[1]["web_timeout_min"], "45")
        self.assertNotIn("substituted default", result)


class ReadBackRefusalTest(_Base):
    """Review fix 3: set_web_password reports ok:true but the read-back
    before set_policy still shows admin_password_set false -- must refuse
    to enable web auth and must never call set_policy."""

    def test_refuses_set_policy_when_readback_shows_no_admin(self):
        with unittest.mock.patch.object(
                wac, "get_auth_config", side_effect=[_OFF_NO_ADMIN, _OFF_NO_ADMIN]), \
             unittest.mock.patch.object(
                 wac, "post_security", return_value={"ok": True}) as post_mock:
            result = msw.web_auth_setup(confirm=True, enable_web_auth=True)
        self.assertEqual(post_mock.call_count, 1)
        self.assertEqual(post_mock.call_args_list[0].args[1]["cmd"], "set_web_password")
        self.assertIn("FAILED verification", result)
        self.assertIn("refusing to enable web auth", result)

    def test_readback_failure_itself_refuses_set_policy(self):
        readback_err = wac.WebAuthSetupHttpError("board unreachable: timed out")
        with unittest.mock.patch.object(
                wac, "get_auth_config", side_effect=[_OFF_NO_ADMIN, readback_err]), \
             unittest.mock.patch.object(
                 wac, "post_security", return_value={"ok": True}) as post_mock:
            result = msw.web_auth_setup(confirm=True, enable_web_auth=True)
        self.assertEqual(post_mock.call_count, 1)
        self.assertIn("refusing to enable web auth", result)


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
