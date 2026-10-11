#!/usr/bin/env python3
"""Unit tests for mcp_server_aux (control_get_aux_outputs, control_set_aux_output,
control_set_aux_manual) against a fake board: aux_http_client's three functions
and the io read are mocked; no socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_mcp_server_aux.py -q
"""
from __future__ import annotations

import copy
import os
import sys
import unittest
import unittest.mock
import urllib.error
import io as _io

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import aux_http_client as ahc  # noqa: E402
from kilnctrl import mcp_server_aux as ma  # noqa: E402
from kilnctrl import mcp_server_ota  # noqa: E402

GATE_TEXT = "refused -- a firing or autotune run is active"


def _entry(relay, **kw):
    e = {"relay": relay, "enabled": False, "conflicted": False, "tc_zone": -1,
         "hyst_c": 2.0, "min_on_s": 10, "min_off_s": 20}
    e.update(kw)
    return e


def _snap(by_relay=None, zones_mask=0b0111):
    by_relay = by_relay or {}
    return {"quarantined": False, "enabled_mask": 0, "conflict_mask": 0, "zones_relay_mask": zones_mask,
            "relays": [_entry(r, **by_relay.get(r, {})) for r in (1, 2, 3, 4)]}


class _Base(unittest.TestCase):
    def setUp(self):
        self._idle = unittest.mock.patch.object(ma, "_running_reason", return_value=None)
        self._host = unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", return_value="10.0.0.5")
        self._idle.start()
        self._host.start()
        self.addCleanup(self._idle.stop)
        self.addCleanup(self._host.stop)


class SetAuxOutputTest(_Base):
    def _run(self, before, after, **kw):
        with unittest.mock.patch.object(ahc, "get_aux_outputs", side_effect=[before, after]), \
             unittest.mock.patch.object(ahc, "post_aux_output", return_value=True) as post:
            r = ma.control_set_aux_output(**kw)
        return r, post

    def test_happy_path(self):
        before = _snap()
        after = copy.deepcopy(before)
        after["relays"][3].update(enabled=True, tc_zone=0, hyst_c=3.5)
        r, post = self._run(before, after, relay=4, enabled=True, tc_zone=0, hyst_c=3.5, confirm=True)
        self.assertTrue(r.startswith("ok"), r)
        post.assert_called_once_with("10.0.0.5", 4, True, 0, 3.5, None, None)

    def test_confirm_gate_exactly_true(self):
        for bad in (False, 1, "yes", None):
            with unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=_snap()), \
                 unittest.mock.patch.object(ahc, "post_aux_output") as post:
                r = ma.control_set_aux_output(relay=4, enabled=True, confirm=bad)
            self.assertIn("DRY RUN", r, (bad, r))
            post.assert_not_called()

    def test_arg_validation_before_any_io(self):
        with unittest.mock.patch.object(ahc, "get_aux_outputs") as get:
            for kw in ({"relay": 0}, {"relay": 5}, {"relay": True}, {"relay": 4, "tc_zone": -2},
                       {"relay": 4, "hyst_c": float("nan")}, {"relay": 4, "min_on_s": -1}):
                kw.setdefault("relay", 4)
                r = ma.control_set_aux_output(enabled=True, confirm=True, **kw)
                self.assertTrue(r.startswith("refused"), (kw, r))
            r = ma.control_set_aux_output(relay=4, enabled="yes", confirm=True)
            self.assertTrue(r.startswith("refused"), r)
        get.assert_not_called()

    def test_refuses_mid_run_precheck(self):
        with unittest.mock.patch.object(ma, "_running_reason", return_value="a profile is currently running"), \
             unittest.mock.patch.object(ahc, "get_aux_outputs") as get, \
             unittest.mock.patch.object(ahc, "post_aux_output") as post:
            r = ma.control_set_aux_output(relay=4, enabled=True, confirm=True)
        self.assertTrue(r.startswith("refused"), r)
        get.assert_not_called()
        post.assert_not_called()

    def test_409_gate_is_a_refusal(self):
        exc = ahc.AuxHttpError("x", 409, GATE_TEXT)
        with unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=_snap()), \
             unittest.mock.patch.object(ahc, "post_aux_output", side_effect=exc):
            r = ma.control_set_aux_output(relay=4, enabled=True, confirm=True)
        self.assertTrue(r.startswith("refused: system_mode_gate"), r)

    def test_refuses_relay_used_by_a_zone(self):
        with unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=_snap(zones_mask=0b0001)), \
             unittest.mock.patch.object(ahc, "post_aux_output") as post:
            r = ma.control_set_aux_output(relay=1, enabled=True, confirm=True)
        self.assertTrue(r.startswith("refused"), r)
        self.assertIn("zone", r)
        post.assert_not_called()

    def test_unbind_allowed_on_zone_relay(self):
        before = _snap({1: {"enabled": True, "conflicted": True}}, zones_mask=0b0001)
        after = copy.deepcopy(before)
        after["relays"][0].update(enabled=False)
        r, _ = self._run(before, after, relay=1, enabled=False, confirm=True)
        self.assertTrue(r.startswith("ok"), r)

    def test_refuses_quarantined_store(self):
        s = _snap()
        s["quarantined"] = True
        with unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=s), \
             unittest.mock.patch.object(ahc, "post_aux_output") as post:
            r = ma.control_set_aux_output(relay=4, enabled=True, confirm=True)
        self.assertTrue(r.startswith("refused"), r)
        post.assert_not_called()

    def test_readback_mismatch_fails_loud(self):
        before = _snap()
        after = copy.deepcopy(before)  # board "accepted" but nothing changed
        r, _ = self._run(before, after, relay=4, enabled=True, confirm=True)
        self.assertTrue(r.startswith("FAILED"), r)

    def test_collateral_other_relay_fails_loud(self):
        before = _snap()
        after = copy.deepcopy(before)
        after["relays"][3].update(enabled=True)
        after["relays"][2].update(hyst_c=9.0)
        r, _ = self._run(before, after, relay=4, enabled=True, confirm=True)
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("relay 3", r)

    def test_omitted_field_not_preserved_fails_loud(self):
        before = _snap()
        after = copy.deepcopy(before)
        after["relays"][3].update(enabled=True, min_on_s=0)
        r, _ = self._run(before, after, relay=4, enabled=True, confirm=True)
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("min_on_s", r)


