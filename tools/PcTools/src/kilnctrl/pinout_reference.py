"""Static, schematic-derived pinout/interface reference for the GUI's
"Full Board Pinout (reference)..." popup (see gui.py's About menu).

This is deliberately the opposite of pin_overlay.py / the "Pin Configuration"
popup it feeds: that popup only ever shows the subset of ESP32-S3 GPIOs the
*connected device firmware* reports live over INFO_CMD_GET_PIN_CONFIG, and
says so explicitly ("not from a static table"). SX1509 expander pins, the
thermocouple/safety daughterboards, the display, and the RP2040 safety
processor never appear there because nothing currently drives them over the
wire. This module exists to cover exactly that gap: everything the *schematic*
defines, present or not-yet-populated, with nothing needed live -- no device,
no UART link, no KiCad runtime.

Source of truth is ``docs/HARDWARE.md``, which this repo's own header calls
"the authority" above even the firmware's own header comments (see
App/drivers/owners/kiln_io.h). We parse it rather than duplicate it by hand, so the
GUI page can never drift from the doc the way a hand-copied table would.

Parser approach: a small, bounded, line-based state machine -- no markdown
library dependency, matching this codebase's general preference for
hand-rolled parsers over pulling in a dependency for one file with a known,
stable structure (see App/drivers/common/http_form.h for the same spirit in C). The
parser only understands the two constructs HARDWARE.md actually uses:
``##``-level section headings and GFM pipe tables. Anything else in a
section (paragraphs, numbered lists) is kept as prose text, in reading order,
interleaved with any tables in that section.

If the file is missing, unreadable, or a section's table looks malformed, the
loader does not raise -- it returns whatever did parse and records what
didn't (see :func:`get_parse_error`), the same defensive spirit as the rest
of the GUI: a bad doc must never crash a Tk callback, it must degrade to
"here's what we could read."
"""

from __future__ import annotations

import logging
import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

logger = logging.getLogger(__name__)

# tools/PcTools/src/kilnctrl/pinout_reference.py -> parents[0]=kilnctrl,
# [1]=src, [2]=PcTools, [3]=tools, [4]=repo root.
# These tools moved out of KilnFW/ because they serve both processors, so the
# main firmware's docs are now reached explicitly rather than by walking up to
# the parent project. Verified against the actual directory layout, not assumed.
_REPO_ROOT = Path(__file__).resolve().parents[4]
HARDWARE_MD_PATH = _REPO_ROOT / "firmware" / "KilnFW" / "docs" / "HARDWARE.md"

# -- status classification ---------------------------------------------------
#
# Three buckets, matching what someone building out the rest of the board
# actually needs to know: is this on the bench today, drawn but not yet
# populated, or not even this firmware's concern.
#
# Per TODO.md's opening note (2026-08-10 planning doc): "Only the bare
# ESP32-S3 board is wired up so far (no thermocouple daughterboard, relays,
# display, or safety RP2040)." Read literally that would also demote the
# SX1509/relay section, but the task this module was built for is explicit
# that ESP32-S3 pin assignments, SPI bus, I2C bus, SX1509 expander, and Power
# all count as "built and wired" -- these are all main-board-resident
# circuits documented in HARDWARE.md with no daughterboard/panel dependency,
# as opposed to the three sections that explicitly *require* a separate,
# not-yet-populated board or panel to do anything (thermocouple daughterboard,
# safety thermocouple board, display) or a barrier that only matters once the
# far side of it exists (isolation barrier). RP2040 gets its own bucket
# because HARDWARE.md says outright, in its own heading, "for reference" /
# "Not driven by this firmware" -- the clearest "future" marker in the doc.
STATUS_BUILT = "built_and_wired"
STATUS_DESIGNED = "designed_not_assembled"
STATUS_FUTURE = "future_not_driven"

STATUS_LABELS = {
    STATUS_BUILT: "Built and wired",
    STATUS_DESIGNED: "Designed, not yet assembled",
    STATUS_FUTURE: "Future — not driven by this firmware",
}

