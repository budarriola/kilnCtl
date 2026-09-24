"""LCD capture + sampling pipeline for the `lcd` suite (plan §8 Wave 1c).

Wraps two existing scripts rather than reimplementing them:

* ``tools/PcTools/scripts/capture_lcd.ps1`` grabs one frame from the bench
  webcam. Wave 1c always calls it with ``-Full`` -- the CLAUDE.md corner
  measurements (2026-09-19 camera aim) are in full 1280x720 frame
  coordinates, and composing them through the default crop+scale would
  just add another transform to get wrong.
* ``tools/PcTools/scripts/sample_lcd_region.ps1`` (this wave adds a
  ``-Json`` switch, backwards compatible -- the plain-text output is
  unchanged when it is omitted) reports a region's mean RGB plus a bezel
  reference sample.

The other half of this module is the widget-centre -> camera-frame
transform: LVGL reports tap-target centres in the display's own 480x320
landscape coordinate space (``list_tap_targets()``, ``ui_test_client.py``).
The board sits at a perspective skew in front of the camera (CLAUDE.md
"Camera aim": the right edge measures shorter than the left and slants,
the top edge is longer than the bottom -- not a simple in-plane rotation),
so a plain per-axis scale, or even a full affine map, is wrong -- this
fits a 4-point homography (projective transform) from the four measured
screen corners, which is exact at all four corners by construction, unlike
a least-squares affine fit. A point off the four corners is still not
automatically exact for a real (imperfectly measured) quadrilateral, which
is why tolerance work below is calibrated against the bezel, not against
an assumed-perfect transform.
"""
from __future__ import annotations

import dataclasses
import json
import os
import subprocess
from typing import List, Optional, Sequence, Tuple

#: The kilnCtl LCD's native landscape resolution (ILI9488/ST7796, 480x320 --
#: firmware/KilnFW/docs/ILI9488.md, DISPLAY_ST7796_PLAN.md). Tap-target
#: centres from list_tap_targets() are in this space.
LCD_WIDTH = 480
LCD_HEIGHT = 320

#: The four screen corners in the LCD's own coordinate space, matched 1:1
#: (by list index) with FRAME_CORNERS below: top-left, top-right,
#: bottom-left, bottom-right.
WIDGET_CORNERS: Tuple[Tuple[float, float], ...] = (
    (0.0, 0.0),
    (float(LCD_WIDTH), 0.0),
    (0.0, float(LCD_HEIGHT)),
    (float(LCD_WIDTH), float(LCD_HEIGHT)),
)

#: The same four corners, in the bench camera's full 1280x720 frame, per
#: CLAUDE.md's "Camera aim (2026-09-24)" numeric edge scan (never re-typed
#: from a screenshot -- these are the exact values quoted there):
#:   top-left corner:     (160, 52)
#:   top-right corner:    (1044, 116)
#:   bottom-left corner:  (161, 645)
#:   bottom-right corner: (983, 624)
#: The geometry is a perspective skew, not a simple rotation: the right
#: edge is shorter than the left (~14%) and slants (~61px lower at the top
#: than at the bottom vs the left edge's near-vertical run), while the top
#: edge is longer than the bottom (~8%). This has changed shape since
#: 2026-09-19, not just direction.
#: Superseded 2026-09-19 corners, kept for history: TL=(298,86) TR=(1145,60)
#: BL=(323,635) BR=(1147,617).
FRAME_CORNERS: Tuple[Tuple[float, float], ...] = (
    (160.0, 52.0),
    (1044.0, 116.0),
    (161.0, 645.0),
    (983.0, 624.0),
)


