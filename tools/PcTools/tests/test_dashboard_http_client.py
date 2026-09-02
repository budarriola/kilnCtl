#!/usr/bin/env python3
"""Unit tests for kilnctrl.dashboard_http_client and the get_heap_status MCP
tool it backs (DRAM_PSRAM_PLAN.md Phase 0, section 4.1). All against MOCKED
urllib responses -- no real socket, no live board.

The centerpiece is FacadeDiscoverabilityTest below: it exercises the REAL
production kilnctrl registry, not a synthetic one, so a missing
mcp_facade.py keyword row or a missing mcp_server_info.py import both fail
here -- the exact failure mode this plan's Phase 0 called out ("nothing
under tools/PcTools/src/kilnctrl parses those keys today, so the data is
reachable by hand but not from the tooling every other measurement in this
plan uses"): a tool that is registered but not findable through kiln_find is
barely better than one that was never written.

Run with: python -m pytest tools/PcTools/tests/test_dashboard_http_client.py -q
"""
from __future__ import annotations

import io
import json
import os
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import dashboard_http_client as dh  # noqa: E402


def _fake_response(body: bytes, status: int = 200):
    resp = io.BytesIO(body)
    resp.status = status

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


def _sample_status(**heap_overrides) -> dict:
    """A minimal but realistic /api/status body -- just enough for
    get_heap_status()'s three keys, plus a handful of unrelated fields (the
    real endpoint returns far more) to prove the parser reads through a
    normal-sized payload rather than assuming it is the only content."""
    status = {
        "fw_version": "1.2.3",
        "uptime_s": 4567,
        "heap_internal": {"free": 16847, "largest_free_block": 7680, "min_free": 8875, "total": 320000},
        "heap_spiram": {"free": 8073848, "largest_free_block": 8060000, "min_free": 8000000, "total": 8388608},
        "heap_dma": {"free": 12000, "largest_free_block": 6000, "min_free": 7500, "total": 200000},
    }
    status.update(heap_overrides)
    return status


class GetHeapStatusTest(unittest.TestCase):
    def test_parses_all_three_heap_objects(self):
        body = json.dumps(_sample_status()).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            heap = dh.get_heap_status("10.0.0.5")
        self.assertEqual(heap["heap_internal"]["min_free"], 8875)
        self.assertEqual(heap["heap_spiram"]["free"], 8073848)
        self.assertEqual(heap["heap_dma"]["largest_free_block"], 6000)

    def test_min_free_is_the_low_water_field_not_instantaneous_free(self):
        # The plan doc is explicit that min_free (not free) is the metric
        # every acceptance criterion is written against -- a regression that
        # swapped the two keys would still "parse" but read back the wrong
        # number silently. Use distinguishable values so a mix-up fails loud.
        body = json.dumps(_sample_status(heap_internal={
            "free": 99999, "largest_free_block": 88888, "min_free": 111, "total": 320000,
        })).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            heap = dh.get_heap_status("10.0.0.5")
        self.assertEqual(heap["heap_internal"]["min_free"], 111)
        self.assertEqual(heap["heap_internal"]["free"], 99999)

    def test_missing_heap_dma_key_raises_not_silently_zero(self):
        """NEGATIVE TEST: an older-firmware /api/status response (pre-4.1,
        before heap_dma existed) must be refused loudly, not read back as
        heap_dma == 0-everywhere -- which would look identical to the DRAM
        exhaustion this whole plan exists to measure. Proves this check can
        actually fail: drop heap_dma from the fixture and watch the
        assertion below go red before it is restored."""
        status = _sample_status()
        del status["heap_dma"]
        body = json.dumps(status).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            with self.assertRaises(dh.DashboardHttpError) as ctx:
                dh.get_heap_status("10.0.0.5")
        self.assertIn("heap_dma", str(ctx.exception))

    def test_http_error_surfaced_as_dashboard_http_error(self):
        with unittest.mock.patch(
            "urllib.request.urlopen", side_effect=OSError("connection refused")
        ):
            with self.assertRaises(dh.DashboardHttpError):
                dh.get_heap_status("10.0.0.5")

    def test_non_json_body_surfaced_as_dashboard_http_error(self):
        body = b"not json"
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            with self.assertRaises(dh.DashboardHttpError):
                dh.get_heap_status("10.0.0.5")


class McpToolTest(unittest.TestCase):
    """get_heap_status() as wired into mcp_server_info.py -- the string
    formatting a caller actually sees, and the host-resolution/error path."""

    def test_reports_all_three_heap_regions(self):
        from kilnctrl import mcp_server as m

        body = json.dumps(_sample_status()).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            result = m.get_heap_status(host="10.0.0.5")
        self.assertIn("heap_internal", result)
        self.assertIn("heap_spiram", result)
        self.assertIn("heap_dma", result)
        self.assertIn("min_free=8875", result)

    def test_unreachable_board_reported_not_raised(self):
        from kilnctrl import mcp_server as m

        with unittest.mock.patch(
            "urllib.request.urlopen", side_effect=OSError("connection refused")
        ):
            result = m.get_heap_status(host="10.0.0.5")
        self.assertTrue(result.startswith("error"))


class FacadeDiscoverabilityTest(unittest.TestCase):
    """Exercises the REAL production kilnctrl registry (kilnctrl.mcp_server),
    not a synthetic stand-in -- the failure mode this test guards against is
    exactly the one DRAM_PSRAM_PLAN.md section 4.1 called out: a tool that
    exists but that kiln_find can never surface."""

    def test_found_by_plain_language_dram_query(self):
        from kilnctrl import mcp_server as m

        hits = [h.name for h in m.registry.search("how much internal DRAM is free on the board")]
        self.assertIn("get_heap_status", hits[:3])

    def test_found_via_keyword_synonym_alone(self):
        # Deliberately avoids "heap"/"status"/"get" -- every word the tool's
        # own name would match on -- so this only passes if mcp_facade.py's
        # KEYWORDS row for get_heap_status is actually wired up, not because
        # the query happens to overlap the name.
        from kilnctrl import mcp_server as m

        hits = [h.name for h in m.registry.search("check for memory fragmentation and malloc failures")]
        self.assertIn("get_heap_status", hits[:3])

    def test_registered_in_the_registry(self):
        from kilnctrl import mcp_server as m

        self.assertIn("get_heap_status", m.registry.by_name)

    def test_not_directly_published_on_the_wire(self):
        # Only the six facade tools stay published -- get_heap_status, like
        # get_stack_margin next to it, is reachable only through
        # kiln_call/kiln_find, never as its own top-level MCP schema.
        from kilnctrl import mcp_server as m

        published = set(m.mcp._tool_manager._tools)
        self.assertNotIn("get_heap_status", published)


if __name__ == "__main__":
    unittest.main()
