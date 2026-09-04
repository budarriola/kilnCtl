#!/usr/bin/env python3
"""Tests for kilnctrl.jsonl_util.iter_jsonl (B10 ROADMAP.md M15 item).

Covers the three ``on_error`` behaviours it has to preserve exactly, since
each call site it replaces had picked a different one before this helper
existed: skip-silent (coupling_pair_log/http_capture_log/link_hub's second
loop), raise (relay_ku_tu_check, which never caught json.loads() at all),
and callback-then-skip (link_hub's first loop, which logs before skipping).

Run with: python -m pytest tools/PcTools/tests/test_jsonl_util.py
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.jsonl_util import iter_jsonl


class IterJsonlWellFormedTests(unittest.TestCase):
    def test_path_source(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = os.path.join(tmp, "log.jsonl")
            with open(path, "w", encoding="utf-8") as f:
                f.write('{"a": 1}\n{"a": 2}\n')
            got = list(iter_jsonl(path))
        self.assertEqual(got, [{"a": 1}, {"a": 2}])

    def test_iterable_of_lines_source(self):
        lines = ['{"a": 1}\n', '{"a": 2}\n']
        got = list(iter_jsonl(lines))
        self.assertEqual(got, [{"a": 1}, {"a": 2}])

    def test_blank_lines_skipped(self):
        lines = ['{"a": 1}\n', "\n", "   \n", '{"a": 2}\n']
        got = list(iter_jsonl(lines))
        self.assertEqual(got, [{"a": 1}, {"a": 2}])


class IterJsonlMalformedSkipTests(unittest.TestCase):
    """on_error="skip" (the default): silently skip a malformed line."""

    def test_default_skips_malformed_line(self):
        lines = ['{"a": 1}\n', "not json\n", '{"a": 2}\n']
        got = list(iter_jsonl(lines))
        self.assertEqual(got, [{"a": 1}, {"a": 2}])

    def test_explicit_skip_same_as_default(self):
        lines = ['{"a": 1}\n', "{broken\n", '{"a": 2}\n']
        got = list(iter_jsonl(lines, on_error="skip"))
        self.assertEqual(got, [{"a": 1}, {"a": 2}])


class IterJsonlMalformedRaiseTests(unittest.TestCase):
    """on_error="raise": propagate json.JSONDecodeError, matching
    relay_ku_tu_check._read_jsonl's prior (uncaught) behaviour."""

    def test_raise_on_malformed_line(self):
        lines = ['{"a": 1}\n', "not json\n", '{"a": 2}\n']
        it = iter_jsonl(lines, on_error="raise")
        self.assertEqual(next(it), {"a": 1})
        with self.assertRaises(json.JSONDecodeError):
            next(it)

    def test_raise_does_not_fire_on_well_formed_input(self):
        lines = ['{"a": 1}\n', '{"a": 2}\n']
        got = list(iter_jsonl(lines, on_error="raise"))
        self.assertEqual(got, [{"a": 1}, {"a": 2}])


class IterJsonlMalformedCallbackTests(unittest.TestCase):
    """on_error=<callable>: called with the raw stripped line, then the
    line is skipped -- matches link_hub.py's log.debug(...)-and-continue."""

    def test_callback_invoked_and_line_skipped(self):
        seen = []
        lines = ['{"a": 1}\n', "  garbage  \n", '{"a": 2}\n']
        got = list(iter_jsonl(lines, on_error=seen.append))
        self.assertEqual(got, [{"a": 1}, {"a": 2}])
        self.assertEqual(seen, ["garbage"])  # stripped, matching link_hub's %r log

    def test_callback_not_invoked_on_well_formed_input(self):
        seen = []
        lines = ['{"a": 1}\n']
        list(iter_jsonl(lines, on_error=seen.append))
        self.assertEqual(seen, [])


class IterJsonlWithLineTests(unittest.TestCase):
    """with_line=True: needed by coupling_pair_log.load_thermo_samples_any_format,
    which falls back to re-parsing the raw text a different way on failure."""

    def test_with_line_pairs_well_formed(self):
        lines = ['{"a": 1}\n']
        got = list(iter_jsonl(lines, with_line=True))
        self.assertEqual(got, [('{"a": 1}', {"a": 1})])

    def test_with_line_yields_none_obj_on_malformed_skip(self):
        lines = ["23:32:34 not-json-but-has-a-brace {\n"]
        got = list(iter_jsonl(lines, with_line=True, on_error="skip"))
        self.assertEqual(len(got), 1)
        line, obj = got[0]
        self.assertEqual(line, "23:32:34 not-json-but-has-a-brace {")
        self.assertIsNone(obj)


if __name__ == "__main__":
    unittest.main()
