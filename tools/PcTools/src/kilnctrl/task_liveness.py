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
   ``tools/check_stack_margin_registration.ps1`` (:func:`load_required_task_names`,
   :func:`load_required_task_specs`), so the expected-task list lives in
   exactly one place -- that script's own source -- rather than being copied
   into a second, driftable Python list.
2. Diff a live :class:`~kilnctrl.devices_info.StackMarginEntry` list against
   that expected set (:func:`check_task_liveness`), producing
   :class:`TaskLivenessReport`.

Not every required task is expected to be alive on every boot. The
``$requiredNames`` list mixes three different lifecycles with the plain
long-lived-service one:

- ``pico_auto_update`` self-deletes once it has a verdict (boot-once) --
  DEAD after boot is normal; never having registered at all (ABSENT) is
  still a fault (a code regression dropped the call site).
- ``gpio_probe``/``i2c_owner_ns2009`` only get created under a build config
  or a runtime hardware probe (config-conditional) -- DEAD or ABSENT is
  informational, not a fault, on a board/build that doesn't have that
  config/hardware.
- ``ota_pico_rollback``/``recovery_exit``/``ota_rollback_reboot`` are
  transient tasks an HTTP handler creates on demand -- DEAD or ABSENT
  between requests is normal, informational only.

Rather than a second, driftable Python classification list, each name in
``$requiredNames`` carries its own tag as a trailing PowerShell comment on
the SAME source line, e.g. ``"pico_auto_update",  # liveness: boot-once`` --
one source of truth, same discipline as the required-name list itself. A
name with no tag comment defaults to ``always`` (a plain long-lived
service: DEAD or ABSENT is always a fault). See :data:`VALID_LIVENESS_TAGS`.

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

#: Matches a "liveness: <tag>" trailing comment on a $requiredNames entry
#: line, found ANYWHERE in the comment text (not anchored to its start) --
#: an entry like "b",  # note # liveness: config still carries a real tag
#: rather than silently defaulting to "always". Case-sensitive on purpose --
#: a typo'd tag must fail loud (see parse_required_task_specs()) rather than
#: silently falling back to "always" and reporting a real by-design gap as a
#: fault.
_LIVENESS_TAG_RE = re.compile(r'liveness:\s*([A-Za-z][A-Za-z0-9_-]*)')

#: The one always-default tag plus the three by-design exceptions
#: check_task_liveness()/TaskLivenessReport.describe() understand. See this
#: module's docstring for what each one means.
VALID_LIVENESS_TAGS = ("always", "config", "on-demand", "boot-once")

#: Tags whose ABSENT (never registered on this board at all) is still a
#: fault -- "always" because that's the plain case, "boot-once" because a
#: task that runs once at every boot and self-deletes must still have
#: registered at some point; only "config"/"on-demand" tolerate a real
#: never-appeared.
_ABSENT_IS_FAULT_TAGS = ("always", "boot-once")


@dataclass(frozen=True)
class TaskSpec:
    """One ``$requiredNames`` entry: its task name and its liveness tag
    (one of :data:`VALID_LIVENESS_TAGS`)."""

    name: str
    tag: str


class TaskLivenessParseError(Exception):
    """Raised when ``$requiredNames`` cannot be found or parsed out of
    check_stack_margin_registration.ps1 -- fail loud rather than silently
    reporting zero required tasks (which would make every board look
    perfectly alive)."""


def default_check_script_path(repo_root: "str | Path") -> Path:
    """``repo_root``/``tools/check_stack_margin_registration.ps1``."""
    return Path(repo_root) / DEFAULT_CHECK_SCRIPT_RELPATH


def _find_required_names_block_lines(script_text: str) -> "tuple[int, int]":
    """Return ``(start_idx, end_idx)`` (inclusive, 0-based) into
    ``script_text.splitlines()`` spanning the ``$requiredNames = @( ... )``
    block, found on the RAW (comment-intact) text -- callers need the raw
    lines to recover each entry's trailing ``# liveness: ...`` tag, which a
    whole-file comment strip would otherwise erase before it is ever seen.

    The closing paren is found by comment-stripping each candidate line
    individually (never the whole file) and looking for the first ``)`` at
    or after the ``$requiredNames`` line -- safe because a $requiredNames
    array literal never nests a parenthesized expression inside itself, so
    the first real ``)`` after the opening ``@(`` is always the matching
    one, exactly as the original single-shot regex assumed.
    """
    raw_lines = script_text.splitlines()
    start_idx = None
    for i, line in enumerate(raw_lines):
        if "$requiredNames" in line and "@(" in line:
            start_idx = i
            break
    if start_idx is None:
        raise TaskLivenessParseError(
            "could not find a '$requiredNames = @( ... )' array literal -- "
            "check_stack_margin_registration.ps1 may have renamed or "
            "restructured this variable; update task_liveness.py's parser."
        )
    end_idx = None
    for i in range(start_idx, len(raw_lines)):
        code_part = raw_lines[i].split("#", 1)[0]
        if ")" in code_part:
            end_idx = i
            break
    if end_idx is None:
        raise TaskLivenessParseError(
            "'$requiredNames = @(' has no closing ')' -- the file may be "
            "truncated or malformed."
        )
    return start_idx, end_idx