@dataclasses.dataclass(frozen=True)
class AffineTransform:
    """A perspective (projective) map, fit exactly through 4 point
    correspondences -- a plain affine map cannot represent the panel's
    perspective skew (CLAUDE.md "Camera aim": the right edge measures ~14%
    shorter than the left and slants 61px while the left edge is vertical,
    and the top edge is ~8% longer than the bottom), so 4 corners are fit
    with a full homography instead of a 6-parameter least-squares affine
    fit:

        x' = (a*x + b*y + c) / (g*x + h*y + 1)
        y' = (d*x + e*y + f) / (g*x + h*y + 1)

    The class name is kept as ``AffineTransform`` for callers, even though
    the map is now projective, to avoid touching call sites outside this
    module (``widget_to_frame``/``sample_widget`` are the only other
    consumers, and they only ever call ``.apply()``).
    """

    a: float
    b: float
    c: float
    d: float
    e: float
    f: float
    g: float = 0.0
    h: float = 0.0

    def apply(self, x: float, y: float) -> Tuple[float, float]:
        w = self.g * x + self.h * y + 1.0
        if abs(w) < 1e-12:
            raise ValueError("degenerate homography apply() (w ~= 0)")
        return ((self.a * x + self.b * y + self.c) / w, (self.d * x + self.e * y + self.f) / w)

    @classmethod
    def fit(cls, src: Sequence[Tuple[float, float]], dst: Sequence[Tuple[float, float]]) -> "AffineTransform":
        """Exact 4-point homography fit (8 unknowns from 4 correspondences,
        8 equations -- not a least-squares over-determined solve). No numpy
        dependency (memory project_pctools_numpy_undeclared_dependency) --
        plain Gaussian elimination on the 8x8 linear system, generalising
        the previous 3x3 ``_solve3`` pattern.

        Requires exactly 4 point correspondences (the classic 4-point DLT
        case); a caller needing more or fewer points would need a genuine
        least-squares homography solve, which this does not implement.
        """
        if len(src) != len(dst):
            raise ValueError("src and dst must have the same number of points")
        if len(src) != 4:
            raise ValueError("homography fit requires exactly 4 point correspondences")

        # Build the 8x8 system A p = b for p = [a,b,c,d,e,f,g,h].
        rows: List[List[float]] = []
        rhs: List[float] = []
        for (x, y), (xp, yp) in zip(src, dst):
            rows.append([x, y, 1.0, 0.0, 0.0, 0.0, -x * xp, -y * xp])
            rhs.append(xp)
            rows.append([0.0, 0.0, 0.0, x, y, 1.0, -x * yp, -y * yp])
            rhs.append(yp)

        a, b, c, d, e, f, g, h = _solve_n(rows, rhs)
        return cls(a=a, b=b, c=c, d=d, e=e, f=f, g=g, h=h)


def _solve_n(m: List[List[float]], v: List[float]) -> Tuple[float, ...]:
    """Solve an NxN linear system m @ x = v via Gaussian elimination with
    partial pivoting. Raises ValueError on a singular system (e.g. the 4
    corners are collinear -- would mean the calibration itself is broken,
    not something to silently paper over)."""
    n = len(v)
    m = [row[:] + [v[i]] for i, row in enumerate(m)]
    for col in range(n):
        pivot_row = max(range(col, n), key=lambda r: abs(m[r][col]))
        if abs(m[pivot_row][col]) < 1e-9:
            raise ValueError("singular system fitting perspective transform (degenerate corner points)")
        m[col], m[pivot_row] = m[pivot_row], m[col]
        pivot = m[col][col]
        m[col] = [val / pivot for val in m[col]]
        for r in range(n):
            if r == col:
                continue
            factor = m[r][col]
            if factor:
                m[r] = [a - factor * b for a, b in zip(m[r], m[col])]
    return tuple(row[n] for row in m)


#: The transform fit once, from the CLAUDE.md corners, at import time. A
#: fresh camera re-aim only ever needs the constants above updated (same
#: place capture_lcd.ps1's own defaults get updated) -- this recomputes
#: itself from them rather than caching a stale fit.
DEFAULT_TRANSFORM = AffineTransform.fit(WIDGET_CORNERS, FRAME_CORNERS)


