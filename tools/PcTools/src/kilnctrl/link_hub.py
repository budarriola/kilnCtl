"""Lets multiple pc_tools processes (the GUI, the MCP server, several MCP
clients, ...) share one physical UART link instead of exclusively fighting
over the OS serial port handle -- only one process can ever have the port
open via pyserial, so without this, starting the GUI while the MCP server is
already connected (or vice versa) just fails with "port busy".

Design: whichever process calls :func:`get_shared_link` first binds
``HUB_PORT`` on loopback and becomes the *hub* -- it owns the one real
:class:`~kilnctrl.serial_link.UartLink` talking to the physical port,
and serves every other process (including itself, via its own client
connection -- see below) over a small JSON-lines TCP protocol. A second
process that finds the port already bound simply becomes a *client* of the
first one's hub instead of standing up its own.

There is no leader election beyond "whoever gets there first, wins" and no
failover: if the hub process exits, every other client's connection to it
drops (surfacing as ``is_connected`` going False), and the physical port is
freed for whichever process next calls :func:`get_shared_link` to become the
new hub. Callers that need the link back after that have to be restarted --
this module doesn't attempt to migrate a live hub's state to a successor.

Both the hub process and every plain client end up holding the exact same
class, :class:`RemoteUartLink` -- the hub process is not special-cased to
talk to its own :class:`UartLink` directly, it just happens to be client #0
of a hub it also happens to be running. This keeps there being exactly one
code path (`RemoteUartLink`) that `gui.py`/`mcp_server.py` need to know
about, regardless of which role this process ended up playing.

Wire protocol (newline-delimited JSON, loopback only):

* client -> hub request:  ``{"id": N, "op": "...", ...fields}``
* hub -> client response: ``{"id": N, "ok": true, "result": ...}`` or
  ``{"id": N, "ok": false, "error": "..."}``
* hub -> client push (no "id"): ``{"event": "frame", "task_id": ..., ...}``
  for an inbound DATA frame on a task_id this client has registered, or
  ``{"event": "connection", "connected": ..., "port": ..., "baudrate": ...}``
  broadcast to *every* client whenever the shared link's connection state
  changes (including as a result of some other client's connect/disconnect),
  so ``RemoteUartLink.is_connected`` stays accurate without polling.

Ops: ``connect`` (fields: port), ``disconnect``, ``status``, ``send``
(fields: dst_task, src_task, dst_device, payload [base64], timeout),
``register_task`` (fields: task_id), ``unregister_task`` (fields: task_id).

register_task/unregister_task are subscriptions, not 1:1 registrations: the
hub registers a given task_id on the real UartLink at most once (on the
first subscriber) and fans out every inbound frame to every client
currently subscribed to it, so e.g. both the GUI's and the MCP server's own
InfoClient/LogClient can each independently "register" task INFO/LOG and
both see everything -- mirroring one physical device with several listeners,
not several independent devices.
"""

from __future__ import annotations

import base64
import json
import logging
import queue
import socket
import threading
import time
from typing import Optional

