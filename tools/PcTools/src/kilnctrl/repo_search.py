#!/usr/bin/env python3
"""repo_search.py -- a time-limited, output-capped repository text search.

WHY THIS EXISTS. Every search in this tree used to be hand-written as
``timeout 180 grep -r <pattern> <path>`` through a shell, which has three
recurring problems this module removes:

1. **The deadline was advisory.** A shell ``timeout`` that fires leaves the
   caller with a truncated stdout and an exit code nobody inspects, so a
   search that died halfway is indistinguishable from a search that honestly
   found nothing. That is the single most expensive failure mode here: "no
   matches" is a *conclusion*, and a timed-out search must never be allowed
   to look like one. :class:`SearchResult` carries ``timed_out`` as a
   first-class field and :func:`format_result` leads with it.

2. **The process could outlive the call.** ``timeout`` is not available in
   every shell this repo is driven from, and a grep over a tree that also
   holds ``C:/wt`` worktrees and multi-hundred-megabyte build directories can
   run for minutes. :func:`run_search` kills the child at the deadline
   (``Popen.kill`` after ``communicate(timeout=...)`` raises) rather than
   detaching from it.

3. **Full match text landed in the caller's context unasked.** The default
   mode here is therefore ``"files"`` (paths only); ``"count"`` gives
   per-file counts; ``"lines"`` -- the expensive one -- is opt-in and is the
   only mode that honours ``context``.

ENGINE. ripgrep if ``rg`` is on PATH, plain ``grep`` otherwise. Which one
actually ran is reported in every result, because the two differ in ways a
caller can see: ripgrep skips ``.gitignore``d files and binary files by
default, grep does not, so an empty ripgrep result and an empty grep result
are not quite the same statement. Nothing here shells out through a shell --
``Popen`` is handed an argv list, so a pattern containing shell metacharacters
is a pattern, not an injection.

Pure stdlib, no new dependency, and no board access: this module is unit
tested (tools/PcTools/tests/test_repo_search.py) with a fake engine, no
subprocess and no repository scan.
"""

from __future__ import annotations

import dataclasses
import os
import shutil
import subprocess
import time
from typing import Callable, List, Optional, Sequence, Tuple

#: Wall-clock default. Long enough for a full-tree grep on this machine,
#: short enough that a pathological pattern does not hold a tool call open.
DEFAULT_TIMEOUT_S = 30.0

#: Hard ceiling. A caller asking for more than this gets clamped (and told),
#: never silently granted -- an unbounded search is the thing this module
#: exists to make impossible.
MAX_TIMEOUT_S = 300.0

#: Result cap defaults. ``MAX_MAX_RESULTS`` is the ceiling on the ceiling.
DEFAULT_MAX_RESULTS = 200
MAX_MAX_RESULTS = 5000

#: Output modes, in increasing order of how much text they put in front of
#: the caller. ``files`` is the default on purpose.
MODES = ("files", "count", "lines")

#: Ceiling on ``context`` lines, which multiply the output of ``lines`` mode
#: by (2*context + 1) in the worst case.
MAX_CONTEXT = 20


class RepoSearchError(RuntimeError):
    """A search that could not be run at all: no engine, a bad mode, a path
    that does not exist. Distinct from a search that ran and found nothing."""


@dataclasses.dataclass
class SearchResult:
    """One completed (or deadline-killed) search.

    ``timed_out`` is deliberately separate from ``returncode``: a killed
    child's return code says nothing useful on either platform, and the
    whole point of this module is that "hit the deadline" is reportable.
    """

    engine: str
    engine_path: str
    mode: str
    argv: List[str]
    returncode: Optional[int]
    timed_out: bool
    timeout_s: float
    elapsed_s: float
    lines: List[str]
    total_lines: int
    truncated: bool
    max_results: int
    stderr: str

    @property
    def matched(self) -> bool:
        return self.total_lines > 0

    @property
    def complete(self) -> bool:
        """True only when the answer can be trusted as exhaustive: the
        engine finished on its own AND nothing was capped away."""
        return not self.timed_out and not self.truncated


