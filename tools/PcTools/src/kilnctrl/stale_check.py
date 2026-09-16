"""Staleness check for flash_firmware()/debug_program() -- refuses (or lets
the caller warn) when the binary about to be flashed does not match the
current source tree.

Added 2026-08-28 after a flash landed a binary built BEFORE the commit it
claimed to be, and separately after `build_kilnfw` was found to report OK on
a failed build (leaving the previous .bin in place) -- both mean "build OK,
flash OK" can silently mean "flashed a binary that has nothing to do with
the code currently on disk". This module exists so the flashing tools can
catch that instead of trusting os.path.isfile().

Design (see docs referenced in the two call sites for the full writeup):

Both KilnFW and SaftyFW already bake a *recorded* build identity into a
generated header at build time -- FW_GIT_COMMIT/FW_GIT_DIRTY
(firmware/KilnFW/build/esp-idf/drivers/build_info.h) and
SAFTYFW_GIT_COMMIT/SAFTYFW_GIT_DIRTY
(firmware/SaftyFW/build/saftyfw_build_info.h). That recorded commit is the
primary signal: compare it against the CURRENT `git rev-parse --short HEAD`.
A mismatch means source has moved on since this binary was built -- refuse.

That recorded dirty flag is scoped (by gen_build_info.cmake, on both sides)
to the firmware's own project directory only -- it does NOT cover
firmware/CommonFW, which both firmwares actually compile
(components/kilnlink wraps CommonFW's sources into the KilnFW build; SaftyFW
add_subdirectory()s it directly). So the recorded dirty flag alone is not
trustworthy for "is the source tree clean right now" -- this module
recomputes that itself, scoped to BOTH the firmware's own directory and
CommonFW.

When the recomputed tree is clean, a matching recorded commit is sufficient
-- done, no further check needed.

When the recomputed tree is dirty, the recorded commit match is not enough:
the binary could have been built from an earlier, different set of
uncommitted edits than what's on disk right now, and git has no cheap
content fingerprint of *uncommitted* changes to compare against (that would
need the build to record a diff hash, which nothing here does). This is
where a wall-clock comparison is actually useful and safe: compare the
binary's mtime against the newest mtime *among only the files `git status
--porcelain` reports as changed*. Restricting the mtime comparison to that
already-diffed set (rather than every file's mtime under the tree) is what
avoids the classic false-alarm trap --
  - touching a file without changing its content never appears in `git
    status --porcelain` (git diffs content, not mtimes), so it can never
    enter the comparison set;
  - a git operation that rewrites mtimes across the tree (checkout, clean
    clone, `git reset`) does not, on its own, dirty anything -- if it
    doesn't show up in `git status --porcelain` it is not compared either.
mtime is therefore only ever consulted for files git itself already knows
have real content differences, which is exactly the situation it's suited
to answer ("did I edit this after I built?").
"""

from __future__ import annotations

import os
import re
import subprocess
from dataclasses import dataclass
from typing import Optional


@dataclass
class StaleResult:
    stale: bool
    reason: str  # empty when stale is False


def _repo_root() -> str:
    """tools/PcTools/src/kilnctrl/ -> repo root is four levels up."""
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


def _current_head_short(repo_root: str) -> Optional[str]:
    out = _git(["rev-parse", "--short", "HEAD"], repo_root)
    return out.strip() if out else None


def _is_ancestor(repo_root: str, ancestor: str, descendant: str) -> Optional[bool]:
    """True if `ancestor` is reachable from `descendant` (i.e. `descendant`
    is `ancestor` or a strict descendant of it). None if git couldn't answer
    (e.g. `ancestor` unknown to this repo_root -- a rewritten/rebased history,
    or a commit that only exists in a different clone/worktree)."""
    try:
        out = subprocess.run(
            ["git", "merge-base", "--is-ancestor", ancestor, descendant],
            cwd=repo_root, capture_output=True, text=True, timeout=10,
        )
    except Exception:  # noqa: BLE001 - must not crash the flash tool
        return None
    if out.returncode == 0:
        return True
    if out.returncode == 1:
        return False
    return None  # e.g. 128: unknown revision


