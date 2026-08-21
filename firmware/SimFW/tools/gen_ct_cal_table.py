#!/usr/bin/env python3
"""Generate SimFW's compiled-in CT calibration defaults header.

Reads a versioned calibration table produced by
``firmware/SimFW/tools/ct_calibration/calibrate_ct.py`` (see that package's
README) and emits ``firmware/SimFW/src/sim/ct_calibration_defaults.h``, the
per-channel gain/offset constants ``ct_cal_default_table()`` returns.

Why a generated header at all: ``firmware/SimFW/src/`` has no
config/flash-persistence subsystem (unlike ``firmware/SaftyFW/src/
config_store.c``), and building one is a real firmware subsystem. A
checked-in, regenerated header gets DESIGN_NOTES.md 3.3's *behavior* with no new
firmware subsystem -- the interim path ct_calibration/README.md's "Remaining
firmware work" section names. A per-unit flashable table is the follow-up.

**No calibration data exists today.** No CT hardware is on the bench and no
calibration run has ever been performed, so running this with no ``--json``
(the current, checked-in state) emits an all-UNCALIBRATED table, i.e. exact
identity behavior on every channel. That is the intended default: an
uncalibrated fixture must behave as it always has, never silently apply some
other fixture's constants.

Fit inversion
-------------
``calibration_table.py`` stores an obverse fit, ``measured_a = fit_gain *
commanded + fit_offset``. The firmware needs the inverse (amps -> command),
so this script inverts once, at generation time::

    gain   =  1 / fit_gain
    offset = -fit_offset / fit_gain

which is algebraically identical to ``ChannelCalibration.to_command()``'s
``(target_amps - fit_offset) / fit_gain``, leaving the on-target path a
single multiply-add.

Usage
-----
    # regenerate the all-uncalibrated identity default (current state)
    python firmware/SimFW/tools/gen_ct_cal_table.py

    # after a real bench calibration run
    python firmware/SimFW/tools/gen_ct_cal_table.py \
        --json firmware/SimFW/tools/ct_calibration/ct_calibration_table.json

    # CI-style: fail if the checked-in header is stale
    python firmware/SimFW/tools/gen_ct_cal_table.py --check

Exit codes: 0 ok, 1 bad/unreadable JSON table, 2 ``--check`` found the
checked-in header out of date.
"""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

#: Must match ct_calibration.h's CT_CAL_NUM_CHANNELS and wave_owner.h's
#: CT_WAVE_NUM_CHANNELS.
NUM_CHANNELS = 3

#: Must match calibration_table.py's SCHEMA_VERSION.
SCHEMA_VERSION = 1

TOOLS_DIR = Path(__file__).resolve().parent
SIMFW_DIR = TOOLS_DIR.parent
DEFAULT_OUT = SIMFW_DIR / "src" / "sim" / "ct_calibration_defaults.h"
DEFAULT_JSON = TOOLS_DIR / "ct_calibration" / "ct_calibration_table.json"


class TableError(ValueError):
    """Raised for a table this generator refuses to compile in."""


def load_channels(json_path: Path) -> "dict[int, tuple[float, float, str]]":
    """Returns {channel: (gain, offset, provenance)} in *firmware* (inverted)
    form. Refuses a table that was not crosstalk-validated, or whose fit gain
    is zero/non-finite -- the same "refuse rather than silently mis-map"
    instinct calibration_table.save() applies on the writing side."""
    raw = json.loads(json_path.read_text(encoding="utf-8"))
    version = raw.get("schema_version")
    if version != SCHEMA_VERSION:
        raise TableError(f"{json_path}: schema_version {version!r} != supported {SCHEMA_VERSION}")
    if not raw.get("crosstalk_passed", False):
        raise TableError(
            f"{json_path}: crosstalk_passed is false -- refusing to compile in a table "
            "built on an unproven channel mapping (see CURRENT_SENSE.md Sec.5)"
        )
    created_at = str(raw.get("created_at", "unknown"))

    out: "dict[int, tuple[float, float, str]]" = {}
    for ch_s, cal in raw.get("channels", {}).items():
        ch = int(ch_s)
        if not 0 <= ch < NUM_CHANNELS:
            raise TableError(f"{json_path}: channel {ch} outside 0..{NUM_CHANNELS - 1}")
        fit_gain = float(cal["gain"])
        fit_offset = float(cal["offset"])
        if fit_gain == 0.0 or fit_gain != fit_gain:  # zero or NaN
            raise TableError(f"{json_path}: channel {ch} fit gain {fit_gain!r} is not invertible")
        gain = 1.0 / fit_gain
        offset = -fit_offset / fit_gain
        prov = (
            f"fit r2={float(cal.get('r2', float('nan'))):.6f}, "
            f"n={int(cal.get('n_points', 0))}, "
            f"fit_gain={fit_gain!r}, fit_offset={fit_offset!r}, run {created_at}"
        )
        out[ch] = (gain, offset, prov)
    if not out:
        raise TableError(f"{json_path}: table carries zero channels")
    return out


