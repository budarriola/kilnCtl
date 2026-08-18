#!/usr/bin/env python3
"""Per-processor console capture: one log file per processor, plus a merged one.

ROADMAP.md M1: "Per-processor console windows and log files, plus an
interleaved file." `tools/PcTools/TODO.md`, "Logging and consoles".

Two independent text streams feed this today:

* **ESP** -- the existing, structured path. Reuses :class:`~kilnctrl.serial_link.UartLink`
  and :class:`~kilnctrl.device_log.LogClient` (task ``UART_TASK_ID_LOG``), the
  same plumbing `get_device_log`/`get_device_log_json` in `mcp_server.py` and
  the GUI's Device Console window already use. No new wire code.
* **SAFETY** -- the Pico's bench console, plain ASCII lines over the Debug
  Probe's UART bridge (UART0, GP16/GP17 -- `firmware/SaftyFW/docs/HARDWARE.md`
  §7b). There is no PC-side reader for this yet (`tools/PcTools/TODO.md`
  confirms the wire-protocol LOG-relay path, device SAFETY on task 5, is not
  implemented in firmware -- `Frame` carries no source-device field to
  distinguish a relayed Pico line from the ESP's own). Until that relay
  exists, this module talks to the probe's UART bridge directly as a second,
  plain serial port -- no framing, one line of ASCII per read.

Both streams are timestamped on PC arrival (`time.time()`) -- neither
processor has an RTC this tool trusts, same reasoning as the MCP server's
`get_device_log_json` and `SAFETY_LINK.md`'s "age" field. That PC-arrival time
is the sort key for the interleaved file.

Each source gets its own timestamped file (`esp_YYYYmmdd_HHMMSS.log`,
`safety_YYYYmmdd_HHMMSS.log`) under ``tools/PcTools/logs/console/``, plus a
third ``interleaved_YYYYmmdd_HHMMSS.log`` with every line from both, sorted by
arrival time and tagged with its source.

Run standalone:

    python -m kilnctrl.console_capture --esp-port COM7 --safety-port COM9

or via the installed console script ``kilnctrl-console-capture``. Either
source may be omitted (``--no-esp`` / omit ``--safety-port``) to capture just
one processor. Ctrl+C stops cleanly and closes all three files.

Not yet done, because it needs firmware or protocol changes this task is not
scoped to touch (`tools/PcTools/TODO.md` "Logging and consoles" tracks these):
per-line transport tagging (relayed vs probe-UART vs RTT) beyond the
source-processor tag this module already applies, an RTT-over-SWD console
path, and reading the Pico's log over the wire-protocol relay once
`Frame`/`LogLine` carry a source device.
"""

from __future__ import annotations

import argparse
import logging
import queue
import sys
import threading
import time
from dataclasses import dataclass
from datetime import datetime
from pathlib import Path
from typing import Callable, Optional

import serial

from .device_log import LogClient
from .devices import LogLine
from .protocol import DEFAULT_BAUD_RATE
from .serial_link import UartLink

log = logging.getLogger(__name__)

#: tools/PcTools/  (this file is tools/PcTools/src/kilnctrl/console_capture.py)
PROJECT_DIR = Path(__file__).resolve().parents[2]
LOG_DIR = PROJECT_DIR / "logs" / "console"

TIMESTAMP_FORMAT = "%Y%m%d_%H%M%S"
LINE_TIME_FORMAT = "%Y-%m-%d %H:%M:%S.%f"

#: How often the interleaved writer sorts and flushes its buffer. Bounds how
#: far out of arrival order two nearly-simultaneous lines from different
#: source threads can land in the merged file -- see interleave().
FLUSH_INTERVAL_S = 0.25

SOURCE_ESP = "ESP"
SOURCE_SAFETY = "SAFETY"


# ---------------------------------------------------------------------------
# pure logic -- testable with synthetic events, no serial ports involved
# ---------------------------------------------------------------------------
@dataclass(frozen=True)
class ConsoleEvent:
    """One console line from one processor, timestamped on PC arrival."""

    pc_time: float
    source: str  # SOURCE_ESP or SOURCE_SAFETY
    text: str
    level: str = "I"  # single-char level tag, ESP-IDF convention ('?' if unknown)


