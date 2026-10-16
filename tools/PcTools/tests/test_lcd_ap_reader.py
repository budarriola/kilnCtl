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


def _draw_qr(panel, payload, module=3, quiet=4, area=124):
    """recovery_wifi_qr.c layout: light quiet zone, top-right of the panel."""
    import zxingcpp

    bc = zxingcpp.create_barcode(payload, zxingcpp.BarcodeFormat.QRCode, ec_level="L")
    mods = np.asarray(zxingcpp.write_barcode_to_image(bc, add_quiet_zones=False)) > 127  # True = light
    n = mods.shape[0]
    full = np.ones((n + 2 * quiet, n + 2 * quiet), dtype=bool)
    full[quiet:quiet + n, quiet:quiet + n] = mods
    px = np.kron(full, np.ones((module, module))).astype(float)
    ext = px.shape[0]
    assert ext <= area
    panel[0:ext, r.PANEL_W - ext:r.PANEL_W] = px


def wifi_payload(ssid, passphrase):
    return f"WIFI:T:WPA;S:{ssid};P:{passphrase};;"


def render_screen(font, passphrase, ssid=SSID, corners=TRUE_CORNERS, qr=None):
    """RGB camera frame of the recovery screen with the panel at ``corners``.

    ``qr`` is the text of a QR drawn top-right as the recovery firmware does.
    """
    panel = np.zeros((r.PANEL_H, r.PANEL_W))
    for y, s, text in r.ANCHOR_LINES:
        _draw(panel, font, y, s, text)
    _draw(panel, font, r.Y_SSID, 3 if len(ssid) <= 26 else 2, ssid)
    _draw(panel, font, r.Y_PASS, r.PASS_SCALE, passphrase)
    if qr is not None:
        _draw_qr(panel, qr)
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
        r.read_recovery = lambda img, font=None: orig(img, font, corners=TRUE_CORNERS)
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


# --- QR-first recovery read (recovery_wifi_qr.c draws the QR) -----------------

def _qr_read(font, pw, qr, ssid=SSID, monkeypatch=None):
    """read_recovery_auto on a frame; glyph reader pinned to the true corners."""
    img = render_screen(font, pw, ssid=ssid, qr=qr)
    orig = r.read_recovery
    r.read_recovery = lambda im, f=None: orig(im, f, corners=TRUE_CORNERS)
    try:
        return r.read_recovery_auto(img, font)
    finally:
        r.read_recovery = orig


def test_recovery_prefers_the_qr(font, monkeypatch):
    pw = "23456789ABCD"

    def no_glyphs(*_a, **_k):
        raise AssertionError("glyph segmentation ran although the QR decoded")

    img = render_screen(font, pw, qr=wifi_payload(SSID, pw))
    monkeypatch.setattr(r, "read_recovery", no_glyphs)
    read = r.read_recovery_auto(img, font)
    assert read.ssid == SSID
    assert read.passphrases == [pw]
    assert read.glyph_count == r.PASS_LEN
    assert read.warnings == []


def test_recovery_qr_resolves_the_5_s_ambiguity(font):
    # The glyph reader would offer both spellings; the QR yields exactly one.
    pw = "5S5S5S5S5S5S"
    read = _qr_read(font, pw, wifi_payload(SSID, pw))
    assert read.passphrases == [pw]


def test_recovery_without_qr_falls_back_to_glyphs(font):
    pw = "23456789ABCD"
    read = _qr_read(font, pw, None)
    assert read.passphrases[0] == pw
    assert read.ssid == SSID
    assert len(read.warnings) == 1 and "glyph segmentation" in read.warnings[0]
    assert pw not in read.warnings[0]


def test_recovery_bad_qr_passphrase_falls_back_to_glyphs(font):
    pw = "23456789ABCD"
    # QR present but its passphrase is not a recovery passphrase (length, alphabet).
    for bad in ("short", "0OIl0OIl0OIl", pw + "E", pw[:11]):
        read = _qr_read(font, pw, wifi_payload(SSID, bad))
        assert read.passphrases[0] == pw, bad
        assert read.warnings and "glyph segmentation" in read.warnings[0]
        assert bad not in read.warnings[0]


def test_recovery_non_wifi_qr_falls_back(font):
    pw = "23456789ABCD"
    read = _qr_read(font, pw, "https://example.invalid/x")
    assert read.passphrases[0] == pw and read.warnings


def test_recovery_qr_with_escaped_ssid(font):
    pw = "HJKLMNPQRSTU"
    ssid = r"kiln;ap"
    payload = r"WIFI:T:WPA;S:kiln\;ap;P:" + pw + ";;"
    img = render_screen(font, pw, qr=payload)
    read = r.read_recovery_qr(img)
    assert read.ssid == ssid and read.passphrases == [pw]


