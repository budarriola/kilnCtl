#!/usr/bin/env python3
"""Unit tests for kilnctrl.devices_safety's SAFETY_CMD_GET_PARAM (0x23) /
SAFETY_CMD_PARAM (0x1E) reply parsing, SafetyClient.get_param(), and
mcp_server.safety_get_param() -- modelled on test_safety_ct_cal.py.

Unlike GET_CT_CAL, uart_bridge_safety.c's safety_bridge_task() has no case
for SAFETY_CMD_GET_PARAM at all as of this writing, so every real reply
today is an "unsupported" refusal carrying the REQUEST's own id (0x23) back,
never SAFETY_CMD_PARAM (0x1E) -- see protocol.py's SAFETY_CMD_GET_PARAM doc
comment and devices_safety.py's parse branch for both ids. This suite
therefore also exercises the not-yet-reachable "found"/"not found" decode
path directly against devices.parse_safety_response(), since a hand-built
0x1E vector is the only way to reach it before firmware wiring lands.

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
from kilnctrl.protocol import (  # noqa: E402
    SAFETY_CMD_GET_PARAM,
    SAFETY_CMD_PARAM,
)


class SafetyGetParamIdSeparationTests(unittest.TestCase):
    """The _REPLY_ID_FOR_REQUEST mapping this whole PARAM/GET_PARAM split
    depends on: 0x23 (request) must map to 0x1E (reply), same shape as
    GET_CT_CAL's 0x22 -> 0x1A entry."""

    def test_request_and_reply_ids_differ(self):
        self.assertNotEqual(SAFETY_CMD_GET_PARAM, SAFETY_CMD_PARAM)

    def test_reply_id_for_request_maps_get_param_to_param(self):
        from kilnctrl.safety import _REPLY_ID_FOR_REQUEST

        self.assertEqual(_REPLY_ID_FOR_REQUEST[SAFETY_CMD_GET_PARAM], SAFETY_CMD_PARAM)


class SafetyParamReplyParseTests(unittest.TestCase):
    """SAFETY_CMD_PARAM (0x1E) reply parsing via devices.parse_safety_response()."""

    def test_found_bool_reply_decodes(self):
        vector = bytes([SAFETY_CMD_PARAM, 0x05, 0x00, 0x01, 0x00, 0x01])
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_PARAM)
        self.assertIsInstance(value, devices.SafetyGetParam)
        self.assertEqual(value.param_id, 5)
        self.assertTrue(value.found)
        self.assertEqual(value.type, 0x00)
        self.assertIs(value.value, True)

    def test_not_found_reply_decodes(self):
        vector = bytes([SAFETY_CMD_PARAM, 0x05, 0x05, 0x00, 0x00])
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_PARAM)
        self.assertIsInstance(value, devices.SafetyGetParam)
        self.assertFalse(value.found)
        self.assertIsNone(value.value)

    def test_malformed_bool_value_byte_rejected(self):
        # type 0x00 (bool) with a value byte that is neither 0 nor 1 --
        # CONFIG_REFERENCE.md/kilnlink_param_value.h treats this as an
        # error, not a truthy value.
        vector = bytes([SAFETY_CMD_PARAM, 0x05, 0x00, 0x01, 0x00, 0x02])
        with self.assertRaises(devices.SafetyResponseError):
            devices.parse_safety_response(vector)


