#!/usr/bin/env python3
"""Unit tests for wifi_prov_http_client's /ip_config helpers and the
network_get_ip_config / network_set_ip_config MCP tools
(mcp_server_network.py). Fakes only: no socket, no board.

Run with: python -m pytest tools/PcTools/tests/test_network_ip_config.py -q
"""
from __future__ import annotations

import http.client
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_network as mn  # noqa: E402
from kilnctrl import mcp_server_ota  # noqa: E402
from kilnctrl import wifi_prov_http_client as wph  # noqa: E402

GOOD = dict(ip="192.168.1.50", netmask="255.255.255.0", gateway="192.168.1.1")


class ValidateTest(unittest.TestCase):
    def test_good_static_and_dhcp(self):
        self.assertIsNone(wph.validate_ip_config("static", **GOOD))
        self.assertIsNone(wph.validate_ip_config("dhcp"))

    def test_bad_inputs_refused(self):
        bad = [
            ("static", dict(ip="192.168.4.9", netmask="255.255.255.0", gateway="192.168.4.1")),  # AP subnet
            ("static", dict(GOOD, ip="192.168.1")),              # short form
            ("static", dict(GOOD, ip="192.168.1.256")),          # octet
            ("static", dict(GOOD, ip="192.168.01.5")),           # leading zero
            ("static", dict(GOOD, netmask="255.0.255.0")),       # non-contiguous
            ("static", dict(GOOD, netmask="0.0.0.0")),
            ("static", dict(GOOD, netmask="255.255.255.255")),
            ("static", dict(GOOD, gateway="10.0.0.1")),          # outside subnet
            ("static", dict(GOOD, gateway="192.168.1.50")),      # == ip
            ("static", dict(GOOD, ip="192.168.1.0")),            # network addr
            ("static", dict(GOOD, ip="192.168.1.255")),          # broadcast
            ("static", dict(GOOD, ip="127.0.0.1", gateway="127.0.0.2", netmask="255.0.0.0")),
            ("static", dict(GOOD, ip=None)),                     # missing
            ("static", dict(GOOD, ip=5)),                        # not a str
            ("dhcp", dict(ip="1.2.3.4")),                        # dhcp takes none
            ("auto", {}),
            (None, {}),
        ]
        for mode, kw in bad:
            self.assertIsNotNone(wph.validate_ip_config(mode, **kw), (mode, kw))

    def test_body(self):
        self.assertEqual(wph.build_ip_config_body("dhcp"), b"mode=dhcp")
        self.assertEqual(wph.build_ip_config_body("static", **GOOD),
                         b"mode=static&ip=192.168.1.50&netmask=255.255.255.0&gateway=192.168.1.1")


class _Resp:
    def __init__(self, text="ok", exc=None):
        self._t, self._exc = text, exc

    def __enter__(self):
        return self

    def __exit__(self, *a):
        return False

    def read(self):
        if self._exc:
            raise self._exc
        return self._t.encode()


class PostTest(unittest.TestCase):
    def _post(self, **side):
        with unittest.mock.patch.object(wph.http_auth, "urlopen", **side) as m:
            return wph.post_ip_config("10.0.0.5", "static", **GOOD), m

    def test_ok_body(self):
        out, m = self._post(return_value=_Resp("ok"))
        self.assertEqual(out, "ok")
        req = m.call_args[0][0]
        self.assertEqual(req.get_method(), "POST")
        self.assertTrue(req.full_url.endswith("/ip_config"))

    def test_connection_reset_is_expected(self):
        out, _ = self._post(side_effect=ConnectionResetError(10054, "reset"))
        self.assertEqual(out, "dropped")

    def test_reset_wrapped_in_urlerror_is_expected(self):
        out, _ = self._post(side_effect=urllib.error.URLError(ConnectionResetError(10054, "x")))
        self.assertEqual(out, "dropped")

    def test_incomplete_read_is_expected(self):
        out, _ = self._post(return_value=_Resp(exc=http.client.IncompleteRead(b"")))
        self.assertEqual(out, "dropped")

    def test_http_400_raises_with_detail(self):
        err = urllib.error.HTTPError("u", 400, "Bad", {}, None)
        err.read = lambda: b"ip must not be in 192.168.4.0/24"
        with self.assertRaises(wph.WifiProvHttpError) as cm:
            self._post(side_effect=err)
        self.assertEqual(cm.exception.status, 400)
        self.assertIn("192.168.4.0/24", str(cm.exception))

    def test_connect_refused_is_failure_not_drop(self):
        with self.assertRaises(wph.WifiProvHttpError):
            self._post(side_effect=urllib.error.URLError(ConnectionRefusedError(10061, "refused")))

    def test_timeout_is_failure_not_drop(self):
        with self.assertRaises(wph.WifiProvHttpError):
            self._post(side_effect=TimeoutError("timed out"))


