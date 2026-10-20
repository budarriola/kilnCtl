"""RA-01..RA-07 (docs/REVIEW_ELF_ARCHIVE_2026_10_20.md): release ELF archiving.

Run with: python -m pytest tools/PcTools/tests/test_release_elf_archive.py -q
"""
from __future__ import annotations

import importlib.util
import os
import shutil
import struct
import subprocess
import sys

import pytest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "src"))

from kilnctrl import diag_bundle, elf_archive, esp_app_desc  # noqa: E402

TOOLS = os.path.normpath(os.path.join(HERE, "..", ".."))
MAKE_RELEASE = os.path.join(TOOLS, "make_release.ps1")


def _load_script():
    spec = importlib.util.spec_from_file_location("archive_release_elf", os.path.join(TOOLS, "archive_release_elf.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def _image(project: str, tail: bytes = b"") -> bytes:
    data = bytearray(b"\x00" * 0x400)
    data[0] = 0xE9
    struct.pack_into("<H", data, 12, 9)
    desc = bytearray(256)
    struct.pack_into("<I", desc, 0, esp_app_desc.ESP_APP_DESC_MAGIC_WORD)
    desc[16:21] = b"v0.1\x00"
    desc[48:48 + len(project)] = project.encode()
    desc[80:88] = b"20:13:41"
    desc[96:107] = b"Sep  3 2026"
    data[0x20:0x20 + 256] = desc
    return bytes(data) + tail


def _git(cwd, *args):
    subprocess.run(["git", "-c", "user.email=t@t", "-c", "user.name=t", *args], cwd=cwd, check=True,
                   capture_output=True)


@pytest.fixture
def fake_root(tmp_path, monkeypatch):
    monkeypatch.setattr(elf_archive, "_repo_root", lambda: str(tmp_path / "root"))
    monkeypatch.setenv("KILNCTL_ALLOW_TEST_ARCHIVE_WRITE", "1")
    return tmp_path / "root"


def test_successful_archive_and_key(tmp_path, fake_root):
    app = tmp_path / "KilnCtrl.bin"
    elf = tmp_path / "KilnCtrl.elf"
    app.write_bytes(_image("kiln"))
    elf.write_bytes(_image("kiln", b"elfbody"))
    out = _load_script().archive(str(elf), str(app), "unknown (external build)", "v1")
    key = elf_archive._sha256_key(str(elf))
    assert len(key) == 12  # RA-07: key length coupling
    dest = fake_root / "firmware" / "KilnFW" / "elf_archive" / f"KilnCtrl-{key}.elf"
    assert dest.is_file() and f"key {key}" in out
    found, _ = elf_archive.find_elf_by_sha_prefix(key[:9])
    assert found and os.path.samefile(found, dest)
    entry = list(elf_archive._load_manifest(str(dest.parent)).values())[0]
    assert entry["git_commit"] == "unknown (external build)"
    assert entry["source"] == "make_release:v1"


def test_recovery_elf_archived_with_release(tmp_path, fake_root):
    app = tmp_path / "KilnCtrl.bin"
    elf = tmp_path / "KilnCtrl.elf"
    rbin = tmp_path / "recovery.bin"
    app.write_bytes(_image("kiln"))
    elf.write_bytes(_image("kiln", b"x"))
    rbin.write_bytes(_image("recovery"))
    (tmp_path / "recovery.elf").write_bytes(_image("recovery", b"rec"))
    out = _load_script().archive(str(elf), str(app), "abc", "v1", str(rbin))
    assert "recovery ELF archived" in out
    assert list((fake_root / "firmware" / "KilnFW" / "recovery_elf_archive").glob("recovery-*.elf"))


def test_recovery_elf_missing_fails(tmp_path, fake_root):
    app = tmp_path / "KilnCtrl.bin"
    elf = tmp_path / "KilnCtrl.elf"
    rbin = tmp_path / "recovery.bin"
    app.write_bytes(_image("kiln"))
    elf.write_bytes(_image("kiln", b"x"))
    rbin.write_bytes(_image("recovery"))
    with pytest.raises(RuntimeError, match="recovery ELF not archived"):
        _load_script().archive(str(elf), str(app), "abc", "v1", str(rbin))


@pytest.mark.skipif(shutil.which("git") is None, reason="git not available")
def test_main_tree_resolved_from_worktree():
    # System temp, not pytest tmp_path: nested git repos under the checkout defeat `git clean -fdx` (negtest).
    import tempfile
    with tempfile.TemporaryDirectory() as td:
        _worktree_case(__import__("pathlib").Path(td))


def _worktree_case(tmp_path):
    main = tmp_path / "main"
    main.mkdir()
    _git(main, "init", "-q")
    (main / "f").write_text("x")
    _git(main, "add", "f")
    _git(main, "commit", "-q", "-m", "c")
    wt = tmp_path / "wt"
    _git(main, "worktree", "add", "-q", str(wt))
    norm = lambda p: os.path.normcase(os.path.realpath(p))  # noqa: E731
    assert norm(elf_archive._main_tree_of(str(wt))) == norm(str(main))
    assert norm(elf_archive._main_tree_of(str(main))) == norm(str(main))


def test_main_tree_falls_back_without_git(tmp_path, monkeypatch):
    def boom(*a, **k):
        raise FileNotFoundError("git")
    monkeypatch.setattr(subprocess, "run", boom)
    assert elf_archive._main_tree_of(str(tmp_path)) == str(tmp_path)
    plain = tmp_path / "notarepo"
    plain.mkdir()


def test_make_release_wiring():
    src = open(MAKE_RELEASE, encoding="utf-8").read()
    assert 'archive_release_elf.py") $elf $appBin $headSha $Tag $RecoveryBin' in src  # RA-02
    assert "if ($externalBuild)" in src and '"unknown (external build)"' in src  # RA-03
    # externalBuild must be computed before the defaults fill BuildDir/RecoveryBin
    assert src.index("$externalBuild = [bool]") < src.index("if (-not $BuildDir)")


def test_elf_missing_message_points_at_release_asset(tmp_path):
    src = open(diag_bundle.__file__, encoding="utf-8").read()
    assert "KilnCtrl-<tag>.elf.zip" in src  # RA-05
