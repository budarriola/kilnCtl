#!/usr/bin/env python3
"""Tests for mcpkit.registry -- the five-tool search/batch facade that the
kilnctrl MCP server collapses its real tool surface into.

Why this test exists: ``collapse()`` is a single choke point that both
production servers run through at import time, so a defect here is silent
in a way that is easy to miss -- it does not throw, it just publishes the
wrong tools, mis-scores a search, or lets a bad argument through to real
hardware. In particular the relevance floor in ``ToolRegistry.search`` is
the one thing standing between "search returns what you meant" and "search
returns every tool that shares one weak token with the query" -- that
distinction is asserted directly, in a way that would fail if the floor
were ever set to zero, rather than merely exercised in passing by some
other test.

Run with: python -m unittest discover -s tools/PcTools/tests
(pytest also collects this file directly, which is how it's normally run.)
"""
from __future__ import annotations

import json
import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from mcp.server.mcpserver import MCPServer  # noqa: E402

from mcpkit.registry import collapse  # noqa: E402

#: The five facade tool names published by every collapse() call, given a
#: prefix -- reused by several tests below.
_FACADE_SUFFIXES = ("help", "find", "describe", "call", "batch")


def _facade_names(prefix: str) -> "list[str]":
    return [f"{prefix}{suffix}" for suffix in _FACADE_SUFFIXES]


def _build_server(*, prefix: str = "x_", keep=(), synonyms=None):
    """A small synthetic server standing in for kilnctrl's real,
    much larger tool set. Three tools are enough to exercise grouping,
    search relevance, and argument coercion without depending on the
    production server's actual surface (which would make this test brittle
    against their unrelated changes)."""
    mcp = MCPServer("probe")

    @mcp.tool()
    def thermo_read(channel: int, cj_offset_c: float = 0.0,
                     active: bool = False, meta: dict = None) -> str:
        """Read a thermocouple channel's temperature in degrees C."""
        return json.dumps({
            "channel": channel, "cj_offset_c": cj_offset_c,
            "active": active, "meta": meta,
        })

    @mcp.tool()
    def wifi_scan() -> str:
        """Scan for wifi networks and read the results list."""
        return "networks"

    @mcp.tool()
    def relay_set(index: int, state: bool) -> str:
        """Set a relay output on or off."""
        return "ok"

    registry = collapse(
        mcp, prefix=prefix, label="probe", title="Probe server",
        keep=keep, synonyms=synonyms or {},
    )
    return mcp, registry


class CollapseTests(unittest.TestCase):
    """collapse() must withdraw the real tools from the wire, publish
    exactly the five facade tools (plus anything in `keep`), and keep every
    withdrawn tool reachable through the registry it returns."""

    def test_withdraws_every_tool_except_keep(self):
        mcp, registry = _build_server(keep=())
        published = set(mcp._tool_manager._tools)
        self.assertEqual(published, set(_facade_names("x_")))
        # The withdrawn tools are gone from the wire but not gone -- the
        # registry that collapse() returns is exactly where they live now.
        self.assertEqual(
            set(registry.by_name),
            {"thermo_read", "wifi_scan", "relay_set"},
        )

    def test_kept_tool_stays_published_alongside_facade(self):
        mcp, registry = _build_server(keep=("wifi_scan",))
        published = set(mcp._tool_manager._tools)
        self.assertEqual(published, set(_facade_names("x_")) | {"wifi_scan"})
        # Kept tools are still indexed in the registry too -- withdrawal from
        # the wire and registration in the registry are independent.
        self.assertIn("wifi_scan", registry.by_name)

    def test_publishes_exactly_five_facade_tools_with_prefix(self):
        mcp, _registry = _build_server(prefix="probe_")
        published = set(mcp._tool_manager._tools)
        self.assertEqual(published, set(_facade_names("probe_")))
        self.assertEqual(len(published), 5)

    def test_instructions_are_set_and_name_the_facade_tools(self):
        mcp, _registry = _build_server(prefix="x_")
        instructions = mcp._lowlevel_server.instructions
        self.assertTrue(instructions and instructions.strip())
        for name in _facade_names("x_"):
            self.assertIn(name, instructions)