class SafetyGetParamRefusalTests(unittest.TestCase):
    """SAFETY_CMD_GET_PARAM (0x23) echoed back == a refusal, exactly like
    GET_CT_CAL's 0x22-echoed-back case -- today's ONLY real-world outcome."""

    def test_unsupported_refusal_decodes_as_ok_reason(self):
        vector = bytes([SAFETY_CMD_GET_PARAM, 0])
        subcommand, value = devices.parse_safety_response(vector)
        self.assertEqual(subcommand, SAFETY_CMD_GET_PARAM)
        self.assertIsInstance(value, devices.OkReason)
        self.assertFalse(value.ok)

    def test_refusal_reply_is_never_mistaken_for_a_success_reply(self):
        vector = bytes([SAFETY_CMD_GET_PARAM, 0])
        _subcommand, value = devices.parse_safety_response(vector)
        self.assertNotIsInstance(value, devices.SafetyGetParam)

    def test_get_param_returns_ok_reason_not_raises_on_refusal(self):
        # UNLIKE get_ct_cal(), SafetyClient.get_param() must hand the
        # OkReason refusal back to the caller rather than raising --
        # see safety.py's get_param() doc comment.
        with unittest.mock.patch.object(
            mcp_server._safety,
            "_query",
            return_value=devices.OkReason(ok=False, reason="unsupported"),
        ):
            result = mcp_server._safety.get_param(0x0505)
        self.assertIsInstance(result, devices.OkReason)
        self.assertFalse(result.ok)


class SafetyGetParamIdMismatchTests(unittest.TestCase):
    """A reply carrying a different param_id than what was requested must
    never be silently accepted as an answer to the request."""

    def test_mismatched_param_id_raises(self):
        from kilnctrl.safety import SafetyQueryError

        mismatched = devices.SafetyGetParam(param_id=0x0506, found=True, type=0x01, value=7)
        with unittest.mock.patch.object(
            mcp_server._safety, "_query", return_value=mismatched
        ):
            with self.assertRaises(SafetyQueryError):
                mcp_server._safety.get_param(0x0505)

    def test_matching_param_id_is_accepted(self):
        matching = devices.SafetyGetParam(param_id=0x0505, found=True, type=0x01, value=7)
        with unittest.mock.patch.object(
            mcp_server._safety, "_query", return_value=matching
        ):
            result = mcp_server._safety.get_param(0x0505)
        self.assertIs(result, matching)


class SafetyGetParamToolTests(unittest.TestCase):
    """mcp_server.safety_get_param()'s three output shapes -- found, not
    found, and refused -- must never be confused with each other."""

    def test_found_reply_reports_value(self):
        found = devices.SafetyGetParam(param_id=5, found=True, type=0x01, value=42)
        with unittest.mock.patch.object(mcp_server._safety, "get_param", return_value=found):
            result = mcp_server.safety_get_param(5)
        self.assertIn("found", result)
        self.assertIn("42", result)
        self.assertNotIn("not found", result)
        self.assertNotIn("refused", result)

    def test_not_found_reply_reports_not_found(self):
        not_found = devices.SafetyGetParam(param_id=5, found=False, type=0, value=None)
        with unittest.mock.patch.object(
            mcp_server._safety, "get_param", return_value=not_found
        ):
            result = mcp_server.safety_get_param(5)
        self.assertIn("not found", result)
        self.assertNotIn("refused", result)

    def test_refused_reply_reports_refused_never_not_found(self):
        refusal = devices.OkReason(ok=False, reason="unsupported")
        with unittest.mock.patch.object(mcp_server._safety, "get_param", return_value=refusal):
            result = mcp_server.safety_get_param(5)
        self.assertIn("refused", result)
        # The explicit assertion this task calls for: an unsupported/refused
        # reply must NEVER read as "not found" -- those are different wire
        # outcomes (OkReason vs. a found=False SafetyGetParam) and collapsing
        # them would hide a real "not wired up yet" refusal as a bogus claim
        # about the id itself not existing.
        self.assertNotIn("not found", result)
        self.assertNotEqual(result.count("refused"), 2, f"duplicated 'refused' wording: {result!r}")

    def test_query_error_surfaces_as_error_string_not_exception(self):
        from kilnctrl.safety import SafetyQueryError

        with unittest.mock.patch.object(
            mcp_server._safety, "get_param", side_effect=SafetyQueryError("timed out")
        ):
            result = mcp_server.safety_get_param(5)
        self.assertEqual(result, "error: timed out")


if __name__ == "__main__":
    unittest.main()
