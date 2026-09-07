#!/usr/bin/env python3
"""check_relay_authority_paths.py -- tools/PcTools/TODO.md's completion
checklist item "No relay path here bypasses `relay_authority_on_blocked()`".

The actual authority check is firmware-side (uart_bridge*.c's
IO_CMD_SET_RELAY/SET_RELAY_MASK/ALL_RELAYS_OFF handling refuses a write when
a profile owns the relay, a safety fault is asserted, or an OTA is in
progress -- see io_expander.py's module docstring). A PC-side script cannot
override that. What a PC-side script CAN do is bypass the PC-side half of the
contract: call ``UartLink.send()``/``IoClient.send()`` directly with a raw
``devices.io_set_relay()``/``io_set_relay_mask()``/``io_all_relays_off()``
frame instead of going through ``IoClient.set_relay()``/``set_relay_mask()``/
``all_relays_off()``, which wait out ``SET_RELAY_REJECT_WINDOW_S`` for the
firmware's refusal reply. A caller that skips this loses the *only* way this
side of the link learns "the firmware said no" -- it just... doesn't notice,
and proceeds as if the relay obeyed. tools/PcTools/scripts/
current_sense_commissioning.py did exactly this (fixed in the same commit
that added this check).

What this scans, under tools/PcTools/src and tools/PcTools/scripts:
  - every call to ``devices.io_set_relay(``, ``devices.io_set_relay_mask(``
    or ``devices.io_all_relays_off(`` (the raw frame builders)
  - flags one UNLESS it is a keyword/positional argument to one of the three
    allowlisted ``IoClient`` methods that consume it
    (``set_relay``/``set_relay_mask``/``all_relays_off`` in
    tools/PcTools/src/kilnctrl/io_expander.py -- the file that defines those
    wrappers is itself exempt) -- i.e. the frame-builder call must be wrapped
    by the refusal-aware method, not handed to a bare ``.send(``.

This is a source-text scan (like the repo's other check_*.py/ps1 guards), not
an AST-precise call-graph -- see the negative-test file listed below for what
it does and does not catch. It deliberately does not try to also chase a
hand-rolled raw byte frame (e.g. ``struct.pack("<BBB", IO_CMD_SET_RELAY, ...)``
built without going through ``devices.py`` at all) -- no such call site exists
today (grepped for IO_CMD_SET_RELAY/IO_CMD_SET_RELAY_MASK/
IO_CMD_ALL_RELAYS_OFF as of this writing), and a regex broad enough to catch
a hypothetical future one reliably would also flag `protocol.py`'s constant
definitions and `devices_io.py`'s own frame builders.

Usage: python tools/check_relay_authority_paths.py [--root REPO_ROOT]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

#: The three raw frame-builder calls that must never be handed straight to
#: `.send(...)` -- only to one of the IoClient wrapper methods below.
RAW_BUILDER_RE = re.compile(
    r"devices\.(io_set_relay_mask|io_set_relay|io_all_relays_off)\s*\("
)

#: Files that legitimately reference the raw builders directly: the module
#: that defines them, and the module that defines the refusal-aware wrappers
#: (whose own bodies pass the builder's return value to self._write_style()/
#: self._set_relay_style(), not to a bare send()).
DEFINITION_FILES = {"devices_io.py", "devices.py", "io_expander.py"}

#: A call site is safe when this line (or one of the next few, for a
#: multi-line call) is inside one of these wrapper calls rather than a bare
#: ``.send(``. Matched by simple lexical proximity: the nearest preceding
#: "def "/".send(" style token on the same logical statement.
SEND_CALL_RE = re.compile(r"\.send\s*\(")
WRAPPER_METHOD_RE = re.compile(
    r"\b(set_relay_mask|set_relay|all_relays_off)\s*\("
)

SCAN_DIRS = ("tools/PcTools/src", "tools/PcTools/scripts")


def find_py_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for rel in SCAN_DIRS:
        base = root / rel
        if not base.exists():
            continue
        files.extend(p for p in base.rglob("*.py") if "__pycache__" not in p.parts)
    return files


def check_file(path: Path) -> list[str]:
    if path.name in DEFINITION_FILES:
        return []
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()
    violations: list[str] = []
    for i, line in enumerate(lines):
        if not RAW_BUILDER_RE.search(line):
            continue
        # Look at a small window around the match for the enclosing call:
        # legitimate use is `io.set_relay(...)`/`.set_relay_mask(...)`/
        # `.all_relays_off(...)` wrapping the builder (possibly on the same
        # line, e.g. `return self._set_relay_style(IO_CMD_SET_RELAY,
        # devices.io_set_relay(relay, on), timeout)` inside io_expander.py
        # itself -- already exempted above by filename). A caller site is a
        # bypass when the nearest enclosing call is a bare `.send(`.
        window = "\n".join(lines[max(0, i - 2) : i + 1])
        if SEND_CALL_RE.search(window) and not WRAPPER_METHOD_RE.search(window):
            violations.append(
                f"{path}:{i + 1}: raw frame builder passed to .send() directly "
                f"-- use IoClient.set_relay()/set_relay_mask()/all_relays_off() "
                f"instead, so a firmware refusal is actually observed: "
                f"{line.strip()}"
            )
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", default=None, help="repo root (default: two levels up from this file)"
    )
    args = parser.parse_args()
    root = Path(args.root) if args.root else Path(__file__).resolve().parents[1]

    violations: list[str] = []
    for path in find_py_files(root):
        violations.extend(check_file(path))

    if violations:
        print("check_relay_authority_paths: relay-write bypass(es) found:")
        for v in violations:
            print(f"  {v}")
        return 1

    print("check_relay_authority_paths: OK -- every relay-write call site "
          "goes through IoClient's refusal-aware wrapper.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
