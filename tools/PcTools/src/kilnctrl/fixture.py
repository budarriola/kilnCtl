"""Client for the UnitTestFixture board's PCF8575 relay-control surface.

This is a **second, independent** board from the kilnCtl main controller:
its own ESP32-S3, its own USB-UART bridge, its own hardened UART link (same
wire framing as ``kilnctrl.protocol`` -- SLIP byte-stuffing, CRC-16/CCITT-
FALSE header, ACK/NACK/retry -- but a completely different task/subcommand
map, defined in ``firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md``).
Never share a link/port between this module and the main ``kilnctrl``
``UartLink`` -- they are two physical boards.

Only the PCF8575 I/O-expander task (task 7) is in scope here, per the
2026-09-05 owner decision (``docs/UNIT_TEST_FIXTURE_PLAN.md``): drive the
fixture's relays. The DAC/AD9833/OLED tasks that firmware also exposes are
out of scope and untouched.

As of that plan's authoring, the fixture's schematic
(``hardware/UnitTestFixture/UnitTestFixture.kicad_sch``) wires up two
PCF8575DBR expanders (U4, U5) with their P0x/P1x pins broken out but **not**
yet connected to any relay coil, connector, or thermocouple switch network --
the physical relay board this plan targets has not been laid out yet. The
relay map below is therefore a placeholder keyed by raw ``U<n>:P<pin>``
expander-pin names (2 expanders x 16 pins), not by function ("heater 1
open"), so it can be relabeled with zero code changes once real relay
wiring lands -- update :data:`DEFAULT_RELAY_MAP` (or pass a caller-supplied
map into :class:`FixtureClient`) at that point.

**Trust nothing the transport ACKed as actually having happened on the
device.** ``UartLink.send()`` returning ``SendResult.OK`` means the frame
reached the firmware task's inbox (serial_link.py's own
``SendResult.describe()``), not that the I2C transfer it triggered
succeeded, and not (for ``SET_ADDRESS``) that the target address actually
ACKed on the bus. ``PCF8575_set_address()`` can silently leave the firmware
on the OLD address (``ESP_ERR_NOT_FOUND``; uart_bridge.c's PCF8575 handler
sends no reply frame on that failure either -- the protocol ACK already
went out for "delivered"), and a driving transistor that never got built
can make a WRITE_PORT/WRITE_PIN's I2C leg fail the same silent way. Every
address switch and every relay write in this module is therefore followed
by a ``READ_PORT`` round trip that reads back both the live pins *and* the
address byte the firmware is actually talking to (``PCF8575.md``'s
``READ_PORT`` response, byte5), and refuses (raises
:class:`FixtureError`) rather than assuming success when that read-back
disagrees.
"""

from __future__ import annotations

import logging
import os
import queue
import threading
from dataclasses import dataclass
from typing import Optional

from .protocol import Device, Frame, MsgType  # noqa: F401 - re-exported for callers/tests
from .serial_link import SendResult, UartLink
from .serial_link import list_ports as _list_serial_ports
from .serial_link import is_main_board_port as _is_main_board_port

log = logging.getLogger(__name__)

# ---------------------------------------------------------------------------
# Wire constants -- fixture firmware's own protocol, NOT kilnctrl's.
# See firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md.
# ---------------------------------------------------------------------------
FIXTURE_BAUD_RATE = 115200
UART_TASK_ID_PCF8575 = 7

PCF8575_CMD_WRITE_PORT = 0x01
PCF8575_CMD_WRITE_PIN = 0x02
PCF8575_CMD_SET_MASK = 0x03
PCF8575_CMD_CLEAR_MASK = 0x04
PCF8575_CMD_TOGGLE_MASK = 0x05
PCF8575_CMD_READ_PORT = 0x06
PCF8575_CMD_SET_ADDRESS = 0x07
PCF8575_CMD_SCAN = 0x08

PCF8575_ADDR_MIN = 0x20
PCF8575_ADDR_MAX = 0x27
PCF8575_PIN_COUNT = 16

#: Env var override for the fixture's serial port, honored by connect() ahead
#: of recommend_fixture_port() -- the fallback FIXTURE_VID_PID_HINT's
#: docstring and the error message below both already promise.
KILNCTL_FIXTURE_PORT_ENV = "KILNCTL_FIXTURE_PORT"