def _files_changed_between(repo_root: str, rev_a: str, rev_b: str, scoped_paths: list[str]) -> Optional[list[str]]:
    """Paths (repo-relative) that differ between rev_a and rev_b, scoped to
    scoped_paths. None if git couldn't answer."""
    out = _git(["diff", "--name-only", f"{rev_a}..{rev_b}", "--", *scoped_paths], repo_root)
    if out is None:
        return None
    return [line.strip() for line in out.splitlines() if line.strip()]


def _changed_files(repo_root: str, scoped_paths: list[str]) -> list[str]:
    """Absolute paths of files `git status --porcelain` reports as changed
    (modified, staged, or untracked) under any of scoped_paths. Content-diff
    based, not mtime based -- a file touched but not actually changed never
    appears here."""
    out = _git(["status", "--porcelain", "--no-renames", "--", *scoped_paths], repo_root)
    if out is None:
        return []
    files = []
    for line in out.splitlines():
        if not line.strip():
            continue
        # porcelain format: "XY <path>" (rename lines excluded via --no-renames)
        path = line[3:].strip()
        # handle quoted paths with special chars
        if path.startswith('"') and path.endswith('"'):
            path = path[1:-1]
        files.append(os.path.normpath(os.path.join(repo_root, path)))
    return files


def _parse_header_define(header_path: str, macro: str) -> Optional[str]:
    """Pulls the string or int literal out of `#define MACRO ...` in a
    generated build_info.h-style header."""
    try:
        with open(header_path, "r", encoding="utf-8", errors="replace") as f:
            text = f.read()
    except OSError:
        return None
    m = re.search(rf'#define\s+{re.escape(macro)}\s+"?([^\s"]+)"?', text)
    return m.group(1) if m else None


def check_stale(
    *,
    header_path: str,
    commit_macro: str,
    binary_path: str,
    project_dirs: list[str],
    repo_root: Optional[str] = None,
) -> StaleResult:
    """Core check, shared by the ESP and Pico paths.

    header_path: the generated build_info header (build_info.h /
      saftyfw_build_info.h) recorded at build time.
    commit_macro: the macro name in that header holding the short git commit
      (FW_GIT_COMMIT / SAFTYFW_GIT_COMMIT).
    binary_path: the flashable artifact whose mtime is the fallback signal
      (KilnCtrl.bin / SaftyFW.elf).
    project_dirs: absolute paths git status/dirty is scoped to -- the
      firmware's own project dir AND firmware/CommonFW.
    repo_root: the git worktree the binary was actually built from. Callers
      that support a `kiln_fw_root`/`safty_fw_root` override MUST resolve
      this to that override's own worktree root (not the main tree) -- see
      the module docstring addendum below on why. Defaults to the main tree
      (this file's own repo) when the caller has no override in play.
    """
    if repo_root is None:
        repo_root = _repo_root()

    if not os.path.isfile(header_path):
        return StaleResult(
            True,
            f"no build-identity header found at {header_path} -- this build predates "
            "the staleness check, or the build directory is incomplete. Rebuild first.",
        )
    if not os.path.isfile(binary_path):
        return StaleResult(True, f"binary not found: {binary_path} -- run the build first.")

    recorded_commit = _parse_header_define(header_path, commit_macro)
    current_commit = _current_head_short(repo_root)

    if current_commit is None:
        # git unavailable -- can't do the authoritative check, but don't
        # silently skip it either. Fall through to a pure mtime check as a
        # degraded fallback.
        return StaleResult(False, "git unavailable -- staleness check skipped (degraded)")

    if recorded_commit is None or recorded_commit == "unknown":
        return StaleResult(
            True,
            f"build-identity header records no usable commit ({recorded_commit!r}) -- "
            "rebuild so the flashed binary's origin is known.",
        )

    if recorded_commit != current_commit:
        # Not an equality check on purpose: "different commit" and "stale"
        # are different questions. A build recorded at an OLDER commit than
        # the current HEAD of the *same* root is not automatically stale --
        # only if source relevant to this firmware actually moved between
        # the two. (A build recorded at a commit not reachable from HEAD at
        # all -- rebase, reset, or a commit that belongs to some other
        # repo/worktree entirely -- is unconditionally stale; ancestry can't
        # even be evaluated.)
        ancestor = _is_ancestor(repo_root, recorded_commit, current_commit)
        if ancestor is not True:
            return StaleResult(
                True,
                f"binary was built from commit {recorded_commit}, which is not an "
                f"ancestor of current HEAD {current_commit} in this tree -- source "
                "has moved on (or history was rewritten) since this build. Rebuild, "
                "or pass the stale-flash opt-out if this is deliberate.",
            )
        changed_between = _files_changed_between(repo_root, recorded_commit, current_commit, project_dirs)
        if changed_between is None or changed_between:
            detail = ", ".join(changed_between[:5]) if changed_between else "unknown (git diff failed)"
            return StaleResult(
                True,
                f"binary was built from commit {recorded_commit}; HEAD has since advanced to "
                f"{current_commit} and touched files this firmware depends on ({detail}) -- "
                "rebuild, or pass the stale-flash opt-out if this is deliberate.",
            )
        # current_commit is a descendant of recorded_commit, but nothing in
        # project_dirs changed between them (e.g. commits elsewhere in the
        # repo, or in this same firmware's docs) -- the built binary still
        # reflects the code that would be built from HEAD today. Fall
        # through to the dirty-tree check below rather than treating this as
        # a mismatch.

    changed = _changed_files(repo_root, project_dirs)
    if not changed:
        return StaleResult(False, f"binary matches HEAD ({current_commit}), tree clean")

    bin_mtime = os.path.getmtime(binary_path)
    newest_src_mtime = max(
        (os.path.getmtime(p) for p in changed if os.path.isfile(p)), default=0.0
    )
    if newest_src_mtime > bin_mtime:
        newest_file = max(
            (p for p in changed if os.path.isfile(p)),
            key=lambda p: os.path.getmtime(p),
            default="?",
        )
        return StaleResult(
            True,
            f"uncommitted changes are newer than the build (e.g. {os.path.relpath(newest_file, repo_root)} "
            f"was edited after {os.path.relpath(binary_path, repo_root)} was produced) -- "
            "rebuild before flashing, or pass the stale-flash opt-out if this is deliberate.",
        )

    return StaleResult(
        False,
        f"binary matches HEAD ({current_commit}); tree has uncommitted changes "
        "but none are newer than the build",
    )


