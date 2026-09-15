"""Flash-time working-tree provenance: WHAT rode along in a flash, not just
whether the tree was dirty.

Added 2026-09-04 after an incident: an agent authorised only to build+flash
for a display/watchdog diagnosis picked up ANOTHER session's uncommitted,
in-progress `zones_config_*` schema-migration edits (shared working tree,
several sessions/agents normal for this project) and flashed them. It landed
correctly, but that was luck, not process -- `get_fw_version()`'s `tree:
dirty` is a single bit, so nobody could tell WHICH uncommitted changes went
into the image, before or after the fact.

Two things live here:

1. `capture_tree_state()` -- records `git status --porcelain` (the full
   repo, not scoped to one firmware's project dirs the way stale_check.py's
   staleness comparison is) plus HEAD, at the moment of a flash. This is
   pure information, not a gate: every dirty file is listed regardless of
   sensitivity, so "what was actually on the board at 14:53" is answerable
   later from the persisted JSON (see `write_provenance_json`) even when
   nothing was refused.

2. `decide_guard()` -- the ONE guard shape this module implements: refuse
   (return a non-None reason) only when the dirty set touches a small,
   explicit SENSITIVE list (config-schema/migration code, safety config).
   Two other shapes were considered and rejected:

   - `allow_dirty` default-True/False toggle on "any dirty file": this
     project's normal state IS a dirty tree (several sessions share one
     working tree by design -- see CLAUDE.md), so a default-refuse-on-any-
     dirt would block routine work within a day and get permanently
     disabled, which is worse than not existing.
   - caller-declares-its-own-scope ("I'm only touching display code, ignore
     everything else"): this trusts the caller to know and honestly state
     the full uncommitted footprint of every session sharing the tree at
     that instant, which is exactly the piece of information the caller
     did NOT have in the incident this module exists to prevent -- the
     agent doing the display diagnosis had no way to know another session
     had zones_config edits in flight. A declared-scope guard would have
     let that flash straight through, because the agent would have
     (honestly) declared "display code" and the guard would have only
     checked declared-vs-dirty, not dirty-vs-sensitive.

   A fixed sensitive-path list catches the incident regardless of what the
   caller believes it is doing, and stays silent on the other 99% of dirty
   files (test tweaks, docs, unrelated drivers) that make "any dirty file"
   unworkable here. It is bypassable only by an explicit, named opt-in
   (`allow_sensitive_dirty=True`) that shows up in the tool result and the
   persisted JSON -- there is no quiet default that erodes into "everyone
   just sets it True forever" the way a blanket dirty-refusal would.
"""
from __future__ import annotations

import json
import os
import re
import subprocess
import time
from dataclasses import dataclass, field
from typing import Optional


#: Repo-relative path fragments (matched case-insensitively, substring/regex)
#: that mark a dirty file as sensitive enough to refuse an unacknowledged
#: flash over. Deliberately narrow and named, not "anything under
#: firmware/" -- see module docstring for why a broad dirty-tree gate is
#: the wrong shape for this project. Extend this list, don't widen it into
#: a directory-level match, if another schema/migration/safety surface
#: needs the same protection.
SENSITIVE_PATTERNS = [
    r"zones?_config",       # zone-config schema + its accessors/migration
    r"_migrat",             # *_migrate.c / *_migration.* generically
    r"safety_cfg",          # safety config store/http (KilnFW-side)
    r"safety_link",         # safety UART link framing/protocol
    r"kiln_cfg_store",      # top-level kiln config persistence
    r"schema",              # anything self-describing as a schema change
]
_SENSITIVE_RE = re.compile("|".join(SENSITIVE_PATTERNS), re.IGNORECASE)


@dataclass
class TreeState:
    timestamp: float
    head: Optional[str]
    dirty_files: list[str] = field(default_factory=list)     # repo-relative, all of them
    sensitive_files: list[str] = field(default_factory=list)  # subset matching SENSITIVE_PATTERNS
    git_available: bool = True

    @property
    def dirty(self) -> bool:
        return bool(self.dirty_files)


def classify_dirty(files: list[str]) -> list[str]:
    """Returns the subset of repo-relative paths matching SENSITIVE_PATTERNS."""
    return [f for f in files if _SENSITIVE_RE.search(f)]


def _repo_root() -> str:
    """tools/PcTools/src/kilnctrl/ -> repo root is four levels up. Same
    convention as stale_check._repo_root()."""
    return os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", "..", ".."))


def _git(args: list[str], cwd: str) -> Optional[str]:
    try:
        out = subprocess.run(
            ["git", *args], cwd=cwd, capture_output=True, text=True, timeout=10
        )
    except Exception:  # noqa: BLE001 - git missing/unavailable must not crash the flash tool
        return None
    if out.returncode != 0:
        return None
    return out.stdout


def _parse_porcelain(out: str) -> list[str]:
    files = []
    for line in out.splitlines():
        if not line.strip():
            continue
        path = line[3:].strip()
        if path.startswith('"') and path.endswith('"'):
            path = path[1:-1]
        # rename lines look like "old -> new"; keep the new path
        if " -> " in path:
            path = path.split(" -> ", 1)[1]
        files.append(path)
    return files


