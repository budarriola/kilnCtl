#!/usr/bin/env python3
"""Unit tests for kilnctrl.page_structure and its MCP wrapper
board_page_structure.

What these guard:

1. **The inventory is right.** ids, class-prefix counts, SVG symbol ids and
   ``<use>`` references are computed from synthetic HTML shaped like the real
   /settings/zones page (a ``kg-`` classed kiln graphic plus icon symbols),
   including the mismatch case -- an icon referenced but never defined, which
   is what "the graphic is incomplete" actually looks like in markup.

2. **gzip is handled and measured.** A gzipped response must report the wire
   size AND the decompressed size, and a truncated/corrupt gzip body must
   fail loudly rather than being parsed as an empty page (which would read as
   "the graphic is missing").

3. **An authentication failure is a clear sentence, not a traceback.** Both
   shapes are exercised: a 401 from the board, and http_auth refusing for
   lack of a credential. The MCP wrapper must return an ``error: ...`` string
   naming the two environment variables.

No socket is opened by any test here.

Run with: kiln_call(name="run_pctools_tests") -- or
python -m pytest tools/PcTools/tests -q
"""
from __future__ import annotations

import gzip
import io
import os
import sys
import unittest
import unittest.mock
import urllib.error

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl import http_auth  # noqa: E402
from kilnctrl import page_structure as ps  # noqa: E402


SAMPLE_PAGE = """
<html><head><title>Zones</title></head>
<body>
  <div id="wrap" class="card card-body">
    <svg class="kg-root" id="kilnGraphic">
      <symbol id="icon-gear"><path/></symbol>
      <symbol id="icon-flame"><path/></symbol>
      <rect class="kg-shell kg-outline"/>
      <rect class="kg-zone kg-zone-0"/>
      <rect class="kg-zone kg-zone-1"/>
      <use href="#icon-gear"/>
      <use xlink:href="#icon-flame"/>
      <use href="#icon-missing"/>
    </svg>
    <button id="saveBtn" class="btn btn-primary">Save</button>
  </div>
</body></html>
"""


class _FakeResponse(io.BytesIO):
    def __init__(self, body: bytes, headers: dict, status: int = 200):
        super().__init__(body)
        self.status = status
        self.headers = _FakeHeaders(headers)

    def getcode(self):
        return self.status

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
        return False


class _FakeHeaders:
    def __init__(self, mapping: dict):
        self._map = {k.lower(): v for k, v in mapping.items()}

    def get(self, name, default=None):
        return self._map.get(name.lower(), default)


class InventoryTest(unittest.TestCase):
    def setUp(self):
        (self.ids, self.prefixes, self.tokens,
         self.symbols, self.uses) = ps.inventory(SAMPLE_PAGE)

    def test_element_ids_collected_in_order_without_duplicates(self):
        self.assertEqual(self.ids,
                         ["wrap", "kilnGraphic", "icon-gear", "icon-flame", "saveBtn"])

    def test_class_prefix_counts(self):
        # kg-root, kg-shell, kg-outline, kg-zone x2, kg-zone-0, kg-zone-1 = 7
        self.assertEqual(self.prefixes["kg-"], 7)
        self.assertEqual(self.prefixes["card"], 1)
        self.assertEqual(self.prefixes["card-"], 1)
        self.assertEqual(self.prefixes["btn"], 1)
        self.assertEqual(self.tokens, sum(self.prefixes.values()))

    def test_prefix_ordering_is_most_common_first(self):
        self.assertEqual(next(iter(self.prefixes)), "kg-")

    def test_symbol_ids_and_use_refs(self):
        self.assertEqual(self.symbols, ["icon-gear", "icon-flame"])
        self.assertEqual(self.uses, ["icon-gear", "icon-flame", "icon-missing"])

    def test_class_prefix_helper(self):
        self.assertEqual(ps.class_prefix("kg-shell"), "kg-")
        self.assertEqual(ps.class_prefix("btn"), "btn")