def _status(mode="static", ip="192.168.1.50", nm="255.255.255.0", gw="192.168.1.1", sta="192.168.1.50"):
    return {"ip_mode": mode, "static_ip": ip, "static_netmask": nm, "static_gateway": gw,
            "sta_ip": sta, "sta_connected": True}


class VerifyTest(unittest.TestCase):
    def _run(self, responses, mode="static", timeout_s=30.0, hosts=("192.168.1.50",), **kw):
        t = [0.0]
        calls = []
        it = iter(responses)

        def fetch(host, timeout=None):
            calls.append(host)
            r = next(it)
            if isinstance(r, BaseException):
                raise r
            return r

        res = wph.verify_ip_config(lambda: list(hosts), mode, timeout_s=timeout_s, poll_s=5.0,
                                   sleep=lambda s: t.__setitem__(0, t[0] + s), clock=lambda: t[0],
                                   get_status_fn=fetch, **kw)
        return res, calls

    def test_polls_through_unreachable_then_passes(self):
        (ok, why), calls = self._run([wph.WifiProvHttpError("x"), TimeoutError(), _status()], **GOOD)
        self.assertTrue(ok, why)
        self.assertEqual(len(calls), 3)

    def test_wrong_ip_fails_at_timeout(self):
        (ok, why), _ = self._run([_status(ip="192.168.1.99")] * 20, timeout_s=10.0, **GOOD)
        self.assertFalse(ok)
        self.assertIn("not verified", why)

    def test_wrong_mode_fails(self):
        (ok, _), _ = self._run([_status(mode="dhcp")] * 20, timeout_s=10.0, **GOOD)
        self.assertFalse(ok)

    def test_redacted_static_fields_cannot_verify(self):
        (ok, why), _ = self._run([_status(ip=None, nm=None, gw=None)] * 20, timeout_s=10.0, **GOOD)
        self.assertFalse(ok)
        self.assertIn("redacted", why)

    def test_never_reached_fails(self):
        (ok, why), _ = self._run([wph.WifiProvHttpError("down")] * 20, timeout_s=10.0, **GOOD)
        self.assertFalse(ok)
        self.assertIn("down", why)

    def test_dhcp_accepts_empty_and_redacted_but_not_leftover(self):
        (ok, _), _ = self._run([_status(mode="dhcp", ip="", nm="", gw="")], mode="dhcp")
        self.assertTrue(ok)
        (ok, _), _ = self._run([_status(mode="dhcp", ip=None, nm=None, gw=None)], mode="dhcp")
        self.assertTrue(ok)
        (ok, _), _ = self._run([_status(mode="dhcp")] * 20, mode="dhcp", timeout_s=10.0)
        self.assertFalse(ok)

    def test_hosts_re_resolved_every_attempt(self):
        seq = iter([["10.0.0.5"], ["10.0.0.5", "10.0.0.77"], ["10.0.0.77"]])
        t = [0.0]
        seen = []

        def fetch(host, timeout=None):
            seen.append(host)
            return _status(mode="dhcp", ip="", nm="", gw="") if host == "10.0.0.77" else \
                (_ for _ in ()).throw(wph.WifiProvHttpError("down"))

        ok, _ = wph.verify_ip_config(lambda: next(seq), "dhcp", timeout_s=30, poll_s=1,
                                     sleep=lambda s: t.__setitem__(0, t[0] + s), clock=lambda: t[0],
                                     get_status_fn=fetch)
        self.assertTrue(ok)
        self.assertEqual(seen, ["10.0.0.5", "10.0.0.5", "10.0.0.77"])