def capture_tree_state(repo_root: Optional[str] = None) -> TreeState:
    """Records the FULL repo's dirty-file list (unscoped -- deliberately
    wider than stale_check.py's per-firmware scoping, because the incident
    this exists for was a cross-session file nobody flashing had any
    reason to scope to) plus HEAD, at the moment this is called. Never
    raises: git-unavailable degrades to an empty, flagged result rather
    than blocking a flash on tooling being missing."""
    root = repo_root or _repo_root()
    head_out = _git(["rev-parse", "--short", "HEAD"], root)
    status_out = _git(["status", "--porcelain", "--no-renames"], root)
    if head_out is None and status_out is None:
        return TreeState(timestamp=time.time(), head=None, git_available=False)
    dirty = _parse_porcelain(status_out) if status_out is not None else []
    sensitive = classify_dirty(dirty)
    return TreeState(
        timestamp=time.time(),
        head=head_out.strip() if head_out else None,
        dirty_files=dirty,
        sensitive_files=sensitive,
    )


def decide_guard(state: TreeState, allow_sensitive_dirty: bool = False) -> Optional[str]:
    """Returns None if the flash may proceed, else a refusal message naming
    the sensitive files. Only sensitive dirt blocks -- an ordinary dirty
    tree (the normal state of this repo) always returns None."""
    if not state.sensitive_files or allow_sensitive_dirty:
        return None
    files_list = "\n".join(f"  - {f}" for f in state.sensitive_files)
    return (
        "refusing to flash: the working tree has UNCOMMITTED changes in "
        "sensitive files (config schema / migration / safety) that this "
        "flash would carry to the board, whether or not they are what you "
        "are working on:\n"
        f"{files_list}\n\n"
        "If these are someone else's in-progress edits (this repo's working "
        "tree is normally shared across sessions), commit/stash your own "
        "changes or coordinate before flashing -- do NOT just re-run with "
        "the override. If you have specifically reviewed these files and "
        "intend for them to be on the board, pass allow_sensitive_dirty=True."
    )


def format_report(state: TreeState) -> str:
    """Human-readable summary for a tool result -- always included, whether
    or not anything was refused, so an operator sees exactly what rode
    along."""
    if not state.git_available:
        return "provenance: WARNING -- git unavailable, could not record tree state"
    head = state.head or "unknown"
    if not state.dirty_files:
        return f"provenance: HEAD {head}, tree clean"
    lines = [f"provenance: HEAD {head}, {len(state.dirty_files)} uncommitted file(s):"]
    for f in state.dirty_files:
        marker = " [SENSITIVE]" if f in state.sensitive_files else ""
        lines.append(f"  - {f}{marker}")
    return "\n".join(lines)


#: Values written to the persisted JSON's "outcome" field. A refused attempt
#: and a successful flash used to be indistinguishable on disk -- both wrote
#: the same fields, so a session reading flash_provenance.json after the fact
#: had no way to tell "this is what got flashed" from "this is what got
#: refused". Added 2026-09-04 after a refused flash (sensitive zones_config_*
#: dirt riding along with an unrelated LVGL crash fix) sat unnoticed for five
#: hours: the fix never reached the board, the board kept panicking on the
#: bug the fix addressed, and nobody diagnosing it thought to doubt that the
#: refusal even happened, because the provenance file looked exactly like a
#: normal successful-flash record.
OUTCOME_PENDING = "pending"                    # tree captured, guard decision not yet known
OUTCOME_REFUSED_SENSITIVE_DIRTY = "refused_sensitive_dirty"
OUTCOME_REFUSED_STALE_BINARY = "refused_stale_binary"
OUTCOME_FLASHED_OK = "flashed_ok"
OUTCOME_FLASH_FAILED = "flash_failed"

REFUSED_OUTCOMES = frozenset({OUTCOME_REFUSED_SENSITIVE_DIRTY, OUTCOME_REFUSED_STALE_BINARY})


