#!/usr/bin/env python3
"""PC-side control and capture for the debug-UART temperature/telemetry feed.

Owner decision (2026-09-02): flash never holds per-tick temperature samples
any more (``event_log.h`` -- genuinely-necessary state-transition events
only); "loging of temps for debug should be done over the uart interface".
The firmware half of that already exists -- ``telemetry_log.c``'s ESP_LOGI
feed, default OFF, gated by ``telemetry_log_set_enabled()`` -- but nothing on
the PC side could reach that gate short of hand-crafting a SYSTEM frame, and
there was no capture path that fed the existing offline analysis tooling
(``log_analysis.py``). This module is both:

* **enable/disable/status** -- thin wrapper over
  :class:`~kilnctrl.system.SystemClient`'s ``*_telemetry_enabled`` methods
  (SYSTEM subcommands 0x05/0x06, ``protocol.py``), so an operator runs
  ``kilnctrl-telemetry enable`` instead of building the frame by hand.
* **capture** -- drains :class:`~kilnctrl.device_log.LogClient` (the SAME
  task-5 LOG channel every ``ESP_LOGx`` call already rides, per
  ``telemetry_log.h``'s own file banner) to a file, one line per log line:
  ``HH:MM:SS.mmm <level-letter> <raw text>`` -- deliberately the same shape
  ``console_capture.py``'s per-source files already use, so this is not a
  fourth ad hoc log-line format in this codebase. ``log_analysis.py``'s
  ``parse_profile_exec_uart_capture()`` reads exactly this shape back out
  into ``PollRow`` records, per that module's own "SOURCE KINDS" seam.

RATE / STARVATION. See ``docs/TELEMETRY_UART_CAPTURE.md`` for the full
numbers; summary: FIRE lines are emitted at most once per
``TELEMETRY_LOG_FIRING_PERIOD_S`` (5s, telemetry_log.c) at ~190B for a 3-zone
firing, i.e. ~38 B/s -- roughly 0.02% of the PC link's actual capacity
(``CONFIG_KILNCTL_UART_BAUD_RATE``, default 921600 baud on THIS wire; the
230400 baud figure quoted for "the isolated link" is a different UART
peripheral entirely -- the opto-isolated ESP<->RP2040 safety link,
``CONFIG_KILNCTL_SAFETY_BAUD_RATE`` -- telemetry never touches it, see
telemetry_log.h's own doc comment). The shared consumer-side risk is
``uart_log_bridge.c``'s 64-entry firmware queue and this module's LOG task
inbox (default depth 8, ``UartLink.register_task``'s default) filling with
OTHER log traffic (a boot burst, a warning storm) -- not telemetry itself,
which never produces more than about one entry at a time in steady state
(telemetry_log.c's own rate-budget comment). A drop shows up as a
"N log line(s) dropped (queue full)" WARN line arriving out of the capture's
otherwise-regular 5s cadence -- this module does not hide or de-duplicate
that line, it is just another captured LOG line.

Run standalone:

    python -m kilnctrl.telemetry_capture enable
    python -m kilnctrl.telemetry_capture capture --out run1.log
    python -m kilnctrl.telemetry_capture disable

or via the installed console script ``kilnctrl-telemetry``.
"""
from __future__ import annotations

import argparse
import logging
import sys
import time
from datetime import datetime
from pathlib import Path
from typing import Optional

from .device_log import LogClient
from .devices import LogLine
from .protocol import DEFAULT_BAUD_RATE
from .serial_link import UartLink
from .system import SystemClient, SystemQueryError

log = logging.getLogger(__name__)

#: tools/PcTools/  (this file is tools/PcTools/src/kilnctrl/telemetry_capture.py)
PROJECT_DIR = Path(__file__).resolve().parents[2]
CAPTURE_DIR = PROJECT_DIR / "logs" / "telemetry"

LINE_TIME_FORMAT = "%H:%M:%S.%f"


def format_capture_line(pc_time: float, level: str, text: str) -> str:
    """Render one captured LOG line -- same shape
    ``console_capture.format_event`` uses for a per-source (untagged) file,
    but time-of-day only (no date): ``HH:MM:SS.mmm <level> <text>``.
    Matched back apart by ``log_analysis._UART_CAPTURE_LINE_RE``.
    """
    stamp = datetime.fromtimestamp(pc_time).strftime(LINE_TIME_FORMAT)[:-3]
    return f"{stamp} {level} {text}"