def find_engine(which: Callable[[str], Optional[str]] = shutil.which) -> Tuple[str, str]:
    """Return ``(name, path)`` for the search engine to use.

    ripgrep wins when present. ``which`` is injectable so the tests can
    exercise both branches without depending on what happens to be
    installed on the machine running them.
    """
    rg = which("rg")
    if rg:
        return "rg", rg
    grep = which("grep")
    if grep:
        return "grep", grep
    raise RepoSearchError(
        "no search engine found: neither 'rg' (ripgrep) nor 'grep' is on PATH")


def clamp_timeout(timeout_s: Optional[float]) -> Tuple[float, Optional[str]]:
    """Clamp to ``(0, MAX_TIMEOUT_S]``, returning the value and a note when
    the caller's request was altered."""
    if timeout_s is None:
        return DEFAULT_TIMEOUT_S, None
    try:
        value = float(timeout_s)
    except (TypeError, ValueError):
        raise RepoSearchError(f"timeout_s must be a number, got {timeout_s!r}")
    if value <= 0:
        return DEFAULT_TIMEOUT_S, (
            f"timeout_s={timeout_s} is not positive; using the default "
            f"{DEFAULT_TIMEOUT_S:g}s")
    if value > MAX_TIMEOUT_S:
        return MAX_TIMEOUT_S, (
            f"timeout_s={value:g} exceeds the {MAX_TIMEOUT_S:g}s ceiling; clamped")
    return value, None


def clamp_max_results(max_results: Optional[int]) -> Tuple[int, Optional[str]]:
    """Clamp to ``(0, MAX_MAX_RESULTS]``, same reporting contract as
    :func:`clamp_timeout`."""
    if max_results is None:
        return DEFAULT_MAX_RESULTS, None
    try:
        value = int(max_results)
    except (TypeError, ValueError):
        raise RepoSearchError(f"max_results must be an integer, got {max_results!r}")
    if value <= 0:
        return DEFAULT_MAX_RESULTS, (
            f"max_results={max_results} is not positive; using the default "
            f"{DEFAULT_MAX_RESULTS}")
    if value > MAX_MAX_RESULTS:
        return MAX_MAX_RESULTS, (
            f"max_results={value} exceeds the {MAX_MAX_RESULTS} ceiling; clamped")
    return value, None


def build_argv(engine: str, engine_path: str, pattern: str, path: str,
               glob: str = "", ignore_case: bool = False, mode: str = "files",
               context: int = 0) -> List[str]:
    """Build the argv for one search. No shell, ever -- the pattern is an
    argument, so its metacharacters are the engine's business and nobody
    else's."""
    if not pattern:
        raise RepoSearchError("pattern must not be empty")
    if mode not in MODES:
        raise RepoSearchError(f"mode must be one of {', '.join(MODES)}, got {mode!r}")
    if context < 0 or context > MAX_CONTEXT:
        raise RepoSearchError(f"context must be 0..{MAX_CONTEXT}, got {context}")

    argv = [engine_path]
    if engine == "rg":
        argv += ["--color=never", "--no-messages"]
        if mode == "files":
            argv.append("--files-with-matches")
        elif mode == "count":
            argv.append("--count")
        else:
            argv += ["--line-number", "--no-heading"]
            if context:
                argv += ["--context", str(context)]
        if ignore_case:
            argv.append("--ignore-case")
        if glob:
            argv += ["--glob", glob]
    else:
        # -E so the pattern dialect is as close to ripgrep's as plain grep
        # gets; -r so a directory path behaves the same under both engines.
        argv += ["-r", "-E", "--no-messages"]
        if mode == "files":
            argv.append("-l")
        elif mode == "count":
            argv.append("-c")
        else:
            argv.append("-n")
            if context:
                argv += ["-C", str(context)]
        if ignore_case:
            argv.append("-i")
        if glob:
            argv.append(f"--include={glob}")
    argv += ["--", pattern, path or "."]
    return argv