class SourcePrefixTest(unittest.TestCase):
    """The load-bearing measurement for the motivating case. The real
    /settings/zones page builds its kiln graphic in script, so its kg-
    classes never reach a class= attribute: an attribute-only count reports
    0 for a page that carries the graphic in full (measured on the bench
    board 2026-09-18: 77 source occurrences, 0 parsed attributes). This
    covers that shape."""

    SCRIPT_BUILT = """
    <html><head><style>
      .kg-annot text { fill: red; }
      .kg-shell { stroke: black; }
      .kg-zone { fill: none; }
    </style></head><body><div id="host"></div><script>
      var cls = bad ? 'kg-bad' : 'kg-good';
      el.innerHTML = '<rect class="kg-strike"/>';
    </script></body></html>
    """

    def test_source_scan_sees_what_the_markup_parser_cannot(self):
        _ids, class_prefixes, tokens, _sym, _use = ps.inventory(self.SCRIPT_BUILT)
        self.assertEqual(class_prefixes.get("kg-", 0), 0,
                         "precondition: no static kg- class attribute in this page")
        self.assertEqual(tokens, 0)
        source = ps.source_prefix_counts(self.SCRIPT_BUILT)
        self.assertGreaterEqual(source["kg-"], 6)

    def test_min_count_filters_prose_noise(self):
        prose = "a well-known and hard-won result, re-checked."
        self.assertEqual(ps.source_prefix_counts(prose), {})
        self.assertIn("re-", ps.source_prefix_counts(prose, min_count=1))

    def test_counts_are_ordered_most_common_first(self):
        source = ps.source_prefix_counts(self.SCRIPT_BUILT)
        self.assertEqual(next(iter(source)), "kg-")

    def test_analyze_reports_both_counts_and_labels_them(self):
        with unittest.mock.patch.object(
                ps.http_auth, "urlopen",
                return_value=_FakeResponse(self.SCRIPT_BUILT.encode(),
                                           {"Content-Type": "text/html"})):
            page = ps.analyze(host="10.0.0.9")
        self.assertGreaterEqual(page.source_prefixes["kg-"], 6)
        text = ps.format_structure(page)
        self.assertIn("source prefixes", text)
        self.assertIn("as class attribute: 0", text)


class UndefinedRefTest(unittest.TestCase):
    """An icon referenced but never defined is exactly what an incomplete
    graphic looks like in markup -- it must be named, not swallowed."""

    def test_missing_symbol_is_reported(self):
        page = _analyze_bytes(SAMPLE_PAGE.encode())
        self.assertEqual(page.undefined_use_refs, ["icon-missing"])
        self.assertIn("icon-missing", ps.format_structure(page))
        self.assertIn("WARNING", ps.format_structure(page))


def _analyze_bytes(body: bytes, headers=None, status=200, gzipped=False):
    payload = gzip.compress(body) if gzipped else body
    hdrs = {"Content-Type": "text/html"}
    if gzipped:
        hdrs["Content-Encoding"] = "gzip"
    hdrs.update(headers or {})
    with unittest.mock.patch.object(
            ps.http_auth, "urlopen",
            return_value=_FakeResponse(payload, hdrs, status)):
        return ps.analyze(host="10.0.0.9", path="/settings/zones")


class FetchTest(unittest.TestCase):
    def test_plain_response_sizes(self):
        body = SAMPLE_PAGE.encode()
        page = _analyze_bytes(body)
        self.assertEqual(page.status, 200)
        self.assertFalse(page.gzipped)
        self.assertEqual(page.wire_bytes, len(body))
        self.assertEqual(page.decoded_bytes, len(body))
        self.assertEqual(page.url, "http://10.0.0.9/settings/zones")

    def test_gzipped_response_reports_both_sizes(self):
        body = (SAMPLE_PAGE * 40).encode()
        page = _analyze_bytes(body, gzipped=True)
        self.assertTrue(page.gzipped)
        self.assertEqual(page.decoded_bytes, len(body))
        self.assertLess(page.wire_bytes, page.decoded_bytes)
        self.assertGreater(page.class_prefixes["kg-"], 100)

    def test_truncated_gzip_fails_loudly(self):
        """A half-arrived compressed page must NOT parse as an empty page --
        that would read as 'the kiln graphic is missing'."""
        broken = gzip.compress(SAMPLE_PAGE.encode())[: 20]
        with unittest.mock.patch.object(
                ps.http_auth, "urlopen",
                return_value=_FakeResponse(
                    broken, {"Content-Encoding": "gzip", "Content-Type": "text/html"})):
            with self.assertRaises(ps.PageStructureError) as ctx:
                ps.analyze(host="10.0.0.9")
        self.assertIn("truncated", str(ctx.exception).lower())

    def test_build_url_accepts_bare_host_and_full_origin(self):
        self.assertEqual(ps.build_url("192.168.1.156", "/settings/zones"),
                         "http://192.168.1.156/settings/zones")
        self.assertEqual(ps.build_url("http://192.168.1.156/", "settings/zones"),
                         "http://192.168.1.156/settings/zones")