#: USB VID:PID for the fixture's own USB-UART bridge: a CH340K, confirmed by
#: bench enumeration 2026-09-05 (both boards plugged in simultaneously):
#:   fixture   ESP32-S3 native USB-Serial-JTAG  303A:1001  SER=68:B6:B3:29:D0:B8 (COM7)
#:   fixture   CH340K UART                       1A86:7522  no serial number     (COM14)
#:   main board ESP32-S3 native USB-Serial-JTAG  303A:1001  SER=1C:DB:D4:92:F4:7C (COM3)
#:   main board CH343 UART                        1A86:55D3  SER=552E006806       (COM6)
#: The two boards' UART bridges are DIFFERENT silicon (CH340K vs CH343), so
#: VID:PID alone distinguishes them here -- unlike the two ESP32-S3 native
#: ports, which share 303A:1001 and are told apart only by serial number (see
#: mcp_server_flash.py's pinned adapter_serial for that side). The CH340K
#: itself reports no per-device serial (Windows still strips MI_xx from a
#: composite descriptor, but there is no serial to strip here in the first
#: place), so if a second CH340K-based board ever joins the bench, VID:PID
#: stops being sufficient and an explicit port will be required again -- see
#: docs/UNIT_TEST_FIXTURE_PLAN.md "PC connection identity".
FIXTURE_VID_PID_HINT = "1A86:7522"

#: Descriptor substrings that mean "definitely not the fixture's UART bridge"
#: -- the two CMSIS-DAP debug probes, both boards' JTAG/Serial-JTAG ports,
#: and the main board's own CH343 bridge (1A86:55D3 -- a different chip
#: family from the fixture's CH340K, but excluded by name too as a second,
#: independent check: see test_fixture.py's
#: test_recommend_never_picks_main_board_ch343). Kept separate from
#: kilnctrl's own _PORT_HINTS (serial_link.py) rather than imported, since
#: this module must not depend on that link's board-specific scoring
#: assumptions.
_EXCLUDE_HINTS = ("jtag", "cmsis-dap", "debug", "mbed", "55d3", "ch343")


class FixtureError(RuntimeError):
    """Raised for any fixture link/relay failure the caller should see."""


@dataclass(frozen=True)
class RelayId:
    """One PCF8575 pin, addressed by expander I2C address + pin number."""

    address: int
    pin: int


def _default_relay_map() -> "dict[str, RelayId]":
    """U4/U5, both expander pins, named generically -- see module docstring.

    U4 is assumed at the Kconfig/board default 0x20; U5's address is NOT
    confirmed from the schematic (no address-strap resistors were traced as
    part of this plan) and is provisionally 0x21 pending a bench SCAN. Do not
    trust this second address without running ``fixture_get_relays()`` (which
    surfaces the live SCAN) at least once per bench session.
    """
    relay_map: "dict[str, RelayId]" = {}
    for expander_index, address in ((4, 0x20), (5, 0x21)):
        for pin in range(PCF8575_PIN_COUNT):
            relay_map[f"U{expander_index}:P{pin:02d}"] = RelayId(address=address, pin=pin)
    return relay_map


#: Data-driven so a future real relay-board revision only has to replace this
#: dict (or pass its own into FixtureClient) -- no code changes.
DEFAULT_RELAY_MAP: "dict[str, RelayId]" = _default_relay_map()


#: Explicit VID:PID / serial exclusions, checked against ``hwid`` directly
#: (not the free-text description, which is not reliable -- see the reviewer
#: finding this replaced: a 303A:1001 port enumerating with a generic
#: description like "USB Serial Device" was NOT caught by the "jtag"
#: substring in ``_EXCLUDE_HINTS``, so the old text-only check could still
#: hand back a native ESP32-S3 JTAG port, main board's or the fixture's own,
#: as the fixture's UART bridge). Both boards' native USB-Serial-JTAG shares
#: 303A:1001, so that VID:PID is excluded unconditionally regardless of which
#: board it belongs to; the main board's CH343 bridge is excluded by its own
#: VID:PID and, redundantly, by its pinned serial number.
_EXCLUDE_HWID_TOKENS = ("303A:1001", "1A86:55D3", "552E006806")


