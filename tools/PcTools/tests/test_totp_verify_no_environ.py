"""totp_reset_password verification never mutates os.environ; raw body not echoed."""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock as um

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import http_auth, mcp_server_totp as mst, totp_http_client as thc  # noqa: E402
from test_mcp_server_totp import _Base  # noqa: E402


class VerifyNoEnvironTest(_Base):
    def test_login_gets_override_and_environ_untouched(self):
        self._set_env(new_password="Sup3rSecretPW!")
        os.environ[http_auth.PASSWORD_ENV] = "oldpw"
        seen = {}

        def fake_login(origin, timeout=None, password_override=None):
            seen["env"] = os.environ.get(http_auth.PASSWORD_ENV)
            seen["override"] = password_override
            return "cookie"

        with um.patch.object(thc, "post_forgot", return_value=(202, {"reset_token": "t"})), \
                um.patch.object(thc, "post_reset", return_value=(200, {})), \
                um.patch.object(http_auth, "login", side_effect=fake_login):
            mst.totp_reset_password(confirm=True)
        self.assertEqual(seen["env"], "oldpw")
        self.assertEqual(seen["override"], "Sup3rSecretPW!")
        self.assertEqual(os.environ[http_auth.PASSWORD_ENV], "oldpw")

    def test_credentials_override(self):
        os.environ[http_auth.USERNAME_ENV] = "bench"
        self.assertEqual(http_auth.credentials("x"), ("bench", "x"))


class BodyNotEchoedTest(unittest.TestCase):
    def test_non_json_2xx_body_not_in_error(self):
        with self.assertRaises(thc.TotpHttpError) as cm:
            thc._parse_json_body("/api/auth/forgot", 200, "SECRET-TOKEN-xyz not json")
        self.assertNotIn("SECRET-TOKEN-xyz", str(cm.exception))
        self.assertNotIn("SECRET-TOKEN-xyz", repr(getattr(cm.exception, "body", "")))


if __name__ == "__main__":
    unittest.main()
