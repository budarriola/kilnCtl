"""Client for the firmware's INFO task (task 3): pin config + FW version.

Why this is not just ``link.send()``: INFO is the only *query* channel in the
protocol. The request DATA frame is ACKed like any other (delivery
confirmation only -- OK / UNDELIVERABLE / TIMEOUT), and the answer arrives
afterwards as a **separate DATA frame** from (ESP, UART_TASK_ID_INFO)
addressed back to whoever asked, exactly as ``info_bridge_task()`` in
uart_bridge.c does it. The PC therefore has to be registered as task 3 itself
to receive anything.

On top of that, the firmware pushes an *unsolicited* version reply once at
boot (``info_boot_push_task``). Since a boot push and a query reply are
byte-identical DATA frames, the only thing that distinguishes them is whether
we asked -- which is why this module owns task 3's inbox with a single
consumer thread instead of letting each caller drain the queue: a query
registers itself as outstanding, and any version frame that arrives with
nothing outstanding is by definition a boot push, i.e. "the device just
rebooted".

Register early (at :class:`~kilnctrl.serial_link.UartLink` construction,
not lazily when some window opens) so a boot push landing on an
already-connected link is never missed.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Callable, Optional

from . import devices
from .devices import (
    FirmwareVersion,
    InfoResponseError,
    PinConfigEntry,
    StackMarginEntry,
    WifiStatus,
)
from .protocol import (
    INFO_CMD_GET_FW_VERSION,
    INFO_CMD_GET_PIN_CONFIG,
    INFO_CMD_GET_STACK_MARGIN,
    INFO_CMD_GET_WIFI_STATUS,
    UART_TASK_ID_INFO,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

#: How long to wait for the reply DATA frame *after* the request was ACKed.
#: The firmware builds replies from static tables, so this only has to cover
#: one more UART round trip plus its own send retries.
DEFAULT_REPLY_TIMEOUT_S = 2.0


class InfoQueryError(RuntimeError):
    """Raised when an INFO query cannot be completed.

    :attr:`send_result` is set when the failure was at the delivery layer
    (the request never got an ACK), and None when the request was delivered
    but no valid reply came back in time.
    """

    def __init__(self, message: str, send_result: Optional[SendResult] = None) -> None:
        super().__init__(message)
        self.send_result = send_result


class _Pending:
    """A single outstanding query: what we asked for, and where to put it."""

    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None


class InfoClient:
    """Owns task :data:`UART_TASK_ID_INFO` on the PC side of a link.

    Registers the task, drains its inbox on a background thread, satisfies
    outstanding queries, and reports unsolicited boot pushes through
    ``on_boot_push``.

    Thread-safety: :meth:`get_pin_config` / :meth:`get_fw_version` block and
    must not be called from a GUI thread; ``on_boot_push`` is invoked on the
    consumer thread, so a GUI callback has to marshal back itself.
    """

    def __init__(
        self,
        link: UartLink,
        on_boot_push: Optional[Callable[[FirmwareVersion], None]] = None,
        task_id: int = UART_TASK_ID_INFO,
    ) -> None:
        self.link = link
        self.task_id = task_id
        self.on_boot_push = on_boot_push

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        #: Set as soon as any FW version is known (query reply or boot
        #: push). Device-command callers (gui.py, mcp_server.py) check
        #: .compatible before sending anything besides INFO queries
        #: themselves -- see UART_PROTOCOL_VERSION in protocol.py.
        self.last_fw_version: Optional[FirmwareVersion] = None
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        #: Serializes queries so at most one reply is ever outstanding, which
        #: is what makes "unsolicited == nobody asked" a sound inference.
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-info-rx", daemon=True
        )
        self._consumer.start()

    @property
    def compatible(self) -> Optional[bool]:
        """Whether the last-known firmware speaks our protocol version.

        None until a FW version has actually been observed (no query made
        yet and no boot push seen) -- callers must decide for themselves how
        to treat "unknown" (gui.py/mcp_server.py let INFO queries through
        either way, since that's how compatibility gets discovered at all,
        but block THERMO/IO/DISPLAY/SAFETY sends on both None and False).
        """
        return self.last_fw_version.compatible if self.last_fw_version is not None else None

    # -- lifecycle ---------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release task 3. Safe to call twice."""
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- queries -----------------------------------------------------------
    def get_pin_config(
        self, timeout: float = DEFAULT_REPLY_TIMEOUT_S
    ) -> list[PinConfigEntry]:
        """Ask the device which GPIOs it has wired up, and to what.

        Raises :class:`InfoQueryError` on an undelivered request or a missing
        or malformed reply.
        """
        value = self._query(INFO_CMD_GET_PIN_CONFIG, devices.info_get_pin_config(), timeout)
        return value  # type: ignore[return-value]

    def get_fw_version(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> FirmwareVersion:
        """Ask the device for its git commit / build timestamp / dirty flag."""
        value = self._query(INFO_CMD_GET_FW_VERSION, devices.info_get_fw_version(), timeout)
        return value  # type: ignore[return-value]

    def get_wifi_status(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> WifiStatus:
        """Ask the device whether it's joined a network and, if so, its IP."""
        value = self._query(INFO_CMD_GET_WIFI_STATUS, devices.info_get_wifi_status(), timeout)
        return value  # type: ignore[return-value]

    def get_stack_margin(
        self, timeout: float = DEFAULT_REPLY_TIMEOUT_S
    ) -> list[StackMarginEntry]:
        """Ask the device for the live uxTaskGetStackHighWaterMark() reading
        of every task registered with stack_margin.c (App/drivers).

        This is the bench measurement KilnFW TODO.md section 13 needs before
        any of the six candidate internal-only task stacks it names may be
        resized -- see stack_margin.h's header comment. Raises
        :class:`InfoQueryError` on an undelivered request or a missing or
        malformed reply.
        """
        value = self._query(INFO_CMD_GET_STACK_MARGIN, devices.info_get_stack_margin(), timeout)
        return value  # type: ignore[return-value]

    def _query(self, subcommand: int, payload: bytes, timeout: float) -> object:
        with self._query_lock:
            pending = _Pending(subcommand)
            with self._pending_lock:
                self._pending = pending
            try:
                result = self.link.send(
                    dst_task=self.task_id,
                    src_task=self.task_id,
                    payload=payload,
                    dst_device=Device.ESP,
                )
                if not result.ok:
                    raise InfoQueryError(
                        f"INFO request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise InfoQueryError(
                        f"INFO request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s"
                    )
                return pending.value
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    # -- receive -----------------------------------------------------------
    def _consume_loop(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inbox.get(timeout=0.2)
            except queue.Empty:
                continue  # poll interval so close() is noticed promptly
            try:
                self._handle_reply(frame)
            except Exception:  # pragma: no cover - never kill the consumer
                log.exception("error handling INFO frame")

    def _handle_reply(self, frame: Frame) -> None:
        with self._pending_lock:
            pending = self._pending
        prefer = pending.subcommand if pending is not None else None

        try:
            subcommand, value = devices.parse_info_response(frame.payload, prefer=prefer)
        except InfoResponseError as exc:
            log.warning("dropping malformed INFO response: %s", exc)
            return

        if isinstance(value, FirmwareVersion):
            self.last_fw_version = value
            if not value.compatible:
                log.error(
                    "UART protocol version mismatch: device speaks v%d, pc_tools "
                    "speaks v%d -- device commands will be refused until this is "
                    "resolved (flash matching firmware or update pc_tools)",
                    value.protocol_version,
                    devices.UART_PROTOCOL_VERSION,
                )

        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.event.set()
            return

        # Nothing outstanding for this subcommand. A version reply here is the
        # firmware's once-per-boot push (info_boot_push_task) -- i.e. a reboot
        # signal. Anything else is a stale/duplicate reply we already gave up
        # on; drop it.
        if subcommand == INFO_CMD_GET_FW_VERSION and isinstance(value, FirmwareVersion):
            log.info("unsolicited FW version push (device booted): %s", value.describe())
            if self.on_boot_push is not None:
                self.on_boot_push(value)
        else:
            log.debug("ignoring unsolicited INFO response 0x%02X", subcommand)
