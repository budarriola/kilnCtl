"""SEARCH tool -- a time-limited, output-capped repository grep.

Same shape as mcp_server_log_analysis.py: no board, no serial link, no
OpenOCD. It exists because searching this tree was being done as hand-written
``timeout 180 grep -r ...`` shell lines, which cannot report that they hit
their deadline and dump full match text into the caller's context whether or
not it was wanted. See repo_search.py's module docstring for the three
specific failure modes this closes.
"""
from __future__ import annotations

from mcpkit import workbench

from . import repo_search
from . import mcp_server as _srv


@_srv._tool()
def repo_grep(pattern: str, path: str = ".", glob: str = "", ignore_case: bool = False,
              mode: str = "files", context: int = 0,
              timeout_s: float = repo_search.DEFAULT_TIMEOUT_S,
              max_results: int = repo_search.DEFAULT_MAX_RESULTS) -> str:
    """Search the repository for ``pattern`` under a wall-clock deadline.

    Use this instead of writing ``timeout 180 grep -r ...`` through a shell.
    Two things it does that a hand-written shell grep cannot:

    * **It kills the search at the deadline** and says so, distinguishably.
      A timed-out search reports ``INCOMPLETE:`` on its first line and states
      that an empty list is NOT the same as "no matches" -- the single most
      expensive mistake available here is treating a truncated search as a
      conclusion. The child process never outlives the call.
    * **It does not dump match text by default.** ``mode`` picks how much
      output you get, cheapest first:

      - ``"files"`` (default) -- matching file paths only.
      - ``"count"`` -- ``path:count`` per matching file.
      - ``"lines"`` -- matching lines, with ``context`` lines of surrounding
        text (0..20, default 0). The expensive one; ask for it deliberately.

    ``path`` scopes the search (repo-relative, e.g. ``firmware/KilnFW``;
    absolute paths work too) and ``glob`` filters filenames (e.g. ``*.c``,
    ``*.kicad_sch``). ``ignore_case=True`` for a case-insensitive match.
    ``pattern`` is a regular expression (ERE under grep, ripgrep's default
    dialect under ripgrep) and is passed as an argv element, never through a
    shell, so its metacharacters cannot be interpreted as shell syntax.

    ``timeout_s`` defaults to 30 and is clamped to a hard 300s ceiling;
    ``max_results`` defaults to 200 result lines and is clamped to 5000.
    Both clamps are reported rather than applied silently, and hitting the
    result cap is reported as ``TRUNCATED:``.

    Uses ripgrep when ``rg`` is on PATH and plain ``grep`` otherwise; every
    result names which one ran, because they differ in a way you can see --
    ripgrep skips ``.gitignore``d and binary files by default, grep does not,
    so "no matches" from the two is not quite the same statement.
    """
    result, notes = repo_search.run_search(
        pattern=pattern, path=path, glob=glob, ignore_case=ignore_case, mode=mode,
        context=context, timeout_s=timeout_s, max_results=max_results,
        cwd=workbench.repo_root(),
    )
    return repo_search.format_result(result, notes)