class SearchRelevanceTests(unittest.TestCase):
    """The search index and the relevance floor are the load-bearing part
    of the facade: they are what makes `find` return the right tool instead
    of a name the model has to guess at."""

    def test_synonym_query_lands_on_the_right_tool(self):
        _mcp, registry = _build_server(
            synonyms={"temperature": ("thermo", "thermocouple")},
        )
        hits = registry.search("temperature")
        self.assertTrue(hits)
        self.assertEqual(hits[0].name, "thermo_read")

    def test_relevance_floor_drops_a_weak_match(self):
        """`thermo_read` and `wifi_scan` both contain the token "read" (one
        in its name, the other only in its docstring's "read the results
        list"), so a query of "thermocouple read" scores both tools above
        zero. The floor exists specifically to keep the second one, which
        shares only that one weak token, out of the result -- prove it does.

        This assertion is written so that it fails if RELEVANCE_FLOOR were
        ever set to 0: with the floor disabled below, the same query does
        return both tools, confirming the floor -- not some other filter --
        is what is doing the work in the assertion above it.
        """
        _mcp, registry = _build_server(
            synonyms={"thermocouple": ("thermo",)},
        )
        query = "thermocouple read"

        with_floor = [e.name for e in registry.search(query)]
        self.assertEqual(with_floor, ["thermo_read"])
        self.assertNotIn("wifi_scan", with_floor)

        # Same registry, same query, floor disabled: the weak match comes
        # back too. This is the proof that the floor above was load-bearing,
        # not that "wifi_scan" simply never matches this query at all.
        registry.RELEVANCE_FLOOR = 0.0
        without_floor = [e.name for e in registry.search(query)]
        self.assertIn("wifi_scan", without_floor)
        self.assertIn("thermo_read", without_floor)
        self.assertGreater(len(without_floor), len(with_floor))


class ArgumentCoercionTests(unittest.TestCase):
    """ToolRegistry.invoke's coercion is what lets a stringly-typed call
    from a model still land on a real Python function signature."""

    def test_string_digits_coerce_to_int(self):
        _mcp, registry = _build_server()
        result = json.loads(registry.invoke("thermo_read", {"channel": "3"}))
        self.assertEqual(result["channel"], 3)
        self.assertIsInstance(result["channel"], int)

    def test_hex_string_coerces_to_int_base0(self):
        _mcp, registry = _build_server()
        result = json.loads(registry.invoke("thermo_read", {"channel": "0x1f"}))
        self.assertEqual(result["channel"], 31)

    def test_yes_and_off_coerce_to_bool(self):
        _mcp, registry = _build_server()
        result = json.loads(registry.invoke(
            "thermo_read", {"channel": 1, "active": "yes"}))
        self.assertIs(result["active"], True)

        result = json.loads(registry.invoke(
            "thermo_read", {"channel": 1, "active": "off"}))
        self.assertIs(result["active"], False)

    def test_json_string_coerces_to_dict(self):
        _mcp, registry = _build_server()
        result = json.loads(registry.invoke(
            "thermo_read", {"channel": 1, "meta": '{"a":1}'}))
        self.assertEqual(result["meta"], {"a": 1})