class GetStatusAdminTest(unittest.TestCase):
    def test_logs_in_when_redacted_and_rereads(self):
        reads = iter([_status(ip=None, nm=None, gw=None), _status()])
        login = unittest.mock.Mock()
        with unittest.mock.patch.object(wph, "get_status", side_effect=lambda *a, **k: next(reads)):
            st = wph.get_status_admin("10.0.0.5", login=login)
        login.assert_called_once_with("http://10.0.0.5")
        self.assertEqual(st["static_ip"], "192.168.1.50")

    def test_no_login_when_already_visible(self):
        login = unittest.mock.Mock()
        with unittest.mock.patch.object(wph, "get_status", return_value=_status()):
            wph.get_status_admin("10.0.0.5", login=login)
        login.assert_not_called()

    def test_login_failure_keeps_redacted_view(self):
        with unittest.mock.patch.object(wph, "get_status", return_value=_status(ip=None, nm=None, gw=None)):
            st = wph.get_status_admin("10.0.0.5", login=unittest.mock.Mock(side_effect=RuntimeError("no cred")))
        self.assertIsNone(st["static_ip"])


class _Base(unittest.TestCase):
    def setUp(self):
        self.patches = [
            unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"),
            unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host_with_source",
                                       return_value=("10.0.0.9", "STA IP")),
            unittest.mock.patch("kilnctrl.mcp_server_control._profile_or_autotune_running_reason",
                                return_value=None),
        ]
        self.mocks = [p.start() for p in self.patches]
        for p in self.patches:
            self.addCleanup(p.stop)


class ToolGateTest(_Base):
    def test_confirm_must_be_exactly_true(self):
        with unittest.mock.patch.object(wph, "post_ip_config") as post, \
                unittest.mock.patch.object(wph, "get_status_admin") as get:
            for c in (False, 1, "yes", None):
                r = mn.network_set_ip_config("static", confirm=c, **GOOD)
                self.assertTrue(r.startswith("refused"), (c, r))
            post.assert_not_called()
            get.assert_not_called()

    def test_validation_before_any_io(self):
        with unittest.mock.patch.object(wph, "post_ip_config") as post, \
                unittest.mock.patch.object(wph, "get_status_admin") as get:
            r = mn.network_set_ip_config("static", confirm=True, **dict(GOOD, ip="192.168.4.2"))
            self.assertTrue(r.startswith("refused"), r)
            post.assert_not_called()
            get.assert_not_called()

    def test_refused_mid_run(self):
        self.mocks[2].return_value = "a profile is currently running (#3 'x')"
        with unittest.mock.patch.object(wph, "post_ip_config") as post, \
                unittest.mock.patch.object(wph, "get_status_admin") as get:
            r = mn.network_set_ip_config("dhcp", confirm=True)
        self.assertIn("refused", r)
        self.assertIn("mid-run", r)
        post.assert_not_called()
        get.assert_not_called()


