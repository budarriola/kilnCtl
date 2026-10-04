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

from kilnctrl import http_auth  # noqa: E402
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

    def test_read_timeout_after_connect_is_ambiguous_not_failure(self):
        out, _ = self._post(side_effect=TimeoutError("timed out"))
        self.assertEqual(out, "timeout")

    def test_connect_timeout_is_a_definite_failure(self):
        with self.assertRaises(wph.WifiProvHttpError):
            self._post(side_effect=urllib.error.URLError(TimeoutError("timed out")))


def _status(mode="static", ip="192.168.1.50", nm="255.255.255.0", gw="192.168.1.1", sta="192.168.1.50"):
    return {"ip_mode": mode, "static_ip": ip, "static_netmask": nm, "static_gateway": gw,
            "sta_ip": sta, "sta_connected": True}


class VerifyTest(unittest.TestCase):
    def _run(self, responses, mode="static", timeout_s=30.0, hosts=("192.168.1.50",),
             is_trusted=lambda h: True, **kw):
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
                                   is_trusted=is_trusted,
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

        ok, _ = wph.verify_ip_config(lambda: next(seq), "dhcp", is_trusted=lambda h: True, timeout_s=30, poll_s=1,
                                     sleep=lambda s: t.__setitem__(0, t[0] + s), clock=lambda: t[0],
                                     get_status_fn=fetch)
        self.assertTrue(ok)
        self.assertEqual(seen, ["10.0.0.5", "10.0.0.5", "10.0.0.77"])


    def test_untrusted_host_is_never_contacted_and_never_passes(self):
        (ok, why), calls = self._run([_status()] * 20, timeout_s=10.0, is_trusted=lambda h: False, **GOOD)
        self.assertFalse(ok)
        self.assertEqual(calls, [])
        self.assertIn("not contacted", why)

    def test_trust_is_asked_per_host_per_attempt(self):
        asked = []
        trust = {"on": False}

        def is_trusted(h):
            asked.append(h)
            return trust["on"]

        responses = iter([_status()])
        t = [0.0]

        def sleep(s):
            t[0] += s
            trust["on"] = True

        ok, _ = wph.verify_ip_config(lambda: ["192.168.1.50"], "static", **GOOD, is_trusted=is_trusted,
                                     timeout_s=30, poll_s=1, sleep=sleep, clock=lambda: t[0],
                                     get_status_fn=lambda h, timeout=None: next(responses))
        self.assertTrue(ok)
        self.assertEqual(asked, ["192.168.1.50", "192.168.1.50"])


class GetStatusTrustTest(unittest.TestCase):
    def test_untrusted_get_status_uses_no_relogin(self):
        with unittest.mock.patch.object(wph.http_auth, "urlopen", return_value=_Resp('{"ip_mode": "dhcp"}')) as m:
            wph.get_status("10.0.0.9", trusted=False)
            self.assertIs(m.call_args.kwargs.get("no_relogin"), True)
            wph.get_status("10.0.0.9")
            self.assertIs(m.call_args.kwargs.get("no_relogin"), False)

    def test_foreign_401_at_untrusted_host_gets_no_credentials(self):
        err = urllib.error.HTTPError("u", 401, "Unauthorized", {}, None)
        err.read = lambda: b""
        login = unittest.mock.Mock()
        with unittest.mock.patch.object(wph.http_auth, "urlopen", side_effect=err), \
                unittest.mock.patch.object(wph.http_auth, "login", login), \
                unittest.mock.patch.object(wph.http_auth, "_login", login):
            with self.assertRaises(wph.WifiProvHttpError):
                wph.get_status_admin("10.0.0.9", trusted=False)
        login.assert_not_called()

    def test_real_urlopen_never_logs_in_for_untrusted_401(self):
        # Exercise the real http_auth.urlopen: only the urllib layer is faked.
        err = urllib.error.HTTPError("http://10.0.0.9/status", 401, "Unauthorized", {}, None)
        err.read = lambda: b""
        login = unittest.mock.Mock()
        with unittest.mock.patch("urllib.request.urlopen", side_effect=err) as raw, \
                unittest.mock.patch.object(wph.http_auth, "_login", login):
            with self.assertRaises(wph.WifiProvHttpError):
                wph.get_status("10.0.0.9", trusted=False)
        login.assert_not_called()
        self.assertEqual(raw.call_count, 1)