class SetAuxManualTest(_Base):
    def _io(self, relay_on, unknown=False, i2c=False):
        st = unittest.mock.Mock()
        st.relay.side_effect = lambda r: relay_on
        st.relay_state_unknown = unknown
        st.i2c_failed = i2c
        return st

    def _run(self, snap, shadow_on, unknown=False, i2c=False, uart_ip="10.0.0.5", **kw):
        wifi = unittest.mock.Mock(connected=True, ip=uart_ip)
        with unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=snap), \
             unittest.mock.patch.object(ahc, "post_aux_manual", return_value=True) as post, \
             unittest.mock.patch.object(ma._srv._info, "get_wifi_status", return_value=wifi), \
             unittest.mock.patch.object(ma._srv._io, "read", return_value=self._io(shadow_on, unknown, i2c)):
            r = ma.control_set_aux_manual(**kw)
        return r, post

    def test_happy_path(self):
        r, post = self._run(_snap({4: {"enabled": True}}), True, relay=4, on=True, confirm=True)
        self.assertTrue(r.startswith("ok"), r)
        post.assert_called_once_with("10.0.0.5", 4, True)

    def test_confirm_gate_exactly_true(self):
        for bad in (False, 1, "yes", None):
            r, post = self._run(_snap({4: {"enabled": True}}), True, relay=4, on=True, confirm=bad)
            self.assertIn("DRY RUN", r, (bad, r))
            post.assert_not_called()

    def test_refuses_not_enabled_aux(self):
        r, post = self._run(_snap(), True, relay=4, on=True, confirm=True)
        self.assertTrue(r.startswith("refused"), r)
        post.assert_not_called()

    def test_refuses_mid_run_precheck(self):
        with unittest.mock.patch.object(ma, "_running_reason", return_value="a profile is currently running"), \
             unittest.mock.patch.object(ahc, "post_aux_manual") as post:
            r = ma.control_set_aux_manual(relay=4, on=True, confirm=True)
        self.assertTrue(r.startswith("refused"), r)
        post.assert_not_called()

    def test_409_mid_run_is_a_refusal(self):
        exc = ahc.AuxHttpError("x", 409, GATE_TEXT)
        with unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=_snap({4: {"enabled": True}})), \
             unittest.mock.patch.object(ma._srv._info, "get_wifi_status",
                                        return_value=unittest.mock.Mock(connected=True, ip="10.0.0.5")), \
             unittest.mock.patch.object(ahc, "post_aux_manual", side_effect=exc):
            r = ma.control_set_aux_manual(relay=4, on=True, confirm=True)
        self.assertTrue(r.startswith("refused: system_mode_gate"), r)

    def test_shadow_mismatch_fails_loud(self):
        r, _ = self._run(_snap({4: {"enabled": True}}), False, relay=4, on=True, confirm=True)
        self.assertTrue(r.startswith("FAILED"), r)

    def test_unknown_flag_off_fails(self):
        r, _ = self._run(_snap({4: {"enabled": True}}), False, unknown=True, relay=4, on=False, confirm=True)
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("relay_state_unknown", r)

    def test_i2c_failed_fails(self):
        r, _ = self._run(_snap({4: {"enabled": True}}), True, i2c=True, relay=4, on=True, confirm=True)
        self.assertTrue(r.startswith("FAILED"), r)
        self.assertIn("i2c_failed", r)

    def test_board_identity_mismatch_refuses_before_post(self):
        r, post = self._run(_snap({4: {"enabled": True}}), True, uart_ip="10.0.0.9", relay=4, on=True, confirm=True)
        self.assertTrue(r.startswith("refused"), r)
        self.assertIn("10.0.0.9", r)
        post.assert_not_called()

    def test_name_is_resolved_once_and_post_pinned_to_the_ip(self):
        import socket
        calls = []

        def fake_gai(host, *a, **k):
            calls.append(host)
            return [(socket.AF_INET, 0, 0, "", ("10.0.0.5", 0))]
        with unittest.mock.patch.object(socket, "getaddrinfo", side_effect=fake_gai),              unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", side_effect=lambda x: x):
            r, post = self._run(_snap({4: {"enabled": True}}), True, relay=4, on=True, confirm=True,
                                host="kiln.local:8080")
        self.assertTrue(r.startswith("ok"), r)
        post.assert_called_once_with("10.0.0.5:8080", 4, True)
        self.assertEqual(calls, ["kiln.local"])

    def test_name_with_a_second_record_refuses(self):
        import socket
        recs = [(socket.AF_INET, 0, 0, "", ("10.0.0.5", 0)), (socket.AF_INET, 0, 0, "", ("10.0.0.99", 0))]
        with unittest.mock.patch.object(socket, "getaddrinfo", return_value=recs),              unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", side_effect=lambda x: x):
            r, post = self._run(_snap({4: {"enabled": True}}), True, relay=4, on=True, confirm=True,
                                host="kiln.local")
        self.assertTrue(r.startswith("refused"), r)
        self.assertIn("10.0.0.99", r)
        post.assert_not_called()

    def _run_wifi(self, wifi=None, exc=None, **kw):
        with unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=_snap({4: {"enabled": True}})), \
             unittest.mock.patch.object(ahc, "post_aux_manual", return_value=True) as post, \
             unittest.mock.patch.object(ma._srv._info, "get_wifi_status", return_value=wifi, side_effect=exc), \
             unittest.mock.patch.object(ma._srv._io, "read", return_value=self._io(True)):
            r = ma.control_set_aux_manual(relay=4, on=True, confirm=True, **kw)
        return r, post

    def test_not_connected_refuses_before_post(self):
        r, post = self._run_wifi(unittest.mock.Mock(connected=False, ip="10.0.0.5"))
        self.assertTrue(r.startswith("refused"), r)
        post.assert_not_called()

    def test_wifi_exception_refuses_not_matches(self):
        r, post = self._run_wifi(exc=RuntimeError("uart down"))
        self.assertTrue(r.startswith("refused"), r)
        self.assertIn("uart down", r)
        post.assert_not_called()

    def test_ap_mode_no_ip_refuses(self):
        r, post = self._run_wifi(unittest.mock.Mock(connected=True, ip=""))
        self.assertTrue(r.startswith("refused"), r)
        self.assertIn("AP-fallback", r)
        post.assert_not_called()

    def test_host_normalization_accepts_scheme_port(self):
        for h in ("http://10.0.0.5", "10.0.0.5:80", "http://10.0.0.5:8080/x"):
            r, post = self._run_wifi(unittest.mock.Mock(connected=True, ip="10.0.0.5"), host=h)
            self.assertTrue(r.startswith("ok"), (h, r))
            post.assert_called_once()

    def test_unresolvable_host_refuses(self):
        with unittest.mock.patch.object(mcp_server_ota, "_ota_resolve_host", side_effect=lambda x: x):
            r, post = self._run_wifi(unittest.mock.Mock(connected=True, ip="10.0.0.5"), host="no-such-host.invalid")
        self.assertTrue(r.startswith("refused"), r)
        post.assert_not_called()

    def test_bad_args(self):
        for kw in ({"relay": 0, "on": True}, {"relay": 4, "on": 1}):
            r = ma.control_set_aux_manual(confirm=True, **kw)
            self.assertTrue(r.startswith("refused"), (kw, r))