class ToolFlowTest(_Base):
    def test_static_success_polls_new_address_and_warns_move(self):
        seen = []

        def fake_verify(resolve_hosts, mode, ip, nm, gw, **kw):
            seen.append(list(resolve_hosts()))
            return True, "verified at x"

        with unittest.mock.patch.object(wph, "get_status_admin",
                                        return_value=_status(mode="dhcp", ip="", nm="", gw="", sta="10.0.0.5")), \
                unittest.mock.patch.object(wph, "post_ip_config", return_value="dropped") as post, \
                unittest.mock.patch.object(wph, "verify_ip_config", side_effect=fake_verify):
            r = mn.network_set_ip_config("static", confirm=True, **GOOD)
        self.assertTrue(r.startswith("ok:"), r)
        self.assertIn("MOVE", r)
        self.assertIn("expected", r)
        self.assertEqual(seen, [["192.168.1.50"]])
        post.assert_called_once()

    def test_dhcp_polls_old_host_and_uart_sta_ip(self):
        seen = []

        def fake_verify(resolve_hosts, *a, **kw):
            seen.append(list(resolve_hosts()))
            return True, "ok"

        with unittest.mock.patch.object(wph, "get_status_admin", return_value=_status()), \
                unittest.mock.patch.object(wph, "post_ip_config", return_value="dropped"), \
                unittest.mock.patch.object(wph, "verify_ip_config", side_effect=fake_verify):
            r = mn.network_set_ip_config("dhcp", confirm=True)
        self.assertTrue(r.startswith("ok:"), r)
        self.assertEqual(seen, [["10.0.0.5", "10.0.0.9"]])

    def test_verification_failure_is_loud(self):
        with unittest.mock.patch.object(wph, "get_status_admin",
                                        return_value=_status(mode="dhcp", ip="", nm="", gw="")), \
                unittest.mock.patch.object(wph, "post_ip_config", return_value="dropped"), \
                unittest.mock.patch.object(wph, "verify_ip_config", return_value=(False, "not verified")):
            r = mn.network_set_ip_config("static", confirm=True, **GOOD)
        self.assertTrue(r.startswith("FAILED verification"), r)

    def test_board_400_reported_as_failure_no_verify(self):
        with unittest.mock.patch.object(wph, "get_status_admin",
                                        return_value=_status(mode="dhcp", ip="", nm="", gw="")), \
                unittest.mock.patch.object(wph, "post_ip_config",
                                           side_effect=wph.WifiProvHttpError("refused: HTTP 400: bad", 400)), \
                unittest.mock.patch.object(wph, "verify_ip_config") as ver:
            r = mn.network_set_ip_config("static", confirm=True, **GOOD)
        self.assertTrue(r.startswith("failed:"), r)
        ver.assert_not_called()

    def test_already_configured_sends_nothing(self):
        with unittest.mock.patch.object(wph, "get_status_admin", return_value=_status()), \
                unittest.mock.patch.object(wph, "post_ip_config") as post:
            r = mn.network_set_ip_config("static", confirm=True, **GOOD)
        self.assertIn("already configured", r)
        post.assert_not_called()

    def test_pre_read_failure_sends_nothing(self):
        with unittest.mock.patch.object(wph, "get_status_admin", side_effect=wph.WifiProvHttpError("down")), \
                unittest.mock.patch.object(wph, "post_ip_config") as post:
            r = mn.network_set_ip_config("dhcp", confirm=True)
        self.assertTrue(r.startswith("error:"), r)
        post.assert_not_called()


class GetToolTest(_Base):
    def test_reports_redaction(self):
        with unittest.mock.patch.object(wph, "get_status_admin",
                                        return_value=_status(ip=None, nm=None, gw=None)):
            r = mn.network_get_ip_config()
        self.assertIn("redacted", r)
        self.assertIn("ip_mode='static'", r)

    def test_reports_values(self):
        with unittest.mock.patch.object(wph, "get_status_admin", return_value=_status()):
            r = mn.network_get_ip_config()
        self.assertIn("192.168.1.50", r)
        self.assertIn("255.255.255.0", r)

    def test_error_path(self):
        with unittest.mock.patch.object(wph, "get_status_admin", side_effect=wph.WifiProvHttpError("down")):
            self.assertTrue(mn.network_get_ip_config().startswith("error:"))


if __name__ == "__main__":
    unittest.main()
