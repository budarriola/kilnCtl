"""Client for the firmware's GPIO_PROBE task (task 12): raw ESP32-S3 pin
control, gated behind ``CONFIG_KILNCTL_ENABLE_GPIO_PROBE`` (default off).

Exists to answer "is this net actually where the schematic says" without
building and flashing a one-off firmware -- ``tools/PcTools/TODO.md``
capability 1. On a board built without the option, ``UART_TASK_ID_GPIO_PROBE``
is never registered, so every call here times out exactly like any other
unregistered task_id -- that is a normal, expected outcome, not a bug in this
client or a sign the board is dead. Callers that want to distinguish "not
built with the probe" from "board not responding" should try a cheap query
(:meth:`ProbeClient.read_all`, which never touches a pin) and treat a timeout
as "probe not available" rather than escalating.

The firmware enforces its own deny-list and refuses writes while a profile is
running or paused (see ``gpio_probe.c``) -- this client does not duplicate
that policy locally, it only surfaces the refusal reason via
:class:`~kilnctrl.devices.GpioProbeRefused`.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Optional

from . import devices
from .devices import GpioProbePin, GpioProbeRefused, GpioProbeResponseError
from .protocol import (
    GPIO_PROBE_CMD_READ,
    GPIO_PROBE_CMD_READ_ALL,
    GPIO_PROBE_CMD_SET_MODE,
    GPIO_PROBE_CMD_WRITE,
    GPIO_PROBE_MODE_INPUT,
    GPIO_PROBE_MODE_INPUT_PULLDOWN,
    GPIO_PROBE_MODE_INPUT_PULLUP,
    GPIO_PROBE_MODE_OUTPUT,
    UART_TASK_ID_GPIO_PROBE,
    Device,
    Frame,
)
from .serial_link import SendResult, UartLink

log = logging.getLogger(__name__)

DEFAULT_REPLY_TIMEOUT_S = 2.0

#: Re-exported so callers can pass a mode without importing protocol.py too.
MODE_INPUT = GPIO_PROBE_MODE_INPUT
MODE_INPUT_PULLUP = GPIO_PROBE_MODE_INPUT_PULLUP
MODE_INPUT_PULLDOWN = GPIO_PROBE_MODE_INPUT_PULLDOWN
MODE_OUTPUT = GPIO_PROBE_MODE_OUTPUT


class ProbeQueryError(RuntimeError):
    """The ESP itself didn't answer -- delivery failure or timeout. Distinct
    from :class:`~kilnctrl.devices.GpioProbeRefused`, which means the ESP
    answered but said no."""

    def __init__(self, message: str, send_result: Optional[SendResult] = None) -> None:
        super().__init__(message)
        self.send_result = send_result


class _Pending:
    def __init__(self, subcommand: int) -> None:
        self.subcommand = subcommand
        self.event = threading.Event()
        self.value: object = None
        self.error: "Optional[BaseException]" = None


class ProbeClient:
    """Owns task :data:`UART_TASK_ID_GPIO_PROBE` on the PC side of a link.

    Thread-safety: every method here blocks on a UART round trip and must
    not be called from a GUI thread.
    """

    def __init__(self, link: UartLink, task_id: int = UART_TASK_ID_GPIO_PROBE) -> None:
        self.link = link
        self.task_id = task_id
        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._pending: Optional[_Pending] = None
        self._pending_lock = threading.Lock()
        self._query_lock = threading.RLock()

        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-gpio-probe-rx", daemon=True
        )
        self._consumer.start()

    def close(self) -> None:
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- requests --------------------------------------------------------
    def set_mode(self, gpio_num: int, mode: int, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> None:
        """Configure gpio_num as input / input-pullup / input-pulldown / output.

        Raises :class:`~kilnctrl.devices.GpioProbeRefused` for a deny-listed
        pin or a profile running/paused.
        """
        self._query(GPIO_PROBE_CMD_SET_MODE, devices.gpio_probe_set_mode(gpio_num, mode), timeout)

    def write(self, gpio_num: int, level: bool, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> None:
        """Drive gpio_num high/low. Refused unless it was already
        :meth:`set_mode`'d :data:`MODE_OUTPUT`."""
        self._query(GPIO_PROBE_CMD_WRITE, devices.gpio_probe_write(gpio_num, level), timeout)

    def read(self, gpio_num: int, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> bool:
        """Read gpio_num's current level. Works regardless of tracked mode --
        this does not reconfigure the pin, it only reads it."""
        return self._query(GPIO_PROBE_CMD_READ, devices.gpio_probe_read(gpio_num), timeout)  # type: ignore[return-value]

    def read_all(self, timeout: float = DEFAULT_REPLY_TIMEOUT_S) -> "list[GpioProbePin]":
        """Read every pin this connection has :meth:`set_mode`'d since the
        probe task started. Cheap and side-effect-free -- the recommended
        way to check whether the probe capability exists at all (see the
        module docstring)."""
        return self._query(GPIO_PROBE_CMD_READ_ALL, devices.gpio_probe_read_all(), timeout)  # type: ignore[return-value]

    def _query(self, subcommand: int, payload: bytes, timeout: float) -> object:
        with self._query_lock:
            pending = _Pending(subcommand)
            with self._pending_lock:
                self._pending = pending
            try:
                result = self.link.send(
                    dst_task=self.task_id, src_task=self.task_id, payload=payload,
                    dst_device=Device.ESP,
                )
                if not result.ok:
                    raise ProbeQueryError(
                        f"GPIO_PROBE request 0x{subcommand:02X} not delivered: "
                        f"{result.describe()}",
                        send_result=result,
                    )
                if not pending.event.wait(timeout):
                    raise ProbeQueryError(
                        f"GPIO_PROBE request 0x{subcommand:02X} was ACKed but no reply "
                        f"arrived within {timeout:.1f} s -- if this is the first call, "
                        "the board may not have been built with "
                        "CONFIG_KILNCTL_ENABLE_GPIO_PROBE (default off)"
                    )
                if pending.error is not None:
                    raise pending.error
                return pending.value
            finally:
                with self._pending_lock:
                    if self._pending is pending:
                        self._pending = None

    def _consume_loop(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inbox.get(timeout=0.2)
            except queue.Empty:
                continue
            try:
                self._handle_reply(frame)
            except Exception:  # pragma: no cover - never kill the consumer
                log.exception("error handling GPIO_PROBE frame")

    def _handle_reply(self, frame: Frame) -> None:
        try:
            subcommand, value = devices.parse_gpio_probe_response(frame.payload)
            error = None
        except GpioProbeRefused as exc:
            subcommand = frame.payload[0] if frame.payload else -1
            value = None
            error = exc
        except GpioProbeResponseError as exc:
            log.warning("dropping malformed GPIO_PROBE response: %s", exc)
            return

        with self._pending_lock:
            pending = self._pending
        if pending is not None and pending.subcommand == subcommand:
            pending.value = value
            pending.error = error
            pending.event.set()
            return

        log.debug("ignoring unsolicited GPIO_PROBE response 0x%02X", subcommand)


__all__ = [
    "ProbeClient",
    "ProbeQueryError",
    "MODE_INPUT",
    "MODE_INPUT_PULLUP",
    "MODE_INPUT_PULLDOWN",
    "MODE_OUTPUT",
]
