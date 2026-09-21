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

    def test_real_route_shape_passes(self):
        """GET /api/saftyfw_stack_margin's real field names
        (stack_total_words/high_water_words/measured), sample shape from
        logs/bench_test/20260921T004457Z_smoke/pico_stack_margin/*.json --
        a healthy board."""
        body = {
            "tasks": [
                {"name": "relay_owner", "task_id": 0, "measured": True, "stack_total_words": 256, "high_water_words": 214},
                {"name": "link_task", "task_id": 5, "measured": True, "stack_total_words": 2560, "high_water_words": 1478},
                {"name": "watchdog_task", "task_id": 8, "measured": True, "stack_total_words": 256, "high_water_words": 208},
            ]
        }
        with mock.patch.object(C, "_http_get_json", return_value=(200, body)):
            result = C._case_sk03({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)


class Sp01Test(unittest.TestCase):
    def test_params_list_commissioning_passes(self):
        """Sample shape from logs/bench_test/20260921T004457Z_smoke's SP-01
        observed payload: abs_max_temp_c lives in params:[{name,set,value}],
        not as a top-level field."""
        commissioning = {
            "commissioned": True,
            "stale": False,
            "link_up": True,
            "params": [
                {"id": 259, "name": "tc_placement_mode", "set": True, "type": "u8", "value": 0},
                {"id": 260, "name": "abs_max_temp_c", "set": True, "type": "f32", "value": 80},
            ],
        }
        with mock.patch("kilnctrl.safety_cfg_http_client.get_commissioning", return_value=commissioning):
            with mock.patch("kilnctrl.zones_http_client.get_zones", return_value={}):
                result = C._case_sp01({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS, result.reason)

    def test_query_failure_fails(self):
        with mock.patch("kilnctrl.safety_cfg_http_client.get_commissioning", side_effect=OSError("unreachable")):
            result = C._case_sp01({"host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)


class Sp05Test(unittest.TestCase):
    """SP-05 must be read-only: srv.safety_get_status()'s cached text, never
    a POST to /api/estop/verify (that route is an admin write). The
    negative test below asserts the read-only contract directly."""

    def test_not_asserted_passes(self):
        srv = FakeSrv(safety_get_status=lambda: "link up | 22.50 C (CJ 23.10 C) | currents 0.00 A | 1 s old")
        result = C._case_sp05({"srv": srv, "host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.PASS)

    def test_asserted_fails(self):
        srv = FakeSrv(safety_get_status=lambda: "link up; E-stop asserted | 22.50 C (CJ 23.10 C) | currents 0.00 A | 1 s old")
        result = C._case_sp05({"srv": srv, "host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.FAIL)

    def test_serial_hub_unavailable_is_inconclusive(self):
        srv = FakeSrv(safety_get_status=lambda: "error: no serial port open - connect first")
        result = C._case_sp05({"srv": srv, "host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_link_down_is_inconclusive(self):
        srv = FakeSrv(safety_get_status=lambda: "no flags set | ... | never received | tx_dropped unknown")
        result = C._case_sp05({"srv": srv, "host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_timeout_error_is_inconclusive_not_a_bare_fail(self):
        """Opus review finding (LOW): safety_get_status() can raise a bare
        TimeoutError (link_hub.py:573) rather than returning an "error: ..."
        string, when the hub/link is unavailable -- must be caught and
        turned into INCONCLUSIVE, not left to propagate into a bare
        FAIL/exception with no safety-relevant information."""
        def _raise():
            raise TimeoutError("hub request timed out")
        srv = FakeSrv(safety_get_status=_raise)
        result = C._case_sp05({"srv": srv, "host": "1.2.3.4"})
        self.assertEqual(result.verdict, Verdict.INCONCLUSIVE)

    def test_never_calls_urllib_post(self):
        """Negative test: SP-05 is a read-only case and must never issue an
        HTTP POST (to /api/estop/verify or anywhere else)."""
        srv = FakeSrv(safety_get_status=lambda: "link up | ...")
        with mock.patch("urllib.request.urlopen") as urlopen_mock:
            result = C._case_sp05({"srv": srv, "host": "1.2.3.4"})
        urlopen_mock.assert_not_called()
        self.assertEqual(result.verdict, Verdict.PASS)


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


class RepoRootTest(unittest.TestCase):
    def test_resolved_root_contains_firmware_regardless_of_cwd(self):
        """cases_smoke.py used to fall back to ctx.get("repo_root", ".")
        -- a cwd-relative path nothing populated -- when FL-05/FL-07 built
        firmware-tree paths. _repo_root() must resolve to the real repo
        root (derived from __file__, same pattern as
        report.default_logs_root()) no matter what directory the test
        runner's cwd happens to be."""
        root = C._repo_root()
        self.assertTrue(
            os.path.isdir(os.path.join(root, "firmware")),
            f"expected {root} to contain a firmware/ directory",
        )
        cwd = os.getcwd()
        try:
            os.chdir(os.path.dirname(root))
            self.assertEqual(C._repo_root(), root)
            self.assertTrue(os.path.isdir(os.path.join(C._repo_root(), "firmware")))
        finally:
            os.chdir(cwd)


if __name__ == "__main__":
    unittest.main()
