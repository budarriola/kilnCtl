"""Client for the firmware's LOG task (task 5): device-side ESP_LOGx output.

Unlike INFO, this is a pure broadcast channel: the firmware forwards every
ESP_LOGx call as its own unsolicited DATA frame (see
``App/drivers/bridge/uart_log_bridge.c``) and never expects anything back on this
task_id, so there is no request/response pairing to track -- just an inbox to
drain and hand off.

Register early (at construction, not lazily when some terminal window opens)
so nothing logged between connect and the window opening is lost -- same
rationale as InfoClient registering task 3 up front for the boot-version
push.
"""

from __future__ import annotations

import logging
import queue
import threading
from typing import Callable, Optional

from . import devices
from .devices import LogLine
from .protocol import UART_TASK_ID_LOG, Frame
from .serial_link import UartLink

log = logging.getLogger(__name__)


class LogClient:
    """Owns task :data:`UART_TASK_ID_LOG` on the PC side of a link.

    Registers the task and drains its inbox on a background thread, handing
    each decoded :class:`~kilnctrl.devices.LogLine` to ``on_line``.

    Thread-safety: ``on_line`` is invoked on the consumer thread, so a GUI
    callback has to marshal back itself -- same rule as InfoClient's
    ``on_boot_push``.
    """

    def __init__(
        self,
        link: UartLink,
        on_line: Optional[Callable[[LogLine], None]] = None,
        task_id: int = UART_TASK_ID_LOG,
    ) -> None:
        self.link = link
        self.task_id = task_id
        self.on_line = on_line

        self._inbox: "queue.Queue[Frame]" = link.register_task(task_id)
        self._stop = threading.Event()
        self._consumer = threading.Thread(
            target=self._consume_loop, name="uart-log-rx", daemon=True
        )
        self._consumer.start()

    # -- lifecycle -----------------------------------------------------------
    def close(self) -> None:
        """Stop the consumer thread and release the task. Safe to call twice."""
        self._stop.set()
        if self._consumer.is_alive() and self._consumer is not threading.current_thread():
            self._consumer.join(timeout=2.0)
        self.link.unregister_task(self.task_id)

    # -- receive ---------------------------------------------------------------
    def _consume_loop(self) -> None:
        while not self._stop.is_set():
            try:
                frame = self._inbox.get(timeout=0.2)
            except queue.Empty:
                continue  # poll interval so close() is noticed promptly
            try:
                line = devices.parse_log_frame(frame.payload)
            except ValueError:
                log.debug("dropping malformed LOG frame: %r", frame.payload)
                continue
            if self.on_line is not None:
                try:
                    self.on_line(line)
                except Exception:  # pragma: no cover - never kill the consumer
                    log.exception("error handling device log line")
