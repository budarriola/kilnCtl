#!/usr/bin/env python3
"""Standalone sanity check for the KilnCtrl wire protocol implementation.

Run with:  uv run --project tools/PcTools python tools/PcTools/selfcheck.py

Verifies CRC-16/CCITT-FALSE against the standard check value, round-trips
stuffing/unstuffing (including bytes that must be escaped), exercises the
incremental FrameDecoder, and checks every device payload byte layout against
App/drivers/uart_task_ids.h.

Each device task additionally gets a live section that stands up a stub bridge
task on the "ESP" side of a virtual link, so the query flow (request ACKed,
answer in a *separate* DATA frame) is exercised end to end without hardware --
including the two unsolicited push channels this board has that the fixture
did not: THERMO and IO auto-reports, which are byte-identical to a query
answer and are told apart only by "nobody asked".
"""

from __future__ import annotations

import json
import math
import pathlib
import struct
import sys
import threading
import time

from kilnctrl import devices, pin_overlay
from kilnctrl.protocol import (
    FRAME_DELIM,
    FRAME_ESC,
    Device,
    Frame,
    FrameDecoder,
    FrameError,
    MsgType,
    crc16_ccitt_false,
    stuff,
    unstuff,
)
from kilnctrl.serial_link import list_ports, recommend_port

_failures: list[str] = []


def check(label: str, got, want) -> None:
    ok = got == want
    print(f"[{'PASS' if ok else 'FAIL'}] {label}: got {got!r}")
    if not ok:
        _failures.append(f"{label}: got {got!r}, want {want!r}")


class FakePort:
    """Minimal pyserial-shaped endpoint; writes land in the peer's RX buffer."""

    def __init__(self, name: str) -> None:
        self.name = name
        self.is_open = True
        self.peer: "FakePort | None" = None
        self._rx = bytearray()
        self._lock = threading.Lock()
        self._data = threading.Event()
        self.drop_next_writes = 0  # simulate a lossy link

    @property
    def in_waiting(self) -> int:
        with self._lock:
            return len(self._rx)

    def _deliver(self, data: bytes) -> None:
        with self._lock:
            self._rx.extend(data)
        self._data.set()

    def write(self, data: bytes) -> int:
        if self.drop_next_writes > 0:
            self.drop_next_writes -= 1
            return len(data)  # silently swallowed on the "wire"
        if self.peer is not None:
            self.peer._deliver(data)
        return len(data)

    def flush(self) -> None:
        pass

    def read(self, size: int = 1) -> bytes:
        if not self._data.wait(0.05):
            return b""
        with self._lock:
            out = bytes(self._rx[:size])
            del self._rx[: len(out)]
            if not self._rx:
                self._data.clear()
        return out

    def reset_input_buffer(self) -> None:
        with self._lock:
            self._rx.clear()

    reset_output_buffer = flush

    def close(self) -> None:
        self.is_open = False
        self._data.set()


def _wire_up(link, port: FakePort) -> None:
    """Attach a FakePort to a UartLink without touching a real COM port."""
    link._serial = port
    link._port = port.name
    link._decoder.reset()
    link._stop.clear()
    link._reader = threading.Thread(target=link._rx_loop, daemon=True)
    link._reader.start()


def _make_pair(esp_tasks=()):
    """Two cross-wired UartLinks plus their fake ports, ESP side pre-registered."""
    from kilnctrl.serial_link import UartLink

    a, b = FakePort("HOST"), FakePort("ESP")
    a.peer, b.peer = b, a
    host = UartLink(own_device=Device.HOST, ack_timeout=0.3)
    esp = UartLink(own_device=Device.ESP, ack_timeout=0.3)
    inboxes = [esp.register_task(task_id) for task_id in esp_tasks]
    _wire_up(host, a)
    _wire_up(esp, b)
    return a, b, host, esp, inboxes


def _responder(stop: threading.Event, inbox, esp, task_id, answer, seen=None):
    """Generic stub bridge task: answer queries, record fire-and-forget writes.

    ``answer(payload) -> reply bytes or None`` -- None means "this subcommand
    is a write, just ACK it", which is what the real firmware does for
    everything that isn't a query.
    """

    def loop() -> None:
        while not stop.is_set():
            try:
                msg = inbox.get(timeout=0.1)
            except Exception:
                continue
            if not msg.payload:
                continue
            reply = answer(msg.payload)
            if reply is None:
                if seen is not None:
                    seen.append(msg.payload)
                continue
            esp.send(
                dst_task=msg.src_task,
                src_task=task_id,
                payload=reply,
                dst_device=msg.src_device,
            )

    thread = threading.Thread(target=loop, daemon=True)
    thread.start()
    return thread


def loopback_checks() -> None:
    from kilnctrl.serial_link import SendResult

    a, b, host, esp, (inbox,) = _make_pair((devices.UART_TASK_ID_THERMO,))

    try:
        payload = devices.thermo_one_shot(0)
        res = host.send(
            dst_task=devices.UART_TASK_ID_THERMO,
            src_task=devices.UART_TASK_ID_THERMO,
            payload=payload,
        )
        check("registered task -> OK (ACK)", res, SendResult.OK)
        got = inbox.get(timeout=1.0)
        check("payload arrived intact", got.payload, payload)
        check("inbox drained", inbox.qsize(), 0)

        # Task IO was never registered on the "ESP" side -> NACK.
        res = host.send(
            dst_task=devices.UART_TASK_ID_IO,
            src_task=devices.UART_TASK_ID_IO,
            payload=devices.io_all_relays_off(),
        )
        check("unregistered task -> UNDELIVERABLE (NACK)", res, SendResult.UNDELIVERABLE)

        # Drop the first two ACKs: the retransmit must be deduped (not
        # re-delivered) yet still re-ACKed, and the send must still succeed.
        b.drop_next_writes = 2
        res = host.send(
            dst_task=devices.UART_TASK_ID_THERMO,
            src_task=devices.UART_TASK_ID_THERMO,
            payload=payload,
        )
        check("lost ACKs recovered by retry -> OK", res, SendResult.OK)
        check("retransmit deduped (delivered once)", inbox.qsize(), 1)
        inbox.get_nowait()

        # Peer completely deaf: every attempt times out.
        a.drop_next_writes = 100
        host.max_retries = 3  # keep the check fast
        res = host.send(
            dst_task=devices.UART_TASK_ID_THERMO,
            src_task=devices.UART_TASK_ID_THERMO,
            payload=payload,
        )
        check("dead link -> TIMEOUT", res, SendResult.TIMEOUT)
    finally:
        host.disconnect()
        esp.disconnect()

    check(
        "send while disconnected -> NOT_CONNECTED",
        host.send(1, 1, b"\x00"),
        SendResult.NOT_CONNECTED,
    )


#: A GET_PIN_CONFIG reply as build_pin_config_reply() emits it for this
#: board's s_pin_config[] -- the nineteen ESP32-S3 GPIOs docs/HARDWARE.md
#: lists. Expander pins (relays, DRDY, LCD_IORQ/LCD_Reset) are deliberately
#: absent: they are not GPIOs and are reported by the IO task instead.
_PIN_CONFIG_ENTRIES = (
    (8, 0x01),   # SDA
    (9, 0x02),   # SCL
    (43, 0x03),  # UART0 TX (this link)
    (44, 0x04),  # UART0 RX
    (12, 0x05),  # SPI CLK
    (11, 0x06),  # SPI MOSI
    (13, 0x09),  # SPI MISO
    (14, 0x07),  # CS0 thermocouple 0
    (17, 0x07),  # CS1 thermocouple 1
    (18, 0x07),  # CS2 thermocouple 2
    (21, 0x07),  # CS3 display
    (38, 0x0A),  # thermoFault_0
    (47, 0x0A),  # thermoFault_1
    (48, 0x0A),  # thermoFault_2
    (7, 0x0B),   # IO_Expander_IRQ
    (10, 0x0C),  # IO_Expander_RST
    (5, 0x0D),   # DataFromSafty -- the ESP's TX (net name reads backwards)
    (4, 0x0E),   # DataToSafty   -- the ESP's RX
    (6, 0x0F),   # Fault, an ESP OUTPUT to the safety processor
)
_PIN_CONFIG_REPLY = bytes([len(_PIN_CONFIG_ENTRIES)]) + b"".join(
    bytes(entry) for entry in _PIN_CONFIG_ENTRIES
)

#: A GET_FW_VERSION reply as build_fw_version_reply() emits it: protocol
#: version (u16 LE) first at a fixed offset, then dirty/commit/datetime.
_FW_VERSION_REPLY = (
    bytes([5, 0])  # UART_PROTOCOL_VERSION = 5, matches devices.UART_PROTOCOL_VERSION
    + bytes([1, 7])
    + b"a1b2c3d"
    + bytes([20])
    + b"2026-08-09 12:34:56Z"
)