class ProbeTest(unittest.TestCase):
    def _probe(self, **side):
        with unittest.mock.patch.object(wph.http_auth, "urlopen", **side):
            return wph.probe_host_answers("192.168.1.50")[0]

    def test_answers(self):
        self.assertTrue(self._probe(return_value=_Resp("x")))
        self.assertTrue(self._probe(side_effect=urllib.error.HTTPError("u", 401, "x", {}, None)))
        self.assertTrue(self._probe(side_effect=urllib.error.URLError(ConnectionRefusedError(10061, "r"))))

    def test_free(self):
        self.assertFalse(self._probe(side_effect=urllib.error.URLError(TimeoutError("t"))))
        self.assertFalse(self._probe(side_effect=urllib.error.URLError(OSError(10051, "no route"))))

    def test_probe_sends_no_credentials(self):
        # A 401 must not trigger a login: the probe passes no_relogin=True.
        with unittest.mock.patch.object(wph.http_auth, "urlopen",
                                        side_effect=urllib.error.HTTPError("u", 401, "x", {}, None)) as ha:
            wph.probe_host_answers("192.168.1.50")
        self.assertIs(ha.call_args.kwargs.get("no_relogin"), True)


class NoRecordTest(unittest.TestCase):
    """A foreign device answering at an unverified address must never be
    persisted as last_host (a later credentialed call could target it)."""

    def _run(self, fn):
        with unittest.mock.patch("urllib.request.urlopen", return_value=_Resp("{}")), \
                unittest.mock.patch.object(http_auth.host_resolve, "record_host_seen") as rec:
            fn()
        return rec

    def test_probe_never_records_on_2xx(self):
        rec = self._run(lambda: wph.probe_host_answers("192.168.1.50"))
        rec.assert_not_called()

    def test_untrusted_status_read_never_records(self):
        rec = self._run(lambda: wph.get_status("192.168.1.50", trusted=False))
        rec.assert_not_called()

    def test_trusted_status_read_still_records(self):
        rec = self._run(lambda: wph.get_status("10.0.0.5", trusted=True))
        rec.assert_called_once()


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

    def test_no_login_when_reply_lacks_board_keys(self):
        # A foreign JSON (no ip_mode) must not make us send the admin credentials.
        login = unittest.mock.Mock()
        with unittest.mock.patch.object(wph, "get_status", return_value={"hello": "printer"}):
            st = wph.get_status_admin("10.0.0.9", login=login)
        login.assert_not_called()
        self.assertEqual(st, {"hello": "printer"})

    def test_no_login_for_untrusted_host_even_if_redacted(self):
        login = unittest.mock.Mock()
        with unittest.mock.patch.object(wph, "get_status", return_value=_status(ip=None, nm=None, gw=None)):
            wph.get_status_admin("10.0.0.9", login=login, trusted=False)
        login.assert_not_called()

    def test_login_failure_keeps_redacted_view(self):
        with unittest.mock.patch.object(wph, "get_status", return_value=_status(ip=None, nm=None, gw=None)):
            st = wph.get_status_admin("10.0.0.5", login=unittest.mock.Mock(side_effect=RuntimeError("no cred")))
        self.assertIsNone(st["static_ip"])