#: Section title (exact ## heading text in HARDWARE.md, re-checked against
#: the file rather than guessed -- see the `python3 -c` heading dump this
#: module's author ran) -> (status, one-line reason shown in the GUI).
#: A heading that shows up in the doc but not here falls back to
#: STATUS_DESIGNED (see _classify) -- the conservative choice per the task
#: this module was built for, rather than silently defaulting to "built."
_SECTION_STATUS: dict[str, tuple[str, str]] = {
    "ESP32-S3 pin assignments": (
        STATUS_BUILT,
        "Main-board-resident GPIO map for U4, the ESP32-S3-DevKitC that is "
        "the one thing currently wired up per TODO.md.",
    ),
    "SPI bus": (
        STATUS_BUILT,
        "Shared bus on the ESP32-S3's own pins; no daughterboard needed for "
        "the bus itself to exist.",
    ),
    "I2C bus": (
        STATUS_BUILT,
        "Shared bus on the ESP32-S3's own pins; the only device that answers "
        "on it today is the on-main-board SX1509.",
    ),
    "SX1509 I/O expander (U5, 0x3E)": (
        STATUS_BUILT,
        "Main-board-resident expander (U5); relay/digital-IO pins are drawn "
        "here even where the relay or connector they drive is board-only.",
    ),
    "Thermocouple daughterboard (J6 -> J5)": (
        STATUS_DESIGNED,
        "Requires the separate thermocouple daughterboard on J6/J5, which "
        "TODO.md's opening note says is not yet populated.",
    ),
    "Safety thermocouple board (J7 -> J1)": (
        STATUS_DESIGNED,
        "Requires the separate safety thermocouple board on J7/J1, not yet "
        "populated, and the RP2040 safety processor it feeds.",
    ),
    "Isolation barrier (main <-> safety)": (
        STATUS_DESIGNED,
        "The optocouplers are main-board parts, but the barrier only does "
        "anything once the safety-side board/processor on the far side "
        "exists -- not yet populated per TODO.md.",
    ),
    "Safety processor (A1, RP2040) — for reference": (
        STATUS_FUTURE,
        "HARDWARE.md's own words: \"Not driven by this firmware.\" A separate, "
        "not-yet-started firmware project (see docs/PROJECT_STATUS.md).",
    ),
    "Display (J2)": (
        STATUS_DESIGNED,
        "Requires the ILI9488 panel on J2, not yet populated per TODO.md's "
        "opening note.",
    ),
    "Power": (
        STATUS_BUILT,
        "Main-board input protection/regulation; exists independent of which "
        "daughterboards are populated.",
    ),
}


# -- data model ---------------------------------------------------------------


@dataclass(frozen=True)
class PinoutRow:
    """One row of a parsed GFM pipe table. Cells are kept generic (a tuple of
    strings, same order as the section's header) rather than fixed
    pin/net/direction/function fields, because HARDWARE.md's tables don't all
    share one shape -- e.g. the safety thermocouple board's table is a
    two-pins-per-row layout, not the four-column ESP32 pin table."""

    cells: tuple[str, ...]


@dataclass
class PinoutSection:
    """One ``##`` section of HARDWARE.md."""

    title: str
    status: str
    status_note: str
    #: Column headers for `rows`, empty if the section has no table (e.g.
    #: "SPI bus", "I2C bus", the RP2040 paragraph).
    header: tuple[str, ...] = ()
    rows: list[PinoutRow] = field(default_factory=list)
    #: Free-form paragraphs/list items, in document order, interleaved
    #: conceptually with `rows` but kept separate since the GUI renders them
    #: in different widgets (Treeview vs. a wrapped Label).
    prose: list[str] = field(default_factory=list)


# -- parsing --------------------------------------------------------------

_HEADING_RE = re.compile(r"^##\s+(.+?)\s*$")
_LIST_ITEM_RE = re.compile(r"^(\d+\.\s|-\s)")
_SEP_CELL_RE = re.compile(r"^:?-+:?$")


def _split_table_row(line: str) -> tuple[str, ...]:
    """Split a GFM pipe-table row into stripped cells, dropping the
    leading/trailing empty cell produced by the row's outer ``|`` chars.
    Interior empty cells (HARDWARE.md's safety-thermocouple-board table uses
    a blank spacer column between its two pin/signal pairs) are kept."""
    parts = line.strip().split("|")
    if parts and parts[0].strip() == "":
        parts = parts[1:]
    if parts and parts[-1].strip() == "":
        parts = parts[:-1]
    return tuple(p.strip() for p in parts)


def _is_separator_row(cells: tuple[str, ...]) -> bool:
    return bool(cells) and all(_SEP_CELL_RE.match(c) for c in cells)


def _classify(title: str) -> tuple[str, str]:
    entry = _SECTION_STATUS.get(title)
    if entry is not None:
        return entry
    # Unknown heading (HARDWARE.md changed under us): don't guess "built,"
    # since a wrong "built and wired" is the more actively misleading
    # failure mode for someone about to trust it on the bench. Conservative
    # default per this module's own design brief.
    logger.warning(
        "pinout_reference: unrecognized HARDWARE.md heading %r, defaulting "
        "to %r -- update _SECTION_STATUS",
        title,
        STATUS_DESIGNED,
    )
    return STATUS_DESIGNED, "Unrecognized section heading; status not confirmed."