def recommend_fixture_port() -> Optional[str]:
    """The fixture's CH340K UART bridge port, identified by VID:PID
    (:data:`FIXTURE_VID_PID_HINT`), or None if it is not present.

    Never falls back to "some other unexcluded port" -- a positive VID:PID
    match on the fixture's CH340K is required, or this returns None. Also
    excludes by VID:PID/serial directly (:data:`_EXCLUDE_HWID_TOKENS`), not
    just the free-text ``_EXCLUDE_HINTS`` substrings, since a 303A:1001 port
    reporting a generic description (no "JTAG" text at all) would otherwise
    slip past the text-only check and could be returned as if it were the
    fixture's own port.

    Does NOT positively confirm the fixture beyond that VID:PID match -- a
    second CH340K-based device on the bench would still be ambiguous, see
    FIXTURE_VID_PID_HINT's docstring. Prefer an explicit
    ``KILNCTL_FIXTURE_PORT``/``port=`` when in doubt.
    """
    hinted = []
    for info in _list_serial_ports():
        haystack = f"{info.description} {info.hwid}".lower()
        if any(bad in haystack for bad in _EXCLUDE_HINTS):
            continue
        hwid_upper = info.hwid.upper()
        if any(tok in hwid_upper for tok in _EXCLUDE_HWID_TOKENS):
            continue
        # Belt-and-suspenders on top of the checks above: cross-check
        # against serial_link's serial-number-anchored identity table (the
        # same one mcp_server_flash.py pins OpenOCD's `adapter serial` to).
        if _is_main_board_port(info):
            continue
        if FIXTURE_VID_PID_HINT.lower() in info.hwid.lower():
            hinted.append(info)
    if not hinted:
        return None
    return hinted[0].device


class _Pending:
    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None


def _pack_u16_le(value: int) -> bytes:
    return bytes((value & 0xFF, (value >> 8) & 0xFF))


def _unpack_u16_le(data: bytes, offset: int) -> int:
    return data[offset] | (data[offset + 1] << 8)


