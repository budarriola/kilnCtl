"""Read the board's own Wi-Fi AP credentials off a bench-camera LCD capture.

Used by ``tools/PcTools/scripts/join_board_ap.ps1``. Two screens are handled:

* ``recovery`` -- the KilnFW recovery image's status screen
  (``firmware/KilnFW_recovery/main/recovery_lcd.c``). The screen draws a
  ``WIFI:T:WPA;S:<ssid>;P:<pass>;;`` QR code at the top right
  (``recovery_wifi_qr.c``); it is decoded first (``zxing-cpp``) and its
  passphrase must be 12 characters of the alphabet below. If the QR is
  missing or undecodable (older recovery image, glare, no network yet) the
  read falls back to glyph segmentation, with a warning. Every line is drawn
  left-aligned from panel x=0 in the 5x7 font of ``recovery_text.c``; the
  SSID is at y=144 (scale 3, or scale 2 above 26 chars) and the 12-character
  passphrase at y=188, scale 6, drawn from the alphabet
  ``23456789ABCDEFGHJKLMNPQRSTUVWXYZ``. The text is read by glyph
  segmentation: the panel is found in the frame and rectified, each text band
  is thresholded at median+30, glyphs are split by column runs (an over-wide
  run is split by the character pitch), and each glyph's 5x7 grid is matched
  against the font. Nothing is guessed: a glyph that does not segment fails
  the read.
* ``normal`` -- the application's Network page AP-fallback card
  (``firmware/KilnFW/App/drivers/ui/ui_page_network.c``). The password there
  is user-set from an arbitrary charset and drawn in an antialiased font, so
  it is never OCR'd; the card's ``WIFI:T:WPA;S:<ssid>;P:<pass>;;`` QR code is
  decoded instead, with no fallback. The QR and the password are
  only shown with an ADMIN LCD session, or with LCD auth off.

The passphrase never goes to stdout, stderr or a log. stdout carries one JSON
object with the SSID, glyph/candidate counts and warnings only; the
passphrase candidates go to the file named by ``--candidates-out`` (one per
line, most likely first), which the caller must delete.
"""

from __future__ import annotations

import argparse
import itertools
import json
import os
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple

import numpy as np

PANEL_W = 480
PANEL_H = 320

# recovery_lcd.c layout (top edge of each line, panel px).
Y_SSID = 144
Y_PWLBL = 172
Y_PASS = 188
Y_IP = 238
PASS_SCALE = 6
PASS_LEN = 12
PASS_ALPHABET = "23456789ABCDEFGHJKLMNPQRSTUVWXYZ"
FONT_W = 5
FONT_H = 7
FONT_ADV = 6

# Visually close pairs: both spellings are always offered as candidates.
AMBIGUOUS_PAIRS = (("5", "S"),)
MAX_CANDIDATES = 16

_FONT_SOURCE = Path("firmware") / "KilnFW_recovery" / "main" / "recovery_text.c"


class ReadError(RuntimeError):
    """The capture could not be read without guessing."""


# --------------------------------------------------------------------------
# Font
# --------------------------------------------------------------------------

def _repo_root() -> Path:
    here = Path(__file__).resolve()
    for parent in here.parents:
        if (parent / _FONT_SOURCE).is_file():
            return parent
    raise ReadError(f"cannot find {_FONT_SOURCE} above {here}")


def load_font(source: Optional[Path] = None) -> Dict[str, Tuple[int, ...]]:
    """Parse FONT5X7 out of recovery_text.c: char -> 5 column bytes, bit0 = top."""
    path = Path(source) if source else _repo_root() / _FONT_SOURCE
    text = path.read_text(encoding="utf-8")
    start = text.index("FONT5X7")
    rows = re.findall(r"\{\s*(0x[0-9A-Fa-f]{2}(?:\s*,\s*0x[0-9A-Fa-f]{2}){4})\s*\}", text[start:])
    if len(rows) != 0x7E - 0x20 + 1:
        raise ReadError(f"{path}: expected {0x7E - 0x20 + 1} glyphs in FONT5X7, found {len(rows)}")
    font = {}
    for i, row in enumerate(rows):
        font[chr(0x20 + i)] = tuple(int(b, 16) for b in row.split(","))
    return font


def glyph_bitmap(cols: Sequence[int]) -> np.ndarray:
    """7x5 array of 0/1 for one font glyph."""
    bm = np.zeros((FONT_H, FONT_W), dtype=np.float64)
    for c, byte in enumerate(cols):
        for r in range(FONT_H):
            if byte & (1 << r):
                bm[r, c] = 1.0
    return bm


# --------------------------------------------------------------------------
# Panel detection and rectification
# --------------------------------------------------------------------------

def load_image(path: str) -> np.ndarray:
    from PIL import Image  # pillow is a declared PcTools dependency

    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"), dtype=np.float64)


def _component_from_centre(mask: np.ndarray) -> np.ndarray:
    """Connected component of ``mask`` containing (or nearest) the frame centre."""
    h, w = mask.shape
    cy, cx = h // 2, w // 2
    ys, xs = np.nonzero(mask)
    if len(ys) == 0:
        raise ReadError("no lit panel found in the frame (panel dark or camera moved)")
    k = int(np.argmin((ys - cy) ** 2 + (xs - cx) ** 2))
    seed = np.zeros_like(mask)
    seed[ys[k], xs[k]] = True
    comp = seed
    while True:
        grown = comp.copy()
        grown[1:, :] |= comp[:-1, :]
        grown[:-1, :] |= comp[1:, :]
        grown[:, 1:] |= comp[:, :-1]
        grown[:, :-1] |= comp[:, 1:]
        grown &= mask
        if grown.sum() == comp.sum():
            return comp
        comp = grown


