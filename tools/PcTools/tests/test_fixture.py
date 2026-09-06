#!/usr/bin/env python3
"""Tests for kilnctrl.fixture -- the UnitTestFixture board's PCF8575 relay
control client (task 7 on that board's own, separate UART link).

No real UART, no board: a fake ``serial.Serial`` stands in for the fixture
firmware, decoding real SLIP-stuffed frames with the real CRC and replying
the way ``pcf8575_bridge_task`` does per
firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md -- ACK every DATA frame,
and additionally push a separate DATA reply for the two query subcommands
(READ_PORT/SCAN). This exercises the real encode/decode path in
kilnctrl.protocol / kilnctrl.serial_link, not a mocked-out client.

Run with: python -m pytest tools/PcTools/tests/test_fixture.py -q
"""
from __future__ import annotations

import os
import sys
import threading
import time
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import fixture  # noqa: E402
from kilnctrl.fixture import FixtureClient, FixtureError, RelayId  # noqa: E402
from kilnctrl.protocol import Device, Frame, MsgType  # noqa: E402
from kilnctrl.serial_link import PortInfo  # noqa: E402


class RecommendFixturePortTest(unittest.TestCase):
    """Bench enumeration, 2026-09-05, both boards plugged in at once:
    fixture ESP32-S3 JTAG 303A:1001 (COM7), fixture CH340K UART 1A86:7522, no
    serial (COM14), main board ESP32-S3 JTAG 303A:1001 SER=1C:DB:D4:92:F4:7C
    (COM3), main board CH343 UART 1A86:55D3 SER=552E006806 (COM6). The
    fixture and the main board use DIFFERENT UART bridge silicon (CH340K vs
    CH343), so recommend_fixture_port() must pick the CH340K and must never
    pick the main board's CH343 -- confusing the two would let a relay
    command land on the wrong board's UART task 7.
    """

    def _patch_ports(self, infos):
        return mock.patch.object(fixture, "_list_serial_ports", return_value=infos)

    def test_picks_fixture_ch340k_over_everything_else(self) -> None:
        infos = [
            PortInfo(device="COM7", description="USB-SERIAL-JTAG", manufacturer="Espressif",
                     hwid="USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8", score=0),
            PortInfo(device="COM14", description="USB-SERIAL CH340", manufacturer="wch.cn",
                     hwid="USB VID:PID=1A86:7522", score=0),
            PortInfo(device="COM3", description="USB-SERIAL-JTAG", manufacturer="Espressif",
                     hwid="USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C", score=0),
            PortInfo(device="COM6", description="USB-SERIAL CH343", manufacturer="wch.cn",
                     hwid="USB VID:PID=1A86:55D3 SER=552E006806", score=0),
        ]
        with self._patch_ports(infos):
            self.assertEqual(fixture.recommend_fixture_port(), "COM14")

    def test_never_picks_main_board_ch343(self) -> None:
        """Negative test: with only the main board's CH343 port visible (the
        fixture unplugged), recommend_fixture_port() must refuse to guess
        rather than silently pointing at the wrong board."""
        infos = [
            PortInfo(device="COM6", description="USB-SERIAL CH343", manufacturer="wch.cn",
                     hwid="USB VID:PID=1A86:55D3 SER=552E006806", score=0),
        ]
        with self._patch_ports(infos):
            self.assertIsNone(fixture.recommend_fixture_port())

    def test_six_port_bench_each_picker_owns_its_board(self) -> None:
        """Full bench enumeration, 2026-09-05: both boards AND both CMSIS-DAP
        debug probes attached at once. recommend_fixture_port() must still
        land on exactly the fixture's CH340K and nothing else."""
        infos = [
            PortInfo(device="COM7", description="USB-SERIAL-JTAG", manufacturer="Espressif",
                     hwid="USB VID:PID=303A:1001 SER=68:B6:B3:29:D0:B8", score=0),
            PortInfo(device="COM14", description="USB-SERIAL CH340", manufacturer="wch.cn",
                     hwid="USB VID:PID=1A86:7522", score=0),
            PortInfo(device="COM3", description="USB-SERIAL-JTAG", manufacturer="Espressif",
                     hwid="USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C", score=0),
            PortInfo(device="COM6", description="USB-SERIAL CH343", manufacturer="wch.cn",
                     hwid="USB VID:PID=1A86:55D3 SER=552E006806", score=0),
            PortInfo(device="COM10", description="CMSIS-DAP", manufacturer="Raspberry Pi",
                     hwid="USB VID:PID=2E8A:000C SER=E66540F0A36C6E21", score=0),
            PortInfo(device="COM11", description="CMSIS-DAP", manufacturer="Raspberry Pi",
                     hwid="USB VID:PID=2E8A:000C SER=DEADBEEF00000000", score=0),
        ]
        with self._patch_ports(infos):
            self.assertEqual(fixture.recommend_fixture_port(), "COM14")

    def test_never_picks_main_board_native_jtag_with_generic_description(self) -> None:
        """Reviewer finding: a 303A:1001 port that enumerates with a generic
        description (no "jtag" substring at all -- e.g. Windows' fallback
        "USB Serial Device" label some driver states use) must still be
        excluded by VID:PID, not just by the text hint. With only that one
        port present, recommend_fixture_port() must refuse to guess, never
        fall back to it."""
        infos = [
            PortInfo(device="COM3", description="USB Serial Device (COM3)", manufacturer="",
                     hwid="USB VID:PID=303A:1001 SER=1C:DB:D4:92:F4:7C", score=0),
        ]
        with self._patch_ports(infos):
            self.assertIsNone(fixture.recommend_fixture_port())

    def test_never_falls_back_when_only_unrelated_port_present(self) -> None:
        """No CH340K anywhere on the bench: must return None, never fall
        back to some other unexcluded, unidentified port."""
        infos = [
            PortInfo(device="COM20", description="Some Other USB Serial", manufacturer="Acme",
                     hwid="USB VID:PID=0000:0000", score=0),
        ]
        with self._patch_ports(infos):
            self.assertIsNone(fixture.recommend_fixture_port())


