#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_web -- Wave 1a's WEB render
cases and the generated route-tier sweep. Every board/HTTP call is mocked
except the two tests that parse the real
firmware/KilnFW/App/drivers/http/route_tier_table.h off disk (the whole
point of generating WEB-X-03 from that file rather than hardcoding it).

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_web.py -q
"""
from __future__ import annotations

import gzip
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_web as C  # noqa: E402
from kilnctrl.bench_test.registry import REGISTRY, Verdict  # noqa: E402

REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), "..", "..", ".."))
ROUTE_TABLE_PATH = os.path.join(
    REPO_ROOT, "firmware", "KilnFW", "App", "drivers", "http", "route_tier_table.h"
)


class FakeWebClient:
    def __init__(self, pages: dict, raise_for: "set[str]" = frozenset()):
        self._pages = pages
        self._raise_for = raise_for

    def goto(self, path: str) -> str:
        if path in self._raise_for:
            from kilnctrl.web_ui_client import WebUiError

            raise WebUiError(f"GET {path} failed: connection refused")
        return self._pages.get(path, "")


class RenderCaseTest(unittest.TestCase):
    def test_dashboard_render_passes(self):
        html = '<html><body><button id="runBtn"></button><script defer src="/nav.js"></script></body></html>'
        ctx = {"web_client": FakeWebClient({"/": html})}
        result = REGISTRY["WEB-DASH-01"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_login_render_does_not_require_nav(self):
        html = '<html><body><input id="username"></body></html>'
        ctx = {"web_client": FakeWebClient({"/login": html})}
        result = REGISTRY["WEB-LOG-01"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_missing_landmark_fails(self):
        html = '<html><body><script defer src="/nav.js"></script></body></html>'
        ctx = {"web_client": FakeWebClient({"/": html})}
        result = REGISTRY["WEB-DASH-01"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_connection_failure_fails_not_crashes(self):
        """Negative test: a raised WebUiError must become a FAIL CaseResult,
        never propagate out of the case function."""
        ctx = {"web_client": FakeWebClient({}, raise_for={"/"})}
        result = REGISTRY["WEB-DASH-01"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("connection refused", result.reason)

    def test_every_page_case_has_a_distinct_path_and_is_wired(self):
        for cid, path, landmark, expect_nav in C._PAGES:
            self.assertIsNotNone(REGISTRY[cid].judge, f"{cid} has no judge wired")


class HttpGetRawTest(unittest.TestCase):
    def test_sends_accept_encoding_gzip(self):
        """urllib's default Accept-Encoding: identity makes the firmware
        (web_client_accepts_gzip()) answer 406 for its gzip-only embedded
        assets -- this must advertise gzip like a real browser does."""
        resp = mock.MagicMock()
        resp.getcode.return_value = 200
        resp.read.return_value = b"plain body"
        resp.headers.get.return_value = None
        resp.__enter__.return_value = resp
        captured = {}

        def _fake_urlopen(req, timeout=5.0):
            captured["req"] = req
            return resp

        with mock.patch("urllib.request.urlopen", side_effect=_fake_urlopen):
            status, body = C._http_get_raw("1.2.3.4", "/nav.js")
        self.assertEqual(status, 200)
        self.assertEqual(body, "plain body")
        self.assertEqual(captured["req"].get_header("Accept-encoding"), "gzip")

    def test_decodes_gzip_body(self):
        payload = gzip.compress(b'{"ok": true}')
        resp = mock.MagicMock()
        resp.getcode.return_value = 200
        resp.read.return_value = payload
        resp.headers.get.side_effect = lambda name, default=None: "gzip" if name == "Content-Encoding" else default
        resp.__enter__.return_value = resp
        with mock.patch("urllib.request.urlopen", return_value=resp):
            status, body = C._http_get_raw("1.2.3.4", "/theme.css")
        self.assertEqual(status, 200)
        self.assertEqual(body, '{"ok": true}')


class NavX01Test(unittest.TestCase):
    def test_16_links_passes(self):
        text = "\n".join(f"{{ href: '/x{i}', label: 'x' }}," for i in range(16))
        text += "\nactiveFor children"
        with mock.patch.object(C, "_http_get_raw", return_value=(200, text)):
            result = REGISTRY["WEB-X-01"].judge({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_wrong_link_count_fails(self):
        text = "{ href: '/x', label: 'x' },\nactiveFor children"
        with mock.patch.object(C, "_http_get_raw", return_value=(200, text)):
            result = REGISTRY["WEB-X-01"].judge({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_no_host_fails(self):
        result = REGISTRY["WEB-X-01"].judge({})
        self.assertEqual(result.verdict, Verdict.FAIL)


class ParseRouteTierTableTest(unittest.TestCase):
    def test_parses_synthetic_rows(self):
        text = '''
        ROUTE_TIER("/", HTTP_GET, ROUTE_TIER_OPEN),
        ROUTE_TIER("/api/zones", HTTP_POST, ROUTE_TIER_ADMIN),
        '''
        rows = C.parse_route_tier_table(text)
        self.assertEqual(rows, [
            ("/", "HTTP_GET", "ROUTE_TIER_OPEN"),
            ("/api/zones", "HTTP_POST", "ROUTE_TIER_ADMIN"),
        ])

    def test_no_rows_returns_empty_list(self):
        self.assertEqual(C.parse_route_tier_table("no rows here"), [])

    def test_against_the_real_header(self):
        """The whole point of generating WEB-X-03 rather than hardcoding it:
        this must track the real file. The expected count is DERIVED from
        the header by an independent, deliberately dumber counter (every
        ``ROUTE_TIER("`` occurrence outside a comment or preprocessor
        line) rather than hardcoded: a hardcoded 150 went stale the
        moment the live-edit routes landed and the real table reached 155,
        which reddens this test for a reason that has nothing to do with the
        parser it exists to check. The lower bound keeps the derivation from
        going vacuous if the counter itself ever matches nothing."""
        with open(ROUTE_TABLE_PATH, "r", encoding="utf-8") as f:
            text = f.read()
        rows = C.parse_route_tier_table(text)
        expected = len([
            line for line in text.splitlines()
            if 'ROUTE_TIER("' in line and not line.lstrip().startswith(("*", "//", "#"))
        ])
        self.assertGreater(expected, 100, "the independent ROUTE_TIER( counter found suspiciously few rows")
        self.assertEqual(len(rows), expected)
        uris = {uri for uri, _method, _tier in rows}
        self.assertIn("/", uris)
        self.assertIn("/diagnostics", uris)
        self.assertIn("/api/auth/login", uris)


class WebX03Test(unittest.TestCase):
    def _fake_get(self, responses):
        def _get(host, path, timeout=5.0):
            return responses.get(path, (404, None))

        return _get

    def test_open_route_reachable_passes(self):
        text = 'ROUTE_TIER("/api/status", HTTP_GET, ROUTE_TIER_OPEN),'
        ctx = {"host": "1.2.3.4", "route_tier_table_path": "unused"}
        responses = {"/api/auth/config": (200, '{"web_enabled":false}'), "/api/status": (200, "{}")}
        with mock.patch("builtins.open", mock.mock_open(read_data=text)):
            with mock.patch.object(C, "_http_get_raw", side_effect=self._fake_get(responses)):
                result = REGISTRY["WEB-X-03"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_open_route_refused_fails(self):
        """Negative test: an OPEN-tier route that answers >=400 unauthenticated
        is a real defect (auth is blocking a route that must never require
        a credential) and must FAIL, not PASS."""
        text = 'ROUTE_TIER("/api/status", HTTP_GET, ROUTE_TIER_OPEN),'
        ctx = {"host": "1.2.3.4"}
        responses = {"/api/auth/config": (200, '{"web_enabled":false}'), "/api/status": (401, None)}
        with mock.patch("builtins.open", mock.mock_open(read_data=text)):
            with mock.patch.object(C, "_http_get_raw", side_effect=self._fake_get(responses)):
                result = REGISTRY["WEB-X-03"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_open_route_missing_required_param_passes(self):
        """/api/profile_plan and /api/firing_history are ROUTE_TIER_OPEN but
        legitimately answer 400 for a missing id/profile_id query param
        (dashboard_exec_http.c) -- that's not an auth-tier violation and
        must not fail this check (2026-09-21 false-positive fix)."""
        text = "\n".join([
            'ROUTE_TIER("/api/profile_plan", HTTP_GET, ROUTE_TIER_OPEN),',
            'ROUTE_TIER("/api/firing_history", HTTP_GET, ROUTE_TIER_OPEN),',
        ])
        ctx = {"host": "1.2.3.4"}
        responses = {
            "/api/auth/config": (200, '{"web_enabled":false}'),
            "/api/profile_plan": (400, "id missing"),
            "/api/firing_history": (400, "profile_id missing"),
        }
        with mock.patch("builtins.open", mock.mock_open(read_data=text)):
            with mock.patch.object(C, "_http_get_raw", side_effect=self._fake_get(responses)):
                result = REGISTRY["WEB-X-03"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_open_route_not_found_fails(self):
        """A 404 on an OPEN route is the URI-handler-cap failure mode (the
        route silently never got registered, e.g. wifi_provision_http.c's
        160-slot httpd_uri_t cap being hit) -- must still fail, unlike the
        missing-query-param 400 case above (Opus review, MEDIUM)."""
        text = 'ROUTE_TIER("/api/status", HTTP_GET, ROUTE_TIER_OPEN),'
        ctx = {"host": "1.2.3.4"}
        responses = {"/api/auth/config": (200, '{"web_enabled":false}'), "/api/status": (404, None)}
        with mock.patch("builtins.open", mock.mock_open(read_data=text)):
            with mock.patch.object(C, "_http_get_raw", side_effect=self._fake_get(responses)):
                result = REGISTRY["WEB-X-03"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_open_route_server_error_still_fails(self):
        text = 'ROUTE_TIER("/api/status", HTTP_GET, ROUTE_TIER_OPEN),'
        ctx = {"host": "1.2.3.4"}
        responses = {"/api/auth/config": (200, '{"web_enabled":false}'), "/api/status": (500, None)}
        with mock.patch("builtins.open", mock.mock_open(read_data=text)):
            with mock.patch.object(C, "_http_get_raw", side_effect=self._fake_get(responses)):
                result = REGISTRY["WEB-X-03"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_admin_route_requires_401_when_auth_enabled(self):
        text = 'ROUTE_TIER("/diagnostics", HTTP_GET, ROUTE_TIER_ADMIN),'
        ctx = {"host": "1.2.3.4"}
        responses = {"/api/auth/config": (200, '{"web_enabled":true}'), "/diagnostics": (200, "<html></html>")}
        with mock.patch("builtins.open", mock.mock_open(read_data=text)):
            with mock.patch.object(C, "_http_get_raw", side_effect=self._fake_get(responses)):
                result = REGISTRY["WEB-X-03"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_admin_route_ok_when_auth_disabled(self):
        text = 'ROUTE_TIER("/diagnostics", HTTP_GET, ROUTE_TIER_ADMIN),'
        ctx = {"host": "1.2.3.4"}
        responses = {"/api/auth/config": (200, '{"web_enabled":false}'), "/diagnostics": (200, "<html></html>")}
        with mock.patch("builtins.open", mock.mock_open(read_data=text)):
            with mock.patch.object(C, "_http_get_raw", side_effect=self._fake_get(responses)):
                result = REGISTRY["WEB-X-03"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_post_routes_are_recorded_but_never_invoked(self):
        text = 'ROUTE_TIER("/api/zones", HTTP_POST, ROUTE_TIER_ADMIN),'
        ctx = {"host": "1.2.3.4"}
        responses = {"/api/auth/config": (200, '{"web_enabled":true}')}
        calls = []

        def _get(host, path, timeout=5.0):
            calls.append(path)
            return responses.get(path, (404, None))

        with mock.patch("builtins.open", mock.mock_open(read_data=text)):
            with mock.patch.object(C, "_http_get_raw", side_effect=_get):
                result = REGISTRY["WEB-X-03"].judge(ctx)
        self.assertNotIn("/api/zones", calls)
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_side_effect_routes_are_excluded_and_never_fetched(self):
        """/scan, /networks and /api/ota/challenge are read-only by tier but
        have real side effects (Wi-Fi scan, nonce mint) -- the sweep must
        record them as excluded and never fetch them, even though their
        OPEN tier would otherwise mark them safe to exercise."""
        text = "\n".join([
            'ROUTE_TIER("/scan", HTTP_GET, ROUTE_TIER_OPEN),',
            'ROUTE_TIER("/networks", HTTP_GET, ROUTE_TIER_OPEN),',
            'ROUTE_TIER("/api/ota/challenge", HTTP_GET, ROUTE_TIER_OPEN),',
            'ROUTE_TIER("/api/status", HTTP_GET, ROUTE_TIER_OPEN),',
        ])
        ctx = {"host": "1.2.3.4"}
        responses = {"/api/auth/config": (200, '{"web_enabled":false}'), "/api/status": (200, "{}")}
        calls = []

        def _get(host, path, timeout=5.0):
            calls.append(path)
            return responses.get(path, (404, None))

        with mock.patch("builtins.open", mock.mock_open(read_data=text)):
            with mock.patch.object(C, "_http_get_raw", side_effect=_get):
                result = REGISTRY["WEB-X-03"].judge(ctx)

        for excluded in ("/scan", "/networks", "/api/ota/challenge"):
            self.assertNotIn(excluded, calls)

        rows_by_uri = {r["uri"]: r for r in result.observed["rows"]}
        for excluded in ("/scan", "/networks", "/api/ota/challenge"):
            row = rows_by_uri[excluded]
            self.assertFalse(row["exercised"])
            self.assertEqual(row["detail"], "excluded: side effect")

        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(result.observed["exercised"], 1)

    def test_unreadable_table_file_fails(self):
        ctx = {"host": "1.2.3.4", "route_tier_table_path": os.path.join(REPO_ROOT, "does_not_exist.h")}
        result = REGISTRY["WEB-X-03"].judge(ctx)
        self.assertEqual(result.verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
