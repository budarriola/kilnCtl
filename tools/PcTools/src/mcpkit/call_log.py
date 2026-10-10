"""One timestamped line per MCP tool call, in a gitignored log.

``logs/mcp_calls/<port>_<UTC date>.log`` (port named per the shared-log rule).
Line: ``<UTC ISO time> <tool> <arg keys, comma-joined>``. Argument VALUES are
never written (they may hold secrets). Logging failure never breaks a call.
"""
from __future__ import annotations

import datetime
import functools
import os
from typing import Any, Optional

_port: "Optional[int]" = None
#: Test seam / override: directory to write into.
log_dir: "Optional[str]" = None

_REPO_ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(
    os.path.abspath(__file__)))))


def set_port(port: int) -> None:
    global _port
    _port = port


def _dir() -> str:
    return log_dir or os.path.join(_REPO_ROOT, "logs", "mcp_calls")


def log_call(tool: str, args: Any) -> None:
    try:
        now = datetime.datetime.now(datetime.timezone.utc)
        keys = ",".join(sorted(str(k) for k in args)) if isinstance(args, dict) else ""
        d = _dir()
        os.makedirs(d, exist_ok=True)
        name = f"{_port if _port is not None else 'noport'}_{now.strftime('%Y%m%d')}.log"
        with open(os.path.join(d, name), "a", encoding="utf-8") as f:
            f.write(f"{now.strftime('%Y-%m-%dT%H:%M:%S.%f')[:-3]}Z {tool} {keys}\n")
    except Exception:
        pass


def wrap_direct(name: str, fn: Any) -> Any:
    """Wrap a directly-published tool function so it logs too."""
    @functools.wraps(fn)
    def _logged(*a: Any, **kw: Any) -> Any:
        log_call(name, kw)
        return fn(*a, **kw)
    return _logged


def install(mcp: Any, registry: Any, kept: Any) -> None:
    """Hook every tool a ``collapse()``d server dispatches: each registry entry
    (reached via ``{prefix}call``/``{prefix}batch``) and each directly
    published tool in ``kept``. Called once after ``collapse()``; the
    registry module itself stays untouched (it is vendored byte-for-byte into
    mykicadMcp)."""
    for entry in registry.entries:
        entry.fn = wrap_direct(entry.name, entry.fn)
    tools = mcp._tool_manager._tools
    for name in kept:
        if name in tools:
            tools[name].fn = wrap_direct(name, tools[name].fn)
