"""build_kilnfw() now builds SaftyFW first for a `build`/`reconfigure` target
(docs/PICO_AUTO_UPDATE_PLAN.md, 2026-09-20) since the KilnFW application
embeds SaftyFW's slot bins. This test proves the ordering and the abort
behavior WITHOUT running any real toolchain: `mcpkit.workbench.build_saftyfw`
and the ESP-IDF profile / `_run_locked` call are monkeypatched.
"""
from __future__ import annotations

import os

from mcpkit import workbench


def test_saftyfw_runs_before_kilnfw_and_both_reported(monkeypatch, tmp_path):
    calls = []

    def fake_build_saftyfw(jobs=0):
        calls.append("saftyfw")
        return "saftyfw: OK in 1.0s (3 log lines)\nfull log: x\n--\nall good"

    def fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append("kilnfw")
        return f"{tag}: OK in 1.0s (1 log lines)\nfull log: y\n--\nbuilt"

    monkeypatch.setattr(workbench, "build_saftyfw", fake_build_saftyfw)
    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)  # any existing file

    result = workbench.build_kilnfw(target="build")

    assert calls == ["saftyfw", "kilnfw"]
    assert "saftyfw: OK" in result
    assert "kilnfw-build: OK" in result


def test_saftyfw_failure_aborts_before_kilnfw_starts(monkeypatch):
    calls = []

    def fake_build_saftyfw(jobs=0):
        calls.append("saftyfw")
        return "saftyfw: FAILED (exit 1) in 1.0s (3 log lines)\nfull log: x\n--\nerror: bad"

    def fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append("kilnfw")
        return f"{tag}: OK"

    monkeypatch.setattr(workbench, "build_saftyfw", fake_build_saftyfw)
    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)

    result = workbench.build_kilnfw(target="build")

    assert calls == ["saftyfw"]  # kilnfw build never ran
    assert "ABORTED" in result
    assert "FAILED" in result


def test_skip_saftyfw_opts_out(monkeypatch):
    calls = []

    def fake_build_saftyfw(jobs=0):
        calls.append("saftyfw")
        return "saftyfw: OK"

    def fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append("kilnfw")
        return f"{tag}: OK"

    monkeypatch.setattr(workbench, "build_saftyfw", fake_build_saftyfw)
    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert calls == ["kilnfw"]  # SaftyFW build skipped entirely
    assert "kilnfw-build: OK" in result


def test_fullclean_target_does_not_build_saftyfw(monkeypatch):
    calls = []

    def fake_build_saftyfw(jobs=0):
        calls.append("saftyfw")
        return "saftyfw: OK"

    def fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append("kilnfw")
        return f"{tag}: OK"

    monkeypatch.setattr(workbench, "build_saftyfw", fake_build_saftyfw)
    monkeypatch.setattr(workbench, "_run_locked", fake_run_locked)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)

    workbench.build_kilnfw(target="fullclean")

    assert calls == ["kilnfw"]  # fullclean has no embed step to protect
