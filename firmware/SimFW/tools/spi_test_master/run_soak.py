#!/usr/bin/env python3
"""run_soak.py -- host-side runner for the spi_test_master bench tool.

Opens the spi_test_master Pico's USB CDC serial port, sweeps its SPI clock
rate across a set of configurable points, and at each point runs the M-A
soak (>= 10k transactions, firmware/SimFW/docs/PLAN.md section 10) via the
firmware's own SOAK command. Prints a table suitable for pasting directly
into M-A's evidence writeup.

Standalone, plain Python 3 + pyserial. Deliberately NOT part of the
`kilnsim` PC-side package (that package doesn't exist yet -- see PLAN.md
section 6 -- and this is a one-off bring-up utility for a *different*,
scripted-test-master Pico, not the SimFW fixture itself). Requires pyserial:

    pip install pyserial

Usage:
    python run_soak.py --port COM5
    python run_soak.py --port COM5 --rates 100000 500000 1000000 2000000 5000000 --count 10000
    python run_soak.py --port COM5 --mode 0     # diagnose the CPHA ambiguity, see ../README.md

The firmware's line protocol (see ../src/main.c's HELP text) is plain
text, one command per line, one or more reply lines back -- this script
just drives that protocol; nothing here is a protocol of its own.
"""
import argparse
import re
import sys
import time

try:
    import serial
except ImportError:
    print("ERROR: pyserial is required (pip install pyserial)", file=sys.stderr)
    sys.exit(1)

DEFAULT_RATES_HZ = [100_000, 500_000, 1_000_000, 2_000_000, 3_000_000, 4_000_000, 5_000_000]
DEFAULT_SOAK_COUNT = 10_000
LINE_TIMEOUT_S = 30.0  # a 10k-iteration soak at slow rates can take a while; SOAK prints
                        # progress lines every 1000 iterations so this is really an
                        # inter-progress-line timeout, not a whole-soak timeout.


def open_port(port: str, baud: int = 115200) -> "serial.Serial":
    # USB CDC ACM ignores the requested baud (it's not a real UART), but
    # pyserial still wants a value -- 115200 is a harmless placeholder.
    ser = serial.Serial(port, baudrate=baud, timeout=LINE_TIMEOUT_S)
    return ser


def send_line(ser: "serial.Serial", line: str) -> None:
    ser.write((line.strip() + "\n").encode("ascii"))
    ser.flush()


def read_line(ser: "serial.Serial") -> str:
    raw = ser.readline()
    if not raw:
        raise TimeoutError(f"no response within {LINE_TIMEOUT_S}s (line dropped or firmware wedged)")
    return raw.decode("ascii", errors="replace").rstrip("\r\n")


def expect_ok_prefixed(ser: "serial.Serial", expect_prefix: str, max_lines: int = 5) -> str:
    """Reads lines until one starts with expect_prefix, ignoring banner/stray lines.
    Raises if none seen within max_lines."""
    for _ in range(max_lines):
        line = read_line(ser)
        if line.startswith(expect_prefix):
            return line
    raise RuntimeError(f"never saw a line starting with '{expect_prefix}'")


def ping(ser: "serial.Serial") -> bool:
    send_line(ser, "PING")
    try:
        line = read_line(ser)
    except TimeoutError:
        return False
    return line.strip() == "PONG"


SOAK_DONE_RE = re.compile(
    r"^SOAK DONE count=(\d+) failed_iterations=(\d+) mismatches=(\d+) "
    r"suspected_first_byte_late=(\d+) result=(PASS|FAIL)$"
)
SOAK_ABORT_RE = re.compile(r"^SOAK ABORT (.*)$")


def run_one_soak(ser: "serial.Serial", count: int) -> dict:
    """Runs one SOAK <count> and returns a dict of parsed results. Reads and
    discards SOAK progress lines along the way (the firmware prints one
    every 1000 iterations)."""
    send_line(ser, f"SOAK {count}")
    while True:
        line = read_line(ser)
        m = SOAK_DONE_RE.match(line)
        if m:
            return {
                "count": int(m.group(1)),
                "failed_iterations": int(m.group(2)),
                "mismatches": int(m.group(3)),
                "suspected_first_byte_late": int(m.group(4)),
                "result": m.group(5),
            }
        a = SOAK_ABORT_RE.match(line)
        if a:
            return {"result": "ABORT", "reason": a.group(1)}
        # else: a progress line ("SOAK progress ...") or something benign -- keep reading.