def info_checks() -> None:
    """INFO request/response layouts, then the full query flow over the wire."""
    from kilnctrl.info import InfoClient, InfoQueryError
    from kilnctrl.protocol import (
        INFO_CMD_GET_FW_VERSION,
        INFO_CMD_GET_PIN_CONFIG,
        UART_TASK_ID_INFO,
    )

    print("\n== INFO payload layouts (uart_task_ids.h) ==")
    check("task id INFO == 3", UART_TASK_ID_INFO, 3)
    check("GET_PIN_CONFIG request bytes", devices.info_get_pin_config(), b"\x01")
    check("GET_FW_VERSION request bytes", devices.info_get_fw_version(), b"\x02")
    check(
        "pin config reply is 1 + 2N bytes",
        len(_PIN_CONFIG_REPLY),
        1 + len(_PIN_CONFIG_ENTRIES) * 2,
    )

    entries = devices.parse_pin_config_response(_PIN_CONFIG_REPLY)
    check("pin config entry count", len(entries), len(_PIN_CONFIG_ENTRIES))
    check(
        "pin config gpios",
        [e.gpio for e in entries],
        [gpio for gpio, _ in _PIN_CONFIG_ENTRIES],
    )
    check(
        "pin config labels resolved",
        entries[0].label,
        "I2C SDA (SX1509 expander, and J2/J6 pass-through)",
    )
    # The five function ids added for this board must all resolve to text --
    # an unlabelled id would show up in the About window as "unknown function
    # 0x09", which is exactly the regression this catches.
    for gpio, expected in ((13, "MISO"), (38, "TCFLT"), (7, "IOIRQ"), (10, "IORST"), (6, "SFFLT")):
        entry = next(e for e in entries if e.gpio == gpio)
        check(f"GPIO{gpio} abbrev", entry.abbrev, expected)
        check(f"GPIO{gpio} label is known", entry.label.startswith("unknown"), False)
    check("every reported gpio is on the diagram", pin_overlay.unmapped_gpios(entries), [])

    version = devices.parse_fw_version_response(_FW_VERSION_REPLY)
    check("fw version protocol_version", version.protocol_version, 5)
    check("fw version compatible", version.compatible, True)
    check("fw version dirty flag", version.dirty, True)
    check("fw version commit", version.commit, "a1b2c3d")
    check("fw version build timestamp", version.built, "2026-08-09 12:34:56Z")
    check(
        "fw version status-bar text",
        version.describe(),
        "FW a1b2c3d (dirty) built 2026-08-09 12:34:56Z",
    )
    clean = devices.parse_fw_version_response(
        _FW_VERSION_REPLY[:2] + b"\x00" + _FW_VERSION_REPLY[3:]
    )
    check("clean tree reads 'clean'", "(clean)" in clean.describe(), True)

    # v1 is the unit-test fixture, where task 1 is an MCP4728 DAC rather than
    # three thermocouples -- the single most important thing the version gate
    # has to catch on this board.
    fixture = devices.parse_fw_version_response(bytes([1, 0]) + _FW_VERSION_REPLY[2:])
    check("v1 (fixture) firmware parsed", fixture.protocol_version, 1)
    check("v1 (fixture) firmware is incompatible", fixture.compatible, False)
    check("incompatible describe() flags it", "INCOMPATIBLE" in fixture.describe(), True)

    # Replies carry no subcommand byte, so classification is structural.
    check(
        "classify pin config reply",
        devices.parse_info_response(_PIN_CONFIG_REPLY)[0],
        INFO_CMD_GET_PIN_CONFIG,
    )
    check(
        "classify fw version reply",
        devices.parse_info_response(_FW_VERSION_REPLY)[0],
        INFO_CMD_GET_FW_VERSION,
    )
    for label, bad in (
        ("empty payload rejected", b""),
        ("truncated pin config rejected", b"\x08\x04"),
        ("overrunning commit_len rejected", b"\x01\x40AB"),
    ):
        try:
            devices.parse_info_response(bad)
            check(label, False, True)
        except devices.InfoResponseError:
            check(label, True, True)

    print("\n== INFO query flow over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_INFO,))
    stop = threading.Event()

    def answer(payload: bytes):
        if payload[0] == INFO_CMD_GET_PIN_CONFIG:
            return _PIN_CONFIG_REPLY
        if payload[0] == INFO_CMD_GET_FW_VERSION:
            return _FW_VERSION_REPLY
        return None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_INFO, answer)

    boot_pushes: list[devices.FirmwareVersion] = []
    client = InfoClient(host, on_boot_push=boot_pushes.append)
    try:
        got = client.get_pin_config(timeout=3.0)
        check(
            "live pin config query",
            [e.gpio for e in got][:4],
            [8, 9, 43, 44],
        )
        got_version = client.get_fw_version(timeout=3.0)
        check("live fw version query", got_version.commit, "a1b2c3d")
        check("query reply is not mistaken for a boot push", boot_pushes, [])

        # Unsolicited push: info_boot_push_task() sends this once at boot, to
        # (HOST, task INFO), with nobody having asked.
        esp.send(
            dst_task=UART_TASK_ID_INFO,
            src_task=UART_TASK_ID_INFO,
            payload=_FW_VERSION_REPLY,
            dst_device=Device.HOST,
        )
        deadline = time.time() + 2.0
        while not boot_pushes and time.time() < deadline:
            time.sleep(0.02)
        check("unsolicited push detected as reboot signal", len(boot_pushes), 1)

        # Deaf peer: the request itself never gets through.
        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.get_fw_version(timeout=0.3)
            check("undelivered query raises", False, True)
        except InfoQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


def _thermo_read_reply(entries) -> bytes:
    """Build a READ reply the way the firmware does: count, then 12 bytes each."""
    payload = bytearray([0x05, len(entries)])  # THERMO_CMD_READ
    for channel, temperature, cold_junction, status, flags in entries:
        payload += struct.pack("<BffBB", channel, temperature, cold_junction, status, flags)
        payload += b"\x00"  # byte[11] reserved
    return bytes(payload)


