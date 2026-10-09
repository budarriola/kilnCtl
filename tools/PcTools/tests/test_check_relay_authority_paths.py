"""Negative test for tools/check_relay_authority_paths.py.

Proves the check can actually fail: a script that calls
``io.send(devices.io_set_relay(...))`` directly (bypassing the refusal-aware
``IoClient.set_relay()`` wrapper) must be flagged, and the same call routed
through the wrapper must not be. Per this repo's rule that a check nobody has
seen fail is not evidence, this exercises both sides rather than only the
"real code passes" direction.
"""
from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
CHECK_PATH = REPO_ROOT / "tools" / "check_relay_authority_paths.py"

spec = importlib.util.spec_from_file_location("check_relay_authority_paths", CHECK_PATH)
check_mod = importlib.util.module_from_spec(spec)
assert spec.loader is not None
spec.loader.exec_module(check_mod)


def _fixture_path(tmp_path: Path, filename: str) -> Path:
    """Build a scratch fixture path without writing an `x / "name.py"` chain
    literal -- source_path_drift_check.py treats that shape as a reference to
    a real repo source file and fails when no such file exists. These names
    are throwaway pytest fixtures written under `tmp_path`, never real
    checked-in files."""
    return tmp_path.joinpath(filename)


BYPASS_SNIPPET = """
from kilnctrl import devices
from kilnctrl.io_expander import IoClient


def run(io: IoClient) -> None:
    io.send(devices.io_set_relay(1, True))
"""

WRAPPED_SNIPPET = """
from kilnctrl.io_expander import IoClient


def run(io: IoClient) -> None:
    result = io.set_relay(1, True)
"""

ALL_OFF_BYPASS_SNIPPET = """
from kilnctrl import devices
from kilnctrl.io_expander import IoClient


def cleanup(io: IoClient) -> None:
    io.send(devices.io_all_relays_off())
"""


def test_bare_send_of_raw_relay_frame_is_flagged(tmp_path: Path) -> None:
    target = _fixture_path(tmp_path, "fake_bypass_script.py")
    target.write_text(BYPASS_SNIPPET, encoding="utf-8")

    violations = check_mod.check_file(target)

    assert violations, "expected the raw io.send(devices.io_set_relay(...)) call to be flagged"
    assert "io_set_relay" in violations[0]


def test_all_relays_off_bare_send_is_also_flagged(tmp_path: Path) -> None:
    target = _fixture_path(tmp_path, "fake_bypass_all_off.py")
    target.write_text(ALL_OFF_BYPASS_SNIPPET, encoding="utf-8")

    violations = check_mod.check_file(target)

    assert violations, "expected the raw io.send(devices.io_all_relays_off()) call to be flagged"


def test_wrapper_method_call_is_not_flagged(tmp_path: Path) -> None:
    target = _fixture_path(tmp_path, "fake_correct_script.py")
    target.write_text(WRAPPED_SNIPPET, encoding="utf-8")

    violations = check_mod.check_file(target)

    assert violations == []


def test_definition_files_are_exempt(tmp_path: Path) -> None:
    # io_expander.py itself contains `devices.io_set_relay(relay, on)` passed
    # to self._set_relay_style(...), not to a bare .send() -- but it is
    # exempted by filename regardless, since it's the file that defines the
    # wrapper the rest of the tree must route through.
    target = _fixture_path(tmp_path, "io_expander.py")
    target.write_text(BYPASS_SNIPPET, encoding="utf-8")

    violations = check_mod.check_file(target)

    assert violations == []


def test_real_tree_has_no_bypasses() -> None:
    """End-to-end: the actual tools/PcTools src and scripts trees, as fixed."""
    violations: list[str] = []
    for path in check_mod.find_py_files(REPO_ROOT):
        violations.extend(check_mod.check_file(path))
    assert violations == [], violations
