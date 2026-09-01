"""INFO wire encode/decode primitives.

Part of the devices.py split (pure refactor) -- see devices.py's module
docstring for the overall map. Moved verbatim, no logic changes.
"""
from __future__ import annotations

import struct
from dataclasses import dataclass

from .protocol import *  # noqa: F401,F403


# ---------------------------------------------------------------------------
# INFO queries (task_id = UART_TASK_ID_INFO)
#
# These are the one place where the firmware talks back with no subcommand
# byte at all. Requests are trivial (one subcommand byte, no args); the
# interesting code is the response parsing below, which mirrors
# build_pin_config_reply() and build_fw_version_reply() in uart_bridge.c byte
# for byte.
#
# Note what is NOT on the wire: a response carries no subcommand/type byte, so
# a received INFO frame is not self-describing. parse_info_response() below
# classifies structurally; see its docstring.
# ---------------------------------------------------------------------------
def info_get_pin_config() -> bytes:
    """0x01 GET_PIN_CONFIG request: byte0 = subcommand, no args."""
    return struct.pack("<B", INFO_CMD_GET_PIN_CONFIG)


def info_get_fw_version() -> bytes:
    """0x02 GET_FW_VERSION request: byte0 = subcommand, no args."""
    return struct.pack("<B", INFO_CMD_GET_FW_VERSION)


def info_get_wifi_status() -> bytes:
    """0x03 GET_WIFI_STATUS request: byte0 = subcommand, no args."""
    return struct.pack("<B", INFO_CMD_GET_WIFI_STATUS)


def info_get_stack_margin() -> bytes:
    """0x04 GET_STACK_MARGIN request: byte0 = subcommand, no args.

    See stack_margin.h (App/drivers) and KilnFW TODO.md section 13 for what
    this exists to unblock: six internal-only task stacks that must not be
    resized "from the numbers in this entry alone," pending a real
    uxTaskGetStackHighWaterMark() reading.
    """
    return struct.pack("<B", INFO_CMD_GET_STACK_MARGIN)


class InfoResponseError(ValueError):
    """Raised when an INFO response payload does not match its wire layout."""


@dataclass(frozen=True)
class PinConfigEntry:
    """One ``{gpio, function_id}`` pair from a GET_PIN_CONFIG response."""

    gpio: int
    function_id: int

    @property
    def label(self) -> str:
        """Full human-readable description, e.g. "SPI MISO (shared: ...)"."""
        return pin_function_label(self.function_id)

    @property
    def abbrev(self) -> str:
        """Short callout text, e.g. "MISO"."""
        return pin_function_abbrev(self.function_id)


@dataclass(frozen=True)
class FirmwareVersion:
    """Decoded GET_FW_VERSION response (build_fw_version_reply)."""

    protocol_version: int
    dirty: bool
    commit: str
    built: str

    @property
    def compatible(self) -> bool:
        """Whether this firmware speaks the same wire protocol we do.

        Equality, not >=: UART_PROTOCOL_VERSION only changes for
        wire-incompatible changes (see the bump policy in uart_task_ids.h),
        so *any* mismatch -- newer or older -- means the two sides can
        disagree about task_id/subcommand numbering or payload layouts. v1
        firmware is the unit-test fixture, where task 1 is a DAC rather than
        three thermocouples: there is no meaningful "forward compatible" case
        to special-case.
        """
        return self.protocol_version == UART_PROTOCOL_VERSION

    def describe(self) -> str:
        """One-line summary for the status bar, e.g.
        ``FW a1b2c3d (clean) built 2026-08-06 12:34:56Z``."""
        text = (
            f"FW {self.commit or '?'} ({'dirty' if self.dirty else 'clean'}) "
            f"built {self.built or '?'}"
        )
        if not self.compatible:
            text += (
                f" -- INCOMPATIBLE protocol v{self.protocol_version} "
                f"(pc_tools speaks v{UART_PROTOCOL_VERSION})"
            )
        return text


