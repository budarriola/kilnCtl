#!/usr/bin/env python3
"""Saleae Logic 2 automation-API helper.

Logic 2 exposes two local servers (both enabled from Preferences):

* the **automation server**, a gRPC endpoint on 127.0.0.1:10430, driven by the
  ``logic2-automation`` package -- that's what this module wraps;
* a built-in **MCP server** on 127.0.0.1:10530, registered in the repo's
  ``.mcp.json`` as ``saleae``, which an agent can call directly.

Use the MCP server for interactive, agent-driven capture. Use this module when
a capture has to be scripted alongside UART traffic -- e.g. arm the analyzer,
drive the DUT through :mod:`kilnctrl.actions`, then export the decode --
because the timing of those steps has to be controlled from one process.

Logic 2 must already be running; the automation API cannot launch it.
"""

from __future__ import annotations

import argparse
import logging
import os
import re
import sys
from contextlib import contextmanager
from typing import Iterator, Optional, Sequence

from saleae import automation

log = logging.getLogger(__name__)

#: Default gRPC port of the Logic 2 automation server (Preferences ->
#: "Enable automation server"). Overridable for a non-default Logic install
#: or a remote machine.
DEFAULT_HOST = os.environ.get("SALEAE_AUTOMATION_HOST", "127.0.0.1")
DEFAULT_PORT = int(os.environ.get("SALEAE_AUTOMATION_PORT", "10430"))


class LogicNotRunning(RuntimeError):
    """Logic 2 isn't running, or its automation server is disabled."""


@contextmanager
def manager(
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    connect_timeout: float = 5.0,
) -> Iterator[automation.Manager]:
    """Yield a connected :class:`saleae.automation.Manager`, closed on exit.

    Raises :class:`LogicNotRunning` instead of the raw gRPC error when nothing
    is listening, since that's by far the most common failure and the gRPC
    message ("failed to connect to all addresses") doesn't hint at the cause.
    """
    try:
        mgr = automation.Manager.connect(
            address=host, port=port, connect_timeout_seconds=connect_timeout
        )
    except Exception as exc:  # grpc raises its own error types
        raise LogicNotRunning(
            f"no Logic 2 automation server at {host}:{port} -- start Logic 2 and "
            f"enable Preferences -> Enable automation server ({exc})"
        ) from exc
    try:
        yield mgr
    finally:
        mgr.close()


def list_devices(
    include_simulation: bool = False, **kwargs
) -> Sequence[automation.DeviceDesc]:
    """Return the connected Saleae devices."""
    with manager(**kwargs) as mgr:
        return mgr.get_devices(include_simulation_devices=include_simulation)


#: Sample rate used when the caller doesn't pick one. The set of legal rates
#: depends on the device *and* on how many channels are enabled; the API has
#: no way to enumerate them, it only rejects a bad one -- and the rejection
#: message lists the legal set, which is what :func:`allowed_sample_rates`
#: reads. 25 MS/s is comfortably above what this fixture's I2C/SPI/UART lines
#: need and is legal on every channel count of the attached Logic 16.
DEFAULT_SAMPLE_RATE = 25_000_000

_RATE_RE = re.compile(r'"digital"\s*:\s*(\d+)')


def allowed_sample_rates(
    digital_channels: Sequence[int], device_id: Optional[str] = None, **kwargs
) -> list[int]:
    """Legal digital sample rates for this channel count, highest first.

    Works by deliberately requesting an impossible rate and parsing the legal
    set out of the resulting error -- the automation API exposes no direct
    query for it.
    """
    try:
        _start(digital_channels, 0.1, sample_rate=1, device_id=device_id, **kwargs)
    except automation.errors.SaleaeError as exc:
        rates = sorted({int(m) for m in _RATE_RE.findall(str(exc))}, reverse=True)
        if rates:
            return rates
        raise
    return []


