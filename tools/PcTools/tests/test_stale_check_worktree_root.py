#!/usr/bin/env python3
"""Regression test for stale_check.py resolving the WRONG repo's HEAD when a
`kiln_fw_root`/`safty_fw_root` override points at a separate git worktree.

THE BUG: check_kilnfw_stale()/check_saftyfw_stale() compared the recorded
build-identity commit (FW_GIT_COMMIT/SAFTYFW_GIT_COMMIT, baked into the
generated header at build time) against `git rev-parse --short HEAD` of
this file's OWN on-disk location (stale_check._repo_root(), i.e. the main
tree) -- unconditionally, even when the binaries actually came from a
`kiln_fw_root` override pointing at an entirely different worktree (the
sanctioned "build from a clean git worktree at HEAD" workflow used whenever
the main tree carries another session's foreign WIP). A worktree that is
AHEAD of the main tree -- the normal case for that workflow -- was flagged
stale even though the binary exactly matches its own worktree's HEAD.

This test builds two independent, unrelated git repos (never the real
kilnCtl repo) to prove the check resolves HEAD from the override tree, not
from wherever stale_check.py itself happens to live:

  - `override_tree`: a synthetic worktree. Its own HEAD is what a
    build_info.h recorded at build time would show.
  - `unrelated_tree`: stands in for "wherever stale_check.py's hardcoded
    _repo_root() would have pointed" -- a completely different repo with
    unrelated commit hashes, so a check that (bug) ignores the override and
    consults some other tree's HEAD is guaranteed to see a hash mismatch
    that cannot even be resolved as an ancestor, and refuse.

check_kilnfw_stale(kiln_fw_root=<override_tree>/firmware/KilnFW) must find
the recorded commit == that OVERRIDE tree's own current HEAD, and must
therefore report stale=False. Against the pre-fix code, which always
consulted stale_check._repo_root() (the real kilnCtl checkout this test
runs inside), the recorded commit belongs to a repo git there knows nothing
about, so `git rev-parse --short HEAD` in the real repo returns some
unrelated hash and the two never match -> the buggy code returns
stale=True. This is a genuine behavioural difference (StaleResult.stale),
not an API/attribute mismatch.

Run with:
  python -m pytest tools/PcTools/tests/test_stale_check_worktree_root.py -q
"""
from __future__ import annotations

import os
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import stale_check  # noqa: E402


def _run_git(args, cwd):
    out = subprocess.run(
        ["git", *args], cwd=cwd, capture_output=True, text=True, timeout=15
    )
    assert out.returncode == 0, f"git {args} failed in {cwd}: {out.stderr}"
    return out.stdout.strip()


def _init_repo(path):
    os.makedirs(path, exist_ok=True)
    _run_git(["init", "-q"], path)
    _run_git(["config", "user.email", "test@example.com"], path)
    _run_git(["config", "user.name", "Test"], path)


def _commit(path, filename, content, message):
    full_path = os.path.join(path, filename)
    os.makedirs(os.path.dirname(full_path), exist_ok=True)
    with open(full_path, "w", encoding="utf-8") as f:
        f.write(content)
    _run_git(["add", filename], path)
    _run_git(["commit", "-q", "-m", message], path)
    return _run_git(["rev-parse", "--short", "HEAD"], path)


class StaleCheckWorktreeRootTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="stale_check_root_test_")
        self.addCleanup(self._tmp.cleanup)
        self.root = self._tmp.name

    def _make_override_tree(self):
        """A synthetic worktree, its HEAD several commits ahead of nothing
        in particular (unrelated to the real kilnCtl repo). Returns
        (tree_root, kiln_fw_root, recorded_commit)."""
        tree_root = os.path.join(self.root, "override_tree")
        _init_repo(tree_root)
        _commit(tree_root, "base.txt", "base\n", "base commit")
        recorded_commit = _commit(tree_root, "other.txt", "content\n", "advance HEAD")

        kiln_fw_root = os.path.join(tree_root, "firmware", "KilnFW")
        header_dir = os.path.join(kiln_fw_root, "build", "esp-idf", "drivers")
        os.makedirs(header_dir, exist_ok=True)
        with open(os.path.join(header_dir, "build_info.h"), "w", encoding="utf-8") as f:
            f.write(f'#define FW_GIT_COMMIT "{recorded_commit}"\n')
            f.write('#define FW_GIT_DIRTY 0\n')

        build_dir = os.path.join(kiln_fw_root, "build")
        with open(os.path.join(build_dir, "KilnCtrl.bin"), "wb") as f:
            f.write(b"fake-binary")

        return tree_root, kiln_fw_root, recorded_commit

    def test_override_tree_matching_its_own_head_is_not_stale(self):
        """The core regression: a binary whose recorded commit equals the
        OVERRIDE tree's own current HEAD must not be flagged stale, even
        though that commit hash is meaningless to whatever repo
        stale_check.py's hardcoded fallback root would otherwise consult."""
        tree_root, kiln_fw_root, recorded_commit = self._make_override_tree()

        result = stale_check.check_kilnfw_stale(kiln_fw_root)

        self.assertFalse(
            result.stale,
            f"expected not-stale (binary matches its own worktree's HEAD "
            f"{recorded_commit}), got stale=True: {result.reason}",
        )

    def test_override_tree_advanced_past_recorded_commit_without_fw_changes_is_not_stale(self):
        """A worktree that has moved past the recorded commit, but only via
        commits that never touched firmware/KilnFW or firmware/CommonFW, is
        a descendant -- not stale. This is the 'newer than main is not the
        same as stale' distinction from the ancestry-based comparison."""
        tree_root, kiln_fw_root, recorded_commit = self._make_override_tree()
        # Advance HEAD with a commit that touches neither kiln_fw_root nor
        # firmware/CommonFW.
        os.makedirs(os.path.join(tree_root, "docs"), exist_ok=True)
        _commit(tree_root, os.path.join("docs", "unrelated.txt"), "notes\n", "unrelated doc commit")

        result = stale_check.check_kilnfw_stale(kiln_fw_root)

        self.assertFalse(
            result.stale,
            f"a descendant commit that touched nothing this firmware depends on "
            f"must not be stale: {result.reason}",
        )

    def test_override_tree_advanced_with_firmware_changes_is_stale(self):
        """Sanity check the fix doesn't just always say not-stale: a
        worktree that advanced past the recorded commit WITH a change under
        firmware/KilnFW itself must still be flagged stale."""
        tree_root, kiln_fw_root, recorded_commit = self._make_override_tree()
        _commit(
            tree_root,
            os.path.join("firmware", "KilnFW", "App", "new_source.c"),
            "// new code\n",
            "actual firmware change after the recorded build",
        )

        result = stale_check.check_kilnfw_stale(kiln_fw_root)

        self.assertTrue(
            result.stale,
            "a real source change under the firmware dir, made after the "
            "recorded build commit, must be flagged stale",
        )


if __name__ == "__main__":
    unittest.main()