class _Base(unittest.TestCase):
    def setUp(self):
        self.patches = [
            unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5"),
            unittest.mock.patch.object(mn, "_uart_sta_ip", return_value="10.0.0.5"),
            unittest.mock.patch.object(wph, "probe_host_answers", return_value=(False, "no answer")),
            unittest.mock.patch("kilnctrl.mcp_server_control._profile_or_autotune_running_reason",
                                return_value=None),
        ]
        self.mocks = [p.start() for p in self.patches]
        self.uart = self.mocks[1]
        self.probe = self.mocks[2]
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

    def test_refused_when_uart_has_no_station_address(self):
        self.uart.return_value = None
        with unittest.mock.patch.object(wph, "post_ip_config") as post, \
                unittest.mock.patch.object(wph, "get_status_admin") as get:
            r = mn.network_set_ip_config("dhcp", confirm=True)
        self.assertTrue(r.startswith("refused"), r)
        self.assertIn("UART", r)
        post.assert_not_called()
        get.assert_not_called()

    def test_refused_when_explicit_host_differs_from_uart(self):
        with unittest.mock.patch.object(wph, "post_ip_config") as post, \
                unittest.mock.patch.object(wph, "get_status_admin") as get:
            r = mn.network_set_ip_config("dhcp", confirm=True, host="10.9.9.9")
        self.assertTrue(r.startswith("refused"), r)
        self.assertIn("differs", r)
        post.assert_not_called()
        get.assert_not_called()

    def test_explicit_host_equal_to_uart_is_accepted(self):
        with unittest.mock.patch.object(wph, "get_status_admin", return_value=_status(mode="dhcp", ip="", nm="", gw="")), \
                unittest.mock.patch.object(wph, "post_ip_config") as post:
            r = mn.network_set_ip_config("dhcp", confirm=True, host="10.0.0.5")
        self.assertIn("already DHCP", r)
        post.assert_not_called()

    def test_refused_on_address_conflict_before_anything_is_sent(self):
        self.probe.return_value = (True, "answered HTTP 401")
        with unittest.mock.patch.object(wph, "post_ip_config") as post, \
                unittest.mock.patch.object(wph, "get_status_admin") as get:
            r = mn.network_set_ip_config("static", confirm=True, **GOOD)
        self.assertTrue(r.startswith("refused"), r)
        self.assertIn("already answers", r)
        post.assert_not_called()
        get.assert_not_called()
        self.probe.assert_called_once_with("192.168.1.50")

    def test_no_conflict_probe_when_ip_is_the_current_address(self):
        with unittest.mock.patch.object(wph, "get_status_admin", return_value=_status(mode="dhcp", ip="", nm="", gw="")), \
                unittest.mock.patch.object(wph, "post_ip_config", return_value="dropped"), \
                unittest.mock.patch.object(wph, "verify_ip_config", return_value=(True, "v")):
            mn.network_set_ip_config("static", confirm=True, ip="10.0.0.5", netmask="255.255.255.0",
                                     gateway="10.0.0.1")
        self.probe.assert_not_called()

    def test_refused_mid_run(self):
        self.mocks[3].return_value = "a profile is currently running (#3 'x')"
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
            self.assertIn("is_trusted", kw)
            seen.append(list(resolve_hosts()))
            return True, "verified at x"

        with unittest.mock.patch.object(wph, "get_status_admin",
                                        return_value=_status(mode="dhcp", ip="", nm="", gw="", sta="10.0.0.5")), \
                unittest.mock.patch.object(wph, "post_ip_config", return_value="dropped") as post, \
                unittest.mock.patch.object(wph, "verify_ip_config", side_effect=fake_verify):
            r = mn.network_set_ip_config("static", confirm=True, **GOOD)
        self.assertTrue(r.startswith("ok:"), r)
        self.assertIn("MOVED", r)
        self.assertIn("expected", r)
        self.assertEqual(seen, [["192.168.1.50"]])
        post.assert_called_once()

    def test_dhcp_polls_old_host_and_uart_sta_ip(self):
        seen = []

        def fake_verify(resolve_hosts, *a, **kw):
            self.uart.return_value = "10.0.0.9"  # the board re-leased a new address
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
        self.assertNotIn("expected: Wi-Fi was forced", r)

    def _failed_verify(self, uart_after, after_status):
        before = _status(mode="dhcp", ip="", nm="", gw="")
        reads = iter([before, after_status])

        def verify(*a, **kw):
            self.uart.return_value = uart_after
            return False, "not verified within 90s; last: x"

        with unittest.mock.patch.object(wph, "get_status_admin", side_effect=lambda *a, **k: next(reads)), \
                unittest.mock.patch.object(wph, "post_ip_config", return_value="dropped"), \
                unittest.mock.patch.object(wph, "verify_ip_config", side_effect=verify):
            return mn.network_set_ip_config("static", confirm=True, **GOOD)

    def test_failure_says_board_unchanged_at_old_host(self):
        r = self._failed_verify("10.0.0.5", _status(mode="dhcp", ip="", nm="", gw=""))
        self.assertIn("UNCHANGED", r)
        self.assertIn("10.0.0.5", r)
        self.assertIn("unconfirmed", r)

    def test_failure_says_board_is_elsewhere(self):
        r = self._failed_verify("10.0.0.77", _status(mode="dhcp", ip="", nm="", gw="", sta="10.0.0.77"))
        self.assertIn("board is at 10.0.0.77", r)
        self.assertNotIn("UNCHANGED", r)

    def test_failure_with_no_uart_address_says_not_located(self):
        r = self._failed_verify(None, _status())
        self.assertIn("board not located", r)

    def test_post_timeout_goes_to_verification_not_failure(self):
        with unittest.mock.patch.object(wph, "get_status_admin",
                                        return_value=_status(mode="dhcp", ip="", nm="", gw="")), \
                unittest.mock.patch.object(wph, "post_ip_config", return_value="timeout"), \
                unittest.mock.patch.object(wph, "verify_ip_config", return_value=(True, "verified")) as ver:
            r = mn.network_set_ip_config("static", confirm=True, **GOOD)
        self.assertTrue(r.startswith("ok:"), r)
        ver.assert_called_once()
        self.assertIn("timed out", r)

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