@dataclass(frozen=True)
class WifiStatus:
    """Decoded GET_WIFI_STATUS response (build_wifi_status_reply).

    ``ip`` is empty whenever ``connected`` is False -- there is nothing to
    report, not a malformed reply (mirrors wifi_prov_get_sta_ip's own
    "empty string, not a failure" convention).
    """

    connected: bool
    ip: str

    @property
    def url(self) -> "str | None":
        """``http://<ip>/`` for opening the dashboard, or None if not
        connected."""
        return f"http://{self.ip}/" if self.connected and self.ip else None


def parse_wifi_status_response(payload: bytes) -> WifiStatus:
    """Decode a GET_WIFI_STATUS response payload.

    Layout (build_wifi_status_reply)::

        byte0    connected flag (0/1)
        byte1    ip_len (N, 0 if not connected)
        N bytes  station IP, dotted-quad ASCII, NOT null-terminated

    Raises :class:`InfoResponseError` if the length does not match N exactly.
    """
    if len(payload) < 2:
        raise InfoResponseError(
            f"wifi status response too short: {len(payload)} bytes (need >= 2)"
        )
    connected = payload[0]
    if connected > 1:
        raise InfoResponseError(f"wifi status connected flag must be 0 or 1, got {connected}")
    ip_len = payload[1]
    expected = 2 + ip_len
    if len(payload) != expected:
        raise InfoResponseError(
            f"wifi status length mismatch: ip_len={ip_len} implies {expected} bytes, "
            f"got {len(payload)}"
        )
    ip = payload[2:expected].decode("ascii", errors="replace")
    return WifiStatus(connected=bool(connected), ip=ip)


@dataclass(frozen=True)
class StackMarginEntry:
    """One task's entry from a GET_STACK_MARGIN response
    (build_stack_margin_reply in uart_bridge.c).

    ``hwm_bytes`` is the SMALLEST amount of stack ever seen free since the
    task started (uxTaskGetStackHighWaterMark(), converted from FreeRTOS
    stack words to bytes on the ESP side -- see stack_margin_calc.h's
    STACK_MARGIN_WORD_BYTES comment), not the current free amount. Both
    ``hwm_bytes`` and ``level`` are 0/OK when ``alive`` is False -- the task
    was never created, creation failed, or it has since been deleted --
    never a stale prior reading.
    """

    name: str
    configured_stack_bytes: int
    hwm_bytes: int
    alive: bool
    level: StackMarginLevel

    @property
    def headroom_pct(self) -> "float | None":
        """``hwm_bytes`` as a percentage of ``configured_stack_bytes``, or
        None if not alive or the configured size is somehow 0 (mirrors
        stack_margin_classify()'s own "0 is never OK" rule -- there is no
        honest percentage to report against a zero-size stack)."""
        if not self.alive or self.configured_stack_bytes == 0:
            return None
        return 100.0 * self.hwm_bytes / self.configured_stack_bytes

    def describe(self) -> str:
        """One-line summary, e.g. ``rules_task: 1024/3072 B free (33%) OK``."""
        if not self.alive:
            return f"{self.name}: not running"
        pct = self.headroom_pct
        pct_str = f"{pct:.0f}%" if pct is not None else "?"
        return (
            f"{self.name}: {self.hwm_bytes}/{self.configured_stack_bytes} B free "
            f"({pct_str}) {self.level.name}"
        )