from .jsonl_util import iter_jsonl
from .protocol import (
    DEFAULT_BAUD_RATE,
    SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED,
    UART_TASK_ID_SYSTEM,
    Device,
    Frame,
    MsgType,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

HUB_HOST = "127.0.0.1"
HUB_PORT = 8765

#: uart_bridge.h's link watchdog (UART_BRIDGE_LINK_TIMEOUT_MS, 5000ms) counts
#: the link lost the instant 5s pass with no frame delivered to a bridge task
#: and no reply ACKed -- see that header's long "PC link watchdog" comment,
#: which spells out that this is "a heartbeat requirement on the host". Every
#: PC-side tool before this fix only spoke when a human or a script called
#: one: an MCP session sitting idle between tool calls, or the GUI open with
#: nothing clicked, produced exactly the silence the firmware cannot tell
#: apart from a pulled cable -- so it dropped every unowned relay and (with
#: CONFIG_KILNCTL_PC_LINK_LOSS_ASSERTS_FAULT on) asserted the isolated fault
#: line, forever, on a healthy link. Confirmed live on the bench 2026-08-28:
#: "PC link lost" / "PC link back" every ~10-20s with the MCP server
#: connected and idle, and firmware's own link_watchdog_task comment already
#: documented this exact failure ("observed dropping relays roughly every 5s
#: through a whole bench session").
#:
#: The fix belongs on the host, not the firmware: uart_bridge.h's contract
#: already says the host must keep talking, this module just never did. The
#: hub is the single place all of gui.py/mcp_server.py's traffic funnels
#: through (see the module docstring), so one heartbeat thread here covers
#: every PC-side client without each of them needing its own.
#:
#: Comfortably under UART_BRIDGE_LINK_TIMEOUT_MS/2 so a single dropped/slow
#: heartbeat still leaves room for a retry before the firmware's 5s window
#: closes.
_HEARTBEAT_INTERVAL_S = 1.5

#: A short per-attempt ACK budget: a heartbeat that is still waiting on one
#: attempt when the next interval fires would just pile up sends against the
#: link's tx_lock, and a heartbeat exists to prove the link is fast, not to
#: patiently wait out a slow one -- a real command from a client gets the
#: link's normal (longer, retried) timeout regardless.
_HEARTBEAT_ACK_TIMEOUT_S = 0.5

#: task_id the heartbeat sends as and listens on. Deliberately outside
#: 0-13 (every real UART_TASK_ID_* the firmware or any PC client ever
#: registers -- see protocol.py), so this can never collide with a genuine
#: subscriber's inbox (device_log.py holds task LOG open permanently, the
#: GUI's system panel opens/closes task SYSTEM around real queries, etc.):
#: this task id is never claimed by anything but the heartbeat itself, and
#: the reply the firmware sends back is drained and discarded here rather
#: than fanned out to any client.
_HEARTBEAT_TASK_ID = 200

#: How long a client waits for the hub's response to a given op. connect()
#: gets its own longer budget (port open can be slow); send() gets a budget
#: comfortably above the link's own worst-case retry time (max_retries *
#: ack_timeout, ~4s with the defaults) so a slow link doesn't look like a
#: broken hub connection.
_RPC_TIMEOUT_DEFAULT = 5.0
_RPC_TIMEOUT_CONNECT = 10.0
_RPC_TIMEOUT_SEND = 8.0


def hub_diagnosis() -> str:
    """One sentence saying what to DO about a dead hub socket, appended to the
    error a failed RPC raises.

    A bare socket errno is not actionable here: "[WinError 10054] An existing
    connection was forcibly closed by the remote host" is what a caller sees
    whether the hub process exited, was replaced by a newer one that took the
    port, or something unrelated is bound to HUB_PORT. Those need different
    responses, and the difference is one cheap loopback connect away, so make
    the error say which it is. Written for the case that actually happened on
    2026-08-25: the hub was simply gone, and three MCP calls were spent
    rediscovering that from an errno."""
    probe = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    probe.settimeout(0.5)
    try:
        probe.connect((HUB_HOST, HUB_PORT))
    except (OSError, socket.timeout):
        return (
            f"nothing is accepting connections on {HUB_HOST}:{HUB_PORT}, so the hub is gone "
            "(whichever process owned the serial port has exited) -- restart it with "
            "tools/PcTools/scripts/mcp_servers.ps1 restart, then connect() again"
        )
    else:
        return (
            f"something IS listening on {HUB_HOST}:{HUB_PORT}, so only THIS client's socket is "
            "dead -- most likely the old hub exited and a new one took the port. Call connect() "
            "again to re-attach; no restart needed"
        )
    finally:
        probe.close()


def _as_device(value: int) -> Device | int:
    try:
        return Device(value)
    except ValueError:
        return value


# ---------------------------------------------------------------------------
# hub (server) side
# ---------------------------------------------------------------------------
class _ClientHandler:
    """One TCP connection into the hub: reads JSON-line requests, dispatches
    them against the shared UartLink, and is the fan-out target for any
    task_id it has subscribed to."""

    def __init__(self, sock: socket.socket, hub: "LinkHub") -> None:
        self.sock = sock
        self.hub = hub
        self.subscribed_tasks: set[int] = set()
        self._write_lock = threading.Lock()

    def send_json(self, obj: dict) -> None:
        data = (json.dumps(obj) + "\n").encode("utf-8")
        try:
            with self._write_lock:
                self.sock.sendall(data)
        except OSError:
            pass  # the accept loop's read failure is what actually cleans this client up

    def run(self) -> None:
        try:
            f = self.sock.makefile("r", encoding="utf-8", newline="\n")
            for req in iter_jsonl(
                f, on_error=lambda line: log.debug("hub: dropping malformed request line: %r", line)
            ):
                if not isinstance(req, dict):
                    # Valid JSON, wrong shape (a bare number/list/string).
                    # req.get() would raise AttributeError out of this loop,
                    # which is caught nowhere and would kill this client's
                    # handler thread -- the client would just see its
                    # connection die with no error.
                    log.debug("hub: dropping non-object request: %r", req)
                    continue
                try:
                    self._handle(req)
                except Exception:  # pragma: no cover - never kill the handler
                    log.exception("hub: error handling request")
        except OSError:
            pass
        finally:
            self.hub.on_client_disconnect(self)
            try:
                self.sock.close()
            except OSError:
                pass

    def _handle(self, req: dict) -> None:
        op = req.get("op")
        rid = req.get("id")
        try:
            if op == "connect":
                result = self.hub.connect(req.get("port"))
                self._reply(rid, True, result)
                self.hub.broadcast_connection_state()
            elif op == "disconnect":
                self.hub.disconnect()
                self._reply(rid, True, None)
                self.hub.broadcast_connection_state()
            elif op == "ensure_connected":
                status = self.hub.ensure_connected()
                self._reply(rid, True, status)
                self.hub.broadcast_connection_state()
            elif op == "status":
                self._reply(rid, True, self.hub.link.status())
            elif op == "send":
                payload_b64 = req.get("payload") or ""
                result = self.hub.link.send(
                    dst_task=req["dst_task"],
                    src_task=req["src_task"],
                    payload=base64.b64decode(payload_b64) if payload_b64 else b"",
                    dst_device=_as_device(req.get("dst_device", int(Device.ESP))),
                    timeout=req.get("timeout"),
                )
                self._reply(rid, True, result.value)
            elif op == "register_task":
                task_id = req["task_id"]
                if task_id in self.subscribed_tasks:
                    raise ValueError(f"task {task_id} already registered by this client")
                self.hub.subscribe(self, task_id)
                self.subscribed_tasks.add(task_id)
                self._reply(rid, True, None)
            elif op == "unregister_task":
                task_id = req["task_id"]
                self.subscribed_tasks.discard(task_id)
                self.hub.unsubscribe(self, task_id)
                self._reply(rid, True, None)
            else:
                self._reply(rid, False, error=f"unknown op {op!r}")
        except Exception as exc:  # noqa: BLE001 - surfaced to the caller, not fatal here
            self._reply(rid, False, error=str(exc))

    def _reply(self, rid, ok: bool, result=None, error: Optional[str] = None) -> None:
        msg: dict = {"id": rid, "ok": ok}
        if ok:
            msg["result"] = result
        else:
            msg["error"] = error
        self.send_json(msg)


class LinkHub:
    """Owns the one real UartLink and fans it out to every connected client
    (see _ClientHandler)."""

    def __init__(self, listen_sock: socket.socket, own_device: Device = Device.HOST) -> None:
        self.link = UartLink(own_device=own_device)
        self._listen_sock = listen_sock
        self._clients: list[_ClientHandler] = []
        self._clients_lock = threading.Lock()
        #: Set by an explicit disconnect op, cleared by an explicit connect;
        #: while set, ensure_connected() never reopens the port.
        self._explicit_disconnect = False
        #: Port of the last successful open, reused by a lazy reconnect.
        self._last_port: Optional[str] = None
        #: task_id -> (inbox queue, drain thread, stop event); created lazily
        #: on first subscriber, torn down when the last one unsubscribes.
        self._drains: "dict[int, tuple[queue.Queue, threading.Thread, threading.Event]]" = {}
        #: Registered for the life of the hub, not lazily like _drains --
        #: the heartbeat has to run (and therefore needs its inbox open)
        #: whether or not any real client has ever subscribed to anything.
        self._heartbeat_inbox = self.link.register_task(_HEARTBEAT_TASK_ID)

    # -- connection ownership -----------------------------------------------
    def connect(self, port: Optional[str] = None) -> str:
        """Explicit connect: clears any explicit-disconnect latch."""
        opened = self.link.connect(port)
        self._explicit_disconnect = False
        self._last_port = opened
        return opened

    def disconnect(self) -> None:
        """Explicit disconnect: latches so ensure_connected() will not reopen."""
        self._explicit_disconnect = True
        self.link.disconnect()

    def ensure_connected(self) -> dict:
        """Lazy connect for a send that found the shared link closed.

        Reopens the port (the last one explicitly/lazily opened, else the
        autodiscovered recommendation -- what connect() with no port would
        pick, at the link's configured baud) unless the user explicitly
        disconnected. Never steals a port: if the open fails (held by another
        process, device absent) the link stays closed and the failure is
        logged, not raised -- the caller then reports NOT_CONNECTED as before.
        """
        if not self.link.is_connected and not self._explicit_disconnect:
            try:
                self._last_port = self.link.connect(self._last_port)
                log.info("lazy reconnect opened %s", self._last_port)
            except Exception as exc:  # noqa: BLE001 - stays disconnected
                log.warning("lazy reconnect failed: %s", exc)
        return self.link.status()

    def start(self) -> None:
        threading.Thread(target=self._accept_loop, daemon=True, name="link-hub-accept").start()
        threading.Thread(target=self._heartbeat_loop, daemon=True, name="link-hub-heartbeat").start()

    def _heartbeat_loop(self) -> None:
        """Keep uart_bridge.c's link watchdog fed while the port is open and
        no client happens to be talking -- see _HEARTBEAT_INTERVAL_S above
        for why this exists at all.

        Fire-and-forget: the outcome isn't checked. A dropped or NACKed
        heartbeat means nothing (the next one is 1.5s away, well inside the
        firmware's 5s window), and blocking here on retries would itself
        delay the next tick. Any reply frame that comes back is drained
        below so it never fills the dedicated inbox and trips the firmware's
        "inbox full, withholding ACK" path on some later heartbeat.
        """
        payload = bytes([SYSTEM_CMD_GET_WATCHDOG_PANIC_DISABLED])
        while True:
            time.sleep(_HEARTBEAT_INTERVAL_S)
            if self.link.is_connected:
                try:
                    self.link.send(
                        dst_task=UART_TASK_ID_SYSTEM,
                        src_task=_HEARTBEAT_TASK_ID,
                        payload=payload,
                        dst_device=Device.ESP,
                        timeout=_HEARTBEAT_ACK_TIMEOUT_S,
                    )
                except Exception:  # pragma: no cover - never kill the heartbeat
                    log.debug("heartbeat send failed", exc_info=True)
            while True:
                try:
                    self._heartbeat_inbox.get_nowait()
                except queue.Empty:
                    break

    def _accept_loop(self) -> None:
        while True:
            try:
                sock, _addr = self._listen_sock.accept()
            except OSError:
                break
            sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
            handler = _ClientHandler(sock, self)
            with self._clients_lock:
                self._clients.append(handler)
            threading.Thread(target=handler.run, daemon=True, name="link-hub-client").start()

    def on_client_disconnect(self, handler: _ClientHandler) -> None:
        with self._clients_lock:
            if handler in self._clients:
                self._clients.remove(handler)
        for task_id in list(handler.subscribed_tasks):
            self.unsubscribe(handler, task_id)

    def broadcast_connection_state(self) -> None:
        status = self.link.status()
        msg = {
            "event": "connection",
            "connected": status["connected"],
            "port": status["port"],
            "baudrate": status["baudrate"],
        }
        with self._clients_lock:
            targets = list(self._clients)
        for c in targets:
            c.send_json(msg)

    # -- subscriptions -------------------------------------------------------
    def subscribe(self, handler: _ClientHandler, task_id: int) -> None:
        with self._clients_lock:
            if task_id in self._drains:
                return  # already registered on the real link; handler just joins the fan-out
            inbox = self.link.register_task(task_id)
            stop = threading.Event()
            thread = threading.Thread(
                target=self._drain_loop, args=(task_id, inbox, stop), daemon=True,
                name=f"link-hub-drain-{task_id}",
            )
            self._drains[task_id] = (inbox, thread, stop)
            thread.start()

    def unsubscribe(self, handler: _ClientHandler, task_id: int) -> None:
        with self._clients_lock:
            still_wanted = any(task_id in c.subscribed_tasks for c in self._clients)
            if still_wanted or task_id not in self._drains:
                return
            _inbox, _thread, stop = self._drains.pop(task_id)
            stop.set()
        self.link.unregister_task(task_id)

    def _drain_loop(self, task_id: int, inbox: "queue.Queue[Frame]", stop: threading.Event) -> None:
        while not stop.is_set():
            try:
                frame = inbox.get(timeout=0.2)
            except queue.Empty:
                continue
            self._broadcast_frame(task_id, frame)

    def _broadcast_frame(self, task_id: int, frame: Frame) -> None:
        msg = {
            "event": "frame",
            "task_id": task_id,
            "device": int(frame.src_device),
            "src_task": frame.src_task,
            "msg_index": frame.msg_index,
            "payload": base64.b64encode(frame.payload).decode("ascii"),
        }
        with self._clients_lock:
            targets = [c for c in self._clients if task_id in c.subscribed_tasks]
        for c in targets:
            c.send_json(msg)


# ---------------------------------------------------------------------------
# client side
# ---------------------------------------------------------------------------
class RemoteUartLink:
    """Client-side proxy for :class:`LinkHub`: same connect/disconnect/send/
    register_task/status surface as :class:`UartLink`, but every operation is
    forwarded over a local TCP connection to whichever process actually owns
    the physical serial port. See :func:`get_shared_link`.
    """

    def __init__(self, own_device: Device = Device.HOST, host: str = HUB_HOST, port: int = HUB_PORT) -> None:
        self.own_device = own_device
        self._sock = socket.create_connection((host, port), timeout=_RPC_TIMEOUT_CONNECT)
        # create_connection's timeout is meant to bound the *connect*, but it
        # stays on the socket and applies to every later recv too -- so an
        # idle link (nobody sending, which is the normal resting state
        # between commands) made _read_loop's readline raise TimeoutError
        # after exactly _RPC_TIMEOUT_CONNECT seconds. That is an OSError
        # subclass, so _read_loop swallowed it and flipped _is_connected to
        # False: the GUI showed "Disconnected" exactly 10 s after the last
        # command, with a perfectly healthy board and port. Go back to
        # blocking for the socket's actual lifetime; per-request deadlines
        # are enforced at the application layer by _request's event.wait().
        self._sock.settimeout(None)
        self._sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

        self._write_lock = threading.Lock()
        self._next_id = 1
        self._id_lock = threading.Lock()
        self._pending: "dict[int, tuple[threading.Event, dict]]" = {}
        self._pending_lock = threading.Lock()

        self._subscribers: "dict[int, queue.Queue[Frame]]" = {}
        self._subscribers_lock = threading.Lock()

        self._is_connected = False
        self._port: Optional[str] = None
        self.baudrate = DEFAULT_BAUD_RATE

        self._stop = threading.Event()
        self._reader = threading.Thread(target=self._read_loop, daemon=True, name="link-client-rx")
        self._reader.start()

        try:
            self._apply_status(self._request("status"))
        except Exception:  # noqa: BLE001 - best-effort initial sync; state stays "disconnected"
            log.debug("initial status query to hub failed", exc_info=True)

    # -- lifecycle -----------------------------------------------------------
    @property
    def is_connected(self) -> bool:
        return self._is_connected

    @property
    def port(self) -> Optional[str]:
        return self._port

    def connect(self, port: Optional[str] = None) -> str:
        return self._request("connect", rpc_timeout=_RPC_TIMEOUT_CONNECT, port=port)

    def disconnect(self) -> None:
        """Disconnect the *shared* physical link -- affects every client, not
        just this one. For "I'm done, but leave the link up for everyone
        else", use close() instead (e.g. at process shutdown)."""
        self._request("disconnect")

    def close(self) -> None:
        """Tear down only this process's connection to the hub. Safe to call
        at shutdown even if other clients (or this same hub) are still using
        the shared link -- unlike disconnect(), this never touches the
        physical port."""
        self._stop.set()
        try:
            # Shut the socket down before closing it: on Windows, closing a
            # socket another thread is blocked reading does not reliably wake
            # that thread, so without this the reader can linger past close()
            # and the cleanup below is the only thing that runs on time.
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass
        self._is_connected = False
        # Nothing can answer an outstanding request now, so say so immediately
        # instead of leaving each caller to discover it by timing out.
        self._fail_pending("link hub client was closed")

    def status(self) -> dict:
        return self._request("status")

    # -- task registry ---------------------------------------------------------
    def register_task(self, task_id: int, inbox_len: int = 8) -> "queue.Queue[Frame]":
        with self._subscribers_lock:
            if task_id in self._subscribers:
                raise ValueError(f"task {task_id} already registered")
            inbox: "queue.Queue[Frame]" = queue.Queue(maxsize=inbox_len)
            self._subscribers[task_id] = inbox
        try:
            self._request("register_task", task_id=task_id)
        except Exception:
            with self._subscribers_lock:
                self._subscribers.pop(task_id, None)
            raise
        return inbox

    def unregister_task(self, task_id: int) -> None:
        with self._subscribers_lock:
            self._subscribers.pop(task_id, None)
        try:
            self._request("unregister_task", task_id=task_id)
        except Exception:  # noqa: BLE001 - best-effort, mirrors UartLink.unregister_task's leniency
            log.debug("unregister_task(%d) RPC failed", task_id, exc_info=True)

    # -- transmit --------------------------------------------------------------
    def send(
        self,
        dst_task: int,
        src_task: int,
        payload: bytes = b"",
        dst_device: Device = Device.ESP,
        timeout: Optional[float] = None,
    ) -> SendResult:
        if not self._is_connected:
            # The shared port may have closed without an explicit disconnect
            # (observed on the bench: safety_get_status failed "no serial port
            # open" after the server had been up a while). Ask the hub to
            # reopen it; the hub refuses if the user disconnected on purpose
            # and never takes a port another process holds.
            try:
                self._apply_status(self._request("ensure_connected", rpc_timeout=_RPC_TIMEOUT_CONNECT))
            except (TimeoutError, RuntimeError, OSError):
                return SendResult.NOT_CONNECTED
            if not self._is_connected:
                return SendResult.NOT_CONNECTED
        try:
            result = self._request(
                "send",
                rpc_timeout=_RPC_TIMEOUT_SEND,
                dst_task=dst_task,
                src_task=src_task,
                dst_device=int(dst_device),
                payload=base64.b64encode(payload).decode("ascii") if payload else "",
                timeout=timeout,
            )
        except (TimeoutError, RuntimeError, OSError):
            return SendResult.NOT_CONNECTED
        try:
            return SendResult(result)
        except ValueError:
            # The hub answered with something that isn't a SendResult (a
            # different pc_tools version, or something else entirely bound to
            # HUB_PORT). Report a failed send rather than raising into the
            # GUI worker / MCP tool that called us.
            log.warning("hub returned an unrecognized send result %r", result)
            return SendResult.TIMEOUT

    # -- RPC plumbing ------------------------------------------------------
    def _request(self, op: str, rpc_timeout: float = _RPC_TIMEOUT_DEFAULT, **fields) -> object:
        with self._id_lock:
            rid = self._next_id
            self._next_id += 1
        event = threading.Event()
        box: dict = {}
        with self._pending_lock:
            self._pending[rid] = (event, box)
        payload = {"id": rid, "op": op, **fields}
        data = (json.dumps(payload) + "\n").encode("utf-8")
        try:
            with self._write_lock:
                self._sock.sendall(data)
        except OSError as exc:
            with self._pending_lock:
                self._pending.pop(rid, None)
            raise RuntimeError(f"hub connection lost: {exc} -- {hub_diagnosis()}") from exc

        if not event.wait(rpc_timeout):
            with self._pending_lock:
                self._pending.pop(rid, None)
            raise TimeoutError(f"hub did not respond to {op!r} within {rpc_timeout:.1f}s")

        msg = box["msg"]
        if not msg.get("ok"):
            raise RuntimeError(msg.get("error") or f"{op!r} failed")
        return msg.get("result")

    def _read_loop(self) -> None:
        try:
            f = self._sock.makefile("r", encoding="utf-8", newline="\n")
            for msg in iter_jsonl(f, on_error="skip"):
                if self._stop.is_set():
                    break
                if not isinstance(msg, dict):
                    continue
                try:
                    if "event" in msg:
                        self._handle_event(msg)
                    else:
                        self._handle_response(msg)
                except Exception:  # noqa: BLE001 - never kill the reader
                    # A malformed push (missing//wrong-typed field, oversized
                    # payload) must cost one message, not this client's whole
                    # connection: losing the reader silently flips
                    # is_connected to False with healthy hardware.
                    log.warning("dropping malformed hub message", exc_info=True)
        except OSError:
            pass
        finally:
            # Reaching here means the socket to the hub is gone (hub process
            # exited, or it closed our connection) -- NOT that the serial
            # port dropped. Those look identical downstream (is_connected
            # goes False, the GUI just shows "Disconnected"), so say which
            # one it was: a hub that vanished points at another pc_tools
            # process, a serial drop points at the cable/board.
            # Only worth warning about if we actually believed we had a live
            # link: a reader ending on an already-disconnected client (or on
            # a normal close(), which sets _stop) is just teardown, not a
            # fault.
            if not self._stop.is_set() and self._is_connected:
                log.warning(
                    "connection to the link hub was lost (hub process exited?); "
                    "this client is now disconnected -- the physical port may still be fine"
                )
            self._is_connected = False
            self._fail_pending("connection to the link hub was lost")

    def _fail_pending(self, reason: str) -> None:
        """Release every in-flight RPC waiter once no answer can ever arrive.

        Without this, a hub that exits (or a socket that dies) leaves each
        caller blocked for its full rpc_timeout -- up to 8 s for a send, on a
        GUI worker thread -- before it learns something it could have been
        told immediately.
        """
        with self._pending_lock:
            pending, self._pending = self._pending, {}
        for event, box in pending.values():
            box["msg"] = {"ok": False, "error": reason}
            event.set()

    def _handle_event(self, msg: dict) -> None:
        if msg.get("event") == "frame":
            task_id = msg["task_id"]
            with self._subscribers_lock:
                inbox = self._subscribers.get(task_id)
            if inbox is not None:
                frame = Frame(
                    msg_type=MsgType.DATA,
                    msg_index=msg["msg_index"],
                    src_device=_as_device(msg["device"]),
                    src_task=msg["src_task"],
                    dst_device=self.own_device,
                    dst_task=task_id,
                    payload=base64.b64decode(msg["payload"]) if msg.get("payload") else b"",
                )
                try:
                    inbox.put_nowait(frame)
                except queue.Full:
                    log.warning("local inbox full for task %d, dropping frame", task_id)
        elif msg.get("event") == "connection":
            self._apply_status(msg)

    def _handle_response(self, msg: dict) -> None:
        rid = msg.get("id")
        with self._pending_lock:
            entry = self._pending.pop(rid, None)
        if entry is not None:
            event, box = entry
            box["msg"] = msg
            event.set()

    def _apply_status(self, d: dict) -> None:
        self._is_connected = bool(d.get("connected", False))
        self._port = d.get("port")
        if d.get("baudrate"):
            self.baudrate = d["baudrate"]


# ---------------------------------------------------------------------------
# factory
# ---------------------------------------------------------------------------
def get_shared_link(own_device: Device = Device.HOST) -> RemoteUartLink:
    """Become the hub if nobody already is, then return a client of it.

    Every process that wants the shared link -- GUI, MCP server, or a
    one-off script -- should call this instead of constructing UartLink
    directly. Safe to call more than once per process, though there is
    normally just one caller (gui.py / mcp_server.py, at startup).
    """
    listen_sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # The whole "first one to bind wins" election below depends on bind()
    # genuinely FAILING when a hub already holds the port. On Windows,
    # SO_REUSEADDR does not mean what it means on POSIX: it lets a second
    # process bind an already-bound listening port, so every process would
    # believe it was the hub. Two hubs means two UartLinks racing for one
    # COM port -- the loser's connect() blocks on an already-open port until
    # the RPC times out ("hub did not respond to 'connect' within 10.0s"),
    # and when whichever process actually owned the port exits, every client
    # of it silently flips to disconnected. Use SO_EXCLUSIVEADDRUSE there,
    # which is the flag that actually refuses a duplicate bind; keep
    # SO_REUSEADDR on POSIX, where it never permitted duplicate listeners
    # and only avoids TIME_WAIT rebind failures.
    if hasattr(socket, "SO_EXCLUSIVEADDRUSE"):  # Windows
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
    else:
        listen_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    try:
        listen_sock.bind((HUB_HOST, HUB_PORT))
        listen_sock.listen(16)
    except OSError:
        listen_sock.close()
        log.info("link hub already running elsewhere; connecting as a client")
    else:
        log.info("becoming the link hub on %s:%d", HUB_HOST, HUB_PORT)
        LinkHub(listen_sock, own_device=own_device).start()

    return RemoteUartLink(own_device=own_device, host=HUB_HOST, port=HUB_PORT)