def format_event(evt: ConsoleEvent, *, tag_source: bool) -> str:
    """Render one event as a log line.

    ``tag_source=True`` (the interleaved file) prefixes ``[SOURCE]``; the
    per-processor files already say which processor they are via the
    filename, so they omit it.
    """
    stamp = datetime.fromtimestamp(evt.pc_time).strftime(LINE_TIME_FORMAT)[:-3]
    prefix = f"[{evt.source}] " if tag_source else ""
    return f"{stamp} {evt.level} {prefix}{evt.text}"


def interleave(events: "list[ConsoleEvent]") -> "list[ConsoleEvent]":
    """Sort events by PC arrival time. Stable, so same-timestamp lines keep
    the order they were appended in (i.e. the order they were queued)."""
    return sorted(events, key=lambda e: e.pc_time)


# ---------------------------------------------------------------------------
# per-source file writer
# ---------------------------------------------------------------------------
class _SourceFile:
    """One append-only, line-buffered log file for a single source."""

    def __init__(self, path: Path) -> None:
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        self._fh = open(path, "a", encoding="utf-8", newline="\n")

    def write(self, line: str) -> None:
        self._fh.write(line + "\n")
        self._fh.flush()

    def close(self) -> None:
        try:
            self._fh.close()
        except Exception:  # pragma: no cover - defensive
            log.debug("error closing %s", self.path, exc_info=True)


