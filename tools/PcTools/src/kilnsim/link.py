"""Transport layer to SimFW: :class:`SimLink` and its two implementations.

PLAN.md sec 2/5: SimFW talks native USB CDC using ``benchproto``
(``firmware/CommonFW/include/benchproto/``, spec in
``firmware/CommonFW/docs/BENCHPROTO.md``), with SimFW's own command-group
numbering and payload byte layouts documented in
``firmware/SimFW/docs/PROTOCOL.md``. Every module above this one
(``protocol.py``, ``scenario.py``, ``report.py``, the CLI, the MCP server,
the GUI) codes against :class:`SimLink`'s abstract interface -- plain dicts
in, plain dicts out -- never against wire bytes directly, so the real
protocol lift (replacing the old length-prefixed-JSON placeholder framing)
was contained entirely to this file plus the two new modules it delegates
to: :mod:`kilnsim.benchproto_codec` (frame envelope + reliability layer,
proven byte-identical against ``firmware/CommonFW``'s shared test vectors)
and :mod:`kilnsim.payloads` (per-command-group byte layouts).

:class:`SerialSimLink` is a REAL pyserial transport speaking the REAL wire
protocol: SLIP-style framing, CRC-16/CCITT-FALSE, sequence numbers with
retry/dedup (BENCHPROTO.md sec 4), SimFW's `[cmd_id, args...]` request /
`[status, ...]` reply payload convention (PROTOCOL.md sec 2), and unsolicited
BROADCAST demultiplexing for TELEMETRY/EVT frames (PROTOCOL.md sec 6).

:class:`MockSimLink` is the test double every other kilnsim module's tests
should run against -- see the module docstring on why nothing in this
package should need real hardware to be testable. Its canned responses are
shaped like the real decoded payloads (:mod:`kilnsim.payloads`' dict shapes),
not placeholder JSON, so pre-hardware testing against it actually exercises
the field names/types a real reply would have.
"""

from __future__ import annotations

import abc
import logging
import struct
import threading
import time
from dataclasses import dataclass, field
from typing import Any, Callable, Optional

from . import benchproto_codec as bp
from . import payloads as pl
from .protocol import CommandGroup, Event, EventType

log = logging.getLogger(__name__)

#: SimFW's own benchproto device ids -- cmd_ids.h SIMFW_DEVICE_HOST/TARGET,
#: BENCHPROTO.md sec 3's "SRC_DEVICE ... SimFW uses HOST=0, TARGET=1."
SIMFW_DEVICE_HOST = 0
SIMFW_DEVICE_TARGET = 1

#: cmd_ids.h SIMFW_TASK_ID_EVT -- the unsolicited BROADCAST source task
#: (PROTOCOL.md sec 6). dst_task on an inbound BROADCAST is 0 (no specific
#: registered receiver, usb_owner_send_broadcast()'s own doc comment).
SIMFW_TASK_ID_EVT = int(CommandGroup.EVT)
SIMFW_BROADCAST_DST_TASK = 0

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

import socket as _socket

#: SimFW's placeholder USB VID:PID. Raspberry Pi's own default RP2040 CDC
#: VID:PID (2E8A:000A, the "Board CDC" example) until SimFW claims its own --
#: swap this out once firmware/SimFW/docs/HARDWARE.md documents a real one.
SIMFW_VID_PID = "2E8A:000A"

DEFAULT_BAUD_RATE = 115200
DEFAULT_CONNECT_TIMEOUT_S = 2.0
DEFAULT_COMMAND_TIMEOUT_S = 2.0


class SimLinkError(RuntimeError):
    """Raised for connect/send failures that aren't just "not connected"."""