#: Literal wire values, copied from
#: firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md by hand rather than
#: imported from kilnctrl.fixture's PCF8575_CMD_* constants. This is
#: deliberate: the fake plays the role of the real firmware, and a fake that
#: imports the client's own constants would silently "pass" even if a future
#: edit to fixture.py's constants drifted from the documented protocol (the
#: reviewed mirror-value bug class -- a fake and the code under test sharing
#: one source of truth for the wire values under test proves nothing about
#: whether that shared value is still correct).
_WIRE_CMD_WRITE_PORT = 0x01
_WIRE_CMD_WRITE_PIN = 0x02
_WIRE_CMD_READ_PORT = 0x06
_WIRE_CMD_SET_ADDRESS = 0x07


class FakeFixtureSerial:
    """Stands in for the UnitTestFixture board on the other end of the wire.

    Implements just the pyserial surface kilnctrl.serial_link.UartLink
    touches (see its ``connect()``): construct-then-configure, dtr/rts,
    port, open/close, reset_*_buffer, write, read, is_open.
    """

    _by_port: "dict[str, FakeFixtureSerial]" = {}

    def __init__(self, *_, **__):
        self.port = None
        self.is_open = False
        self._dtr = False
        self._rts = False
        self._to_device = bytearray()
        self._to_host = bytearray()
        self._lock = threading.Lock()
        self._next_esp_index = 1
        #: address the fake firmware is "currently talking to" (SET_ADDRESS)
        self.current_address = 0x20
        #: per-address LIVE PIN state, as an external READ_PORT would see it.
        #: Deliberately a separate dict from ``shadow`` below (the mirror
        #: problem the review flagged: a fake that only ever reports back
        #: whatever was last written proves nothing about the client reading
        #: the *pins* field rather than trusting its own write). Tests that
        #: want a write to "not really take" (I2C failure, stuck pin,
        #: external wiring) mutate this directly; ordinary writes update it
        #: in step with ``shadow``.
        self.pins: "dict[int, int]" = {}
        #: per-address output shadow (what was last WRITTEN) -- kept
        #: separate from ``pins`` for the same reason.
        self.shadow: "dict[int, int]" = {}
        self.refuse_write_port = False
        #: WRITE_PIN silently "fails" (frame accepted, pins unchanged) --
        #: simulates the I2C leg failing the way PCF8575.md describes.
        self.refuse_write_pin = False
        #: addresses that never ACK on the I2C bus: SET_ADDRESS to one of
        #: these leaves ``current_address`` unchanged, exactly like
        #: PCF8575_set_address() -> ESP_ERR_NOT_FOUND (no reply frame either
        #: way -- SET_ADDRESS has none in the real protocol; the only way a
        #: caller ever notices is a subsequent READ_PORT's address byte).
        self.refuse_set_address_to: "set[int]" = set()

    # -- pyserial surface used by UartLink.connect() ------------------
    @property
    def dtr(self):
        return self._dtr

    @dtr.setter
    def dtr(self, value):
        self._dtr = value

    @property
    def rts(self):
        return self._rts

    @rts.setter
    def rts(self, value):
        self._rts = value

    def open(self):
        self.is_open = True
        FakeFixtureSerial._by_port[self.port] = self

    def close(self):
        self.is_open = False

    def reset_input_buffer(self):
        self._to_host.clear()

    def reset_output_buffer(self):
        self._to_device.clear()

    def flush(self):
        pass

    @property
    def in_waiting(self) -> int:
        with self._lock:
            return len(self._to_host)

    def write(self, data: bytes) -> int:
        with self._lock:
            self._to_device.extend(data)
            self._drain_frames()
        return len(data)

    def read(self, n: int = 1) -> bytes:
        # UartLink's reader thread polls with a short timeout; a non-blocking
        # "give me what's there" read is enough since our replies are queued
        # synchronously inside write() above.
        with self._lock:
            if not self._to_host:
                time.sleep(0.01)
                return b""
            take = self._to_host[:n] if n > 0 else bytes(self._to_host)
            del self._to_host[: len(take)]
            return bytes(take)

    # -- fake firmware behavior -----------------------------------------
    def _drain_frames(self) -> None:
        while True:
            delim_positions = [i for i, b in enumerate(self._to_device) if b == 0x7E]
            if len(delim_positions) < 2:
                return
            start, end = delim_positions[0], delim_positions[1]
            stuffed = bytes(self._to_device[start : end + 1])
            del self._to_device[: end + 1]
            self._handle_stuffed(stuffed)

    def _handle_stuffed(self, stuffed: bytes) -> None:
        from kilnctrl.protocol import unstuff

        raw = unstuff(stuffed)
        try:
            frame = Frame.from_raw(raw)
        except Exception:
            return
        if frame.msg_type != MsgType.DATA:
            return

        # ACK first, like the real firmware's protocol layer.
        ack = Frame(
            msg_type=MsgType.ACK,
            msg_index=frame.msg_index,
            src_device=Device.ESP,
            src_task=frame.dst_task,
            dst_device=Device.HOST,
            dst_task=frame.src_task,
        )
        self._to_host.extend(ack.to_wire())

        if frame.dst_task != fixture.UART_TASK_ID_PCF8575 or not frame.payload:
            return
        self._apply_pcf8575(frame)

    def _apply_pcf8575(self, frame: Frame) -> None:
        subcmd = frame.payload[0]
        addr = self.current_address
        shadow = self.shadow.get(addr, 0xFFFF)
        pins = self.pins.get(addr, 0xFFFF)

        if subcmd == _WIRE_CMD_SET_ADDRESS:
            target = frame.payload[1]
            if target not in self.refuse_set_address_to:
                self.current_address = target
            # else: nothing ACKed at `target` -- stay on the old address,
            # exactly like PCF8575_set_address() -> ESP_ERR_NOT_FOUND. No
            # reply either way; SET_ADDRESS has none in the real protocol.
            return
        if subcmd == _WIRE_CMD_WRITE_PIN:
            if self.refuse_write_pin:
                return  # I2C leg "fails": shadow/pins both left untouched
            pin, level = frame.payload[1], frame.payload[2]
            if level:
                shadow |= 1 << pin
                pins |= 1 << pin
            else:
                shadow &= ~(1 << pin) & 0xFFFF
                pins &= ~(1 << pin) & 0xFFFF
            self.shadow[addr] = shadow
            self.pins[addr] = pins
            return
        if subcmd == _WIRE_CMD_WRITE_PORT:
            if self.refuse_write_port:
                return  # simulate a firmware-side I2C failure: state unchanged
            value = frame.payload[1] | (frame.payload[2] << 8)
            self.shadow[addr] = value
            self.pins[addr] = value
            return
        if subcmd == _WIRE_CMD_READ_PORT:
            live_pins = self.pins.get(addr, 0xFFFF)
            live_shadow = self.shadow.get(addr, 0xFFFF)
            reply_payload = bytes(
                (
                    _WIRE_CMD_READ_PORT,
                    live_pins & 0xFF,
                    (live_pins >> 8) & 0xFF,
                    live_shadow & 0xFF,
                    (live_shadow >> 8) & 0xFF,
                    addr,
                )
            )
            self._next_esp_index += 1
            reply = Frame(
                msg_type=MsgType.DATA,
                msg_index=self._next_esp_index,
                src_device=Device.ESP,
                src_task=fixture.UART_TASK_ID_PCF8575,
                dst_device=Device.HOST,
                dst_task=fixture.UART_TASK_ID_PCF8575,
                payload=reply_payload,
            )
            self._to_host.extend(reply.to_wire())
            return