class TelemetryCapture:
    """Owns one LogClient and one output file for a capture session.

    2026-09-02: this used to always open ``out_path`` in append mode, no
    questions asked. Running ``kilnctrl-telemetry capture --out run1.log``
    twice (e.g. after a Ctrl+C, or a second firing) silently concatenated
    two firings' lines into one file -- exactly the multi-session hazard
    ``a58f0dd`` fixed the same day for the other two ``PollRow`` sources
    (``coupling_pair_log.load_pair_run``'s ``MultiSessionError``,
    ``log_analysis.select_run``'s ``MultiRunError``), left open here because
    this capture path writes the file rather than reading it back. Now:
    by default the file must not already exist (refuses with a clear
    ``FileExistsError``-derived message naming the fix); pass
    ``append=True`` (``--append`` on the CLI) to opt in explicitly for a
    file you intend to keep appending to.
    """

    def __init__(self, link: UartLink, out_path: Path, append: bool = False) -> None:
        self.out_path = out_path
        out_path.parent.mkdir(parents=True, exist_ok=True)
        mode = "a" if append else "x"
        try:
            self._fh = open(out_path, mode, encoding="utf-8", newline="\n")
        except FileExistsError:
            raise RuntimeError(
                f"{out_path} already exists -- refusing to append blindly, that is exactly how a "
                f"telemetry poller left running across a kiln cooldown/reconnect used to concatenate "
                f"two firings into one file that parse_profile_exec_uart_capture then reads as one "
                f"continuous run. Pass --append if you really mean to keep appending to this file, "
                f"or choose a new --out path for a fresh capture."
            ) from None
        self.lines_written = 0

        def on_line(line: LogLine) -> None:
            self._fh.write(format_capture_line(time.time(), line.letter, line.text) + "\n")
            self._fh.flush()
            self.lines_written += 1

        self._client = LogClient(link, on_line=on_line)

    def close(self) -> None:
        self._client.close()
        try:
            self._fh.close()
        except Exception:  # pragma: no cover - defensive
            log.debug("error closing %s", self.out_path, exc_info=True)


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def _connect(port: Optional[str], baud: int) -> UartLink:
    link = UartLink(baudrate=baud)
    connected_port = link.connect(port)
    print(f"connected on {connected_port}")
    return link


def _cmd_enable(args: argparse.Namespace) -> int:
    link = _connect(args.port, args.baud)
    try:
        client = SystemClient(link)
        try:
            client.set_telemetry_enabled(True)
            enabled = client.get_telemetry_enabled()
            print(f"telemetry enabled={enabled}")
            return 0 if enabled else 1
        finally:
            client.close()
    finally:
        link.disconnect()


def _cmd_disable(args: argparse.Namespace) -> int:
    link = _connect(args.port, args.baud)
    try:
        client = SystemClient(link)
        try:
            client.set_telemetry_enabled(False)
            enabled = client.get_telemetry_enabled()
            print(f"telemetry enabled={enabled}")
            return 0 if not enabled else 1
        finally:
            client.close()
    finally:
        link.disconnect()


def _cmd_status(args: argparse.Namespace) -> int:
    link = _connect(args.port, args.baud)
    try:
        client = SystemClient(link)
        try:
            enabled = client.get_telemetry_enabled()
            print(f"telemetry enabled={enabled}")
            return 0
        finally:
            client.close()
    finally:
        link.disconnect()


def _cmd_capture(args: argparse.Namespace) -> int:
    link = _connect(args.port, args.baud)
    try:
        if args.enable:
            sys_client = SystemClient(link)
            try:
                sys_client.set_telemetry_enabled(True)
            finally:
                sys_client.close()

        out_path = Path(args.out) if args.out else CAPTURE_DIR / f"telemetry_{datetime.now():%Y%m%d_%H%M%S}.log"
        capture = TelemetryCapture(link, out_path, append=args.append)
        print(f"capturing to {out_path} -- Ctrl+C to stop")
        try:
            while True:
                time.sleep(0.5)
        except KeyboardInterrupt:
            print(f"\nstopped -- {capture.lines_written} line(s) written")
        finally:
            capture.close()
            if args.disable_on_exit:
                sys_client = SystemClient(link)
                try:
                    sys_client.set_telemetry_enabled(False)
                finally:
                    sys_client.close()
        return 0
    finally:
        link.disconnect()


def _build_arg_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Control and capture the debug-UART temperature/telemetry feed (telemetry_log.c)."
    )
    parser.add_argument("--port", default=None, help="ESP USB-UART bridge COM port. Autodiscovered if omitted.")
    parser.add_argument("--baud", type=int, default=DEFAULT_BAUD_RATE, help=f"PC link baud rate (default {DEFAULT_BAUD_RATE}).")
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("enable", help="Turn the telemetry feed on.").set_defaults(func=_cmd_enable)
    sub.add_parser("disable", help="Turn the telemetry feed off.").set_defaults(func=_cmd_disable)
    sub.add_parser("status", help="Report whether the telemetry feed is on.").set_defaults(func=_cmd_status)

    cap = sub.add_parser("capture", help="Capture the LOG task to a file, matching log_analysis's UART-capture shape.")
    cap.add_argument("--out", default=None, help=f"Output file (default {CAPTURE_DIR}/telemetry_<timestamp>.log).")
    cap.add_argument("--enable", action="store_true", help="Also enable telemetry before capturing.")
    cap.add_argument("--disable-on-exit", action="store_true", help="Also disable telemetry when capture stops.")
    cap.add_argument("--append", action="store_true",
                      help="Allow appending to an already-existing --out file instead of refusing "
                           "(default refuses, to avoid silently concatenating two firings into one "
                           "file that parse_profile_exec_uart_capture then reads as one continuous run).")
    cap.set_defaults(func=_cmd_capture)

    return parser


def main(argv: Optional[list] = None) -> int:
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")
    args = _build_arg_parser().parse_args(argv)
    try:
        return args.func(args)
    except RuntimeError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1
    except SystemQueryError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
