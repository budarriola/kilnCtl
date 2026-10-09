#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices.safety_set_config()/SAFETY_TC_TYPE_NAMES --
the PC-facing side of SAFETY_CMD_SET_CONFIG (0x16, CommonFW/docs/LINK_PROTOCOL.md
sec 4), and mcp_server.safety_set_tc_type()'s name-to-byte resolution.

No real UART/serial connection is used -- this only checks the byte-exact
wire encoding and the human-readable-name mapping, mirroring the byte-exact
vector convention firmware/CommonFW/test uses for the same codec on the
firmware side (test/vectors/set_config_vectors.json).

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import devices  # noqa: E402
from kilnctrl import mcp_server  # noqa: E402
from kilnctrl.protocol import SAFETY_CMD_SET_CONFIG  # noqa: E402


class SafetySetConfigEncodeTests(unittest.TestCase):
    def test_tc_type_k_matches_commonfw_vector(self):
        # firmware/CommonFW/test/vectors/set_config_vectors.json's "tc_type_k"
        # vector: cmd 0x16, tc_type 3 -> bytes 1603.
        self.assertEqual(devices.safety_set_config(0x03), bytes([0x16, 0x03]))

    def test_tc_type_zero(self):
        self.assertEqual(devices.safety_set_config(0x00), bytes([SAFETY_CMD_SET_CONFIG, 0x00]))

    def test_tc_type_max_nibble(self):
        self.assertEqual(devices.safety_set_config(0x0F), bytes([SAFETY_CMD_SET_CONFIG, 0x0F]))

    def test_tc_type_out_of_range_rejected(self):
        # Wire field is the MAX31856 CR1 TC[3:0] nibble -- 0x10 can never be
        # a legal register value, so this must be refused locally rather
        # than silently sent.
        with self.assertRaises(ValueError):
            devices.safety_set_config(0x10)

    def test_tc_type_negative_rejected(self):
        with self.assertRaises(ValueError):
            devices.safety_set_config(-1)


class SafetyTcTypeNameTableTests(unittest.TestCase):
    def test_all_eight_names_present(self):
        self.assertEqual(
            set(devices.SAFETY_TC_TYPE_NAMES),
            {"B", "E", "J", "K", "N", "R", "S", "T"},
        )

    def test_k_is_three_matching_max31856_tc_type_k(self):
        # firmware/SaftyFW/src/max31856.h: MAX31856_TC_TYPE_K = 0x03.
        self.assertEqual(devices.SAFETY_TC_TYPE_NAMES["K"], 0x03)

    def test_values_are_unique(self):
        values = list(devices.SAFETY_TC_TYPE_NAMES.values())
        self.assertEqual(len(values), len(set(values)), "no two names share a wire value")


def _idle_profile():
    return unittest.mock.MagicMock(state=0, state_name="idle", profile_id=0, name="")


def _idle_autotune():
    return unittest.mock.MagicMock(state=0, state_name="idle", zone=0)