def widget_to_frame(cx: float, cy: float, transform: Optional[AffineTransform] = None) -> Tuple[int, int]:
    """Map a widget-space point (list_tap_targets()' cx/cy, 480x320
    landscape) to a pixel coordinate in the camera's full 1280x720 frame."""
    t = transform or DEFAULT_TRANSFORM
    fx, fy = t.apply(float(cx), float(cy))
    return (int(round(fx)), int(round(fy)))


# ---------------------------------------------------------------------------
# Script wrappers. Kept thin and mockable -- tests patch _run(), never
# subprocess itself, per the existing bench_test convention of separating
# "I/O" from "judgment" (judgments.py's docstring).
# ---------------------------------------------------------------------------

def _scripts_dir(repo_root: Optional[str] = None) -> str:
    root = repo_root or os.path.normpath(
        os.path.join(os.path.dirname(__file__), "..", "..", "..", "..", "..")
    )
    return os.path.join(root, "tools", "PcTools", "scripts")


def _run(args: List[str], timeout: float = 20.0) -> "subprocess.CompletedProcess[str]":
    return subprocess.run(args, capture_output=True, text=True, timeout=timeout, check=False)


class LcdCaptureError(RuntimeError):
    """Raised when capture_lcd.ps1/sample_lcd_region.ps1 fail or the frame
    could not be produced -- e.g. ffmpeg exit -5 (camera busy, CLAUDE.md:
    "check for another process holding the C920 first"). Callers translate
    this to SKIP/INCONCLUSIVE, never to a fabricated PASS."""


def capture_full_frame(out_path: str, repo_root: Optional[str] = None, device: Optional[str] = None) -> str:
    """Run capture_lcd.ps1 -Full -Out out_path. Returns out_path on success."""
    script = os.path.join(_scripts_dir(repo_root), "capture_lcd.ps1")
    args = ["powershell", "-ExecutionPolicy", "Bypass", "-File", script, "-Full", "-Out", out_path]
    if device:
        args += ["-Device", device]
    proc = _run(args, timeout=30.0)
    if proc.returncode != 0:
        raise LcdCaptureError(f"capture_lcd.ps1 -Full failed (exit {proc.returncode}): {proc.stderr.strip() or proc.stdout.strip()}")
    if not os.path.isfile(out_path):
        raise LcdCaptureError(f"capture_lcd.ps1 reported success but {out_path} was not written")
    return out_path


@dataclasses.dataclass
class RegionSample:
    region: Tuple[int, int, int]
    bezel: Optional[Tuple[int, int, int]]


def sample_region(image_path: str, x: int, y: int, w: int = 8, h: int = 8,
                   bezel_x: int = 100, bezel_y: int = 100,
                   repo_root: Optional[str] = None) -> RegionSample:
    """Run sample_lcd_region.ps1 -Json and parse its output."""
    script = os.path.join(_scripts_dir(repo_root), "sample_lcd_region.ps1")
    args = [
        "powershell", "-ExecutionPolicy", "Bypass", "-File", script,
        "-Image", image_path, "-X", str(x), "-Y", str(y), "-W", str(w), "-H", str(h),
        "-BezelX", str(bezel_x), "-BezelY", str(bezel_y), "-Json",
    ]
    proc = _run(args, timeout=20.0)
    if proc.returncode != 0:
        raise LcdCaptureError(f"sample_lcd_region.ps1 failed (exit {proc.returncode}): {proc.stderr.strip() or proc.stdout.strip()}")
    return parse_sample_json(proc.stdout)