class AuthFailureTest(unittest.TestCase):
    def test_401_is_a_clear_sentence(self):
        err = urllib.error.HTTPError("http://b/settings/zones", 401, "Unauthorized", None, None)
        with unittest.mock.patch.object(ps.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ps.PageStructureError) as ctx:
                ps.analyze(host="b")
        message = str(ctx.exception)
        self.assertIn("401", message)
        self.assertIn(http_auth.USERNAME_ENV, message)
        self.assertIn(http_auth.PASSWORD_ENV, message)

    def test_missing_credential_is_reported_not_raised_as_a_traceback(self):
        err = http_auth.HttpAuthError("KILNCTL_WEB_USERNAME is not set in the environment")
        with unittest.mock.patch.object(ps.http_auth, "urlopen", side_effect=err):
            with self.assertRaises(ps.PageStructureError) as ctx:
                ps.analyze(host="b")
        self.assertIn("authentication", str(ctx.exception))

    def test_unreachable_board(self):
        with unittest.mock.patch.object(
                ps.http_auth, "urlopen",
                side_effect=urllib.error.URLError("timed out")):
            with self.assertRaises(ps.PageStructureError) as ctx:
                ps.analyze(host="b")
        self.assertIn("not reachable", str(ctx.exception))


class McpWrapperTest(unittest.TestCase):
    """The tool must ANSWER on a failure, never raise -- the server's tool
    contract."""

    @classmethod
    def setUpClass(cls):
        from kilnctrl import mcp_server as m  # noqa: E402
        cls.m = m

    def test_auth_failure_comes_back_as_an_error_string(self):
        err = urllib.error.HTTPError("http://b/settings/zones", 401, "Unauthorized", None, None)
        with unittest.mock.patch.object(ps.http_auth, "urlopen", side_effect=err):
            result = self.m.board_page_structure(host="10.0.0.9")
        self.assertTrue(result.startswith("error: "), result[:80])
        self.assertIn(http_auth.USERNAME_ENV, result)

    def test_success_renders_the_inventory(self):
        with unittest.mock.patch.object(
                ps.http_auth, "urlopen",
                return_value=_FakeResponse(SAMPLE_PAGE.encode(), {"Content-Type": "text/html"})):
            result = self.m.board_page_structure(host="10.0.0.9")
        self.assertIn("HTTP 200", result)
        self.assertIn("kg-", result)
        self.assertIn("icon-gear", result)


class FacadeDiscoverabilityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        from kilnctrl import mcp_server as m  # noqa: E402
        cls.registry = m.registry

    def test_registered_at_all(self):
        self.assertIn("board_page_structure", self.registry.by_name)

    def test_not_directly_published_on_the_wire(self):
        from kilnctrl import mcp_server as m
        self.assertNotIn("board_page_structure", set(m.mcp._tool_manager._tools))

    def test_found_by_the_questions_a_person_asks(self):
        for query in ("is the kiln graphic on the zones page",
                      "check a served web page structure",
                      "what element ids does the settings page declare",
                      "svg icon symbols in the web ui page"):
            with self.subTest(query=query):
                hits = [hit.name for hit in self.registry.search(query)]
                self.assertIn("board_page_structure", hits[:3],
                              f"not surfaced by kiln_find({query!r}): {hits[:3]}")


if __name__ == "__main__":
    unittest.main()