class SafetySetTcTypeToolTests(unittest.TestCase):
    # 2026-09-17: safety_set_tc_type() no longer sends the raw fire-and-forget
    # SAFETY_CMD_SET_CONFIG wire command (that reply carries no proof the
    # write landed -- see devices.safety_set_config()'s docstring). It now
    # writes the identical config_store `tc_type` field over
    # GET/POST /api/safety/commissioning, the same generically-verifying
    # path safety_set_rate_guard()/safety_set_commissioning_fields() use, so
    # these tests mock safety_cfg_http_client.apply_safety_fields directly
    # (test_safety_rate_guard.py's own convention) instead of
    # SafetyClient.set_config.
    def setUp(self):
        self._patches = [
            unittest.mock.patch.object(mcp_server._profiles, "get_exec_status", return_value=_idle_profile()),
            unittest.mock.patch.object(mcp_server._autotune, "get_status", return_value=_idle_autotune()),
        ]
        for p in self._patches:
            p.start()
            self.addCleanup(p.stop)

    def test_known_name_writes_expected_field(self):
        from kilnctrl import safety_cfg_http_client as sc

        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(ok=True, confirmed=["tc_type"], commissioned_after=True),
        ) as mock_apply:
            result = mcp_server.safety_set_tc_type("K", confirm=True)
        self.assertTrue(result.startswith("ok"))
        self.assertIn("tc_type=3", result)
        (host, fields), kwargs = mock_apply.call_args
        self.assertEqual(fields, {"tc_type": 0x03})
        self.assertTrue(kwargs.get("verify", True))

    def test_lowercase_name_accepted(self):
        from kilnctrl import safety_cfg_http_client as sc

        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(ok=True, confirmed=["tc_type"]),
        ) as mock_apply:
            mcp_server.safety_set_tc_type("k", confirm=True)
        (host, fields), _kwargs = mock_apply.call_args
        self.assertEqual(fields, {"tc_type": 0x03})

    def test_unknown_name_never_reaches_the_wire(self):
        from kilnctrl import safety_cfg_http_client as sc

        with unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            result = mcp_server.safety_set_tc_type("not-a-type")
        mock_apply.assert_not_called()
        self.assertTrue(result.startswith("error:"))

    def test_readback_mismatch_reported_as_failure_not_success(self):
        # The regression this pass fixes: the old wire path had no reply to
        # even be wrong about -- "outcome shows up on the Pico's log, not
        # here" was the tool's own prior docstring. Now a write that does
        # not actually land must be reported as a failure, not "ok".
        from kilnctrl import safety_cfg_http_client as sc

        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(
                ok=False, post_reason="",
                mismatches=["tc_type: expected 3, board reports 0"],
            ),
        ):
            result = mcp_server.safety_set_tc_type("K", confirm=True)
        self.assertTrue(result.startswith("failed"))
        self.assertIn("tc_type", result)

    def test_armed_rejection_names_the_grace_window_fix(self):
        from kilnctrl import safety_cfg_http_client as sc

        with unittest.mock.patch.object(
            sc, "apply_safety_fields",
            return_value=sc.SafetyApplyResult(
                ok=False,
                post_reason="commit rejected: tc_type (id 261) -- relay is ARMED -- "
                            "config writes are refused while ARMED -- values were staged but NOT written",
            ),
        ):
            result = mcp_server.safety_set_tc_type("K", confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn('debug_reset(peer="pico")', result)
        self.assertIn("60 seconds", result)

    def test_http_error_surfaces_as_error_string_not_exception(self):
        from kilnctrl import safety_cfg_http_client as sc

        with unittest.mock.patch.object(
            sc, "apply_safety_fields", side_effect=sc.SafetyCfgHttpError("unreachable")
        ):
            result = mcp_server.safety_set_tc_type("K", confirm=True)
        self.assertTrue(result.startswith("error"))

    def test_refused_while_profile_running(self):
        from kilnctrl import safety_cfg_http_client as sc

        running = unittest.mock.MagicMock(state=1, state_name="running", profile_id=3, name="Cone 6")
        with unittest.mock.patch.object(mcp_server._profiles, "get_exec_status", return_value=running), \
             unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            result = mcp_server.safety_set_tc_type("K", confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("running", result)
        mock_apply.assert_not_called()

    def test_refused_while_autotune_running(self):
        from kilnctrl import safety_cfg_http_client as sc

        at = unittest.mock.MagicMock(state=3, state_name="relay_approach", zone=1)
        with unittest.mock.patch.object(mcp_server._autotune, "get_status", return_value=at), \
             unittest.mock.patch.object(sc, "apply_safety_fields") as mock_apply:
            result = mcp_server.safety_set_tc_type("K", confirm=True)
        self.assertTrue(result.startswith("refused"))
        self.assertIn("autotune", result)
        mock_apply.assert_not_called()


if __name__ == "__main__":
    unittest.main()
