#!/usr/bin/env python3
"""kilnsim's CDC interface strings must be the ones the firmware actually sends.

`kilnsim.link` picks the fixture's benchproto port out of two same-VID/PID
COM ports by matching the CDC interface string descriptor. Those strings live
in `firmware/SimFW/src/tasks/usb_descriptors.c`; `kilnsim.link` keeps its own
copy of them as `SIMFW_CDC_INTERFACE_STRING_PROTOCOL` / `..._CONSOLE`.

Two hand-maintained copies of one vocabulary is the shape that has already
bitten this repo -- `check_scenarios.py` carried a stale second copy of
`fault_catalog`'s fault-type names and rejected a scenario that runs fine on
hardware. Here the failure would be quieter and worse: auto-detect matches
nothing, and every `kilnsim` command without an explicit `--port` reports the
fixture as absent. On a bench where "fixture not present" is also what an
unplugged board looks like, that is a long detour.

So: read the real descriptor table and compare. This does not need hardware
-- the strings are compile-time constants in the firmware source.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim.link import (SIMFW_CDC_INTERFACE_STRING_CONSOLE,  # noqa: E402
                          SIMFW_CDC_INTERFACE_STRING_PROTOCOL, SIMFW_VID_PID)

REPO_ROOT = Path(__file__).resolve().parents[3]
USB_DESCRIPTORS_C = (REPO_ROOT / "firmware" / "SimFW" / "src" / "tasks"
                     / "usb_descriptors.c")


def _string_desc_arr() -> "list[str | None]":
    """The `string_desc_arr[]` entries from usb_descriptors.c, in index order.
    `NULL` entries come back as None (index 0 is LANGID, index 3 is filled at
    runtime from the RP2040's unique board id)."""
    text = USB_DESCRIPTORS_C.read_text(encoding="utf-8")
    # Strip comments FIRST: the table's own comments quote the interface
    # strings by name, and so does the prose above it. A scan that did not
    # strip them could match the documentation instead of the code -- the
    # exact way several checks in this repo have shipped unfailable.
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.S)
    text = re.sub(r"//[^\n]*", " ", text)
    m = re.search(r"string_desc_arr\s*\[\s*\]\s*=\s*\{(.*?)\}\s*;", text, re.S)
    assert m, "could not find string_desc_arr[] in usb_descriptors.c"
    entries: "list[str | None]" = []
    for raw in m.group(1).split(","):
        raw = raw.strip()
        if not raw:
            continue
        if raw == "NULL":
            entries.append(None)
        else:
            lit = re.fullmatch(r'"((?:[^"\\]|\\.)*)"', raw)
            assert lit, f"unparsed string_desc_arr entry: {raw!r}"
            entries.append(lit.group(1))
    return entries


class UsbDescriptorStringTests(unittest.TestCase):
    def setUp(self):
        self.assertTrue(USB_DESCRIPTORS_C.is_file(), f"missing {USB_DESCRIPTORS_C}")
        self.entries = _string_desc_arr()

    def test_the_table_has_the_shape_this_test_assumes(self):
        """Guards the test itself: if the table grows, shrinks, or reorders,
        the index-based checks below would silently start comparing the
        wrong things."""
        self.assertEqual(len(self.entries), 6,
                         "string_desc_arr[] changed length -- update this test deliberately")
        self.assertIsNone(self.entries[0], "index 0 is the LANGID placeholder")
        self.assertIsNone(self.entries[3], "index 3 is filled at runtime from the board id")

    def test_protocol_interface_string_matches_the_firmware(self):
        self.assertEqual(
            self.entries[4], SIMFW_CDC_INTERFACE_STRING_PROTOCOL,
            "kilnsim.link's protocol-CDC interface string is not what the firmware sends -- "
            "auto-detect would find no fixture and every portless kilnsim command would "
            "report it absent",
        )

    def test_console_interface_string_matches_the_firmware(self):
        self.assertEqual(
            self.entries[5], SIMFW_CDC_INTERFACE_STRING_CONSOLE,
            "kilnsim.link's console-CDC interface string is not what the firmware sends -- "
            "auto-detect could hand back the console port as the protocol link",
        )

    def test_the_two_interface_strings_are_distinct(self):
        """The whole reason these strings exist: both CDCs share one VID:PID,
        so the interface string is the only thing telling them apart."""
        self.assertNotEqual(SIMFW_CDC_INTERFACE_STRING_PROTOCOL,
                            SIMFW_CDC_INTERFACE_STRING_CONSOLE)
        self.assertNotEqual(self.entries[4], self.entries[5])

    def test_the_device_is_named_for_what_it_is(self):
        """The bench asks for this fixture to be identifiable as
        `PiPicoUnitTest` wherever a USB device name shows up -- Device
        Manager, `ls /dev/serial/by-id`, pyserial's `interface`/`product`."""
        self.assertEqual(self.entries[2], "PiPicoUnitTest",
                         "product string changed -- the fixture is meant to enumerate as "
                         "PiPicoUnitTest")
        for s in (SIMFW_CDC_INTERFACE_STRING_PROTOCOL, SIMFW_CDC_INTERFACE_STRING_CONSOLE):
            self.assertTrue(s.startswith("PiPicoUnitTest"),
                            f"{s!r} does not identify the fixture by its bench name")

    def test_vid_pid_constant_is_unchanged(self):
        """SIMFW_VID_PID narrows the search before the interface strings
        disambiguate; a wrong value here makes both strings unreachable."""
        self.assertEqual(SIMFW_VID_PID, "2E8A:F00A")


if __name__ == "__main__":
    unittest.main()
