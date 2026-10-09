#!/usr/bin/env python3
"""Guard against SAFETY_CMD_SET_CT_CAL (0x19) coming back as a reachable
PC-side write surface.

Background (docs/audits -- see docs/CURRENT_SENSE.md section 5 and
firmware/SaftyFW/docs/CT_COMMISSIONING_PLAN.md "record layout" note):
config_store.h's `ct_cal[]` is a legacy end-to-end amps CORRECTION applied
to `amps[]` AFTER the ADC-counts-to-amps conversion. It is genuinely read
by current_sense.c, but only ever fed the S14/S15 WARN-only over/under-
current display thresholds -- never S3/S4/S9's presence detection, which
reads `zero_counts`/`k_ct_v_per_a` directly, a completely different field.
The PC-side write surface for it (`devices.safety_set_ct_cal()`,
`SafetyClient.set_ct_cal()`, `mcp_server_safety.safety_set_ct_cal()`) was
removed 2026-09-08 because an agent used it expecting to commission the CT
and clear a latched S3 trip; it silently did nothing relevant.

This is NOT a general "every written field must be read" check -- ct_cal[]
IS read, so that mechanical shape would not have caught the actual defect
(a write surface whose written field is read by the wrong consumer). What
IS mechanically checkable, and is exactly what this script enforces: the
two PC-side layers an operator or an agent actually calls
(`kilnctrl/safety.py`'s SafetyClient and `kilnctrl/mcp_server_safety.py`'s
MCP tool surface) must never reference the SAFETY_CMD_SET_CT_CAL wire
command again. `kilnctrl/devices_safety.py` is allowed to keep decoding a
driver-error refusal reply for it (a read-path table entry, not a writer),
so this check is scoped to the two caller-facing files rather than the
whole package.

Usage: python ct_cal_write_surface_check.py <repo_root>
Exit 0 = clean, 1 = the write surface (or a caller of it) is back.
"""
from __future__ import annotations

import sys
from pathlib import Path

# Files that must never write SAFETY_CMD_SET_CT_CAL again, and the specific
# symbols that would mean it came back. Keyed on symbol names, not prose,
# so a rename of any of these functions must also update this list rather
# than silently going unchecked.
GUARDED_FILES = [
    "tools/PcTools/src/kilnctrl/safety.py",
    "tools/PcTools/src/kilnctrl/mcp_server_safety.py",
]
# Actual-usage patterns only -- NOT a bare substring match on the old
# function name, because both guarded files legitimately mention
# `safety_set_ct_cal` in prose/docstrings explaining why it was removed.
# Each pattern below only matches a live code reference: the wire-command
# constant, a function definition, or a call with an argument list.
FORBIDDEN_PATTERNS = [
    "SAFETY_CMD_SET_CT_CAL",
    "def safety_set_ct_cal(",
    ".set_ct_cal(",
    "def set_ct_cal(",
]


def check(repo_root: Path) -> list[str]:
    failures: list[str] = []
    for rel in GUARDED_FILES:
        path = repo_root / rel
        if not path.is_file():
            failures.append(f"{rel}: file not found -- check is stale, update GUARDED_FILES")
            continue
        text = path.read_text(encoding="utf-8")
        for lineno, line in enumerate(text.splitlines(), start=1):
            for pattern in FORBIDDEN_PATTERNS:
                if pattern in line:
                    failures.append(
                        f"{rel}:{lineno}: references {pattern!r} -- the SAFETY_CMD_SET_CT_CAL "
                        "write surface must stay removed from this file (see this script's "
                        "module docstring)"
                    )
    return failures


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print("usage: ct_cal_write_surface_check.py <repo_root>", file=sys.stderr)
        return 2
    repo_root = Path(argv[1]).resolve()
    failures = check(repo_root)
    if failures:
        print("CT_CAL WRITE SURFACE CHECK: FAILED")
        for f in failures:
            print(f"  {f}")
        return 1
    print("CT_CAL WRITE SURFACE CHECK: ok -- SAFETY_CMD_SET_CT_CAL has no PC-side caller")
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