class EndToEndTest(_Base):
    """Real verify_ip_config / get_status_admin / post_ip_config / http_auth.urlopen;
    only the urllib layer and the UART query are faked."""

    def _world(self):
        state = {"mode": "dhcp", "ip": "", "nm": "", "gw": ""}
        log = []

        def status_json(host):
            return ('{"ip_mode": "%s", "static_ip": "%s", "static_netmask": "%s", "static_gateway": "%s", '
                    '"sta_ip": "%s", "sta_connected": true}'
                    % (state["mode"], state["ip"], state["nm"], state["gw"], host))

        def fake_urlopen(req, timeout=None):
            url = req.full_url
            log.append((req.get_method(), url))
            host = url.split("//")[1].split("/")[0]
            if req.get_method() == "POST":
                self.assertEqual(host, "10.0.0.5")
                state.update(mode="static", ip="192.168.1.50", nm="255.255.255.0", gw="192.168.1.1")
                self.uart.return_value = "192.168.1.50"
                raise ConnectionResetError(10054, "reset")
            if host == "10.0.0.5" and state["mode"] == "dhcp":
                return _Resp(status_json(host))
            if host == "192.168.1.50" and state["mode"] == "static":
                return _Resp(status_json(host))
            raise urllib.error.URLError(ConnectionRefusedError(10061, "refused"))

        return state, log, fake_urlopen

    def test_static_change_verified_end_to_end(self):
        state, log, fake = self._world()
        login = unittest.mock.Mock()
        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake), \
                unittest.mock.patch.object(wph.http_auth, "_login", login):
            r = mn.network_set_ip_config("static", confirm=True, **GOOD)
        self.assertTrue(r.startswith("ok: ip configuration verified"), r)
        self.assertIn(("POST", "http://10.0.0.5/ip_config"), log)
        self.assertEqual(log[-1], ("GET", "http://192.168.1.50/status"))
        login.assert_not_called()

    def test_new_address_not_contacted_until_uart_confirms(self):
        # The UART never reports the new address: the board is "somewhere else".
        state, log, fake = self._world()

        def fake_noconfirm(req, timeout=None):
            try:
                return fake(req, timeout)
            finally:
                self.uart.return_value = "10.0.0.5"

        login = unittest.mock.Mock()
        with unittest.mock.patch("urllib.request.urlopen", side_effect=fake_noconfirm), \
                unittest.mock.patch.object(wph.http_auth, "_login", login), \
                unittest.mock.patch.object(wph.http_auth, "login", login):
            r = mn.network_set_ip_config("static", confirm=True, verify_timeout_s=5, **GOOD)
        self.assertTrue(r.startswith("FAILED verification"), r)
        login.assert_not_called()
        self.assertTrue(all("192.168.1.50" not in u for _, u in log), log)


