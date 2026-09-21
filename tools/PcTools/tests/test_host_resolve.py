#!/usr/bin/env python3
"""Unit tests for kilnctrl.host_resolve -- the default-host resolution order
(``KILNCTL_HOST`` env var -> last-seen cache in settings.json -> the board's
AP fallback address) and its wiring into kilnctrl.http_auth.urlopen.

No real settings.json is ever touched: every test points ``path=`` at a
tempfile that is removed on cleanup.

Run with: python -m pytest tools/PcTools/tests/test_host_resolve.py -q
"""
from __future__ import annotations

import email.message
import io
import os
import sys
import tempfile
import unittest
import unittest.mock
import urllib.request

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import host_resolve, http_auth  # noqa: E402

URL = "http://192.168.1.156/api/zones"


def _response(body: bytes = b"{}"):
    resp = io.BytesIO(body)
    resp.status = 200
    resp.headers = email.message.Message()

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class HostResolveTest(unittest.TestCase):
    def setUp(self):
        fd, self._path = tempfile.mkstemp(suffix=".json")
        os.close(fd)
        os.remove(self._path)  # start absent, exactly like a fresh checkout
        self.path = __import__("pathlib").Path(self._path)
        self.addCleanup(lambda: os.path.exists(self._path) and os.remove(self._path))
        self._env_patcher = unittest.mock.patch.dict(os.environ, {}, clear=False)
        self._env_patcher.start()
        os.environ.pop(host_resolve.HOST_ENV, None)
        self.addCleanup(self._env_patcher.stop)

    # --- resolution order --------------------------------------------------
    def test_falls_back_to_ap_address_with_nothing_set(self):
        self.assertEqual(host_resolve.resolve_default_host(self.path), host_resolve.FALLBACK_HOST)

    def test_uses_cached_last_host_over_the_ap_fallback(self):
        host_resolve.record_host_seen("192.168.1.156", self.path)
        self.assertEqual(host_resolve.resolve_default_host(self.path), "192.168.1.156")

    def test_env_var_wins_over_the_cache(self):
        host_resolve.record_host_seen("192.168.1.156", self.path)
        os.environ[host_resolve.HOST_ENV] = "192.168.1.200"
        self.assertEqual(host_resolve.resolve_default_host(self.path), "192.168.1.200")

    def test_env_var_wins_with_no_cache_at_all(self):
        os.environ[host_resolve.HOST_ENV] = "10.0.0.5"
        self.assertEqual(host_resolve.resolve_default_host(self.path), "10.0.0.5")

    def test_blank_env_var_is_ignored(self):
        os.environ[host_resolve.HOST_ENV] = "   "
        self.assertEqual(host_resolve.resolve_default_host(self.path), host_resolve.FALLBACK_HOST)

    # --- record_host_seen parsing -------------------------------------------
    def test_record_accepts_a_bare_host(self):
        host_resolve.record_host_seen("192.168.1.156", self.path)
        self.assertEqual(host_resolve.resolve_default_host(self.path), "192.168.1.156")

    def test_record_strips_scheme_and_port(self):
        host_resolve.record_host_seen("http://192.168.1.156:8080", self.path)
        self.assertEqual(host_resolve.resolve_default_host(self.path), "192.168.1.156")

    def test_record_ignores_empty_input(self):
        host_resolve.record_host_seen("", self.path)
        self.assertEqual(host_resolve.resolve_default_host(self.path), host_resolve.FALLBACK_HOST)

    # --- never pollute the real, shared settings.json from a test ----------
    def test_record_host_seen_refuses_to_write_the_real_settings_file_under_pytest(self):
        """The bug this guards against: test_ota_http_client.py (and others)
        mock ``urllib.request.urlopen`` and call a client function with a
        fixture host like "kiln.local", with NO patch on
        ``settings.SETTINGS_PATH`` -- exactly what an ordinary client test
        looks like. Before this guard, that meant a mocked "success" inside
        the test suite silently wrote "kiln.local" into the real, shared
        settings.json, which then poisoned every *_AP_DEFAULT_HOST constant
        computed at import time in a LATER pytest process (this really
        happened -- see the commit message)."""
        self.assertTrue(host_resolve._PRODUCTION_SETTINGS_PATH.name == "settings.json")
        with unittest.mock.patch.object(host_resolve.settings, "set_last_host") as mock_set:
            host_resolve.record_host_seen("kiln.local")  # no path= -- real default
        mock_set.assert_not_called()

    def test_record_host_seen_still_writes_an_explicit_tempfile_path_under_pytest(self):
        """The guard must not swallow every write while under pytest -- only
        ones aimed at the real, shared file. This is exactly what
        test_a_successful_request_through_http_auth_updates_the_cache below
        depends on."""
        host_resolve.record_host_seen("192.168.1.156", self.path)
        self.assertEqual(host_resolve.resolve_default_host(self.path), "192.168.1.156")

    # --- wiring into http_auth.urlopen --------------------------------------
    def test_a_successful_request_through_http_auth_updates_the_cache(self):
        """This is the point of the whole feature: a real, successful HTTP
        call anywhere in the package should make the NEXT unspecified-host
        call default to the host that just answered."""
        http_auth.clear_sessions()
        self.addCleanup(http_auth.clear_sessions)
        recorder_response = _response(b'{"ok":true}')
        with unittest.mock.patch.object(urllib.request, "urlopen", return_value=recorder_response):
            with unittest.mock.patch.object(host_resolve.settings, "SETTINGS_PATH", self.path):
                with http_auth.urlopen(urllib.request.Request(URL), timeout=2.0):
                    pass
                self.assertEqual(host_resolve.resolve_default_host(self.path), "192.168.1.156")


if __name__ == "__main__":
    unittest.main()