def _start(
    digital_channels: Sequence[int],
    duration_seconds: float,
    sample_rate: int,
    digital_threshold_volts: Optional[float] = None,
    device_id: Optional[str] = None,
    **kwargs,
):
    """Open a manager and start one timed capture; returns (manager_cm, capture).

    Split out of :func:`capture` only so :func:`allowed_sample_rates` can reuse
    the same configuration path.
    """
    device_cfg = automation.LogicDeviceConfiguration(
        enabled_digital_channels=list(digital_channels),
        digital_sample_rate=sample_rate,
        # Not every device accepts an explicit threshold: the original Logic 16
        # exposes fixed voltage *ranges* and rejects any specific value, so the
        # default is to leave it unset and use whatever Logic is configured for.
        digital_threshold_volts=digital_threshold_volts,
    )
    capture_cfg = automation.CaptureConfiguration(
        capture_mode=automation.TimedCaptureMode(duration_seconds=duration_seconds)
    )
    cm = manager(**kwargs)
    mgr = cm.__enter__()
    try:
        cap = mgr.start_capture(
            device_id=device_id,
            device_configuration=device_cfg,
            capture_configuration=capture_cfg,
        )
    except BaseException:
        cm.__exit__(*sys.exc_info())
        raise
    return cm, cap


def capture(
    digital_channels: Sequence[int],
    duration_seconds: float,
    output_dir: str,
    sample_rate: int = DEFAULT_SAMPLE_RATE,
    digital_threshold_volts: Optional[float] = None,
    device_id: Optional[str] = None,
    filename: str = "capture.sal",
    **kwargs,
) -> str:
    """Run one timed digital capture and save it as a ``.sal`` file.

    Returns the path of the saved capture. ``sample_rate`` is in samples/second
    and must be one of the values :func:`allowed_sample_rates` reports for this
    channel count. ``digital_threshold_volts`` left as ``None`` keeps Logic's
    own threshold setting, which is the only thing some devices accept.
    """
    # Absolute: Logic 2 resolves a relative path against *its own* working
    # directory, not ours, so the .sal would silently land somewhere else.
    output_dir = os.path.abspath(output_dir)
    os.makedirs(output_dir, exist_ok=True)
    path = os.path.join(output_dir, filename)
    cm, cap = _start(
        digital_channels,
        duration_seconds,
        sample_rate=sample_rate,
        digital_threshold_volts=digital_threshold_volts,
        device_id=device_id,
        **kwargs,
    )
    try:
        with cap:
            cap.wait()
            cap.save_capture(filepath=path)
    finally:
        cm.__exit__(None, None, None)
    return path


def main(argv: Optional[Sequence[str]] = None) -> int:
    """CLI: ``python -m kilnctrl.logic_capture devices|capture``."""
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--host", default=DEFAULT_HOST)
    parser.add_argument("--port", type=int, default=DEFAULT_PORT)
    sub = parser.add_subparsers(dest="cmd", required=True)

    sub.add_parser("devices", help="list connected Saleae devices")

    cap = sub.add_parser("capture", help="run a timed digital capture")
    cap.add_argument(
        "--channels",
        default="0,1",
        help="comma-separated digital channel numbers (default 0,1)",
    )
    cap.add_argument("--seconds", type=float, default=1.0)
    cap.add_argument("--sample-rate", type=int, default=DEFAULT_SAMPLE_RATE)
    cap.add_argument(
        "--threshold",
        type=float,
        default=None,
        help="digital threshold in volts; omit to keep Logic's own setting "
        "(some devices reject an explicit value)",
    )
    cap.add_argument("--out", default="logs/saleae")

    rates = sub.add_parser(
        "rates", help="list legal sample rates for a channel count"
    )
    rates.add_argument("--channels", default="0,1")

    args = parser.parse_args(argv)
    logging.basicConfig(level=logging.INFO, stream=sys.stderr)
    conn = {"host": args.host, "port": args.port}

    try:
        if args.cmd == "devices":
            devices = list_devices(**conn)
            if not devices:
                print("no Saleae devices connected")
                return 1
            for dev in devices:
                print(f"{dev.device_id}: {dev.device_type}{' (sim)' if dev.is_simulation else ''}")
            return 0

        channels = [int(c) for c in args.channels.split(",") if c.strip()]

        if args.cmd == "rates":
            for rate in allowed_sample_rates(channels, **conn):
                print(f"{rate} ({rate / 1e6:g} MS/s)")
            return 0

        path = capture(
            digital_channels=channels,
            duration_seconds=args.seconds,
            output_dir=args.out,
            sample_rate=args.sample_rate,
            digital_threshold_volts=args.threshold,
            **conn,
        )
        print(f"saved {path}")
        return 0
    except LogicNotRunning as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
