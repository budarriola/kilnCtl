"""Tests for build_kilnfw's kiln_fw_root "needs configure" gate.

Opus review of 171cc5bc found that testing build/sdkconfig (which a normal
``idf.py build`` never writes) instead of the root sdkconfig made
``idf.py set-target`` run on EVERY kiln_fw_root build -- clearing the build
directory and regenerating the root sdkconfig from defaults each time,
silently discarding the caller's config. These tests lock down the fixed
condition: set-target runs only when the root sdkconfig is absent (and
build/CMakeCache.txt is also absent), never for a fullclean target.

Everything that would touch a real subprocess or the build gate is
monkeypatched: no idf.py, ninja, or SDK path resolution actually runs.
"""

import os

import mcpkit.workbench as workbench


def _make_root(tmp_path, *, root_sdkconfig=False, cmake_cache=False, lvgl=True):
    root = tmp_path / "KilnFW"
    root.mkdir()
    (root / "CMakeLists.txt").write_text("# fake\n")
    saftyfw = tmp_path / "SaftyFW"
    saftyfw.mkdir()
    (saftyfw / "CMakeLists.txt").write_text("# fake\n")
    if lvgl:
        lvgl_dir = root / "components" / "lvgl"
        lvgl_dir.mkdir(parents=True)
        (lvgl_dir / "CMakeLists.txt").write_text("# fake\n")
    build_dir = root / "build"
    build_dir.mkdir()
    if root_sdkconfig:
        (root / "sdkconfig").write_text("# fake\n")
    if cmake_cache:
        (build_dir / "CMakeCache.txt").write_text("# fake\n")
    return root


def _patch_common(monkeypatch, tmp_path, calls):
    def _fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append(tag)
        return f"{tag}: OK in 0.1s (0 log lines)\nfull log: -\n--\n(no output)"

    monkeypatch.setattr(workbench, "_run_locked", _fake_run_locked)
    monkeypatch.setattr(workbench, "_IDF_PROFILE", str(tmp_path / "profile.ps1"))
    (tmp_path / "profile.ps1").write_text("# fake\n")

    class _FakeGate:
        def __enter__(self):
            return None

        def __exit__(self, *exc):
            return False

    monkeypatch.setattr(workbench, "kiln_build_gate", lambda *a, **k: _FakeGate())


def test_set_target_not_invoked_when_root_sdkconfig_present(tmp_path, monkeypatch):
    root = _make_root(tmp_path, root_sdkconfig=True, cmake_cache=False)
    calls = []
    _patch_common(monkeypatch, tmp_path, calls)

    result = workbench.build_kilnfw(
        target="build", kiln_fw_root=str(root), skip_saftyfw=True)

    assert "kilnfw-set-target" not in calls
    assert "kilnfw-submodule-init" not in calls
    assert "kilnfw-build" in calls
    assert "OK" in result


def test_set_target_invoked_when_root_sdkconfig_absent(tmp_path, monkeypatch):
    root = _make_root(tmp_path, root_sdkconfig=False, cmake_cache=False)
    calls = []
    _patch_common(monkeypatch, tmp_path, calls)

    result = workbench.build_kilnfw(
        target="build", kiln_fw_root=str(root), skip_saftyfw=True)

    assert "kilnfw-set-target" in calls
    assert calls.index("kilnfw-set-target") < calls.index("kilnfw-build")
    assert "OK" in result


def test_set_target_not_invoked_when_build_cmakecache_present_even_without_root_sdkconfig(
        tmp_path, monkeypatch):
    # A configured-but-not-yet-published build dir (CMakeCache.txt exists)
    # should not be reconfigured just because the root sdkconfig happens to
    # be missing too -- CMakeCache.txt presence is enough evidence the build
    # was already targeted.
    root = _make_root(tmp_path, root_sdkconfig=False, cmake_cache=True)
    calls = []
    _patch_common(monkeypatch, tmp_path, calls)

    result = workbench.build_kilnfw(
        target="build", kiln_fw_root=str(root), skip_saftyfw=True)

    assert "kilnfw-set-target" not in calls
    assert "OK" in result


def test_fullclean_never_invokes_set_target_even_without_root_sdkconfig(tmp_path, monkeypatch):
    root = _make_root(tmp_path, root_sdkconfig=False, cmake_cache=False)
    calls = []
    _patch_common(monkeypatch, tmp_path, calls)

    result = workbench.build_kilnfw(
        target="fullclean", kiln_fw_root=str(root), skip_saftyfw=True)

    assert "kilnfw-set-target" not in calls
    assert "kilnfw-fullclean" in calls
    assert "OK" in result


def test_lvgl_submodule_init_runs_even_when_root_sdkconfig_present(tmp_path, monkeypatch):
    # The lvgl submodule check must be independent of the sdkconfig gate --
    # a worktree can have a configured sdkconfig but a missing/empty lvgl
    # checkout (e.g. a fresh `git worktree add` without --recurse-submodules).
    root = _make_root(tmp_path, root_sdkconfig=True, cmake_cache=False, lvgl=False)
    calls = []
    _patch_common(monkeypatch, tmp_path, calls)

    result = workbench.build_kilnfw(
        target="build", kiln_fw_root=str(root), skip_saftyfw=True)

    assert "kilnfw-submodule-init" in calls
    assert "kilnfw-set-target" not in calls
    assert "OK" in result


def test_single_quote_in_kiln_fw_root_is_rejected(tmp_path, monkeypatch):
    def _boom(*args, **kwargs):
        raise AssertionError("must not reach _run_locked for a rejected kiln_fw_root")

    monkeypatch.setattr(workbench, "_run_locked", _boom)

    bad_root = str(tmp_path) + "\\Kiln'FW"
    result = workbench.build_kilnfw(target="build", kiln_fw_root=bad_root, skip_saftyfw=True)
    assert "error" in result
    assert "quote" in result


def test_trailing_backslash_derives_correct_saftyfw_sibling(tmp_path, monkeypatch):
    root = _make_root(tmp_path, root_sdkconfig=True, cmake_cache=False)
    calls = []
    _patch_common(monkeypatch, tmp_path, calls)

    seen_saftyfw_root = {}

    def _fake_build_saftyfw(jobs, saftyfw_root=None):
        seen_saftyfw_root["value"] = saftyfw_root
        return "saftyfw: OK in 0.1s (0 log lines)\nfull log: -\n--\n(no output)"

    monkeypatch.setattr(workbench, "build_saftyfw", _fake_build_saftyfw)

    root_with_trailing_slash = str(root) + os.sep
    result = workbench.build_kilnfw(
        target="build", kiln_fw_root=root_with_trailing_slash, skip_saftyfw=False)

    assert seen_saftyfw_root["value"] == os.path.join(tmp_path, "SaftyFW")
    assert "OK" in result
