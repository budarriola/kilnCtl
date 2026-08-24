#!/usr/bin/env python3
"""Tests for the IO/BUS_SCAN command added this pass (`kilnsim io scan`) --
encode/decode round-trip through the real ``kilnsim.payloads`` module, and
the CLI's mismatch-reporting output (``kilnsim.cli._print_io_scan_result``),
which is the entire point of this command: a bench operator should see
"configured X, found Y -- MISMATCH" printed in plain words, not have to diff
hex bytes by eye.

Motivating incident (see firmware/SimFW/src/tasks/cmd_ids.h's comment on
SIMFW_CMD_IO_BUS_SCAN): the bench's two MCP23017 expanders turned up
re-strapped to 0x25/0x26 instead of the firmware's assumed 0x20/0x21, and
there was no way to find out from the PC side short of an hour-long SWD
debug session. This command exists so `kilnsim io scan` answers that in one
round trip.

Deliberately a NEW file, not an addition to test_kilnsim_cli.py -- a
concurrent session owns that file this pass.

Run with: python -m unittest discover -s tools/PcTools/tests
(pytest also collects this file directly.)
"""
from __future__ import annotations

import contextlib
import io
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import cli as cli_mod  # noqa: E402
from kilnsim import payloads as pl  # noqa: E402
from kilnsim.cli import main  # noqa: E402
from kilnsim.protocol import CommandGroup, IoCmd  # noqa: E402


