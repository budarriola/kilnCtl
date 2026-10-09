#!/usr/bin/env python3
"""Standalone sanity check for the KilnCtrl wire protocol implementation.

Run with:  uv run --project tools/PcTools python tools/PcTools/selfcheck.py

Verifies CRC-16/CCITT-FALSE against the standard check value, round-trips
stuffing/unstuffing (including bytes that must be escaped), exercises the
incremental FrameDecoder, and checks every device payload byte layout against
App/drivers/common/uart_task_ids.h.

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
import os
import pathlib
import re
import struct
import sys
import threading
import time

from kilnctrl import devices, pin_overlay
from mcpkit.registry import check_staleness, take_snapshot
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

from selfcheck_common import check, _failures  # noqa: F401

# ---------------------------------------------------------------------------
# The rest of this module used to live here inline. It is now split into
# per-subsystem submodules (pure refactor, zero behavior change) -- moved
# verbatim, see each submodule's docstring, and selfcheck_common.py for the
# shared FakePort/_make_pair/_responder/check test infrastructure they all
# build on (kept out of this file specifically so it has no import back on
# selfcheck.py -- see that module's docstring for why).
# ---------------------------------------------------------------------------
from selfcheck_loopback import loopback_checks  # noqa: F401
from selfcheck_info import info_checks  # noqa: F401
from selfcheck_thermo import thermo_checks  # noqa: F401
from selfcheck_io import io_checks  # noqa: F401
from selfcheck_display import display_checks  # noqa: F401
from selfcheck_safety import safety_checks  # noqa: F401
from selfcheck_log import log_checks  # noqa: F401
from selfcheck_link_hub import link_hub_checks  # noqa: F401
from selfcheck_actions import actions_checks  # noqa: F401
from selfcheck_commonfw import commonfw_vector_checks, commonfw_payload_vector_checks  # noqa: F401
from selfcheck_hardening import hardening_checks  # noqa: F401
from selfcheck_zones_fields import (  # noqa: F401
    zones_field_table_checks,
    zones_per_zone_field_table_checks,
)

# Same cross-language-pin idiom as tests/test_ramp_assist.py's
# RampAssistLagBandCrossLanguageTest: parse the firmware header's #define
# directly rather than hand-copying its value into a literal here. A
# hardcoded literal is exactly what went stale before (this check sat at
# "== 5" long after uart_task_ids.h moved to 10, silently failing ten
# downstream checks that depend on FirmwareVersion.compatible) -- asserting
# devices.UART_PROTOCOL_VERSION against uart_task_ids.h's own #define turns
# "someone forgot to update selfcheck" into "PC and firmware disagree",
# which is the failure actually worth detecting.
_UART_TASK_IDS_H_PATH = (
    pathlib.Path(__file__).resolve().parents[2]
    / "firmware" / "KilnFW" / "App" / "drivers" / "common" / "uart_task_ids.h"
)
_UART_PROTOCOL_VERSION_RE = re.compile(
    r"#define\s+UART_PROTOCOL_VERSION\s+\(\(uint16_t\)\s*([0-9]+)\)"
)


def _firmware_uart_protocol_version() -> int:
    text = _UART_TASK_IDS_H_PATH.read_text(encoding="utf-8")
    m = _UART_PROTOCOL_VERSION_RE.search(text)
    if m is None:
        raise AssertionError(
            "UART_PROTOCOL_VERSION #define not found in "
            f"{_UART_TASK_IDS_H_PATH} -- parser or macro spelling is broken"
        )
    return int(m.group(1))


#: B12: tests/test_mcpkit_vendored_copy.py already asserts (via pytest) that
#: tools/mykicadMcp/mcpkit_registry.py is byte-identical (line-ending
#: normalized) to tools/PcTools/src/mcpkit/registry.py. selfcheck.py runs in
#: contexts where pytest doesn't (see this module's docstring/run line), so
#: mirror that one guard here too -- a one-line diff, not a duplicate of the
#: pytest suite's fuller assertions (missing-source failure, skip-on-
#: uninitialized-submodule).
_MCPKIT_REGISTRY_SOURCE_PATH = (
    pathlib.Path(__file__).resolve().parents[2]
    / "tools" / "PcTools" / "src" / "mcpkit" / "registry.py"
)
_MCPKIT_REGISTRY_VENDORED_PATH = (
    pathlib.Path(__file__).resolve().parents[2]
    / "tools" / "mykicadMcp" / "mcpkit_registry.py"
)


def _mcpkit_vendored_copy_check() -> None:
    if not _MCPKIT_REGISTRY_VENDORED_PATH.is_file():
        print("  (skipped: tools/mykicadMcp submodule not checked out)")
        return
    source_text = _MCPKIT_REGISTRY_SOURCE_PATH.read_text(encoding="utf-8")
    vendored_text = _MCPKIT_REGISTRY_VENDORED_PATH.read_text(encoding="utf-8")
    check(
        "tools/mykicadMcp/mcpkit_registry.py matches tools/PcTools/src/mcpkit/registry.py",
        vendored_text,
        source_text,
    )


def _mcpkit_staleness_checks() -> None:
    """mcpkit.registry's stale-server detection (see docs/MCP_SERVERS.md),
    exercised against a real temp directory rather than mocked file objects
    -- everything else in this module's staleness logic (mtime provider
    injection) is covered directly by tools/PcTools/tests/
    test_mcpkit_freshness.py; this is a smoke test that the two halves
    (take_snapshot's directory walk, check_staleness's re-stat) still agree
    when wired together against real files on disk."""
    import tempfile

    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "mod.py")
        with open(src, "w", encoding="utf-8") as f:
            f.write("x = 1\n")
        snap = take_snapshot(tmp)
        stale, changed = check_staleness(snap)
        check("fresh snapshot reports no changed files", (stale, changed), (False, 0))

        # mtimes have whole-second resolution on some filesystems -- bump the
        # written mtime explicitly rather than sleeping, so this is fast and
        # not flaky under load.
        newer = os.path.getmtime(src) + 5.0
        os.utime(src, (newer, newer))
        stale, changed = check_staleness(snap)
        check("edited file after snapshot is detected as stale", (stale, changed), (True, 1))

        os.remove(src)
        stale, changed = check_staleness(snap)
        check("deleted file after snapshot is detected as stale", (stale, changed), (True, 1))


def main() -> int:
    print("== mcpkit stale-server detection ==")
    _mcpkit_staleness_checks()

    print("\n== mcpkit vendored-registry copy (mykicadMcp) ==")
    _mcpkit_vendored_copy_check()

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
    check(
        "protocol version matches uart_task_ids.h's #define",
        devices.UART_PROTOCOL_VERSION,
        _firmware_uart_protocol_version(),
    )
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

    print("\n== zones top-level field table vs firmware (zones_http_get.c / zones_http_post.c) ==")
    zones_field_table_checks()

    print("\n== zones per-zone field table vs firmware (zones_http_get.c / zones_http_post_parse.c) ==")
    zones_per_zone_field_table_checks()

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
