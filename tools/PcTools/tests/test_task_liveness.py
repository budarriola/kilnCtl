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
    TaskSpec,
    check_task_liveness,
    load_required_task_names,
    load_required_task_specs,
    parse_required_task_names,
    parse_required_task_specs,
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


def test_parse_required_task_names_accepts_mixed_quote_styles():
    """PowerShell array literals accept either quote style, and a script
    edit that switches or mixes them must not silently drop entries -- the
    original regex only matched double-quoted strings."""
    text = '''
$requiredNames = @(
    "autotune_engine", 'boot_button',
    'danger_mode'
)
'''
    assert parse_required_task_names(text) == ("autotune_engine", "boot_button", "danger_mode")


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


# --- liveness tags -----------------------------------------------------


def test_parse_required_task_specs_defaults_to_always():
    text = '$requiredNames = @(\n    "a", "b"\n)'
    specs = parse_required_task_specs(text)
    assert specs == (TaskSpec(name="a", tag="always"), TaskSpec(name="b", tag="always"))


def test_parse_required_task_specs_reads_trailing_tag_comment():
    text = '''
$requiredNames = @(
    "a",
    "b",  # liveness: boot-once
    "c",  # liveness: config
    "d"  # liveness: on-demand
)
'''
    specs = {s.name: s.tag for s in parse_required_task_specs(text)}
    assert specs == {"a": "always", "b": "boot-once", "c": "config", "d": "on-demand"}


def test_parse_required_task_specs_rejects_unknown_tag():
    text = '$requiredNames = @(\n    "a",  # liveness: bogus\n)'
    with pytest.raises(TaskLivenessParseError):
        parse_required_task_specs(text)


def test_parse_required_task_specs_preserves_names_order_and_dedup():
    """parse_required_task_names() must keep behaving identically when
    implemented in terms of parse_required_task_specs()."""
    text = '$requiredNames = @("a", "b", "a", "c")'
    assert parse_required_task_names(text) == ("a", "b", "c")


def test_load_required_task_specs_against_the_real_check_script():
    """The six known by-design exceptions from the first live
    check_task_liveness run must carry their tags in the real script."""
    specs = {s.name: s.tag for s in load_required_task_specs(_REAL_CHECK_SCRIPT)}
    assert specs["pico_auto_update"] == "boot-once"
    assert specs["gpio_probe"] == "config"
    assert specs["i2c_owner_ns2009"] == "config"
    assert specs["ota_pico_rollback"] == "on-demand"
    assert specs["recovery_exit"] == "on-demand"
    assert specs["ota_rollback_reboot"] == "on-demand"
    # An ordinary long-lived task stays untagged/"always".
    assert specs["kiln_io_owner"] == "always"


def test_boot_once_dead_is_informational_not_fault():
    expected = ("pico_auto_update",)
    tags = {"pico_auto_update": "boot-once"}
    entries = [_entry("pico_auto_update", alive=False)]
    report = check_task_liveness(entries, expected, tags=tags)
    assert report.ok
    assert report.fault_dead == ()
    assert report.info_dead == ("pico_auto_update",)


def test_boot_once_absent_is_a_fault():
    """Unlike DEAD, a boot-once task that never even registered means its
    stack_margin_register() call site is missing entirely -- still a
    fault."""
    expected = ("pico_auto_update",)
    tags = {"pico_auto_update": "boot-once"}
    report = check_task_liveness([], expected, tags=tags)
    assert not report.ok
    assert report.fault_absent == ("pico_auto_update",)
    assert report.info_absent == ()


def test_config_tag_dead_and_absent_are_both_informational():
    expected = ("gpio_probe", "i2c_owner_ns2009")
    tags = {"gpio_probe": "config", "i2c_owner_ns2009": "config"}
    entries = [_entry("gpio_probe", alive=False)]
    report = check_task_liveness(entries, expected, tags=tags)
    assert report.ok
    assert report.info_dead == ("gpio_probe",)
    assert report.info_absent == ("i2c_owner_ns2009",)
    assert report.fault_dead == () and report.fault_absent == ()


def test_on_demand_tag_dead_and_absent_are_both_informational():
    expected = ("ota_pico_rollback", "recovery_exit")
    tags = {"ota_pico_rollback": "on-demand", "recovery_exit": "on-demand"}
    entries = [_entry("ota_pico_rollback", alive=False)]
    report = check_task_liveness(entries, expected, tags=tags)
    assert report.ok
    assert set(report.info_dead) == {"ota_pico_rollback"}
    assert set(report.info_absent) == {"recovery_exit"}


def test_untagged_dead_task_still_refuses():
    """MANDATORY negative test: a plain (untagged, i.e. 'always') task that
    is dead must still fail the report even when a `tags` map is supplied
    for OTHER names -- tagging must never accidentally widen to tasks that
    were never given a by-design exception."""
    expected = ("kiln_io_owner", "gpio_probe")
    tags = {"gpio_probe": "config"}
    entries = [_entry("kiln_io_owner", alive=False), _entry("gpio_probe", alive=False)]
    report = check_task_liveness(entries, expected, tags=tags)
    assert not report.ok
    assert report.fault_dead == ("kiln_io_owner",)
    assert report.info_dead == ("gpio_probe",)
    text = report.describe()
    assert "kiln_io_owner" in text
    assert "FAIL" in text


def test_tag_comment_on_multi_name_line_raises():
    """A '# liveness: ...' tag comment must apply to exactly one entry --
    two names sharing the same tagged line is ambiguous and must fail loud
    rather than silently tagging both."""
    text = '$requiredNames = @(\n    "a", "b",  # liveness: config\n)'
    with pytest.raises(TaskLivenessParseError):
        parse_required_task_specs(text)


def test_tag_comment_with_no_names_on_line_raises():
    """A liveness tag comment with zero quoted names on its line has
    nothing to apply to and must fail loud rather than being silently
    dropped."""
    text = '''
$requiredNames = @(
    "a"
    # liveness: config
)
'''
    with pytest.raises(TaskLivenessParseError):
        parse_required_task_specs(text)


def test_tag_found_anywhere_in_comment_not_just_at_start():
    """The tag regex must not be anchored to the start of the comment --
    'liveness:' preceded by other comment text must still be parsed rather
    than silently defaulting to 'always'."""
    text = '$requiredNames = @(\n    "a",  # note # liveness: config\n)'
    specs = {s.name: s.tag for s in parse_required_task_specs(text)}
    assert specs == {"a": "config"}


def test_untagged_absent_task_still_refuses_with_no_tags_map_at_all():
    """Same guarantee with no `tags` argument passed at all (the plain
    pre-tag call shape every existing caller used) -- must default every
    name to 'always' and still refuse."""
    expected = ("kiln_io_owner",)
    report = check_task_liveness([], expected)
    assert not report.ok
    assert report.fault_absent == ("kiln_io_owner",)
