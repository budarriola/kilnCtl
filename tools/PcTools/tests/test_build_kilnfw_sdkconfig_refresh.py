"""build_kilnfw() must not leave build/sdkconfig describing an older build
than the ELF it sits beside.

Context (2026-09-23): check_00_kilnfw_target_build.ps1 publishes an isolated
checkbuild's sdkconfig into the invoking tree's build/ next to the ELF it
also publishes there. A later plain `idf.py build` in that same build/
relinks the ELF from the tree's own firmware/KilnFW/sdkconfig but never
touched that published sibling -- so a subsequent
check_all_task_stack_budgets.py run could grade the newer ELF against a
stale, disagreeing config from an earlier, different build (a "reset one
side of a pair" bug; see CLAUDE.md). `build_kilnfw()` now refreshes
build/sdkconfig from the tree's live config after every successful
build/reconfigure so the pair never has a chance to drift.

Both the real toolchain call (`_run_locked`) and SaftyFW's own build are
monkeypatched here -- this test proves only the refresh behavior, not that
a real ESP-IDF build succeeded.
"""
from __future__ import annotations

import os

from mcpkit import workbench


def _fake_ok_run_locked(tag, resource_key, argv, **kwargs):
    return f"{tag}: OK in 1.0s (1 log lines)\nfull log: y\n--\nbuilt"


def _fake_failed_run_locked(tag, resource_key, argv, **kwargs):
    return f"{tag}: FAILED (exit 1) in 1.0s (1 log lines)\nfull log: y\n--\nlink error"


def _stub_common(monkeypatch, tmp_path, run_locked_fn):
    root = tmp_path
    os.makedirs(root / "firmware" / "KilnFW" / "build", exist_ok=True)
    os.makedirs(root / "firmware" / "SaftyFW", exist_ok=True)
    os.makedirs(root / "tools" / "PcTools", exist_ok=True)  # repo_root() landmark
    monkeypatch.setattr(workbench, "repo_root", lambda: str(root))
    monkeypatch.setattr(workbench, "_IDF_PROFILE", __file__)  # any existing file
    monkeypatch.setattr(workbench, "_run_locked", run_locked_fn)
    monkeypatch.setattr(workbench, "build_saftyfw", lambda jobs=0: "saftyfw: OK")
    return root


def test_successful_build_refreshes_stale_sibling(monkeypatch, tmp_path):
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    live.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")
    sibling.write_text("# CONFIG_KILNCTL_GPIO_PROBE is not set\n", encoding="utf-8")

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "sdkconfig-refresh: OK" in result
    assert sibling.read_text(encoding="utf-8") == live.read_text(encoding="utf-8")


def test_no_live_config_skips_without_failing_the_build_report(monkeypatch, tmp_path):
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked)
    # No firmware/KilnFW/sdkconfig at all.
    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "kilnfw-build: OK" in result
    assert "sdkconfig-refresh: SKIPPED" in result


def test_failed_build_does_not_touch_sibling(monkeypatch, tmp_path):
    root = _stub_common(monkeypatch, tmp_path, _fake_failed_run_locked)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    live.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")
    sibling.write_text("# CONFIG_KILNCTL_GPIO_PROBE is not set\n", encoding="utf-8")

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "sdkconfig-refresh" not in result
    # A failed build must never overwrite the sibling with a config that may
    # not even match what (partially) built.
    assert sibling.read_text(encoding="utf-8") == "# CONFIG_KILNCTL_GPIO_PROBE is not set\n"


def test_fullclean_target_does_not_refresh(monkeypatch, tmp_path):
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    live.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")
    sibling.write_text("# CONFIG_KILNCTL_GPIO_PROBE is not set\n", encoding="utf-8")

    result = workbench.build_kilnfw(target="fullclean", skip_saftyfw=True)

    assert "sdkconfig-refresh" not in result
    assert sibling.read_text(encoding="utf-8") == "# CONFIG_KILNCTL_GPIO_PROBE is not set\n"
