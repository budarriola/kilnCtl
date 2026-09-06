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
        #: per-address output shadow, PCF8575 power-on default all-high
        self.shadow: "dict[int, int]" = {}
        self.refuse_write_port = False

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

        if subcmd == fixture.PCF8575_CMD_SET_ADDRESS:
            self.current_address = frame.payload[1]
            return
        if subcmd == fixture.PCF8575_CMD_WRITE_PIN:
            pin, level = frame.payload[1], frame.payload[2]
            if level:
                shadow |= 1 << pin
            else:
                shadow &= ~(1 << pin) & 0xFFFF
            self.shadow[addr] = shadow
            return
        if subcmd == fixture.PCF8575_CMD_WRITE_PORT:
            if self.refuse_write_port:
                return  # simulate a firmware-side I2C failure: no reply frame
            value = frame.payload[1] | (frame.payload[2] << 8)
            self.shadow[addr] = value
            return
        if subcmd == fixture.PCF8575_CMD_READ_PORT:
            pins = self.shadow.get(addr, 0xFFFF)
            reply_payload = bytes(
                (
                    fixture.PCF8575_CMD_READ_PORT,
                    pins & 0xFF,
                    (pins >> 8) & 0xFF,
                    pins & 0xFF,
                    (pins >> 8) & 0xFF,
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
        # Both addresses touched by the connect-time all_off(), both all-high.
        self.assertEqual(fake.shadow.get(0x20), 0xFFFF)
        self.assertEqual(fake.shadow.get(0x21), 0xFFFF)

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
        self.assertEqual(fake.shadow[0x20] & 0x1, 0)
        self.client.disconnect()
        self.assertEqual(fake.shadow[0x20] & 0x1, 1)


if __name__ == "__main__":
    unittest.main()