class FixtureClient:
    """Owns one :class:`~kilnctrl.serial_link.UartLink` to the UnitTestFixture
    board's PCF8575 task (task 7), plus a relay name -> pin map.

    De-energizes everything, WITH a verifying read-back, on connect and on
    disconnect/close, per the 2026-09-05 owner decision: this device should
    never be left mid-state between sessions, and a failed de-energize is
    raised loudly rather than assumed to have worked. "De-energize" here
    means writing every mapped pin to 1 (weak pull-up / quasi-bidirectional
    "input" state, the PCF8575's power-on default) -- see docs/PCF8575.md:
    whether that reads as a relay's coil OFF or ON on real hardware depends
    on the (not-yet-designed) drive polarity, so treat "all pins high" as
    the fixture's own rest state, not as a verified "all relays open" claim,
    until a bench pass confirms it.

    No client-side shadow of "what we last wrote" is kept: every operation
    that needs to know the device's state reads it back from the device,
    since a written-and-ACKed value cannot be trusted on its own (see module
    docstring).
    """

    def __init__(
        self,
        relay_map: Optional["dict[str, RelayId]"] = None,
        baudrate: int = FIXTURE_BAUD_RATE,
    ) -> None:
        self.relay_map = dict(relay_map if relay_map is not None else DEFAULT_RELAY_MAP)
        self._link = UartLink(baudrate=baudrate)
        self._inbox: Optional["queue.Queue[Frame]"] = None
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        self._query_lock = threading.RLock()
        self._stop = threading.Event()
        self._consumer: Optional[threading.Thread] = None
        #: which address the *firmware* was last CONFIRMED (by READ_PORT's
        #: own address byte, not just a bare ACK) to be talking to. None
        #: until confirmed, so the very first operation on any address
        #: always retargets and verifies rather than assuming the firmware's
        #: boot default or trusting a SET_ADDRESS that may not have taken.
        self._current_address: Optional[int] = None

    # -- lifecycle -----------------------------------------------------
    @property
    def is_connected(self) -> bool:
        return self._link.is_connected

    def connect(self, port: Optional[str] = None) -> str:
        if self.is_connected:
            raise FixtureError(f"already connected to {self._link.port}")
        resolved = port or os.environ.get(KILNCTL_FIXTURE_PORT_ENV) or recommend_fixture_port()
        if resolved is None:
            raise FixtureError(
                "no candidate serial port found for the fixture - pass port= "
                f"explicitly (or set {KILNCTL_FIXTURE_PORT_ENV})"
            )
        opened = self._link.connect(resolved)
        self._inbox = self._link.register_task(UART_TASK_ID_PCF8575)
        self._stop.clear()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="fixture-pcf8575-rx", daemon=True
        )
        self._consumer.start()
        try:
            self.all_off()
        except FixtureError:
            # Do NOT downgrade this to a warning: a fixture that cannot be
            # confirmed de-energized on connect must not be handed back to a
            # caller as "ready". Tear the link down so is_connected reflects
            # reality and the caller sees the failure.
            self._teardown_link()
            raise
        return opened

    def disconnect(self) -> None:
        if self.is_connected:
            try:
                self.all_off()
            except FixtureError:
                log.error(
                    "all-off before disconnect failed - relays may be left energized",
                    exc_info=True,
                )
        self._teardown_link()

    def _teardown_link(self) -> None:
        self._stop.set()
        if self._consumer is not None and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self._consumer = None
        if self._inbox is not None:
            self._link.unregister_task(UART_TASK_ID_PCF8575)
            self._inbox = None
        self._link.disconnect()
        self._current_address = None

    def close(self) -> None:
        """Alias kept for symmetry with the other *Client classes in this
        package (mcp_server.py's shutdown loop calls ``.close()`` on every
        client uniformly)."""
        self.disconnect()

    # -- relay map -------------------------------------------------------
    def list_relays(self) -> "list[str]":
        return sorted(self.relay_map)

    def _resolve(self, name: str) -> RelayId:
        try:
            return self.relay_map[name]
        except KeyError as exc:
            raise FixtureError(
                f"unknown relay {name!r} - known: {', '.join(sorted(self.relay_map))}"
            ) from exc

    # -- writes ------------------------------------------------------------
    def set_relay(self, name: str, on: bool) -> None:
        """Energize/de-energize one relay by name, verified by read-back.

        ``on=True`` drives the pin low (0); ``on=False`` releases it to the
        weak pull-up (1) -- see docs/PCF8575.md. Whether "pin low" maps to a
        relay's coil actually energizing depends on drive-circuit polarity
        not yet designed; this is the firmware-level convention only.

        Verification reads the field that is actually trustworthy for the
        direction attempted (PCF8575.c: only a bit *written* 0 is guaranteed
        to read back 0) -- an energize (``on=True``) is checked against the
        live INPUT register (``pins``); a release (``on=False``) is checked
        against the firmware's write ``shadow``, since a released pin's
        INPUT reading depends on whatever is externally wired and can
        legitimately read 0 on a populated board with nothing to do with
        this write failing.

        If an energize cannot be confirmed by read-back, this makes a
        best-effort ``all_off()`` before raising -- never leave a relay that
        might switch a heater/element in an unconfirmed, possibly still-
        energized state. A non-OK send on an energize attempt is treated the
        same way: TIMEOUT means retries were exhausted, not that the write
        definitely never reached the device.
        """
        relay = self._resolve(name)
        self._ensure_address(relay.address)
        level = 0 if on else 1
        payload = bytes((PCF8575_CMD_WRITE_PIN, relay.pin & 0xFF, level & 0xFF))
        result = self._send(payload)
        if result != SendResult.OK:
            self._fail_safe_after(
                on, f"set_relay({name!r}, {on}) not delivered: {result.describe()}"
            )
            return
        try:
            pins, shadow, _addr = self._read_port_raw()
        except FixtureError as exc:
            self._fail_safe_after(on, f"set_relay({name!r}, {on}) read-back failed: {exc}")
            return
        if on:
            energized = not bool(pins & (1 << relay.pin))
        else:
            energized = not bool(shadow & (1 << relay.pin))
        if energized != on:
            self._fail_safe_after(
                on,
                f"set_relay({name!r}, {on}) did not take - device reports "
                f"{'energized' if energized else 'de-energized'}",
            )

    def _fail_safe_after(self, was_energizing: bool, message: str) -> None:
        """Common tail of a failed verified write: if we were trying to
        energize something, attempt an all_off() before surfacing the
        original failure -- an unconfirmed relay state must never be left
        assumed safe just because it also failed to de-energize cleanly."""
        if was_energizing:
            try:
                self.all_off()
            except FixtureError:
                log.error("all_off() after a failed energize also failed", exc_info=True)
        raise FixtureError(message)

    def all_off(self) -> None:
        """De-energize (release) every mapped relay, verified by read-back
        per address.

        This is a RELEASE, so it is verified against the firmware's write
        ``shadow`` (== 0xFFFF), never against the live ``pins`` (INPUT)
        register: PCF8575.c only guarantees a bit *written* 0 reads back 0,
        so a pin released to the weak pull-up can legitimately read 0 on a
        populated board (something externally holding that line low) with
        nothing to do with this write failing. Checking ``pins`` here would
        make all_off() raise spuriously on real hardware -- and since
        connect() hard-fails on an all_off() it cannot confirm, that would
        make the fixture unusable the moment anything is actually wired to
        it. See module docstring / ``_read_port_raw``'s docstring.

        Raises (does not merely log) if a write isn't delivered, an address
        switch doesn't confirm, or the shadow read back afterwards
        disagrees.
        """
        addresses = sorted({relay.address for relay in self.relay_map.values()})
        for address in addresses:
            self._ensure_address(address)
            result = self._send(bytes((PCF8575_CMD_WRITE_PORT, 0xFF, 0xFF)))
            if result != SendResult.OK:
                raise FixtureError(f"all_off() on 0x{address:02X} not delivered: {result.describe()}")
            _pins, shadow, _addr = self._read_port_raw()
            if shadow != 0xFFFF:
                raise FixtureError(
                    f"all_off() on 0x{address:02X} did not take - device reports "
                    f"shadow=0x{shadow:04X}, expected 0xFFFF"
                )

    def _ensure_address(self, address: int) -> None:
        """SET_ADDRESS the firmware to ``address`` unless already CONFIRMED
        there, and confirm it via READ_PORT's own address byte. A SET_ADDRESS
        whose target never ACKs on the I2C bus gets no reply frame at all
        from the firmware (PCF8575_set_address() -> ESP_ERR_NOT_FOUND); the
        transport ACK alone only proves delivery to the inbox, never that the
        switch actually happened -- see module docstring."""
        if not (PCF8575_ADDR_MIN <= address <= PCF8575_ADDR_MAX):
            raise FixtureError(f"address 0x{address:02X} out of range 0x20-0x27")
        if self._current_address == address:
            return
        result = self._send(bytes((PCF8575_CMD_SET_ADDRESS, address & 0xFF)))
        if result != SendResult.OK:
            raise FixtureError(f"SET_ADDRESS(0x{address:02X}) not delivered: {result.describe()}")
        self._current_address = None  # unknown until confirmed below
        _pins, _shadow, confirmed = self._read_port_raw()
        if confirmed != address:
            raise FixtureError(
                f"SET_ADDRESS(0x{address:02X}) did not take - firmware reports it is "
                f"talking to 0x{confirmed:02X} (nothing ACKed at 0x{address:02X}?)"
            )
        self._current_address = address

    def _send(self, payload: bytes) -> SendResult:
        if not self.is_connected:
            return SendResult.NOT_CONNECTED
        return self._link.send(
            dst_task=UART_TASK_ID_PCF8575, src_task=UART_TASK_ID_PCF8575, payload=payload
        )

    # -- reads -------------------------------------------------------------
    def get_relays(self, timeout: float = 2.0) -> "dict[str, bool]":
        """Read back every mapped relay's live pin state via READ_PORT.

        One query per distinct address on the map. A pin currently driven
        low by us always reads 0 regardless of external wiring (PCF8575.md);
        a pin left at the weak pull-up reads whatever is externally wired.
        """
        addresses = sorted({relay.address for relay in self.relay_map.values()})
        pins_by_address: "dict[int, int]" = {}
        for address in addresses:
            self._ensure_address(address)
            pins, _shadow, _addr = self._read_port_raw(timeout)
            pins_by_address[address] = pins
        out: "dict[str, bool]" = {}
        for name, relay in self.relay_map.items():
            pins = pins_by_address.get(relay.address, 0)
            out[name] = not bool(pins & (1 << relay.pin))  # low == energized == True
        return out

    def _drain_inbox(self) -> None:
        """Discard any frames already sitting in this task's inbox queue,
        without blocking. Called right before a new query is armed so a
        stale reply to an earlier, already-abandoned (timed-out) query can
        never be mistaken for the answer to this one -- _Pending matches by
        subcommand only, not by a per-request tag/sequence number."""
        if self._inbox is None:
            return
        while True:
            try:
                self._inbox.get_nowait()
            except queue.Empty:
                return

    def _read_port_raw(self, timeout: float = 2.0) -> "tuple[int, int, int]":
        """READ_PORT the currently-addressed expander; returns ``(pins,
        shadow, addr)`` exactly as the firmware reports them (PCF8575.md's
        READ_PORT response: pins u16 LE, shadow u16 LE, then the address
        byte).

        ``pins`` is the live INPUT register -- PCF8575.c only guarantees a
        bit *written* 0 reads back 0; a bit released to the weak pull-up
        (written 1) reads back whatever is externally wired, which on a
        populated board can legitimately be 0 (something holding that line
        low). ``pins`` is therefore only trustworthy for confirming an
        ENERGIZE (driven low), never a release. ``shadow`` is the firmware's
        own record of the last value WRITTEN and is what a release must be
        checked against instead. Callers that care whether ``addr`` matches
        what they expected check it themselves (see ``_ensure_address``) --
        this makes no assumption on their behalf.
        """
        with self._query_lock:
            # Drain any already-buffered frame before starting a new query:
            # _Pending matches by subcommand only, so a late reply to a
            # PREVIOUS query that already timed out (its own _pending was
            # cleared, so _handle_reply dropped it as "unsolicited") could
            # otherwise still be sitting in the inbox queue and get consumed
            # here as noise -- harmless on its own, but worth clearing so it
            # can never coincide with the also-possible case of a stale
            # frame arriving just as this new pending is registered below.
            self._drain_inbox()
            pending = _Pending(PCF8575_CMD_READ_PORT)
            with self._pending_lock:
                self._pending = pending
            try:
                result = self._send(bytes((PCF8575_CMD_READ_PORT,)))
                if result != SendResult.OK:
                    raise FixtureError(f"READ_PORT not delivered: {result.describe()}")
                if not pending.event.wait(timeout):
                    raise FixtureError(
                        f"READ_PORT was ACKed but no reply arrived within {timeout:.1f}s"
                    )
                return pending.value  # type: ignore[return-value]
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    # -- receive -------------------------------------------------------
    def _consume_loop(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inbox.get(timeout=0.2)  # type: ignore[union-attr]
            except queue.Empty:
                continue
            except AttributeError:
                return  # inbox torn down mid-shutdown
            try:
                self._handle_reply(frame)
            except Exception:  # pragma: no cover - never kill the consumer
                log.exception("error handling fixture PCF8575 frame")

    def _handle_reply(self, frame: Frame) -> None:
        payload = frame.payload
        if not payload:
            return
        subcommand = payload[0]
        if subcommand == PCF8575_CMD_READ_PORT:
            # byte0 subcmd, 1-2 pins (live INPUT register) u16 LE,
            # 3-4 shadow (last value WRITTEN) u16 LE, 5 address.
            if len(payload) < 6:
                log.warning("dropping short READ_PORT reply: %r", payload)
                return
            pins = _unpack_u16_le(payload, 1)
            shadow = _unpack_u16_le(payload, 3)
            address = payload[5]
            value: object = (pins, shadow, address)
        elif subcommand == PCF8575_CMD_SCAN:
            count = payload[1] if len(payload) > 1 else 0
            value = list(payload[2 : 2 + count])
        else:
            log.debug("ignoring fixture reply with subcommand 0x%02X", subcommand)
            return

        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