def parse_sample_json(stdout: str) -> RegionSample:
    """Pulled out of sample_region() so a fake-subprocess unit test can
    feed captured stdout text directly (plan's test requirement) without
    invoking powershell at all."""
    text = stdout.strip()
    if not text:
        raise LcdCaptureError("sample_lcd_region.ps1 -Json produced no output")
    try:
        data = json.loads(text)
    except json.JSONDecodeError as exc:
        raise LcdCaptureError(f"sample_lcd_region.ps1 -Json produced unparseable output: {text!r}") from exc
    region = data.get("region")
    if not region or "R" not in region:
        raise LcdCaptureError(f"sample_lcd_region.ps1 -Json missing 'region': {data!r}")
    bezel = data.get("bezel")
    return RegionSample(
        region=(int(region["R"]), int(region["G"]), int(region["B"])),
        bezel=(int(bezel["R"]), int(bezel["G"]), int(bezel["B"])) if bezel else None,
    )


def sample_widget(image_path: str, cx: float, cy: float, w: int = 8, h: int = 8,
                   bezel_x: int = 100, bezel_y: int = 100,
                   transform: Optional[AffineTransform] = None,
                   repo_root: Optional[str] = None) -> RegionSample:
    """Convenience: widget-space centre -> frame coords -> sample_region()."""
    fx, fy = widget_to_frame(cx, cy, transform)
    # Sample a small box centred on the mapped point, same convention as
    # sample_lcd_region.ps1's own X/Y (top-left of the box).
    return sample_region(image_path, fx - w // 2, fy - h // 2, w, h, bezel_x, bezel_y, repo_root)


# ---------------------------------------------------------------------------
# Tolerance math (plan §8: "the bezel-calibrated tolerance"). CLAUDE.md is
# explicit that colors are judged by numeric sampling, never by eye and
# never by trusting the theme source constants blindly -- so every check
# here is relative to an actually-sampled bezel reference, not an assumed-
# perfect camera/color pipeline.
# ---------------------------------------------------------------------------

#: Minimum Euclidean RGB distance from the bezel reference for a region to
#: be considered "distinctly different from off" -- i.e. actually lit,
#: not just camera/JPEG noise around the same dark bezel level. Chosen
#: generously (JPEG compression + auto-exposure noise on this webcam has
#: been observed to vary a dark region by a few counts per channel; 15 in
#: quadrature is comfortably above that floor while still well below the
#: 40-100 count swings between the theme's actual accent colors).
MIN_BEZEL_CONTRAST = 25.0

#: Tolerance for "this region approximately matches this target color",
#: applied on top of (never instead of) the bezel-contrast check above.
COLOR_MATCH_TOLERANCE = 45.0

#: Tolerance for the chromaticity fallback below, in normalised (r/sum,
#: g/sum, b/sum) space -- see matches_color()'s docstring. Chosen from the
#: 2026-09-24 bench evidence: a genuinely-lit ACCENT_4 (green Start button)
#: sampled at RGB(60,138,92) against the theme's RGB(92,192,110) reference
#: is only chroma-distance ~0.048 away (the camera's under-exposure/white-
#: balance scaled all three channels together, which chromaticity cancels),
#: while a wrong-hue red RGB(200,60,60) is ~0.50 away and a colorless grey
#: RGB(150,150,150) is ~0.19 away -- both comfortably outside this
#: threshold, so a genuinely wrong button color still fails.
CHROMA_MATCH_TOLERANCE = 0.10

#: Brightness floor for the chromaticity fallback only: the sample's channel
#: sum must be at least this fraction of the target's own channel sum before
#: its channel RATIOS are trusted at all. Chromaticity is scale-invariant by
#: construction, so without this a near-black region -- a blanked panel, an
#: unlit/hidden button, dark sensor noise with a faint green cast such as
#: RGB(10,20,12) or RGB(3,6,4) -- has almost exactly ACCENT_4's ratios and
#: would "match" whenever it clears the bezel-contrast gate (which a dark
#: region does easily against a mis-exposed bright bezel like the
#: 2026-09-24 RGB(160,233,253), and even against the normal dark bezel when
#: the region reads darker than it). The 2026-09-24 genuine sample
#: RGB(60,138,92) sums to 290, ~0.77 of ACCENT_4's 378, so 0.5 keeps that
#: evidence passing while rejecting anything dimmer than half-exposure.
CHROMA_MIN_BRIGHTNESS_RATIO = 0.5


def color_distance(a: Tuple[int, int, int], b: Tuple[int, int, int]) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def _chromaticity(rgb: Tuple[int, int, int]) -> Tuple[float, float, float]:
    """Normalise `rgb` to (r/sum, g/sum, b/sum) -- a uniform-scale-invariant
    "hue+saturation" descriptor. A camera's auto-exposure/white-balance
    gain multiplies every channel by roughly the same factor (that is what
    "under-exposed" or "warmer/cooler white balance" means physically), so
    this cancels that factor out while a genuine hue difference (a red or
    grey button instead of green) still survives it. Never used in place of
    the absolute bezel-contrast floor -- a near-black sample's channel
    ratios are noise, not signal, which is exactly why is_off()/
    min_bezel_contrast still gate on the raw distance first."""
    total = sum(rgb)
    if total <= 0:
        return (0.0, 0.0, 0.0)
    return (rgb[0] / total, rgb[1] / total, rgb[2] / total)


def is_off(rgb: Tuple[int, int, int], bezel: Tuple[int, int, int], tol: float = MIN_BEZEL_CONTRAST) -> bool:
    """True if `rgb` reads indistinguishable from the (dark) bezel
    reference -- i.e. this region is background/hidden, not lit."""
    return color_distance(rgb, bezel) <= tol


def matches_color(rgb: Tuple[int, int, int], target: Tuple[int, int, int], bezel: Tuple[int, int, int],
                   tol: float = COLOR_MATCH_TOLERANCE, min_bezel_contrast: float = MIN_BEZEL_CONTRAST,
                   chroma_tol: float = CHROMA_MATCH_TOLERANCE,
                   chroma_min_brightness_ratio: float = CHROMA_MIN_BRIGHTNESS_RATIO) -> bool:
    """True if `rgb` is close to `target` AND distinctly different from
    the bezel -- a region cannot "match" a bright accent color while also
    reading as indistinguishable from the dark bezel (a camera fault or a
    badly mis-measured region would otherwise pass by accident if only the
    target-distance half were checked).

    "Close to `target`" is checked two ways, either sufficient once the
    bezel-contrast gate above has passed:

    1. The original absolute Euclidean RGB distance (`tol`) -- fine when
       the camera's white balance happens to be neutral.
    2. A chromaticity (hue/saturation, exposure-cancelled) distance
       (`chroma_tol`) -- 2026-09-24 bench evidence (CLAUDE.md's ST7796
       colour-order history) showed a real, correctly-lit ACCENT_4 button
       sampled well outside `tol` under this camera's actual white balance
       (the bezel reference itself sampled as sky-blue, not black, that
       same run), while still being unmistakably green by hue. Only (1) can
       tell a genuinely wrong hue (red, grey) apart from a merely exposure-
       shifted correct one when both would otherwise pass a loose enough
       absolute tolerance, so (2) is an OR added on top of (1), never a
       replacement for it -- CLAUDE.md's "judge colors by numeric pixel
       sampling, never by eye" still holds: this compares two sampled
       numbers, never a theme source constant read informally. (2) only
       applies once the sample is at least `chroma_min_brightness_ratio` as
       bright (channel sum) as `target` -- a dark/blanked region has
       noise-level channel ratios that can mimic any hue."""
    if color_distance(rgb, bezel) < min_bezel_contrast:
        return False
    if color_distance(rgb, target) <= tol:
        return True
    # Chromaticity is meaningless for a dim sample (see
    # CHROMA_MIN_BRIGHTNESS_RATIO): a blanked or unlit region must never
    # reach the scale-invariant comparison below.
    if sum(rgb) < chroma_min_brightness_ratio * sum(target):
        return False
    return color_distance(_chromaticity(rgb), _chromaticity(target)) <= chroma_tol
