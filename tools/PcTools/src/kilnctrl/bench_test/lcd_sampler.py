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
The board sits rotated a few degrees in front of the camera (CLAUDE.md
"Camera aim (2026-09-19)"), so a plain per-axis scale is wrong -- this
fits a full affine map (6 parameters: rotation + independent x/y scale +
translation) from the four measured screen corners, least-squares, so the
four corners is not itself the whole calibration; it also means a fifth
point off the diagonal is not automatically exact, which is why tolerance
work below is calibrated against the bezel, not against an assumed-perfect
transform.
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
#: CLAUDE.md's "Camera aim (2026-09-19)" numeric edge scan (never re-typed
#: from a screenshot -- these are the exact values quoted there):
#:   left edge:   x=298 (top) .. x=323 (bottom)
#:   right edge:  x=1145 (top) .. x=1147 (bottom)  (quoted as "1145-1147")
#:   top edge:    y=86 (left) .. y=60 (right)
#:   bottom edge: y=635 (left) .. y=617 (right)
#: giving corners TL=(298,86) TR=(1145,60) BL=(323,635) BR=(1147,617), every value quoted verbatim from
#: CLAUDE.md (no midpoints invented) --
#: and consistent with "the board sits rotated a few degrees with its right
#: side about 25px higher than its left".
FRAME_CORNERS: Tuple[Tuple[float, float], ...] = (
    (298.0, 86.0),
    (1145.0, 60.0),
    (323.0, 635.0),
    (1147.0, 617.0),
)


@dataclasses.dataclass(frozen=True)
class AffineTransform:
    """x' = a*x + b*y + c ; y' = d*x + e*y + f -- a full 2D affine map, not
    a scale-only one, so the panel's rotation (CLAUDE.md) is represented."""

    a: float
    b: float
    c: float
    d: float
    e: float
    f: float

    def apply(self, x: float, y: float) -> Tuple[float, float]:
        return (self.a * x + self.b * y + self.c, self.d * x + self.e * y + self.f)

    @classmethod
    def fit(cls, src: Sequence[Tuple[float, float]], dst: Sequence[Tuple[float, float]]) -> "AffineTransform":
        """Least-squares fit of the 6 affine parameters from >=3 point
        correspondences (4 here -- one more than the minimum, so the fit is
        a genuine least-squares solve, not an exact interpolation of 3
        points with the 4th unused). No numpy dependency (memory
        project_pctools_numpy_undeclared_dependency) -- plain Gaussian
        elimination on the two independent 3x3 normal-equation systems
        (x' and y' each depend on the same [x, y, 1] design matrix).
        """
        if len(src) != len(dst):
            raise ValueError("src and dst must have the same number of points")
        if len(src) < 3:
            raise ValueError("need at least 3 point correspondences to fit an affine transform")

        # Design matrix rows: [x, y, 1] for each point once.
        rows = [(x, y, 1.0) for x, y in src]

        def solve_axis(targets: Sequence[float]) -> Tuple[float, float, float]:
            # Normal equations: (R^T R) p = R^T t, solved by hand for the
            # 3x3 case with Gaussian elimination + partial pivoting.
            ata = [[0.0] * 3 for _ in range(3)]
            atb = [0.0, 0.0, 0.0]
            for row, t in zip(rows, targets):
                for i in range(3):
                    atb[i] += row[i] * t
                    for j in range(3):
                        ata[i][j] += row[i] * row[j]
            return _solve3(ata, atb)

        xa, xb, xc = solve_axis([p[0] for p in dst])
        ya, yb, yc = solve_axis([p[1] for p in dst])
        return cls(a=xa, b=xb, c=xc, d=ya, e=yb, f=yc)


def _solve3(m: List[List[float]], v: List[float]) -> Tuple[float, float, float]:
    """Solve a 3x3 linear system m @ x = v via Gaussian elimination with
    partial pivoting. Raises ValueError on a singular system (e.g. the 4
    corners are collinear -- would mean the calibration itself is broken,
    not something to silently paper over)."""
    m = [row[:] + [v[i]] for i, row in enumerate(m)]
    n = 3
    for col in range(n):
        pivot_row = max(range(col, n), key=lambda r: abs(m[r][col]))
        if abs(m[pivot_row][col]) < 1e-9:
            raise ValueError("singular system fitting affine transform (degenerate corner points)")
        m[col], m[pivot_row] = m[pivot_row], m[col]
        pivot = m[col][col]
        m[col] = [val / pivot for val in m[col]]
        for r in range(n):
            if r == col:
                continue
            factor = m[r][col]
            if factor:
                m[r] = [a - factor * b for a, b in zip(m[r], m[col])]
    return (m[0][3], m[1][3], m[2][3])


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


def color_distance(a: Tuple[int, int, int], b: Tuple[int, int, int]) -> float:
    return sum((x - y) ** 2 for x, y in zip(a, b)) ** 0.5


def is_off(rgb: Tuple[int, int, int], bezel: Tuple[int, int, int], tol: float = MIN_BEZEL_CONTRAST) -> bool:
    """True if `rgb` reads indistinguishable from the (dark) bezel
    reference -- i.e. this region is background/hidden, not lit."""
    return color_distance(rgb, bezel) <= tol


def matches_color(rgb: Tuple[int, int, int], target: Tuple[int, int, int], bezel: Tuple[int, int, int],
                   tol: float = COLOR_MATCH_TOLERANCE, min_bezel_contrast: float = MIN_BEZEL_CONTRAST) -> bool:
    """True if `rgb` is close to `target` AND distinctly different from
    the bezel -- a region cannot "match" a bright accent color while also
    reading as indistinguishable from the dark bezel (a camera fault or a
    badly mis-measured region would otherwise pass by accident if only the
    target-distance half were checked)."""
    if color_distance(rgb, bezel) < min_bezel_contrast:
        return False
    return color_distance(rgb, target) <= tol