def parse_stack_margin_response(payload: bytes) -> list[StackMarginEntry]:
    """Decode a GET_STACK_MARGIN response payload.

    Layout (build_stack_margin_reply)::

        byte0     count (N)
        N * {
            u8   name_len (Nn)
            Nn   ASCII task name, NOT null-terminated
            u32  configured_stack_bytes, LE
            u32  hwm_bytes, LE
            u8   flags: bit0 alive, bits1-2 level (StackMarginLevel)
        }

    Raises :class:`InfoResponseError` if the length does not match N exactly,
    any name_len overruns the payload, or a level nibble is out of range.
    """
    if len(payload) < 1:
        raise InfoResponseError("stack margin response is empty")
    count = payload[0]
    entries: list[StackMarginEntry] = []
    offset = 1
    for _ in range(count):
        if offset >= len(payload):
            raise InfoResponseError(
                f"stack margin response truncated: expected {count} entries, "
                f"ran out of bytes after {len(entries)}"
            )
        name_len = payload[offset]
        offset += 1
        name_end = offset + name_len
        entry_end = name_end + 9  # configured(4) + hwm(4) + flags(1)
        if entry_end > len(payload):
            raise InfoResponseError(
                f"stack margin entry {len(entries)} overruns {len(payload)}-byte "
                f"payload (name_len={name_len})"
            )
        name = payload[offset:name_end].decode("ascii", errors="replace")
        configured_stack_bytes, hwm_bytes, flags = struct.unpack(
            "<IIB", payload[name_end:entry_end]
        )
        alive = bool(flags & 0x01)
        level_val = (flags >> 1) & 0x03
        try:
            level = StackMarginLevel(level_val)
        except ValueError as exc:
            raise InfoResponseError(
                f"stack margin entry {len(entries)} ('{name}'): unrecognized "
                f"level {level_val}"
            ) from exc
        entries.append(
            StackMarginEntry(
                name=name,
                configured_stack_bytes=configured_stack_bytes,
                hwm_bytes=hwm_bytes,
                alive=alive,
                level=level,
            )
        )
        offset = entry_end
    if offset != len(payload):
        raise InfoResponseError(
            f"stack margin response has {len(payload) - offset} trailing byte(s) "
            f"after {count} entries"
        )
    return entries


def parse_pin_config_response(payload: bytes) -> list[PinConfigEntry]:
    """Decode a GET_PIN_CONFIG response payload.

    Layout (build_pin_config_reply)::

        byte0            entry_count (N)
        N * { u8 gpio, u8 function_id }

    Raises :class:`InfoResponseError` if the length does not match N exactly.
    """
    if len(payload) < 1:
        raise InfoResponseError("pin config response is empty")
    count = payload[0]
    expected = 1 + count * 2
    if len(payload) != expected:
        raise InfoResponseError(
            f"pin config length mismatch: entry_count={count} implies {expected} "
            f"bytes, got {len(payload)}"
        )
    return [
        PinConfigEntry(gpio=payload[1 + i * 2], function_id=payload[1 + i * 2 + 1])
        for i in range(count)
    ]


def parse_fw_version_response(payload: bytes) -> FirmwareVersion:
    """Decode a GET_FW_VERSION response payload.

    Layout (build_fw_version_reply)::

        byte0-1      UART_PROTOCOL_VERSION, u16 LE -- fixed offset across all
                     versions; read and compare this *before* trusting
                     anything else in the payload, since an incompatible
                     peer's idea of what follows may not match this layout
                     at all
        byte2        dirty flag (0 = clean, 1 = dirty or unknown)
        byte3        commit_len (N1)
        N1 bytes     git commit, ASCII, NOT null-terminated
        byte(4+N1)   datetime_len (N2)
        N2 bytes     "YYYY-MM-DD HH:MM:SSZ", ASCII, NOT null-terminated

    Raises :class:`InfoResponseError` if any length field overruns the payload
    or if trailing bytes are left over. If ``protocol_version`` doesn't match
    :data:`~kilnctrl.protocol.UART_PROTOCOL_VERSION`, the rest of the
    payload is parsed on a best-effort basis (it may not even be this
    layout) -- callers MUST check ``.compatible`` before trusting anything
    beyond the version number itself.
    """
    if len(payload) < 2:
        raise InfoResponseError(
            f"fw version response too short: {len(payload)} bytes (need >= 2 "
            "just to read the protocol version)"
        )
    protocol_version = payload[0] | (payload[1] << 8)

    if len(payload) < 5:
        raise InfoResponseError(
            f"fw version response too short: {len(payload)} bytes (need >= 5)"
        )
    dirty = payload[2]
    if dirty > 1:
        raise InfoResponseError(f"fw version dirty flag must be 0 or 1, got {dirty}")

    commit_len = payload[3]
    commit_end = 4 + commit_len
    if commit_end >= len(payload):
        raise InfoResponseError(
            f"fw version commit_len={commit_len} overruns {len(payload)}-byte payload"
        )
    commit = payload[4:commit_end].decode("ascii", errors="replace")

    datetime_len = payload[commit_end]
    datetime_end = commit_end + 1 + datetime_len
    if datetime_end != len(payload):
        raise InfoResponseError(
            f"fw version datetime_len={datetime_len} implies {datetime_end} bytes, "
            f"got {len(payload)}"
        )
    built = payload[commit_end + 1 : datetime_end].decode("ascii", errors="replace")

    return FirmwareVersion(
        protocol_version=protocol_version, dirty=bool(dirty), commit=commit, built=built
    )


