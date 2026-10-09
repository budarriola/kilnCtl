#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_static (ST-01..04). Fake workbench
functions only -- the real ones launch real builds.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_static.py -q
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_static as C  # noqa: E402
from kilnctrl.bench_test.registry import REGISTRY, Verdict  # noqa: E402

OK = "tag: OK in 1.0s (3 log lines)\nfull log: x\n--\n"
CHECKS_OK = OK + "160 passed, 1 skipped (0 due to -Fast), 0 failed.\n"
BAD = "tag: FAILED (exit 1) in 1.0s (3 log lines)\nfull log: x\n--\nboom"


class CtxTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        fw = os.path.join(self.tmp, "firmware", "KilnFW")
        os.makedirs(os.path.join(fw, "build"))
        with open(os.path.join(fw, "partitions.csv"), "w") as fh:
            fh.write("# Name, Type, SubType, Offset, Size, Flags\n"
                     "nvs, data, nvs, 0x9000, 0x6000,\n"
                     "app, app, ota_0, 0x10000, 0x100000,\n")
        self.bin = os.path.join(fw, "build", "KilnCtrl.bin")
        self.set_bin(1000)

    def set_bin(self, n):
        with open(self.bin, "wb") as fh:
            fh.write(b"\0" * n)

    def ctx(self, **kw):
        c = dict(repo_root="C:/wt/x",
                 run_repo_checks_fn=lambda: CHECKS_OK,
                 run_pctools_tests_fn=lambda: OK,
                 build_saftyfw_host_tests_fn=lambda: OK,
                 build_kilnfw_fn=lambda: OK,
                 kiln_fw_root=os.path.join(self.tmp, "firmware", "KilnFW"))
        c.update(kw)
        return c


class StaticTest(CtxTest):
    def test_wired(self):
        for cid in ("ST-01", "ST-02", "ST-03", "ST-04"):
            self.assertIsNotNone(REGISTRY[cid].judge)

    def test_all_pass_and_counts_recorded(self):
        c = self.ctx()
        for cid in ("ST-01", "ST-02", "ST-03", "ST-04"):
            r = REGISTRY[cid].judge(c)
            self.assertEqual(r.verdict, Verdict.PASS, (cid, r.reason))
        r = REGISTRY["ST-01"].judge(c)
        self.assertEqual((r.observed["passed"], r.observed["skipped"], r.observed["failed"]), (160, 1, 0))

    def test_long_path_skips_st03(self):
        r = REGISTRY["ST-03"].judge(self.ctx(repo_root="C:/" + "a" * 80))
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertIn("path_too_long", r.reason)

    def test_nonzero_exit_fails(self):
        self.assertEqual(REGISTRY["ST-01"].judge(self.ctx(run_repo_checks_fn=lambda: BAD)).verdict, Verdict.FAIL)
        self.assertEqual(REGISTRY["ST-02"].judge(self.ctx(run_pctools_tests_fn=lambda: BAD)).verdict, Verdict.FAIL)
        self.assertEqual(REGISTRY["ST-03"].judge(self.ctx(build_saftyfw_host_tests_fn=lambda: BAD)).verdict, Verdict.FAIL)
        self.assertEqual(REGISTRY["ST-04"].judge(self.ctx(build_kilnfw_fn=lambda: BAD)).verdict, Verdict.FAIL)

    def test_multiline_failed_kilnfw_after_ok_saftyfw_fails(self):
        two = ("saftyfw: OK in 40.0s (10 log lines)\nfull log: a\n--\n"
               "kilnfw: FAILED (exit 1) in 5.0s (3 log lines)\nfull log: b\n--\nboom")
        r = REGISTRY["ST-04"].judge(self.ctx(build_kilnfw_fn=lambda: two))
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_failed_exit0_output_check_fails(self):
        txt = ("pctools: FAILED (exit 0, but output check failed) in 9.0s (3 log lines)\n"
               "full log: x\n--\n12 passed\n")
        for cid, key in (("ST-02", "run_pctools_tests_fn"), ("ST-03", "build_saftyfw_host_tests_fn"),
                         ("ST-04", "build_kilnfw_fn")):
            r = REGISTRY[cid].judge(self.ctx(**{key: lambda: txt}))
            self.assertEqual(r.verdict, Verdict.FAIL, cid)
        self.assertEqual(REGISTRY["ST-01"].judge(self.ctx(run_repo_checks_fn=lambda: txt + CHECKS_OK)).verdict, Verdict.FAIL)

    def test_pass_count_drop_fails(self):
        r = REGISTRY["ST-01"].judge(self.ctx(st01_baseline_passed=161))
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertEqual(REGISTRY["ST-01"].judge(self.ctx(st01_baseline_passed=160)).verdict, Verdict.PASS)

    def test_oversize_bin_fails(self):
        self.set_bin(0x100000)
        r = REGISTRY["ST-04"].judge(self.ctx())
        self.assertEqual(r.verdict, Verdict.FAIL)

    def test_unparseable_is_error_not_pass(self):
        for cid, key in (("ST-01", "run_repo_checks_fn"), ("ST-02", "run_pctools_tests_fn"),
                         ("ST-04", "build_kilnfw_fn")):
            r = REGISTRY[cid].judge(self.ctx(**{key: lambda: "garbage"}))
            self.assertNotEqual(r.verdict, Verdict.PASS)
            self.assertTrue(r.reason.startswith("ERROR:"))
        # exit 0 but no counts line
        r = REGISTRY["ST-01"].judge(self.ctx(run_repo_checks_fn=lambda: OK))
        self.assertNotEqual(r.verdict, Verdict.PASS)


if __name__ == "__main__":
    unittest.main()
