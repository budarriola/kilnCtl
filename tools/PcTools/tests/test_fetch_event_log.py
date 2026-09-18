#!/usr/bin/env python3
"""Unit tests for kilnctrl.dashboard_http_client.get_event_log_bytes() and
the fetch_event_log MCP tool built on it (mcp_server_info.py).

2026-09-17 vacuity sweep finding: kilnctrl.event_log_decoder.py was fully
implemented and had its own test suite (test_event_log_decoder.py), but
nothing in production ever called it -- no MCP tool, no script, fetched a
real board's /api/logs/{firing,autotune} and ran the bytes through
decode_stream(). fetch_event_log() is that caller; these tests exercise the
whole path (mocked HTTP -> real decode_stream() -> real format_record()),
not just the HTTP layer, since a decoder call that silently never fires
(e.g. a caller that returns the raw bytes without decoding them) would
still pass an HTTP-only test.

All against MOCKED urllib responses -- no real socket, no live board.

Run with: python -m pytest tools/PcTools/tests/test_fetch_event_log.py -q
"""
from __future__ import annotations

import io
import os
import struct
import sys
import unittest
import unittest.mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import dashboard_http_client as dh  # noqa: E402
from kilnctrl import event_log_decoder as eld  # noqa: E402


def _fake_response(body: bytes):
    resp = io.BytesIO(body)
    resp.status = 200

    class _Ctx:
        def __enter__(self_inner):
            return resp

        def __exit__(self_inner, *exc):
            return False

    return _Ctx()


class GetEventLogBytesTest(unittest.TestCase):
    def test_rejects_unknown_kind_before_any_request(self):
        with unittest.mock.patch("urllib.request.urlopen") as mock_open:
            with self.assertRaises(ValueError):
                dh.get_event_log_bytes("10.0.0.5", "bogus")
        mock_open.assert_not_called()

    def test_fetches_raw_bytes_for_firing_and_autotune(self):
        body = b"\x00" * eld.RECORD_SIZE
        with unittest.mock.patch("urllib.request.urlopen", return_value=_fake_response(body)) as mock_open:
            result = dh.get_event_log_bytes("10.0.0.5", "firing")
        self.assertEqual(result, body)
        (req,), _ = mock_open.call_args
        self.assertIn("/api/logs/firing", req.full_url)

    def test_http_error_surfaced_as_dashboard_http_error(self):
        with unittest.mock.patch("urllib.request.urlopen", side_effect=OSError("connection refused")):
            with self.assertRaises(dh.DashboardHttpError):
                dh.get_event_log_bytes("10.0.0.5", "autotune")


def _make_record_bytes(severity, source, code, zone, uptime_s, arg, note=""):
    """Builds one raw RECORD_SIZE-byte record using the module's own private
    struct so this test stays in lockstep with the real on-wire layout
    rather than re-deriving it by hand."""
    note_bytes = note.encode("utf-8")[: eld.NOTE_LEN - 1]
    return eld._STRUCT.pack(
        eld.EVENT_MAGIC, eld.EVENT_VERSION, severity, source, uptime_s, code, zone, arg,
        note_bytes.ljust(eld.NOTE_LEN, b"\x00"),
    )


class FetchEventLogToolTest(unittest.TestCase):
    """Exercises the real mcp_server_info.fetch_event_log() end to end:
    mocked HTTP in, real event_log_decoder.decode_stream()/format_record()
    in the middle, formatted text out."""

    def test_decodes_a_real_record_and_reports_its_fields(self):
        from kilnctrl import mcp_server_info as info

        record = _make_record_bytes(
            severity=eld.Severity.WARN, source=eld.Source.FIRING,
            code=eld.FiringCode.FAULTED, zone=1, uptime_s=12345, arg=7, note="hi",
        )
        with unittest.mock.patch.object(dh, "get_event_log_bytes", return_value=record):
            result = info.fetch_event_log("firing", host="10.0.0.5")
        self.assertIn("WARN", result)
        self.assertIn("FIRING", result)
        self.assertIn("FAULTED", result)
        self.assertIn("zone=1", result)
        self.assertIn("12345", result)
        self.assertIn("hi", result)

    def test_empty_log_reports_explicitly_not_as_blank_output(self):
        from kilnctrl import mcp_server_info as info

        with unittest.mock.patch.object(dh, "get_event_log_bytes", return_value=b""):
            result = info.fetch_event_log("autotune", host="10.0.0.5")
        self.assertIn("empty", result.lower())
        self.assertIn("0 records", result)

    def test_old_format_log_is_refused_not_misread(self):
        """NEGATIVE-SHAPED CASE: bytes that don't carry the current
        magic/version (e.g. a pre-2026-09-02 text-format log) must surface
        as an error string, never as a garbage-but-plausible-looking
        decoded record."""
        from kilnctrl import mcp_server_info as info

        with unittest.mock.patch.object(dh, "get_event_log_bytes", return_value=b"KTEL1 FIRE not a binary record"):
            result = info.fetch_event_log("firing", host="10.0.0.5")
        self.assertTrue(result.startswith("error:"))

    def test_transport_failure_reported_with_host(self):
        from kilnctrl import mcp_server_info as info

        with unittest.mock.patch.object(
            dh, "get_event_log_bytes", side_effect=dh.DashboardHttpError("boom")
        ):
            result = info.fetch_event_log("firing", host="10.0.0.5")
        self.assertTrue(result.startswith("error:"))
        self.assertIn("10.0.0.5", result)

    def test_negative_undecoded_passthrough_would_fail_this_suite(self):
        """Proves the tests above actually exercise decode_stream(), not
        just the HTTP call: a caller that returned raw bytes verbatim
        instead of decoding them would not contain 'FAULTED' or 'zone=1' as
        readable text, so test_decodes_a_real_record_and_reports_its_fields
        would go red. Checked here directly against the raw bytes, which
        must NOT already look like the formatted report."""
        record = _make_record_bytes(
            severity=eld.Severity.WARN, source=eld.Source.FIRING,
            code=eld.FiringCode.FAULTED, zone=1, uptime_s=12345, arg=7, note="hi",
        )
        self.assertNotIn(b"FAULTED", record)
        self.assertNotIn(b"zone=1", record)


class FacadeDiscoverabilityTest(unittest.TestCase):
    """Same convention as test_dashboard_http_client.py's
    FacadeDiscoverabilityTest: a tool that exists but that kiln_find can
    never surface is barely better than one that was never written."""

    def test_registered_in_the_registry(self):
        from kilnctrl import mcp_server as m

        self.assertIn("fetch_event_log", m.registry.by_name)

    def test_found_via_keyword_synonym_alone(self):
        from kilnctrl import mcp_server as m

        hits = [h.name for h in m.registry.search("read the flash log_store history of firings")]
        self.assertIn("fetch_event_log", hits[:3])

    def test_not_directly_published_on_the_wire(self):
        from kilnctrl import mcp_server as m

        published = set(m.mcp._tool_manager._tools)
        self.assertNotIn("fetch_event_log", published)


if __name__ == "__main__":
    unittest.main()