def _robust_line(t: np.ndarray, s: np.ndarray) -> Tuple[float, float]:
    """Fit s = a*t + b, rejecting outliers (text touching the panel edge)."""
    keep = np.ones(len(t), dtype=bool)
    a = b = 0.0
    for _ in range(8):
        if keep.sum() < 10:
            raise ReadError("panel edge fit has too few points")
        a, b = np.polyfit(t[keep], s[keep], 1)
        r = s - (a * t + b)
        mad = np.median(np.abs(r[keep])) + 0.5
        new = np.abs(r) <= max(2.0, 3.0 * mad)
        if np.array_equal(new, keep):
            break
        keep = new
    return float(a), float(b)


# Blue-over-red margins tried in turn for the lit panel. The camera's white
# balance drifts between sessions: one frame's panel read b-r ~ 45, a later
# one ~ 23. A lower margin is only accepted while the component stays clear
# of the frame border; below that it bleeds into the bezel and background.
PANEL_BLUE_MARGINS = (36, 16)


def detect_panel(img: np.ndarray, step: int = 2) -> np.ndarray:
    """Panel corners by the first margin in PANEL_BLUE_MARGINS that works."""
    err: Optional[ReadError] = None
    for margin in PANEL_BLUE_MARGINS:
        try:
            return _detect_panel_at(img, step, margin)
        except ReadError as e:
            err = e
    assert err is not None
    raise err


def _detect_panel_at(img: np.ndarray, step: int, margin: float) -> np.ndarray:
    """Panel corners in the frame, TL, TR, BL, BR, as a 4x2 array (x, y).

    The lit panel's black reads teal through this camera (G and B both well
    above R), while the unlit bezel around it is a near-neutral dark grey
    that can still read faintly blue; the panel is the teal connected
    component around the frame centre. Each edge is a
    robust line through that component's outermost pixels, so glyphs drawn
    at the panel's edge (every recovery line starts at x=0) are rejected as
    outliers rather than bending the edge.
    """
    sub = img[::step, ::step]
    r, g, b = sub[..., 0], sub[..., 1], sub[..., 2]
    mask = ((g - r) > 0) & ((b - r) > margin)
    comp = _component_from_centre(mask)
    rows = np.nonzero(comp.any(axis=1))[0]
    cols = np.nonzero(comp.any(axis=0))[0]
    if len(rows) < 40 or len(cols) < 40:
        raise ReadError("lit panel region too small (panel dark or camera moved)")
    h, w = comp.shape
    if rows[0] == 0 or cols[0] == 0 or rows[-1] == h - 1 or cols[-1] == w - 1:
        raise ReadError("lit panel region touches the frame edge (not the panel alone)")

    def middle(idx: np.ndarray) -> np.ndarray:
        n = len(idx)
        return idx[int(n * 0.12): int(n * 0.88)]

    ry = middle(rows)
    left = np.array([np.nonzero(comp[y])[0][0] for y in ry], dtype=np.float64)
    right = np.array([np.nonzero(comp[y])[0][-1] + 1 for y in ry], dtype=np.float64)
    cx = middle(cols)
    top = np.array([np.nonzero(comp[:, x])[0][0] for x in cx], dtype=np.float64)
    bottom = np.array([np.nonzero(comp[:, x])[0][-1] + 1 for x in cx], dtype=np.float64)
    ry = ry.astype(np.float64)
    cx = cx.astype(np.float64)
    la, lb = _robust_line(ry, left)
    ra, rb = _robust_line(ry, right)
    ta, tb = _robust_line(cx, top)
    ba, bb = _robust_line(cx, bottom)

    def cross(xa: float, xb: float, ya: float, yb: float) -> Tuple[float, float]:
        # x = xa*y + xb, y = ya*x + yb
        y = (ya * xb + yb) / (1.0 - ya * xa)
        return xa * y + xb, y

    corners = [cross(la, lb, ta, tb), cross(ra, rb, ta, tb),
               cross(la, lb, ba, bb), cross(ra, rb, ba, bb)]
    return np.array(corners, dtype=np.float64) * step


def homography(src: np.ndarray, dst: np.ndarray) -> np.ndarray:
    """3x3 H with dst ~ H @ src for four point pairs."""
    m = []
    v = []
    for (x, y), (u, w) in zip(src, dst):
        m.append([x, y, 1, 0, 0, 0, -u * x, -u * y])
        v.append(u)
        m.append([0, 0, 0, x, y, 1, -w * x, -w * y])
        v.append(w)
    h = np.linalg.solve(np.array(m, dtype=np.float64), np.array(v, dtype=np.float64))
    return np.append(h, 1.0).reshape(3, 3)


PANEL_CORNERS = np.array([[0, 0], [PANEL_W, 0], [0, PANEL_H], [PANEL_W, PANEL_H]], dtype=np.float64)


def to_grey(img: np.ndarray) -> np.ndarray:
    return img @ np.array([0.299, 0.587, 0.114])


