"""Offline tests for kilnctrl.lcd_ap_reader against synthetic recovery screens.

The screens are rendered from the firmware's own font and layout constants
and warped into a camera-sized frame with a known homography, so every test
knows the true SSID, passphrase and panel corners. No saved bench capture is
used: those carry a live per-boot passphrase.
"""

import json

import numpy as np
import pytest

from kilnctrl import lcd_ap_reader as r

FRAME_W, FRAME_H = 1280, 720
# A skewed panel, about the bench camera's size and perspective.
TRUE_CORNERS = np.array([[180.0, 70.0], [1040.0, 122.0], [178.0, 628.0], [984.0, 660.0]])
SSID = "kilnctl-recovery"


@pytest.fixture(scope="module")
def font():
    return r.load_font()


def _draw(panel, font, y, scale, text):
    for i, ch in enumerate(text):
        cols = font.get(ch)
        if cols is None:
            continue
        bm = np.kron(r.glyph_bitmap(cols), np.ones((scale, scale)))
        x = i * r.FONT_ADV * scale
        h = min(bm.shape[0], r.PANEL_H - y)
        w = min(bm.shape[1], r.PANEL_W - x)
        panel[y:y + h, x:x + w] = np.maximum(panel[y:y + h, x:x + w], bm[:h, :w])


def render_screen(font, passphrase, ssid=SSID, corners=TRUE_CORNERS):
    """RGB camera frame of the recovery screen with the panel at ``corners``."""
    panel = np.zeros((r.PANEL_H, r.PANEL_W))
    for y, s, text in r.ANCHOR_LINES:
        _draw(panel, font, y, s, text)
    _draw(panel, font, r.Y_SSID, 3 if len(ssid) <= 26 else 2, ssid)
    _draw(panel, font, r.Y_PASS, r.PASS_SCALE, passphrase)
    # Camera pixel centre -> panel coordinate.
    h = r.homography(np.asarray(corners), r.PANEL_CORNERS)
    v, u = np.mgrid[0:FRAME_H, 0:FRAME_W].astype(np.float64) + 0.5
    den = h[2, 0] * u + h[2, 1] * v + h[2, 2]
    px = (h[0, 0] * u + h[0, 1] * v + h[0, 2]) / den
    py = (h[1, 0] * u + h[1, 1] * v + h[1, 2]) / den
    inside = (px >= 0) & (px < r.PANEL_W) & (py >= 0) & (py < r.PANEL_H)
    ink = np.zeros((FRAME_H, FRAME_W))
    ink[inside] = panel[py[inside].astype(int), px[inside].astype(int)]
    grey = np.where(inside, 30.0 + 190.0 * ink, 5.0)
    grey = r._box_blur(grey, 1)
    rng = np.random.default_rng(1)
    grey = grey + rng.normal(0, 3, grey.shape)
    return np.repeat(np.clip(grey, 0, 255)[:, :, None], 3, axis=2)


def test_reads_ssid_and_passphrase_at_true_corners(font):
    pw = "23456789ABCD"
    read = r.read_recovery(render_screen(font, pw), font, corners=TRUE_CORNERS)
    assert read.ssid == SSID
    assert read.glyph_count == r.PASS_LEN
    assert read.passphrases[0] == pw


def test_ambiguous_5_and_s_give_both_candidates(font):
    pw = "5SKLMNPQRTUV"
    read = r.read_recovery(render_screen(font, pw), font, corners=TRUE_CORNERS)
    assert pw in read.passphrases
    swapped = "S5" + pw[2:]
    assert swapped in read.passphrases
    assert len(read.passphrases) <= r.MAX_CANDIDATES


def test_fit_and_reread_recover_a_few_px_corner_error(font, monkeypatch):
    # No coarse grid: the start is already inside refine_corners' basin.
    monkeypatch.setattr(r, "COARSE_SHIFT_PX", 0)
    monkeypatch.setattr(r, "COARSE_SCALES", (1.0,))
    pw = "WXYZ23456789"
    img = render_screen(font, pw)
    start = TRUE_CORNERS + np.array([[6, -5], [-4, 7], [5, 4], [-6, -6]])
    monkeypatch.setattr(r, "_corner_starts", lambda _img: [start])
    seen = []
    orig = r._reread_with_dynamic_lines

    def spy(img_, font_, c, scale, first):
        seen.append(c)
        return orig(img_, font_, c, scale, first)

    monkeypatch.setattr(r, "_reread_with_dynamic_lines", spy)
    read = r.read_recovery(img, font)
    assert len(seen) == 1
    assert read.ssid == SSID
    assert read.passphrases[0] == pw