def parse_required_task_specs(script_text: str) -> "tuple[TaskSpec, ...]":
    """Extract the ``$requiredNames`` array literal's entries, in source
    order, de-duplicated while preserving first occurrence, each carrying
    its liveness tag (:data:`VALID_LIVENESS_TAGS`) from a trailing
    ``# liveness: <tag>`` comment on the SAME source line -- defaulting to
    ``"always"`` when a line has no such comment.

    Raises :class:`TaskLivenessParseError` if the ``$requiredNames = @( ... )``
    block cannot be found, if it parses to zero names, or if a
    ``# liveness: ...`` comment names a tag outside :data:`VALID_LIVENESS_TAGS`
    (fail loud on a typo'd tag rather than silently defaulting it to
    "always" and reporting a by-design gap as a fault).
    """
    start_idx, end_idx = _find_required_names_block_lines(script_text)
    raw_lines = script_text.splitlines()
    specs: "list[TaskSpec]" = []
    seen = set()
    for line in raw_lines[start_idx:end_idx + 1]:
        if "#" in line:
            code_part, comment_part = line.split("#", 1)
        else:
            code_part, comment_part = line, ""
        names_on_line = []
        for m in _QUOTED_STRING_RE.finditer(code_part):
            name = m.group(1) if m.group(1) is not None else m.group(2)
            names_on_line.append(name)
        tag = "always"
        tag_match = _LIVENESS_TAG_RE.search(comment_part)
        if tag_match:
            if len(names_on_line) > 1:
                raise TaskLivenessParseError(
                    f"a '# liveness: ...' tag comment applies to a single "
                    f"entry, but line {line.strip()!r} carries {len(names_on_line)} "
                    f"quoted names -- put each tagged name on its own line."
                )
            if len(names_on_line) == 0:
                raise TaskLivenessParseError(
                    f"a '# liveness: ...' tag comment was found on line "
                    f"{line.strip()!r}, but that line has no quoted task "
                    f"name for it to apply to."
                )
            tag = tag_match.group(1)
            if tag not in VALID_LIVENESS_TAGS:
                raise TaskLivenessParseError(
                    f"unknown liveness tag {tag!r} on line {line.strip()!r} -- "
                    f"valid tags are {VALID_LIVENESS_TAGS}."
                )
        for name in names_on_line:
            if name not in seen:
                seen.add(name)
                specs.append(TaskSpec(name=name, tag=tag))
    if not specs:
        raise TaskLivenessParseError(
            "'$requiredNames' block matched but contained no quoted string "
            "literals -- the parser has gone blind to the real "
            "required-task list."
        )
    return tuple(specs)


def parse_required_task_names(script_text: str) -> "tuple[str, ...]":
    """Extract just the names from :func:`parse_required_task_specs`, in
    source order -- kept for callers that don't need the liveness tags.

    Raises :class:`TaskLivenessParseError` under the same conditions as
    :func:`parse_required_task_specs`.
    """
    return tuple(spec.name for spec in parse_required_task_specs(script_text))


def load_required_task_specs(script_path: "str | Path") -> "tuple[TaskSpec, ...]":
    """Read and parse ``script_path`` (normally
    ``tools/check_stack_margin_registration.ps1``). Raises
    :class:`TaskLivenessParseError` (missing/unparsable variable, or an
    unknown liveness tag) or ``OSError`` (file not found/unreadable)."""
    text = Path(script_path).read_text(encoding="utf-8")
    return parse_required_task_specs(text)