ESCAPED_NORMAL_QR = "WIFI:T:WPA;S:kiln-ap;P:any pass" + "\\:word\\;with\\,specials;;"


def test_normal_mode_reads_the_qr_only(font, tmp_path):
    from PIL import Image

    pw = "any pass:word;with,specials"
    img = render_screen(font, "23456789ABCD",
                        qr=ESCAPED_NORMAL_QR)
    path = tmp_path / "frame.png"
    Image.fromarray(img.astype(np.uint8)).save(path)
    read = r.read_normal(str(path))
    assert read.mode == "normal" and read.ssid == "kiln-ap"
    assert read.passphrases == [pw]
    # No QR: normal mode never falls back to OCR.
    Image.fromarray(render_screen(font, "23456789ABCD").astype(np.uint8)).save(path)
    with pytest.raises(r.ReadError):
        r.read_normal(str(path))


def test_cli_recovery_qr_never_prints_the_passphrase(font, tmp_path, capsys):
    from PIL import Image

    pw = "QRSTUVWXYZ23"
    path = tmp_path / "frame.png"
    Image.fromarray(render_screen(font, pw, qr=wifi_payload(SSID, pw)).astype(np.uint8)).save(path)
    cand = tmp_path / "cand.txt"
    rc = r.main(["--image", str(path), "--candidates-out", str(cand)])
    out = capsys.readouterr().out
    assert rc == 0 and pw not in out
    res = json.loads(out.strip().splitlines()[-1])
    assert res["ok"] and res["candidate_count"] == 1 and res["warnings"] == []
    assert cand.read_text(encoding="utf-8").splitlines() == [pw]

class _Hit:
    def __init__(self, text):
        self.text = text


def test_wifi_payloads_upscales_when_raw_frame_has_no_code():
    from PIL import Image

    sizes = []

    def fake_reader(img):
        sizes.append(img.size)
        # Decodes only once the frame has been enlarged at least 3x.
        return [_Hit("WIFI:T:WPA;S:x;P:y;;")] if img.size[0] >= 300 else []

    im = Image.new("RGB", (100, 80))
    assert r.wifi_payloads(fake_reader, im) == ["WIFI:T:WPA;S:x;P:y;;"]
    assert sizes[0] == (100, 80)
    assert max(sizes) == (300, 240)


def test_wifi_payloads_ignores_non_wifi_and_gives_up():
    from PIL import Image

    def fake_reader(img):
        return [_Hit("https://example.com")]

    assert r.wifi_payloads(fake_reader, Image.new("RGB", (50, 50))) == []


def test_join_script_retries_live_reads_with_fresh_capture():
    from pathlib import Path

    text = (Path(r.__file__).parents[2] / "scripts" / "join_board_ap.ps1").read_text(encoding="utf-8")
    assert "[Math]::Max(1, $ReadAttempts)" in text
    assert "for ($try = 1; $try -le $attempts; $try++)" in text
    # The capture sits inside the retry loop, not before it.
    assert text.index("for ($try") < text.index("capture_lcd.ps1')")


def _pin_payloads(monkeypatch, *payloads):
    monkeypatch.setattr(r, "_wifi_qr_payloads", lambda img, region=None: list(payloads))


def test_recovery_qr_two_distinct_codes_refused(monkeypatch):
    pw = "23456789ABCD"
    _pin_payloads(monkeypatch, wifi_payload(SSID, pw), wifi_payload("other-ap", "HJKLMNPQRSTU"))
    with pytest.raises(r.ReadError, match="more than one"):
        r.read_recovery_qr(np.zeros((4, 4, 3)))


def test_recovery_qr_same_code_twice_is_one_code(monkeypatch):
    pw = "23456789ABCD"
    _pin_payloads(monkeypatch, wifi_payload(SSID, pw), wifi_payload(SSID, pw))
    assert r.read_recovery_qr(np.zeros((4, 4, 3))).passphrases == [pw]


def test_recovery_qr_empty_or_overlong_ssid_refused(monkeypatch):
    pw = "23456789ABCD"
    for ssid in ("", "x" * 33):
        _pin_payloads(monkeypatch, wifi_payload(ssid, pw))
        with pytest.raises(r.ReadError, match="SSID"):
            r.read_recovery_qr(np.zeros((4, 4, 3)))
    _pin_payloads(monkeypatch, wifi_payload("x" * 32, pw))
    assert r.read_recovery_qr(np.zeros((4, 4, 3))).ssid == "x" * 32


class _Pt:
    def __init__(self, x, y):
        self.x, self.y = x, y


class _PosHit:
    def __init__(self, text, cx, cy):
        self.text = text
        d = 5
        self.position = type("P", (), {
            "top_left": _Pt(cx - d, cy - d), "top_right": _Pt(cx + d, cy - d),
            "bottom_right": _Pt(cx + d, cy + d), "bottom_left": _Pt(cx - d, cy + d)})()