class GetToolTest(_Base):
    def test_no_login_for_host_the_uart_does_not_confirm(self):
        self.uart.return_value = "10.0.0.77"
        with unittest.mock.patch.object(wph, "get_status_admin",
                                        return_value=_status(ip=None, nm=None, gw=None)) as get:
            r = mn.network_get_ip_config()
        self.assertIs(get.call_args.kwargs["trusted"], False)
        self.assertIn("no login", r)

    def test_login_allowed_for_uart_confirmed_host(self):
        with unittest.mock.patch.object(wph, "get_status_admin", return_value=_status()) as get:
            mn.network_get_ip_config()
        self.assertIs(get.call_args.kwargs["trusted"], True)


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



class DnsTest(unittest.TestCase):
    def test_validate(self):
        self.assertIsNone(wph.validate_ip_config("static", dns="1.1.1.1", **GOOD))
        self.assertIsNone(wph.validate_ip_config("static", dns="1.1.1.1", dns2="8.8.8.8", **GOOD))
        for kw in (dict(dns="1.1.1"), dict(dns="256.1.1.1"), dict(dns="0.0.0.0"), dict(dns="127.0.0.1"),
                   dict(dns="224.0.0.1"), dict(dns="01.1.1.1"), dict(dns=7),
                   dict(dns="1.1.1.1", dns2="bogus"), dict(dns2="8.8.8.8")):
            self.assertIsNotNone(wph.validate_ip_config("static", **GOOD, **kw), kw)
        self.assertIsNotNone(wph.validate_ip_config("dhcp", dns="1.1.1.1"))
        self.assertIsNotNone(wph.validate_ip_config("dhcp", dns2="8.8.8.8"))

    def test_body_and_cap(self):
        self.assertEqual(wph.build_ip_config_body("static", dns="1.1.1.1", **GOOD),
                         b"mode=static&ip=192.168.1.50&netmask=255.255.255.0&gateway=192.168.1.1&dns=1.1.1.1")
        self.assertTrue(wph.build_ip_config_body("static", dns="1.1.1.1", dns2="8.8.8.8", **GOOD)
                        .endswith(b"&dns=1.1.1.1&dns2=8.8.8.8"))
        worst = wph.build_ip_config_body("static", ip="255.255.255.255", netmask="255.255.255.255",
                                         gateway="255.255.255.255", dns="255.255.255.255", dns2="255.255.255.255")
        self.assertLessEqual(len(worst), wph.IP_CONFIG_BODY_MAX)

    def test_matches(self):
        st = _status()
        st.update(static_dns="1.1.1.1", static_dns2="")
        self.assertTrue(wph._matches(st, "static", GOOD["ip"], GOOD["netmask"], GOOD["gateway"], "1.1.1.1")[0])
        ok, why = wph._matches(st, "static", GOOD["ip"], GOOD["netmask"], GOOD["gateway"])
        self.assertFalse(ok)  # dns left over when none requested
        self.assertIn("static_dns", why)
        ok, why = wph._matches(st, "static", GOOD["ip"], GOOD["netmask"], GOOD["gateway"], "1.1.1.1", "8.8.8.8")
        self.assertFalse(ok)
        self.assertIn("static_dns2", why)
        st.update(static_dns=None, static_dns2=None)
        self.assertIn("redacted", wph._matches(st, "static", GOOD["ip"], GOOD["netmask"], GOOD["gateway"])[1])

    def test_matches_older_firmware_without_dns_keys(self):
        st = _status()
        self.assertNotIn("static_dns", st)
        self.assertTrue(wph._matches(st, "static", GOOD["ip"], GOOD["netmask"], GOOD["gateway"])[0])
        ok, why = wph._matches(st, "static", GOOD["ip"], GOOD["netmask"], GOOD["gateway"], "1.1.1.1")
        self.assertFalse(ok)
        self.assertIn("predates", why)

    def test_post_sends_dns(self):
        with unittest.mock.patch.object(wph.http_auth, "urlopen", return_value=_Resp("ok")) as m:
            wph.post_ip_config("10.0.0.5", "static", dns="1.1.1.1", dns2="8.8.8.8", **GOOD)
        req = m.call_args[0][0]
        self.assertTrue(req.data.endswith(b"&dns=1.1.1.1&dns2=8.8.8.8"), req.data)

    def test_describe_shows_dns_only_when_reported(self):
        st = _status()
        self.assertNotIn("static_dns", mn._describe_status(st))
        st.update(static_dns="1.1.1.1", static_dns2="")
        d = mn._describe_status(st)
        self.assertIn("static_dns='1.1.1.1'", d)
        self.assertIn("static_dns2=''", d)

    def test_omitted_dns_not_sent(self):
        # Firmware (wifi_provision_http.c, http_form_find_field(body, "dns"/"dns2")):
        # absent == unset, so an omitted dns must not appear in the body at all
        # (not as "dns=" and not as "0.0.0.0").
        body = wph.build_ip_config_body("static", **GOOD)
        self.assertEqual(body, b"mode=static&ip=192.168.1.50&netmask=255.255.255.0&gateway=192.168.1.1")
        self.assertNotIn(b"dns", body)
        self.assertNotIn(b"0.0.0.0", body)
        self.assertNotIn(b"dns2", wph.build_ip_config_body("static", dns="1.1.1.1", **GOOD))
        with unittest.mock.patch.object(wph.http_auth, "urlopen", return_value=_Resp("ok")) as m:
            wph.post_ip_config("10.0.0.5", "static", **GOOD)
        self.assertNotIn(b"dns", m.call_args[0][0].data)

    def test_field_names_and_dns2_alone_refused_client_side(self):
        # Field names match the C parser's "dns" / "dns2" keys; dns2 without dns is
        # refused client-side (firmware re-validates the same rule).
        body = wph.build_ip_config_body("static", dns="1.1.1.1", dns2="8.8.8.8", **GOOD)
        fields = dict(kv.split(b"=", 1) for kv in body.split(b"&"))
        self.assertEqual(fields[b"dns"], b"1.1.1.1")
        self.assertEqual(fields[b"dns2"], b"8.8.8.8")
        with unittest.mock.patch.object(wph.http_auth, "urlopen") as m:
            r = mn.network_set_ip_config("static", confirm=True, dns2="8.8.8.8", **GOOD)
        self.assertTrue(r.startswith("refused:"), r)
        self.assertIn("dns2 requires dns", r)
        m.assert_not_called()


