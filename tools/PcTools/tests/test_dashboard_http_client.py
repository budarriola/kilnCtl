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


def _urlopen_router(responses: dict):
    """responses: {path_substring: body_bytes}. First match by substring
    wins; used to route GET /api/status vs GET /api/crash_report to
    different canned bodies within one test."""

    def _fake_urlopen(req, timeout=None):
        url = req.full_url if hasattr(req, "full_url") else req
        for key, value in responses.items():
            if key in url:
                return _fake_response(value)
        raise AssertionError(f"unexpected URL in test: {url}")

    return _fake_urlopen


_CRASH_UNACK_BODY = json.dumps({
    "present": True, "acknowledged": False, "exc_cause": 6, "exc_cause_str": "IllegalInstruction",
    "exc_task": "safety_poll", "found_on_boot_reset_reason": "panic/exception",
    "backtrace": [], "backtrace_corrupted": False,
    "dump_id": 3735928559, "fw_build": "Sep 22 2026 09:00:00",
    "crash_uptime_s": 42, "crash_uptime_known": True,
}).encode("utf-8")

_CRASH_NONE_BODY = json.dumps({"present": False}).encode("utf-8")


class UnacknowledgedCrashSurfacingTest(unittest.TestCase):
    """MANDATORY negative test: the 2026-08-31 incident happened because
    get_heap_status() (dashboard_http_client.py:89) discarded reset_reason
    and never looked at /api/crash_report at all -- a board that panicked at
    ~15:05 and ran 5 hours with an unacknowledged crash report looked
    identical, through this function, to a perfectly healthy one. These
    tests construct that exact synthetic state and prove it now surfaces,
    then prove a clean board still reads clean."""

    def test_unacknowledged_crash_surfaces_in_get_heap_status(self):
        responses = {
            "/api/status": json.dumps(_sample_status(reset_reason="panic/exception")).encode(),
            "/api/crash_report": _CRASH_UNACK_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            heap = dh.get_heap_status("10.0.0.5")
        self.assertEqual(heap["reset_reason"], "panic/exception")
        self.assertIsNotNone(heap["unacknowledged_crash"])
        self.assertEqual(heap["unacknowledged_crash"]["exc_task"], "safety_poll")
        self.assertTrue(heap["unacknowledged_crash"]["present"])
        self.assertFalse(heap["unacknowledged_crash"]["acknowledged"])

    def test_unacknowledged_crash_surfaces_in_mcp_tool_text(self):
        from kilnctrl import mcp_server as m

        responses = {
            "/api/status": json.dumps(_sample_status(reset_reason="panic/exception")).encode(),
            "/api/crash_report": _CRASH_UNACK_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            result = m.get_heap_status(host="10.0.0.5")
        self.assertIn("UNACKNOWLEDGED CRASH REPORT", result)
        self.assertIn("safety_poll", result)
        self.assertIn("panic/exception", result)
        self.assertIn("Sep 22 2026 09:00:00", result)
        self.assertIn("crash_uptime_s=42", result)
        self.assertIn("dump_id=3735928559", result)

    def test_clean_board_reports_no_crash(self):
        """PROOF a clean board passes: reset_reason is a normal boot reason
        and /api/crash_report says present:false -- neither the dict key nor
        the MCP text should claim a crash exists."""
        responses = {
            "/api/status": json.dumps(_sample_status(reset_reason="power-on")).encode(),
            "/api/crash_report": _CRASH_NONE_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            heap = dh.get_heap_status("10.0.0.5")
        self.assertIsNone(heap["unacknowledged_crash"])
        self.assertEqual(heap["reset_reason"], "power-on")

        from kilnctrl import mcp_server as m

        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            result = m.get_heap_status(host="10.0.0.5")
        self.assertNotIn("UNACKNOWLEDGED CRASH REPORT", result)
        self.assertIn("reset_reason='power-on'", result)


class LargestLowTest(unittest.TestCase):
    def test_largest_low_carried_and_printed(self):
        low = {"bytes": 8192, "at_uptime_s": 57000}
        responses = {
            "/api/status": json.dumps(_sample_status(heap_internal_largest_low=low)).encode(),
            "/api/crash_report": _CRASH_NONE_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            heap = dh.get_heap_status("10.0.0.5")
        self.assertEqual(heap["heap_internal_largest_low"], low)
        from kilnctrl import mcp_server as m
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            result = m.get_heap_status(host="10.0.0.5")
        self.assertIn("low-water: 8192 B first seen at uptime_s=57000", result)

    def test_largest_low_absent_is_reported(self):
        responses = {
            "/api/status": json.dumps(_sample_status()).encode(),
            "/api/crash_report": _CRASH_NONE_BODY,
        }
        from kilnctrl import mcp_server as m
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            result = m.get_heap_status(host="10.0.0.5")
        self.assertIn("low-water: not reported", result)


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


_TIMING_FULL_BODY = json.dumps({
    "display_flush_us": {"count": 42, "last": 1200, "min": 900, "max": 2100, "mean": 1300},
    "thermo_read_us": {"count": 10, "last": 500, "min": 400, "max": 700, "mean": 480},
    "link_reply_us": {"count": 5, "last": 40000, "min": 35000, "max": 50000, "mean": 41000, "timeouts": 2},
}).encode("utf-8")

_TIMING_ZERO_BODY = json.dumps({
    "display_flush_us": {"count": 0, "last": 0, "min": 0, "max": 0, "mean": 0},
    "thermo_read_us": {"count": 0, "last": 0, "min": 0, "max": 0, "mean": 0},
    "link_reply_us": {"count": 0, "last": 0, "min": 0, "max": 0, "mean": 0, "timeouts": 0},
}).encode("utf-8")


class DiagnosticsTimingTest(unittest.TestCase):
    """get_diagnostics_timing() / its wiring into get_heap_status() and the
    get_heap_status MCP tool text -- covers HW_ABSTRACTION.md's
    display_flush_us/thermo_read_us metrics and 2026-09-06's addition,
    link_reply_us (the on-board ESP<->Pico safety-link reply latency, added
    alongside safety_link_stats_t.link_reply_us_* in 9eecb0ff). None of this
    had test coverage before this file's change."""

    def test_get_diagnostics_timing_parses_all_three_blocks(self):
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(_TIMING_FULL_BODY)):
            timing = dh.get_diagnostics_timing("10.0.0.5")
        self.assertEqual(timing["link_reply_us"]["count"], 5)
        self.assertEqual(timing["link_reply_us"]["timeouts"], 2)
        self.assertEqual(timing["display_flush_us"]["mean"], 1300)
        self.assertEqual(timing["thermo_read_us"]["max"], 700)

    def test_get_diagnostics_timing_http_error_surfaced(self):
        with unittest.mock.patch("urllib.request.urlopen", side_effect=OSError("connection refused")):
            with self.assertRaises(dh.DashboardHttpError):
                dh.get_diagnostics_timing("10.0.0.5")

    def test_get_diagnostics_timing_non_json_surfaced(self):
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(b"not json")):
            with self.assertRaises(dh.DashboardHttpError):
                dh.get_diagnostics_timing("10.0.0.5")

    def test_get_heap_status_carries_all_three_timing_blocks(self):
        responses = {
            "/api/status": json.dumps(_sample_status()).encode(),
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/diagnostics/timing": _TIMING_FULL_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            heap = dh.get_heap_status("10.0.0.5")
        self.assertEqual(heap["link_reply_us"]["count"], 5)
        self.assertEqual(heap["link_reply_us"]["timeouts"], 2)
        self.assertEqual(heap["display_flush_us"]["last"], 1200)
        self.assertEqual(heap["thermo_read_us"]["min"], 400)
        self.assertNotIn("diagnostics_timing_check_error", heap)

    def test_get_heap_status_older_firmware_missing_endpoint_is_non_fatal(self):
        """NEGATIVE-shaped case: a board whose firmware predates
        /api/diagnostics/timing must not blow up get_heap_status() -- the
        heap figures still have to come back, with the timing gap reported
        under diagnostics_timing_check_error, not silently dropped."""

        def _fake_urlopen(req, timeout=None):
            url = req.full_url if hasattr(req, "full_url") else req
            if "/api/diagnostics/timing" in url:
                raise OSError("404 not found")
            if "/api/crash_report" in url:
                return _fake_response(_CRASH_NONE_BODY)
            return _fake_response(json.dumps(_sample_status()).encode())

        with unittest.mock.patch("urllib.request.urlopen", side_effect=_fake_urlopen):
            heap = dh.get_heap_status("10.0.0.5")
        self.assertIn("diagnostics_timing_check_error", heap)
        self.assertNotIn("link_reply_us", heap)
        self.assertEqual(heap["heap_internal"]["min_free"], 8875)

    def test_mcp_tool_text_reports_link_reply_us_with_timeouts(self):
        from kilnctrl import mcp_server as m

        responses = {
            "/api/status": json.dumps(_sample_status()).encode(),
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/diagnostics/timing": _TIMING_FULL_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            result = m.get_heap_status(host="10.0.0.5")
        self.assertIn("link_reply_us: count=5 last=40000 min=35000 max=50000 mean=41000 (us) timeouts=2", result)
        self.assertIn("display_flush_us: count=42", result)
        self.assertIn("thermo_read_us: count=10", result)

    def test_mcp_tool_text_reports_no_samples_yet_when_count_zero(self):
        """PROOF the count==0 branch (path never ran this boot) reads as 'no
        samples yet' rather than a misleading 'mean=0' that looks like a real
        zero-length measurement."""
        from kilnctrl import mcp_server as m

        responses = {
            "/api/status": json.dumps(_sample_status()).encode(),
            "/api/crash_report": _CRASH_NONE_BODY,
            "/api/diagnostics/timing": _TIMING_ZERO_BODY,
        }
        with unittest.mock.patch("urllib.request.urlopen", side_effect=_urlopen_router(responses)):
            result = m.get_heap_status(host="10.0.0.5")
        self.assertIn("link_reply_us: no samples yet", result)
        self.assertIn("display_flush_us: no samples yet", result)
        self.assertIn("thermo_read_us: no samples yet", result)
        self.assertNotIn("timeouts=0", result)


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


def _cfgfs_item(name, file_rev=5, nvs_rev=5, file_backed=True, diverged=False, nvs_stale=False,
                migration_deferred=False):
    """One dual_write.items[] row exactly as cfg_fs_status.c's
    cfg_fs_status_build_json_ex() emits it (keys and order: name, file_backed,
    file_rev, nvs_backed, nvs_rev, diverged, nvs_stale, migration_deferred)."""
    return {"name": name, "file_backed": file_backed, "file_rev": file_rev, "nvs_backed": True,
            "nvs_rev": nvs_rev, "diverged": diverged, "nvs_stale": nvs_stale,
            "migration_deferred": migration_deferred}


def _sample_cfgfs(**overrides) -> dict:
    """A realistic GET /api/cfgfs body (cfg_fs_status.c's shape), mounted
    with a couple of files and a healthy (non-diverged) dual-write."""
    body = {
        "mounted": True,
        "status": "mounted",
        "capacity": {"known": True, "total_bytes": 524288, "used_bytes": 4096, "free_bytes": 520192},
        "file_count": 2,
        "files": [
            {"name": "zones.json", "size_bytes": 640},
            {"name": "prefs.json", "size_bytes": 96},
        ],
        "tmp_entries_now": 0,
        "subdirs": [{"name": "profiles", "file_count": 3, "size_bytes": 1800, "unknown_size": 0}],
        "dual_write": {
            "write_mode": "cfg_only",
            "items": [
                _cfgfs_item("zones", file_rev=5, nvs_rev=5),
                _cfgfs_item("unit_pref", file_rev=2, nvs_rev=2),
            ],
            "nvs_only": [],
            "nvs_permanent": ["wifi_creds", "profiles_favorites", "aux_convert_journal"],
        },
    }
    body.update(overrides)
    return body


class GetCfgfsStatusClientTest(unittest.TestCase):
    """dashboard_http_client.get_cfgfs_status() -- GET /api/cfgfs, mocked."""

    def test_parses_mounted_body(self):
        body = json.dumps(_sample_cfgfs()).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            cfgfs = dh.get_cfgfs_status("10.0.0.5")
        self.assertTrue(cfgfs["mounted"])
        self.assertEqual(cfgfs["file_count"], 2)
        self.assertEqual(cfgfs["dual_write"]["items"][0]["file_rev"], 5)

    def test_unmounted_body_parses_too(self):
        body = json.dumps({
            "mounted": False, "status": "unmounted", "reason": "not mounted this boot",
            "capacity": {"known": False}, "file_count": 0, "files": [], "tmp_entries_now": 0,
            "dual_write": {"write_mode": "cfg_only", "items": [], "nvs_only": [], "nvs_permanent": []},
        }).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            cfgfs = dh.get_cfgfs_status("10.0.0.5")
        self.assertFalse(cfgfs["mounted"])
        self.assertFalse(cfgfs["capacity"]["known"])

    def test_http_error_surfaced_as_dashboard_http_error(self):
        with unittest.mock.patch("urllib.request.urlopen", side_effect=OSError("connection refused")):
            with self.assertRaises(dh.DashboardHttpError):
                dh.get_cfgfs_status("10.0.0.5")

    def test_non_json_body_surfaced_as_dashboard_http_error(self):
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(b"not json")):
            with self.assertRaises(dh.DashboardHttpError):
                dh.get_cfgfs_status("10.0.0.5")


class GetCfgfsStatusMcpToolTest(unittest.TestCase):
    """get_cfgfs_status() as wired into mcp_server_info.py -- the text a
    caller actually sees, including the diverged-dual-write warning."""

    def test_reports_mounted_capacity_and_files(self):
        from kilnctrl import mcp_server as m

        body = json.dumps(_sample_cfgfs()).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            result = m.get_cfgfs_status(host="10.0.0.5")
        self.assertIn("mounted=True", result)
        self.assertIn("free=520192", result)
        self.assertIn("zones.json", result)
        self.assertIn("640 B", result)
        self.assertIn("dual_write.zones: file_rev=5 nvs_rev=5", result)
        self.assertIn("dual_write.unit_pref: file_rev=2 nvs_rev=2", result)
        self.assertNotIn("not file-backed yet", result)
        self.assertNotIn("DIVERGED", result)
        self.assertIn("profiles/: 3 file(s), 1800 B", result)
        self.assertIn("NVS by design: wifi_creds, profiles_favorites, aux_convert_journal", result)

    def test_diverged_dual_write_is_flagged(self):
        from kilnctrl import mcp_server as m

        cfgfs = _sample_cfgfs()
        cfgfs["dual_write"]["items"][0] = _cfgfs_item("zones", file_rev=5, nvs_rev=6, diverged=True)
        body = json.dumps(cfgfs).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            result = m.get_cfgfs_status(host="10.0.0.5")
        self.assertIn("dual_write.zones: file_rev=5 nvs_rev=6 [!!! DIVERGED", result)
        self.assertNotIn("dual_write.unit_pref: file_rev=2 nvs_rev=2 [", result)

    def test_stale_deferred_and_unbacked_rows_are_flagged(self):
        from kilnctrl import mcp_server as m

        cfgfs = _sample_cfgfs()
        cfgfs["dual_write"]["items"] = [
            _cfgfs_item("firing_stats", file_rev=9, nvs_rev=4, nvs_stale=True),
            _cfgfs_item("relay_cycles", migration_deferred=True),
            _cfgfs_item("tz", file_backed=False),
        ]
        body = json.dumps(cfgfs).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            result = m.get_cfgfs_status(host="10.0.0.5")
        self.assertIn("dual_write.firing_stats: file_rev=9 nvs_rev=4 [nvs_stale", result)
        self.assertIn("dual_write.relay_cycles: file_rev=5 nvs_rev=5 [migration_deferred]", result)
        self.assertIn("dual_write.tz: file_rev=5 nvs_rev=5 [not file-backed yet]", result)

    def test_unmounted_reports_reason_and_unknown_capacity(self):
        from kilnctrl import mcp_server as m

        body = json.dumps({
            "mounted": False, "status": "unavailable", "reason": "mount was attempted and failed",
            "capacity": {"known": False}, "file_count": 0, "files": [], "tmp_entries_now": 0,
            "dual_write": {"write_mode": "cfg_only", "items": [
                _cfgfs_item("zones", file_backed=False)], "nvs_only": ["prefs"], "nvs_permanent": []},
        }).encode("utf-8")
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)):
            result = m.get_cfgfs_status(host="10.0.0.5")
        self.assertIn("mounted=False", result)
        self.assertIn("reason: mount was attempted and failed", result)
        self.assertIn("capacity: unknown", result)
        self.assertIn("not file-backed yet", result)

    def test_unreachable_board_reported_not_raised(self):
        from kilnctrl import mcp_server as m

        with unittest.mock.patch("urllib.request.urlopen", side_effect=OSError("connection refused")):
            result = m.get_cfgfs_status(host="10.0.0.5")
        self.assertTrue(result.startswith("error"))


class CfgfsStatusFacadeDiscoverabilityTest(unittest.TestCase):
    """Same real-registry discoverability guarantee as
    FacadeDiscoverabilityTest above, for get_cfgfs_status -- both the task's
    named example queries ("filesystem", "cfg partition") and a keyword-only
    query must land on it, and it must stay off the directly-published wire
    surface like every other facade-only tool."""

    def test_found_by_filesystem_query(self):
        from kilnctrl import mcp_server as m

        hits = [h.name for h in m.registry.search("filesystem")]
        self.assertIn("get_cfgfs_status", hits[:5])

    def test_found_by_cfg_partition_query(self):
        from kilnctrl import mcp_server as m

        hits = [h.name for h in m.registry.search("cfg partition")]
        self.assertIn("get_cfgfs_status", hits[:5])

    def test_found_via_keyword_synonym_alone(self):
        # Avoids "cfgfs"/"status"/"get" -- only passes if mcp_facade.py's
        # KEYWORDS row is actually wired up.
        from kilnctrl import mcp_server as m

        hits = [h.name for h in m.registry.search("is the dual-write diverged")]
        self.assertIn("get_cfgfs_status", hits[:5])

    def test_registered_in_the_registry(self):
        from kilnctrl import mcp_server as m

        self.assertIn("get_cfgfs_status", m.registry.by_name)

    def test_not_directly_published_on_the_wire(self):
        from kilnctrl import mcp_server as m

        published = set(m.mcp._tool_manager._tools)
        self.assertNotIn("get_cfgfs_status", published)


if __name__ == "__main__":
    unittest.main()
