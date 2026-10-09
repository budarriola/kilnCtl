"""Shared test infrastructure for selfcheck.py's check-group submodules.

Part of the selfcheck.py split (pure refactor) -- moved verbatim, no logic
changes. Deliberately has no import back on ``selfcheck`` (or any
``selfcheck_*`` check-group module): selfcheck.py can be run either as a
script (``__name__ == "__main__"``) or imported as a module, and those are
two different entries in ``sys.modules`` for the same file. A back-import
here would either duplicate ``_failures`` (silently dropping every failure
recorded by a check-group module) or deadlock on a circular import,
depending on which way it happened to run first -- so this module is a pure
leaf: everything below imports it, it imports nothing that leads back here.
"""
from __future__ import annotations

import threading

from kilnctrl.protocol import Device

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


def _make_pair(esp_tasks=()):
    """Two cross-wired UartLinks plus their fake ports, ESP side pre-registered."""
    from kilnctrl.serial_link import UartLink

    a, b = FakePort("HOST"), FakePort("ESP")
    a.peer, b.peer = b, a
    host = UartLink(own_device=Device.HOST, ack_timeout=0.3)
    esp = UartLink(own_device=Device.ESP, ack_timeout=0.3)
    inboxes = [esp.register_task(task_id) for task_id in esp_tasks]
    _wire_up(host, a)
    _wire_up(esp, b)
    return a, b, host, esp, inboxes


def _responder(stop: threading.Event, inbox, esp, task_id, answer, seen=None):
    """Generic stub bridge task: answer queries, record fire-and-forget writes.

    ``answer(payload) -> reply bytes or None`` -- None means "this subcommand
    is a write, just ACK it", which is what the real firmware does for
    everything that isn't a query.
    """

    def loop() -> None:
        while not stop.is_set():
            try:
                msg = inbox.get(timeout=0.1)
            except Exception:
                continue
            if not msg.payload:
                continue
            reply = answer(msg.payload)
            if reply is None:
                if seen is not None:
                    seen.append(msg.payload)
                continue
            esp.send(
                dst_task=msg.src_task,
                src_task=task_id,
                payload=reply,
                dst_device=msg.src_device,
            )

    thread = threading.Thread(target=loop, daemon=True)
    thread.start()
    return thread
