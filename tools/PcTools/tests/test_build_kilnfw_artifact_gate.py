"""build_kilnfw must judge success by exit code AND fresh artifacts, and every
idf.py child must run without Git Bash's MSYSTEM (2026-10-11: two false OKs).
A fake idf.py (a python script) stands in for the toolchain."""
from __future__ import annotations

import contextlib
import os
import sys
import time

import pytest

from mcpkit import workbench

BINS = (("KilnCtrl.bin",), ("bootloader", "bootloader.bin"),
        ("partition_table", "partition-table.bin"))

FAKE_IDF = r"""
import os, sys
mode = sys.argv[1]
build = sys.argv[2]
if mode in ("msys", "bins"):
    for parts in (("KilnCtrl.bin",), ("bootloader", "bootloader.bin"),
                  ("partition_table", "partition-table.bin")):
        p = os.path.join(build, *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(b"x")
if mode == "msys":
    print("MSys/Mingw is no longer supported"); sys.exit(0)
if mode == "nobin":
    print("done"); sys.exit(0)
if mode == "bins":
    print("built"); sys.exit(0)
if mode == "env":
    print("MSYSTEM=" + repr(os.environ.get("MSYSTEM"))); sys.exit(0)
"""


@pytest.fixture
def harness(monkeypatch, tmp_path):
    fw = tmp_path / "firmware" / "KilnFW"
    (tmp_path / "firmware" / "SaftyFW").mkdir(parents=True)
    fw.mkdir()
    (fw / "CMakeLists.txt").write_text("")
    (tmp_path / "firmware" / "SaftyFW" / "CMakeLists.txt").write_text("")
    (fw / "sdkconfig").write_text("")
    script = tmp_path / "fake_idf.py"
    script.write_text(FAKE_IDF)
    build = str(fw / "build")
    os.makedirs(build)
    state = {"mode": "nobin", "env": None}

    def fake_run_locked(tag, key, argv, **kwargs):
        with contextlib.nullcontext():
            return workbench._run(tag, [sys.executable, str(script), state["mode"], build],
                                  env=kwargs.get("env"))

    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)
    return fw, build, state


def _build(fw):
    return workbench.build_kilnfw(target="build", skip_saftyfw=True, kiln_fw_root=str(fw))


def test_exit0_but_no_bin_is_failed(harness):
    fw, build, state = harness
    state["mode"] = "nobin"
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "missing" in out


def test_msys_message_is_failed(harness):
    fw, build, state = harness
    state["mode"] = "msys"
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "MSys/Mingw" in out


def test_stale_bins_are_failed(harness):
    fw, build, state = harness
    state["mode"] = "nobin"
    old = time.time() - 3600
    for parts in BINS:
        p = os.path.join(build, *parts)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        open(p, "wb").write(b"x")
        os.utime(p, (old, old))
    out = _build(fw)
    assert "kilnfw-build: FAILED" in out and "stale" in out


def test_fresh_bins_are_ok(harness):
    fw, build, state = harness
    state["mode"] = "bins"
    out = _build(fw)
    assert "kilnfw-build: OK" in out


def test_child_env_has_no_msystem(monkeypatch):
    monkeypatch.setenv("MSYSTEM", "MINGW64")
    monkeypatch.setenv("MSYSTEM_PREFIX", "/mingw64")
    # Gated path: kiln_build_gate and build_lock are bypassed, env goes through
    # _run_locked exactly as build_kilnfw passes it.
    monkeypatch.setattr(workbench, "kiln_build_gate", lambda *a, **k: contextlib.nullcontext())
    monkeypatch.setattr(workbench, "build_lock", lambda *a, **k: contextlib.nullcontext())
    out = workbench._run_locked(
        "envprobe", "k", [sys.executable, "-c",
                          "import os;print('MSYSTEM=%r'%os.environ.get('MSYSTEM'),"
                          "'PFX=%r'%os.environ.get('MSYSTEM_PREFIX'))"],
        gate_label="envprobe")
    assert "MSYSTEM=None" in out and "PFX=None" in out