def rectify(img: np.ndarray, corners: np.ndarray, scale: int = 2,
            rows: Optional[np.ndarray] = None) -> np.ndarray:
    """Panel image, ``PANEL_H*scale`` x ``PANEL_W*scale`` grey, bilinear-sampled.

    ``img`` is RGB or an already-grey 2-D array (from :func:`to_grey`).
    ``rows`` optionally limits the output to those output row indices.
    """
    h = homography(PANEL_CORNERS, corners)
    grey = img if img.ndim == 2 else to_grey(img)
    vr = np.arange(PANEL_H * scale) if rows is None else np.asarray(rows)
    v = ((vr.astype(np.float64) + 0.5) / scale)[:, None]
    u = ((np.arange(PANEL_W * scale, dtype=np.float64) + 0.5) / scale)[None, :]
    den = h[2, 0] * u + h[2, 1] * v + h[2, 2]
    fx = (h[0, 0] * u + h[0, 1] * v + h[0, 2]) / den - 0.5
    fy = (h[1, 0] * u + h[1, 1] * v + h[1, 2]) / den - 0.5
    fh, fw = grey.shape
    fx = np.clip(fx, 0, fw - 1.001)
    fy = np.clip(fy, 0, fh - 1.001)
    x0 = np.floor(fx).astype(int)
    y0 = np.floor(fy).astype(int)
    ax = fx - x0
    ay = fy - y0
    return ((grey[y0, x0] * (1 - ax) + grey[y0, x0 + 1] * ax) * (1 - ay)
            + (grey[y0 + 1, x0] * (1 - ax) + grey[y0 + 1, x0 + 1] * ax) * ay)


# Static recovery-screen strings drawn whenever the network is up
# (recovery_lcd.c, the s_net_ready branch): (y, scale, text). Each line
# starts at panel x=0.
ANCHOR_LINES = (
    (4, 3, "RECOVERY MODE"),
    (30, 2, "KilnFW recovery image"),
    (126, 2, "Join Wi-Fi network:"),
    (172, 2, "Password:"),
    (290, 2, "New password each boot"),
)


def render_template(font: Dict[str, Tuple[int, ...]],
                    lines: Sequence[Tuple[int, int, str]] = ANCHOR_LINES) -> np.ndarray:
    """PANEL_H x PANEL_W float mask, 1 where the static strings draw ink."""
    out = np.zeros((PANEL_H, PANEL_W), dtype=np.float64)
    for y, s, text in lines:
        for i, ch in enumerate(text):
            cols = font.get(ch)
            if cols is None:
                continue
            bm = np.kron(glyph_bitmap(cols), np.ones((s, s)))
            x = i * FONT_ADV * s
            h = min(bm.shape[0], PANEL_H - y)
            w = min(bm.shape[1], PANEL_W - x)
            if h > 0 and w > 0:
                out[y:y + h, x:x + w] = np.maximum(out[y:y + h, x:x + w], bm[:h, :w])
    return out


def _box_blur(a: np.ndarray, r: int) -> np.ndarray:
    if r <= 0:
        return a
    # Running-sum box filter, zero padded (same as np.convolve mode="same").
    n = 2 * r + 1
    for axis in (0, 1):
        pad = [(0, 0), (0, 0)]
        pad[axis] = (r + 1, r)
        c = np.cumsum(np.pad(a, pad), axis=axis)
        hi = np.take(c, np.arange(n, c.shape[axis]), axis=axis)
        lo = np.take(c, np.arange(0, c.shape[axis] - n), axis=axis)
        a = (hi - lo) / n
    return a


def _local_contrast(panel: np.ndarray, r: int = 8) -> np.ndarray:
    """Grey minus its local mean, so uneven panel brightness drops out."""
    return panel - _box_blur(panel, r)


class _AnchorScorer:
    """Normalised correlation of a corner guess against the static text.

    Coarse scores use the anchor-line row bands only (fast); fine scores use
    the whole panel, whose ink-free rows also penalise a misfit. The
    template is blurred more for coarse steps so the search has a wide basin.
    """

    def __init__(self, img: np.ndarray, font: Dict[str, Tuple[int, ...]],
                 lines: Sequence[Tuple[int, int, str]] = ANCHOR_LINES):
        self.tmpl = render_template(font, lines)
        self.grey = img if img.ndim == 2 else to_grey(img)
        # Row bands around the anchor lines: blocks of rows that hold
        # template ink, padded past the blur and local-contrast radii, and
        # kept as separate blocks so a blur never mixes rows from two bands.
        pad = 16
        blocks: List[Tuple[int, int]] = []
        for r in np.flatnonzero(self.tmpl.any(axis=1)):
            lo, hi = max(0, r - pad), min(PANEL_H, r + pad + 1)
            if blocks and lo <= blocks[-1][1]:
                blocks[-1] = (blocks[-1][0], hi)
            else:
                blocks.append((lo, hi))
        self.rows = np.concatenate([np.arange(lo, hi) for lo, hi in blocks])
        self.splits = np.cumsum([hi - lo for lo, hi in blocks])[:-1]
        self._t: Dict[Tuple[int, bool], np.ndarray] = {}

    def _per_block(self, a: np.ndarray, fn) -> np.ndarray:
        return np.concatenate([fn(b) for b in np.split(a, self.splits)])

    def template(self, blur: int, banded: bool) -> np.ndarray:
        key = (blur, banded)
        if key not in self._t:
            if banded:
                t = self._per_block(self.tmpl[self.rows], lambda b: _box_blur(b, blur))
            else:
                t = _box_blur(self.tmpl, blur)
            self._t[key] = t - t.mean()
        return self._t[key]

    def score(self, cc: np.ndarray, blur: int, banded: bool,
              blur_panel: bool = False) -> float:
        t = self.template(blur, banded)
        if banded:
            p = self._per_block(rectify(self.grey, cc, 1, self.rows), _local_contrast)
        else:
            p = _local_contrast(rectify(self.grey, cc, 1))
        if blur_panel:
            # Blurring both sides widens the peak to the blur radius, which a
            # coarse grid needs; a sharp panel against a blurred template
            # scores near zero everywhere off the exact fit.
            p = self._per_block(p, lambda b: _box_blur(b, blur)) if banded else _box_blur(p, blur)
        p = p - p.mean()
        n = np.linalg.norm(p) * np.linalg.norm(t)
        return float((p * t).sum() / n) if n > 0 else -1.0


