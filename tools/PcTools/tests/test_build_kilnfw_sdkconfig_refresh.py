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

2026-09-23 follow-up (opus review): the blind refresh above has its own
narrow window. `check_00_kilnfw_target_build.ps1` publishes the checkbuild
worktree's sdkconfig as the provenance record of the ELF it published. If
the live sdkconfig is then edited and `build_kilnfw(jobs=N)` (the
`ninja -j N` path) is called but ninja does NOT relink -- which it is not
guaranteed to do off an sdkconfig-only change -- a blind copy would
overwrite the correct published sibling with a config that never produced
the ELF still on disk, silencing the sibling-agreement guard instead of
tripping it. `_refresh_build_sdkconfig` now only copies when the live
config already matches the sibling, or when the ELF actually changed
(mtime/size) during this build; otherwise it skips and says so by name.

Both the real toolchain call (`_run_locked`) and SaftyFW's own build are
monkeypatched here -- this test proves only the refresh behavior, not that
a real ESP-IDF build succeeded.
"""
from __future__ import annotations

import os

from mcpkit import workbench


def _fake_ok_run_locked_relinks_elf(tag, resource_key, argv, **kwargs):
    """Simulates a build that actually relinks build/KilnCtrl.elf."""
    elf = os.path.join(resource_key, "KilnCtrl.elf")
    with open(elf, "ab") as f:
        f.write(b"x")
    return f"{tag}: OK in 1.0s (1 log lines)\nfull log: y\n--\nbuilt"


def _fake_ok_run_locked_no_relink(tag, resource_key, argv, **kwargs):
    """Simulates a build where ninja decided nothing needed relinking."""
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
    # No ELF exists before the build; the fake run_locked creates one, so the
    # ELF demonstrably changed (None -> present) and the refresh proceeds.
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked_relinks_elf)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    live.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")
    sibling.write_text("# CONFIG_KILNCTL_GPIO_PROBE is not set\n", encoding="utf-8")

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "sdkconfig-refresh: OK" in result
    assert sibling.read_text(encoding="utf-8") == live.read_text(encoding="utf-8")


def test_no_live_config_skips_without_failing_the_build_report(monkeypatch, tmp_path):
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked_relinks_elf)
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
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked_relinks_elf)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    live.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")
    sibling.write_text("# CONFIG_KILNCTL_GPIO_PROBE is not set\n", encoding="utf-8")

    result = workbench.build_kilnfw(target="fullclean", skip_saftyfw=True)

    assert "sdkconfig-refresh" not in result
    assert sibling.read_text(encoding="utf-8") == "# CONFIG_KILNCTL_GPIO_PROBE is not set\n"


def test_live_differs_and_elf_unchanged_skips_refresh(monkeypatch, tmp_path):
    """The narrow window: ninja did not relink, so the published sibling is
    still the correct provenance record for the ELF on disk. A blind copy
    here would silence check_all_task_stack_budgets.py's sibling-agreement
    guard instead of leaving it free to catch a genuine drift.
    """
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked_no_relink)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    elf = root / "firmware" / "KilnFW" / "build" / "KilnCtrl.elf"
    elf.write_bytes(b"unchanged-elf-bytes")
    live.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")
    sibling.write_text("# CONFIG_KILNCTL_GPIO_PROBE is not set\n", encoding="utf-8")

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "sdkconfig-refresh: SKIPPED" in result
    assert "did not change during this build" in result
    assert str(live) in result
    assert str(sibling) in result
    # The published sibling -- still the correct provenance record for the
    # ELF that is actually on disk -- must be left untouched.
    assert sibling.read_text(encoding="utf-8") == "# CONFIG_KILNCTL_GPIO_PROBE is not set\n"


def test_live_differs_and_elf_changed_refreshes(monkeypatch, tmp_path):
    """When the ELF actually changed, the build really did relink from the
    live config, so the sibling must be brought into agreement with it.
    """
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked_relinks_elf)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    elf = root / "firmware" / "KilnFW" / "build" / "KilnCtrl.elf"
    elf.write_bytes(b"stale-elf-bytes")
    live.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")
    sibling.write_text("# CONFIG_KILNCTL_GPIO_PROBE is not set\n", encoding="utf-8")

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "sdkconfig-refresh: OK" in result
    assert sibling.read_text(encoding="utf-8") == live.read_text(encoding="utf-8")


def test_live_identical_and_elf_unchanged_is_a_harmless_noop(monkeypatch, tmp_path):
    """Nothing changed anywhere; the report must not claim a refresh happened
    when there was nothing to refresh (the sibling already agreed).
    """
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked_no_relink)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    elf = root / "firmware" / "KilnFW" / "build" / "KilnCtrl.elf"
    elf.write_bytes(b"unchanged-elf-bytes")
    same_text = "CONFIG_KILNCTL_GPIO_PROBE=y\n"
    live.write_text(same_text, encoding="utf-8")
    sibling.write_text(same_text, encoding="utf-8")

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "SKIPPED -- " not in result or "did not change during this build" not in result
    assert sibling.read_text(encoding="utf-8") == same_text


def test_missing_sibling_and_elf_unchanged_creates_it(monkeypatch, tmp_path):
    """Opus review advisory 1: with no published sibling yet, there is nothing
    for a "differs from <sibling>" message to name -- this must be reported
    as a plain first-time copy, never a SKIP naming a nonexistent file.
    """
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked_no_relink)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    elf = root / "firmware" / "KilnFW" / "build" / "KilnCtrl.elf"
    elf.write_bytes(b"unchanged-elf-bytes")
    live.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")
    assert not sibling.exists()

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "sdkconfig-refresh: OK" in result
    assert "SKIPPED" not in result
    assert sibling.read_text(encoding="utf-8") == live.read_text(encoding="utf-8")


def test_comment_only_difference_and_elf_unchanged_still_refreshes(monkeypatch, tmp_path):
    """Opus review advisory 2: the sibling comparison must match
    check_all_task_stack_budgets.py's `_check_sibling_pair_agreement`, which
    compares parsed CONFIG_ lines, not raw bytes. A comment-only (or
    line-ending-only) difference is not a real disagreement and must not
    produce an alarming SKIP.
    """
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked_no_relink)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    elf = root / "firmware" / "KilnFW" / "build" / "KilnCtrl.elf"
    elf.write_bytes(b"unchanged-elf-bytes")
    live.write_text(
        "# a harmless comment that was not here before\n"
        "CONFIG_KILNCTL_GPIO_PROBE=y\n",
        encoding="utf-8",
    )
    sibling.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "sdkconfig-refresh: OK" in result
    assert "SKIPPED" not in result


def test_real_config_line_difference_and_elf_unchanged_still_skips(monkeypatch, tmp_path):
    """Guard against overcorrecting: a genuine CONFIG_ symbol disagreement
    with an unchanged ELF must still SKIP -- this is the same case
    `test_live_differs_and_elf_unchanged_skips_refresh` covers, re-asserted
    here after switching the comparison from raw bytes to parsed CONFIG_
    lines, to prove that switch didn't quietly widen what counts as "same".
    """
    root = _stub_common(monkeypatch, tmp_path, _fake_ok_run_locked_no_relink)
    live = root / "firmware" / "KilnFW" / "sdkconfig"
    sibling = root / "firmware" / "KilnFW" / "build" / "sdkconfig"
    elf = root / "firmware" / "KilnFW" / "build" / "KilnCtrl.elf"
    elf.write_bytes(b"unchanged-elf-bytes")
    live.write_text("CONFIG_KILNCTL_GPIO_PROBE=y\n", encoding="utf-8")
    sibling.write_text("CONFIG_KILNCTL_GPIO_PROBE=n\n", encoding="utf-8")

    result = workbench.build_kilnfw(target="build", skip_saftyfw=True)

    assert "sdkconfig-refresh: SKIPPED" in result
    assert "did not change during this build" in result
    assert sibling.read_text(encoding="utf-8") == "CONFIG_KILNCTL_GPIO_PROBE=n\n"
