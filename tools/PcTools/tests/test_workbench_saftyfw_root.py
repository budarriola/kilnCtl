"""Tests for build_saftyfw's saftyfw_root validation and _cmake_build's
configure-failure gate -- see the two review advisories closed alongside
this file for the exact defects being locked down.

Everything that would touch a real subprocess is monkeypatched: no cmake,
ninja, or SDK path resolution actually runs.
"""

import os

import mcpkit.workbench as workbench


def test_relative_saftyfw_root_is_rejected(tmp_path, monkeypatch):
    def _boom(*args, **kwargs):
        raise AssertionError("must not reach _cmake_build for a relative root")

    monkeypatch.setattr(workbench, "_cmake_build", _boom)

    result = workbench.build_saftyfw(saftyfw_root="relative/path")
    assert "error" in result
    assert "absolute" in result


def test_missing_cmakelists_is_rejected(tmp_path, monkeypatch):
    def _boom(*args, **kwargs):
        raise AssertionError("must not reach _cmake_build without CMakeLists.txt")

    monkeypatch.setattr(workbench, "_cmake_build", _boom)

    empty_dir = tmp_path / "not_a_saftyfw_root"
    empty_dir.mkdir()

    result = workbench.build_saftyfw(saftyfw_root=str(empty_dir))
    assert "error" in result
    assert "CMakeLists.txt" in result


def test_valid_absolute_root_with_cmakelists_reaches_cmake_build(tmp_path, monkeypatch):
    root = tmp_path / "SaftyFW"
    root.mkdir()
    (root / "CMakeLists.txt").write_text("# fake\n")

    seen = {}

    def _fake_cmake_build(tag, build_dir, jobs, source_dir=None):
        seen["tag"] = tag
        seen["build_dir"] = build_dir
        seen["source_dir"] = source_dir
        return f"{tag}: OK in 0.1s (0 log lines)\nfull log: -\n--\n(no output)"

    monkeypatch.setattr(workbench, "_cmake_build", _fake_cmake_build)

    result = workbench.build_saftyfw(saftyfw_root=str(root))
    assert "OK" in result
    assert seen["source_dir"] == str(root)
    assert seen["build_dir"] == os.path.join(str(root), "build")


def test_configure_cmake_not_found_does_not_proceed_to_build(tmp_path, monkeypatch):
    build_dir = tmp_path / "build"

    def _fake_run_locked(tag, resource_key, argv, **kwargs):
        if tag.endswith("-configure"):
            return "saftyfw-configure: error: cmake not found on PATH"
        raise AssertionError("must not reach the build step after a failed configure")

    monkeypatch.setattr(workbench, "_run_locked", _fake_run_locked)
    monkeypatch.setattr(
        "mcpkit.pico_sdk.resolve_pico_sdk_path", lambda: str(tmp_path / "pico-sdk"))

    result = workbench._cmake_build("saftyfw", str(build_dir), 0, source_dir=str(tmp_path))
    assert "cmake not found on PATH" in result


def test_configure_ok_proceeds_to_build(tmp_path, monkeypatch):
    build_dir = tmp_path / "build"
    calls = []

    def _fake_run_locked(tag, resource_key, argv, **kwargs):
        calls.append(tag)
        if tag.endswith("-configure"):
            return "saftyfw-configure: OK in 3.0s (2 log lines)\nfull log: -\n--\n(no output)"
        return "saftyfw: OK in 5.0s (4 log lines)\nfull log: -\n--\n(no output)"

    monkeypatch.setattr(workbench, "_run_locked", _fake_run_locked)
    monkeypatch.setattr(
        "mcpkit.pico_sdk.resolve_pico_sdk_path", lambda: str(tmp_path / "pico-sdk"))

    result = workbench._cmake_build("saftyfw", str(build_dir), 0, source_dir=str(tmp_path))
    assert calls == ["saftyfw-configure", "saftyfw"]
    assert "configured from scratch" in result
    assert "saftyfw: OK" in result