def set_rate(ser: "serial.Serial", hz: int) -> int:
    send_line(ser, f"RATE {hz}")
    line = expect_ok_prefixed(ser, "OK RATE")
    return int(line.split()[-1])


def set_mode(ser: "serial.Serial", cpha: int) -> None:
    send_line(ser, f"MODE {cpha}")
    expect_ok_prefixed(ser, "OK MODE")


def select_cs(ser: "serial.Serial", cs: int) -> None:
    send_line(ser, f"CS {cs}")
    expect_ok_prefixed(ser, "OK CS")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", required=True, help="serial port of the spi_test_master Pico (e.g. COM5)")
    ap.add_argument("--rates", type=int, nargs="+", default=DEFAULT_RATES_HZ,
                     help=f"clock rates to sweep, Hz (default: {DEFAULT_RATES_HZ})")
    ap.add_argument("--count", type=int, default=DEFAULT_SOAK_COUNT,
                     help=f"transactions per rate point (default {DEFAULT_SOAK_COUNT}, M-A's exit criterion floor)")
    ap.add_argument("--mode", type=int, choices=[0, 1], default=1,
                     help="SPI CPHA to test (0 or 1; CPOL always 0). Default 1 (datasheet mode 1). "
                          "See ../README.md for why MODE 0 exists as a diagnostic escape hatch.")
    ap.add_argument("--cs", type=int, default=None,
                     help="if set, pin every soak to this CS index instead of the firmware's "
                          "own per-iteration 0/1/2 rotation (useful to isolate a single-channel "
                          "problem after a multi-CS sweep flags one)")
    args = ap.parse_args()

    ser = open_port(args.port)
    time.sleep(0.3)
    ser.reset_input_buffer()

    if not ping(ser):
        print(f"ERROR: no PONG from {args.port} -- wrong port, or firmware not running "
              f"(reset the board and retry)", file=sys.stderr)
        return 2

    set_mode(ser, args.mode)
    if args.cs is not None:
        select_cs(ser, args.cs)

    print(f"spi_test_master M-A soak sweep -- port={args.port} mode(CPHA)={args.mode} "
          f"count/point={args.count}")
    print(f"{'rate_req_hz':>12} {'rate_act_hz':>12} {'result':>6} {'failed_iter':>12} "
          f"{'mismatches':>11} {'first_byte_late':>16}")
    print("-" * 82)

    rows = []
    overall_pass = True
    for hz in args.rates:
        actual = set_rate(ser, hz)
        send_line(ser, "RESET")
        expect_ok_prefixed(ser, "OK")
        result = run_one_soak(ser, args.count)
        rows.append((hz, actual, result))

        if result["result"] != "PASS":
            overall_pass = False

        if result["result"] == "ABORT":
            print(f"{hz:>12} {actual:>12} {'ABORT':>6} {'-':>12} {'-':>11} {'-':>16}  ({result['reason']})")
        else:
            print(f"{hz:>12} {actual:>12} {result['result']:>6} "
                  f"{result['failed_iterations']:>12} {result['mismatches']:>11} "
                  f"{result['suspected_first_byte_late']:>16}")

    print("-" * 82)
    print(f"Overall: {'PASS' if overall_pass else 'FAIL'} "
          f"({'all rate points clean' if overall_pass else 'see first FAIL/ABORT row above'})")
    print()
    print("M-A exit criterion reminder (docs/PLAN.md section 10): this table is necessary")
    print("evidence but not sufficient on its own -- M-A's actual exit criterion is a Saleae")
    print("capture showing correct mode-1 multi-byte reads with zero TX underruns over")
    print(">=10k transactions. Capture the target rate's run (see ../README.md) alongside this")
    print("table's PASS row for that rate.")

    ser.close()
    return 0 if overall_pass else 1


if __name__ == "__main__":
    sys.exit(main())
