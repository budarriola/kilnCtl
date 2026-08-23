"""Overlay geometry for the About window's live pinout diagram.

``assets/pinout.png`` is an 800x800 two-column pin-list diagram for the
ESP32-S3 dev board (colored pin-label badges, not a bare photo). This module
holds the mapping from GPIO number to the badge's position in that image, and
draws the highlight/callout for whatever pins the device reports over
INFO_CMD_GET_PIN_CONFIG.

The diagram is a stock **ESP32-S3-DevKitC-1** pin list, which is exactly the
module (U4) the kilnCtl main board carries -- so the artwork itself did not
have to change when the board did. What changed is which badges get
highlighted: this board wires up nineteen GPIOs where the unit-test fixture
used eight.

Provenance of :data:`PIN_POSITIONS`, because it is now mixed and that matters:
eight entries (GPIO 4, 5, 6, 8, 9, 18, 43, 44) were **measured against the
actual image**. The rest were derived from those anchors using the badge row
pitch, which is uniform down both columns: ``y = 115 + 30.7 * row``, where row
0 is the topmost badge in that column (3.3V on the left, GND on the right).
That formula reproduces all eight measured values to within ~2 px, which is
what makes it trustworthy here and is the only reason deriving was acceptable
at all. Do not extend this table by guessing -- either re-measure against the
PNG, or derive from the anchors and check the result against every measured
value first.

A reported GPIO with no entry is not a bug to crash on: :func:`draw_overlay`
returns it so the caller can list it as text instead.
"""

from __future__ import annotations

from typing import Iterable, Sequence

from .devices import PinConfigEntry

#: assets/pinout.png is square; both dimensions.
IMAGE_SIZE = 800

#: Horizontal extent of the existing colored pin-label badges, per column.
#: The highlight rectangle is drawn over these; the leader line runs from the
#: badge's outer edge toward the nearer image margin.
BADGE_X_SPAN: dict[str, tuple[int, int]] = {
    "left": (108, 240),
    "right": (565, 690),
}

#: Height of one badge, centered on the pin's measured Y.
BADGE_HEIGHT = 30

#: Badge row pitch and origin, used to derive the non-measured entries below.
#: Row 0 is the topmost badge of a column (3.3V left / GND right).
BADGE_ROW_ORIGIN_Y = 115.0
BADGE_ROW_PITCH = 30.7


def _row_y(row: int) -> int:
    """Badge center Y for row ``row`` of either column (see the module docstring)."""
    return round(BADGE_ROW_ORIGIN_Y + BADGE_ROW_PITCH * row)


#: GPIO number -> (column, badge center Y in image pixels). "measured" marks
#: the eight anchors read off the PNG by hand; the rest are _row_y() values
#: validated against those anchors.
PIN_POSITIONS: dict[int, tuple[str, int]] = {
    # -- left column ----------------------------------------------------
    4: ("left", 207),        # measured -- DataFromSafty (safety link RX)
    5: ("left", 237),        # measured -- DataToSafty   (safety link TX)
    6: ("left", 268),        # measured -- Fault (isolated fault OUT)
    7: ("left", _row_y(6)),  # IO_Expander_IRQ
    17: ("left", _row_y(9)),  # CS1
    18: ("left", 420),       # measured -- CS2 (was the fixture's heartbeat LED)
    8: ("left", 451),        # measured -- SDA
    9: ("left", 545),        # measured -- SCL
    10: ("left", _row_y(15)),  # IO_Expander_RST
    11: ("left", _row_y(16)),  # MOSI
    12: ("left", _row_y(17)),  # CLK
    13: ("left", _row_y(18)),  # MISO
    14: ("left", _row_y(19)),  # CS0
    # -- right column ---------------------------------------------------
    43: ("right", 146),      # measured -- UART0 TX (silkscreened "TX")
    44: ("right", 176),      # measured -- UART0 RX (silkscreened "RX")
    38: ("right", _row_y(9)),   # thermoFault_0
    48: ("right", _row_y(15)),  # thermoFault_2
    47: ("right", _row_y(16)),  # thermoFault_1
    21: ("right", _row_y(17)),  # CS3 (display)
}

#: Accent for the highlight + callout. Deliberately not red or green: the
#: image's own badges already use those, so a highlight in either colour
#: would disappear into the artwork.
ACCENT = "#00e5ff"  # bright cyan
ACCENT_OUTLINE_WIDTH = 3

#: How far the leader line runs from the badge edge toward the margin, and how
#: far past its end the text label starts.
LEADER_LENGTH = 56
LABEL_GAP = 4


def draw_overlay(
    canvas,
    entries: Iterable[PinConfigEntry],
    scale: float = 1.0,
    accent: str = ACCENT,
    tag: str = "overlay",
) -> "list[PinConfigEntry]":
    """Draw highlights + callouts for ``entries`` on ``canvas``.

    ``canvas`` must already show the pinout image with its top-left at (0, 0)
    and the same ``scale`` applied. Every coordinate here is in image pixels
    and multiplied by ``scale``, so the two stay consistent whatever scale the
    caller picked.

    All items are tagged ``tag`` so a refresh can ``canvas.delete(tag)``
    without disturbing the image itself.

    Returns the entries that had no :data:`PIN_POSITIONS` mapping and could
    therefore not be placed -- the caller should surface those as text.
    """
    unplaced: list[PinConfigEntry] = []

    for entry in entries:
        position = PIN_POSITIONS.get(entry.gpio)
        if position is None:
            unplaced.append(entry)
            continue
        column, center_y = position
        x0, x1 = BADGE_X_SPAN[column]
        y0 = center_y - BADGE_HEIGHT / 2
        y1 = center_y + BADGE_HEIGHT / 2

        canvas.create_rectangle(
            x0 * scale,
            y0 * scale,
            x1 * scale,
            y1 * scale,
            outline=accent,
            width=ACCENT_OUTLINE_WIDTH,
            tags=tag,
        )

        # Leader line out toward the nearer margin, clamped to the image.
        if column == "left":
            start_x = x0
            end_x = max(2, start_x - LEADER_LENGTH)
            text_x = end_x - LABEL_GAP
            anchor = "e"
        else:
            start_x = x1
            end_x = min(IMAGE_SIZE - 2, start_x + LEADER_LENGTH)
            text_x = end_x + LABEL_GAP
            anchor = "w"

        canvas.create_line(
            start_x * scale,
            center_y * scale,
            end_x * scale,
            center_y * scale,
            fill=accent,
            width=ACCENT_OUTLINE_WIDTH,
            tags=tag,
        )
        canvas.create_text(
            text_x * scale,
            center_y * scale,
            text=entry.abbrev,
            fill=accent,
            anchor=anchor,
            font=("Segoe UI", max(7, int(10 * scale)), "bold"),
            tags=tag,
        )

    return unplaced


def unmapped_gpios(entries: Sequence[PinConfigEntry]) -> "list[int]":
    """GPIOs in ``entries`` that :data:`PIN_POSITIONS` cannot place."""
    return [e.gpio for e in entries if e.gpio not in PIN_POSITIONS]
