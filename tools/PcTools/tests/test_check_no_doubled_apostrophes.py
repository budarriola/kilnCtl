"""Tests for tools/check_no_doubled_apostrophes.py.

Covers the artifact class fixed in commit ee0b8754 (39 instances in
firmware/KilnFW/TODO.md): a contraction/possessive doubled apostrophe,
a backtick-then-possessive doubled apostrophe, and that a legitimate ''
inside a fenced code block or an inline code span is never flagged. Also
covers a clean file passing outright.
"""
from __future__ import annotations

import importlib.util
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO_ROOT / "tools" / "check_no_doubled_apostrophes.py"

spec = importlib.util.spec_from_file_location("check_no_doubled_apostrophes", CHECK_PATH)
check_mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(check_mod)


def _write(tmp_path: Path, filename: str, content: str) -> Path:
    target = tmp_path.joinpath(filename)
    target.write_text(content, encoding="utf-8")
    return target


def test_contraction_is_caught(tmp_path: Path) -> None:
    src = "This doesn''t work as expected.\n"
    target = _write(tmp_path, "fake_todo.md", src)
    violations = check_mod.check_file(target)
    assert violations, "expected doesn''t to be flagged"
    assert "doesn''t" in violations[0]


def test_backtick_possessive_is_caught(tmp_path: Path) -> None:
    src = "See `file.h`''s comment for details.\n"
    target = _write(tmp_path, "fake_doc.md", src)
    violations = check_mod.check_file(target)
    assert violations, "expected `file.h`''s to be flagged"
    assert "`file.h`''s" in violations[0]


def test_fenced_code_block_is_ignored(tmp_path: Path) -> None:
    src = (
        "Prose before.\n"
        "\n"
        "```python\n"
        "s = ''\n"
        "if s == '':\n"
        "    pass\n"
        "```\n"
        "\n"
        "Prose after.\n"
    )
    target = _write(tmp_path, "fake_fenced.md", src)
    violations = check_mod.check_file(target)
    assert violations == []


def test_inline_code_span_is_ignored(tmp_path: Path) -> None:
    src = "Use the literal `''` empty string in this context.\n"
    target = _write(tmp_path, "fake_inline.md", src)
    violations = check_mod.check_file(target)
    assert violations == []


def test_clean_file_passes(tmp_path: Path) -> None:
    src = (
        "# Title\n"
        "\n"
        "This doesn't do anything weird, and `file.h`'s comment is fine too.\n"
        "\n"
        "```python\n"
        "s = ''\n"
        "```\n"
    )
    target = _write(tmp_path, "fake_clean.md", src)
    violations = check_mod.check_file(target)
    assert violations == []


def test_real_tree_has_no_violations() -> None:
    """End-to-end: the actual repo tree, as fixed by ee0b8754."""
    violations: list[str] = []
    for path in check_mod.find_files(REPO_ROOT):
        violations.extend(check_mod.check_file(path))
    assert violations == [], violations
