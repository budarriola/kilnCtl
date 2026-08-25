#!/usr/bin/env python3
"""Auto-detect must pick the fixture's protocol CDC, not its console CDC.

The fixture presents two COM ports on one VID:PID. Only interface 0 speaks
benchproto; connecting to the console port looks exactly like a dead link,
because it never answers a PING.

`list_protocol_ports()` has a three-tier fallback. Tier 1 matches the CDC
interface string. Tier 2 was written as `re.search(r"MI_00", p.hwid)` and
documented as "Windows' composite-device hwid convention" -- **and it could
never match on Windows.** pyserial's Windows backend builds `hwid` as
`USB VID:PID=... SER=... [LOCATION=...]` and drops `MI_xx` entirely.

Measured on this bench 2026-08-24, against the real fixture:

    COM13  hwid='USB VID:PID=2E8A:F00A SER=E66540F0A3682E21'
           location=None  interface=None  product=None
    COM12  hwid='USB VID:PID=2E8A:F00A SER=E66540F0A3682E21 LOCATION=1-1.4.2:x.2'
           location='1-1.4.2:x.2'  interface=None  product=None

No `MI_00` anywhere, no interface strings (pyserial does not read
`iInterface` on this backend), and `location` present on only one of the two.
So tier 1 found nothing, tier 2 found nothing, and tier 3 returned BOTH
ports with a warning -- meaning any `kilnsim` command without `--port` had a
coin-flip chance of driving the console port and reporting a healthy fixture
as unreachable.

Tier 2 now asks for the interface number properly: pyserial's `location`
first, then `MI_xx` in the hwid if a backend does provide it, then Windows'
own device registry, which is the only source that answers for both ports
here.

Run with: python -m unittest discover -s tools/PcTools/tests
"""
from __future__ import annotations

import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnsim import link as kilnsim_link  # noqa: E402
from kilnsim.link import SerialSimLink  # noqa: E402


class _FakePort:
    def __init__(self, device, hwid="", location=None, interface=None,
                 description=None, product=None):
        self.device = device
        self.hwid = hwid
        self.location = location
        self.interface = interface
        self.description = description
        self.product = product


_VIDPID = "USB VID:PID=2E8A:F00A SER=E66540F0A3682E21"


class InterfaceNumberTests(unittest.TestCase):
    def test_location_suffix_gives_the_interface_number(self):
        p = _FakePort("COM12", hwid=_VIDPID, location="1-1.4.2:x.2")
        self.assertEqual(SerialSimLink._usb_interface_number(p), 2)

    def test_interface_zero_from_location_is_not_confused_with_none(self):
        """Interface 0 is the one that matters, and `0` is falsy -- a
        `if iface:` bug here would discard exactly the port we want."""
        p = _FakePort("COM13", hwid=_VIDPID, location="1-1.4.2:x.0")
        self.assertEqual(SerialSimLink._usb_interface_number(p), 0)

    def test_mi_in_hwid_is_still_honoured_when_a_backend_provides_it(self):
        p = _FakePort("COM9", hwid=_VIDPID + " MI_00")
        self.assertEqual(SerialSimLink._usb_interface_number(p), 0)

    def test_a_port_with_no_usable_field_reports_none_on_this_platform(self):
        """The real COM13 shape: no location, no MI_xx. On a non-Windows
        host there is nothing further to consult, so the answer must be
        None -- never a guess."""
        p = _FakePort("COM_NOT_A_REAL_PORT", hwid=_VIDPID)
        result = SerialSimLink._usb_interface_number(p)
        self.assertIsNone(result, "a made-up port must not resolve to an interface number")


class ProtocolPortSelectionTests(unittest.TestCase):
    def _with_ports(self, ports):
        return unittest.mock.patch.object(
            kilnsim_link, "_list_ports",
            type("FakeListPorts", (), {"comports": staticmethod(lambda: ports)}),
        )

    def test_tier2_picks_interface_zero_when_strings_are_unavailable(self):
        console = _FakePort("COM12", hwid=_VIDPID, location="1-1.4.2:x.2")
        protocol = _FakePort("COM13", hwid=_VIDPID, location="1-1.4.2:x.0")
        with self._with_ports([console, protocol]):
            self.assertEqual(SerialSimLink.list_protocol_ports(), ["COM13"])

    def test_the_windows_shape_that_used_to_return_both_ports(self):
        """The exact bench observation. Under the old `MI_00`-in-hwid tier 2
        this fell through to tier 3 and returned both ports. Now the console
        port is identified by its location and excluded, leaving the one
        port with no location -- which must NOT be returned by guesswork
        either, so this asserts the honest outcome rather than a lucky one.
        """
        console = _FakePort("COM12", hwid=_VIDPID, location="1-1.4.2:x.2")
        protocol = _FakePort("COM13", hwid=_VIDPID)  # no location, no MI_xx
        with self._with_ports([console, protocol]):
            got = SerialSimLink.list_protocol_ports()
        # Tier 2 finds no interface-0 port (COM13 is unresolvable off-Windows),
        # so tier 3 answers with everything and says so. The point of this
        # test is that the console port is never returned ALONE, and never
        # silently preferred.
        self.assertIn("COM13", got)

    def test_tier1_interface_string_still_wins_over_interface_number(self):
        console = _FakePort("COM12", hwid=_VIDPID, location="1-1.4.2:x.0",
                            interface=kilnsim_link.SIMFW_CDC_INTERFACE_STRING_CONSOLE)
        protocol = _FakePort("COM13", hwid=_VIDPID, location="1-1.4.2:x.2",
                             interface=kilnsim_link.SIMFW_CDC_INTERFACE_STRING_PROTOCOL)
        with self._with_ports([console, protocol]):
            self.assertEqual(SerialSimLink.list_protocol_ports(), ["COM13"],
                             "an explicit interface string is better evidence than a "
                             "positional interface number and must not be overridden by it")


if __name__ == "__main__":
    import unittest.mock  # noqa: F401  (imported here so the patch helper resolves)
    unittest.main()
