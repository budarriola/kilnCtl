#!/usr/bin/env python3
"""Regression test for SaftyFW/TODO.md's "Shared ids split out of
uart_task_ids.h; PC-link ids left behind" item.

UART_PROTOCOL_VERSION (the PC<->ESP link's own version, firmware/KilnFW/App/
drivers/uart_task_ids.h) was, from 2026-08-17 to 2026-08-24, a plain C alias
of KILNLINK_PROTOCOL_VERSION (the ESP<->Pico isolated safety link's own,
unrelated version, firmware/CommonFW/include/kilnlink/kilnlink_version.h).
Because kilnctrl.devices.FirmwareVersion.compatible is a hard equality gate
against this module's UART_PROTOCOL_VERSION constant, a bump to the isolated
link's number that had nothing to do with the PC link (e.g. 2026-08-23's
Frame A tx_dropped_sat addition) silently dragged the PC link's version
along with it and got every PC command refused on real hardware with
"device speaks vN, pc_tools speaks vN-1" -- observed live three times.

This test asserts the two constants are independent literals today, not
merely equal by coincidence: the C header's #define must not textually
reference any KILNLINK_* symbol (the same rule
tools/check_uart_version_independence.ps1 enforces via CI grep -- this test
is the pytest-reachable half of that same guarantee, run as part of this
project's normal `pytest -q` baseline rather than only via a separate
PowerShell invocation), and this Python module's own UART_PROTOCOL_VERSION
must be a plain literal too, not computed from anything kilnlink-flavored.
"""

from __future__ import annotations

import re
from pathlib import Path

from kilnctrl import protocol

from _drivers_layout import resolve_driver_file

_REPO_ROOT = Path(__file__).resolve().parents[3]
_UART_TASK_IDS_H = resolve_driver_file(_REPO_ROOT, "uart_task_ids.h")
_KILNLINK_VERSION_H = (
    _REPO_ROOT
    / "firmware"
    / "CommonFW"
    / "include"
    / "kilnlink"
    / "kilnlink_version.h"
)


def _strip_comments(text: str) -> str:
    """Minimal C comment stripper -- good enough for these two small headers,
    same "block comments then line comments" order as
    tools/check_uart_version_independence.ps1's Get-CodeOnlyLines, just done
    on the whole text instead of line-by-line."""
    text = re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)
    text = re.sub(r"//.*", "", text)
    return text


def _find_define_value(code: str, name: str) -> str:
    match = re.search(
        rf"^\s*#\s*define\s+{re.escape(name)}\b(.*)$", code, flags=re.MULTILINE
    )
    assert match is not None, f"no #define {name} found -- has it moved/been renamed?"
    return match.group(1)


def test_uart_task_ids_header_exists():
    assert _UART_TASK_IDS_H.is_file(), _UART_TASK_IDS_H


def test_uart_protocol_version_not_derived_from_kilnlink():
    """The actual regression: uart_task_ids.h's UART_PROTOCOL_VERSION must
    not be defined in terms of KILNLINK_PROTOCOL_VERSION (or any other
    KILNLINK_* symbol) -- that is exactly the alias that bit real hardware
    three times."""
    code = _strip_comments(_UART_TASK_IDS_H.read_text(encoding="utf-8"))
    value_side = _find_define_value(code, "UART_PROTOCOL_VERSION")
    assert "KILNLINK_" not in value_side, (
        "UART_PROTOCOL_VERSION has been re-aliased to a KILNLINK_* symbol: "
        f"{value_side!r} -- see uart_task_ids.h's own doc comment for why "
        "this must stay an independent literal"
    )


def test_uart_task_ids_header_no_longer_includes_kilnlink_version_h():
    """The #include this alias needed is gone -- its presence would be a
    strong hint the alias crept back in even if the #define itself looks
    fine (e.g. hidden behind an intermediate macro). Checked against the
    actual #include directive, not prose: this file's own doc comment (and
    this test's docstring) legitimately mention "kilnlink_version.h" by name
    when explaining the history, and that must not trip the check."""
    code = _strip_comments(_UART_TASK_IDS_H.read_text(encoding="utf-8"))
    assert not re.search(r'#\s*include\s*"[^"]*kilnlink_version\.h"', code), (
        "uart_task_ids.h includes kilnlink_version.h again -- that header "
        "is only needed to alias UART_PROTOCOL_VERSION, which must not "
        "happen; see this file's module doc comment"
    )


def test_kilnlink_protocol_version_not_derived_from_uart_protocol_version():
    """Symmetry check: the isolated link's own version must not become a
    derived alias of the PC link's number either -- independence cuts both
    ways."""
    code = _strip_comments(_KILNLINK_VERSION_H.read_text(encoding="utf-8"))
    value_side = _find_define_value(code, "KILNLINK_PROTOCOL_VERSION")
    assert "UART_PROTOCOL_VERSION" not in value_side


def test_pc_tools_uart_protocol_version_is_a_plain_literal():
    """protocol.UART_PROTOCOL_VERSION (the Python mirror pc_tools actually
    gates on in devices.FirmwareVersion.compatible) must be an int literal
    the module owns, not merely re-exported from anything kilnlink-shaped."""
    assert isinstance(protocol.UART_PROTOCOL_VERSION, int)


def test_wire_version_matches_on_both_sides():
    """The firmware's C-side constant and pc_tools' Python-side constant must
    stay the same number, even though they are no longer textually related --
    a real wire change (like the GET_DIAG payload growing to 31 bytes, which
    bumped this to 13) must be applied to both sides by hand, and this test
    is what catches a one-sided miss."""
    code = _strip_comments(_UART_TASK_IDS_H.read_text(encoding="utf-8"))
    value_side = _find_define_value(code, "UART_PROTOCOL_VERSION")
    # The value is `((uint16_t)13)` -- pull the literal that follows the cast,
    # not the "16" inside "uint16_t" itself (a bare r"\d+" search would find
    # that first and silently check the wrong number).
    firmware_literal = re.search(r"\(uint16_t\)\s*(\d+)", value_side)
    assert firmware_literal is not None, value_side
    assert int(firmware_literal.group(1)) == 13
    assert protocol.UART_PROTOCOL_VERSION == 13
