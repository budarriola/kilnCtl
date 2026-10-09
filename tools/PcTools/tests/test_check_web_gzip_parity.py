"""Tests for tools/check_web_gzip_parity.py.

Builds a minimal fake repo tree (a CMakeLists.txt with a KILNCTL_GZIP_ASSETS
list, a source asset, and a build-output .gz) under tmp_path and drives the
check's main() against it with --root, covering:
  - a matching source/.gz pair passes (exit 0)
  - a mismatched pair fails (exit 1) and names the offending file
  - a missing build directory (no build ever run) SKIPs (exit 3)
  - a missing CMakeLists.txt SKIPs (exit 3)
  - a .gz older than its own source SKIPs (exit 3) rather than failing
  - a mismatched .gz that is NOT older than its source still fails (exit 1)
"""
from __future__ import annotations

import gzip
import importlib.util
import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO_ROOT / "tools" / "check_web_gzip_parity.py"

spec = importlib.util.spec_from_file_location("check_web_gzip_parity", CHECK_PATH)
check_mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(check_mod)

CMAKE_TEMPLATE = """
set(KILNCTL_GZIP_ASSETS
    "http/main_page.html" "http/theme.css")
"""


def _make_repo(tmp_path: Path) -> Path:
    root = tmp_path / "repo"
    drivers = root / "firmware" / "KilnFW" / "App" / "drivers"
    (drivers / "http").mkdir(parents=True)
    (drivers / "CMakeLists.txt").write_text(CMAKE_TEMPLATE, encoding="utf-8")
    return root


def _write_source_and_gz(root: Path, rel: str, content: bytes, gz_content: bytes | None = None) -> None:
    src = root / "firmware" / "KilnFW" / "App" / "drivers" / rel
    src.write_bytes(content)
    build_dir = root / "firmware" / "KilnFW" / "build" / "esp-idf" / "drivers"
    build_dir.mkdir(parents=True, exist_ok=True)
    gz_path = build_dir / (Path(rel).name + ".gz")
    with gzip.open(gz_path, "wb", 9) as f:
        f.write(gz_content if gz_content is not None else content)


def _run(root: Path) -> int:
    argv = sys.argv
    sys.argv = ["check_web_gzip_parity.py", "--root", str(root)]
    try:
        return check_mod.main()
    finally:
        sys.argv = argv


def test_matching_pair_passes(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = _make_repo(tmp_path)
    _write_source_and_gz(root, "http/main_page.html", b"<html>hello</html>")
    _write_source_and_gz(root, "http/theme.css", b"body { color: red; }")
    code = _run(root)
    out = capsys.readouterr().out
    assert code == 0, out
    assert "OK" in out


def test_mismatched_pair_fails(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = _make_repo(tmp_path)
    _write_source_and_gz(root, "http/main_page.html", b"<html>hello</html>")
    _write_source_and_gz(
        root, "http/theme.css", b"body { color: red; }", gz_content=b"body { color: blue; }"
    )
    code = _run(root)
    out = capsys.readouterr().out
    assert code == 1, out
    assert "mismatch" in out
    assert "theme.css" in out


def _age_gz(root: Path, rel: str, seconds: int) -> None:
    """Backdate one build-output .gz so it predates its own source."""
    gz_path = (
        root / "firmware" / "KilnFW" / "build" / "esp-idf" / "drivers" / (Path(rel).name + ".gz")
    )
    src = root / "firmware" / "KilnFW" / "App" / "drivers" / rel
    stamp = src.stat().st_mtime - seconds
    os.utime(gz_path, (stamp, stamp))


def test_stale_gz_skips_instead_of_failing(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """A .gz older than its source is an un-rebuilt build dir, not a defect.

    Without the freshness gate this is exactly the shared main tree's
    2026-09-22 state (7 .gz older than their sources) and exits 1 -- a full
    run_all_checks.ps1 run red for a non-defect.
    """
    root = _make_repo(tmp_path)
    _write_source_and_gz(root, "http/main_page.html", b"<html>hello</html>")
    _write_source_and_gz(
        root, "http/theme.css", b"body { color: red; }", gz_content=b"body { color: blue; }"
    )
    _age_gz(root, "http/theme.css", 60)
    code = _run(root)
    out = capsys.readouterr().out
    assert code == 3, out
    assert "SKIP" in out
    assert "theme.css" in out


def test_fresh_mismatch_still_fails_alongside_a_stale_file(
    tmp_path: Path, capsys: pytest.CaptureFixture[str]
) -> None:
    """A hand-edited .gz that is NOT older than its source stays a hard FAIL."""
    root = _make_repo(tmp_path)
    _write_source_and_gz(
        root, "http/main_page.html", b"<html>hello</html>", gz_content=b"<html>tampered</html>"
    )
    _write_source_and_gz(root, "http/theme.css", b"body { color: red; }")
    _age_gz(root, "http/theme.css", 60)
    code = _run(root)
    out = capsys.readouterr().out
    assert code == 1, out
    assert "mismatch" in out
    assert "main_page.html" in out


def test_missing_build_dir_skips(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = _make_repo(tmp_path)
    # No firmware/KilnFW/build directory created at all -- never built.
    code = _run(root)
    out = capsys.readouterr().out
    assert code == 3, out
    assert "SKIP" in out


def test_missing_cmakelists_skips(tmp_path: Path, capsys: pytest.CaptureFixture[str]) -> None:
    root = tmp_path / "empty_repo"
    root.mkdir()
    code = _run(root)
    out = capsys.readouterr().out
    assert code == 3, out
    assert "SKIP" in out
