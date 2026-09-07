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

2026-09-07 extension: this script also scans firmware/KilnFW/App's C sources
for a direct call to kiln_io_set_relay()/kiln_io_set_relay_mask()/
kiln_io_all_relays_off() from outside kiln_io.c (where they're defined),
kiln_io_owner.c (the one module allowed to call them directly), and a small
allowlist of documented fail-safe paths that must keep working even if the
owner task itself is wedged (main.c's panic/shutdown path, profile_
executor.c's watchdog). See the FW_* section below for the full reasoning
-- this is the mechanical half of the "bypassed owner module" bug class
audit (docs/audits/relay_write_paths_2026-09-07.md), which found every
existing call site already correctly routed or allowlisted.

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

# --- Firmware side (2026-09-07 extension) -----------------------------------
#
# The PC-side check above answers "did the PC observe a firmware refusal".
# It says nothing about the firmware itself: kiln_io_owner.h documents a
# 2026-08-19 audit that found FIVE independent callers writing the SX1509
# relay register through kiln_io_set_relay()/kiln_io_set_relay_mask() with
# no shared ownership/safety-fault gate and no serialization against each
# other's read-modify-write -- the "bypassed owner module" bug class (see
# CLAUDE.md/memory). All five were fixed by routing through kiln_io_owner.c's
# queue. Nothing mechanical stopped a SIXTH caller from reintroducing a
# direct call later, which is what this half of the script checks for.
#
# What counts as a write: a call to kiln_io_set_relay(), kiln_io_set_relay_
# mask(), or kiln_io_all_relays_off() (kiln_io.h) -- the three functions that
# reach the SX1509 relay bits. SX1509_write_masked/_port/_pin are one layer
# lower still and are used ONLY by kiln_io.c itself (grepped as of this
# writing) -- not scanned separately because a regex loose enough to catch a
# hypothetical future direct SX1509 call would also flag SX1509.c's own
# definitions and kiln_io.c's legitimate use of its own primitives.
#
# Legitimate direct callers (allowlisted below, WHY inline at each entry):
#   - kiln_io.c: defines the three functions.
#   - kiln_io_owner.c: the owner module itself -- the one task that is
#     allowed to reach the expander, per kiln_io_owner.h's whole design.
#   - main.c's main_kiln_enter_safe_state(): the panic/shutdown path.
#     kiln_io_owner.h's top comment explains why this and the two entries
#     below stay direct: they must still work when the owner task ITSELF is
#     the thing that's wedged, so routing them through its queue would be
#     exactly backwards. kiln_io_all_relays_off() is unconditional and
#     only ever turns things off, so a race with owner_task here is benign
#     (worst case a redundant I2C transaction, never an unsafe state).
#   - profile_executor.c's watchdog_task_entry() (guard 9 / FAULT /
#     RETRY_RELAYS_OFF): same reasoning -- must still force relays off when
#     the main control task has stopped ticking, which is a symptom the
#     owner task's own health says nothing about.
#
# kiln_io_set_relay()/kiln_io_set_relay_mask() (as opposed to _all_relays_
# off()) have NO documented direct-call exception anywhere in this codebase
# -- every legitimate caller of those two goes through kiln_io_owner's
# MANUAL producers (kiln_io_owner_command_set_relay[_mask]()) or its
# AUTHORIZED producer (kiln_io_owner_command_set_relay_mask_authorized()).
# A direct call to either from outside kiln_io.c/kiln_io_owner.c is always
# flagged, with no allowlist entry available.
FW_SCAN_DIRS = ("firmware/KilnFW/App",)
FW_EXCLUDE_DIR_PARTS = ("test", "__pycache__", "build")

FW_DEFINITION_FILES = {"kiln_io.c", "kiln_io.h"}
FW_OWNER_FILES = {"kiln_io_owner.c", "kiln_io_owner.h"}

#: (filename, function-name substring) -- a direct kiln_io_all_relays_off()
#: call is allowed only inside one of these functions in one of these files.
#: Anywhere else (including a DIFFERENT function in the same file) is flagged.
FW_ALL_OFF_ALLOWLIST = {
    ("main.c", "main_kiln_enter_safe_state"),
    ("profile_executor.c", "watchdog_task_entry"),
}

FW_CALL_RE = re.compile(
    r"\b(kiln_io_set_relay_mask|kiln_io_set_relay|kiln_io_all_relays_off)\s*\("
)
#: Crude but sufficient for this codebase's style: a function definition
#: line is a name at column 0 (no indent) followed by "(" somewhere before
#: the line's end, with a return-type-looking token before it. We only need
#: "what function am I currently inside", found by scanning upward for the
#: nearest such line -- good enough given the file sizes involved (checked
#: against every file this scans as of writing; no false split found).
FUNC_DEF_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*]*\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*$")


def _strip_c_comments(text: str) -> str:
    # Block comments first (non-greedy, DOTALL), then line comments. Good
    # enough for this repo's style -- it does not need to survive a string
    # literal containing "/*", which none of these call sites do. Each
    # block comment is replaced by the SAME number of newlines it spanned
    # (rather than deleted outright) so line numbers reported below stay
    # aligned with the original file -- a multi-line block comment
    # collapsing to zero lines was found, during this check's own negative
    # test, to shift every subsequent match's reported line number.
    def _blank_block(m: re.Match) -> str:
        return "\n" * m.group(0).count("\n")

    text = re.sub(r"/\*.*?\*/", _blank_block, text, flags=re.DOTALL)
    text = re.sub(r"//.*", "", text)
    return text


def _enclosing_function(lines: list[str], call_line_idx: int) -> str | None:
    for i in range(call_line_idx, -1, -1):
        line = lines[i]
        if line.startswith(("    ", "\t", "}")) or not line.strip():
            continue
        m = FUNC_DEF_RE.match(line)
        if m:
            return m.group(1)
    return None


def find_fw_c_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for rel in FW_SCAN_DIRS:
        base = root / rel
        if not base.exists():
            continue
        for p in list(base.rglob("*.c")) + list(base.rglob("*.h")):
            if any(part in FW_EXCLUDE_DIR_PARTS for part in p.parts):
                continue
            files.append(p)
    return files


def check_fw_file(path: Path) -> list[str]:
    if path.name in FW_DEFINITION_FILES or path.name in FW_OWNER_FILES:
        return []
    raw = path.read_text(encoding="utf-8")
    stripped = _strip_c_comments(raw)
    stripped_lines = stripped.splitlines()
    raw_lines = raw.splitlines()
    violations: list[str] = []
    for i, line in enumerate(stripped_lines):
        m = FW_CALL_RE.search(line)
        if not m:
            continue
        fn = m.group(1)
        source_line = raw_lines[i].strip() if i < len(raw_lines) else line.strip()
        if fn == "kiln_io_all_relays_off":
            enclosing = _enclosing_function(stripped_lines, i)
            if (path.name, enclosing) in FW_ALL_OFF_ALLOWLIST:
                continue
            violations.append(
                f"{path}:{i + 1}: direct kiln_io_all_relays_off() call outside "
                f"the allowlisted fail-safe paths (in "
                f"{enclosing or '<unknown function>'}()) -- route through "
                f"kiln_io_owner instead, or add a justified allowlist entry "
                f"if this really is a wedged-owner-task fail-safe: {source_line}"
            )
        else:
            violations.append(
                f"{path}:{i + 1}: direct {fn}() call bypasses kiln_io_owner -- "
                f"use kiln_io_owner_command_set_relay()/set_relay_mask()/"
                f"set_relay_mask_authorized() so the ownership/safety-fault gate "
                f"and the SX1509 read-modify-write serialization actually apply: "
                f"{source_line}"
            )
    return violations


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
    for path in find_fw_c_files(root):
        violations.extend(check_fw_file(path))

    if violations:
        print("check_relay_authority_paths: relay-write bypass(es) found:")
        for v in violations:
            print(f"  {v}")
        return 1

    print("check_relay_authority_paths: OK -- every PC-side relay-write call "
          "site goes through IoClient's refusal-aware wrapper, and every "
          "firmware-side relay write goes through kiln_io_owner or a "
          "documented fail-safe allowlist entry.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
