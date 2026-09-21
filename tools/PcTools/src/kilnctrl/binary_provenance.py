"""Guards against measuring a host-test executable that is older than the
sources it claims to have been built from.

Added 2026-09-21 after `docs/audits/review_sim_fuzzy_commits_2026-09-13.md`:
`build_host_tests.ps1` rebuilt `kilnctl_sim_fuzzy_closedloop.exe` mid a
negative test for `ed854ac5` (source deliberately sabotaged), the source was
then hand-restored, and `git diff` came back empty -- but the poisoned .exe
sitting in the build directory was never rebuilt. `ba230bca` then read that
prebuilt binary directly instead of rebuilding, and measured six numbers from
sabotaged code that became a committed verdict reaching the project owner
before the audit caught it.

An empty `git diff` proves the *source* is restored; it says nothing about
build artifacts already on disk. This module is the mechanical version of
"never measure from a prebuilt binary whose provenance isn't established" --
see CLAUDE.md's "Firmware gotchas" section for the full incident.

Deliberately mtime-based, not content-hash based: the sources that matter are
already known at each call site (a handful of .c/.h files a given harness
links), and a wall-clock comparison is cheap, dependency-free, and exactly
matches the failure mode above (a source file edited/restored later than the
exe was produced).
"""

from __future__ import annotations

import glob
import os
from dataclasses import dataclass
from typing import Iterable, Optional


@dataclass
class BinaryStaleness:
    stale: bool
    reason: str  # empty when stale is False
    newest_source: Optional[str] = None


def check_binary_fresh(exe_path: str, source_globs: Iterable[str]) -> BinaryStaleness:
    """Pure check: does NOT raise. `source_globs` are glob patterns (absolute
    or relative to the current working directory) naming every source file
    the exe was built from. At least one glob must match at least one file,
    or the check itself is refused as unable to answer the question."""
    if not os.path.isfile(exe_path):
        return BinaryStaleness(True, f"binary not found: {exe_path} -- build it first.")

    matched: list[str] = []
    for pattern in source_globs:
        matched.extend(glob.glob(pattern, recursive=True))
    matched = sorted(set(p for p in matched if os.path.isfile(p)))

    if not matched:
        return BinaryStaleness(
            True,
            f"no source files matched {list(source_globs)!r} -- cannot establish "
            f"provenance for {exe_path}, refusing to treat it as fresh.",
        )

    exe_mtime = os.path.getmtime(exe_path)
    newest_src = max(matched, key=lambda p: os.path.getmtime(p))
    newest_src_mtime = os.path.getmtime(newest_src)

    if newest_src_mtime > exe_mtime:
        return BinaryStaleness(
            True,
            f"{exe_path} is older than {newest_src} -- rebuild before measuring "
            "anything from this binary. An empty `git diff` does not prove the "
            "binary on disk reflects current source; only a rebuild does.",
            newest_source=newest_src,
        )

    return BinaryStaleness(False, f"{exe_path} is newer than every matched source file", newest_source=newest_src)


def assert_binary_fresh(exe_path: str, source_globs: Iterable[str]) -> None:
    """Raises `RuntimeError` with a clear message when `exe_path` is stale
    relative to `source_globs` (or missing, or provenance can't be
    established at all). Call this immediately before reading any number out
    of a host-test executable -- never trust a path alone."""
    result = check_binary_fresh(exe_path, source_globs)
    if result.stale:
        raise RuntimeError(f"binary_provenance: {result.reason}")