def test_wifi_payloads_region_drops_a_foreign_code():
    from PIL import Image

    inside = _PosHit("WIFI:T:WPA;S:a;P:b;;", 900, 100)    # top-right of 1000x800
    outside = _PosHit("WIFI:T:WPA;S:evil;P:c;;", 100, 700)  # bottom-left

    def reader(img):
        return [inside, outside]

    im = Image.new("RGB", (1000, 800))
    assert r.wifi_payloads(reader, im, r.RECOVERY_QR_REGION) == ["WIFI:T:WPA;S:a;P:b;;"]
    assert len(r.wifi_payloads(reader, im)) == 2  # no region: unfiltered


def test_wifi_payloads_region_scales_upscaled_coordinates():
    from PIL import Image

    def reader(img):
        if img.size[0] < 200:
            return []
        k = img.size[0] / 100.0  # symbol at (80,10) of the original frame
        return [_PosHit("WIFI:T:WPA;S:a;P:b;;", 80 * k, 10 * k)]

    im = Image.new("RGB", (100, 80))
    assert r.wifi_payloads(reader, im, r.RECOVERY_QR_REGION) == ["WIFI:T:WPA;S:a;P:b;;"]


def test_zxing_dependency_floor_covers_the_writer_api():
    from pathlib import Path

    text = (Path(__file__).resolve().parents[1] / "pyproject.toml").read_text(encoding="utf-8")
    import re as _re

    m = _re.search(r'"zxing-cpp>=(\d+)\.(\d+)', text)
    assert m and (int(m.group(1)), int(m.group(2))) >= (3, 0)


# --- recovery QR region filter, end to end through the real decoder ------------

def _paste_decoy(img, payload, x0, y0, module=3, quiet=4):
    """Paste a QR (light quiet zone) with its top-left at frame (x0, y0)."""
    import zxingcpp

    bc = zxingcpp.create_barcode(payload, zxingcpp.BarcodeFormat.QRCode, ec_level="L")
    mods = np.asarray(zxingcpp.write_barcode_to_image(bc, add_quiet_zones=False)) > 127
    n = mods.shape[0]
    full = np.ones((n + 2 * quiet, n + 2 * quiet), dtype=bool)
    full[quiet:quiet + n, quiet:quiet + n] = mods
    px = np.kron(full, np.ones((module, module)))
    h, w = px.shape
    out = img.copy()
    out[y0:y0 + h, x0:x0 + w, :] = np.where(px > 0, 225.0, 30.0)[:, :, None]
    return out, w, h


def _blank_frame():
    return np.full((FRAME_H, FRAME_W, 3), 5.0)


def test_region_ignores_a_wifi_code_outside_the_quarter():
    pw = "23456789ABCD"
    payload = wifi_payload(SSID, pw)
    # lower-left, upper-left and lower-right are all outside (0.5,0,1,0.5).
    for x0, y0 in ((100, 500), (100, 20), (1000, 500)):
        img, _w, _h = _paste_decoy(_blank_frame(), payload, x0, y0)
        with pytest.raises(r.ReadError):
            r.read_recovery_qr(img)
    img, _w, _h = _paste_decoy(_blank_frame(), payload, 900, 20)
    assert r.read_recovery_qr(img).passphrases == [pw]


def test_region_y_bound_is_half_the_frame_height():
    pw = "23456789ABCD"
    payload = wifi_payload(SSID, pw)
    _img, _w, h = _paste_decoy(_blank_frame(), payload, 900, 0)
    ok_y = int(FRAME_H * 0.5 - h / 2) - 20      # symbol centre well above 0.5*H
    bad_y = int(FRAME_H * 0.5 - h / 2) + 20     # symbol centre well below 0.5*H
    img, _w, _h = _paste_decoy(_blank_frame(), payload, 900, ok_y)
    assert r.read_recovery_qr(img).passphrases == [pw]
    img, _w, _h = _paste_decoy(_blank_frame(), payload, 900, bad_y)
    with pytest.raises(r.ReadError):
        r.read_recovery_qr(img)


def test_region_applies_to_upscaled_retries():
    pw = "23456789ABCD"
    payload = wifi_payload(SSID, pw)
    # 1 px modules: too small for the raw frame, found only after an upscale.
    inside, _w, _h = _paste_decoy(_blank_frame(), payload, 900, 20, module=1)
    outside, _w, _h = _paste_decoy(_blank_frame(), payload, 100, 500, module=1)
    assert r.read_recovery_qr(inside).passphrases == [pw]
    with pytest.raises(r.ReadError):
        r.read_recovery_qr(outside)