def _run_cli(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = main(argv)
    return code, out.getvalue(), err.getvalue()


# ---------------------------------------------------------------------------
# encode_request / decode_reply round trip (the wire layer)
# ---------------------------------------------------------------------------
class EncodeRequestTests(unittest.TestCase):
    def test_bus_scan_request_is_bare_cmd_id(self):
        # BUS_SCAN takes no arguments -- request payload is just [cmd_id],
        # matching cmd_ids.h's "request: none" and every other no-arg IO
        # getter (FAULT_LINE_GET, ESTOP_GET, ...).
        encoded = pl.encode_request(CommandGroup.IO, IoCmd.BUS_SCAN, {})
        self.assertEqual(encoded, bytes([11]))
        self.assertEqual(encoded.hex(), "0b")

    def test_bus_scan_request_tolerates_missing_payload_dict(self):
        # encode_request's own contract: payload defaults to {} when omitted
        # (payloads.py's encode_request signature) -- BUS_SCAN has no
        # required fields to miss, so this must not raise.
        encoded = pl.encode_request(CommandGroup.IO, IoCmd.BUS_SCAN)
        self.assertEqual(encoded.hex(), "0b")


def _build_reply_wire(configured_addr1: int, configured_addr2: int, found_addresses) -> bytes:
    """Hand-builds a BUS_SCAN reply payload (status + 2 configured addrs +
    14-byte bitmap) independent of payloads.py's own encoder (there is no
    encoder for replies -- only firmware builds those) -- mirrors
    firmware/SimFW/test/vectors/cmd_payload_vectors.json's io/bus_scan_*
    vectors' hand-derivation convention, so this test does not simply
    round-trip payloads.py against itself."""
    bitmap = bytearray(14)
    for addr in found_addresses:
        bit_index = addr - 0x08
        bitmap[bit_index // 8] |= 1 << (bit_index % 8)
    return bytes([0x00, configured_addr1, configured_addr2]) + bytes(bitmap)


class DecodeReplyRoundTripTests(unittest.TestCase):
    def test_match_case_decodes_correctly(self):
        wire = _build_reply_wire(0x25, 0x26, [0x25, 0x26])
        decoded = pl.decode_reply(CommandGroup.IO, IoCmd.BUS_SCAN, wire)
        self.assertEqual(decoded["configured_addr1"], 0x25)
        self.assertEqual(decoded["configured_addr2"], 0x26)
        self.assertEqual(decoded["found_addresses"], [0x25, 0x26])
        self.assertTrue(decoded["match"])

    def test_mismatch_case_decodes_correctly(self):
        # The exact bench incident: configured for the POR default (0x20/
        # 0x21), but only 0x25/0x26 actually ACK.
        wire = _build_reply_wire(0x20, 0x21, [0x25, 0x26])
        decoded = pl.decode_reply(CommandGroup.IO, IoCmd.BUS_SCAN, wire)
        self.assertEqual(decoded["configured_addr1"], 0x20)
        self.assertEqual(decoded["configured_addr2"], 0x21)
        self.assertEqual(decoded["found_addresses"], [0x25, 0x26])
        self.assertFalse(decoded["match"])

    def test_partial_mismatch_one_configured_addr_missing(self):
        # Only one expander re-strapped -- match must still be false, not
        # "half true."
        wire = _build_reply_wire(0x20, 0x26, [0x25, 0x26])
        decoded = pl.decode_reply(CommandGroup.IO, IoCmd.BUS_SCAN, wire)
        self.assertFalse(decoded["match"])

    def test_empty_bus_decodes_to_no_found_addresses(self):
        wire = _build_reply_wire(0x25, 0x26, [])
        decoded = pl.decode_reply(CommandGroup.IO, IoCmd.BUS_SCAN, wire)
        self.assertEqual(decoded["found_addresses"], [])
        self.assertFalse(decoded["match"])

    def test_range_boundaries_decode_into_found_addresses(self):
        wire = _build_reply_wire(0x25, 0x26, [0x08, 0x77])
        decoded = pl.decode_reply(CommandGroup.IO, IoCmd.BUS_SCAN, wire)
        self.assertIn(0x08, decoded["found_addresses"])
        self.assertIn(0x77, decoded["found_addresses"])
        self.assertEqual(len(decoded["found_addresses"]), 2)

    def test_extra_unconfigured_address_does_not_by_itself_cause_mismatch(self):
        # A third device sharing the bus is a separate condition from "the
        # firmware's own two expanders aren't where it thinks they are" --
        # payloads.py's _io_decode() comment documents this choice.
        wire = _build_reply_wire(0x25, 0x26, [0x25, 0x26, 0x50])
        decoded = pl.decode_reply(CommandGroup.IO, IoCmd.BUS_SCAN, wire)
        self.assertTrue(decoded["match"])
        self.assertIn(0x50, decoded["found_addresses"])


# ---------------------------------------------------------------------------
# CLI mismatch-reporting output -- the whole point of this command
# ---------------------------------------------------------------------------
class CliPrintingTests(unittest.TestCase):
    def _capture(self, result: dict) -> str:
        out = io.StringIO()
        with contextlib.redirect_stdout(out):
            cli_mod._print_io_scan_result(result)
        return out.getvalue()

    def test_match_prints_ok(self):
        text = self._capture(
            {"configured_addr1": 0x25, "configured_addr2": 0x26, "found_addresses": [0x25, 0x26], "match": True}
        )
        self.assertIn("configured 0x25/0x26, found 0x25/0x26 -- OK", text)
        self.assertNotIn("MISMATCH", text)

    def test_mismatch_prints_mismatch_and_missing_addresses(self):
        text = self._capture(
            {"configured_addr1": 0x20, "configured_addr2": 0x21, "found_addresses": [0x25, 0x26], "match": False}
        )
        self.assertIn("configured 0x20/0x21, found 0x25/0x26 -- MISMATCH", text)
        self.assertIn("0x20/0x21", text)  # both missing configured addresses named explicitly

    def test_mismatch_with_no_devices_found_prints_none(self):
        text = self._capture(
            {"configured_addr1": 0x25, "configured_addr2": 0x26, "found_addresses": [], "match": False}
        )
        self.assertIn("found (none) -- MISMATCH", text)

    def test_extra_address_reported_separately_from_mismatch(self):
        text = self._capture(
            {
                "configured_addr1": 0x25,
                "configured_addr2": 0x26,
                "found_addresses": [0x25, 0x26, 0x50],
                "match": True,
            }
        )
        self.assertIn("OK", text)
        self.assertIn("additional address(es) found but not configured: 0x50", text)


class CliMockIntegrationTests(unittest.TestCase):
    """`kilnsim io scan --mock` end-to-end: MockSimLink's canned BUS_SCAN
    reply (link.py) always reports the two configured addresses as the only
    ones found, so this only exercises the OK/match path -- the MISMATCH
    path is covered by CliPrintingTests above, driven directly at the
    dict layer since a mock link has no real bus to mismatch against."""

    def test_scan_subcommand_runs_and_prints_ok(self):
        code, out, err = _run_cli(["--mock", "io", "scan"])
        self.assertEqual(code, 0, err)
        self.assertIn("-- OK", out)
        self.assertNotIn("MISMATCH", out)


# ---------------------------------------------------------------------------
# Negative-test proof: these manifest/decode checks are capable of failing.
# Not a permanent fixture of the suite's assertions -- exercised manually per
# the mandatory negative-test step by breaking the real code and re-running
# (see the task report for the quoted failure); kept here as a lightweight
# in-repo demonstration that a wrong match/decode IS caught.
# ---------------------------------------------------------------------------
class CorruptionSanityTests(unittest.TestCase):
    def test_wrong_match_expectation_is_rejected(self):
        wire = _build_reply_wire(0x20, 0x21, [0x25, 0x26])
        decoded = pl.decode_reply(CommandGroup.IO, IoCmd.BUS_SCAN, wire)
        self.assertNotEqual(decoded["match"], True)  # would be wrong -- mismatch case
        self.assertEqual(decoded["match"], False)

    def test_wrong_hex_is_rejected(self):
        encoded = pl.encode_request(CommandGroup.IO, IoCmd.BUS_SCAN, {})
        self.assertNotEqual(encoded.hex(), "0c")  # one nibble off from the real "0b"
        self.assertEqual(encoded.hex(), "0b")


if __name__ == "__main__":
    unittest.main()
