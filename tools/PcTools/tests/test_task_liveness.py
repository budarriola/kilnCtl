"""Tests for kilnctrl.task_liveness -- no link, no serial port, no board.

Covers both halves: parsing $requiredNames out of a (fabricated, then real)
check_stack_margin_registration.ps1 text, and diffing a fabricated
StackMarginEntry list against an expected-name tuple.
"""
from __future__ import annotations

from pathlib import Path

import pytest

from kilnctrl.devices_info import StackMarginEntry
from kilnctrl.protocol import StackMarginLevel
from kilnctrl.task_liveness import (
    TaskLivenessParseError,
    check_task_liveness,
    load_required_task_names,
    parse_required_task_names,
)

_REPO_ROOT = Path(__file__).resolve().parents[3]
_REAL_CHECK_SCRIPT = _REPO_ROOT / "tools" / "check_stack_margin_registration.ps1"


def _entry(name, alive=True, level=StackMarginLevel.OK, configured=4096, hwm=2000):
    return StackMarginEntry(
        name=name, configured_stack_bytes=configured, hwm_bytes=hwm, alive=alive, level=level
    )


# --- parse_required_task_names ---------------------------------------------


def test_parse_required_task_names_basic():
    text = '''
$requiredNames = @(
    "autotune_engine", "boot_button",
    "danger_mode"
)
'''
    assert parse_required_task_names(text) == ("autotune_engine", "boot_button", "danger_mode")


def test_parse_required_task_names_strips_comments():
    text = '''
$requiredNames = @(
    "autotune_engine",  # trailing comment
    # a full-line comment naming "not_a_real_task"
    "boot_button"
)
'''
    names = parse_required_task_names(text)
    assert names == ("autotune_engine", "boot_button")
    assert "not_a_real_task" not in names


def test_parse_required_task_names_dedupes_preserving_order():
    text = '$requiredNames = @("a", "b", "a", "c")'
    assert parse_required_task_names(text) == ("a", "b", "c")


def test_parse_required_task_names_raises_when_variable_missing():
    """A renamed or restructured $requiredNames must fail loud, not report
    an empty (vacuously-passing) required-task set."""
    with pytest.raises(TaskLivenessParseError):
        parse_required_task_names("$someOtherVariable = @(\"a\")")


def test_parse_required_task_names_raises_when_block_empty():
    with pytest.raises(TaskLivenessParseError):
        parse_required_task_names("$requiredNames = @(\n    # nothing but comments\n)")


def test_parse_required_task_names_raises_on_truncated_block():
    """No closing paren at all -- must not silently match garbage."""
    with pytest.raises(TaskLivenessParseError):
        parse_required_task_names('$requiredNames = @(\n    "a", "b"\n# never closed')


def test_load_required_task_names_against_the_real_check_script():
    """This is the negative-test-relevant guard for THIS parser: if
    check_stack_margin_registration.ps1's $requiredNames block is ever
    renamed or restructured without updating parse_required_task_names(),
    this test fails loud rather than every caller silently getting an
    empty/stale required-task set."""
    assert _REAL_CHECK_SCRIPT.is_file(), (
        f"expected {_REAL_CHECK_SCRIPT} to exist -- has the check script moved?"
    )
    names = load_required_task_names(_REAL_CHECK_SCRIPT)
    # A sanity floor mirroring the check script's own "implausibly low"
    # guard for its call-site count -- catches this parser going blind the
    # same way that script guards against itself going blind.
    assert len(names) >= 20
    assert "kiln_io_owner" in names
    assert "httpd_worker" in names
    assert "kiln_cfg_swap" in names
    assert "pico_auto_update" in names


# --- check_task_liveness ----------------------------------------------------


def test_all_expected_alive_is_ok():
    expected = ("a", "b", "c")
    entries = [_entry("a"), _entry("b"), _entry("c")]
    report = check_task_liveness(entries, expected)
    assert report.ok
    assert report.alive == ("a", "b", "c")
    assert report.dead == ()
    assert report.absent == ()
    assert report.extra == ()


def test_registered_but_dead_task_is_reported_dead_not_absent():
    """The whole point of this module: a task whose creation failed this
    boot is still REGISTERED (its slot exists, alive=False) -- distinct
    from a task that never registered at all."""
    expected = ("a", "b")
    entries = [_entry("a"), _entry("b", alive=False)]
    report = check_task_liveness(entries, expected)
    assert not report.ok
    assert report.dead == ("b",)
    assert report.absent == ()


def test_missing_from_reply_entirely_is_absent():
    expected = ("a", "b", "c")
    entries = [_entry("a"), _entry("b")]
    report = check_task_liveness(entries, expected)
    assert not report.ok
    assert report.absent == ("c",)
    assert report.dead == ()


def test_extra_alive_task_is_informational_only():
    expected = ("a",)
    entries = [_entry("a"), _entry("brand_new_task")]
    report = check_task_liveness(entries, expected)
    assert report.ok
    assert report.extra == ("brand_new_task",)


def test_dead_extra_task_is_not_reported_at_all():
    """A dead task outside the expected set is neither a failure (it was
    never required) nor 'extra' (extra is defined as alive-only, so a dead
    unexpected task doesn't manufacture a spurious informational entry)."""
    expected = ("a",)
    entries = [_entry("a"), _entry("some_other_task", alive=False)]
    report = check_task_liveness(entries, expected)
    assert report.ok
    assert report.extra == ()


def test_describe_mentions_dead_and_absent_names():
    expected = ("a", "b", "c")
    entries = [_entry("a", alive=False)]
    report = check_task_liveness(entries, expected)
    text = report.describe()
    assert "DEAD" in text
    assert "a" in text
    assert "ABSENT" in text
    assert "b" in text and "c" in text
    assert "FAIL" in text
