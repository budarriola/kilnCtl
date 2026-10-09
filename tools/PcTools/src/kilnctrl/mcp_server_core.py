"""Import-cycle-free home of the MCP server object and the ``_tool`` decorator.

Every ``mcp_server_*`` submodule registers its tools with ``@_core._tool()`` at
import time, while ``mcp_server`` star-re-exports each submodule. If the
decorator lived in ``mcp_server`` itself, importing a submodule first would
re-enter ``mcp_server`` while that submodule was only partly initialised; its
``import *`` would copy nothing and ``kilnctrl.mcp_server`` would permanently
lack the submodule's tools. This module imports nothing from ``mcp_server`` or
any ``mcp_server_*`` submodule at load time; the few pieces of server state the
guard needs (session log, stale banner) are looked up lazily at call time.
``mcp_server`` re-exports ``mcp`` and ``_tool`` so both names stay one object.
"""

from __future__ import annotations

import functools
import logging

try:
    # mcp >= 2.0 renamed FastMCP to MCPServer (same decorator/run API).
    from mcp.server.mcpserver import MCPServer as _McpServer
except ImportError:  # pragma: no cover - mcp 1.x
    from mcp.server.fastmcp import FastMCP as _McpServer

from .autotune import AutotuneQueryError  # noqa: F401
from .display import BlitError, DisplayQueryError
from .info import InfoQueryError
from .io_expander import IoQueryError
from .safety import SafetyQueryError
from .system import SystemQueryError
from .thermo import ThermoQueryError
from .touch import TouchQueryError

# Same logger name the guard used when it lived in mcp_server.
log = logging.getLogger("kilnctrl.mcp_server")

mcp = _McpServer("kilnctrl")


def _srv():
    """The aggregate module, resolved at call time (never at import time)."""
    import importlib
    return importlib.import_module("kilnctrl.mcp_server")


def _tool():
    """The ``mcp.tool()`` registration plus a blanket "never raise" guard.

    Every tool below builds its payload by calling a ``devices.py`` builder in
    its own argument list -- ``_send(TASK, devices.display_fill_rect(...))`` --
    so the builder's validation runs *before* ``_send`` is entered and its
    ValueError propagates straight out of the tool function. The tool contract
    here is that a bad argument comes back as an ``error: ...`` string the
    caller can read and correct, not as an exception the transport has to
    render; a model passing a negative width should be told what was wrong
    with it, not handed a traceback.

    The guard also catches the four query clients' errors and anything else
    unexpected, so a malformed reply or a link fault can never surface as a
    raised exception either.
    """
    register = mcp.tool()

    def decorate(fn):
        @functools.wraps(fn)  # keeps the signature/docstring FastMCP builds the schema from
        def wrapper(*args, **kwargs):
            try:
                result = fn(*args, **kwargs)
            except (
                ThermoQueryError,
                IoQueryError,
                DisplayQueryError,
                TouchQueryError,
                SafetyQueryError,
                InfoQueryError,
                SystemQueryError,
                BlitError,
            ) as exc:
                result = f"error: {exc}"
            except (ValueError, TypeError, OSError) as exc:
                _srv()._session_log.error("%s: rejected: %s", fn.__name__, exc)
                result = f"error: {exc}"
            except Exception as exc:  # noqa: BLE001 - a tool must always answer
                log.exception("unexpected error in tool %s", fn.__name__)
                result = f"error: unexpected {type(exc).__name__}: {exc}"
            # Applies to every tool through this one shared wrapper -- see
            # _srv()._stale_banner()'s docstring for why that is deliberate rather
            # than picked per-tool: every tool's result is produced by this
            # server's own Python (formatting/validation/error-handling
            # alone, even for tools that are mostly a passthrough), so any
            # of them can be affected by code that changed since this
            # process started. A cheap, cached, silent-when-fresh check at
            # the one choke point every tool already passes through beats
            # auditing which specific tools "depend on server code" (most
            # of them do, and that classification would itself go stale).
            if isinstance(result, str):
                banner = _srv()._stale_banner()
                if banner:
                    result = result + banner
            return result

        return register(wrapper)

    return decorate