def load_required_task_names(script_path: "str | Path") -> "tuple[str, ...]":
    """Read and parse ``script_path``, names only -- see
    :func:`load_required_task_specs`."""
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

    ``dead``/``absent`` above are the FULL sets, undivided by tag -- kept for
    backward compatibility with callers that only care "is this name not
    currently up". ``fault_dead``/``fault_absent`` are the subsets that
    ``ok`` and callers like ``capability_preflight.py`` actually treat as a
    problem: an ``always``-tagged (untagged) name being DEAD or ABSENT, or a
    ``boot-once``-tagged name being ABSENT (it must have registered at some
    point even though DEAD is its normal post-boot state).
    ``info_dead``/``info_absent`` are the remainder -- by-design gaps
    (``config``/``on-demand``/``boot-once`` DEAD, ``config``/``on-demand``
    ABSENT) worth surfacing but never worth refusing a run over.
    """

    expected: "tuple[str, ...]"
    alive: "tuple[str, ...]"
    dead: "tuple[str, ...]"
    absent: "tuple[str, ...]"
    extra: "tuple[str, ...]"
    #: name -> liveness tag for every name in ``expected``. Untagged names
    #: (the plain ``check_task_liveness(entries, names)`` call with a bare
    #: name iterable, e.g. existing tests/callers) default every name to
    #: ``"always"``, preserving the pre-tag behavior exactly.
    tags: "dict[str, str]" = field(default_factory=dict)
    fault_dead: "tuple[str, ...]" = ()
    fault_absent: "tuple[str, ...]" = ()
    info_dead: "tuple[str, ...]" = ()
    info_absent: "tuple[str, ...]" = ()

    @property
    def ok(self) -> bool:
        """False if any ``always``-tagged (default) task is dead or absent,
        or any ``boot-once``-tagged task is absent. A ``config``/
        ``on-demand`` task being dead or absent, or a ``boot-once`` task
        being dead (its normal post-boot state), is informational only and
        never fails this property."""
        return not self.fault_dead and not self.fault_absent

    def describe(self) -> str:
        lines = [
            f"task liveness: {len(self.alive)}/{len(self.expected)} expected task(s) alive"
        ]
        if self.fault_dead:
            lines.append(
                f"  DEAD (registered, not running -- task creation failed this boot): "
                f"{', '.join(sorted(self.fault_dead))}"
            )
        if self.fault_absent:
            lines.append(
                f"  ABSENT (never registered on this board -- older firmware or a "
                f"code regression): {', '.join(sorted(self.fault_absent))}"
            )
        if self.info_dead:
            lines.append(
                f"  dead, but by design (config/hardware-conditional or on-demand, or a "
                f"one-shot boot task that has since self-deleted -- not a fault): "
                f"{', '.join(sorted(self.info_dead))}"
            )
        if self.info_absent:
            lines.append(
                f"  absent, but by design (config/hardware-conditional or on-demand -- "
                f"not a fault): {', '.join(sorted(self.info_absent))}"
            )
        if self.extra:
            lines.append(
                f"  extra (alive, not in the expected list -- informational): "
                f"{', '.join(sorted(self.extra))}"
            )
        lines.append("  RESULT: " + ("ok" if self.ok else "FAIL -- see DEAD/ABSENT above"))
        return "\n".join(lines)


def check_task_liveness(
    entries: "Iterable[StackMarginEntry]",
    expected_names: "Iterable[str]",
    tags: "dict[str, str] | None" = None,
) -> TaskLivenessReport:
    """Diff a live ``get_stack_margin()`` reading (``entries``) against
    ``expected_names`` (normally :func:`load_required_task_names`'s
    result, or the names out of :func:`load_required_task_specs`). Pure --
    no I/O.

    ``tags``: optional ``{name: tag}`` map (normally built from
    :func:`load_required_task_specs`'s result) classifying each expected
    name per :data:`VALID_LIVENESS_TAGS`. A name with no entry here (or
    when ``tags`` is omitted entirely) defaults to ``"always"`` -- the
    pre-tag behavior, so every existing caller that only ever passed bare
    names keeps refusing on any dead/absent name exactly as before.
    """
    expected = tuple(expected_names)
    tags = dict(tags) if tags else {}
    resolved_tags = {name: tags.get(name, "always") for name in expected}
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
    fault_dead = tuple(name for name in dead if resolved_tags[name] == "always")
    info_dead = tuple(name for name in dead if resolved_tags[name] != "always")
    fault_absent = tuple(name for name in absent if resolved_tags[name] in _ABSENT_IS_FAULT_TAGS)
    info_absent = tuple(name for name in absent if resolved_tags[name] not in _ABSENT_IS_FAULT_TAGS)
    return TaskLivenessReport(
        expected=expected, alive=alive, dead=dead, absent=absent, extra=extra,
        tags=resolved_tags, fault_dead=fault_dead, fault_absent=fault_absent,
        info_dead=info_dead, info_absent=info_absent,
    )