def get_state_snapshot(link: "SimLink", wait_s: float = 1.0) -> dict:
    """A "current telemetry snapshot" that works across every concrete
    :class:`SimLink`, papering over a real gap this function exists to fix:
    callers used to send ``SYS/100`` unconditionally (a purely kilnsim-local
    convenience id `MockSimLink._default_response` answers directly) --
    fine against :class:`MockSimLink`, but PROTOCOL.md sec 4 defines no such
    id, so a real :class:`SerialSimLink`/:class:`TcpSimLink` raises
    ``kilnsim.payloads.PayloadError`` (uncaught -- not even a clean
    :class:`SimLinkError`) the instant anything called it, e.g. ``kilnsim
    state --virtual`` or the GUI's zone chart against a real link. Found
    while wiring the GUI/CLI up against ``virtual_simfw`` for real.

    The real-link answer is the most recent TELEMETRY broadcast
    (:meth:`_FramedSimLink.get_last_telemetry`, PROTOCOL.md sec 6) -- if
    none has arrived yet, this drains :meth:`SimLink.read_events` (which
    demultiplexes TELEMETRY on the framed links as a side effect) for up to
    ``wait_s`` seconds waiting for the first one. Raises
    :class:`SimLinkError` if none ever arrives.
    """
    get_last = getattr(link, "get_last_telemetry", None)
    if get_last is None:  # MockSimLink and any other non-framed SimLink
        return link.send_command(CommandGroup.SYS, 100)
    snap = get_last()
    if snap is None:
        deadline = time.monotonic() + wait_s
        while snap is None and time.monotonic() < deadline:
            link.read_events(timeout=0.2)
            snap = get_last()
    if snap is None:
        raise SimLinkError("no TELEMETRY frame received yet (nothing published since connect?)")
    return snap


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
# Real transport: pyserial + the real benchproto wire protocol
# ---------------------------------------------------------------------------
class _FramedSimLink(SimLink):
    """Shared plumbing for every :class:`SimLink` that speaks the real
    ``benchproto`` wire protocol over a byte-stream transport: frame
    reassembly, request/reply matching + retry, and BROADCAST (TELEMETRY/
    EVT) demultiplexing. This is the part :class:`SerialSimLink` and
    :class:`TcpSimLink` share byte-for-byte -- the two differ only in how
    bytes actually move (a serial port vs. a TCP socket), so this base class
    factors out everything above that line and each subclass supplies just
    four transport primitives: :meth:`_transport_open`, :meth:`_transport_close`,
    :meth:`_transport_read_chunk`, :meth:`_transport_write`.

    Framing/CRC/stuffing lives in :mod:`kilnsim.benchproto_codec` (proven
    byte-identical against ``firmware/CommonFW``'s shared C test vectors);
    per-command-group byte layouts live in :mod:`kilnsim.payloads`.
    """

    def __init__(self) -> None:
        self._port: Optional[str] = None
        self._reader: Optional[threading.Thread] = None
        self._stop = threading.Event()
        self._rx_buf = bytearray()

        self._events: "list[Event]" = []
        self._events_lock = threading.Lock()
        self._events_cv = threading.Condition(self._events_lock)

        # Only one outstanding send-and-await-reply cycle at a time
        # (BENCHPROTO.md sec 4: "Only one outstanding send-and-await-reply
        # cycle is allowed per benchproto_pending_request_t") -- callers of
        # send_command() are already serialized by this lock, matching the
        # firmware side's own single-outstanding-per-link discipline.
        self._send_lock = threading.Lock()
        self._link = bp.BenchprotoLink(own_device=SIMFW_DEVICE_HOST)
        self._pending = bp.PendingRequest()
        self._reply_cv = threading.Condition()
        self._reply_frame: Optional[bp.Frame] = None

        # EVT sequence-gap tracking (PROTOCOL.md sec 6 "Loss visibility":
        # "the PC's report generator refuses to certify a run with a
        # sequence gap"). None until the first EVT frame is seen (nothing to
        # compare the first seq against yet).
        self._last_evt_seq: Optional[int] = None
        self.evt_seq_gap_count = 0
        self._last_telemetry: Optional[dict] = None

    # -- transport primitives, supplied by subclasses ---------------------------
    def _transport_open(self, port: Optional[str]) -> str:
        """Opens the underlying byte pipe and returns the identifier actually
        used (e.g. the serial device path, or "host:port" for TCP)."""
        raise NotImplementedError

    def _transport_close(self) -> None:
        raise NotImplementedError

    def _transport_read_chunk(self) -> bytes:
        """Blocks briefly (a short, subclass-chosen timeout) for whatever
        bytes are available; returns b"" on timeout with nothing new, raises
        on a real I/O error."""
        raise NotImplementedError

    def _transport_write(self, data: bytes) -> None:
        raise NotImplementedError

    def connect(self, port: Optional[str] = None) -> str:
        if self.is_connected:
            raise SimLinkError(f"already connected to {self._port}")
        opened = self._transport_open(port)
        self._port = opened
        self._rx_buf.clear()
        self._stop.clear()
        self._reader = threading.Thread(target=self._rx_loop, name="simlink-rx", daemon=True)
        self._reader.start()

        # PING round-trip proves this is actually a SimFW peer, not just a
        # port/address that happened to accept a connection.
        try:
            self.send_command(CommandGroup.SYS, 1, timeout=DEFAULT_CONNECT_TIMEOUT_S)  # SysCmd.PING
        except SimLinkError:
            self.disconnect()
            raise
        return opened

    def disconnect(self) -> None:
        self._stop.set()
        reader, self._reader = self._reader, None
        if reader is not None and reader is not threading.current_thread():
            reader.join(timeout=2.0)
        try:
            self._transport_close()
        except Exception:  # pragma: no cover
            log.debug("error closing SimFW transport", exc_info=True)
        self._port = None

    # -- RX: byte stream -> frames --------------------------------------------
    def _rx_loop(self) -> None:
        while not self._stop.is_set():
            try:
                if not self.is_connected:
                    break
                chunk = self._transport_read_chunk()
            except Exception:
                if not self._stop.is_set():
                    log.warning("SimFW transport read failed; reader exiting", exc_info=True)
                break
            if chunk:
                self._rx_buf.extend(chunk)
            self._drain_frames()

    def _drain_frames(self) -> None:
        """Splits the accumulated byte stream on DELIM (0x7E) boundaries and
        hands each complete stuffed frame to :meth:`_handle_wire_frame`.
        Mirrors a live receiver's resync behavior (BENCHPROTO.md sec 2): any
        bytes before the first DELIM, and back-to-back DELIMs (empty-frame
        noise), are simply consumed with nothing decoded."""
        while True:
            try:
                first = self._rx_buf.index(bp.DELIM)
            except ValueError:
                return  # no delimiter yet -- keep buffering
            del self._rx_buf[:first]
            try:
                second = self._rx_buf.index(bp.DELIM, 1)
            except ValueError:
                return  # frame not complete yet
            if second == 1:
                del self._rx_buf[:1]  # empty-frame noise (0x7E 0x7E) -- drop one and resync
                continue
            wire = bytes(self._rx_buf[: second + 1])
            del self._rx_buf[:second]  # leave the trailing DELIM as the next frame's leading one
            self._handle_wire_frame(wire)

    def _handle_wire_frame(self, wire: bytes) -> None:
        try:
            frame = bp.decode_frame(wire)
        except bp.FrameError as exc:
            log.warning("dropping malformed SimFW frame: %s", exc)
            return

        if frame.msg_type == bp.MsgType.BROADCAST:
            self._handle_broadcast(frame)
            return

        if frame.msg_type in (bp.MsgType.ACK, bp.MsgType.NACK):
            action = self._link.on_frame(self._pending, frame)
            if action in (bp.LinkAction.ACK_MATCHED, bp.LinkAction.NACK_MATCHED):
                with self._reply_cv:
                    self._reply_frame = frame
                    self._reply_cv.notify_all()
            # IGNORE (stale retry's late reply, or nothing outstanding):
            # nothing to do, matches BENCHPROTO_LINK_ACTION_IGNORE's contract.
            return

        # SimFW never sends DATA to the host (PROTOCOL.md sec 7: "no inbound
        # BROADCAST consumer" and the host has no registered SimFW-facing
        # command handlers) -- an inbound DATA frame here is unexpected.
        log.debug("ignoring unexpected inbound DATA frame from SimFW")

    def _handle_broadcast(self, frame: bp.Frame) -> None:
        if frame.src_task != SIMFW_TASK_ID_EVT or not frame.payload:
            log.debug("ignoring BROADCAST from unexpected src_task %s", frame.src_task)
            return
        kind = frame.payload[0]
        try:
            if kind == pl.EVT_FRAME_KIND_EVENT:
                fields = pl.decode_evt_frame(frame.payload)
                evt = Event.from_wire(fields)
                self._note_evt_seq(evt.seq)
                with self._events_cv:
                    self._events.append(evt)
                    self._events_cv.notify_all()
            elif kind == pl.EVT_FRAME_KIND_TELEMETRY:
                telemetry = pl.decode_telemetry_frame(frame.payload)
                with self._events_lock:
                    self._last_telemetry = telemetry
            else:
                log.warning("unknown EVT frame kind 0x%02X", kind)
        except (struct.error, IndexError, KeyError, ValueError) as exc:
            log.warning("dropping malformed EVT/TELEMETRY frame: %s", exc, exc_info=True)

    def _note_evt_seq(self, seq: int) -> None:
        """PROTOCOL.md sec 6 "Loss visibility": a gap between consecutive
        received EVT `seq` values is, by itself, enough to detect loss --
        this is that client-side check. ``evt_seq_gap_count`` is exposed so
        report.py (PLAN.md 5.3: "the PC's report generator refuses to
        certify a run with a sequence gap") can inspect it after a run."""
        if self._last_evt_seq is not None and seq != self._last_evt_seq + 1:
            gap = seq - self._last_evt_seq - 1
            if gap > 0:
                log.warning("EVT sequence gap: expected seq %d, got %d (%d missing)",
                            self._last_evt_seq + 1, seq, gap)
                self.evt_seq_gap_count += gap
        self._last_evt_seq = seq

    def get_last_telemetry(self) -> Optional[dict]:
        """Most recently received TELEMETRY frame (PROTOCOL.md sec 6),
        decoded, or None if none has arrived yet."""
        with self._events_lock:
            return dict(self._last_telemetry) if self._last_telemetry is not None else None

    # -- SimLink surface -------------------------------------------------------
    def send_command(self, group: CommandGroup, cmd: int, payload: Optional[dict] = None,
                      timeout: Optional[float] = None) -> dict:
        if not self.is_connected:
            raise SimLinkError("not connected")

        deadline_timeout = timeout if timeout is not None else DEFAULT_COMMAND_TIMEOUT_S
        request_payload = pl.encode_request(group, cmd, payload)

        with self._send_lock:
            msg_index = self._link.next_msg_index()
            self._pending.begin(dst_device=SIMFW_DEVICE_TARGET, dst_task=int(group),
                                 msg_index=msg_index)
            frame = bp.Frame(
                msg_type=bp.MsgType.DATA,
                msg_index=msg_index,
                src_device=SIMFW_DEVICE_HOST,
                src_task=0,
                dst_device=SIMFW_DEVICE_TARGET,
                dst_task=int(group),
                payload=request_payload,
            )
            try:
                while True:
                    with self._reply_cv:
                        self._reply_frame = None
                    self._write_frame(frame)
                    reply = self._wait_for_reply(deadline_timeout)
                    if reply is not None:
                        break
                    if not self._pending.note_retry():
                        raise SimLinkError(
                            f"timeout waiting for reply to {group.name}/{cmd} "
                            f"(after {bp.MAX_RETRIES} attempts)"
                        )
                    # Retry reuses the same msg_index (BENCHPROTO.md sec 4).
                    frame.msg_index = self._pending.msg_index
            finally:
                self._pending.clear()

        if reply.msg_type == bp.MsgType.NACK:
            raise SimLinkError(f"{group.name}/{cmd}: NACK (undeliverable -- task not registered)")
        try:
            return pl.decode_reply(group, cmd, reply.payload)
        except pl.CommandStatusError as exc:
            raise SimLinkError(str(exc)) from exc

    def _write_frame(self, frame: bp.Frame) -> None:
        wire = bp.encode_frame(frame)
        try:
            self._transport_write(wire)
        except Exception as exc:  # noqa: BLE001
            raise SimLinkError(f"write failed: {exc}") from exc

    def _wait_for_reply(self, timeout: float) -> Optional[bp.Frame]:
        with self._reply_cv:
            if self._reply_frame is None:
                self._reply_cv.wait(timeout=timeout)
            return self._reply_frame

    def read_events(self, timeout: float = 0.0) -> list:
        with self._events_cv:
            if not self._events:
                self._events_cv.wait(timeout=timeout)
            out, self._events[:] = self._events[:], []
            return out