class DnsToolTest(_Base):
    def test_dns_flows_to_post_and_verify(self):
        seen = {}

        def fake_verify(resolve_hosts, mode, ip, nm, gw, **kw):
            seen["verify"] = (kw.get("dns"), kw.get("dns2"))
            return True, "verified at x"

        with unittest.mock.patch.object(wph, "get_status_admin",
                                        return_value=_status(mode="dhcp", ip="", nm="", gw="", sta="10.0.0.5")), \
                unittest.mock.patch.object(wph, "post_ip_config", return_value="dropped") as post, \
                unittest.mock.patch.object(wph, "verify_ip_config", side_effect=fake_verify):
            r = mn.network_set_ip_config("static", confirm=True, dns="1.1.1.1", dns2="8.8.8.8", **GOOD)
        self.assertTrue(r.startswith("ok:"), r)
        self.assertEqual(post.call_args.kwargs, dict(dns="1.1.1.1", dns2="8.8.8.8"))
        self.assertEqual(seen["verify"], ("1.1.1.1", "8.8.8.8"))

    def test_bad_dns_refused_before_any_io(self):
        with unittest.mock.patch.object(wph, "post_ip_config") as post:
            r = mn.network_set_ip_config("static", confirm=True, dns2="8.8.8.8", **GOOD)
        self.assertTrue(r.startswith("refused:"), r)
        post.assert_not_called()

    def test_already_configured_includes_dns(self):
        st = _status(sta="192.168.1.50")
        st.update(static_dns="1.1.1.1", static_dns2="")
        with unittest.mock.patch.object(wph, "get_status_admin", return_value=st), \
                unittest.mock.patch.object(wph, "post_ip_config") as post:
            r = mn.network_set_ip_config("static", confirm=True, dns="1.1.1.1", **GOOD)
        self.assertIn("already configured", r)
        post.assert_not_called()
        with unittest.mock.patch.object(wph, "get_status_admin", return_value=st), \
                unittest.mock.patch.object(wph, "post_ip_config", return_value="dropped") as post, \
                unittest.mock.patch.object(wph, "verify_ip_config", return_value=(True, "v")):
            mn.network_set_ip_config("static", confirm=True, **GOOD)  # dns dropped -> a real change
        post.assert_called_once()


if __name__ == "__main__":
    unittest.main()