def write_provenance_json(
    state: TreeState,
    out_path: str,
    outcome: str = OUTCOME_PENDING,
    detail: Optional[str] = None,
    kiln_fw_root_override: Optional[str] = None,
) -> None:
    """Persists the capture to `out_path` (the caller decides the path; see
    elf_archive.kiln_provenance_path() -- since 2026-09-15 this is
    KilnFW/flash_provenance.json, a sibling of build/, not inside it) so
    "what was actually on the board at 14:53" is answerable later from disk,
    not just from a chat transcript. Overwrites -- this is a point-in-time
    snapshot of the LAST
    flash ATTEMPT from this build dir, not a log; the session log captures
    the running history via the normal _srv._session_log calls at each call
    site. Best-effort: a write failure must not block or fail the flash.

    `outcome` is the one field that used to be missing entirely -- callers
    are expected to write once per state transition (pending -> refused, or
    pending -> flashed_ok/flash_failed) so the file on disk always names
    what actually happened to the LAST attempt, not just what was dirty at
    the time. See OUTCOME_* constants above.

    `kiln_fw_root_override`: non-None when the image being flashed was built
    in a tree other than the main working tree (e.g. a git worktree checked
    out at HEAD, used to build cleanly while the main tree carries another
    session's WIP -- see flash_firmware()'s `kiln_fw_root` parameter). `state`
    passed in this case is already scoped to THAT tree (its own
    `git status --porcelain`/HEAD), not the main tree's -- this field just
    records which path produced it, so a later reader isn't misled into
    thinking `head`/`dirty_files` describe the main working tree."""
    try:
        os.makedirs(os.path.dirname(out_path), exist_ok=True)
        with open(out_path, "w", encoding="utf-8") as f:
            json.dump(
                {
                    "timestamp": state.timestamp,
                    "head": state.head,
                    "dirty_files": state.dirty_files,
                    "sensitive_files": state.sensitive_files,
                    "git_available": state.git_available,
                    "outcome": outcome,
                    "detail": detail,
                    "kiln_fw_root_override": kiln_fw_root_override,
                },
                f,
                indent=2,
            )
    except (OSError, ValueError):
        pass


def read_provenance_json(path: str) -> Optional[dict]:
    """Reads back a persisted provenance file, or None if it does not exist
    or is not parseable JSON. Never raises -- a missing/corrupt file is
    reported as "nothing recorded", not a tool failure."""
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def format_last_flash_warning(prov: Optional[dict]) -> Optional[str]:
    """Given a parsed flash_provenance.json, returns a loud warning string
    if the LAST recorded flash attempt was refused, else None. This is the
    "make a refusal loud" half of the fix: the refusal used to only exist as
    a return string handed back to whichever session asked, and a warning
    line in that session's own log file -- both gone the moment that session
    ended. Persisting the outcome (see write_provenance_json) means this can
    now be checked from a totally different, later session, by whatever
    reads board state next, rather than requiring anyone to have been
    watching at the time."""
    if not prov:
        return None
    outcome = prov.get("outcome")
    if outcome not in REFUSED_OUTCOMES:
        return None
    age_s = time.time() - prov.get("timestamp", 0)
    age_desc = f"{age_s / 3600:.1f}h ago" if age_s >= 0 else "at an unknown time"
    return (
        f"!!! LAST FLASH ATTEMPT WAS REFUSED ({outcome}) {age_desc}, head={prov.get('head')} !!!\n"
        f"    {prov.get('detail') or '(no detail recorded)'}\n"
        "    Whatever that flash was trying to put on the board never landed -- "
        "the board is still running whatever it booted before this attempt. "
        "Resolve the refusal (commit/stash the sensitive files, or rebuild) and "
        "flash again; do not assume this refusal was already handled by someone else."
    )


def describe_head_gap(board_commit: Optional[str], repo_root: Optional[str] = None) -> str:
    """Compares the firmware's self-reported git commit (from GET_FW_VERSION,
    embedded at build time) against this working tree's current HEAD, and
    says in one line whether they match, how far apart they are, or why that
    could not be determined.

    Nothing did this before: the board reports its own commit, the repo
    knows its own HEAD, and no tool put the two side by side -- a fix could
    sit committed on HEAD indefinitely while the board kept running the
    commit before it, with no signal anywhere short of manually comparing
    two hashes by hand. This is the cheap version of that comparison,
    surfaced in get_fw_version() (the board-state read already made on every
    tool call), not a new tool nobody would think to call.

    Best-effort like the rest of this module: `git rev-list --count` fails
    (short hash too short/long, board commit unknown to this clone, git
    unavailable) degrades to a plain "could not compare" line rather than
    raising -- a stale board is worse than an unclear one, but a crash here
    must not take down get_fw_version().
    """
    root = repo_root or _repo_root()
    head = _git(["rev-parse", "--short", "HEAD"], root)
    head = head.strip() if head else None
    if not board_commit or board_commit in ("?", ""):
        return "board/HEAD comparison: board did not report a commit"
    if head is None:
        return f"board/HEAD comparison: could not read local HEAD (board reports {board_commit})"
    if board_commit == head:
        return f"board/HEAD comparison: OK, board is running HEAD ({head})"
    # is board_commit an ancestor of HEAD? if so, count how far behind.
    count_out = _git(["rev-list", "--count", f"{board_commit}..HEAD"], root)
    if count_out is None:
        return (
            f"board/HEAD comparison: board reports {board_commit}, HEAD is {head} -- "
            "could not determine how many commits apart (commit not found in this "
            "clone's history, or ahead of HEAD -- check by hand)"
        )
    try:
        n = int(count_out.strip())
    except ValueError:
        return f"board/HEAD comparison: board reports {board_commit}, HEAD is {head} (count unavailable)"
    if n == 0:
        return f"board/HEAD comparison: board reports {board_commit}, HEAD is {head} (0 commits apart -- likely a short-hash collision, verify by hand)"
    return f"!!! board is {n} commit(s) behind HEAD: board={board_commit}, HEAD={head} !!!"