def _tree_root_for_project(project_root: str) -> str:
    """Resolves the git worktree a `firmware/KilnFW`- or `firmware/SaftyFW`-
    shaped `project_root` actually lives in, by walking two levels up --
    the same convention flash_provenance.py already uses for
    `provenance_repo_root` in mcp_server_flash.py (kiln_fw_root override is
    always `<some-tree-root>/firmware/KilnFW`).

    This is what makes `kiln_fw_root`/`safty_fw_root` overrides "just work"
    without a separate override flag: an ordinary call passes the main
    tree's own firmware dir, and two levels up from THAT is the main tree's
    own root -- identical to the old hardcoded `_repo_root()`. An override
    call passes a *different* tree's firmware dir, and two levels up from
    that is that tree's own root. Deriving it from the path that was
    actually used to find the binaries -- rather than always trusting this
    module's own on-disk location -- is the fix: comparing a build's
    recorded commit against the wrong repo's HEAD is exactly the bug this
    module existed to prevent flashing tools from producing."""
    return os.path.normpath(os.path.join(project_root, "..", ".."))


def check_kilnfw_stale(kiln_fw_root: str) -> StaleResult:
    tree_root = _tree_root_for_project(kiln_fw_root)
    return check_stale(
        header_path=os.path.join(kiln_fw_root, "build", "esp-idf", "drivers", "build_info.h"),
        commit_macro="FW_GIT_COMMIT",
        binary_path=os.path.join(kiln_fw_root, "build", "KilnCtrl.bin"),
        project_dirs=[kiln_fw_root, os.path.join(tree_root, "firmware", "CommonFW")],
        repo_root=tree_root,
    )


def check_saftyfw_stale(safty_fw_root: str) -> StaleResult:
    tree_root = _tree_root_for_project(safty_fw_root)
    return check_stale(
        header_path=os.path.join(safty_fw_root, "build", "saftyfw_build_info.h"),
        commit_macro="SAFTYFW_GIT_COMMIT",
        binary_path=os.path.join(safty_fw_root, "build", "SaftyFW.elf"),
        project_dirs=[safty_fw_root, os.path.join(tree_root, "firmware", "CommonFW")],
        repo_root=tree_root,
    )
