"""Transport layer to SimFW: :class:`SimLink` and its two implementations.

PLAN.md sec 2/5: SimFW talks native USB CDC using a hardened protocol that is
being lifted from ``firmware/UnitTestFw`` into ``firmware/CommonFW`` by a
parallel effort, not finalized as this module is written. Every module above
this one (``protocol.py``, ``scenario.py``, ``report.py``, the CLI, the MCP
server, the GUI) codes against :class:`SimLink`'s abstract interface, never
against a wire format directly, so that lift is a change contained entirely
to this file.

:class:`SerialSimLink` is a REAL pyserial transport with a PLACEHOLDER wire
encoding (length-prefixed JSON -- see ``# TODO(protocol-lift)`` below). It is
not a guess at the final byte protocol; it exists so the rest of the stack
(scenario running, report generation, the CLI, the MCP tool surface) is
testable end-to-end today, against :class:`MockSimLink`, without hardware and
without having to freeze byte layouts before ``firmware/CommonFW``'s
extraction lands.

:class:`MockSimLink` is the test double every other kilnsim module's tests
should run against -- see the module docstring on why nothing in this
package should need real hardware to be testable.
"""

from __future__ import annotations

import abc
import json
import logging
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

from .protocol import CommandGroup, Event, EventType

log = logging.getLogger(__name__)

#: kilnctrl (this repo's other USB device) already depends on pyserial
#: (tools/PcTools/pyproject.toml); kilnsim reuses the same dependency rather
#: than adding a new one. Imported lazily so MockSimLink-only code paths
#: (most of the test suite, and any --mock CLI/GUI/MCP run) work even on a
#: machine without pyserial installed.
try:
    import serial
    from serial.tools import list_ports as _list_ports
except ImportError:  # pragma: no cover - exercised implicitly by CI without pyserial
    serial = None
    _list_ports = None

#: SimFW's placeholder USB VID:PID. Raspberry Pi's own default RP2040 CDC
#: VID:PID (2E8A:000A, the "Board CDC" example) until SimFW claims its own --
#: swap this out once firmware/SimFW/docs/HARDWARE.md documents a real one.
SIMFW_VID_PID = "2E8A:000A"

DEFAULT_BAUD_RATE = 115200
DEFAULT_CONNECT_TIMEOUT_S = 2.0
DEFAULT_COMMAND_TIMEOUT_S = 2.0


class SimLinkError(RuntimeError):
    """Raised for connect/send failures that aren't just "not connected"."""


# ---------------------------------------------------------------------------
# Abstract interface
# ---------------------------------------------------------------------------
class SimLink(abc.ABC):
    """Everything above this layer is coded against this interface only.

    A concrete link owns exactly one physical (or fake) connection. Command
    replies are synchronous (``send_command`` blocks for the matching
    reply); telemetry and EVT frames arrive asynchronously and are drained
    through :meth:`read_events`.
    """

    @property
    @abc.abstractmethod
    def is_connected(self) -> bool:
        ...

    @abc.abstractmethod
    def connect(self, port: Optional[str] = None) -> str:
        """Open a connection. Autodetects (VID/PID + a PING round-trip) if
        ``port`` is omitted. Returns the port/identifier actually used.
        Raises :class:`SimLinkError` if nothing could be opened."""

    @abc.abstractmethod
    def disconnect(self) -> None:
        ...

    @abc.abstractmethod
    def send_command(self, group: CommandGroup, cmd: int, payload: Optional[dict] = None,
                      timeout: Optional[float] = None) -> dict:
        """Send one command and block for its reply.

        ``payload`` and the return value are plain JSON-able dicts at this
        layer (see the module docstring -- the real byte encoding is an
        implementation detail of the concrete subclass). Raises
        :class:`SimLinkError` on a NACK/timeout/link failure.
        """

    @abc.abstractmethod
    def read_events(self, timeout: float = 0.0) -> list:
        """Drain currently-buffered :class:`~kilnsim.protocol.Event`\\ s
        (EVT frames), waiting up to ``timeout`` seconds for at least one if
        none are buffered yet. Never blocks longer than that. Returns []
        on timeout, not an error -- "no events yet" is normal."""

    def __enter__(self) -> "SimLink":
        self.connect()
        return self

    def __exit__(self, *exc) -> None:
        self.disconnect()


