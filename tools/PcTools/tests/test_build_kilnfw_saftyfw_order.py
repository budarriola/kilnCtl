"""build_kilnfw() now builds SaftyFW first for a `build`/`reconfigure` target
(docs/PICO_AUTO_UPDATE.md, 2026-09-20) since the KilnFW application
embeds SaftyFW's slot bins. This test proves the ordering and the abort
behavior WITHOUT running any real toolchain: `mcpkit.workbench.build_saftyfw`
and the ESP-IDF profile / `_run_locked` call are monkeypatched.
"""
from __future__ import annotations

import os

from mcpkit import workbench


import pytest as _pytest

from _kilnfw_fakes import write_fresh_bins


@_pytest.fixture(autouse=True)
def _tmp_repo(monkeypatch, tmp_path):
    """Point repo_root() at a scratch tree so the REAL artifact gate runs against
    bins the fakes write there (no bypass)."""
    os.makedirs(tmp_path / "firmware" / "KilnFW" / "build")
    os.makedirs(tmp_path / "tools" / "PcTools")
    monkeypatch.setattr(workbench, "repo_root", lambda: str(tmp_path))


def _ok(tag, resource_key):
    """A successful kilnfw step: report OK and leave the build's artifacts."""
    if tag == "kilnfw-fullclean":
        for name in os.listdir(resource_key):
            os.remove(os.path.join(resource_key, name))
    elif tag == "kilnfw-build":
        write_fresh_bins(resource_key)
    return f"{tag}: OK in 1.0s (1 log lines)\nfull log: y\n--\nbuilt"


def test_saftyfw_runs_before_kilnfw_and_both_reported(monkeypatch, tmp_path):
    calls = []

    def fake_build_saftyfw(jobs=0, saftyfw_root=None):
        calls.append("saftyfw")
        return "saftyfw: OK in 1.0s (3 log lines)\nfull log: x\n--\nall good"

    def fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append("kilnfw")
        return _ok(tag, resource_key)

    monkeypatch.setattr(workbench, "build_saftyfw", fake_build_saftyfw)
    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)  # any existing file

    result = workbench.build_kilnfw(target="build")

    assert calls == ["saftyfw", "kilnfw"]
    assert "saftyfw: OK" in result
    assert "kilnfw-build: OK" in result


def test_saftyfw_failure_aborts_before_kilnfw_starts(monkeypatch):
    calls = []

    def fake_build_saftyfw(jobs=0, saftyfw_root=None):
        calls.append("saftyfw")
        return "saftyfw: FAILED (exit 1) in 1.0s (3 log lines)\nfull log: x\n--\nerror: bad"

    def fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append("kilnfw")
        return _ok(tag, resource_key)

    monkeypatch.setattr(workbench, "build_saftyfw", fake_build_saftyfw)
    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)

    result = workbench.build_kilnfw(target="build")

    assert calls == ["saftyfw"]  # kilnfw build never ran
    assert "ABORTED" in result
    assert "FAILED" in result


def test_skip_saftyfw_opts_out(monkeypatch):
    calls = []

    def fake_build_saftyfw(jobs=0, saftyfw_root=None):
        calls.append("saftyfw")
        return "saftyfw: OK"

    def fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append("kilnfw")
        return _ok(tag, resource_key)

    monkeypatch.setattr(workbench, "build_saftyfw", fake_build_saftyfw)
    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert calls == ["kilnfw"]  # SaftyFW build skipped entirely
    assert "kilnfw-build: OK" in result


def test_fullclean_target_does_not_build_saftyfw(monkeypatch):
    calls = []

    def fake_build_saftyfw(jobs=0, saftyfw_root=None):
        calls.append("saftyfw")
        return "saftyfw: OK"

    def fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append("kilnfw")
        return _ok(tag, resource_key)

    monkeypatch.setattr(workbench, "build_saftyfw", fake_build_saftyfw)
    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)

    workbench.build_kilnfw(target="fullclean")

    assert calls == ["kilnfw"]  # fullclean has no embed step to protect


def _write_cfg(tmp_path, defaults, generated):
    d = tmp_path / "firmware" / "KilnFW"
    (d / "sdkconfig.defaults").write_text(defaults, encoding="utf-8")
    if generated is not None:
        (d / "sdkconfig").write_text(generated, encoding="utf-8")


_PINS = "CONFIG_SPIRAM_XIP_FROM_PSRAM=y\n# CONFIG_UART_ISR_IN_IRAM is not set\n"


def test_stale_sdkconfig_pin_mismatch_refuses_build_before_anything_runs(monkeypatch, tmp_path):
    """REVIEW_XIPLAND MED-1: build_kilnfw must not build against a sdkconfig that disagrees
    with a pinned sdkconfig.defaults key (ESP-IDF keeps the generated value)."""
    _write_cfg(tmp_path, _PINS, "# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set\n# CONFIG_UART_ISR_IN_IRAM is not set\n")
    calls = []
    monkeypatch.setattr(workbench, "build_saftyfw", lambda *a, **k: calls.append("s") or "saftyfw: OK")
    monkeypatch.setattr(workbench, "_run_locked", lambda *a, **k: calls.append("k") or "kilnfw-build: OK")
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)
    out = workbench.build_kilnfw(target="build")
    assert "REFUSED" in out and "CONFIG_SPIRAM_XIP_FROM_PSRAM" in out
    assert "CONFIG_UART_ISR_IN_IRAM" not in out
    assert calls == []


def test_missing_pinned_key_in_generated_sdkconfig_refuses(tmp_path):
    _write_cfg(tmp_path, _PINS, "CONFIG_FOO=1\n")
    bad = workbench.sdkconfig_pin_mismatches(str(tmp_path / "firmware" / "KilnFW"))
    assert len(bad) == 2


def test_matching_or_absent_sdkconfig_is_not_a_mismatch(tmp_path):
    kd = str(tmp_path / "firmware" / "KilnFW")
    _write_cfg(tmp_path, _PINS, None)
    assert workbench.sdkconfig_pin_mismatches(kd) == []
    _write_cfg(tmp_path, _PINS, "CONFIG_SPIRAM_XIP_FROM_PSRAM=y\n# CONFIG_UART_ISR_IN_IRAM is not set\nCONFIG_X=2\n")
    assert workbench.sdkconfig_pin_mismatches(kd) == []


def test_pin_mismatch_does_not_block_reconfigure(monkeypatch, tmp_path):
    """reconfigure is the remedy, so it must stay reachable."""
    _write_cfg(tmp_path, _PINS, "# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set\n")
    monkeypatch.setattr(workbench, "build_saftyfw", lambda *a, **k: "saftyfw: OK")
    monkeypatch.setattr(workbench, "_run_locked", lambda tag, rk, argv, **k: f"{tag}: OK in 1s")
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)
    out = workbench.build_kilnfw(target="reconfigure")
    assert "REFUSED" not in out