def _parse_section_body(lines: list[str]) -> tuple[tuple[str, ...], list[PinoutRow], list[str]]:
    """Parse one section's body lines into (table header, table rows, prose
    paragraphs). Tolerant: a section may have zero or one table (every
    section in HARDWARE.md today has at most one) plus any number of prose
    paragraphs before/after/around it. If a second table block appears, its
    rows are appended to the first table's rows (best-effort; HARDWARE.md
    doesn't currently do this, so this is just not-crashing insurance)."""
    header: tuple[str, ...] = ()
    rows: list[PinoutRow] = []
    prose: list[str] = []

    buf: list[str] = []

    def flush_buf() -> None:
        if buf:
            prose.append(" ".join(s.strip() for s in buf).strip())
            buf.clear()

    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        stripped = line.strip()

        if not stripped:
            flush_buf()
            i += 1
            continue

        if stripped.startswith("|"):
            flush_buf()
            table_lines = []
            while i < n and lines[i].strip().startswith("|"):
                table_lines.append(lines[i].strip())
                i += 1
            if table_lines:
                first_cells = _split_table_row(table_lines[0])
                body_lines = table_lines[1:]
                # Second line is normally the `---|---` separator; skip it if so.
                if body_lines and _is_separator_row(_split_table_row(body_lines[0])):
                    body_lines = body_lines[1:]
                if not header:
                    header = first_cells
                else:
                    rows.append(PinoutRow(cells=first_cells))
                for row_line in body_lines:
                    rows.append(PinoutRow(cells=_split_table_row(row_line)))
            continue

        if _LIST_ITEM_RE.match(stripped):
            flush_buf()
            buf.append(stripped)
            i += 1
            # Swallow wrapped continuation lines (no marker, not blank, not a
            # table) into this same list item, same as an ordinary paragraph.
            while i < n and lines[i].strip() and not lines[i].strip().startswith("|") \
                    and not _LIST_ITEM_RE.match(lines[i].strip()):
                buf.append(lines[i].strip())
                i += 1
            flush_buf()
            continue

        buf.append(stripped)
        i += 1

    flush_buf()
    return header, rows, prose


def parse_hardware_md(path: Path = HARDWARE_MD_PATH) -> list[PinoutSection]:
    """Parse ``docs/HARDWARE.md`` into an ordered list of :class:`PinoutSection`.

    Never raises -- a missing file or a parse hiccup on one section logs a
    warning and yields whatever else did parse, same as the rest of this
    codebase never lets a doc/data problem raise into a Tk callback.
    """
    try:
        text = path.read_text(encoding="utf-8")
    except OSError as exc:
        logger.warning("pinout_reference: could not read %s: %s", path, exc)
        return []

    lines = text.splitlines()

    # Find every "## " heading and the line range of its body (up to the
    # next "## " heading or EOF). The level-1 "# KilnCtrl Hardware Map"
    # title and its intro paragraph are not a "##" section and are skipped,
    # per the task brief (turn the ##-level sections into PinoutSections).
    heading_indices: list[tuple[int, str]] = []
    for idx, line in enumerate(lines):
        match = _HEADING_RE.match(line)
        if match:
            heading_indices.append((idx, match.group(1)))

    sections: list[PinoutSection] = []
    for pos, (idx, title) in enumerate(heading_indices):
        end = heading_indices[pos + 1][0] if pos + 1 < len(heading_indices) else len(lines)
        body = lines[idx + 1 : end]
        try:
            header, rows, prose = _parse_section_body(body)
        except Exception as exc:  # pragma: no cover - defensive, see module docstring
            logger.warning("pinout_reference: failed to parse section %r: %s", title, exc)
            header, rows, prose = (), [], [f"(failed to parse this section: {exc})"]

        status, note = _classify(title)
        sections.append(
            PinoutSection(
                title=title,
                status=status,
                status_note=note,
                header=header,
                rows=rows,
                prose=prose,
            )
        )

    return sections


# -- cache --------------------------------------------------------------

#: Parsed once and reused -- this is static reference data derived from a
#: doc that doesn't change at runtime, so there is no reason to re-parse the
#: file every time the GUI popup opens. Call load_sections(force=True) if a
#: caller genuinely wants to pick up an edited HARDWARE.md without restarting.
_cache: Optional[list[PinoutSection]] = None
_parse_error: Optional[str] = None


def load_sections(force: bool = False) -> list[PinoutSection]:
    """Return the cached, parsed section list, parsing on first call."""
    global _cache, _parse_error
    if _cache is None or force:
        if not HARDWARE_MD_PATH.exists():
            _parse_error = f"docs/HARDWARE.md not found at {HARDWARE_MD_PATH}"
            logger.warning("pinout_reference: %s", _parse_error)
            _cache = []
        else:
            _parse_error = None
            _cache = parse_hardware_md(HARDWARE_MD_PATH)
            if not _cache:
                _parse_error = f"docs/HARDWARE.md at {HARDWARE_MD_PATH} produced no sections"
    return _cache


def get_parse_error() -> Optional[str]:
    """Set if the last :func:`load_sections` call found nothing to show --
    the GUI uses this to render a clear message instead of a blank popup."""
    return _parse_error