def render(channels: "dict[int, tuple[float, float, str]]", source: str) -> str:
    lines = [
        "// ct_calibration_defaults.h -- GENERATED FILE, DO NOT EDIT BY HAND.",
        "//",
        "// Regenerate with:",
        "//     python firmware/SimFW/tools/gen_ct_cal_table.py [--json <table.json>]",
        "//",
        "// Source: " + source,
        "//",
        "// See src/sim/ct_calibration.h for what these constants mean and how the",
        "// PC-side fit (measured = fit_gain * commanded + fit_offset) is inverted",
        "// into the (gain, offset) pair stored here.",
        "#ifndef SIMFW_SIM_CT_CALIBRATION_DEFAULTS_H",
        "#define SIMFW_SIM_CT_CALIBRATION_DEFAULTS_H",
        "",
        '#include "ct_calibration.h"',
        "",
    ]
    if not channels:
        lines += [
            "// NO CALIBRATION DATA EXISTS. No CT hardware is on the bench and no",
            "// calibration run has ever been performed, so every channel below is",
            "// marked UNCALIBRATED: ct_cal_apply() falls back to exact identity,",
            "// pwm_scale = clamp(amps, 0, 1) -- byte-for-byte the behavior SimFW had",
            "// before the calibration path existed. Milestone M-D is NOT closed by",
            "// this file; only the ability to apply a calibration is.",
            "",
        ]
    lines += [
        "static const ct_cal_table_t CT_CAL_DEFAULT_TABLE = {",
        "    .channels = {",
    ]
    for ch in range(NUM_CHANNELS):
        entry = channels.get(ch)
        if entry is None:
            lines.append(
                f"        [{ch}] = {{ .calibrated = false, .gain = 0.0f, .offset = 0.0f }},"
                "  // UNCALIBRATED -- identity; gain/offset are ignored"
            )
        else:
            gain, offset, prov = entry
            lines.append(f"        // channel {ch}: {prov}")
            lines.append(
                f"        [{ch}] = {{ .calibrated = true, .gain = {gain:.9g}f, "
                f".offset = {offset:.9g}f }},"
            )
    lines += [
        "    },",
        "};",
        "",
        "#endif // SIMFW_SIM_CT_CALIBRATION_DEFAULTS_H",
        "",
    ]
    return "\n".join(lines)


def main(argv: "list[str] | None" = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument(
        "--json",
        type=Path,
        default=None,
        help=f"calibration table to compile in (default: none -> all-uncalibrated identity; "
             f"a real bench run writes {DEFAULT_JSON})",
    )
    ap.add_argument("--out", type=Path, default=DEFAULT_OUT, help=f"output header (default: {DEFAULT_OUT})")
    ap.add_argument("--check", action="store_true", help="do not write; exit 2 if the file on disk differs")
    args = ap.parse_args(argv)

    if args.json is None:
        channels: "dict[int, tuple[float, float, str]]" = {}
        source = "none -- no calibration run has been performed (all channels UNCALIBRATED / identity)"
    else:
        try:
            channels = load_channels(args.json)
        except (OSError, TableError, KeyError, ValueError) as exc:
            print(f"gen_ct_cal_table: {exc}", file=sys.stderr)
            return 1
        source = args.json.as_posix()

    text = render(channels, source)

    if args.check:
        current = args.out.read_text(encoding="utf-8") if args.out.exists() else ""
        if current.replace("\r\n", "\n") != text:
            print(f"gen_ct_cal_table: {args.out} is out of date -- rerun without --check", file=sys.stderr)
            return 2
        print(f"gen_ct_cal_table: {args.out} is up to date")
        return 0

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(text, encoding="utf-8", newline="\n")
    calibrated = sorted(channels)
    print(f"gen_ct_cal_table: wrote {args.out}")
    print(f"  calibrated channels: {calibrated if calibrated else 'NONE (identity on all channels)'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
