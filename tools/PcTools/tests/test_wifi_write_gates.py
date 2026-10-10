"""wifi_add_network/set_mode/forget: confirm gate, mid-run gate, read-back."""
from __future__ import annotations

import os
import sys
import types
import unittest
import unittest.mock as um

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import mcp_server_wifi as w  # noqa: E402


def _res(ok=True, reason=""):
    return types.SimpleNamespace(ok=ok, reason=reason)


class _Fake:
    def __init__(self, nets=(), mode="home", ok=True):
        self.nets = list(nets)
        self.mode = mode
        self.ok = ok
        self.calls = []

    def add_network(self, ssid, pw):
        self.calls.append(("add", ssid))
        if self.ok:
            self.nets.append(ssid)
        return _res(self.ok)

    def forget(self, ssid):
        self.calls.append(("forget", ssid))
        if self.ok and ssid in self.nets:
            self.nets.remove(ssid)
        return _res(self.ok)

    def set_mode(self, m):
        self.calls.append(("mode", m))
        return _res(self.ok)

    def get_networks(self):
        return [types.SimpleNamespace(ssid=s) for s in self.nets], False

    def get_status(self):
        return types.SimpleNamespace(mode_name=self.mode)


class WifiGateTests(unittest.TestCase):
    def _srv(self, fake, state=0):
        prof = types.SimpleNamespace(get_exec_status=lambda timeout=2.0: types.SimpleNamespace(
            state=state, state_name="RUNNING"))
        return um.patch.multiple(w._srv, _wifi=fake, _profiles=prof, create=True)

    def test_unconfirmed_refused_no_call(self):
        f = _Fake()
        with self._srv(f):
            for out in (w.wifi_add_network("a", "p"), w.wifi_set_mode("ap"), w.wifi_forget("a"),
                        w.wifi_forget("a", confirm="yes")):
                self.assertIn("confirm=True", out)
        self.assertEqual(f.calls, [])

    def test_refused_while_running_override(self):
        f = _Fake(nets=["a"])
        with self._srv(f, state=1):
            self.assertIn("while a profile", w.wifi_forget("a", confirm=True))
            self.assertEqual(f.calls, [])
            self.assertTrue(w.wifi_forget("a", confirm=True, allow_running=True).startswith("ok"))

    def test_add_readback(self):
        f = _Fake()
        with self._srv(f):
            self.assertTrue(w.wifi_add_network("a", "p", confirm=True).startswith("ok"))
        f2 = _Fake()
        f2.add_network = lambda s, p: _res(True)  # board lies: never stored
        with self._srv(f2):
            self.assertIn("FAILED", w.wifi_add_network("a", "p", confirm=True))

    def test_mode_readback_mismatch(self):
        f = _Fake(mode="home")
        with self._srv(f):
            self.assertIn("FAILED", w.wifi_set_mode("ap", confirm=True))

    def test_forget_refusal_not_mislabelled(self):
        f = _Fake(ok=False)
        with self._srv(f):
            out = w.wifi_forget("a", confirm=True)
        self.assertTrue(out.startswith("refused"))
        self.assertNotIn("no such saved network", out)

    def test_unreadable_executor_refuses_and_override_works(self):
        f = _Fake(nets=["a"])
        def boom(timeout=2.0):
            raise OSError("down")
        with um.patch.multiple(w._srv, _wifi=f, _profiles=types.SimpleNamespace(get_exec_status=boom),
                               create=True):
            self.assertIn("could not be read", w.wifi_forget("a", confirm=True))
            self.assertEqual(f.calls, [])
            self.assertTrue(w.wifi_forget("a", confirm=True, allow_running=True).startswith("ok"))

    def test_executor_no_answer_refuses(self):
        f = _Fake(nets=["a"])
        with um.patch.multiple(w._srv, _wifi=f, _profiles=types.SimpleNamespace(
                get_exec_status=lambda timeout=2.0: None), create=True):
            self.assertIn("could not be read", w.wifi_forget("a", confirm=True))
        self.assertEqual(f.calls, [])

    def test_forget_unreadable_readback_fails(self):
        f = _Fake(nets=["a"])
        f.get_networks = lambda: (_ for _ in ()).throw(OSError("x"))
        with self._srv(f):
            self.assertIn("FAILED", w.wifi_forget("a", confirm=True))

    def test_status_explicit_host_untrusted(self):
        st = types.SimpleNamespace(ap_password="", mode_name="home", state_name="x", sta_connected=False,
                                   ssid="", ap_ssid="", sta_ip="", sta_rssi=0, ap_clients=0)
        with um.patch.multiple(w._srv, _wifi=types.SimpleNamespace(get_status=lambda: st), create=True), \
                um.patch.object(w.wifi_prov_http_client, "get_status", return_value={}) as g:
            w.wifi_get_status(host="1.2.3.4")
        self.assertIs(g.call_args.kwargs["trusted"], False)


if __name__ == "__main__":
    unittest.main()