def parse_info_response(
    payload: bytes, prefer: int | None = None
) -> "tuple[int, list[PinConfigEntry] | FirmwareVersion | WifiStatus | list[StackMarginEntry]]":
    """Classify and decode an INFO response payload structurally.

    Returns ``(subcommand, value)`` where subcommand is
    :data:`~kilnctrl.protocol.INFO_CMD_GET_PIN_CONFIG` (value = list of
    :class:`PinConfigEntry`),
    :data:`~kilnctrl.protocol.INFO_CMD_GET_FW_VERSION` (value =
    :class:`FirmwareVersion`),
    :data:`~kilnctrl.protocol.INFO_CMD_GET_WIFI_STATUS` (value =
    :class:`WifiStatus`), or
    :data:`~kilnctrl.protocol.INFO_CMD_GET_STACK_MARGIN` (value = list of
    :class:`StackMarginEntry`).

    Why this is needed: uart_bridge.c's replies carry *no* subcommand/type
    byte -- ``build_pin_config_reply`` starts with the entry count and
    ``build_fw_version_reply`` starts with the protocol version (currently
    2, i.e. bytes ``02 00``). Both arrive as plain DATA frames on task INFO,
    so the payload alone is not self-describing and the layouts are only
    distinguishable by whether their internal length fields add up. A real
    version reply starts with 2 and is far too short to be a 2-entry pin
    config (which would need 5 bytes and a plausible function id), but the
    caller can pass ``prefer`` -- the subcommand it currently has
    outstanding -- to settle any tie deterministically. GET_WIFI_STATUS and
    GET_STACK_MARGIN are never sent unsolicited, so they only ever get
    classified via ``prefer`` -- an empty ``build_stack_margin_reply()``
    (nothing registered) is the single byte ``00``, byte-identical to an
    empty ``build_pin_config_reply()``, and only ``prefer`` breaks that tie.

    Raises :class:`InfoResponseError` if the payload fits no known layout.
    """
    parsers = (
        (INFO_CMD_GET_FW_VERSION, parse_fw_version_response),
        (INFO_CMD_GET_PIN_CONFIG, parse_pin_config_response),
        (INFO_CMD_GET_WIFI_STATUS, parse_wifi_status_response),
        (INFO_CMD_GET_STACK_MARGIN, parse_stack_margin_response),
    )
    if prefer is not None:
        parsers = tuple(sorted(parsers, key=lambda p: p[0] != prefer))

    errors = []
    for subcommand, parser in parsers:
        try:
            return subcommand, parser(payload)
        except InfoResponseError as exc:
            errors.append(str(exc))
    raise InfoResponseError(
        f"{len(payload)}-byte INFO response matches no known layout: "
        + "; ".join(errors)
    )