# ---------------------------------------------------------------------------
# Real transport #1: pyserial, for real SimFW hardware over native USB CDC.
# ---------------------------------------------------------------------------
class SerialSimLink(_FramedSimLink):
    """pyserial-backed :class:`_FramedSimLink` against real SimFW hardware."""

    def __init__(self, baudrate: int = DEFAULT_BAUD_RATE) -> None:
        super().__init__()
        self.baudrate = baudrate
        self._serial = None

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

    def _transport_open(self, port: Optional[str]) -> str:
        if serial is None:
            raise SimLinkError("pyserial is not installed -- pip install pyserial")
        if port is None:
            candidates = self.list_candidate_ports()
            if not candidates:
                raise SimLinkError(
                    f"no SimFW-looking port found (VID:PID {SIMFW_VID_PID}); "
                    "pass an explicit port"
                )
            port = candidates[0]
        self._serial = serial.Serial(port=port, baudrate=self.baudrate, timeout=0.2)
        return port

    def _transport_close(self) -> None:
        ser, self._serial = self._serial, None
        if ser is not None:
            ser.close()

    def _transport_read_chunk(self) -> bytes:
        ser = self._serial
        if ser is None or not ser.is_open:
            return b""
        return ser.read(max(1, ser.in_waiting or 1))

    def _transport_write(self, data: bytes) -> None:
        self._serial.write(data)
        self._serial.flush()