def test_dynamic_refit_pins_the_right_hand_corners(font, monkeypatch):
    # The static lines alone leave the right edge loose; the refit with the
    # read text must bring a 12 px horizontal-scale error back within 3 px.
    # A short SSID: a long one would pin the right edge by itself, leaving the
    # passphrase line (432 px wide) untested.
    pw, ssid = "ABCDEFGHJKLM", "kc-ap"
    img = render_screen(font, pw, ssid=ssid)
    off = TRUE_CORNERS + np.array([[0, 0], [-12, 0], [0, 0], [-12, 0]])
    # The refit scores against the first read's text, so that text is right;
    # the sentinel mode shows the result is a fresh read, not ``first``.
    first = r.ApRead("sentinel", ssid, r.PASS_LEN, [pw])
    used = []
    orig = r._read_recovery_at

    def spy(img_, font_, c, scale):
        used.append(np.array(c))
        return orig(img_, font_, c, scale)

    monkeypatch.setattr(r, "_read_recovery_at", spy)
    read = r._reread_with_dynamic_lines(img, font, off, 2, first)
    assert len(used) == 1
    assert np.abs(used[0] - TRUE_CORNERS).max() <= 3.0
    assert read.mode == "recovery"
    assert read.ssid == ssid
    assert read.passphrases[0] == pw


def test_coarse_align_then_read_from_offset_start(font, monkeypatch):
    # A smaller grid than the shipped one keeps the test fast; the start is
    # off by more than refine_corners' basin alone.
    monkeypatch.setattr(r, "COARSE_SHIFT_PX", 72)
    monkeypatch.setattr(r, "COARSE_SCALES", (0.92, 1.0, 1.08))
    pw = "HJKLMNPQRSTU"
    img = render_screen(font, pw)
    centre = TRUE_CORNERS.mean(axis=0)
    start = centre + (TRUE_CORNERS - centre) * 1.06 + (-48.0, 40.0)
    monkeypatch.setattr(r, "_corner_starts", lambda _img: [start])
    read = r.read_recovery(img, font)
    assert read.ssid == SSID
    assert read.passphrases[0] == pw


def test_blank_frame_is_a_read_error(font, monkeypatch):
    monkeypatch.setattr(r, "COARSE_SHIFT_PX", 24)
    monkeypatch.setattr(r, "COARSE_SCALES", (1.0,))
    monkeypatch.setattr(r, "_corner_starts", lambda _img: [TRUE_CORNERS.copy()])
    img = np.full((FRAME_H, FRAME_W, 3), 40.0)
    with pytest.raises(r.ReadError):
        r.read_recovery(img, font)


def test_candidates_reject_unreadable_glyph():
    ranked = [[(r.MAX_GLYPH_DISTANCE + 1, "A"), (20.0, "B")]]
    with pytest.raises(r.ReadError):
        r.candidates(ranked)


def test_candidates_order_and_ambiguity():
    ranked = [[(0.5, "5"), (1.0, "S"), (9.0, "6")], [(0.0, "A"), (8.0, "B")]]
    assert r.candidates(ranked) == ["5A", "SA"]


def test_cli_never_prints_the_passphrase(font, tmp_path, capsys):
    from PIL import Image

    pw = "QRSTUVWXYZ23"
    img_path = tmp_path / "frame.png"
    Image.fromarray(render_screen(font, pw).astype(np.uint8)).save(img_path)
    cand = tmp_path / "cand.txt"
    rc_args = ["--image", str(img_path), "--candidates-out", str(cand)]

    # Pin the start to the true corners so the CLI path stays fast.
    orig = r.read_recovery
    rc = None
    try:
        r.read_recovery = lambda img: orig(img, corners=TRUE_CORNERS)
        rc = r.main(rc_args)
    finally:
        r.read_recovery = orig
    out = capsys.readouterr().out
    assert rc == 0
    assert pw not in out
    res = json.loads(out.strip().splitlines()[-1])
    assert res["ok"] and res["ssid"] == SSID and res["glyph_count"] == 12
    assert set(res) == {"mode", "ok", "ssid", "glyph_count", "candidate_count", "warnings"}
    assert cand.read_text(encoding="utf-8").splitlines()[0] == pw


def test_parse_wifi_qr():
    ssid, pw = r.parse_wifi_qr(r"WIFI:T:WPA;S:kiln\;ap;P:p\:w;;")
    assert ssid == "kiln;ap"
    assert pw == "p:w"


def test_failed_colour_detection_falls_back_to_bench_corners(monkeypatch):
    def dark(_img, step=2):
        raise r.ReadError("lit panel region too small")

    monkeypatch.setattr(r, "detect_panel", dark)
    starts = r._corner_starts(np.zeros((FRAME_H, FRAME_W, 3)))
    from kilnctrl.bench_test.lcd_sampler import FRAME_CORNERS
    assert len(starts) == 1
    assert np.allclose(starts[0], np.array(FRAME_CORNERS, dtype=np.float64))


def test_low_blue_margin_panel_is_detected(font):
    # White balance drift: the panel reads only b-r ~ 23, below the first margin.
    img = render_screen(font, "23456789ABCD").astype(np.float64)
    inside = img[..., 0] > 20
    img[..., 0] = np.where(inside, img[..., 0], 5.0)
    img[..., 2] = np.where(inside, img[..., 0] + 23.0, 5.0)
    img[..., 1] = np.where(inside, img[..., 0] + 6.0, 5.0)
    c = r.detect_panel(img)
    assert np.abs(c - TRUE_CORNERS).max() <= 6.0