# ---------------------------------------------------------------------------
# Real transport: pyserial + placeholder framing
# ---------------------------------------------------------------------------
class SerialSimLink(SimLink):
    """pyserial-backed :class:`SimLink` against real SimFW hardware.

    # TODO(protocol-lift): reconcile with firmware/CommonFW's extracted
    # protocol once that agent's work lands. What's here is a deliberately
    # simple PLACEHOLDER wire framing -- length-prefixed JSON, see
    # ``_encode``/``_decode`` below -- chosen so the rest of the kilnsim
    # stack (scenario running, report generation, CLI, MCP tools) is
    # testable end-to-end today against MockSimLink, and so this class is at
    # least structurally exercisable against a bare CDC echo/loopback while
    # SimFW firmware doesn't exist yet either. It is NOT a proposal for the
    # final byte protocol -- do not build tooling that assumes this framing
    # survives the lift. When CommonFW's extraction lands, only
    # ``_encode``/``_decode``/``_rx_loop`` below should need to change; the
    # public SimLink surface (connect/disconnect/send_command/read_events)
    # is what the rest of this package is written against and should not.
    """

    def __init__(self, baudrate: int = DEFAULT_BAUD_RATE) -> None:
        self.baudrate = baudrate
        self._serial = None
        self._port: Optional[str] = None
        self._reader: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._rx_buf = bytearray()

        self._events: "list[Event]" = []
        self._events_lock = threading.Lock()
        self._events_cv = threading.Condition(self._events_lock)

        self._pending_lock = threading.Lock()
        self._pending: dict[int, threading.Event] = {}
        self._replies: dict[int, dict] = {}
        self._next_req_id = 1

    @property
    def is_connected(self) -> bool:
        return self._serial is not None and self._serial.is_open

    # -- discovery -----------------------------------------------------------
    @staticmethod
    def list_candidate_ports() -> list:
        """Ports whose VID:PID matches :data:`SIMFW_VID_PID`."""
        if _list_ports is None:
            raise SimLinkError("pyserial is not installed -- cannot enumerate serial ports")
        out = []
        for p in _list_ports.comports():
            hwid = (p.hwid or "").upper()
            if SIMFW_VID_PID in hwid:
                out.append(p.device)
        return out

    def connect(self, port: Optional[str] = None) -> str:
        if serial is None:
            raise SimLinkError("pyserial is not installed -- pip install pyserial")
        if self.is_connected:
            raise SimLinkError(f"already connected to {self._port}")

        if port is None:
            candidates = self.list_candidate_ports()
            if not candidates:
                raise SimLinkError(
                    f"no SimFW-looking port found (VID:PID {SIMFW_VID_PID}); "
                    "pass an explicit port"
                )
            port = candidates[0]

        ser = serial.Serial(port=port, baudrate=self.baudrate, timeout=0.2)
        self._serial = ser
        self._port = port
        self._rx_buf.clear()
        self._stop.clear()
        self._reader = threading.Thread(target=self._rx_loop, name="simlink-rx", daemon=True)
        self._reader.start()

        # PING round-trip proves this is actually a SimFW peer, not just a
        # port that happened to match the VID:PID.
        try:
            self.send_command(CommandGroup.SYS, 1, timeout=DEFAULT_CONNECT_TIMEOUT_S)  # SysCmd.PING
        except SimLinkError:
            self.disconnect()
            raise
        return port

    def disconnect(self) -> None:
        self._stop.set()
        reader, self._reader = self._reader, None
        ser, self._serial = self._serial, None
        if reader is not None and reader is not threading.current_thread():
            reader.join(timeout=2.0)
        if ser is not None:
            try:
                ser.close()
            except Exception:  # pragma: no cover
                log.debug("error closing SimFW port", exc_info=True)
        self._port = None

    # -- framing (placeholder -- see class docstring) -------------------------
    def _encode(self, req_id: int, group: CommandGroup, cmd: int, payload: dict) -> bytes:
        body = json.dumps(
            {"req_id": req_id, "group": int(group), "cmd": int(cmd), "payload": payload}
        ).encode("utf-8")
        return struct.pack(">I", len(body)) + body

    def _rx_loop(self) -> None:
        ser = self._serial
        while not self._stop.is_set():
            try:
                if ser is None or not ser.is_open:
                    break
                chunk = ser.read(max(1, ser.in_waiting or 1))
            except Exception:
                if not self._stop.is_set():
                    log.warning("SimFW serial read failed; reader exiting", exc_info=True)
                break
            if chunk:
                self._rx_buf.extend(chunk)
            self._drain_frames()

    def _drain_frames(self) -> None:
        while True:
            if len(self._rx_buf) < 4:
                return
            (length,) = struct.unpack(">I", self._rx_buf[:4])
            if len(self._rx_buf) < 4 + length:
                return
            body = bytes(self._rx_buf[4:4 + length])
            del self._rx_buf[: 4 + length]
            try:
                message = json.loads(body.decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                log.warning("dropping malformed SimFW frame (%d bytes)", length)
                continue
            self._handle_message(message)

    def _handle_message(self, message: dict) -> None:
        kind = message.get("kind")
        if kind == "reply":
            req_id = message.get("req_id")
            with self._pending_lock:
                waiter = self._pending.get(req_id)
                if waiter is not None:
                    self._replies[req_id] = message
                    waiter.set()
            return
        if kind == "event":
            try:
                evt = Event.from_dict(message["event"])
            except (KeyError, ValueError, TypeError):
                log.warning("dropping malformed SimFW event frame", exc_info=True)
                return
            with self._events_cv:
                self._events.append(evt)
                self._events_cv.notify_all()
            return
        log.debug("ignoring unrecognized SimFW frame kind %r", kind)

    # -- SimLink surface -------------------------------------------------------
    def send_command(self, group: CommandGroup, cmd: int, payload: Optional[dict] = None,
                      timeout: Optional[float] = None) -> dict:
        if not self.is_connected:
            raise SimLinkError("not connected")
        wait = threading.Event()
        with self._pending_lock:
            req_id = self._next_req_id
            self._next_req_id += 1
            self._pending[req_id] = wait
        try:
            wire = self._encode(req_id, group, cmd, payload or {})
            try:
                self._serial.write(wire)
                self._serial.flush()
            except Exception as exc:  # noqa: BLE001
                raise SimLinkError(f"write failed: {exc}") from exc

            if not wait.wait(timeout if timeout is not None else DEFAULT_COMMAND_TIMEOUT_S):
                raise SimLinkError(
                    f"timeout waiting for reply to {group.name}/{cmd}"
                )
            with self._pending_lock:
                reply = self._replies.pop(req_id, None)
        finally:
            with self._pending_lock:
                self._pending.pop(req_id, None)

        if reply is None:  # pragma: no cover - defensive
            raise SimLinkError("reply vanished")
        if reply.get("status") == "error":
            raise SimLinkError(str(reply.get("error", "unknown error")))
        return reply.get("payload", {})

    def read_events(self, timeout: float = 0.0) -> list:
        with self._events_cv:
            if not self._events:
                self._events_cv.wait(timeout=timeout)
            out, self._events[:] = self._events[:], []
            return out


# ---------------------------------------------------------------------------
# Mock transport: everything else's test double
# ---------------------------------------------------------------------------
@dataclass
class _ScriptedResponse:
    payload: dict
    error: Optional[str] = None


class MockSimLink(SimLink):
    """In-memory fake fixture. No I/O, no thread races to reason about
    (single-threaded, direct call/return) unless a test explicitly wants
    async event delivery -- see :meth:`inject_event`.

    Answers ``PING``/``GET_CAPS``/``GET_STATE`` out of the box with
    reasonable canned data (enough to smoke-test the CLI/MCP tools without
    any setup). A test can override or extend those with
    :meth:`script_response`, and push unsolicited events with
    :meth:`inject_event`.
    """

    def __init__(self) -> None:
        self._connected = False
        self._port: Optional[str] = None
        self._events: "list[Event]" = []
        self._scripts: dict[tuple, list] = {}
        self._history: "list[tuple[CommandGroup, int, dict]]" = []
        self._next_seq = 1
        self._state: dict = {
            "sim_time_us": 0,
            "timescale": 1.0,
            "seed": 0,
            "zones": [
                {"t_zone": 20.0, "t_tc_reported": 20.0, "t_safety_reported": 20.0, "i_amps": 0.0}
            ],
            "relay_state_mask": 0,
            "estop_open": False,
            "fault_line_asserted": False,
            "active_fault_count": 0,
        }

    # -- test-double controls --------------------------------------------------
    def script_response(self, group: CommandGroup, cmd: int, payload: dict,
                         error: Optional[str] = None) -> None:
        """Queue a canned reply for the next matching ``send_command`` call.
        Multiple calls for the same (group, cmd) queue in FIFO order; once
        the queue is empty, the built-in defaults (or a generic ack) apply
        again."""
        key = (int(group), int(cmd))
        self._scripts.setdefault(key, []).append(_ScriptedResponse(payload, error))

    def inject_event(self, event: Event) -> None:
        """Make ``event`` available on the next :meth:`read_events` call."""
        self._events.append(event)

    def make_event(self, event_type: EventType, payload: Optional[dict] = None,
                    sim_time_us: Optional[int] = None) -> Event:
        """Convenience: build + inject an :class:`Event` with an
        auto-incrementing seq, mirroring the real link's EVT sequencing."""
        seq = self._next_seq
        self._next_seq += 1
        evt = Event(
            seq=seq,
            sim_time_us=sim_time_us if sim_time_us is not None else self._state["sim_time_us"],
            event_type=event_type,
            payload=payload or {},
        )
        self.inject_event(evt)
        return evt

    @property
    def sent_commands(self) -> list:
        """Everything sent through :meth:`send_command`, for test assertions."""
        return list(self._history)

    # -- SimLink surface -------------------------------------------------------
    @property
    def is_connected(self) -> bool:
        return self._connected

    def connect(self, port: Optional[str] = None) -> str:
        if self._connected:
            raise SimLinkError("already connected")
        self._connected = True
        self._port = port or "MOCK"
        return self._port

    def disconnect(self) -> None:
        self._connected = False
        self._port = None

    def send_command(self, group: CommandGroup, cmd: int, payload: Optional[dict] = None,
                      timeout: Optional[float] = None) -> dict:
        if not self._connected:
            raise SimLinkError("not connected")
        payload = payload or {}
        self._history.append((group, cmd, payload))

        key = (int(group), int(cmd))
        queued = self._scripts.get(key)
        if queued:
            resp = queued.pop(0)
            if resp.error:
                raise SimLinkError(resp.error)
            return dict(resp.payload)

        return self._default_response(group, cmd, payload)

    def read_events(self, timeout: float = 0.0) -> list:
        # Single-threaded fake: nothing arrives asynchronously that isn't
        # already in self._events, so `timeout` is a no-op here (there is
        # nothing to usefully wait for) -- kept in the signature to match
        # SimLink exactly.
        out, self._events[:] = self._events[:], []
        return out

    # -- built-in defaults -------------------------------------------------------
    def _default_response(self, group: CommandGroup, cmd: int, payload: dict) -> dict:
        if group is CommandGroup.SYS:
            if cmd == 1:  # PING
                return {"pong": True}
            if cmd == 2:  # GET_VERSION
                return {"fw_version": "mock-0.0", "fw_git_hash": "0000000"}
            if cmd == 6:  # GET_CAPS
                return {
                    "protocol_version": 0,
                    "fw_version": "mock-0.0",
                    "fw_git_hash": "0000000",
                    "zone_count_min": 1,
                    "zone_count_max": 4,
                    "tc_channel_count": 4,
                    "ct_channel_count": 3,
                    "relay_count": 5,
                    "feature_bitmask": 0,
                }
            if cmd == 4:  # SET_TIMESCALE
                self._state["timescale"] = payload.get("value", 1.0)
                return {"ok": True}
            if cmd == 5:  # SET_SEED
                self._state["seed"] = payload.get("value", 0)
                return {"ok": True}
            if cmd == 3:  # RESET_SIM
                self._state["sim_time_us"] = 0
                self._state["active_fault_count"] = 0
                return {"ok": True}
        if group is CommandGroup.MODEL and cmd == 4:  # LOAD_PRESET
            return {"ok": True, "preset": payload.get("name")}
        # GET_STATE is not its own group in the section-5 table (telemetry is
        # push-only) -- the CLI/MCP "state" surface reads the mock's snapshot
        # directly via `sim_get_state`-shaped SYS traffic instead, handled
        # here so `kilnsim state --mock` has something to show immediately.
        if group is CommandGroup.SYS and cmd == 100:  # GET_STATE (kilnsim-local convenience)
            return dict(self._state)
        # Generic fallback: most PLAN.md 5-table commands are fire-and-forget
        # writes (SET_*, INJECT_FAULT, RELAY_SET_CONTACT_FAULT, IO_WRITE...);
        # answering them all with a plain ack keeps every CLI/MCP wrapper
        # testable without a bespoke script for each one.
        return {"ok": True}

    # -- state helpers, used by tests / the mock GUI/CLI path -------------------
    def get_state_snapshot(self) -> dict:
        return dict(self._state)

    def set_state(self, **kwargs) -> None:
        self._state.update(kwargs)