# ---------------------------------------------------------------------------
# live capture
# ---------------------------------------------------------------------------
class MultiConsoleCapture:
    """Owns 0-2 live source readers, their per-source files, and the merge file.

    Each source pushes :class:`ConsoleEvent` onto a shared queue as lines
    arrive; a single writer thread drains it, appends to that source's file
    immediately, and buffers for the interleaved file, sorting and flushing
    the buffer every :data:`FLUSH_INTERVAL_S`.
    """

    def __init__(self, log_dir: Path = LOG_DIR) -> None:
        self.log_dir = Path(log_dir)
        self._queue: "queue.Queue[ConsoleEvent]" = queue.Queue()
        self._stop = threading.Event()
        self._writer: Optional[threading.Thread] = None
        self._files: dict[str, _SourceFile] = {}
        self._interleaved: Optional[_SourceFile] = None
        self._readers: "list[Callable[[], None]]" = []  # close callbacks

        ts = datetime.now().strftime(TIMESTAMP_FORMAT)
        self._ts = ts

    # -- sources -------------------------------------------------------------
    def add_esp(self, link: UartLink) -> None:
        """Start capturing the ESP's LOG task on an already-connected link."""
        self._files[SOURCE_ESP] = _SourceFile(self.log_dir / f"esp_{self._ts}.log")

        def on_line(line: LogLine) -> None:
            self._queue.put(
                ConsoleEvent(pc_time=time.time(), source=SOURCE_ESP, text=line.text, level=line.letter)
            )

        client = LogClient(link, on_line=on_line)
        self._readers.append(client.close)

    def add_safety_probe_uart(
        self, port: str, baudrate: int = DEFAULT_BAUD_RATE, timeout: float = 0.5
    ) -> None:
        """Start capturing the Pico's bench console over the Debug Probe's
        UART bridge (plain ASCII lines, no framing -- see module docstring).
        """
        self._files[SOURCE_SAFETY] = _SourceFile(self.log_dir / f"safety_{self._ts}.log")

        ser = serial.Serial(port=port, baudrate=baudrate, timeout=timeout)
        stop = threading.Event()

        def read_loop() -> None:
            while not stop.is_set():
                try:
                    raw = ser.readline()
                except serial.SerialException:
                    log.warning("SAFETY console port %s closed/errored", port, exc_info=True)
                    return
                if not raw:
                    continue  # timeout, no data -- poll again so stop is noticed
                text = raw.decode("ascii", errors="replace").rstrip("\r\n")
                if not text:
                    continue
                self._queue.put(
                    ConsoleEvent(pc_time=time.time(), source=SOURCE_SAFETY, text=text, level="?")
                )

        thread = threading.Thread(target=read_loop, name="safety-console-rx", daemon=True)
        thread.start()

        def _close() -> None:
            stop.set()
            try:
                ser.close()
            except Exception:  # pragma: no cover - defensive
                pass
            thread.join(timeout=2.0)

        self._readers.append(_close)

    # -- lifecycle -------------------------------------------------------------
    def start(self) -> None:
        if len(self._files) > 1:
            self._interleaved = _SourceFile(self.log_dir / f"interleaved_{self._ts}.log")
        self._writer = threading.Thread(target=self._writer_loop, name="console-capture-writer", daemon=True)
        self._writer.start()

    def stop(self) -> None:
        for close in self._readers:
            try:
                close()
            except Exception:  # pragma: no cover - defensive
                log.debug("error closing a console source", exc_info=True)
        self._stop.set()
        if self._writer is not None:
            self._writer.join(timeout=FLUSH_INTERVAL_S * 4 + 1.0)
        for f in self._files.values():
            f.close()
        if self._interleaved is not None:
            self._interleaved.close()

    # -- writer thread -----------------------------------------------------
    def _writer_loop(self) -> None:
        buffer: "list[ConsoleEvent]" = []
        last_flush = time.monotonic()
        while True:
            timeout = max(0.0, FLUSH_INTERVAL_S - (time.monotonic() - last_flush))
            try:
                evt = self._queue.get(timeout=timeout or 0.01)
            except queue.Empty:
                evt = None
            if evt is not None:
                f = self._files.get(evt.source)
                if f is not None:
                    f.write(format_event(evt, tag_source=False))
                buffer.append(evt)
            now = time.monotonic()
            if now - last_flush >= FLUSH_INTERVAL_S or (self._stop.is_set() and evt is None):
                self._flush(buffer)
                buffer = []
                last_flush = now
                if self._stop.is_set() and self._queue.empty():
                    return

    def _flush(self, buffer: "list[ConsoleEvent]") -> None:
        if not buffer or self._interleaved is None:
            return
        for evt in interleave(buffer):
            self._interleaved.write(format_event(evt, tag_source=True))


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Capture ESP and/or SAFETY (Pico) console output to per-processor "
        "and interleaved log files under tools/PcTools/logs/console/."
    )
    parser.add_argument(
        "--esp-port", default=None,
        help="ESP USB-UART bridge COM port. Autodiscovered if omitted (see serial_link.recommend_port).",
    )
    parser.add_argument("--no-esp", action="store_true", help="Skip the ESP source entirely.")
    parser.add_argument(
        "--safety-port", default=None,
        help="SAFETY (Pico) Debug Probe UART bridge COM port. No autodiscovery -- "
        "check Device Manager / serial_link.list_ports() for the probe's CDC port. "
        "Omit to skip this source.",
    )
    parser.add_argument("--safety-baud", type=int, default=115200, help="SAFETY console baud rate (default 115200).")
    parser.add_argument(
        "--esp-baud", type=int, default=DEFAULT_BAUD_RATE,
        help=f"ESP link baud rate (default {DEFAULT_BAUD_RATE}).",
    )
    parser.add_argument("--log-dir", default=None, help="Override the log directory (default tools/PcTools/logs/console/).")
    return parser


def main(argv: Optional[list] = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")
    args = _build_arg_parser().parse_args(argv)

    if args.no_esp and not args.safety_port:
        print("error: --no-esp given with no --safety-port -- nothing to capture", file=sys.stderr)
        return 2

    log_dir = Path(args.log_dir) if args.log_dir else LOG_DIR
    capture = MultiConsoleCapture(log_dir)
    link: Optional[UartLink] = None

    try:
        if not args.no_esp:
            link = UartLink(baudrate=args.esp_baud)
            port = link.connect(args.esp_port)
            print(f"ESP: connected on {port}")
            capture.add_esp(link)

        if args.safety_port:
            capture.add_safety_probe_uart(args.safety_port, baudrate=args.safety_baud)
            print(f"SAFETY: reading console on {args.safety_port}")

        capture.start()
        for source, f in capture._files.items():
            print(f"  {source} -> {f.path}")
        if capture._interleaved is not None:
            print(f"  interleaved -> {capture._interleaved.path}")
        print("Capturing. Ctrl+C to stop.")

        while True:
            time.sleep(0.5)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except KeyboardInterrupt:
        print("\nstopping...")
    finally:
        capture.stop()
        if link is not None and link.is_connected:
            link.disconnect()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
