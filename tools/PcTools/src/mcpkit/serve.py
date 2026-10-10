"""Transport and lifecycle for the PcTools MCP servers.

Why HTTP rather than stdio
--------------------------
A stdio server is spawned by, and dies with, whichever client launched it. For
a server that owns a *physical port* that is the wrong lifetime: reconnecting a
client power-cycles the link, two clients cannot share one board, and there is
no way to ask a running server how it is doing without going through the
protocol it is currently blocked on.

Over HTTP the server is a long-lived process the operator starts and stops, and
every client -- editor, GUI, a curl from a terminal -- attaches to the same one.
That also gives us somewhere to hang two plain routes that are not MCP at all:

* ``GET /health``   -- liveness plus the tool counts, for the status button
* ``POST /shutdown`` -- graceful stop, for the stop button

Both are deliberately unauthenticated and bound to loopback only. Anything that
can reach them can already reach the MCP endpoint itself, which can flash
firmware; a token on the shutdown route would be security theatre.

``--transport stdio`` is still here. Headless and CI runs (and anything that
wants a private, disposable server) need a transport with no port to collide
on -- see ``docs/MCP_SERVERS.md``.
"""

from __future__ import annotations

import argparse
import asyncio
import logging
import os
import sys
from typing import Any, Callable, Optional

log = logging.getLogger(__name__)

#: Loopback only. These servers drive hardware -- flashing firmware, switching
#: relays, opening the E-stop loop -- so they must not be reachable off-box.
DEFAULT_HOST = "127.0.0.1"


def build_parser(name: str, default_port: int) -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        prog=f"{name}-mcp-server",
        description=f"MCP server for {name}. Defaults to HTTP on {DEFAULT_HOST}:{default_port}.",
    )
    parser.add_argument(
        "--transport", choices=("http", "stdio"), default="http",
        help="http (default): long-lived, shared, start/stop from the editor buttons. "
             "stdio: private one-client server, for headless or CI use.",
    )
    parser.add_argument("--host", default=DEFAULT_HOST,
                        help=f"HTTP bind address (default {DEFAULT_HOST}; loopback only)")
    parser.add_argument("--port", type=int, default=default_port,
                        help=f"HTTP port (default {default_port})")
    parser.add_argument("--path", default="/mcp", help="HTTP endpoint path (default /mcp)")
    parser.add_argument("--log-level", default="INFO",
                        choices=("DEBUG", "INFO", "WARNING", "ERROR"))
    return parser


def _add_control_routes(mcp: Any, name: str, port: int, path: str,
                        stopper: "Callable[[], None]",
                        freshness: "Optional[Any]" = None) -> None:
    """``/health`` and ``/shutdown``, registered before the ASGI app is built.

    ``freshness`` -- a ``mcpkit.registry.SourceSnapshot`` taken at startup, if
    the caller's ``collapse()``/``collapse_table()`` call was given a
    ``source_root``. When present, ``/health`` is re-checked against it on
    every call so ``mcp_servers.ps1 status`` can report stale/fresh without a
    second HTTP round trip or its own copy of the mtime-scanning logic.
    """
    from starlette.requests import Request
    from starlette.responses import JSONResponse

    published = sorted(mcp._tool_manager._tools)

    @mcp.custom_route("/health", methods=["GET"])
    async def health(_request: Request) -> JSONResponse:  # noqa: ARG001 - starlette signature
        payload = {
            "ok": True,
            "server": name,
            "pid": os.getpid(),
            "port": port,
            "endpoint": path,
            "published_tools": published,
        }
        if freshness is not None:
            from mcpkit.registry import check_staleness
            stale, changed = check_staleness(freshness)
            payload["fresh"] = not stale
            payload["changed_files"] = changed
            payload["started_at"] = freshness.started_at_human()
            payload["commit"] = freshness.commit
        return JSONResponse(payload)

    @mcp.custom_route("/shutdown", methods=["POST"])
    async def shutdown(_request: Request) -> JSONResponse:  # noqa: ARG001
        # Ask uvicorn to drain and exit *after* this response is written --
        # a stop button that reports "connection reset" is indistinguishable
        # from one that failed.
        stopper()
        return JSONResponse({"ok": True, "server": name, "pid": os.getpid(), "stopping": True})


async def _run_http(mcp: Any, name: str, host: str, port: int, path: str, log_level: str,
                    freshness: "Optional[Any]" = None) -> None:
    """Serve over streamable HTTP with our own uvicorn.Server.

    ``MCPServer.run_streamable_http_async`` builds its uvicorn server inline and
    keeps no handle on it, so ``/shutdown`` would have nothing to signal. Doing
    the two steps ourselves is the whole reason this function exists.
    """
    import uvicorn

    server: "Optional[uvicorn.Server]" = None

    def stop() -> None:
        if server is not None:
            server.should_exit = True

    _add_control_routes(mcp, name, port, path, stop, freshness=freshness)
    app = mcp.streamable_http_app(streamable_http_path=path, host=host)
    server = uvicorn.Server(uvicorn.Config(app, host=host, port=port, log_level=log_level.lower()))
    log.info("%s MCP server on http://%s:%d%s (health: /health, stop: POST /shutdown)",
             name, host, port, path)
    await server.serve()


def serve(mcp: Any, *, name: str, default_port: int, argv: "Optional[list[str]]" = None,
          on_close: "Optional[Callable[[], None]]" = None,
          freshness: "Optional[Any]" = None) -> int:
    """Parse argv, run the chosen transport, and always run ``on_close``.

    ``on_close`` is where a server releases its hardware clients. It runs on a
    clean exit, on Ctrl-C, and after ``POST /shutdown`` -- never skipped, since
    a serial port left open outlives the process that opened it on Windows.

    ``freshness`` -- forwarded to ``/health`` (HTTP transport only); see
    ``_add_control_routes``.
    """
    args = build_parser(name, default_port).parse_args(argv)
    from . import call_log
    call_log.set_port(args.port)
    logging.basicConfig(
        level=getattr(logging, args.log_level),
        stream=sys.stderr,  # stdout is the stdio transport; never log there
        format="%(asctime)s %(levelname)s %(name)s: %(message)s",
    )
    try:
        if args.transport == "stdio":
            asyncio.run(mcp.run_stdio_async())
        else:
            asyncio.run(_run_http(mcp, name, args.host, args.port, args.path, args.log_level,
                                  freshness=freshness))
    except KeyboardInterrupt:
        pass
    except OSError as exc:
        # Almost always "address already in use": another copy is running, and
        # that is worth saying plainly rather than as a traceback.
        log.error("%s MCP server could not start: %s", name, exc)
        return 1
    finally:
        if on_close is not None:
            on_close()
    return 0
