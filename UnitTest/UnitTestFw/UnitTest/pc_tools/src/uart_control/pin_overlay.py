"""Overlay geometry for the About window's live pinout diagram.

``assets/pinout.png`` is an 800x800 two-column pin-list diagram for the
ESP32-S3 dev board (colored pin-label badges, not a bare photo). This module
holds the mapping from GPIO number to the badge's position in that image, and
draws the highlight/callout for whatever pins the device reports over
INFO_CMD_GET_PIN_CONFIG.

The coordinates in :data:`PIN_POSITIONS` were **measured against the actual
image**, not computed from a pitch. Do not "improve" or re-derive them without
re-measuring against the PNG -- there is no way to verify a guess from code.

Only the eight pins this firmware wires up (see ``s_pin_config[]`` in
uart_bridge.c, itself built from App/drivers/settings.h) have entries. A
reported GPIO with no entry is not a bug to crash on: :func:`draw_overlay`
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

#: GPIO number -> (column, measured badge center Y in image pixels).
PIN_POSITIONS: dict[int, tuple[str, int]] = {
    4: ("left", 207),    # AD9833_SCLK_IO
    5: ("left", 237),    # AD9833_MOSI_IO
    6: ("left", 268),    # AD9833_CS_IO
    18: ("left", 420),   # HEARTBEAT_LED_GPIO
    8: ("left", 451),    # I2C_MASTER_SDA_IO
    9: ("left", 545),    # I2C_MASTER_SCL_IO
    43: ("right", 146),  # UART_OWNER_TX_IO (silkscreened "TX")
    44: ("right", 176),  # UART_OWNER_RX_IO (silkscreened "RX")
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
