#!/usr/bin/env python3
"""CLI: implied sensor tau from PWM ripple in a recorded firing capture.

Diagnostic only; writes no tune or config. Exit 4 if the capture is in --gate-ids.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import iter_tune_ripple as r  # noqa: E402


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("capture")
    ap.add_argument("--window-s", type=float, default=60.0)
    ap.add_argument("--gate-ids", nargs="*", default=[],
                    help="sec 6.5 gate-set capture ids")
    ap.add_argument("--gain-c", type=float, default=None,
                    help="ripple gain, C per unit duty (required for a tau)")
    ap.add_argument("--out", default=None, help="JSON report path (default stdout)")
    a = ap.parse_args(argv)
    if not os.path.isfile(a.capture):
        print("capture not found: " + a.capture, file=sys.stderr)
        return r.EXIT_USAGE
    code, rep = r.analyse(a.capture, a.gate_ids, a.window_s, a.gain_c)
    text = json.dumps(rep, indent=2)
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(text + "\n")
    else:
        print(text)
    return code


if __name__ == "__main__":
    sys.exit(main())