def thermo_checks() -> None:
    """THERMO payload layouts, the query flow, and the auto-report push."""
    from kilnctrl.protocol import THERMO_CHANNEL_ALL, UART_TASK_ID_THERMO, ThermoFault
    from kilnctrl.thermo import ThermoClient, ThermoQueryError

    print("\n== THERMO payload layouts (uart_task_ids.h) ==")
    check("task id THERMO == 1", UART_TASK_ID_THERMO, 1)
    check(
        "config_channel bytes",
        devices.thermo_config_channel(1, 3, 2, True, False),
        b"\x01\x01\x03\x02\x01\x00",
    )
    check(
        "set_thresholds bytes (f32 LE x2 then i8 x2)",
        devices.thermo_set_thresholds(0, 1300.0, -20.0, 85, -20),
        b"\x02\x00" + struct.pack("<ff", 1300.0, -20.0) + struct.pack("<bb", 85, -20),
    )
    check("set_thresholds len", len(devices.thermo_set_thresholds(0, 0, 0, 0, 0)), 12)
    check(
        "set_cj_offset bytes",
        devices.thermo_set_cj_offset(2, 1.5),
        b"\x03\x02" + struct.pack("<f", 1.5),
    )
    check("one_shot bytes", devices.thermo_one_shot(1), b"\x04\x01")
    check("read (all) bytes", devices.thermo_read(), b"\x05\xff")
    check("read (one) bytes", devices.thermo_read(2), b"\x05\x02")
    check("read_faults bytes", devices.thermo_read_faults(0), b"\x06\x00")
    check("clear_faults bytes", devices.thermo_clear_faults(1), b"\x07\x01")
    check(
        "set_auto_report bytes (u16 LE period)",
        devices.thermo_set_auto_report(0x07, 1000),
        b"\x08\x07" + struct.pack("<H", 1000),
    )
    check("read_reg bytes", devices.thermo_read_reg(0, 0x02, 4), b"\x09\x00\x02\x04")
    check("write_reg bytes", devices.thermo_write_reg(0, 0x02, 0xC3), b"\x0a\x00\x02\xc3")

    for label, fn in (
        ("channel 3 rejected", lambda: devices.thermo_one_shot(3)),
        ("channel 0xFF rejected where not allowed", lambda: devices.thermo_one_shot(0xFF)),
        ("bad tc_type rejected", lambda: devices.thermo_config_channel(0, 0x0B)),
        ("bad avg_mode rejected", lambda: devices.thermo_config_channel(0, 3, 9)),
        ("cj offset out of range rejected", lambda: devices.thermo_set_cj_offset(0, 9.0)),
        ("channel mask 0x08 rejected", lambda: devices.thermo_set_auto_report(0x08, 100)),
        ("read_reg len 17 rejected", lambda: devices.thermo_read_reg(0, 0, 17)),
    ):
        try:
            fn()
            check(label, False, True)
        except ValueError:
            check(label, True, True)

    # Replies as thermo_bridge_task() emits them.
    read_reply = _thermo_read_reply(
        (
            (0, 25.5, 24.0, 0x00, 0x00),
            (1, 1201.25, 26.5, 0x08, 0x01),  # TCHIGH + ~FAULT pin asserted
            (2, float("nan"), float("nan"), 0x00, 0x02),  # SPI read failed
        )
    )
    check("READ reply is 2 + 12N bytes", len(read_reply), 2 + 3 * 12)
    subcommand, readings = devices.parse_thermo_response(read_reply)
    check("classify READ reply", subcommand, 0x05)
    check("read count", len(readings), 3)
    check("channel 0 temperature", round(readings[0].temperature_c, 2), 25.5)
    check("channel 0 cold junction", round(readings[0].cold_junction_c, 2), 24.0)
    check("channel 0 has no faults", readings[0].fault_labels, [])
    check("channel 0 valid", readings[0].valid, True)
    check(
        "channel 1 fault decoded to text",
        readings[1].fault_labels,
        ["TC above high threshold"],
    )
    check("channel 1 flag decoded to text", readings[1].flag_labels, ["~FAULT pin asserted"])
    check("channel 2 SPI failure flagged", readings[2].flag_labels, ["SPI read failed"])
    check("channel 2 not valid", readings[2].valid, False)
    check("channel 2 temperature is NaN", math.isnan(readings[2].temperature_c), True)
    check(
        "fault labels decode every bit",
        len(devices.thermo_fault_labels(0xFF)),
        8,
    )
    check("ThermoFault flag values", int(ThermoFault.OPEN | ThermoFault.CJRANGE), 0x81)

    faults_reply = bytes([0x06, 2, 0, 0x01, 0xFF, 1, 0x00, 0xFF])
    subcommand, faults = devices.parse_thermo_response(faults_reply)
    check("classify READ_FAULTS reply", subcommand, 0x06)
    check("fault entry count", len(faults), 2)
    check("fault entry 0 decoded", faults[0].fault_labels, ["open circuit (no thermocouple?)"])
    check("fault entry 1 has no faults", faults[1].fault_labels, [])

    reg_reply = bytes([0x09, 1, 0x02, 3, 0xAA, 0xBB, 0xCC])
    subcommand, registers = devices.parse_thermo_response(reg_reply)
    check("classify READ_REG reply", subcommand, 0x09)
    check("register bytes", registers.data, b"\xaa\xbb\xcc")

    for label, bad in (
        ("empty thermo reply rejected", b""),
        ("READ count mismatch rejected", bytes([0x05, 2, 0, 0])),
        ("READ_FAULTS count mismatch rejected", bytes([0x06, 3, 0, 0, 0])),
        ("READ_REG len mismatch rejected", bytes([0x09, 0, 0, 4, 1])),
        ("unknown thermo subcommand rejected", b"\x7f\x00"),
    ):
        try:
            devices.parse_thermo_response(bad)
            check(label, False, True)
        except devices.ThermoResponseError:
            check(label, True, True)

    print("\n== THERMO query flow + auto-report push over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_THERMO,))
    stop = threading.Event()
    writes: list[bytes] = []

    def answer(payload: bytes):
        if payload[0] == 0x05:
            return read_reply
        if payload[0] == 0x06:
            return faults_reply
        if payload[0] == 0x09:
            if payload[1] == 2:
                # Zero-length body: firmware's "this channel's SPI read
                # failed" marker (echoes channel/reg, len=0, no data bytes).
                return bytes([0x09, payload[1], payload[2], 0])
            return reg_reply
        return None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_THERMO, answer, seen=writes)

    reports: list[list] = []
    client = ThermoClient(host, on_report=reports.append)
    try:
        got = client.read(THERMO_CHANNEL_ALL, timeout=3.0)
        check("live read query", [r.channel for r in got], [0, 1, 2])
        check("query answer is not mistaken for an auto-report", reports, [])
        check("last_readings cached", len(client.last_readings), 3)
        check("live read_faults query", len(client.read_faults(timeout=3.0)), 2)
        check("live read_reg query", client.read_reg(1, 0x02, 3, timeout=3.0).data, b"\xaa\xbb\xcc")

        # 2026-08-20: a 0-length READ_REG reply is the firmware's documented
        # marker for "this channel's SPI read failed" (uart_bridge.c). Channel
        # 2 is wired in `answer()` below to send that zero-length reply so we
        # can prove read_reg() actually raises on it -- not just that the
        # parse layer accepts the wire shape (that's covered separately by
        # "classify READ_REG reply" above). We also re-check the success path
        # through the same client/mechanism right after, so this proves the
        # *difference*, not merely that something threw.
        try:
            client.read_reg(2, 0x02, 3, timeout=3.0)
            check("read_reg raises on 0-length firmware reply", False, True)
        except ThermoQueryError as exc:
            check("read_reg raises on 0-length firmware reply", True, True)
            check(
                "0-length read_reg error names the channel",
                "ch2" in str(exc),
                True,
            )
        check(
            "read_reg still succeeds after a 0-length reply on another channel",
            client.read_reg(1, 0x02, 3, timeout=3.0).data,
            b"\xaa\xbb\xcc",
        )

        from kilnctrl.serial_link import SendResult

        res = client.send(devices.thermo_set_auto_report(0x07, 500))
        check("write-style subcommand ACKed", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not writes and time.time() < deadline:
            time.sleep(0.02)
        check("write reached the bridge", writes[:1], [b"\x08\x07" + struct.pack("<H", 500)])

        # The push: byte-identical to a READ answer, but nobody asked. This is
        # the inference the whole client design rests on.
        esp.send(
            dst_task=UART_TASK_ID_THERMO,
            src_task=UART_TASK_ID_THERMO,
            payload=read_reply,
            dst_device=Device.HOST,
        )
        deadline = time.time() + 2.0
        while not reports and time.time() < deadline:
            time.sleep(0.02)
        check("unsolicited READ detected as an auto-report", len(reports), 1)
        if reports:
            check("auto-report carries all channels", len(reports[0]), 3)

        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.read(timeout=0.3)
            check("undelivered query raises", False, True)
        except ThermoQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


def io_checks() -> None:
    """IO (SX1509) payload layouts, query flow, and the auto-report push."""
    from kilnctrl.io_expander import IoClient, IoQueryError
    from kilnctrl.protocol import UART_TASK_ID_IO

    print("\n== IO payload layouts (uart_task_ids.h) ==")
    check("task id IO == 2", UART_TASK_ID_IO, 2)
    check("set_relay bytes", devices.io_set_relay(1, True), b"\x01\x01\x01")
    check("set_relay_mask bytes", devices.io_set_relay_mask(0x0F, 0x05), b"\x02\x0f\x05")
    check("set_io bytes", devices.io_set_io(7, False), b"\x03\x07\x00")
    check("set_io_dir bytes", devices.io_set_io_dir(3, True, True), b"\x04\x03\x01\x01")
    check("read request bytes", devices.io_read(), b"\x05")
    check(
        "set_auto_report bytes (u16 LE)",
        devices.io_set_auto_report(500),
        b"\x06" + struct.pack("<H", 500),
    )
    check("all_relays_off bytes", devices.io_all_relays_off(), b"\x07")
    check("sx_write_reg bytes", devices.sx_write_reg(0x0F, 0xA5), b"\x10\x0f\xa5")
    check("sx_read_reg bytes", devices.sx_read_reg(0x10, 2), b"\x11\x10\x02")
    check("sx_set_dir bytes", devices.sx_set_dir(0xFF00), b"\x12\x00\xff")
    check("sx_set_pullup bytes", devices.sx_set_pullup(0x0010), b"\x13\x10\x00")
    check("sx_set_opendrain bytes", devices.sx_set_opendrain(0x0020), b"\x14\x20\x00")
    check(
        "sx_set_debounce bytes (u16 mask then config)",
        devices.sx_set_debounce(0x00F0, 3),
        b"\x15" + struct.pack("<H", 0x00F0) + b"\x03",
    )
    check(
        "sx_set_int_mask bytes (mask u16 then sense u16)",
        devices.sx_set_int_mask(0x00FF, 0x5555),
        b"\x16" + struct.pack("<HH", 0x00FF, 0x5555),
    )
    check("sx_led_driver bytes", devices.sx_led_driver(4, True, 128), b"\x17\x04\x01\x80")
    check("sx_reset bytes", devices.sx_reset(True), b"\x18\x01")
    check("sx_scan bytes", devices.sx_scan(), b"\x19")

    for label, fn in (
        ("relay 0 rejected", lambda: devices.io_set_relay(0, True)),
        ("relay 5 rejected", lambda: devices.io_set_relay(5, True)),
        ("io 8 rejected", lambda: devices.io_set_io(8, True)),
        ("relay mask 0x10 rejected", lambda: devices.io_set_relay_mask(0x10, 0)),
        ("debounce config 8 rejected", lambda: devices.sx_set_debounce(0, 8)),
        ("led pin 16 rejected", lambda: devices.sx_led_driver(16, True)),
        ("sx_read_reg len 17 rejected", lambda: devices.sx_read_reg(0, 17)),
    ):
        try:
            fn()
            check(label, False, True)
        except ValueError:
            check(label, True, True)

    # The relay numbering is the single most dangerous thing on this board to
    # get wrong, so pin the mapping down in a test rather than only in a doc.
    check("Relay1 is K3 on J8", devices.relay_label(1), "Relay1 (K3 -> J8)")
    check("Relay2 is K1 on J3", devices.relay_label(2), "Relay2 (K1 -> J3)")
    check("Relay3 is K2 on J4", devices.relay_label(3), "Relay3 (K2 -> J4)")
    check("Relay4 is K5 on J11", devices.relay_label(4), "Relay4 (K5 -> J11)")
    check("expander pin 8 is DRDY0", devices.expander_pin_name(8), "thermoDrdy_0")
    check("expander pin 15 is LCD_Reset", devices.expander_pin_name(15).startswith("LCD_Reset"), True)

    # READ reply as io_bridge_task() emits it: relays 1+3 on, IO1/IO2 high,
    # DRDY on channel 1, ~INT asserted.
    read_reply = bytes([0x05]) + struct.pack(
        "<HHBBBB", 0x0105, 0xFF00, 0b0101, 0b0000011, 0b010, 0x01
    )
    check("READ reply is 9 bytes", len(read_reply), 9)
    subcommand, state = devices.parse_io_response(read_reply)
    check("classify READ reply", subcommand, 0x05)
    check("relay 1 on", state.relay(1), True)
    check("relay 2 off", state.relay(2), False)
    check("relay 3 on", state.relay(3), True)
    check("io 1 high", state.io_level(1), True)
    check("io 3 low", state.io_level(3), False)
    check("io 1 is an input (dir bit 4)", state.io_is_input(1), False)
    check("io 5 is an input (dir bit 11)", state.io_is_input(5), True)
    check("channel 1 DRDY asserted", state.drdy_asserted(1), True)
    check("channel 0 DRDY idle", state.drdy_asserted(0), False)
    check("~INT asserted", state.int_asserted, True)
    check("I2C did not fail", state.i2c_failed, False)

    reg_reply = bytes([0x11, 0x10, 2, 0xDE, 0xAD])
    subcommand, registers = devices.parse_io_response(reg_reply)
    check("classify SX_READ_REG reply", subcommand, 0x11)
    check("expander register bytes", registers.data, b"\xde\xad")

    scan_reply = bytes([0x19, 1, 0x3E])
    subcommand, found = devices.parse_io_response(scan_reply)
    check("classify SX_SCAN reply", subcommand, 0x19)
    check("scan found the board's expander", found, [0x3E])

    for label, bad in (
        ("empty io reply rejected", b""),
        ("short READ reply rejected", bytes([0x05, 0, 0])),
        ("SX_READ_REG len mismatch rejected", bytes([0x11, 0, 4, 1])),
        ("SX_SCAN count mismatch rejected", bytes([0x19, 3, 0x3E])),
        ("unknown io subcommand rejected", b"\x7f\x00"),
    ):
        try:
            devices.parse_io_response(bad)
            check(label, False, True)
        except devices.IoResponseError:
            check(label, True, True)

    print("\n== IO query flow + auto-report push over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_IO,))
    stop = threading.Event()
    writes: list[bytes] = []

    def answer(payload: bytes):
        if payload[0] == 0x05:
            return read_reply
        if payload[0] == 0x11:
            return reg_reply
        if payload[0] == 0x19:
            return scan_reply
        return None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_IO, answer, seen=writes)

    reports: list = []
    client = IoClient(host, on_report=reports.append)
    try:
        state = client.read(timeout=3.0)
        check("live read query", (state.relay(1), state.drdy_asserted(1)), (True, True))
        check("query answer is not mistaken for an auto-report", reports, [])
        check("live scan query", client.scan(timeout=3.0), [0x3E])
        check("live read_reg query", client.read_reg(0x10, 2, timeout=3.0).data, b"\xde\xad")

        from kilnctrl.serial_link import SendResult

        res = client.send(devices.io_set_relay(2, True))
        check("write-style subcommand ACKed", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not writes and time.time() < deadline:
            time.sleep(0.02)
        check("write reached the bridge", writes[:1], [b"\x01\x02\x01"])

        esp.send(
            dst_task=UART_TASK_ID_IO,
            src_task=UART_TASK_ID_IO,
            payload=read_reply,
            dst_device=Device.HOST,
        )
        deadline = time.time() + 2.0
        while not reports and time.time() < deadline:
            time.sleep(0.02)
        check("unsolicited READ detected as an auto-report", len(reports), 1)

        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.read(timeout=0.3)
            check("undelivered query raises", False, True)
        except IoQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


def display_checks() -> None:
    """DISPLAY payload layouts, the READ_ID query, and the blit stream."""
    from kilnctrl.display import DisplayClient, DisplayQueryError, test_pattern_rgb565
    from kilnctrl.protocol import UART_PROTO_MAX_PAYLOAD, UART_TASK_ID_DISPLAY

    print("\n== DISPLAY payload layouts (uart_task_ids.h) ==")
    check("task id DISPLAY == 4", UART_TASK_ID_DISPLAY, 4)
    check("reset bytes", devices.display_reset(True), b"\x01\x01")
    check("set_power bytes", devices.display_set_power(False), b"\x02\x00")
    check("set_rotation bytes", devices.display_set_rotation(3), b"\x03\x03")
    check("set_invert bytes", devices.display_set_invert(True), b"\x04\x01")
    check("clear bytes (u16 LE color)", devices.display_clear(0xF800), b"\x05\x00\xf8")
    check(
        "fill_rect bytes (5 x u16 LE)",
        devices.display_fill_rect(1, 2, 3, 4, 0x07E0),
        b"\x06" + struct.pack("<HHHHH", 1, 2, 3, 4, 0x07E0),
    )
    check(
        "draw_rect bytes",
        devices.display_draw_rect(1, 2, 3, 4, 0x001F),
        b"\x07" + struct.pack("<HHHHH", 1, 2, 3, 4, 0x001F),
    )
    check(
        "draw_line bytes",
        devices.display_draw_line(0, 0, 479, 319, 0xFFFF),
        b"\x08" + struct.pack("<HHHHH", 0, 0, 479, 319, 0xFFFF),
    )
    check(
        "set_text_cursor bytes",
        devices.display_set_text_cursor(10, 20),
        b"\x09" + struct.pack("<HH", 10, 20),
    )
    check(
        "set_text_style bytes",
        devices.display_set_text_style(0xFFFF, 0x0000, 2, True),
        b"\x0a" + struct.pack("<HHBB", 0xFFFF, 0x0000, 2, 1),
    )
    check("print bytes", devices.display_print("Kiln"), b"\x0bKiln")
    check(
        "blit_begin bytes",
        devices.display_blit_begin(0, 0, 480, 320),
        b"\x0c" + struct.pack("<HHHH", 0, 0, 480, 320),
    )
    check("blit_data bytes", devices.display_blit_data(b"\x01\x02"), b"\x0d\x01\x02")
    check("blit_end bytes", devices.display_blit_end(), b"\x0e")
    check("read_id bytes", devices.display_read_id(), b"\x0f")

    check("rgb565 white", hex(devices.rgb565(255, 255, 255)), hex(0xFFFF))
    check("rgb565 red", hex(devices.rgb565(255, 0, 0)), hex(0xF800))
    check("rgb565 green", hex(devices.rgb565(0, 255, 0)), hex(0x07E0))
    check("rgb565 blue", hex(devices.rgb565(0, 0, 255)), hex(0x001F))
    check("rgb565 round trip (black)", devices.rgb565_to_rgb(0x0000), (0, 0, 0))
    check("rgb565 round trip (white)", devices.rgb565_to_rgb(0xFFFF), (255, 255, 255))

    for label, fn in (
        ("rotation 4 rejected", lambda: devices.display_set_rotation(4)),
        ("text size 9 rejected", lambda: devices.display_set_text_style(0, 0, 9)),
        ("odd blit chunk rejected", lambda: devices.display_blit_data(b"\x01")),
        ("empty blit chunk rejected", lambda: devices.display_blit_data(b"")),
        # limit is UART_PROTO_MAX_PAYLOAD - 1 = 252 bytes; use 254 (still even)
        # so this stays oversize regardless of future payload-size changes as
        # long as they don't also grow past 254.
        ("oversize blit chunk rejected", lambda: devices.display_blit_data(b"\x00" * 254)),
        ("zero-size blit window rejected", lambda: devices.display_blit_begin(0, 0, 0, 10)),
        # limit is UART_PROTO_MAX_PAYLOAD - 1 = 252 bytes.
        ("text over 252 bytes rejected", lambda: devices.display_print("x" * 253)),
    ):
        try:
            fn()
            check(label, False, True)
        except ValueError:
            check(label, True, True)

    # Chunking: every frame must fit the payload limit and hold whole pixels,
    # because an odd byte count is a firmware-side error and a short frame
    # would silently shift every pixel after it.
    # UART_PROTO_MAX_PAYLOAD is 253 as of uart_task_ids.h version 3
    # (2026-08-11), so a blit chunk holds (253-1)//2 = 126 pixels, not the
    # old 128-payload figure of 63.
    check("blit chunk size is 126 pixels", devices.DISPLAY_BLIT_CHUNK_PIXELS, 126)
    pixels = bytes(range(256)) * 2  # 512 bytes = 256 pixels
    chunks = list(devices.iter_blit_chunks(pixels))
    check("chunk count for 256 pixels", len(chunks), 3)  # 126*2 + 4
    check("every chunk fits the payload limit", all(len(c) <= UART_PROTO_MAX_PAYLOAD for c in chunks), True)
    check("every chunk holds whole pixels", all((len(c) - 1) % 2 == 0 for c in chunks), True)
    check(
        "chunks reassemble to the original pixels",
        b"".join(c[1:] for c in chunks),
        pixels,
    )

    pattern = test_pattern_rgb565(16, 8)
    check("test pattern is 2 bytes per pixel", len(pattern), 16 * 8 * 2)
    check("test pattern needs no Pillow", isinstance(pattern, bytes), True)

    id_reply = bytes([0x0F, 1, 0x54, 0x80, 0x66]) + struct.pack("<HH", 480, 320)
    check("READ_ID reply is 9 bytes", len(id_reply), 9)
    subcommand, ident = devices.parse_display_response(id_reply)
    check("classify READ_ID reply", subcommand, 0x0F)
    check("panel id bytes", ident.id_bytes, b"\x54\x80\x66")
    check("panel geometry", (ident.width, ident.height), (480, 320))
    check("panel ok flag", ident.ok, True)

    for label, bad in (
        ("empty display reply rejected", b""),
        ("short READ_ID reply rejected", bytes([0x0F, 1, 0, 0])),
        ("bad ok flag rejected", bytes([0x0F, 2, 0, 0, 0, 0, 0, 0, 0])),
        ("unknown display subcommand rejected", b"\x7f\x00"),
    ):
        try:
            devices.parse_display_response(bad)
            check(label, False, True)
        except devices.DisplayResponseError:
            check(label, True, True)

    print("\n== DISPLAY query + blit stream over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_DISPLAY,))
    stop = threading.Event()
    writes: list[bytes] = []

    def answer(payload: bytes):
        return id_reply if payload[0] == 0x0F else None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_DISPLAY, answer, seen=writes)

    client = DisplayClient(host)
    try:
        ident = client.read_id(timeout=3.0)
        check("live read_id query", ident.id_bytes, b"\x54\x80\x66")

        progress: list[tuple[int, int]] = []
        frames = client.blit(0, 0, 8, 4, test_pattern_rgb565(8, 4), progress=lambda s, t: progress.append((s, t)))
        check("blit of 32 pixels is one data frame", frames, 1)
        check("progress reported once per chunk", len(progress), 1)
        deadline = time.time() + 2.0
        while len(writes) < 3 and time.time() < deadline:
            time.sleep(0.02)
        # BLIT_BEGIN, one BLIT_DATA, BLIT_END -- in that order, with nothing
        # else interleaved, which is what the firmware requires.
        check("blit sent BEGIN/DATA/END", [w[0] for w in writes[:3]], [0x0C, 0x0D, 0x0E])

        # A mismatched buffer must be caught here, not halfway through the
        # stream with a window already open on the panel.
        try:
            client.blit(0, 0, 10, 10, b"\x00\x00")
            check("wrong-size blit buffer rejected", False, True)
        except ValueError:
            check("wrong-size blit buffer rejected", True, True)

        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.read_id(timeout=0.3)
            check("undelivered query raises", False, True)
        except DisplayQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


def safety_checks() -> None:
    """SAFETY payload layouts and the query flow, including a down link."""
    from kilnctrl.protocol import SAFETY_AGE_NEVER, UART_TASK_ID_SAFETY
    from kilnctrl.safety import SafetyClient, SafetyQueryError

    print("\n== SAFETY payload layouts (uart_task_ids.h) ==")
    check("task id SAFETY == 7", UART_TASK_ID_SAFETY, 7)
    check("get_status bytes", devices.safety_get_status(), b"\x01")
    check("request_enable bytes", devices.safety_request_enable(True), b"\x02\x01")
    check("ping bytes", devices.safety_ping(), b"\x03")
    check("get_link_stats bytes", devices.safety_get_link_stats(), b"\x04")
    check(
        "set_poll_period bytes (u16 LE)",
        devices.safety_set_poll_period(500),
        b"\x05" + struct.pack("<H", 500),
    )
    check("set_fault_out bytes", devices.safety_set_fault_out(True), b"\x06\x01")
    check("clear_fault_out bytes", devices.safety_set_fault_out(False), b"\x06\x00")

    # The state this board is actually in today: no Pico firmware exists, so
    # the ESP has never had a valid reply. This must parse cleanly and read as
    # "expected", not as a malformed frame or an error.
    down_reply = bytes([0x01, 0x00]) + struct.pack(
        "<ffBfffH", 0.0, 0.0, 0x00, 0.0, 0.0, 0.0, SAFETY_AGE_NEVER
    )
    check("GET_STATUS reply is 25 bytes", len(down_reply), 25)
    subcommand, status = devices.parse_safety_response(down_reply)
    check("classify GET_STATUS reply", subcommand, 0x01)
    check("link reported down", status.link_up, False)
    check("age is 'never received'", status.never_received, True)
    check("no flags set", status.flag_labels, [])
    check("down link is not an exception", isinstance(status, devices.SafetyStatus), True)

    # A live link, for when the Pico firmware exists.
    up_reply = bytes([0x01, 0x01 | 0x08 | 0x10 | 0x20]) + struct.pack(
        "<ffBfffH", 1150.5, 27.25, 0x00, 12.5, 0.0, 3.25, 120
    )
    _subcommand, live = devices.parse_safety_response(up_reply)
    check("link reported up", live.link_up, True)
    check("relay energized", live.relay_energized, True)
    check("heating enable granted", live.enabled, True)
    check("safety temperature", round(live.temperature_c, 2), 1150.5)
    check("current sense channels", [round(a, 2) for a in live.current_a], [12.5, 0.0, 3.25])
    check("age in ms", live.age_ms, 120)
    check("fault line not asserted by us", live.fault_asserted, False)

    # The fault flag is ours: GPIO6 is an ESP output.
    _subcommand, faulting = devices.parse_safety_response(
        bytes([0x01, 0x02]) + down_reply[2:]
    )
    check("fault line asserted by this firmware", faulting.fault_asserted, True)
    check(
        "fault flag has a readable label",
        faulting.flag_labels,
        ["fault line asserted (by us)"],
    )

    stats_reply = bytes([0x04]) + struct.pack("<IIIIH", 1000, 0, 0, 1000, 500)
    check("GET_LINK_STATS reply is 19 bytes", len(stats_reply), 19)
    subcommand, stats = devices.parse_safety_response(stats_reply)
    check("classify GET_LINK_STATS reply", subcommand, 0x04)
    check("frames sent", stats.frames_sent, 1000)
    # Sent climbing with received stuck at zero is exactly the signature of
    # the missing Pico firmware.
    check("nothing ever received", stats.frames_received, 0)
    check("every poll timed out", stats.timeouts, 1000)
    check("poll period", stats.poll_period_ms, 500)

    for label, bad in (
        ("empty safety reply rejected", b""),
        ("short GET_STATUS reply rejected", bytes([0x01, 0, 0])),
        ("short GET_LINK_STATS reply rejected", bytes([0x04, 0, 0])),
        ("unknown safety subcommand rejected", b"\x7f\x00"),
    ):
        try:
            devices.parse_safety_response(bad)
            check(label, False, True)
        except devices.SafetyResponseError:
            check(label, True, True)

    print("\n== SAFETY query flow over the virtual link ==")
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_SAFETY,))
    stop = threading.Event()
    writes: list[bytes] = []

    def answer(payload: bytes):
        if payload[0] == 0x01:
            return down_reply
        if payload[0] == 0x04:
            return stats_reply
        return None

    _responder(stop, esp_inbox, esp, UART_TASK_ID_SAFETY, answer, seen=writes)

    client = SafetyClient(host)
    try:
        got = client.get_status(timeout=3.0)
        check("live get_status query (link down)", got.never_received, True)
        check("status cached", client.last_status is not None, True)
        check("live get_link_stats query", client.get_link_stats(timeout=3.0).timeouts, 1000)

        from kilnctrl.serial_link import SendResult

        res = client.send(devices.safety_set_fault_out(True))
        check("set_fault_out ACKed", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not writes and time.time() < deadline:
            time.sleep(0.02)
        check("fault-out write reached the bridge", writes[:1], [b"\x06\x01"])

        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.get_status(timeout=0.3)
            check("undelivered query raises", False, True)
        except SafetyQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


def log_checks() -> None:
    """LOG task payload parsing, then a live push over the virtual link."""
    from kilnctrl.device_log import LogClient
    from kilnctrl.protocol import UART_TASK_ID_LOG, LogLevel
    from kilnctrl.serial_link import SendResult

    print("\n== LOG payload parsing (uart_task_ids.h) ==")
    check("task id LOG == 5", UART_TASK_ID_LOG, 5)

    line = devices.parse_log_frame(bytes([0]) + b"thermo: SPI read failed on ch1")
    check("level byte 0 -> ERROR", line.level, LogLevel.ERROR)
    check("text decoded", line.text, "thermo: SPI read failed on ch1")
    check("letter tag", line.letter, "E")

    line = devices.parse_log_frame(bytes([2]) + b"hello")
    check("level byte 2 -> INFO", line.level, LogLevel.INFO)

    line = devices.parse_log_frame(bytes([0xFF]) + b"unknown level")
    check("unknown level byte falls back to INFO", line.level, LogLevel.INFO)

    try:
        devices.parse_log_frame(b"")
        check("empty log frame rejected", False, True)
    except ValueError:
        check("empty log frame rejected", True, True)

    print("\n== LOG: unsolicited push over the virtual link ==")
    _a, _b, host, esp, _ = _make_pair()

    received: list[devices.LogLine] = []
    client = LogClient(host, on_line=received.append)
    try:
        # Firmware side never expects a reply -- same fire-and-forget shape
        # as uart_log_bridge_task() in uart_log_bridge.c.
        res = esp.send(
            dst_task=UART_TASK_ID_LOG,
            src_task=UART_TASK_ID_LOG,
            payload=bytes([LogLevel.WARN]) + b"sx1509: i2c write failed",
            dst_device=Device.HOST,
        )
        check("unsolicited LOG frame delivered", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not received and time.time() < deadline:
            time.sleep(0.02)
        check("LogClient received one line", len(received), 1)
        if received:
            check("received line level", received[0].level, LogLevel.WARN)
            check("received line text", received[0].text, "sx1509: i2c write failed")
    finally:
        client.close()
        host.disconnect()
        esp.disconnect()


def link_hub_checks() -> None:
    """Two independent RemoteUartLink clients sharing one LinkHub -- proves
    the fan-out/refcounted-subscription design in link_hub.py without
    touching the well-known HUB_PORT (so this doesn't collide with a real
    GUI/MCP server hub that might already be running on this machine) or
    real hardware (no connect() call; the hub's own UartLink just never
    opens a port)."""
    import socket as _socket

    from kilnctrl.link_hub import LinkHub, RemoteUartLink

    print("\n== link hub: multiple clients share one link (no hardware) ==")
    listen = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    listen.bind(("127.0.0.1", 0))
    listen.listen(8)
    hub_port = listen.getsockname()[1]
    hub = LinkHub(listen, own_device=Device.HOST)
    hub.start()

    client_a = RemoteUartLink(own_device=Device.HOST, host="127.0.0.1", port=hub_port)
    client_b = RemoteUartLink(own_device=Device.HOST, host="127.0.0.1", port=hub_port)
    try:
        status = client_a.status()
        check("hub status reachable via RPC", status["connected"], False)

        # Regression guard: create_connection's connect timeout must not be
        # left on the socket. If it is, an idle link (the normal resting
        # state between commands) makes the reader thread raise TimeoutError
        # -- an OSError subclass, so _read_loop swallows it -- and the client
        # silently goes "disconnected" that many seconds after the last
        # traffic, with healthy hardware.
        check(
            "client socket is blocking (no lingering connect timeout)",
            client_a._sock.gettimeout(),
            None,
        )

        task_id = devices.UART_TASK_ID_LOG
        inbox_a = client_a.register_task(task_id)
        inbox_b = client_b.register_task(task_id)
        check("task registered once on the real (shared) link", task_id in hub.link._tasks, True)

        # Simulate an inbound DATA frame "from the ESP" by injecting straight
        # into the hub's real UartLink inbox -- both subscribers should see
        # their own independent copy.
        frame = Frame(
            msg_type=MsgType.DATA,
            msg_index=1,
            src_device=Device.ESP,
            src_task=task_id,
            dst_device=Device.HOST,
            dst_task=task_id,
            payload=bytes([2]) + b"hello",
        )
        hub.link._tasks[task_id].inbox.put(frame)

        got_a = inbox_a.get(timeout=2.0)
        got_b = inbox_b.get(timeout=2.0)
        check("client A saw the fanned-out frame", got_a.payload, frame.payload)
        check("client B saw the same fanned-out frame", got_b.payload, frame.payload)

        client_a.unregister_task(task_id)
        time.sleep(0.1)
        check(
            "task stays registered while another subscriber remains",
            task_id in hub.link._tasks,
            True,
        )
        client_b.unregister_task(task_id)
        time.sleep(0.1)
        check(
            "task unregistered from the real link once the last subscriber leaves",
            task_id in hub.link._tasks,
            False,
        )
    finally:
        client_a.close()
        client_b.close()


class _FakeSessionLog:
    """Stand-in for SessionLogger that records instead of touching disk."""

    def __init__(self) -> None:
        self.lines: list[str] = []

    def info(self, msg, *args) -> None:
        self.lines.append(msg % args if args else msg)

    def warning(self, msg, *args) -> None:
        self.lines.append(msg % args if args else msg)

    def error(self, msg, *args) -> None:
        self.lines.append(msg % args if args else msg)


def actions_checks() -> None:
    """Exercise the generic action registry (actions.py) end to end.

    This is what backs the MCP server's press_button/list_buttons tools --
    verifies every registered action is well-formed, that a spot-check of real
    actions across all four device tasks goes over the wire and back, and --
    the safety-critical part -- that the protocol-version compatibility gate
    genuinely blocks device commands both before compatibility is known and
    after an incompatible version is observed.
    """
    from kilnctrl import actions
    from kilnctrl.display import DisplayClient
    from kilnctrl.info import InfoClient
    from kilnctrl.io_expander import IoClient
    from kilnctrl.protocol import (
        INFO_CMD_GET_FW_VERSION,
        INFO_CMD_GET_PIN_CONFIG,
        UART_TASK_ID_DISPLAY,
        UART_TASK_ID_INFO,
        UART_TASK_ID_IO,
        UART_TASK_ID_SAFETY,
        UART_TASK_ID_THERMO,
    )
    from kilnctrl.safety import SafetyClient
    from kilnctrl.thermo import ThermoClient

    print("\n== action registry (actions.py) ==")
    check("registry non-empty", len(actions.ACTIONS) > 0, True)
    for name, action in actions.ACTIONS.items():
        check(f"action {name!r} has a description", bool(action.description), True)
        check(f"action {name!r} is callable", callable(action.run), True)

    _a, _b, host, esp, (info_inbox, thermo_inbox, io_inbox, display_inbox, safety_inbox) = _make_pair(
        (
            UART_TASK_ID_INFO,
            UART_TASK_ID_THERMO,
            UART_TASK_ID_IO,
            UART_TASK_ID_DISPLAY,
            UART_TASK_ID_SAFETY,
        )
    )

    stop = threading.Event()
    thermo_read_reply = _thermo_read_reply(((0, 25.5, 24.0, 0x00, 0x00),))
    io_read_reply = bytes([0x05]) + struct.pack("<HHBBBB", 0x0000, 0xFF00, 0b0001, 0, 0, 0)
    display_id_reply = bytes([0x0F, 1, 0x54, 0x80, 0x66]) + struct.pack("<HH", 480, 320)
    safety_status_reply = bytes([0x01, 0x00]) + struct.pack(
        "<ffBfffH", 0.0, 0.0, 0x00, 0.0, 0.0, 0.0, 0xFFFF
    )

    def info_answer(payload: bytes):
        if payload[0] == INFO_CMD_GET_PIN_CONFIG:
            return _PIN_CONFIG_REPLY
        if payload[0] == INFO_CMD_GET_FW_VERSION:
            return _FW_VERSION_REPLY
        return None

    _responder(stop, info_inbox, esp, UART_TASK_ID_INFO, info_answer)
    _responder(
        stop, thermo_inbox, esp, UART_TASK_ID_THERMO,
        lambda p: thermo_read_reply if p[0] == 0x05 else None,
    )
    _responder(
        stop, io_inbox, esp, UART_TASK_ID_IO,
        lambda p: io_read_reply if p[0] == 0x05 else None,
    )
    _responder(
        stop, display_inbox, esp, UART_TASK_ID_DISPLAY,
        lambda p: display_id_reply if p[0] == 0x0F else None,
    )
    _responder(
        stop, safety_inbox, esp, UART_TASK_ID_SAFETY,
        lambda p: safety_status_reply if p[0] == 0x01 else None,
    )

    info_client = InfoClient(host)
    thermo_client = ThermoClient(host)
    io_client = IoClient(host)
    display_client = DisplayClient(host)
    safety_client = SafetyClient(host)
    ctx = actions.ActionContext(
        link=host,
        info=info_client,
        session_log=_FakeSessionLog(),
        thermo=thermo_client,
        io=io_client,
        display=display_client,
        safety=safety_client,
    )

    try:
        print("\n== action registry: live virtual-link exercise ==")

        result = actions.ACTIONS["IO: All Relays Off"].run(ctx)
        check(
            "device command blocked before compatibility is known",
            result.startswith("error: refused"),
            True,
        )

        version_text = actions.ACTIONS["INFO: Get FW Version"].run(ctx)
        check("INFO: Get FW Version reports compatible", "compatible: yes" in version_text, True)
        check("InfoClient.compatible now True", info_client.compatible, True)

        result = actions.ACTIONS["IO: All Relays Off"].run(ctx)
        check("IO: All Relays Off succeeds once compatible", result.startswith("ok"), True)

        result = actions.ACTIONS["IO: Set Relay"].run(ctx, relay=1, on=True)
        check("IO: Set Relay succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Thermo: One Shot"].run(ctx, channel=0)
        check("Thermo: One Shot succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Thermo: Read All"].run(ctx)
        check("Thermo: Read All returns device data", "CH0: 25.50 C" in result, True)

        result = actions.ACTIONS["IO: Read"].run(ctx)
        check("IO: Read returns device data", "R1=1" in result, True)

        result = actions.ACTIONS["Display: Clear"].run(ctx, color=0)
        check("Display: Clear succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Display: Read ID"].run(ctx)
        check("Display: Read ID returns device data", "480x320" in result, True)

        result = actions.ACTIONS["Safety: Get Status"].run(ctx)
        check("Safety: Get Status returns device data", "never received" in result, True)

        result = actions.ACTIONS["Safety: Set Fault Out"].run(ctx, assert_fault=True)
        check("Safety: Set Fault Out succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Display: Test Pattern"].run(ctx, width=8, height=4)
        check("Display: Test Pattern streams", result.startswith("ok"), True)

        pin_text = actions.ACTIONS["INFO: Get Pin Config"].run(ctx)
        check("INFO: Get Pin Config returns entries", "GPIO8" in pin_text, True)

        # Bad args must not crash press_button's caller -- TypeError/ValueError
        # from a wrong/missing parameter should be catchable, not a hard raise
        # out of run(). mcp_server.press_button relies on exactly this.
        try:
            actions.ACTIONS["IO: Set Relay"].run(ctx, relay=1)  # missing `on`
            check("missing required arg raises", False, True)
        except TypeError:
            check("missing required arg raises", True, True)

        # A query action with no client registered must report, not raise.
        bare = actions.ActionContext(link=host, info=info_client, session_log=_FakeSessionLog())
        check(
            "query action without its client reports cleanly",
            actions.ACTIONS["Thermo: Read All"].run(bare).startswith("error: no THERMO client"),
            True,
        )

        # Now simulate a firmware speaking a different protocol version and
        # confirm the gate re-engages, proving this isn't only checked once.
        # v1 is the unit-test fixture, where task 1 is a DAC -- the exact case
        # that must never be allowed to receive a thermocouple command.
        fixture_version = devices.parse_fw_version_response(
            bytes([1, 0]) + _FW_VERSION_REPLY[2:]
        )
        info_client.last_fw_version = fixture_version
        result = actions.ACTIONS["Thermo: One Shot"].run(ctx, channel=0)
        check(
            "device command blocked after incompatible (v1 fixture) version observed",
            result.startswith("error: refused"),
            True,
        )
        # INFO queries must still work even while incompatible -- that's the
        # only way compatibility could ever be re-established.
        version_text_again = actions.ACTIONS["INFO: Get FW Version"].run(ctx)
        check(
            "INFO queries stay allowed while incompatible",
            version_text_again.startswith("protocol_version:"),
            True,
        )
    finally:
        stop.set()
        for client in (info_client, thermo_client, io_client, display_client, safety_client):
            client.close()
        host.disconnect()
        esp.disconnect()


def _expect_raises(label: str, exc_type, fn) -> None:
    """Assert ``fn()`` raises ``exc_type`` -- and specifically that, not just
    "something went wrong": an IndexError or a struct.error escaping a parser
    is exactly the failure these checks exist to catch."""
    try:
        fn()
    except exc_type:
        check(label, True, True)
    except BaseException as exc:  # noqa: BLE001 - the wrong exception is a failure
        check(f"{label} [raised {type(exc).__name__}: {exc}]", False, True)
    else:
        check(f"{label} [did not raise]", False, True)


def commonfw_vector_checks() -> None:
    """Consumes firmware/CommonFW/test/vectors/frame_vectors.json -- the
    manifest kilnlink's own host test (test/test_frame.c) also asserts
    against. pc_tools is the *third* implementation of this exact envelope
    (CommonFW/README.md); this is what keeps it honest rather than trusting
    that Frame/stuff/protocol.py still agrees with the C side after a change
    on either end.

    Skips cleanly (not a failure) if the manifest doesn't exist yet or this
    checkout doesn't have CommonFW -- the manifest is new as of 2026-08-16
    and this file needs to keep working for anyone on an older checkout.
    """
    vectors_path = (
        pathlib.Path(__file__).resolve().parents[2]
        / "firmware" / "CommonFW" / "test" / "vectors" / "frame_vectors.json"
    )
    if not vectors_path.is_file():
        print(f"\n== CommonFW kilnlink vectors == (skipped: {vectors_path} not found)")
        return

    manifest = json.loads(vectors_path.read_text(encoding="utf-8"))
    print(f"\n== CommonFW kilnlink vectors == ({vectors_path.name})")

    def as_device(value: int):
        # Device only has ESP/HOST -- SAFETY (2) is a real device id on the
        # isolated link but isn't part of this PC-link-facing enum, same
        # leniency Frame.from_raw itself uses (protocol.py's _as_device).
        try:
            return Device(value)
        except ValueError:
            return value

    for v in manifest.get("vectors", []):
        name = v["name"]
        frame = Frame(
            msg_type=MsgType(v["msg_type"]),
            msg_index=v["msg_index"],
            src_device=as_device(v["src_device"]),
            src_task=v["src_task"],
            dst_device=as_device(v["dst_device"]),
            dst_task=v["dst_task"],
            payload=bytes.fromhex(v["payload_hex"]),
        )
        raw = frame.to_raw()
        check(f"{name}: raw_hex matches", raw.hex(), v["raw_hex"])
        check(f"{name}: wire_hex matches", stuff(raw).hex(), v["wire_hex"])
        decoded = Frame.from_raw(bytes.fromhex(v["raw_hex"]))
        check(f"{name}: decode round-trips", decoded, frame)

    for hv in manifest.get("hostile_vectors", []):
        name = hv["name"]
        expect = hv["expect_error"]
        checked_by = hv.get("checked_by", "decode")
        if checked_by == "unstuff":
            # pc_tools' protocol.py unstuff() is documented as a one-shot
            # test/decode convenience, not the live RX path (that's
            # FrameDecoder) -- it does not raise on every case kilnlink's
            # stricter kilnlink_unstuff does. See the vector's own
            # "checked_by_note" in the manifest for which ones apply here.
            if hv.get("checked_by_note", "").startswith("kilnlink only"):
                print(f"  (skip) {name}: kilnlink-only case, see manifest note")
                continue
            try:
                unstuff(bytes.fromhex(hv["wire_hex"]))
                check(f"{name}: raises on {expect}", "did not raise", expect)
            except (FrameError, IndexError, ValueError):
                check(f"{name}: raises on {expect}", True, True)
        else:
            try:
                Frame.from_raw(bytes.fromhex(hv["raw_hex"]))
                check(f"{name}: raises on {expect}", "did not raise", expect)
            except FrameError:
                check(f"{name}: raises on {expect}", True, True)


#: The two vector-file shapes in firmware/CommonFW/test/vectors/. The older
#: ones (context/status/announce/power) put each vector's fields at the top
#: level of the vector object and record the expected bytes as
#: "payload_hex"; the newer ones (diag/trip, and this pass's ceiling/
#: clear_trip/get_fw_version/set_clock) nest fields under "fields" and
#: record expected bytes as "bytes_hex" -- see each file's own
#: "_comment"/"note" for why. Mapping each manifest file name to
#: (kilnlink_codec function, which key holds the field dict -- None means
#: "the vector object itself", which key holds the expected hex) lets one
#: loop below drive every one of them.
_PAYLOAD_VECTOR_MANIFESTS = (
    ("context_vectors.json", "encode_context", None, "payload_hex"),
    ("status_vectors.json", "encode_status", None, "payload_hex"),
    ("announce_vectors.json", "encode_announce", None, "payload_hex"),
    ("power_vectors.json", "encode_power", None, "payload_hex"),
    ("diag_vectors.json", "encode_diag", "fields", "bytes_hex"),
    ("trip_vectors.json", "encode_trip", "fields", "bytes_hex"),
    ("ceiling_vectors.json", "encode_ceiling", "fields", "bytes_hex"),
    ("clear_trip_vectors.json", "encode_clear_trip", "fields", "bytes_hex"),
    ("get_fw_version_vectors.json", "encode_get_fw_version", "fields", "bytes_hex"),
    ("set_clock_vectors.json", "encode_set_clock", "fields", "bytes_hex"),
)


def commonfw_payload_vector_checks() -> None:
    """Consumes every *payload*-codec vector manifest in
    firmware/CommonFW/test/vectors/ (as opposed to commonfw_vector_checks()
    above, which only covers frame_vectors.json, the framing layer) against
    kilnctrl.kilnlink_codec -- pc_tools' pure-Python mirror of the C payload
    encoders. Proves Python produces byte-identical output to
    firmware/CommonFW/src/kilnlink_<name>.c for every codec that has host
    tests and a vectors file, closing the ROADMAP.md M2 gap where
    selfcheck.py only ever consumed the framing layer.

    Skips a manifest cleanly (not a failure) if it doesn't exist yet, same
    convention as commonfw_vector_checks() -- this file needs to keep
    working for anyone on an older checkout that predates a given codec.
    """
    from kilnctrl import kilnlink_codec

    vectors_dir = (
        pathlib.Path(__file__).resolve().parents[2]
        / "firmware" / "CommonFW" / "test" / "vectors"
    )

    for filename, fn_name, fields_key, hex_key in _PAYLOAD_VECTOR_MANIFESTS:
        path = vectors_dir / filename
        if not path.is_file():
            print(f"\n== CommonFW kilnlink payload vectors: {filename} == (skipped: not found)")
            continue

        manifest = json.loads(path.read_text(encoding="utf-8"))
        print(f"\n== CommonFW kilnlink payload vectors: {filename} ==")
        encode = getattr(kilnlink_codec, fn_name)

        for v in manifest.get("vectors", []):
            name = v["name"]
            fields = v[fields_key] if fields_key else v
            expected = v[hex_key]
            got = encode(fields).hex()
            check(f"{filename} {name}: bytes match {hex_key}", got, expected)


def hardening_checks() -> None:
    """Negative paths: malformed bytes off the wire, absurd arguments, and the
    link failing underneath a caller.

    Everything here is something a *non*-firmware peer can do -- line noise, a
    half-flashed board, a v1 fixture, a process that isn't pc_tools bound to
    the hub port -- so each case must produce a specific, catchable exception
    or a clean error result, never an IndexError/struct.error escaping into
    the GUI event loop or an MCP tool response.
    """
    from kilnctrl.display import (
        MAX_BLIT_DIMENSION,
        DisplayClient,
        DisplayQueryError,
        image_to_rgb565,
        test_pattern_rgb565,
    )
    from kilnctrl.protocol import (
        HEADER_LEN,
        UART_PROTO_MAX_PAYLOAD,
        UART_TASK_ID_DISPLAY,
    )
    from kilnctrl.serial_link import SendResult

    print("\n== hardening: malformed frames ==")
    # A LENGTH byte larger than the protocol's maximum payload must be a
    # FrameError. Frame's own constructor raises a plain ValueError for the
    # same thing, and the RX loop does not catch that -- it would kill the
    # reader thread rather than dropping one frame.
    over = bytearray([int(MsgType.DATA), 0, 1, 0, 1, 1, 1, UART_PROTO_MAX_PAYLOAD + 1])
    over += bytes(UART_PROTO_MAX_PAYLOAD + 1)
    over += struct.pack(">H", crc16_ccitt_false(bytes(over)))
    _expect_raises("over-long LENGTH rejected as FrameError", FrameError, lambda: Frame.from_raw(bytes(over)))
    _expect_raises(
        "header-only frame rejected",
        FrameError,
        lambda: Frame.from_raw(bytes(HEADER_LEN)),
    )
    _expect_raises(
        "unknown msg type rejected",
        FrameError,
        lambda: Frame.from_raw(bytes([0x55, 0, 1, 0, 1, 1, 1, 0, 0, 0])),
    )
    # Pure noise must never yield a frame, and must never grow the decoder's
    # buffer without bound. NOTE: bytes(range(256)) is NOT valid noise for
    # this -- it contains FRAME_DELIM (0x7E) and FRAME_ESC (0x7D) exactly
    # once each per 256-byte cycle, so the decoder correctly treats those as
    # real frame boundaries and returns the (garbage) bytes between them.
    # That is FrameDecoder doing its job, not a bug -- CRC/structure
    # validation happens one layer up, in Frame.from_raw(). Excluding those
    # two byte values keeps this a real no-delimiter-ever-seen test.
    dec = FrameDecoder()
    noise = bytes(b for b in range(256) if b not in (FRAME_DELIM, FRAME_ESC)) * 8
    got = dec.feed(noise)
    check("noise stream yields no frames", got, [])
    check("decoder buffer stays bounded", len(dec._buf) <= 138, True)

    print("\n== hardening: THERMO reply parsing ==")
    for label, bad in (
        ("READ count 4 (more than the board has) rejected", bytes([0x05, 4]) + bytes(4 * 12)),
        (
            "READ entry with channel 7 rejected",
            bytes([0x05, 1]) + struct.pack("<BffBB", 7, 25.0, 24.0, 0, 0) + b"\x00",
        ),
        (
            "READ entry with infinite temperature rejected",
            bytes([0x05, 1]) + struct.pack("<BffBB", 0, float("inf"), 24.0, 0, 0) + b"\x00",
        ),
        (
            "READ entry with infinite cold junction rejected",
            bytes([0x05, 1]) + struct.pack("<BffBB", 0, 25.0, float("-inf"), 0, 0) + b"\x00",
        ),
        ("READ truncated mid-entry rejected", bytes([0x05, 1]) + bytes(6)),
        ("READ_FAULTS count 9 rejected", bytes([0x06, 9]) + bytes(27)),
        ("READ_FAULTS channel 5 rejected", bytes([0x06, 1, 5, 0x01, 0xFF])),
        ("READ_REG channel 9 rejected", bytes([0x09, 9, 0x02, 1, 0xAA])),
        ("READ_REG len 17 rejected", bytes([0x09, 0, 0x02, 17]) + bytes(17)),
        ("READ_REG header truncated rejected", bytes([0x09, 0, 0x02])),
        ("subcommand-only READ payload rejected", bytes([0x05])),
    ):
        _expect_raises(label, devices.ThermoResponseError, lambda b=bad: devices.parse_thermo_response(b))

    # READ_REG len=0 is NOT a malformed reply as of uart_bridge.c's
    # THERMO_CMD_READ_REG case (2026-08-20): the firmware now replies with a
    # 0-length body instead of dropping the reply when the channel's SPI read
    # failed. parse_thermo_response() must accept it structurally; only
    # ThermoClient.read_reg() -- which knows the PC never legitimately asks
    # for 0 bytes -- turns it into an error for the caller.
    zero_len_reg = bytes([0x09, 0, 0x02, 0])
    subcommand, zero_regs = devices.parse_thermo_response(zero_len_reg)
    check("READ_REG len 0 accepted as a failure marker", subcommand, 0x09)
    check("READ_REG len 0 carries no data bytes", zero_regs.data, b"")

    # NaN stays legal: it is how the firmware reports a channel whose SPI read
    # failed, and dropping it would hide a real fault.
    nan_entry = bytes([0x05, 1]) + struct.pack(
        "<BffBB", 2, float("nan"), float("nan"), 0, 0x02
    ) + b"\x00"
    _sub, nan_readings = devices.parse_thermo_response(nan_entry)
    check("NaN temperature still accepted (SPI-failed channel)", math.isnan(nan_readings[0].temperature_c), True)
    check("SPI-failed channel reads as invalid", nan_readings[0].valid, False)

    print("\n== hardening: IO / DISPLAY / SAFETY reply parsing ==")
    for label, bad in (
        ("SX_READ_REG len 17 rejected", bytes([0x11, 0x10, 17]) + bytes(17)),
        ("SX_READ_REG len 0 rejected", bytes([0x11, 0x10, 0])),
        ("SX_SCAN with a non-7-bit address rejected", bytes([0x19, 1, 0x80])),
        ("READ 8 bytes (one short) rejected", bytes([0x05]) + bytes(7)),
        ("READ 10 bytes (one long) rejected", bytes([0x05]) + bytes(9)),
    ):
        _expect_raises(label, devices.IoResponseError, lambda b=bad: devices.parse_io_response(b))

    _expect_raises(
        "READ_ID claiming ok with 0x0 geometry rejected",
        devices.DisplayResponseError,
        lambda: devices.parse_display_response(
            bytes([0x0F, 1, 0x54, 0x80, 0x66]) + struct.pack("<HH", 0, 0)
        ),
    )
    _expect_raises(
        "READ_ID truncated mid-geometry rejected",
        devices.DisplayResponseError,
        lambda: devices.parse_display_response(bytes([0x0F, 1, 0, 0, 0, 0, 0, 0])),
    )

    for label, bad in (
        (
            "GET_STATUS with an infinite current rejected",
            bytes([0x01, 0x01])
            + struct.pack("<ffBfffH", 20.0, 20.0, 0, float("inf"), 0.0, 0.0, 10),
        ),
        (
            "GET_STATUS with an infinite temperature rejected",
            bytes([0x01, 0x01])
            + struct.pack("<ffBfffH", float("inf"), 20.0, 0, 0.0, 0.0, 0.0, 10),
        ),
        ("GET_LINK_STATS one byte short rejected", bytes([0x04]) + bytes(17)),
    ):
        _expect_raises(label, devices.SafetyResponseError, lambda b=bad: devices.parse_safety_response(b))

    print("\n== hardening: INFO reply parsing ==")
    # Non-ASCII in a text field is replaced, not raised on: a mangled commit
    # string is worth showing, unlike a mangled numeric field.
    weird = bytes([2, 0, 0, 3]) + b"\xff\xfe\xfd" + bytes([1]) + b"\x80"
    version = devices.parse_fw_version_response(weird)
    # Compared as a length/codepoint rather than printed: this selfcheck's
    # own console is cp1252 on Windows and cannot render U+FFFD.
    check(
        "non-ASCII commit is replaced, not fatal",
        (len(version.commit), set(version.commit) == {"�"}),
        (3, True),
    )
    for label, bad in (
        ("fw version 1 byte (can't even read the version) rejected", bytes([2])),
        ("fw version with trailing junk rejected", _FW_VERSION_REPLY + b"\x00"),
        ("fw version datetime_len overrun rejected", bytes([2, 0, 0, 1, 65, 40]) + b"x"),
        ("fw version dirty flag 2 rejected", bytes([2, 0, 2, 0, 0])),
    ):
        _expect_raises(
            label,
            devices.InfoResponseError,
            lambda b=bad: devices.parse_fw_version_response(b),
        )
    _expect_raises(
        "pin config claiming 200 entries rejected",
        devices.InfoResponseError,
        lambda: devices.parse_pin_config_response(bytes([200, 1, 1])),
    )
    # A v1 device's version reply must still parse far enough to *report* the
    # mismatch -- the version field is at a fixed offset precisely so this
    # works across an incompatible peer.
    v1 = devices.parse_fw_version_response(bytes([1, 0]) + _FW_VERSION_REPLY[2:])
    check("v1 peer's version is still readable", v1.protocol_version, 1)
    check("v1 peer is refused", v1.compatible, False)

    print("\n== hardening: argument validation at the API surface ==")
    for label, fn in (
        ("NaN threshold rejected", lambda: devices.thermo_set_thresholds(0, float("nan"), 0, 0, 0)),
        ("infinite threshold rejected", lambda: devices.thermo_set_thresholds(0, float("inf"), 0, 0, 0)),
        ("NaN cj offset rejected", lambda: devices.thermo_set_cj_offset(0, float("nan"))),
        ("negative fill_rect width rejected", lambda: devices.display_fill_rect(0, 0, -5, 10, 0)),
        ("negative coordinate rejected", lambda: devices.display_draw_line(-1, 0, 10, 10, 0)),
        ("color above 0xFFFF rejected", lambda: devices.display_clear(0x1FFFF)),
        ("non-string print text rejected", lambda: devices.display_print(None)),
        ("blit window 0 wide rejected", lambda: devices.display_blit_begin(0, 0, 0, 10)),
        ("absurd test-pattern size rejected", lambda: test_pattern_rgb565(60000, 60000)),
        (
            f"test pattern over {MAX_BLIT_DIMENSION}px per side rejected",
            lambda: test_pattern_rgb565(MAX_BLIT_DIMENSION + 1, 1),
        ),
        ("negative test-pattern size rejected", lambda: test_pattern_rgb565(-1, 10)),
        ("image target size 0 rejected", lambda: image_to_rgb565(__file__, 0, 10)),
        ("absurd image target size rejected", lambda: image_to_rgb565(__file__, 60000, 60000)),
    ):
        _expect_raises(label, ValueError, fn)

    # A file that is not an image must be an ordinary error, not a traceback:
    # this selfcheck's own source is a convenient non-image.
    _expect_raises(
        "non-image file rejected by the blit loader",
        (ValueError, OSError),
        lambda: image_to_rgb565(__file__, 16, 16),
    )
    _expect_raises(
        "missing image file rejected by the blit loader",
        (ValueError, OSError),
        lambda: image_to_rgb565("no-such-file-here.png", 16, 16),
    )

    print("\n== hardening: link failure paths ==")
    a, _b, host, esp, _ = _make_pair((UART_TASK_ID_DISPLAY,))
    try:
        # Port yanked mid-session: the sender must say "no port", not burn
        # every retry instantly and report a peer timeout.
        a.is_open = False
        check(
            "send with the port gone -> NOT_CONNECTED",
            host.send(UART_TASK_ID_DISPLAY, UART_TASK_ID_DISPLAY, b"\x0f"),
            SendResult.NOT_CONNECTED,
        )
    finally:
        host.disconnect()
        esp.disconnect()

    # A reply carrying the wrong subcommand must not satisfy the outstanding
    # query -- otherwise a late/stray frame would be handed to the next
    # caller as if it answered them.
    a, _b, host, esp, (esp_inbox,) = _make_pair((UART_TASK_ID_DISPLAY,))
    stop = threading.Event()
    _responder(
        stop, esp_inbox, esp, UART_TASK_ID_DISPLAY,
        # Answers a READ_ID request with a *THERMO-shaped* payload: the
        # subcommand byte does not match what was asked.
        lambda p: bytes([0x05, 0]) if p[0] == 0x0F else None,
    )
    client = DisplayClient(host)
    try:
        _expect_raises(
            "reply with a mismatched subcommand does not satisfy the query",
            DisplayQueryError,
            lambda: client.read_id(timeout=0.6),
        )
        check("no waiter left dangling after a timeout", client._pending, None)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()

    print("\n== hardening: link hub survives a hostile client ==")
    import socket as _socket

    from kilnctrl.link_hub import LinkHub, RemoteUartLink

    listen = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
    listen.bind(("127.0.0.1", 0))
    listen.listen(8)
    hub_port = listen.getsockname()[1]
    hub = LinkHub(listen, own_device=Device.HOST)
    hub.start()

    raw = _socket.create_connection(("127.0.0.1", hub_port), timeout=2.0)
    try:
        # Valid JSON of the wrong shape, then an unparseable line, then a
        # real request: the first two must cost one message each, not the
        # connection (req.get() on a non-dict used to kill the handler
        # thread, and the client just saw its socket die).
        raw.sendall(b'5\n["not", "an", "object"]\n{oops\n{"id": 1, "op": "status"}\n')
        reply = raw.makefile("r", encoding="utf-8").readline()
        payload = __import__("json").loads(reply)
        check("hub answers after malformed request lines", payload.get("ok"), True)
        check("hub answered the right request id", payload.get("id"), 1)
    finally:
        raw.close()

    client = RemoteUartLink(own_device=Device.HOST, host="127.0.0.1", port=hub_port)
    try:
        check("client reached the hub", client.status()["connected"], False)
        # An unknown op is an error *reply*, not a dropped connection.
        _expect_raises(
            "unknown op reported as an error",
            RuntimeError,
            lambda: client._request("no-such-op"),
        )
        check("client still usable after an error reply", client.status()["connected"], False)
    finally:
        client.close()
    # close() ends the reader, which must release every in-flight waiter
    # rather than leaving it to block for the full RPC timeout.
    check("no pending RPC waiters left after close", client._pending, {})

    # And the waiter must actually be *woken*, not merely forgotten: a GUI
    # worker blocked in _request when the hub process exits would otherwise
    # sit there for the whole 5-8 s budget with the answer already known.
    doomed = RemoteUartLink(own_device=Device.HOST, host="127.0.0.1", port=hub_port)
    waiter = threading.Event()
    box: dict = {}
    with doomed._pending_lock:
        doomed._pending[999] = (waiter, box)
    doomed.close()
    check("in-flight RPC waiter released when the hub goes away", waiter.wait(2.0), True)
    check("released waiter carries a failure, not a result", box.get("msg", {}).get("ok"), False)
    listen.close()


def main() -> int:
    print("== CRC-16/CCITT-FALSE ==")
    # The canonical check value for this variant.
    check("crc('123456789') == 0x29B1", hex(crc16_ccitt_false(b"123456789")), hex(0x29B1))
    check("crc(b'') == 0xFFFF", hex(crc16_ccitt_false(b"")), hex(0xFFFF))
    check("crc(b'\\x00') == 0xE1F0", hex(crc16_ccitt_false(b"\x00")), hex(0xE1F0))

    print("\n== byte stuffing ==")
    raw = bytes([0x01, FRAME_DELIM, 0x02, FRAME_ESC, 0x03, 0x5E, 0x5D])
    stuffed = stuff(raw)
    check("stuffed is delimiter-wrapped", (stuffed[0], stuffed[-1]), (FRAME_DELIM, FRAME_DELIM))
    check("no bare DELIM inside body", FRAME_DELIM in stuffed[1:-1], False)
    check("DELIM -> ESC 0x5E", bytes([FRAME_ESC, 0x5E]) in stuffed, True)
    check("ESC   -> ESC 0x5D", bytes([FRAME_ESC, 0x5D]) in stuffed, True)
    check("unstuff(stuff(x)) == x", unstuff(stuffed), raw)

    print("\n== frame round trip ==")
    payload = devices.thermo_set_thresholds(0, 1300.0, -20.0, 85, -20)
    frame = Frame(
        msg_type=MsgType.DATA,
        msg_index=0x1234,
        src_device=Device.HOST,
        src_task=devices.UART_TASK_ID_THERMO,
        dst_device=Device.ESP,
        dst_task=devices.UART_TASK_ID_THERMO,
        payload=payload,
    )
    raw = frame.to_raw()
    check("raw length == 8 + len + 2", len(raw), 8 + len(payload) + 2)
    check("header[0] MSG_TYPE", raw[0], int(MsgType.DATA))
    check("header[1:3] MSG_INDEX big-endian", raw[1:3], b"\x12\x34")
    check("header[3] SRC_DEVICE", raw[3], int(Device.HOST))
    check("header[5] DST_DEVICE", raw[5], int(Device.ESP))
    check("header[7] LENGTH", raw[7], len(payload))
    crc = crc16_ccitt_false(raw[:-2])
    check("trailing CRC big-endian", raw[-2:], struct.pack(">H", crc))
    check("Frame.from_raw round trip", Frame.from_raw(raw), frame)

    commonfw_vector_checks()
    commonfw_payload_vector_checks()

    print("\n== FrameDecoder (incremental) ==")
    dec = FrameDecoder()
    wire = frame.to_wire()
    # Prepend garbage, split mid-frame, and append back-to-back delimiters.
    stream = b"\xAA\xBB" + wire + bytes([FRAME_DELIM, FRAME_DELIM]) + wire
    got: list[bytes] = []
    for i in range(0, len(stream), 3):  # feed in ragged chunks
        got.extend(dec.feed(stream[i : i + 3]))
    check("two frames recovered from noisy stream", len(got), 2)
    check("decoded frame 0 matches", Frame.from_raw(got[0]), frame)
    check("decoded frame 1 matches", Frame.from_raw(got[1]), frame)

    print("\n== corrupt frames rejected ==")
    bad = bytearray(raw)
    bad[-1] ^= 0xFF
    try:
        Frame.from_raw(bytes(bad))
        check("CRC mismatch raises", False, True)
    except FrameError:
        check("CRC mismatch raises", True, True)
    try:
        Frame.from_raw(raw[:5])
        check("short frame raises", False, True)
    except FrameError:
        check("short frame raises", True, True)

    print("\n== task ids and protocol version (uart_task_ids.h) ==")
    check("protocol version == 5", devices.UART_PROTOCOL_VERSION, 5)
    check("task id THERMO == 1", devices.UART_TASK_ID_THERMO, 1)
    check("task id IO == 2", devices.UART_TASK_ID_IO, 2)
    check("task id INFO == 3", devices.UART_TASK_ID_INFO, 3)
    check("task id DISPLAY == 4", devices.UART_TASK_ID_DISPLAY, 4)
    check("task id LOG == 5", devices.UART_TASK_ID_LOG, 5)
    check("task id SYSTEM == 6", devices.UART_TASK_ID_SYSTEM, 6)
    check("task id SAFETY == 7", devices.UART_TASK_ID_SAFETY, 7)
    check("SYSTEM restart_uart bytes", devices.system_restart_uart(), b"\x01")
    check(
        "SYSTEM get_watchdog_panic_disabled bytes",
        devices.system_get_watchdog_panic_disabled(),
        b"\x03",
    )
    check(
        "SYSTEM set_watchdog_panic_disabled(True) bytes",
        devices.system_set_watchdog_panic_disabled(True),
        b"\x04\x01",
    )
    check(
        "SYSTEM set_watchdog_panic_disabled(False) bytes",
        devices.system_set_watchdog_panic_disabled(False),
        b"\x04\x00",
    )

    print("\n== virtual link: two UartLinks cross-wired (no hardware) ==")
    loopback_checks()

    info_checks()
    thermo_checks()
    io_checks()
    display_checks()
    safety_checks()
    log_checks()
    link_hub_checks()
    actions_checks()
    hardening_checks()

    print("\n== port discovery (no device required) ==")
    ports = list_ports()
    print(f"  {len(ports)} serial port(s) found")
    for info in ports:
        print(f"    {info.device}  score={info.score:<5} {info.description}")
    print(f"  recommend_port() -> {recommend_port()!r}")

    print()
    if _failures:
        print(f"FAILED ({len(_failures)}):")
        for f in _failures:
            print("  -", f)
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