# ---------------------------------------------------------------------------
# Real transport #2: plain TCP, for firmware/SimFW/tools/virtual_simfw's
# host-side "virtual SimFW" (see that directory's README.md). Same real
# benchproto wire protocol as SerialSimLink -- only the byte pipe differs
# (a loopback TCP socket instead of a serial port), which is exactly what
# _FramedSimLink's split makes a ~60-line subclass instead of a second copy
# of the whole request/reply/BROADCAST machinery.
# ---------------------------------------------------------------------------
#: virtual_simfw's default listen port (firmware/SimFW/tools/virtual_simfw/
#: src/virtual_simfw.c's own `port` default) -- used when TcpSimLink.connect()
#: is given no explicit "host:port" address.
DEFAULT_VIRTUAL_SIMFW_PORT = 8765
DEFAULT_TCP_CONNECT_TIMEOUT_S = 5.0


class TcpSimLink(_FramedSimLink):
    """TCP-backed :class:`_FramedSimLink` against ``virtual_simfw`` (or any
    other peer speaking the same real benchproto wire protocol over a raw
    TCP byte stream -- the protocol has no notion of "this is the virtual
    one", so real hardware bridged over TCP would work here too).

    ``port`` passed to :meth:`connect` is ``"host:port"`` (default host
    ``127.0.0.1`` if just a bare port number or nothing is given) --
    kilnsim's CLI/MCP surface calls this ``--virtual [host:port]`` per the
    task's own sketch.
    """

    def __init__(self) -> None:
        super().__init__()
        self._sock: Optional[_socket.socket] = None

    @property
    def is_connected(self) -> bool:
        return self._sock is not None

    @staticmethod
    def _parse_address(address: Optional[str]) -> tuple:
        if not address:
            return "127.0.0.1", DEFAULT_VIRTUAL_SIMFW_PORT
        address = str(address)
        if address.isdigit():
            return "127.0.0.1", int(address)
        if ":" in address:
            host, _, port_s = address.rpartition(":")
            return (host or "127.0.0.1"), int(port_s)
        return address, DEFAULT_VIRTUAL_SIMFW_PORT

    def _transport_open(self, port: Optional[str]) -> str:
        host, tcp_port = self._parse_address(port)
        sock = _socket.socket(_socket.AF_INET, _socket.SOCK_STREAM)
        sock.settimeout(DEFAULT_TCP_CONNECT_TIMEOUT_S)
        try:
            sock.connect((host, tcp_port))
        except OSError as exc:
            sock.close()
            raise SimLinkError(f"could not connect to virtual SimFW at {host}:{tcp_port}: {exc}") from exc
        sock.setsockopt(_socket.IPPROTO_TCP, _socket.TCP_NODELAY, 1)
        sock.settimeout(0.2)  # short blocking-with-timeout reads, matching
                                # SerialSimLink's ser.read(timeout=0.2)
        self._sock = sock
        return f"{host}:{tcp_port}"

    def _transport_close(self) -> None:
        sock, self._sock = self._sock, None
        if sock is not None:
            try:
                sock.shutdown(_socket.SHUT_RDWR)
            except OSError:
                pass
            sock.close()

    def _transport_read_chunk(self) -> bytes:
        sock = self._sock
        if sock is None:
            return b""
        try:
            return sock.recv(4096)
        except _socket.timeout:
            return b""
        except OSError:
            return b""

    def _transport_write(self, data: bytes) -> None:
        if self._sock is None:
            raise SimLinkError("not connected")
        self._sock.sendall(data)


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
        # FAULT_SCHEDULE/UNTIL_TRIGGER two-frame design (PROTOCOL.md sec
        # 5.6): slot ids with a pending FAULT_SCHEDULE(duration_kind==2)
        # frame 1 that hasn't yet been completed by a FAULT_SET_UNTIL_TRIGGER
        # frame 2. Mirrors cmd_task.c's s_pending_until[slot_id] closely
        # enough to keep mock-based scenario/runner tests meaningful: a
        # direct PERMANENT/FOR FAULT_SCHEDULE or a FAULT_CANCEL on the same
        # slot discards it, and FAULT_SET_UNTIL_TRIGGER on a slot with
        # nothing pending answers ERR_BAD_ARGS via SimLinkError.
        self._pending_until_trigger: "set[int]" = set()
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
    #
    # Shaped like the REAL decoded reply dicts kilnsim.payloads.decode_reply()
    # would produce for the matching (group, cmd) -- PROTOCOL.md's field
    # names, not placeholder JSON -- so pre-hardware testing against this
    # mock (CLI/GUI/MCP --mock, scenario dry runs) exercises the same field
    # names/types a real SerialSimLink reply would have.
    def _default_response(self, group: CommandGroup, cmd: int, payload: dict) -> dict:
        if group is CommandGroup.SYS:
            if cmd == 1:  # PING
                return {"pong": True}
            if cmd == 2:  # GET_VERSION
                return {
                    "protocol_version": 1,
                    "min_compatible": 1,
                    "fw_version": "mock-0.0",
                    "fw_version_major": 0,
                    "fw_version_minor": 0,
                    "fw_version_patch": 0,
                    "fw_git_dirty": False,
                    "fw_git_hash": "0000000",
                }
            if cmd == 6:  # GET_CAPS
                return {
                    "protocol_version": 1,
                    "min_compatible": 1,
                    "fw_version": "mock-0.0",
                    "fw_git_dirty": False,
                    "fw_git_hash": "0000000",
                    "zone_count_min": 1,
                    "zone_count_max": 4,
                    "zone_count_default": 3,
                    "tc_channel_count": 4,
                    "tc_main_channels": 3,
                    "tc_safety_channels": 1,
                    "ct_channel_count": 3,
                    "relay_count": 5,
                    "feature_bitmask": 0x3F,
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
            if cmd == 7:  # GET_SIM_STATE
                return {
                    "seed": self._state["seed"],
                    "snapshot_valid": True,
                    "timescale": self._state["timescale"],
                    "sim_time_us": self._state["sim_time_us"],
                }
        if group is CommandGroup.MODEL and cmd == 4:  # LOAD_PRESET
            return {"ok": True, "preset": payload.get("name")}
        if group is CommandGroup.TC and cmd == 1:  # GET_REGS
            channel = payload.get("channel", 0)
            return {
                "channel": channel,
                "flags": 0x02,
                "reg_image_valid": False,
                "snapshot_valid": True,
                "regs": bytes(16),
                "shadow_temp_c": self._state["zones"][0]["t_zone"],
                "shadow_reported_c": self._state["zones"][0]["t_tc_reported"],
                "dead_mode": 0,
                "noise_sigma_c": 0.0,
                "bit_error_rate": 0.0,
                "spi_transactions": 0,
                "spi_protocol_errors": 0,
                "spi_first_byte_late": 0,
            }
        if group is CommandGroup.TC and cmd == 6:  # GET_MASTER_CONFIG
            channel = payload.get("channel", 0)
            return {
                "channel": channel,
                "flags": 0x00,
                "configured": False,
                "reg_image_valid": False,
                "cr0": 0,
                "cr1": 0,
                "mask": 0,
            }
        if group is CommandGroup.CT and cmd == 4:  # GET_STATE
            return {
                "mode": 0,
                "amps": 0.0,
                "phase_deg": 0.0,
                "distortion": {
                    "dc_offset": 0.0,
                    "clip_fraction": 0.0,
                    "dropout_half_cycle": False,
                    "dropout_negative_half": False,
                    "apply_immediately": True,
                },
                "last_pwm_scale": 1.0,
                "valid": True,
            }
        if group is CommandGroup.RELAY and cmd == 1:  # GET_STATES
            mask = self._state["relay_state_mask"]
            return {
                "k1_closed": bool(mask & 0x01),
                "k2_closed": bool(mask & 0x02),
                "k3_closed": bool(mask & 0x04),
                "k5_closed": bool(mask & 0x08),
                "k4_closed": bool(mask & 0x10),
                "fault_line_asserted": self._state["fault_line_asserted"],
                "sample_time_us": self._state["sim_time_us"],
                "valid": True,
            }
        if group is CommandGroup.RELAY and cmd == 2:  # GET_EDGES
            return {"returned_count": 0, "edges": []}
        if group is CommandGroup.IO:
            if cmd == 3:  # READ
                return {"level": False}
            if cmd == 5:  # FAULT_LINE_GET
                return {
                    "asserted": self._state["fault_line_asserted"],
                    "sample_time_us": self._state["sim_time_us"],
                    "valid": True,
                }
            if cmd == 7:  # ESTOP_GET
                return {"open": self._state["estop_open"]}
            if cmd == 8:  # DUT_POWER_GET
                return {"on": True}
            if cmd == 4:  # ESTOP_SET
                self._state["estop_open"] = bool(payload.get("open", False))
                return {"ok": True}
        if group is CommandGroup.FAULT and cmd == 3:  # LIST
            return {"returned_count": 0, "faults": []}
        if group is CommandGroup.FAULT and cmd == 1:  # SCHEDULE
            slot = payload.get("fault_slot", payload.get("slot_id", 0))
            duration = payload.get("duration", {}) or {}
            duration_kind = duration.get("kind")
            if duration_kind in (2, "until_trigger"):
                # Frame 1 of the two-frame design: park, do not arm yet.
                self._pending_until_trigger.add(slot)
            else:
                # A direct PERMANENT/FOR FAULT_SCHEDULE discards any stale
                # pending UNTIL_TRIGGER entry for this slot (PROTOCOL.md sec
                # 5.6).
                self._pending_until_trigger.discard(slot)
            return {"fault_slot": slot}
        if group is CommandGroup.FAULT and cmd == 2:  # CANCEL
            slot = payload.get("fault_slot", payload.get("slot_id", 0))
            self._pending_until_trigger.discard(slot)
            return {"ok": True}
        if group is CommandGroup.FAULT and cmd == 5:  # SET_UNTIL_TRIGGER
            slot = payload.get("fault_slot", payload.get("slot_id", 0))
            if slot not in self._pending_until_trigger:
                raise SimLinkError(
                    f"FAULT/SET_UNTIL_TRIGGER: ERR_BAD_ARGS (no pending UNTIL_TRIGGER for slot {slot})"
                )
            self._pending_until_trigger.discard(slot)
            return {"fault_slot": slot}
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
