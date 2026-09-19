#!/usr/bin/env python3
"""Thin MCP-over-HTTP client for tools/bench_test.ps1.

`tools/bench_test.ps1` is meant to stay a thin caller of the `kilnctrl` MCP
server (port 8767) per docs/BENCH_TEST_SYSTEM_PLAN.md §2.1 -- the actual
MCP wire protocol (streamable-http, JSON-RPC, a session handshake) is not
something PowerShell should reimplement by hand, so this script does the
one `tools/call` round trip using the `mcp` package already vendored into
`tools/PcTools/.venv` (the same dependency kilnctrl_mcp_server.py itself
uses), and prints the tool's plain-text result to stdout.

Usage: python bench_test_client.py <tool_name> <json_args> [--port N]
Exits 0 on a successful call (regardless of the tool's own PASS/FAIL
content -- that is bench_test.ps1's job to parse), non-zero only if the
call itself could not be made (server unreachable, tool not found, bad
arguments).
"""
from __future__ import annotations

import argparse
import asyncio
import json
import sys


async def _call(tool_name: str, arguments: dict, port: int) -> str:
    from mcp import ClientSession
    from mcp.client.streamable_http import streamable_http_client

    url = f"http://127.0.0.1:{port}/mcp"
    async with streamable_http_client(url) as (read, write, _get_session_id):
        async with ClientSession(read, write) as session:
            await session.initialize()
            result = await session.call_tool(tool_name, arguments=arguments)
            if result.isError:
                text = "\n".join(getattr(c, "text", str(c)) for c in result.content)
                raise RuntimeError(text or "tool call reported an error")
            return "\n".join(getattr(c, "text", str(c)) for c in result.content)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("tool_name")
    parser.add_argument("json_args")
    parser.add_argument("--port", type=int, default=8767)
    args = parser.parse_args()

    try:
        arguments = json.loads(args.json_args) if args.json_args else {}
    except json.JSONDecodeError as exc:
        print(f"error: bad --json_args: {exc}", file=sys.stderr)
        return 2

    try:
        text = asyncio.run(_call(args.tool_name, arguments, args.port))
    except Exception as exc:  # noqa: BLE001 - report cleanly, this is a CLI entry point
        print(f"error: {exc}", file=sys.stderr)
        return 1

    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