def run_search(pattern: str, path: str = ".", glob: str = "",
               ignore_case: bool = False, mode: str = "files", context: int = 0,
               timeout_s: Optional[float] = None, max_results: Optional[int] = None,
               cwd: Optional[str] = None,
               engine: Optional[Tuple[str, str]] = None) -> Tuple[SearchResult, List[str]]:
    """Run one search and return ``(result, notes)``.

    ``notes`` holds any clamping messages, so a caller is told when its own
    arguments were altered rather than quietly getting different behaviour
    than it asked for.

    The deadline is enforced by killing the child: ``communicate(timeout=)``
    raising :class:`subprocess.TimeoutExpired` is followed by ``kill()`` and
    a second, unbounded ``communicate()`` to reap it. Whatever the engine
    had already written to the pipe is kept and returned -- flagged as
    incomplete, never as an answer.
    """
    notes: List[str] = []
    timeout, note = clamp_timeout(timeout_s)
    if note:
        notes.append(note)
    cap, note = clamp_max_results(max_results)
    if note:
        notes.append(note)

    search_path = path or "."
    probe = search_path if os.path.isabs(search_path) else os.path.join(cwd or os.getcwd(), search_path)
    if not os.path.exists(probe):
        raise RepoSearchError(f"path does not exist: {search_path}")

    name, exe = engine if engine else find_engine()
    argv = build_argv(name, exe, pattern, search_path, glob, ignore_case, mode, context)

    started = time.monotonic()
    proc = subprocess.Popen(argv, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    timed_out = False
    try:
        out, err = proc.communicate(timeout=timeout)
    except subprocess.TimeoutExpired:
        timed_out = True
        proc.kill()
        # Unbounded on purpose: the child is already killed, so this only
        # drains the pipes and reaps the zombie. Leaving it unreaped is how
        # a search outlives the call that started it.
        out, err = proc.communicate()
    elapsed = time.monotonic() - started

    text = (out or b"").decode("utf-8", errors="replace")
    all_lines = [ln for ln in text.splitlines() if ln.strip()]
    if mode == "count":
        # GNU grep -c prints EVERY searched file, including the ones with
        # zero matches -- which on this tree means hundreds of `path:0`
        # lines burying the handful that matter, and a `matches=` count that
        # is really a file count. ripgrep already prints only files that
        # matched; this makes grep agree with it, so the two engines answer
        # the same question.
        all_lines = [ln for ln in all_lines if not ln.rsplit(":", 1)[-1].strip() == "0"]
    truncated = len(all_lines) > cap
    return SearchResult(
        engine=name,
        engine_path=exe,
        mode=mode,
        argv=argv,
        returncode=None if timed_out else proc.returncode,
        timed_out=timed_out,
        timeout_s=timeout,
        elapsed_s=elapsed,
        lines=all_lines[:cap],
        total_lines=len(all_lines),
        truncated=truncated,
        max_results=cap,
        stderr=(err or b"").decode("utf-8", errors="replace").strip(),
    ), notes


def format_result(result: SearchResult, notes: Optional[Sequence[str]] = None) -> str:
    """Render a :class:`SearchResult` as the text a caller reads.

    The deadline and the cap are stated on the FIRST line when either
    fired, because the one thing this output must never do is let an
    incomplete search read as "nothing here".
    """
    head: List[str] = []
    if result.timed_out:
        head.append(
            f"INCOMPLETE: search hit the {result.timeout_s:g}s deadline and was killed. "
            f"{result.total_lines} result line(s) were produced before the kill -- this is "
            "NOT a complete answer, and an empty list here does NOT mean 'no matches'. "
            "Narrow `path`/`glob` or raise `timeout_s`.")
    if result.truncated:
        head.append(
            f"TRUNCATED: {result.total_lines} result line(s) matched; only the first "
            f"{result.max_results} are shown (max_results={result.max_results}).")

    status = "complete" if result.complete else "INCOMPLETE"
    head.append(
        f"engine={result.engine} mode={result.mode} matches={result.total_lines} "
        f"shown={len(result.lines)} elapsed={result.elapsed_s:.2f}s "
        f"limit={result.timeout_s:g}s status={status}")
    for note in notes or ():
        head.append(f"note: {note}")
    if result.stderr:
        first = result.stderr.splitlines()[0]
        head.append(f"stderr: {first}")
    if not result.lines and not result.timed_out:
        head.append("no matches")
    body = "\n".join(result.lines)
    return "\n".join(head) + ("\n" + body if body else "")
