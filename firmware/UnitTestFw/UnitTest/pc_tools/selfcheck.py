#!/usr/bin/env python3
"""Standalone sanity check for the wire protocol implementation.

Run with:  uv run --project pc_tools python pc_tools/selfcheck.py

Verifies CRC-16/CCITT-FALSE against the standard check value, round-trips
stuffing/unstuffing (including bytes that must be escaped), exercises the
incremental FrameDecoder, and checks the device payload byte layouts against
uart_task_ids.h / uart_bridge.c.

The INFO section additionally stands up a stub ``info_bridge_task`` on the
"ESP" side of the virtual link, so the query flow (request ACKed, answer in a
*separate* DATA frame) and the once-per-boot version push are both exercised
end to end without hardware.
"""

from __future__ import annotations

import struct
import sys
import threading
import time

from uart_control import devices, pin_overlay
from uart_control.protocol import (
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
from uart_control.serial_link import list_ports, recommend_port

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


def loopback_checks() -> None:
    from uart_control.serial_link import SendResult, UartLink

    a, b = FakePort("HOST"), FakePort("ESP")
    a.peer, b.peer = b, a

    host = UartLink(own_device=Device.HOST, ack_timeout=0.3)
    esp = UartLink(own_device=Device.ESP, ack_timeout=0.3)
    inbox = esp.register_task(devices.UART_TASK_ID_DAC)
    _wire_up(host, a)
    _wire_up(esp, b)

    try:
        payload = devices.dac_set_channel_percent(0, 12.5)
        res = host.send(
            dst_task=devices.UART_TASK_ID_DAC,
            src_task=devices.UART_TASK_ID_DAC,
            payload=payload,
        )
        check("registered task -> OK (ACK)", res, SendResult.OK)
        got = inbox.get(timeout=1.0)
        check("payload arrived intact", got.payload, payload)
        check("inbox drained", inbox.qsize(), 0)

        # Task 2 was never registered on the "ESP" side -> NACK.
        res = host.send(
            dst_task=devices.UART_TASK_ID_AD9833,
            src_task=devices.UART_TASK_ID_AD9833,
            payload=devices.ad9833_set_waveform(0),
        )
        check("unregistered task -> UNDELIVERABLE (NACK)", res, SendResult.UNDELIVERABLE)

        # Drop the first two ACKs: the retransmit must be deduped (not
        # re-delivered) yet still re-ACKed, and the send must still succeed.
        b.drop_next_writes = 2
        res = host.send(
            dst_task=devices.UART_TASK_ID_DAC,
            src_task=devices.UART_TASK_ID_DAC,
            payload=payload,
        )
        check("lost ACKs recovered by retry -> OK", res, SendResult.OK)
        check("retransmit deduped (delivered once)", inbox.qsize(), 1)
        inbox.get_nowait()

        # Peer completely deaf: every attempt times out.
        a.drop_next_writes = 100
        host.max_retries = 3  # keep the check fast
        res = host.send(
            dst_task=devices.UART_TASK_ID_DAC,
            src_task=devices.UART_TASK_ID_DAC,
            payload=payload,
        )
        check("dead link -> TIMEOUT", res, SendResult.TIMEOUT)
    finally:
        host.disconnect()
        esp.disconnect()

    check("send while disconnected -> NOT_CONNECTED", host.send(1, 1, b"\x00"), SendResult.NOT_CONNECTED)


#: A GET_PIN_CONFIG reply exactly as build_pin_config_reply() emits it for
#: this firmware's s_pin_config[] (App/drivers/settings.h GPIO numbers).
_PIN_CONFIG_REPLY = bytes(
    [
        8,        # entry_count
        8, 0x01,  # I2C_MASTER_SDA_IO  -> PIN_FUNC_I2C_SDA
        9, 0x02,  # I2C_MASTER_SCL_IO  -> PIN_FUNC_I2C_SCL
        43, 0x03,  # UART_OWNER_TX_IO  -> PIN_FUNC_UART_TX
        44, 0x04,  # UART_OWNER_RX_IO  -> PIN_FUNC_UART_RX
        4, 0x05,  # AD9833_SCLK_IO     -> PIN_FUNC_SPI_SCLK
        5, 0x06,  # AD9833_MOSI_IO     -> PIN_FUNC_SPI_MOSI
        6, 0x07,  # AD9833_CS_IO       -> PIN_FUNC_SPI_CS
        18, 0x08,  # HEARTBEAT_LED_GPIO -> PIN_FUNC_LED_HEARTBEAT
    ]
)

#: A GET_FW_VERSION reply as build_fw_version_reply() emits it: protocol
#: version (u16 LE) first at a fixed offset, then dirty/commit/datetime.
_FW_VERSION_REPLY = (
    bytes([1, 0])  # UART_PROTOCOL_VERSION = 1, matches devices.UART_PROTOCOL_VERSION
    + bytes([1, 7])
    + b"a1b2c3d"
    + bytes([20])
    + b"2026-08-06 12:34:56Z"
)


def info_checks() -> None:
    """INFO request/response layouts, then the full query flow over the wire."""
    from uart_control.info import InfoClient, InfoQueryError
    from uart_control.protocol import (
        INFO_CMD_GET_FW_VERSION,
        INFO_CMD_GET_PIN_CONFIG,
        UART_TASK_ID_INFO,
    )
    from uart_control.serial_link import UartLink

    print("\n== INFO payload layouts (uart_task_ids.h) ==")
    check("task id INFO == 3", UART_TASK_ID_INFO, 3)
    check("GET_PIN_CONFIG request bytes", devices.info_get_pin_config(), b"\x01")
    check("GET_FW_VERSION request bytes", devices.info_get_fw_version(), b"\x02")
    check("pin config reply is 1 + 2N bytes", len(_PIN_CONFIG_REPLY), 1 + 8 * 2)

    entries = devices.parse_pin_config_response(_PIN_CONFIG_REPLY)
    check("pin config entry count", len(entries), 8)
    check(
        "pin config gpios (settings.h order)",
        [e.gpio for e in entries],
        [8, 9, 43, 44, 4, 5, 6, 18],
    )
    check(
        "pin config labels resolved",
        entries[0].label,
        "I2C SDA (MCP4728 DAC + SSD1306 OLED + PCF8575)",
    )
    check("pin config abbrevs resolved", [e.abbrev for e in entries][:4], ["SDA", "SCL", "TX", "RX"])
    check("every reported gpio is on the diagram", pin_overlay.unmapped_gpios(entries), [])

    version = devices.parse_fw_version_response(_FW_VERSION_REPLY)
    check("fw version protocol_version", version.protocol_version, 1)
    check("fw version compatible", version.compatible, True)
    check("fw version dirty flag", version.dirty, True)
    check("fw version commit", version.commit, "a1b2c3d")
    check("fw version build timestamp", version.built, "2026-08-06 12:34:56Z")
    check(
        "fw version status-bar text",
        version.describe(),
        "FW a1b2c3d (dirty) built 2026-08-06 12:34:56Z",
    )
    clean = devices.parse_fw_version_response(
        _FW_VERSION_REPLY[:2] + b"\x00" + _FW_VERSION_REPLY[3:]
    )
    check("clean tree reads 'clean'", "(clean)" in clean.describe(), True)

    incompatible = devices.parse_fw_version_response(bytes([99, 0]) + _FW_VERSION_REPLY[2:])
    check("mismatched protocol_version parsed", incompatible.protocol_version, 99)
    check("mismatched protocol_version is incompatible", incompatible.compatible, False)
    check(
        "incompatible describe() flags it",
        "INCOMPATIBLE" in incompatible.describe(),
        True,
    )

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
    a, b = FakePort("HOST"), FakePort("ESP")
    a.peer, b.peer = b, a

    host = UartLink(own_device=Device.HOST, ack_timeout=0.3)
    esp = UartLink(own_device=Device.ESP, ack_timeout=0.3)
    esp_inbox = esp.register_task(UART_TASK_ID_INFO)
    _wire_up(host, a)
    _wire_up(esp, b)

    stop = threading.Event()

    def stub_info_bridge_task() -> None:
        """Stand-in for info_bridge_task(): reply to the *requester*."""
        while not stop.is_set():
            try:
                msg = esp_inbox.get(timeout=0.1)
            except Exception:
                continue
            if not msg.payload:
                continue
            if msg.payload[0] == INFO_CMD_GET_PIN_CONFIG:
                reply = _PIN_CONFIG_REPLY
            elif msg.payload[0] == INFO_CMD_GET_FW_VERSION:
                reply = _FW_VERSION_REPLY
            else:
                continue
            esp.send(
                dst_task=msg.src_task,
                src_task=UART_TASK_ID_INFO,
                payload=reply,
                dst_device=msg.src_device,
            )

    responder = threading.Thread(target=stub_info_bridge_task, daemon=True)
    responder.start()

    boot_pushes: list[devices.FirmwareVersion] = []
    client = InfoClient(host, on_boot_push=boot_pushes.append)
    try:
        got = client.get_pin_config(timeout=3.0)
        check("live pin config query", [e.gpio for e in got], [8, 9, 43, 44, 4, 5, 6, 18])
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


def expander_checks() -> None:
    """PCF8575 payload layouts, then the query flow over the virtual link.

    Same request/response shape as INFO (request ACKed, answer in a separate
    DATA frame), but with a self-describing reply -- byte0 echoes the
    subcommand -- so this also checks that the two reply layouts are told
    apart by that byte rather than structurally.
    """
    from uart_control.expander import ExpanderClient, ExpanderQueryError
    from uart_control.protocol import (
        PCF8575_CMD_READ_PORT,
        PCF8575_CMD_SCAN,
        UART_TASK_ID_PCF8575,
    )
    from uart_control.serial_link import UartLink

    print("\n== PCF8575 payload layouts (uart_task_ids.h) ==")
    check("task id PCF8575 == 7", UART_TASK_ID_PCF8575, 7)
    check(
        "write_port bytes (u16 LE)",
        devices.pcf8575_write_port(0xBEEF),
        b"\x01" + struct.pack("<H", 0xBEEF),
    )
    check("write_pin bytes", devices.pcf8575_write_pin(15, True), b"\x02\x0F\x01")
    check("set_mask bytes", devices.pcf8575_set_mask(0x0100), b"\x03\x00\x01")
    check("clear_mask bytes", devices.pcf8575_clear_mask(0x0001), b"\x04\x01\x00")
    check("toggle_mask bytes", devices.pcf8575_toggle_mask(0xFFFF), b"\x05\xff\xff")
    check("read_port request bytes", devices.pcf8575_read_port(), b"\x06")
    check("set_address bytes", devices.pcf8575_set_address(0x27), b"\x07\x27")
    check("scan request bytes", devices.pcf8575_scan(), b"\x08")
    check("all eight addresses offered", devices.PCF8575_ADDRESSES,
          (0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27))

    for label, fn in (
        ("pin 16 rejected", lambda: devices.pcf8575_write_pin(16, True)),
        ("port 0x10000 rejected", lambda: devices.pcf8575_write_port(0x10000)),
        ("address 0x28 rejected", lambda: devices.pcf8575_set_address(0x28)),
        ("address 0x1F rejected", lambda: devices.pcf8575_set_address(0x1F)),
    ):
        try:
            fn()
            check(label, False, True)
        except ValueError:
            check(label, True, True)

    # Replies as pcf8575_bridge_task() emits them.
    read_reply = bytes([PCF8575_CMD_READ_PORT]) + struct.pack("<HH", 0xFF0E, 0xFFFF) + bytes([0x21])
    subcommand, port = devices.parse_pcf8575_response(read_reply)
    check("classify READ_PORT reply", subcommand, PCF8575_CMD_READ_PORT)
    check("read port pins", hex(port.pins), hex(0xFF0E))
    check("read port shadow", hex(port.shadow), hex(0xFFFF))
    check("read port address", hex(port.addr), hex(0x21))
    check("pin 0 reads low (external pull-down)", port.pin(0), False)
    check("pin 1 reads high", port.pin(1), True)
    check("pin 0 not driven low by us", port.driven_low(0), False)

    scan_reply = bytes([PCF8575_CMD_SCAN, 2, 0x20, 0x24])
    subcommand, found = devices.parse_pcf8575_response(scan_reply)
    check("classify SCAN reply", subcommand, PCF8575_CMD_SCAN)
    check("scan addresses", found, [0x20, 0x24])

    for label, bad in (
        ("empty expander reply rejected", b""),
        ("short READ_PORT reply rejected", bytes([PCF8575_CMD_READ_PORT, 0, 0])),
        ("SCAN count mismatch rejected", bytes([PCF8575_CMD_SCAN, 3, 0x20])),
        ("unknown subcommand rejected", b"\x7f\x00"),
    ):
        try:
            devices.parse_pcf8575_response(bad)
            check(label, False, True)
        except devices.ExpanderResponseError:
            check(label, True, True)

    print("\n== PCF8575 query flow over the virtual link ==")
    a, b = FakePort("HOST"), FakePort("ESP")
    a.peer, b.peer = b, a

    host = UartLink(own_device=Device.HOST, ack_timeout=0.3)
    esp = UartLink(own_device=Device.ESP, ack_timeout=0.3)
    esp_inbox = esp.register_task(UART_TASK_ID_PCF8575)
    _wire_up(host, a)
    _wire_up(esp, b)

    stop = threading.Event()
    writes: list[bytes] = []

    def stub_pcf8575_bridge_task() -> None:
        """Stand-in for pcf8575_bridge_task(): answer queries, record writes."""
        while not stop.is_set():
            try:
                msg = esp_inbox.get(timeout=0.1)
            except Exception:
                continue
            if not msg.payload:
                continue
            if msg.payload[0] == PCF8575_CMD_READ_PORT:
                reply = read_reply
            elif msg.payload[0] == PCF8575_CMD_SCAN:
                reply = scan_reply
            else:
                writes.append(msg.payload)  # write-style subcommand: ACK only
                continue
            esp.send(
                dst_task=msg.src_task,
                src_task=UART_TASK_ID_PCF8575,
                payload=reply,
                dst_device=msg.src_device,
            )

    responder = threading.Thread(target=stub_pcf8575_bridge_task, daemon=True)
    responder.start()

    client = ExpanderClient(host)
    try:
        got = client.read_port(timeout=3.0)
        check("live read_port query", (hex(got.pins), hex(got.addr)), (hex(0xFF0E), hex(0x21)))
        check("live scan query", client.scan(timeout=3.0), [0x20, 0x24])

        from uart_control.serial_link import SendResult

        res = client.send(devices.pcf8575_write_pin(3, False))
        check("write-style subcommand ACKed", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not writes and time.time() < deadline:
            time.sleep(0.02)
        check("write reached the bridge", writes[:1], [b"\x02\x03\x00"])

        # Deaf peer: the request itself never gets through.
        a.drop_next_writes = 100
        host.max_retries = 2
        try:
            client.read_port(timeout=0.3)
            check("undelivered query raises", False, True)
        except ExpanderQueryError as exc:
            check("undelivered query raises", exc.send_result is not None, True)
    finally:
        stop.set()
        client.close()
        host.disconnect()
        esp.disconnect()


def log_checks() -> None:
    """LOG task payload parsing, then a live push over the virtual link."""
    from uart_control.device_log import LogClient
    from uart_control.protocol import UART_TASK_ID_LOG, LogLevel
    from uart_control.serial_link import SendResult, UartLink

    print("\n== LOG payload parsing (uart_task_ids.h) ==")
    check("task id LOG == 5", UART_TASK_ID_LOG, 5)

    line = devices.parse_log_frame(bytes([0]) + b"app_main: OLED init failed")
    check("level byte 0 -> ERROR", line.level, LogLevel.ERROR)
    check("text decoded", line.text, "app_main: OLED init failed")
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
    a, b = FakePort("HOST"), FakePort("ESP")
    a.peer, b.peer = b, a

    host = UartLink(own_device=Device.HOST, ack_timeout=0.3)
    esp = UartLink(own_device=Device.ESP, ack_timeout=0.3)
    _wire_up(host, a)
    _wire_up(esp, b)

    received: list[devices.LogLine] = []
    client = LogClient(host, on_line=received.append)
    try:
        # Firmware side never expects a reply -- same fire-and-forget shape
        # as uart_log_bridge_task() in uart_log_bridge.c.
        res = esp.send(
            dst_task=UART_TASK_ID_LOG,
            src_task=UART_TASK_ID_LOG,
            payload=bytes([LogLevel.WARN]) + b"SSD1306: i2c_master_bus_add_device failed",
            dst_device=Device.HOST,
        )
        check("unsolicited LOG frame delivered", res, SendResult.OK)
        deadline = time.time() + 2.0
        while not received and time.time() < deadline:
            time.sleep(0.02)
        check("LogClient received one line", len(received), 1)
        if received:
            check("received line level", received[0].level, LogLevel.WARN)
            check(
                "received line text",
                received[0].text,
                "SSD1306: i2c_master_bus_add_device failed",
            )
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

    from uart_control.link_hub import LinkHub, RemoteUartLink

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
        # traffic, with healthy hardware. Asserted as a property rather than
        # by idling for real, so this stays instant.
        check("client socket is blocking (no lingering connect timeout)",
              client_a._sock.gettimeout(), None)

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
    verifies every registered action is well-formed, that a spot-check of
    real actions (across DAC/AD9833/OLED/INFO, plus the OLED compound
    action) actually goes over the wire and back, and -- the safety-critical
    part -- that the protocol-version compatibility gate genuinely blocks
    device commands both before compatibility is known and after an
    incompatible version is observed, not just when explicitly told to.
    """
    from uart_control import actions
    from uart_control.expander import ExpanderClient
    from uart_control.info import InfoClient
    from uart_control.protocol import (
        INFO_CMD_GET_FW_VERSION,
        INFO_CMD_GET_PIN_CONFIG,
        PCF8575_CMD_READ_PORT,
        UART_TASK_ID_INFO,
        UART_TASK_ID_PCF8575,
    )
    from uart_control.serial_link import UartLink

    print("\n== action registry (actions.py) ==")
    check("registry non-empty", len(actions.ACTIONS) > 0, True)
    for name, action in actions.ACTIONS.items():
        check(f"action {name!r} has a description", bool(action.description), True)
        check(f"action {name!r} is callable", callable(action.run), True)

    a, b = FakePort("HOST"), FakePort("ESP")
    a.peer, b.peer = b, a
    host = UartLink(own_device=Device.HOST, ack_timeout=0.3)
    esp = UartLink(own_device=Device.ESP, ack_timeout=0.3)
    # Stand in for the real firmware bridge tasks: just registering these
    # task ids is enough for uart_protocol's own ACK/NACK logic to answer
    # DAC/AD9833/OLED sends with OK instead of UNDELIVERABLE.
    for task_id in (devices.UART_TASK_ID_DAC, devices.UART_TASK_ID_AD9833, devices.UART_TASK_ID_OLED):
        esp.register_task(task_id)
    esp_info_inbox = esp.register_task(UART_TASK_ID_INFO)
    esp_expander_inbox = esp.register_task(UART_TASK_ID_PCF8575)
    _wire_up(host, a)
    _wire_up(esp, b)

    stop = threading.Event()

    def stub_info_bridge_task() -> None:
        while not stop.is_set():
            try:
                msg = esp_info_inbox.get(timeout=0.1)
            except Exception:
                continue
            if not msg.payload:
                continue
            if msg.payload[0] == INFO_CMD_GET_PIN_CONFIG:
                reply = _PIN_CONFIG_REPLY
            elif msg.payload[0] == INFO_CMD_GET_FW_VERSION:
                reply = _FW_VERSION_REPLY
            else:
                continue
            esp.send(
                dst_task=msg.src_task,
                src_task=UART_TASK_ID_INFO,
                payload=reply,
                dst_device=msg.src_device,
            )

    def stub_pcf8575_bridge_task() -> None:
        """Answer READ_PORT; every other subcommand is ACK-only."""
        while not stop.is_set():
            try:
                msg = esp_expander_inbox.get(timeout=0.1)
            except Exception:
                continue
            if not msg.payload or msg.payload[0] != PCF8575_CMD_READ_PORT:
                continue
            esp.send(
                dst_task=msg.src_task,
                src_task=UART_TASK_ID_PCF8575,
                payload=bytes([PCF8575_CMD_READ_PORT])
                + struct.pack("<HH", 0xFFFE, 0xFFFF)
                + bytes([0x20]),
                dst_device=msg.src_device,
            )

    responder = threading.Thread(target=stub_info_bridge_task, daemon=True)
    responder.start()
    expander_responder = threading.Thread(target=stub_pcf8575_bridge_task, daemon=True)
    expander_responder.start()

    info_client = InfoClient(host)
    expander_client = ExpanderClient(host)
    ctx = actions.ActionContext(
        link=host, info=info_client, session_log=_FakeSessionLog(), expander=expander_client
    )

    try:
        print("\n== action registry: live virtual-link exercise ==")

        result = actions.ACTIONS["OLED: Clear"].run(ctx)
        check(
            "device command blocked before compatibility is known",
            result.startswith("error: refused"),
            True,
        )

        version_text = actions.ACTIONS["INFO: Get FW Version"].run(ctx)
        check("INFO: Get FW Version reports compatible", "compatible: yes" in version_text, True)
        check("InfoClient.compatible now True", info_client.compatible, True)

        result = actions.ACTIONS["OLED: Clear"].run(ctx)
        check("OLED: Clear succeeds once compatible", result.startswith("ok"), True)

        result = actions.ACTIONS["DAC: Set Channel Percent"].run(ctx, channel=0, percent=50.0)
        check("DAC: Set Channel Percent succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["AD9833: Set Frequency"].run(ctx, reg=0, freq_hz=1000.0)
        check("AD9833: Set Frequency succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["OLED: Write & Show"].run(ctx, text="hi")
        check("OLED: Write & Show (compound) succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Expander: Write Pin"].run(ctx, pin=2, level=False)
        check("Expander: Write Pin succeeds", result.startswith("ok"), True)

        result = actions.ACTIONS["Expander: Read Port"].run(ctx)
        check("Expander: Read Port returns device data", "addr 0x20" in result, True)

        pin_text = actions.ACTIONS["INFO: Get Pin Config"].run(ctx)
        check("INFO: Get Pin Config returns entries", "GPIO8" in pin_text, True)

        # Bad args must not crash press_button's caller -- TypeError/ValueError
        # from a wrong/missing parameter should be catchable, not a hard raise
        # out of run(). mcp_server.press_button relies on exactly this.
        try:
            actions.ACTIONS["DAC: Set Channel Percent"].run(ctx, channel=0)  # missing percent
            check("missing required arg raises", False, True)
        except TypeError:
            check("missing required arg raises", True, True)

        # Now simulate a firmware speaking a different protocol version and
        # confirm the gate re-engages, proving this isn't only checked once.
        bad_version = devices.parse_fw_version_response(bytes([99, 0]) + _FW_VERSION_REPLY[2:])
        info_client.last_fw_version = bad_version
        result = actions.ACTIONS["OLED: Display"].run(ctx)
        check(
            "device command blocked after incompatible version observed",
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
        info_client.close()
        expander_client.close()
        host.disconnect()
        esp.disconnect()


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
    payload = devices.dac_set_channel_percent(2, 42.5)
    frame = Frame(
        msg_type=MsgType.DATA,
        msg_index=0x1234,
        src_device=Device.HOST,
        src_task=devices.UART_TASK_ID_DAC,
        dst_device=Device.ESP,
        dst_task=devices.UART_TASK_ID_DAC,
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

    print("\n== payload layouts (uart_task_ids.h) ==")
    p = devices.dac_set_channel_percent(1, 75.0)
    check("DAC set_channel_percent len", len(p), 6)
    check("DAC set_channel_percent bytes", p, b"\x01\x01" + struct.pack("<f", 75.0))
    p = devices.dac_set_all_percent([0.0, 25.0, 50.0, 100.0])
    check("DAC set_all_percent len (bridge needs >=17)", len(p), 17)
    check(
        "DAC set_all_percent floats LE",
        struct.unpack("<4f", p[1:]),
        (0.0, 25.0, 50.0, 100.0),
    )
    check("DAC power_down bytes", devices.dac_power_down(3, 2), b"\x03\x03\x02")

    p = devices.ad9833_set_frequency(1, 1234.5)
    check("AD9833 set_frequency len (bridge needs >=10)", len(p), 10)
    check("AD9833 set_frequency bytes", p, b"\x01\x01" + struct.pack("<d", 1234.5))
    p = devices.ad9833_set_phase(0, 90.0)
    check("AD9833 set_phase bytes", p, b"\x02\x00" + struct.pack("<d", 90.0))
    check("AD9833 set_waveform bytes", devices.ad9833_set_waveform(2), b"\x03\x02")
    check("AD9833 select_freq_reg bytes", devices.ad9833_select_freq_reg(1), b"\x04\x01")
    check("AD9833 select_phase_reg bytes", devices.ad9833_select_phase_reg(0), b"\x05\x00")
    check("AD9833 reset bytes", devices.ad9833_reset(True), b"\x06\x01")
    check("AD9833 sleep bytes", devices.ad9833_sleep(False, True), b"\x07\x00\x01")

    check("task id SYSTEM == 6", devices.UART_TASK_ID_SYSTEM, 6)
    check("SYSTEM restart_uart bytes", devices.system_restart_uart(), b"\x01")

    print("\n== validation ==")
    for label, fn in (
        ("channel 4 rejected", lambda: devices.dac_set_channel_percent(4, 0)),
        ("percent 101 rejected", lambda: devices.dac_set_channel_percent(0, 101)),
        ("reg 2 rejected", lambda: devices.ad9833_set_frequency(2, 1.0)),
        ("waveform 9 rejected", lambda: devices.ad9833_set_waveform(9)),
    ):
        try:
            fn()
            check(label, False, True)
        except ValueError:
            check(label, True, True)

    print("\n== virtual link: two UartLinks cross-wired (no hardware) ==")
    loopback_checks()

    info_checks()

    expander_checks()

    log_checks()

    link_hub_checks()

    actions_checks()

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
