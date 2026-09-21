"""Negative-test coverage for kilnctrl.binary_provenance -- see the module
docstring for the incident this guards against
(docs/audits/review_sim_fuzzy_commits_2026-09-13.md)."""

from __future__ import annotations

import os
import time

import pytest

from kilnctrl.binary_provenance import assert_binary_fresh, check_binary_fresh


def _touch(path: str, mtime: float) -> None:
    with open(path, "w", encoding="utf-8") as f:
        f.write("x")
    os.utime(path, (mtime, mtime))


def test_fresh_binary_passes(tmp_path):
    src = tmp_path / "a.c"
    exe = tmp_path / "a.exe"
    now = time.time()
    _touch(str(src), now - 10)
    _touch(str(exe), now)

    result = check_binary_fresh(str(exe), [str(tmp_path / "*.c")])
    assert result.stale is False
    assert_binary_fresh(str(exe), [str(tmp_path / "*.c")])  # must not raise


def test_stale_binary_is_rejected(tmp_path):
    """The decisive negative case: a source file edited (or hand-restored)
    AFTER the exe was built must fail loudly, exactly the shape that let
    ba230bca measure sabotaged code -- source newer than build product."""
    src = tmp_path / "a.c"
    exe = tmp_path / "a.exe"
    now = time.time()
    _touch(str(exe), now - 10)
    _touch(str(src), now)  # source touched AFTER the exe was produced

    result = check_binary_fresh(str(exe), [str(tmp_path / "*.c")])
    assert result.stale is True
    assert "older than" in result.reason

    with pytest.raises(RuntimeError, match="binary_provenance"):
        assert_binary_fresh(str(exe), [str(tmp_path / "*.c")])


def test_missing_binary_is_stale(tmp_path):
    result = check_binary_fresh(str(tmp_path / "missing.exe"), [str(tmp_path / "*.c")])
    assert result.stale is True
    assert "not found" in result.reason


def test_no_matched_sources_refuses_rather_than_passing(tmp_path):
    exe = tmp_path / "a.exe"
    _touch(str(exe), time.time())

    result = check_binary_fresh(str(exe), [str(tmp_path / "*.nomatch")])
    assert result.stale is True
    assert "no source files matched" in result.reason