def refine_corners(img: np.ndarray, corners: np.ndarray,
                   font: Optional[Dict[str, Tuple[int, ...]]] = None,
                   steps: Sequence[float] = (16, 8, 4, 2, 1),
                   max_rounds: int = 6, full_below: float = 2,
                   scorer: Optional[_AnchorScorer] = None) -> Tuple[np.ndarray, float]:
    """Refine panel corners against the known static text; (corners, score).

    Coordinate search over the 8 corner coordinates maximising the
    normalised correlation between the rectified panel's local contrast and
    a rendering of the strings recovery_lcd.c always draws. The text is
    high contrast everywhere on the panel, unlike the panel's own colour, so
    this pins the geometry where colour segmentation cannot.
    """
    sc = scorer or _AnchorScorer(img, font or load_font())
    c = np.array(corners, dtype=np.float64).copy()
    best = -1.0
    for step in steps:
        banded = step > full_below
        blur = max(0, int(step // 4))
        best = sc.score(c, blur, banded)
        for _ in range(max_rounds):
            improved = False
            for k in range(8):
                for d in (-step, step):
                    trial = c.copy()
                    trial.flat[k] += d
                    s = sc.score(trial, blur, banded)
                    if s > best:
                        best, c, improved = s, trial, True
            if not improved:
                break
    return c, best


# Coarse placement grid for coarse_align: camera px shifts either side of the
# start and relative sizes about its centre. The camera or the board gets
# moved between sessions by tens of px, more than refine_corners' basin.
COARSE_SHIFT_PX = 192
COARSE_SHIFT_STEP = 24
COARSE_SCALES = (0.85, 0.92, 1.0, 1.08, 1.16)
COARSE_BLUR = 6


def coarse_align(img: np.ndarray, start: np.ndarray,
                 font: Optional[Dict[str, Tuple[int, ...]]] = None,
                 scorer: Optional[_AnchorScorer] = None) -> Tuple[np.ndarray, float]:
    """Best shifted/scaled copy of the start corners by a blurred whole-panel score."""
    sc = scorer or _AnchorScorer(img, font or load_font())
    start = np.asarray(start, dtype=np.float64)
    centre = start.mean(axis=0)
    best_c, best = start, sc.score(start, COARSE_BLUR, False, True)
    shifts = range(-COARSE_SHIFT_PX, COARSE_SHIFT_PX + 1, COARSE_SHIFT_STEP)
    for k in COARSE_SCALES:
        base = centre + (start - centre) * k
        for dy in shifts:
            for dx in shifts:
                trial = base + (dx, dy)
                s = sc.score(trial, COARSE_BLUR, False, True)
                if s > best:
                    best_c, best = trial, s
    return best_c, best


# --------------------------------------------------------------------------
# Glyph segmentation and matching
# --------------------------------------------------------------------------

@dataclass
class Glyph:
    x0: float  # panel px
    x1: float
    cells: np.ndarray  # 7x5 ink fraction per font pixel
    slot: int = 0  # character cell index from the left edge


@dataclass
class LineRead:
    glyphs: List[Glyph]
    ranked: List[List[Tuple[float, str]]] = field(default_factory=list)  # per glyph, (distance, char)


# Per-glyph vertical re-registration range, panel px either side of the band.
GLYPH_DY_PX = 4


def _layout_rows() -> List[Tuple[int, int]]:
    """(top, height) in panel px of every text line recovery_lcd.c draws."""
    rows = [(y, FONT_H * s) for y, s, _ in ANCHOR_LINES]
    rows.append((Y_SSID, FONT_H * 3))
    rows.append((Y_PASS, FONT_H * PASS_SCALE))
    return rows


def _band(panel: np.ndarray, scale: int, y_top: int, font_scale: int,
          search: int = 12) -> Tuple[np.ndarray, int, int]:
    """Binary ink of one text band, padded, with the pad and the top row.

    The band is located by row projection within ``search`` px of the layout
    position, so a few px of corner-fit error does not shift the grid.
    Returns (ink, pad, top): ``ink`` holds up to ``pad`` rows (rectified px)
    above and below the band so single glyphs can be re-registered
    vertically, and ``top`` is the band's top row in panel px.
    """
    hgt = FONT_H * font_scale
    lo = max(0, (y_top - search) * scale)
    hi = min(panel.shape[0], (y_top + hgt + search) * scale)
    region = panel[lo:hi]
    # Panel brightness drifts by tens of grey levels across one line through
    # this camera, so flatten it first: each column's background is a low
    # percentile of the band (the search margin rows are always background),
    # smoothed across columns. Then threshold at median+30, raised to half
    # the ink peak when the camera blooms the strokes.
    bg = np.percentile(region, 10, axis=0)
    k = np.ones(9) / 9.0
    bg = np.convolve(np.pad(bg, 4, mode="edge"), k, mode="valid")
    region = region - bg[None, :]
    thr = max(np.median(region) + 30.0, 0.5 * np.percentile(region, 99))
    ink = region > thr
    # The search margin reaches into the neighbouring layout lines (the SSID
    # sits 4 px under "Join Wi-Fi network:"), and a short SSID has less ink
    # than they do, so the projection would lock onto them. Blank the rows of
    # every other layout line first.
    rows_y = (lo + np.arange(len(ink))) / scale
    for oy, oh in _layout_rows():
        if oy < y_top + hgt and y_top < oy + oh:
            continue
        ink[(rows_y >= oy) & (rows_y < oy + oh)] = False
    prof = ink.sum(axis=1).astype(np.float64)
    win = hgt * scale
    if len(prof) < win:
        raise ReadError("text band outside the rectified panel")
    sums = np.convolve(prof, np.ones(win), mode="valid")
    best = int(np.argmax(sums))
    if sums[best] <= 0:
        raise ReadError(f"no text found at panel y={y_top}")
    top = lo + best
    pad = min(GLYPH_DY_PX * scale, best, len(prof) - win - best)
    return ink[best - pad: best + win + pad], pad, top // scale


def _runs(cols: np.ndarray) -> List[Tuple[int, int]]:
    runs = []
    start = None
    for i, on in enumerate(cols):
        if on and start is None:
            start = i
        elif not on and start is not None:
            runs.append((start, i))
            start = None
    if start is not None:
        runs.append((start, len(cols)))
    return runs


def segment_line(ink: np.ndarray, scale: int, font_scale: int,
                 min_ink: int = 1) -> List[Tuple[float, float]]:
    """Glyph boxes (panel px) from column runs; over-wide runs split by pitch."""
    pitch = FONT_ADV * font_scale * scale
    glyph_w = FONT_W * font_scale * scale
    colsum = ink.sum(axis=0)
    runs = [r for r in _runs(colsum >= min_ink) if r[1] - r[0] >= font_scale * scale // 2]
    boxes: List[Tuple[float, float]] = []
    for a, b in runs:
        if boxes and a < boxes[-1][1]:
            # The run starts inside the previous glyph's box: that glyph's
            # columns broke where the camera lost a thin stroke. Whatever
            # reaches past the box belongs to the next cell, if anything.
            if b - boxes[-1][1] < glyph_w / 2:
                continue
            a = boxes[-1][0] + pitch
        n = max(1, int(round((b - a + (pitch - glyph_w)) / pitch)))
        for k in range(n):
            boxes.append((a + k * pitch, a + k * pitch + glyph_w))
    return [(x0 / scale, x1 / scale) for x0, x1 in boxes]


def snap_to_grid(boxes: List[Tuple[float, float]], font_scale: int,
                 dx: float = 0.0, stretch: float = 1.0) -> List[int]:
    """Grid slot of each box. Every recovery line starts at x=0, so glyph i
    spans [i*pitch, i*pitch+5*s]; ``dx``/``stretch`` absorb the few px of
    residual corner-fit error (slot i sits at ``dx + i*pitch*stretch``).

    A box maps to a slot only if it lies within a third of a pitch of it;
    otherwise segmentation is inconsistent with the layout and the read
    fails rather than guessing. Two boxes in one cell are one glyph whose
    column run broke where the camera lost a thin stroke; the cell is
    sampled from the grid, not the box, so they collapse to one slot.
    """
    pitch = FONT_ADV * font_scale * stretch
    slots: List[int] = []
    for x0, _x1 in boxes:
        slot = int(round((x0 - dx) / pitch))
        if abs(x0 - dx - slot * pitch) > pitch / 3.0:
            raise ReadError(f"glyph at panel x={x0:.0f} does not sit on the "
                            f"{FONT_ADV * font_scale} px character grid")
        if slots and slot < slots[-1]:
            raise ReadError("glyph boxes out of order")
        if slots and slot == slots[-1]:
            continue
        slots.append(slot)
    return slots


def cell_fractions(ink: np.ndarray, scale: int, font_scale: int, x0: float,
                   stretch: float = 1.0, y0: int = 0) -> np.ndarray:
    """Ink fraction of the central half of each of the glyph's 7x5 font pixels.

    ``y0`` is the glyph's top row in ``ink`` (rectified px).
    """
    px = font_scale * scale
    pxw = px * stretch
    cells = np.zeros((FONT_H, FONT_W))
    q = max(1, px // 4)
    base = x0 * scale
    for r in range(FONT_H):
        for c in range(FONT_W):
            ys = y0 + r * px + q
            xs = int(round(base + c * pxw + q))
            xe = int(round(base + (c + 1) * pxw - q))
            block = ink[ys: y0 + r * px + px - q, max(0, xs): max(0, xe)]
            cells[r, c] = block.mean() if block.size else 0.0
    return cells


def _font_stack(font: Dict[str, Tuple[int, ...]], alphabet: str) -> Tuple[str, np.ndarray]:
    chars = "".join(ch for ch in alphabet if ch in font)
    return chars, np.stack([glyph_bitmap(font[ch]) for ch in chars])


def rank_glyph(cells: np.ndarray, font: Dict[str, Tuple[int, ...]], alphabet: str,
               stack: Optional[Tuple[str, np.ndarray]] = None) -> List[Tuple[float, str]]:
    chars, bms = stack or _font_stack(font, alphabet)
    d = np.abs(bms - cells[None]).sum(axis=(1, 2))
    return sorted(zip(d.tolist(), chars))


# Residual horizontal fit searched per line, in panel px and as a pitch
# factor. The text-anchored corner fit leaves about a px of error; a third
# of a font pixel at the smallest scale would already defeat the sampling.
LINE_DX = tuple(np.arange(-4.0, 4.01, 0.5))
LINE_STRETCH = (0.985, 0.99, 0.995, 1.0, 1.005, 1.01, 1.015)


def read_line(panel: np.ndarray, scale: int, y_top: int, font_scale: int,
              font: Dict[str, Tuple[int, ...]], alphabet: str) -> LineRead:
    """Read one line, choosing the grid offset/stretch with the best total match."""
    ink, pad, _top = _band(panel, scale, y_top, font_scale)
    win = FONT_H * font_scale * scale
    boxes = segment_line(ink[pad: pad + win], scale, font_scale)
    stack = _font_stack(font, alphabet)
    best: Optional[Tuple[float, LineRead]] = None
    last_err: Optional[ReadError] = None
    for stretch in LINE_STRETCH:
        pitch = FONT_ADV * font_scale * stretch
        for dx in LINE_DX:
            try:
                slots = snap_to_grid(boxes, font_scale, dx, stretch)
            except ReadError as e:
                last_err = e
                continue
            glyphs = []
            ranked = []
            for slot in slots:
                x0 = dx + slot * pitch
                cells = cell_fractions(ink, scale, font_scale, x0, stretch, pad)
                glyphs.append(Glyph(x0, x0 + FONT_W * font_scale * stretch, cells, slot))
                ranked.append(rank_glyph(cells, font, alphabet, stack))
            total = sum(r[0][0] for r in ranked)
            if best is None or total < best[0]:
                best = (total, LineRead(glyphs, ranked))
    if best is None:
        raise last_err or ReadError(f"no glyphs found at panel y={y_top}")
    # The band is registered for the line as a whole; residual tilt from the
    # corner fit can leave one end a panel px or two off. Re-register each
    # glyph vertically within the pad and keep its closest match.
    line = best[1]
    for i, g in enumerate(line.glyphs):
        stretch = (g.x1 - g.x0) / (FONT_W * font_scale)
        for y0 in range(0, 2 * pad + 1):
            if y0 == pad:
                continue
            cells = cell_fractions(ink, scale, font_scale, g.x0, stretch, y0)
            ranked = rank_glyph(cells, font, alphabet, stack)
            if ranked[0][0] < line.ranked[i][0][0]:
                line.glyphs[i] = Glyph(g.x0, g.x1, cells, g.slot)
                line.ranked[i] = ranked
    return line


# A glyph is unreadable if its best match differs from the font in more than
# this many of its 35 font pixels' worth of ink.
MAX_GLYPH_DISTANCE = 6.0
# The SSID is drawn at scale 2-3, a few camera px per font pixel, so its
# cells sample noisier than the scale-6 passphrase. It is only displayed and
# matched against netsh's SSID list, so a looser cap costs nothing.
SSID_MAX_GLYPH_DISTANCE = 9.0
# Second-best matches within this distance of the best are also offered.
ALT_MARGIN = 2.0


def candidates(ranked: List[List[Tuple[float, str]]], limit: int = MAX_CANDIDATES) -> List[str]:
    """Candidate strings, most likely first: best read, then 5/S and close alternates."""
    options = []
    for pos in ranked:
        best_d, best_c = pos[0]
        if best_d > MAX_GLYPH_DISTANCE:
            raise ReadError("a glyph does not match any font character closely enough")
        opts = {best_c: best_d}
        for d, c in pos[1:]:
            if d - best_d <= ALT_MARGIN:
                opts.setdefault(c, d)
        for a, b in AMBIGUOUS_PAIRS:
            for x, y in ((a, b), (b, a)):
                if best_c == x:
                    alt = next(d for d, c in pos if c == y)
                    opts.setdefault(y, alt)
        options.append(sorted(opts.items(), key=lambda kv: kv[1]))
    combos = []
    for combo in itertools.product(*options):
        combos.append((sum(d for _, d in combo), "".join(c for c, _ in combo)))
        if len(combos) > 4096:
            break
    combos.sort()
    out = []
    for _, s in combos:
        if s not in out:
            out.append(s)
        if len(out) >= limit:
            break
    return out


# --------------------------------------------------------------------------
# Recovery and normal readers
# --------------------------------------------------------------------------

@dataclass
class ApRead:
    mode: str
    ssid: str
    glyph_count: int
    passphrases: List[str]
    warnings: List[str] = field(default_factory=list)


# Normalised correlation of the static recovery strings after refine_corners.
# A good fit scores about 0.38 on the bench camera; a frame without the
# recovery screen scores near 0.
MIN_ANCHOR_SCORE = 0.15


def read_recovery(img: np.ndarray, font: Optional[Dict[str, Tuple[int, ...]]] = None,
                  corners: Optional[np.ndarray] = None, scale: int = 2) -> ApRead:
    font = font or load_font()
    if corners is not None:
        return _read_recovery_at(img, font, np.asarray(corners, dtype=np.float64), scale)
    # The colour-based panel guess can land far enough off that the text fit
    # converges on a wrong local optimum: search from a wide first step, and
    # fall back to the camera's recorded panel corners if that read fails.
    last_err: Optional[ReadError] = None
    best_fit = 0.0
    scorer = _AnchorScorer(img, font)
    for start in _corner_starts(img):
        start, _ = coarse_align(img, start, scorer=scorer)
        c, fit = refine_corners(img, start, steps=REFINE_STEPS, max_rounds=10, scorer=scorer)
        best_fit = max(best_fit, fit)
        if fit < MIN_ANCHOR_SCORE:
            continue
        try:
            first = _read_recovery_at(img, font, c, scale)
        except ReadError as e:
            last_err = e
            continue
        return _reread_with_dynamic_lines(img, font, c, scale, first)
    if last_err is not None:
        raise last_err
    raise ReadError(f"recovery screen text not found (anchor fit {best_fit:.2f} < {MIN_ANCHOR_SCORE})")


# Corner search steps (camera px) after coarse_align. coarse_align's grid
# already lands within about half its shift step, and wider steps here use a
# sharp-panel score with too narrow a peak to climb, so they walk off the fit.
REFINE_STEPS = (8, 4, 2, 1)


# Steps (camera px) for the second fit against the static text plus the
# first read's SSID and passphrase.
REREAD_STEPS = (4, 2, 1)


def _reread_with_dynamic_lines(img: np.ndarray, font: Dict[str, Tuple[int, ...]],
                               corners: np.ndarray, scale: int, first: "ApRead") -> "ApRead":
    """Refit the corners with the first read's own text in the template, then reread.

    Every static line is left aligned and spans at most 276 of the panel's
    480 px, so the right-hand corners are only loosely pinned by them and
    the fit can leave the horizontal scale a few percent off. The
    passphrase line spans 432 px: adding what the first pass read (one
    misread glyph barely moves the score) pins the full width.
    """
    lines = tuple(ANCHOR_LINES) + (
        (Y_SSID, 3 if len(first.ssid) <= 26 else 2, first.ssid),
        (Y_PASS, PASS_SCALE, first.passphrases[0]),
    )
    sc = _AnchorScorer(img, font, lines)
    c, _ = refine_corners(img, corners, steps=REREAD_STEPS, max_rounds=10, scorer=sc)
    try:
        return _read_recovery_at(img, font, c, scale)
    except ReadError:
        return first


def _corner_starts(img: np.ndarray) -> List[np.ndarray]:
    """Start corners for the fit: colour detection, then the bench crop.

    A failed colour detection is not fatal while the bench's measured
    corners are available; coarse_align then searches around those.
    """
    starts: List[np.ndarray] = []
    err: Optional[ReadError] = None
    try:
        starts.append(detect_panel(img))
    except ReadError as e:
        err = e
    try:
        from kilnctrl.bench_test.lcd_sampler import FRAME_CORNERS
    except ImportError:
        FRAME_CORNERS = None
    if FRAME_CORNERS is not None:
        starts.append(np.array(FRAME_CORNERS, dtype=np.float64))
    if not starts:
        assert err is not None
        raise err
    return starts


def _read_recovery_at(img: np.ndarray, font: Dict[str, Tuple[int, ...]],
                      corners: np.ndarray, scale: int) -> ApRead:
    panel = rectify(img, corners, scale)
    warnings = []

    printable = "".join(chr(c) for c in range(0x21, 0x7F))
    ssid = None
    for ssid_scale in (3, 2):
        try:
            line = read_line(panel, scale, Y_SSID, ssid_scale, font, printable)
        except ReadError:
            continue
        if line.glyphs and all(r[0][0] <= SSID_MAX_GLYPH_DISTANCE for r in line.ranked):
            n = len(line.glyphs)
            # recovery_lcd.c: scale 3 only for names of 26 chars or fewer.
            if (ssid_scale == 3) == (n <= 26):
                ssid = _ssid_text(line, ssid_scale)
                break
    if ssid is None:
        raise ReadError("SSID line could not be read (is the recovery screen showing a network?)")

    pw = read_line(panel, scale, Y_PASS, PASS_SCALE, font, PASS_ALPHABET)
    count = len(pw.glyphs)
    if count != PASS_LEN:
        raise ReadError(f"passphrase line segmented into {count} glyphs, expected {PASS_LEN}")
    cands = candidates(pw.ranked)
    return ApRead("recovery", ssid, count, cands, warnings)


def _ssid_text(line: LineRead, font_scale: int) -> str:
    """Rebuild the SSID including spaces from grid slots."""
    pitch = FONT_ADV * font_scale
    out = []
    pos = 0
    for g, r in zip(line.glyphs, line.ranked):
        slot = g.slot
        out.append(" " * (slot - pos))
        out.append(r[0][1])
        pos = slot + 1
    return "".join(out)


_WIFI_QR = re.compile(r"^WIFI:(.*);;?$", re.S)


def parse_wifi_qr(text: str) -> Tuple[str, str]:
    """(ssid, password) from a ``WIFI:T:WPA;S:<ssid>;P:<pass>;;`` payload."""
    m = _WIFI_QR.match(text.strip())
    if not m:
        raise ReadError("QR code is not a WIFI: payload")
    fields: Dict[str, str] = {}
    body = m.group(1)
    i = 0
    while i < len(body):
        j = body.find(":", i)
        if j < 0:
            break
        key = body[i:j]
        val = []
        k = j + 1
        while k < len(body) and body[k] != ";":
            if body[k] == "\\" and k + 1 < len(body):
                k += 1
            val.append(body[k])
            k += 1
        fields[key] = "".join(val)
        i = k + 1
    if "S" not in fields or "P" not in fields:
        raise ReadError("WIFI: QR payload has no S: or P: field")
    return fields["S"], fields["P"]


# Where the recovery screen's own QR sits in a bench camera frame: the panel's
# top-right corner, i.e. the upper-right quarter of the frame (bench aim
# 2026-09-24: x ~820..1045 of 1280, y ~70..230 of 720). A foreign WIFI: code
# (phone, printout) elsewhere in the frame is ignored. Fractions (x0,y0,x1,y1).
RECOVERY_QR_REGION = (0.5, 0.0, 1.0, 0.5)
MAX_SSID_BYTES = 32


def _wifi_qr_payloads(img, region=None) -> List[str]:
    """Every ``WIFI:`` QR payload in ``img`` (PIL image or HxWx3 array)."""
    try:
        import zxingcpp
    except ImportError as exc:  # declared dependency; a broken install only
        raise ReadError("the zxing-cpp wheel is missing (uv sync in tools/PcTools)") from exc
    if isinstance(img, np.ndarray):
        img = np.clip(img, 0, 255).astype(np.uint8)
    return wifi_payloads(zxingcpp.read_barcodes, img, region)


QR_UPSCALES = (1, 2, 3, 4)


def wifi_payloads(read_barcodes, im, region=None) -> List[str]:
    """WIFI: payloads in a camera frame, retrying at larger scales.

    The panel's QR is only about 90 camera px wide, so a decoder given the
    raw frame often finds nothing; the same frame upscaled 2x-4x (grayscale,
    then autocontrast) decodes. The raw frame is tried first, then the first
    scale that yields a payload wins.
    """
    from PIL import Image, ImageOps

    if isinstance(im, np.ndarray):
        frame_w, frame_h = im.shape[1], im.shape[0]
    else:
        frame_w, frame_h = im.size

    def in_region(res, scale):
        # `region` filters by the symbol centre (fractions of the ORIGINAL
        # frame); a result without a position (or no region) is kept.
        pos = getattr(res, "position", None)
        if region is None or pos is None:
            return True
        pts = (pos.top_left, pos.top_right, pos.bottom_right, pos.bottom_left)
        cx = sum(p.x for p in pts) / 4.0 / scale / frame_w
        cy = sum(p.y for p in pts) / 4.0 / scale / frame_h
        return region[0] <= cx <= region[2] and region[1] <= cy <= region[3]

    def hits(x, scale=1):
        return [res.text for res in read_barcodes(x)
                if res.text.startswith("WIFI:") and in_region(res, scale)]

    found = hits(im)
    if found:
        return found
    if isinstance(im, np.ndarray):
        im = Image.fromarray(im)
    gray = ImageOps.grayscale(im)
    for scale in QR_UPSCALES:
        base = gray if scale == 1 else gray.resize((gray.width * scale, gray.height * scale))
        for variant in (base, ImageOps.autocontrast(base)):
            found = hits(variant, scale)
            if found:
                return found
    return []


def read_recovery_qr(img: np.ndarray) -> ApRead:
    """Recovery credentials from the screen's own Wi-Fi QR; ReadError if unusable."""
    payloads = _wifi_qr_payloads(img, RECOVERY_QR_REGION)
    if not payloads:
        raise ReadError("no WIFI: QR code found on the recovery screen")
    if len(set(payloads)) > 1:
        raise ReadError("more than one distinct WIFI: QR code in the frame")
    ssid, password = parse_wifi_qr(payloads[0])
    if not ssid or len(ssid.encode("utf-8")) > MAX_SSID_BYTES:
        raise ReadError("recovery QR has an empty or over-long SSID")
    if len(password) != PASS_LEN or any(ch not in PASS_ALPHABET for ch in password):
        raise ReadError("recovery QR passphrase is not 12 characters of the recovery alphabet")
    return ApRead("recovery", ssid, len(password), [password])


def read_recovery_auto(img: np.ndarray, font: Optional[Dict[str, Tuple[int, ...]]] = None) -> ApRead:
    """QR first, glyph segmentation as the fallback (recovery mode only)."""
    try:
        return read_recovery_qr(img)
    except ReadError as qr_err:
        read = read_recovery(img, font)
        read.warnings.append(f"QR not used ({qr_err}); passphrase read by glyph segmentation")
        return read


def read_normal(image_path: str) -> ApRead:
    from PIL import Image

    with Image.open(image_path) as im:
        payloads = _wifi_qr_payloads(im.convert("RGB"))
    if not payloads:
        raise ReadError("no WIFI: QR code found -- open Settings > Network with an ADMIN LCD "
                        "session (the QR and password are hidden without one)")
    if len(set(payloads)) > 1:
        raise ReadError("more than one distinct WIFI: QR code in the frame")
    ssid, password = parse_wifi_qr(payloads[0])
    return ApRead("normal", ssid, len(password), [password])


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def main(argv: Optional[Sequence[str]] = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--image", required=True, help="full (uncropped) capture_lcd.ps1 -Full frame")
    ap.add_argument("--mode", choices=("recovery", "normal"), default="recovery")
    ap.add_argument("--candidates-out", help="file to write passphrase candidates to (one per line)")
    args = ap.parse_args(argv)

    result: Dict[str, object] = {"mode": args.mode}
    try:
        if args.mode == "recovery":
            read = read_recovery_auto(load_image(args.image))
        else:
            read = read_normal(args.image)
    except ReadError as exc:
        result.update(ok=False, error=str(exc))
        print(json.dumps(result))
        return 2
    if args.candidates_out:
        fd = os.open(args.candidates_out, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as f:
            f.write("\n".join(read.passphrases) + "\n")
    result.update(ok=True, ssid=read.ssid, glyph_count=read.glyph_count,
                  candidate_count=len(read.passphrases), warnings=read.warnings)
    print(json.dumps(result))
    return 0


if __name__ == "__main__":
    sys.exit(main())
