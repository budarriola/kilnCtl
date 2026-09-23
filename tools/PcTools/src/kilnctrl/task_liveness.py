"""Cross-check a live GET_STACK_MARGIN reading against the required-task
list ``check_stack_margin_registration.ps1`` enforces at the source level.

That check guards the STATIC side of the contract: every task this codebase
cares about has a ``stack_margin_register()`` call site, so its high-water
mark is reachable. It says nothing about the DYNAMIC side -- whether, on a
given boot, ``xTaskCreate*()`` actually succeeded for each of those tasks.
Every KilnFW task-creation failure is logged and nothing else
(``ESP_LOGE``, non-fatal, no counter, no HTTP-visible flag), so a board that
silently failed to start ``profile_executor`` at boot looks perfectly
healthy in ``/api/status`` -- ``stack_margin_read()``
(``App/drivers/common/stack_margin.c``) is the one place that failure is
still visible: the task's registry slot is present (registered by name at
compile time) but its handle is NULL, so ``alive`` reads false.

This module is pure: no link, no serial port, no board. It has two jobs:

1. Parse the ``$requiredNames`` PowerShell array literal out of
   ``tools/check_stack_margin_registration.ps1`` (:func:`load_required_task_names`),
   so the expected-task list lives in exactly one place -- that script's own
   source -- rather than being copied into a second, driftable Python list.
2. Diff a live :class:`~kilnctrl.devices_info.StackMarginEntry` list against
   that expected set (:func:`check_task_liveness`), producing
   :class:`TaskLivenessReport`.

See ``mcp_server_info.py``'s ``check_task_liveness`` tool for the live,
board-reading half, and ``mcp_server_capability_preflight.py`` for how a
FATAL result here also blocks ``capability_preflight_check``.
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

from .devices_info import StackMarginEntry

#: Repo-relative path to the PowerShell check this module mirrors the
#: required-task list from. Callers on a real checkout pass an absolute
#: path resolved against their own repo root; this constant is only used by
#: :func:`default_check_script_path` as a convenience for the common case.
DEFAULT_CHECK_SCRIPT_RELPATH = "tools/check_stack_margin_registration.ps1"

#: Matches the ``$requiredNames = @( ... )`` PowerShell array literal.
#: Non-greedy so a truncated/edited file (no closing paren) fails to match
#: rather than silently swallowing the rest of the script.
_REQUIRED_NAMES_BLOCK_RE = re.compile(
    r"\$requiredNames\s*=\s*@\(\s*(.*?)\s*\)", re.DOTALL
)
#: Matches one quoted PowerShell string literal, double- or single-quoted,
#: e.g. "kiln_io_owner" or 'kiln_io_owner' -- PowerShell array literals
#: accept either, and a $requiredNames edit that switches quote style (or
#: mixes them) must not silently drop entries from the parsed list.
_QUOTED_STRING_RE = re.compile(r'"([^"]+)"|\'([^\']+)\'')


class TaskLivenessParseError(Exception):
    """Raised when ``$requiredNames`` cannot be found or parsed out of
    check_stack_margin_registration.ps1 -- fail loud rather than silently
    reporting zero required tasks (which would make every board look
    perfectly alive)."""


def default_check_script_path(repo_root: "str | Path") -> Path:
    """``repo_root``/``tools/check_stack_margin_registration.ps1``."""
    return Path(repo_root) / DEFAULT_CHECK_SCRIPT_RELPATH


def _strip_ps1_comment_lines(text: str) -> str:
    """Drop full-line and trailing ``#`` comments. Good enough for this
    file's own style (no ``#`` inside a quoted task name), and mirrors the
    check script's own light-touch comment handling rather than a full
    PowerShell parser."""
    out_lines = []
    for line in text.splitlines():
        idx = line.find("#")
        out_lines.append(line if idx < 0 else line[:idx])
    return "\n".join(out_lines)


def parse_required_task_names(script_text: str) -> "tuple[str, ...]":
    """Extract the ``$requiredNames`` array literal's string entries, in
    source order, de-duplicated while preserving first occurrence.

    Raises :class:`TaskLivenessParseError` if the ``$requiredNames = @( ... )``
    block cannot be found (the script was renamed, restructured, or the
    variable itself renamed) or if it parses to zero names -- either means
    this module has gone blind to the real required-task list, and silently
    reporting an empty requirement set would make ``check_task_liveness``
    vacuously pass on every board.
    """
    cleaned = _strip_ps1_comment_lines(script_text)
    match = _REQUIRED_NAMES_BLOCK_RE.search(cleaned)
    if not match:
        raise TaskLivenessParseError(
            "could not find a '$requiredNames = @( ... )' array literal -- "
            "check_stack_margin_registration.ps1 may have renamed or "
            "restructured this variable; update parse_required_task_names()."
        )
    body = match.group(1)
    names = []
    seen = set()
    for m in _QUOTED_STRING_RE.finditer(body):
        name = m.group(1) if m.group(1) is not None else m.group(2)
        if name not in seen:
            seen.add(name)
            names.append(name)
    if not names:
        raise TaskLivenessParseError(
            "'$requiredNames' block matched but contained no quoted string "
            "literals -- parse_required_task_names() has gone blind to the "
            "real required-task list."
        )
    return tuple(names)


def load_required_task_names(script_path: "str | Path") -> "tuple[str, ...]":
    """Read and parse ``script_path`` (normally
    ``tools/check_stack_margin_registration.ps1``). Raises
    :class:`TaskLivenessParseError` (missing/unparsable variable) or
    ``OSError`` (file not found/unreadable)."""
    text = Path(script_path).read_text(encoding="utf-8")
    return parse_required_task_names(text)


@dataclass(frozen=True)
class TaskLivenessReport:
    """Diff of a live stack-margin reading against the expected task set.

    ``dead``: registered (a stack_margin_register() call site exists and
    fired) but ``alive=False`` -- task creation failed on THIS boot, or the
    task was created and has since deleted itself unexpectedly. This is the
    class of failure ``check_stack_margin_registration.ps1`` cannot see: it
    only proves the call site exists in source, not that it ran.

    ``absent``: named in ``$requiredNames`` but never appeared in the live
    reading at all -- either the board's firmware predates that task, or a
    code regression dropped its stack_margin_register() call site (which
    ``check_stack_margin_registration.ps1`` would also catch on a fresh
    build/checkout, but a board can be running older firmware than the tree
    this tool runs from).

    ``extra``: alive tasks the live reading reported that are not in the
    expected set -- informational only, e.g. a newly added task this
    module's copy of the list has not caught up with yet.
    """

    expected: "tuple[str, ...]"
    alive: "tuple[str, ...]"
    dead: "tuple[str, ...]"
    absent: "tuple[str, ...]"
    extra: "tuple[str, ...]"

    @property
    def ok(self) -> bool:
        """False if any expected task is dead or absent."""
        return not self.dead and not self.absent

    def describe(self) -> str:
        lines = [
            f"task liveness: {len(self.alive)}/{len(self.expected)} expected task(s) alive"
        ]
        if self.dead:
            lines.append(
                f"  DEAD (registered, not running -- task creation failed this boot): "
                f"{', '.join(sorted(self.dead))}"
            )
        if self.absent:
            lines.append(
                f"  ABSENT (never registered on this board -- older firmware or a "
                f"code regression): {', '.join(sorted(self.absent))}"
            )
        if self.extra:
            lines.append(
                f"  extra (alive, not in the expected list -- informational): "
                f"{', '.join(sorted(self.extra))}"
            )
        lines.append("  RESULT: " + ("ok" if self.ok else "FAIL -- see DEAD/ABSENT above"))
        return "\n".join(lines)


def check_task_liveness(
    entries: "Iterable[StackMarginEntry]", expected_names: "Iterable[str]"
) -> TaskLivenessReport:
    """Diff a live ``get_stack_margin()`` reading (``entries``) against
    ``expected_names`` (normally :func:`load_required_task_names`'s
    result). Pure -- no I/O."""
    expected = tuple(expected_names)
    by_name = {}
    for e in entries:
        # A live board is never expected to report the same name twice
        # (check_stack_margin_registration.ps1's duplicate-name guard
        # enforces this at the source level) -- last-one-wins on a
        # malformed reply rather than raising, since this module's job is
        # to report liveness, not to re-validate wire framing already
        # covered by devices_info.parse_stack_margin_page().
        by_name[e.name] = e
    expected_set = set(expected)
    alive = tuple(name for name in expected if name in by_name and by_name[name].alive)
    dead = tuple(name for name in expected if name in by_name and not by_name[name].alive)
    absent = tuple(name for name in expected if name not in by_name)
    extra = tuple(
        name for name, e in by_name.items() if e.alive and name not in expected_set
    )
    return TaskLivenessReport(
        expected=expected, alive=alive, dead=dead, absent=absent, extra=extra
    )
