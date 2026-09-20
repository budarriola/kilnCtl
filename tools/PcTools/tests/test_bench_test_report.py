#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.report -- summary.json/transcript.md
writers and the run_id/redaction helpers. No board, no HTTP.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_report.py -q
"""
from __future__ import annotations

import dataclasses
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import report as report_mod  # noqa: E402
from kilnctrl.bench_test.registry import CaseResult, Verdict  # noqa: E402


@dataclasses.dataclass
class _FakeOutcome:
    run_id: str
    suite: str
    requested: list
    executed: list
    results: dict
    started: float
    ended: float
    preflight_ok: bool
    preflight_reason: str
    tainted: bool = False

    @property
    def exit_code(self):
        if not self.preflight_ok:
            return 1
        verdicts = [r.verdict for r in self.results.values()]
        if any(v == Verdict.FAIL for v in verdicts):
            return 1
        if any(v in (Verdict.SKIP, Verdict.INCONCLUSIVE, Verdict.NOT_RUN) for v in verdicts):
            return 3
        return 0


class MakeRunIdTest(unittest.TestCase):
    def test_run_id_has_the_suite_name(self):
        run_id = report_mod.make_run_id("smoke")
        self.assertIn("smoke", run_id)

    def test_tag_is_appended(self):
        run_id = report_mod.make_run_id("smoke", tag="bench-run-1")
        self.assertTrue(run_id.endswith("bench-run-1"))

    def test_unsafe_tag_characters_are_sanitized(self):
        run_id = report_mod.make_run_id("smoke", tag="a/b c!d")
        self.assertNotIn("/", run_id)
        self.assertNotIn(" ", run_id)
        self.assertNotIn("!", run_id)


class RedactTest(unittest.TestCase):
    def test_json_style_password_is_redacted(self):
        text = '{"ap_password": "sekret123"}'
        out = report_mod._redact(text)
        self.assertNotIn("sekret123", out)
        self.assertIn("***", out)

    def test_kv_style_password_is_redacted(self):
        text = "calling with ap_password=sekret123 now"
        out = report_mod._redact(text)
        self.assertNotIn("sekret123", out)

    def test_unrelated_text_is_unchanged(self):
        text = "ST-05: PASS"
        self.assertEqual(report_mod._redact(text), text)


class BuildSummaryAndWriteRunTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="bench_test_report_test_")

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _make_outcome(self):
        return _FakeOutcome(
            run_id="20260101T000000Z_smoke",
            suite="smoke",
            requested=["ST-05"],
            executed=["ST-05"],
            results={"ST-05": CaseResult(Verdict.PASS, observed={"dirty_files": []})},
            started=1000.0,
            ended=1001.5,
            preflight_ok=True,
            preflight_reason="",
        )

    def test_build_summary_has_the_expected_shape(self):
        summary = report_mod.build_summary(self._make_outcome(), {"a": 1}, {"b": 2})
        self.assertEqual(summary["run_id"], "20260101T000000Z_smoke")
        self.assertEqual(summary["exit_code"], 0)
        self.assertEqual(summary["cases"]["ST-05"]["verdict"], Verdict.PASS)
        self.assertAlmostEqual(summary["duration_s"], 1.5)
        self.assertEqual(summary["board_before"], {"a": 1})
        self.assertEqual(summary["board_after"], {"b": 2})

    def test_verdicts_footer_is_redacted(self):
        """report.py:109-111's `## Verdicts` footer used to be written
        straight from the un-redacted summary dict, bypassing _redact()
        even though every other artifact this module writes is redacted.
        Prove a secret sitting in a case's `reason` -- surfaced via a
        credential-shaped env var, the same mechanism _redact() scans
        for -- never survives into the footer."""
        secret = "supersekretvalue999"
        os.environ["KILNCTL_WEB_PASSWORD"] = secret
        try:
            outcome = _FakeOutcome(
                run_id="20260101T000000Z_smoke",
                suite="smoke",
                requested=["ST-05"],
                executed=["ST-05"],
                results={
                    "ST-05": CaseResult(
                        Verdict.FAIL, reason=f"leaked value {secret} in reason", observed={}
                    )
                },
                started=1000.0,
                ended=1001.5,
                preflight_ok=True,
                preflight_reason="",
            )
            summary = report_mod.build_summary(outcome, {}, {})
            run_dir = os.path.join(self.tmpdir, "20260101T000000Z_smoke_footer")
            report_mod.write_run(run_dir, summary, ["line one"],
                                  log_doc_path=os.path.join(self.tmpdir, "BENCH_TEST_LOG.md"))
            with open(os.path.join(run_dir, "transcript.md"), encoding="utf-8") as f:
                transcript = f.read()
            self.assertIn("## Verdicts", transcript)
            self.assertIn("ST-05", transcript)
            self.assertNotIn(secret, transcript)
        finally:
            del os.environ["KILNCTL_WEB_PASSWORD"]

    def test_write_run_creates_summary_and_transcript_and_captures_dir(self):
        summary = report_mod.build_summary(self._make_outcome(), {}, {})
        run_dir = os.path.join(self.tmpdir, "20260101T000000Z_smoke")
        report_mod.write_run(run_dir, summary, ["line one", "ap_password=hunter2"],
                              log_doc_path=os.path.join(self.tmpdir, "BENCH_TEST_LOG.md"))

        self.assertTrue(os.path.isdir(os.path.join(run_dir, "captures")))
        with open(os.path.join(run_dir, "summary.json"), encoding="utf-8") as f:
            loaded = json.load(f)
        self.assertEqual(loaded["run_id"], "20260101T000000Z_smoke")

        with open(os.path.join(run_dir, "transcript.md"), encoding="utf-8") as f:
            transcript = f.read()
        self.assertIn("ST-05", transcript)
        self.assertNotIn("hunter2", transcript)

    def test_list_recent_runs_returns_newest_first(self):
        for run_id in ("20260101T000000Z_smoke", "20260102T000000Z_smoke"):
            run_dir = os.path.join(self.tmpdir, run_id)
            os.makedirs(run_dir)
            with open(os.path.join(run_dir, "summary.json"), "w", encoding="utf-8") as f:
                json.dump({"run_id": run_id}, f)
        runs = report_mod.list_recent_runs(logs_root=self.tmpdir, n=2)
        self.assertEqual(runs[0]["run_id"], "20260102T000000Z_smoke")

    def test_list_recent_runs_on_missing_root_returns_empty(self):
        missing = os.path.join(self.tmpdir, "does-not-exist")
        self.assertEqual(report_mod.list_recent_runs(logs_root=missing), [])

    def test_write_run_appends_one_log_line(self):
        summary = report_mod.build_summary(self._make_outcome(), {}, {})
        run_dir = os.path.join(self.tmpdir, "20260101T000000Z_smoke")
        doc_path = os.path.join(self.tmpdir, "BENCH_TEST_LOG.md")
        report_mod.write_run(run_dir, summary, ["line one"], log_doc_path=doc_path)
        with open(doc_path, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("20260101T000000Z_smoke", text)
        self.assertIn("PASS=1", text)
        # exactly one appended log-line entry (no accidental duplication or
        # read-modify-write of a previous line) -- count bullet lines, not
        # occurrences of the run id, since it also appears inside the run
        # dir path this particular run_dir happens to share.
        log_lines = [ln for ln in text.splitlines() if ln.startswith("- `")]
        self.assertEqual(len(log_lines), 1)


class AppendLogLineTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="bench_test_log_test_")
        self.doc_path = os.path.join(self.tmpdir, "BENCH_TEST_LOG.md")

    def tearDown(self):
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def _summary(self, **overrides):
        base = {
            "run_id": "20260101T000000Z_nightly",
            "suite": "nightly",
            "exit_code": 0,
            "cases": {
                "ST-05": {"verdict": Verdict.PASS},
                "FL-01": {"verdict": Verdict.FAIL},
                "SK-01": {"verdict": Verdict.INCONCLUSIVE},
                "SP-01": {"verdict": Verdict.NOT_RUN},
                "SP-02": {"verdict": Verdict.SKIP},
            },
            "board_before": {
                "esp_fw_build": "2026-09-19T12:00:00Z",
                "safety_get_fw_version": "commit=abc1234, dirty=False",
            },
        }
        base.update(overrides)
        return base

    def test_creates_file_with_header_on_first_use(self):
        report_mod.append_log_line(self._summary(), "/some/run/dir", doc_path=self.doc_path)
        with open(self.doc_path, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("# Bench Test Log", text)
        self.assertIn("20260101T000000Z_nightly", text)

    def test_line_carries_the_four_named_verdict_counts_and_fw_builds(self):
        line = report_mod.append_log_line(self._summary(), "/some/run/dir", doc_path=self.doc_path)
        self.assertIn("PASS=1", line)
        self.assertIn("FAIL=1", line)
        self.assertIn("INCONCLUSIVE=1", line)
        self.assertIn("NOT_RUN=1", line)
        self.assertIn("esp_fw=2026-09-19T12:00:00Z", line)
        self.assertIn("pico_fw=commit=abc1234", line)

    def test_second_call_appends_rather_than_overwrites(self):
        report_mod.append_log_line(self._summary(run_id="run-1"), "/dir/1", doc_path=self.doc_path)
        report_mod.append_log_line(self._summary(run_id="run-2"), "/dir/2", doc_path=self.doc_path)
        with open(self.doc_path, encoding="utf-8") as f:
            text = f.read()
        self.assertIn("run-1", text)
        self.assertIn("run-2", text)
        self.assertEqual(text.count("# Bench Test Log"), 1, "header must be written only once")

    def test_credential_shaped_env_value_never_reaches_the_log_line(self):
        secret = "supersekretvalue999"
        os.environ["KILNCTL_WEB_PASSWORD"] = secret
        try:
            summary = self._summary(board_before={
                "esp_fw_build": f"leaked {secret}",
                "safety_get_fw_version": "commit=abc1234",
            })
            line = report_mod.append_log_line(summary, "/some/dir", doc_path=self.doc_path)
            self.assertNotIn(secret, line)
        finally:
            del os.environ["KILNCTL_WEB_PASSWORD"]

    def test_missing_board_before_fields_degrade_to_unknown(self):
        line = report_mod.append_log_line(self._summary(board_before={}), "/some/dir", doc_path=self.doc_path)
        self.assertIn("esp_fw=unknown", line)
        self.assertIn("pico_fw=unknown", line)


if __name__ == "__main__":
    unittest.main()