class GetAuxOutputsTest(_Base):
    def test_renders(self):
        with unittest.mock.patch.object(ahc, "get_aux_outputs", return_value=_snap({4: {"enabled": True}})):
            r = ma.control_get_aux_outputs()
        self.assertIn("relay 4: ENABLED", r)

    def test_error(self):
        with unittest.mock.patch.object(ahc, "get_aux_outputs", side_effect=ahc.AuxHttpError("boom")):
            self.assertTrue(ma.control_get_aux_outputs().startswith("error"))


class ClientWireTest(unittest.TestCase):
    def _capture(self, reply=b'{"ok":true}'):
        seen = {}

        def fake(req, timeout=None):
            seen["url"] = req.full_url
            seen["data"] = req.data
            resp = unittest.mock.MagicMock()
            resp.__enter__.return_value.read.return_value = reply
            return resp
        return seen, fake

    def test_post_omits_unset_fields(self):
        seen, fake = self._capture()
        with unittest.mock.patch.object(ahc.http_auth, "urlopen", side_effect=fake):
            self.assertTrue(ahc.post_aux_output("h", 4, True, tc_zone=1))
        self.assertEqual(seen["url"], "http://h/api/aux_outputs")
        self.assertEqual(seen["data"], b"relay=4&enabled=1&tc_zone=1")

    def test_manual_wire(self):
        seen, fake = self._capture()
        with unittest.mock.patch.object(ahc.http_auth, "urlopen", side_effect=fake):
            self.assertTrue(ahc.post_aux_manual("h", 2, False))
        self.assertEqual(seen["url"], "http://h/api/aux_outputs/manual")
        self.assertEqual(seen["data"], b"relay=2&on=0")

    def test_http_error_carries_status_and_detail(self):
        err = urllib.error.HTTPError("u", 409, "Conflict", {}, _io.BytesIO(GATE_TEXT.encode()))
        with unittest.mock.patch.object(ahc.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ahc.AuxHttpError) as cm:
                ahc.post_aux_manual("h", 2, True)
        self.assertEqual(cm.exception.status, 409)
        self.assertIn("firing", cm.exception.detail)


if __name__ == "__main__":
    unittest.main()
