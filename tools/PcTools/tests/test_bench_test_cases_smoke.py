#!/usr/bin/env python3
"""Unit tests for kilnctrl.bench_test.cases_smoke -- the fetch half of each
smoke case. Every board/HTTP call is mocked; these tests only confirm each
`_case_XXX` fetches the right thing and hands it to the right judge
function, never that a real board answers a certain way.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_cases_smoke.py -q
"""
from __future__ import annotations

import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))

from kilnctrl.bench_test import cases_smoke as C  # noqa: E402
from kilnctrl.bench_test.registry import Verdict  # noqa: E402


class FakeSrv:
    def __init__(self, **overrides):
        self._overrides = overrides

    def __getattr__(self, name):
        if name in self._overrides:
            value = self._overrides[name]
            return value if callable(value) else (lambda *a, **kw: value)
        raise AttributeError(name)


class St05Test(unittest.TestCase):
    def test_clean_tree_passes(self):
        fake_state = mock.Mock(dirty_files=[], sensitive_files=[], head="abc123")
        with mock.patch("kilnctrl.flash_provenance.capture_tree_state", return_value=fake_state):
            result = C._case_st05({})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_sensitive_dirty_file_fails(self):
        fake_state = mock.Mock(dirty_files=["a.c"], sensitive_files=["zones_config_json.c"], head="abc123")
        with mock.patch("kilnctrl.flash_provenance.capture_tree_state", return_value=fake_state):
            result = C._case_st05({})
        self.assertEqual(result.verdict, Verdict.FAIL)


class Fl01Test(unittest.TestCase):
    def test_match_report_passes(self):
        srv = FakeSrv(debug_check_partition_table=lambda host=None: "partitions.csv vs chip: MATCH")
        result = C._case_fl01({"srv": srv, "host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_mismatch_report_fails(self):
        srv = FakeSrv(debug_check_partition_table=lambda host=None: "MISMATCH at row 3")
        result = C._case_fl01({"srv": srv, "host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)


class Fl02Test(unittest.TestCase):
    def test_app_running_passes(self):
        with mock.patch("kilnctrl.partition_http_client.get_partitions", return_value={"running": "app"}):
            result = C._case_fl02({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_client_exception_fails_not_crashes(self):
        with mock.patch("kilnctrl.partition_http_client.get_partitions", side_effect=OSError("unreachable")):
            result = C._case_fl02({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)
        self.assertIn("unreachable", result.reason)


class Fl04Test(unittest.TestCase):
    def test_healthy_boot_guard_passes(self):
        data = {"recovery_mode": False, "boot_count": 0}
        with mock.patch("kilnctrl.ota_http_client.get_boot_guard_status", return_value=data):
            result = C._case_fl04({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_recovery_mode_fails(self):
        data = {"recovery_mode": True, "boot_count": 5}
        with mock.patch("kilnctrl.ota_http_client.get_boot_guard_status", return_value=data):
            result = C._case_fl04({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)


class Fl06Test(unittest.TestCase):
    def test_200_ok_passes(self):
        with mock.patch.object(C, "_http_get_json", return_value=(200, {"present": False})):
            result = C._case_fl06({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_unreachable_host_fails(self):
        with mock.patch.object(C, "_http_get_json", return_value=(None, "Connection refused")):
            result = C._case_fl06({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)


class Fl08And09Test(unittest.TestCase):
    def test_fl08_extracts_commit_and_boot_reason(self):
        srv = FakeSrv(
            safety_get_fw_version=lambda: "SaftyFW build 82548f2e clean",
            safety_get_diag=lambda: "boot_reason: power_on, uptime: 120",
        )
        ctx = {"srv": srv}
        result = C._case_fl08(ctx)
        self.assertEqual(result.verdict, Verdict.PASS)
        self.assertEqual(ctx["_fl08_commit"], "82548f2e")

    def test_fl09_not_run_when_fl08_commit_missing(self):
        result = C._case_fl09({"srv": FakeSrv()})
        self.assertEqual(result.verdict, Verdict.NOT_RUN)

    def test_fl09_runs_when_commit_present(self):
        srv = FakeSrv(find_safty_crash_elf=lambda commit: f"archived ELF for {commit}: SaftyFW-{commit}.elf")
        result = C._case_fl09({"srv": srv, "_fl08_commit": "82548f2e"})
        self.assertEqual(result.verdict, Verdict.PASS)


class Sk03Test(unittest.TestCase):
    def test_healthy_tasks_pass(self):
        body = {"tasks": [{"name": "main", "configured": 4096, "free": 2048}]}
        with mock.patch.object(C, "_http_get_json", return_value=(200, body)):
            result = C._case_sk03({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_non_200_fails(self):
        with mock.patch.object(C, "_http_get_json", return_value=(500, {})):
            result = C._case_sk03({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)


class Sp05Test(unittest.TestCase):
    def test_not_asserted_passes(self):
        resp = mock.MagicMock()
        resp.read.return_value = b'{"flags": 0}'
        resp.__enter__.return_value = resp
        with mock.patch("urllib.request.urlopen", return_value=resp):
            result = C._case_sp05({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_request_failure_fails(self):
        with mock.patch("urllib.request.urlopen", side_effect=OSError("timed out")):
            result = C._case_sp05({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)


class Sp07Test(unittest.TestCase):
    def test_matching_rate_guard_passes(self):
        srv = FakeSrv(safety_get_rate_guard=lambda: "max_rate_c_per_min: 10.0")
        with mock.patch.object(C, "_http_get_json", return_value=(200, {"max_rate_c_per_min": 10.0})):
            result = C._case_sp07({"srv": srv, "host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_mismatched_rate_guard_fails(self):
        srv = FakeSrv(safety_get_rate_guard=lambda: "max_rate_c_per_min: 10.0")
        with mock.patch.object(C, "_http_get_json", return_value=(200, {"max_rate_c_per_min": 12.0})):
            result = C._case_sp07({"srv": srv, "host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)


if __name__ == "__main__":
    unittest.main()