def _fake_serial_factory(**kwargs):
    return FakeFixtureSerial(**kwargs)


class FixtureClientTest(unittest.TestCase):
    def setUp(self) -> None:
        FakeFixtureSerial._by_port.clear()
        patcher = mock.patch("kilnctrl.serial_link.serial.Serial", side_effect=_fake_serial_factory)
        self._serial_cls = patcher.start()
        self.addCleanup(patcher.stop)

        small_map = {
            "U4:P00": RelayId(address=0x20, pin=0),
            "U4:P01": RelayId(address=0x20, pin=1),
            "U5:P00": RelayId(address=0x21, pin=0),
        }
        self.client = FixtureClient(relay_map=small_map)
        self.addCleanup(self._safe_close)

    def _safe_close(self) -> None:
        if self.client.is_connected:
            self.client.disconnect()

    def test_connect_de_energizes_everything(self) -> None:
        self.client.connect(port="COMFAKE0")
        fake = FakeFixtureSerial._by_port["COMFAKE0"]
        # Both addresses touched by the connect-time all_off(), both all-high
        # -- checked against the LIVE PINS the client actually verified
        # against, not just the write-side shadow.
        self.assertEqual(fake.pins.get(0x20), 0xFFFF)
        self.assertEqual(fake.pins.get(0x21), 0xFFFF)

    def test_set_relay_then_read_back(self) -> None:
        self.client.connect(port="COMFAKE1")
        self.client.set_relay("U4:P00", True)
        states = self.client.get_relays()
        self.assertTrue(states["U4:P00"])
        self.assertFalse(states["U4:P01"])
        self.assertFalse(states["U5:P00"])

    def test_set_relay_on_second_address_retargets(self) -> None:
        self.client.connect(port="COMFAKE2")
        self.client.set_relay("U5:P00", True)
        states = self.client.get_relays()
        self.assertTrue(states["U5:P00"])

    def test_bad_relay_name_refused(self) -> None:
        """Negative test: an unknown relay name must be rejected before any
        frame is ever sent -- not silently ignored, not sent as garbage."""
        self.client.connect(port="COMFAKE3")
        fake = FakeFixtureSerial._by_port["COMFAKE3"]
        before = bytes(fake._to_device)
        with self.assertRaises(FixtureError):
            self.client.set_relay("NOT_A_RELAY", True)
        # Nothing new was written to the wire for the rejected call.
        self.assertEqual(bytes(fake._to_device), before)

    def test_disconnect_de_energizes(self) -> None:
        self.client.connect(port="COMFAKE4")
        fake = FakeFixtureSerial._by_port["COMFAKE4"]
        self.client.set_relay("U4:P00", True)
        self.assertEqual(fake.pins[0x20] & 0x1, 0)
        self.client.disconnect()
        self.assertEqual(fake.pins[0x20] & 0x1, 1)

    # -- BLOCKER 1: SET_ADDRESS must be confirmed, not just ACKed ----------
    def test_set_address_not_taking_is_refused(self) -> None:
        """Negative test: nothing ACKs at 0x21 (U5's assumed, unconfirmed
        address). A bare transport ACK for the SET_ADDRESS frame must NOT be
        trusted as "the firmware switched" -- the client must READ_PORT back
        the address byte and refuse the operation when it disagrees, rather
        than silently driving U4 (still on 0x20) while believing it is U5."""
        self.client.connect(port="COMFAKE5")
        fake = FakeFixtureSerial._by_port["COMFAKE5"]
        # connect()'s own all_off() loop leaves the client's cached
        # _current_address on the last address it touched (0x21) -- force it
        # back to 0x20 first so the U5 attempt below has to actually switch
        # (and re-confirm) rather than finding the address already cached.
        self.client.set_relay("U4:P00", False)
        fake.refuse_set_address_to = {0x21}
        with self.assertRaises(FixtureError):
            self.client.set_relay("U5:P00", True)
        # Confirm nothing was silently written to U4's pins as a side effect
        # of the failed switch (i.e. the write was refused before WRITE_PIN
        # was ever sent, not sent-then-discovered-wrong).
        self.assertEqual(fake.pins.get(0x20, 0xFFFF), 0xFFFF)

    # -- BLOCKER 2: writes must be verified by read-back, not just ACKed ---
    def test_all_off_raises_when_readback_disagrees(self) -> None:
        """Negative test: WRITE_PORT is ACKed (delivered) but the simulated
        I2C leg fails, so the write SHADOW never actually changes. all_off()
        must raise, not report success. (all_off() verifies against shadow,
        not the live pins register -- see the next two tests for why.)"""
        self.client.connect(port="COMFAKE6")
        fake = FakeFixtureSerial._by_port["COMFAKE6"]
        # Force U4's write shadow off its rest state so the read-back inside
        # all_off() (run again, this time rigged to fail) has something
        # real to disagree with.
        fake.shadow[0x20] = 0x0000
        fake.pins[0x20] = 0x0000
        fake.refuse_write_port = True
        with self.assertRaises(FixtureError):
            self.client.all_off()

    # -- BLOCKER: all_off()/set_relay(on=False) must verify against the
    # write SHADOW, not the live pins/INPUT register -- PCF8575.c only
    # guarantees a bit *written* 0 reads back 0 (PCF8575.md); a bit released
    # to the weak pull-up legitimately reads whatever is externally wired,
    # which on a populated board can be 0 with nothing to do with the write.
    def test_all_off_tolerates_externally_held_low_input(self) -> None:
        """A pin released (shadow bit = 1) but held low by something wired
        to it (e.g. a relay's own feedback contact) must NOT make all_off()
        raise -- only the shadow matters for confirming a release."""
        self.client.connect(port="COMFAKE9")
        fake = FakeFixtureSerial._by_port["COMFAKE9"]
        # Shadow says fully released; the live INPUT register disagrees on
        # one bit because something external is holding it low. This must
        # not be mistaken for a failed release.
        fake.shadow[0x20] = 0xFFFF
        fake.pins[0x20] = 0xFFFE  # bit 0 held low externally
        self.client.all_off()  # must not raise

    def test_all_off_raises_when_shadow_bit_still_zero(self) -> None:
        """The actual failure mode: the firmware's own record of what it
        wrote (shadow) still has a bit driven low. This MUST raise,
        regardless of what the (irrelevant, for a release) pins register
        happens to read."""
        self.client.connect(port="COMFAKE10")
        fake = FakeFixtureSerial._by_port["COMFAKE10"]
        fake.shadow[0x20] = 0xFFFE  # firmware still thinks bit 0 is driven low
        fake.pins[0x20] = 0xFFFF  # even though the input register reads all-high
        fake.refuse_write_port = True  # so all_off()'s own WRITE_PORT can't fix it
        with self.assertRaises(FixtureError):
            self.client.all_off()

    def test_set_relay_raises_when_readback_disagrees(self) -> None:
        """Negative test: WRITE_PIN is ACKed but the simulated I2C leg fails
        silently. set_relay() must raise, not report success -- and since
        this was an energize attempt, it must also leave the device in a
        confirmed de-energized state (fail-safe all_off), never an
        unconfirmed possibly-energized one."""
        self.client.connect(port="COMFAKE7")
        fake = FakeFixtureSerial._by_port["COMFAKE7"]
        # Leave a DIFFERENT pin (P01) already energized so the fail-safe
        # all_off() this test expects has something observable to undo --
        # P00 itself starts de-energized (high), matching connect()'s rest
        # state, so a mismatch on it can only come from the refused write.
        fake.pins[0x20] = ~(1 << 1) & 0xFFFF
        fake.shadow[0x20] = fake.pins[0x20]
        fake.refuse_write_pin = True
        with self.assertRaises(FixtureError):
            self.client.set_relay("U4:P00", True)
        # Fail-safe all_off() ran despite refuse_write_pin (a different
        # subcommand, WRITE_PORT, which is not rigged to fail here): P01,
        # which was energized a moment ago, is confirmed de-energized too.
        self.assertEqual(fake.pins[0x20], 0xFFFF)

    # -- SHOULD: connect() must fail loud, not warn-and-continue ------------
    def test_connect_fails_when_initial_all_off_readback_fails(self) -> None:
        """If the very first all_off() (run inside connect()) cannot be
        confirmed, connect() must raise and leave is_connected False -- a
        caller must never be handed a "connected" fixture whose de-energized
        state was merely assumed."""
        patcher = mock.patch("kilnctrl.serial_link.serial.Serial", side_effect=_fake_serial_factory)
        patcher.start()
        self.addCleanup(patcher.stop)
        broken_client = FixtureClient(relay_map={"U4:P00": RelayId(address=0x20, pin=0)})
        self.addCleanup(lambda: broken_client.is_connected and broken_client.disconnect())

        # Rig the fake to reject WRITE_PORT before it even exists: patch the
        # class so every instance this test creates starts refused.
        original_init = FakeFixtureSerial.__init__

        def _init_refused(self, *a, **kw):
            original_init(self, *a, **kw)
            self.refuse_write_port = True
            # Start energized (not the PCF8575 power-on-reset rest state) so
            # a refused WRITE_PORT is observable: if it silently "succeeded"
            # by leaving the already-rest 0xFFFF alone, this test would pass
            # for the wrong reason.
            self.pins[0x20] = 0x0000
            self.shadow[0x20] = 0x0000

        with mock.patch.object(FakeFixtureSerial, "__init__", _init_refused):
            with self.assertRaises(FixtureError):
                broken_client.connect(port="COMFAKE8")
        self.assertFalse(broken_client.is_connected)


if __name__ == "__main__":
    unittest.main()