class ErrorPathTests(unittest.TestCase):
    """invoke() must never raise -- every failure comes back as a string
    starting with "error:" that carries what the caller needs to retry."""

    def test_unknown_tool_name_returns_error_string_with_suggestions(self):
        _mcp, registry = _build_server()
        result = registry.invoke("thermo_reed", {})
        self.assertTrue(result.startswith("error:"))
        self.assertIn("Closest:", result)
        # The one real tool that plausibly matches the typo should show up
        # as a ranked suggestion.
        self.assertIn("thermo_read", result)

    def test_unknown_tool_name_with_no_close_matches(self):
        _mcp, registry = _build_server()
        result = registry.invoke("completely_unrelated_xyz", {})
        self.assertTrue(result.startswith("error:"))
        self.assertIn("no close matches", result)

    def test_missing_required_argument_returns_signature(self):
        _mcp, registry = _build_server()
        result = registry.invoke("thermo_read", {})
        self.assertTrue(result.startswith("error:"))
        self.assertIn("missing required: channel", result)
        self.assertIn("signature:", result)
        self.assertIn("thermo_read(", result)

    def test_unknown_argument_name_returns_error_string(self):
        _mcp, registry = _build_server()
        result = registry.invoke("thermo_read", {"channel": 1, "bogus": 2})
        self.assertTrue(result.startswith("error:"))
        self.assertIn("unknown: bogus", result)
        self.assertIn("signature:", result)


class BatchTests(unittest.TestCase):
    """The batch tool's control flow (stop-on-error, skip accounting,
    flexible `calls` shapes) is exercised through the actual registered
    facade function, not by calling registry.invoke in a loop by hand."""

    def _batch_fn(self, mcp):
        return mcp._tool_manager._tools["x_batch"].fn

    def test_mixed_ok_and_error_steps_stop_on_error_true(self):
        mcp, _registry = _build_server()
        batch = self._batch_fn(mcp)
        result = batch(calls=[
            {"name": "wifi_scan"},
            {"name": "unknown_tool_xyz"},
            {"name": "relay_set", "args": {"index": 1, "state": True}},
        ], stop_on_error=True)
        self.assertIn("[1] ok wifi_scan", result)
        self.assertIn("[2] ERR unknown_tool_xyz", result)
        # The third step must not have run: it is reported as skipped, and
        # its own success marker never appears.
        self.assertIn("1 not run", result)
        self.assertNotIn("[3]", result)

    def test_mixed_ok_and_error_steps_stop_on_error_false(self):
        mcp, _registry = _build_server()
        batch = self._batch_fn(mcp)
        result = batch(calls=[
            {"name": "wifi_scan"},
            {"name": "unknown_tool_xyz"},
            {"name": "relay_set", "args": {"index": 1, "state": True}},
        ], stop_on_error=False)
        self.assertIn("[1] ok wifi_scan", result)
        self.assertIn("[2] ERR unknown_tool_xyz", result)
        # With stop_on_error disabled, the third step must actually run.
        self.assertIn("[3] ok relay_set", result)
        self.assertIn("2 ok, 1 failed", result)

    def test_calls_accepts_json_string(self):
        mcp, _registry = _build_server()
        batch = self._batch_fn(mcp)
        calls_json = json.dumps([{"name": "wifi_scan"}])
        result = batch(calls=calls_json)
        self.assertIn("[1] ok wifi_scan", result)

    def test_calls_accepts_single_unwrapped_dict(self):
        mcp, _registry = _build_server()
        batch = self._batch_fn(mcp)
        result = batch(calls={"name": "wifi_scan"})
        self.assertIn("[1] ok wifi_scan", result)


class DescribeTests(unittest.TestCase):
    """describe() is the "give me the full schema" escape hatch that a
    truncated `find` result points a caller back to."""

    def _describe_fn(self, mcp):
        return mcp._tool_manager._tools["x_describe"].fn

    def test_known_tool_returns_full_schema_lines(self):
        mcp, _registry = _build_server()
        describe = self._describe_fn(mcp)
        result = describe(names="thermo_read")
        self.assertIn("thermo_read(", result)
        self.assertIn("channel: int", result)
        self.assertIn("required", result)
        self.assertIn("cj_offset_c: num", result)
        self.assertIn("optional", result)
        self.assertIn("default=0.0", result)

    def test_unknown_tool_returns_closest_hint(self):
        mcp, _registry = _build_server()
        describe = self._describe_fn(mcp)
        result = describe(names="thermo_reed")
        self.assertIn("error: unknown", result)
        self.assertIn("Closest:", result)


if __name__ == "__main__":
    unittest.main()
