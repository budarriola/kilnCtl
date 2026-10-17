"""Mirror-drift guard: the web clients' TC sheath tau bounds must match the
firmware parser's.

setup_wizard_page.html (SHEATH_TAU_MIN_S / SHEATH_TAU_MAX_S) and
zones_page.html validate the per-zone protection-tube tau client side before
POST /api/zones. The firmware parser (zones_http_post_parse.c z%u_sheath_tau)
refuses anything outside ZONE_TC_SHEATH_TAU_MIN_S..MAX_S
(zones_config_accessors.h), and tc_lag_comp.h carries the same range as
TC_LAG_COMP_TAU_MIN_S/MAX_S. A client range wider than the firmware's gives a
confusing 400; a narrower one hides valid settings. All copies must agree.
"""
from __future__ import annotations

import re
from pathlib import Path

_REPO = Path(__file__).resolve().parents[3]
_DRV = _REPO / "firmware" / "KilnFW" / "App" / "drivers"


def _c_define(path: Path, name: str) -> float:
    text = path.read_text(encoding="utf-8")
    m = re.search(r"#define\s+" + name + r"\s+([0-9.]+)[uUfF]?\b", text)
    assert m, f"{name} not found in {path}"
    return float(m.group(1))


def _js_var(path: Path, name: str) -> float:
    text = path.read_text(encoding="utf-8")
    m = re.search(r"\bvar\s+" + name + r"\s*=\s*(-?[0-9.]+)\s*;", text)
    assert m, f"{name} not found in {path}"
    return float(m.group(1))


def _fw_bounds() -> tuple[float, float]:
    acc = _DRV / "persist" / "zones_config_accessors.h"
    return _c_define(acc, "ZONE_TC_SHEATH_TAU_MIN_S"), _c_define(acc, "ZONE_TC_SHEATH_TAU_MAX_S")


def test_parser_uses_accessor_bounds() -> None:
    text = (_DRV / "http" / "zones_http_post_parse.c").read_text(encoding="utf-8")
    assert re.search(
        r'"z%u_sheath_tau"[\s\S]{0,400}?ZONE_TC_SHEATH_TAU_MIN_S,\s*ZONE_TC_SHEATH_TAU_MAX_S', text
    ), "zones_http_post_parse.c z%u_sheath_tau no longer parses with ZONE_TC_SHEATH_TAU_MIN_S/MAX_S"


def test_estimator_bounds_match_accessor() -> None:
    lag = _DRV / "control" / "tc_lag_comp.h"
    assert (_c_define(lag, "TC_LAG_COMP_TAU_MIN_S"), _c_define(lag, "TC_LAG_COMP_TAU_MAX_S")) == _fw_bounds()


def test_wizard_bounds_match_firmware() -> None:
    page = _DRV / "http" / "setup_wizard_page.html"
    assert (_js_var(page, "SHEATH_TAU_MIN_S"), _js_var(page, "SHEATH_TAU_MAX_S")) == _fw_bounds()


def test_zones_page_bounds_match_firmware() -> None:
    text = (_DRV / "http" / "zones_page.html").read_text(encoding="utf-8")
    m = re.search(r'min="([0-9.]+)" max="([0-9.]+)" class="sheathtau"', text)
    assert m, "zones_page.html sheathtau input min/max not found"
    assert (float(m.group(1)), float(m.group(2))) == _fw_bounds()


# Pico tc_sheath_tau_s (0x010B, id 267): SaftyFW SET_PARAM refuses > CONFIG_PARAMS_TC_SHEATH_TAU_MAX_S.
def _pico_max() -> float:
    return _c_define(_REPO / "firmware" / "SaftyFW" / "src" / "config_params.h", "CONFIG_PARAMS_TC_SHEATH_TAU_MAX_S")


def test_pico_max_matches_estimator() -> None:
    assert _pico_max() == _c_define(_DRV / "control" / "tc_lag_comp.h", "TC_LAG_COMP_TAU_MAX_S")


def test_commissioning_page_pico_bounds() -> None:
    text = (_DRV / "http" / "safety_commissioning_page.html").read_text(encoding="utf-8")
    m = re.search(r"id: 267, name: 'tc_sheath_tau_s', type: 'u16'[^\n]*min: ([0-9.]+), max: ([0-9.]+)", text)
    assert m, "safety_commissioning_page.html id 267 tc_sheath_tau_s min/max not found"
    assert (float(m.group(1)), float(m.group(2))) == (0.0, _pico_max())


def test_wizard_step7_pico_bounds() -> None:
    text = (_DRV / "http" / "setup_wizard_page.html").read_text(encoding="utf-8")
    m = re.search(r"fields\.tcSheathTauS >= 1 && fields\.tcSheathTauS <= ([0-9]+)\)", text)
    assert m, "setup_wizard_page.html step 7 tube tau bound not found"
    assert float(m.group(1)) == _pico_max()
    m = re.search(r'id="s7TcTau" value="([0-9]+)"', text)
    assert m and float(m.group(1)) == 60.0, "tubed default is not 60 s"
