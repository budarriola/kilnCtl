"""Shared JSONL line iterator.

Five call sites across this package (``coupling_pair_log.py``,
``http_capture_log.py``, ``link_hub.py`` x2, ``relay_ku_tu_check.py``) each
hand-rolled their own ``for line in source: line = line.strip(); ...
json.loads(line)`` loop, and each one picked a *different* answer for what
happens to a malformed line: some skip it silently, one lets the exception
propagate, one logs it and skips. That divergence was never a deliberate
design decision anywhere -- it was five copies drifting independently.
``iter_jsonl`` centralizes the loop (blank-line skipping, ``.strip()``,
``json.loads``) while keeping every caller's chosen error behaviour
explicit and unchanged via the ``on_error`` argument.
"""

from __future__ import annotations

import json
import os
from typing import Any, Callable, Iterable, Iterator, Union

#: "skip" -- swallow a JSONDecodeError and move to the next line (no log).
#: "raise" -- let the JSONDecodeError propagate to the caller.
#: a callable -- called with the raw (stripped) line for the caller to log
#:   or otherwise react, then the line is skipped.
OnError = Union[str, Callable[[str], None]]


def iter_jsonl(
    source: Union[str, "os.PathLike[str]", Iterable[str]],
    on_error: OnError = "skip",
    with_line: bool = False,
) -> Iterator[Any]:
    """Yield one parsed JSON value per non-blank line of ``source``.

    ``source`` is either a filesystem path (opened as UTF-8 text and closed
    when the iterator is exhausted) or an already-open iterable of text
    lines -- e.g. a socket's ``makefile()``, as ``link_hub.py`` uses, or any
    other line iterator a caller already holds open.

    ``on_error`` controls what happens to a line that fails ``json.loads``:
      - ``"skip"`` (the default): silently skip the line and continue --
        matches ``coupling_pair_log._load_jsonl`` and
        ``http_capture_log.parse_http_capture_jsonl``'s prior behaviour.
      - ``"raise"``: let the ``json.JSONDecodeError`` propagate --
        matches ``relay_ku_tu_check._read_jsonl``'s prior behaviour (it never
        caught the exception at all).
      - a callable: called with the raw stripped line (``str``) so the
        caller can log it -- matches ``link_hub.py``'s
        ``_ClientHandler.run()`` loop, which logs
        ``"hub: dropping malformed request line: %r"`` before continuing.
      In every case (except ``"raise"``) the malformed line is then skipped,
      never yielded.

    ``with_line=True`` yields ``(line, obj)`` pairs instead of just ``obj``,
    with ``obj`` set to ``None`` for a line that failed to parse (and was not
    raised) -- needed by callers that fall back to re-parsing the raw text
    a different way, e.g. ``coupling_pair_log.load_thermo_samples_any_format``,
    which tries the ``{"t","s"}`` envelope first and, on failure, retries
    from the first ``{`` in the same line.
    """
    if isinstance(source, (str, os.PathLike)):
        with open(source, "r", encoding="utf-8") as fh:
            yield from iter_jsonl(fh, on_error=on_error, with_line=with_line)
        return

    for raw in source:
        line = raw.strip()
        if not line:
            continue
        try:
            obj = json.loads(line)
        except json.JSONDecodeError:
            if on_error == "raise":
                raise
            if callable(on_error):
                on_error(line)
            if with_line:
                yield line, None
            continue
        if with_line:
            yield line, obj
        else:
            yield obj
