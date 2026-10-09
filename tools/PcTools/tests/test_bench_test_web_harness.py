#!/usr/bin/env python3
"""Tests for the WEB-judge harness prerequisites
(docs/BENCH_TEST_WEB_JUDGES_PLAN.md section 3): window probes, ctx["_results"],
the new HTTP seams, the Wi-Fi/credential deny-list, FL-01/FL-07 stashes and
the _NIGHTLY_ORDER moves. Fake board only.

Run with: python -m pytest tools/PcTools/tests/test_bench_test_web_harness.py -q
"""
from __future__ import annotations

import dataclasses
import os
import sys
import unittest
from unittest import mock

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "src"))
sys.path.insert(0, os.path.dirname(__file__))

from kilnctrl.bench_test import cases_web_rw as C  # noqa: E402
from kilnctrl.bench_test import registry as R  # noqa: E402
from kilnctrl.bench_test import windows as W  # noqa: E402
from kilnctrl.bench_test.runner import BenchTestRunner  # noqa: E402
import test_bench_test_runner as TR  # noqa: E402


class WindowsTest(unittest.TestCase):
    def _ctx(self, probes):
        ctx = {}
        W.register_probes(ctx, list(probes), lambda cid: probes[cid])
        return ctx

    def _spec(self, window, fn):
        return R.CaseSpec(id="X", area="WEB", description="d", window_probe=(window, fn))

    def test_once_window_fires_once_and_stores_result(self):
        calls = []
        ctx = self._ctx({"A": self._spec("hp01_running", lambda c: calls.append(1) or {"v": len(calls)})})
        W.fire_window(ctx, "hp01_running", once=True)
        W.fire_window(ctx, "hp01_running", once=True)
        self.assertEqual(len(calls), 1)
        self.assertEqual(ctx["_probe_results"]["hp01_running"]["A"], {"v": 1})

    def test_append_window_collects_samples(self):
        n = []
        ctx = self._ctx({"A": self._spec("hp01_tick", lambda c: n.append(1) or {"n": len(n)})})
        for _ in range(3):
            W.fire_window(ctx, "hp01_tick")
        self.assertEqual(ctx["_probe_results"]["hp01_tick"]["A"], [{"n": 1}, {"n": 2}, {"n": 3}])

    def test_raising_probe_stored_as_error_and_does_not_raise(self):
        def boom(c):
            raise ValueError("x")
        ctx = self._ctx({"A": self._spec("otb01_tripped", boom)})
        W.fire_window(ctx, "otb01_tripped", once=True)
        self.assertIn("error", ctx["_probe_results"]["otb01_tripped"]["A"])

    def test_unfired_window_key_absent(self):
        ctx = self._ctx({"A": self._spec("otb01_tripped", lambda c: {})})
        self.assertNotIn("otb01_tripped", ctx["_probe_results"])

    def test_fire_without_registration_is_harmless(self):
        ctx = {}
        W.fire_window(ctx, "hp01_tick")
        self.assertIn("hp01_tick", ctx["_probe_results"])


class RunnerWiringTest(unittest.TestCase):
    setUp = TR.RunnerLifecycleTest.setUp
    tearDown = TR.RunnerLifecycleTest.tearDown
    _patch_judge = TR.RunnerLifecycleTest._patch_judge

    def test_results_alias_and_probe_registration(self):
        seen = {}

        def judge(ctx):
            seen["results_is_run_results"] = ctx["_results"] is ctx["_run_results"]
            seen["probes"] = dict(ctx["_window_probes"])
            return R.CaseResult(R.Verdict.PASS)
        self._patch_judge("ST-05", judge)
        spec = R.REGISTRY["ST-05"]
        R.REGISTRY["ST-05"] = dataclasses.replace(spec, window_probe=("hp01_running", lambda c: {}))
        BenchTestRunner(self.ctx, logs_root=self.tmpdir).run(suite="smoke", cases=["ST-05"])
        self.assertTrue(seen["results_is_run_results"])
        self.assertEqual([c for c, _ in seen["probes"]["hp01_running"]], ["ST-05"])


class SeamsTest(unittest.TestCase):
    def test_get_text_uses_fake(self):
        ctx = {"http_get_text": lambda p: (200, "a,b\n1,2\n") if p == "/api/history.csv" else (404, None)}
        self.assertEqual(C._get_text(ctx, "/api/history.csv"), (200, "a,b\n1,2\n"))

    def test_post_raw_uses_fake(self):
        got = []
        ctx = {"http_post_raw": lambda p, f: got.append((p, f)) or (200, "ok")}
        self.assertEqual(C._post_raw(ctx, "/api/profile/save", {"id": "1"}), (200, "ok"))
        self.assertEqual(got, [("/api/profile/save", {"id": "1"})])

    def test_post_json_body_uses_fake(self):
        got = []
        ctx = {"http_post_json_body": lambda p, b: got.append((p, b)) or (200, '{"ok":true}')}
        self.assertEqual(C._post_json_body(ctx, "/api/profile/import", {"name": "x"})[0], 200)
        self.assertEqual(got[0], ("/api/profile/import", {"name": "x"}))

    def test_no_host_returns_none(self):
        self.assertEqual(C._get_text({}, "/x"), (None, None))
        self.assertEqual(C._post_raw({}, "/x", {}), (None, None))
        self.assertEqual(C._post_json_body({}, "/x", {}), (None, None))


class DenyListTest(unittest.TestCase):
    PATHS = ["/api/wifi/provision", "/api/wifi/forget", "/api/network/ip_config", "/provision?x=1"]

    def _all_seams(self, ctx, path, payload):
        return [
            lambda: C._post_json(ctx, path, payload),
            lambda: C._post_raw(ctx, path, payload),
            lambda: C._post_json_body(ctx, path, payload),
        ]

    def test_denied_paths_refused_and_fake_never_called(self):
        called = []
        ctx = {"http_post_json": lambda *a: called.append(a),
               "http_post_raw": lambda *a: called.append(a),
               "http_post_json_body": lambda *a: called.append(a)}
        for path in self.PATHS:
            for call in self._all_seams(ctx, path, {"ssid": "x"}):
                with self.assertRaises(C.ForbiddenWrite):
                    call()
        self.assertEqual(called, [])

    def test_clear_credentials_refused(self):
        ctx = {"http_post_json": lambda *a: (200, {"ok": True})}
        with self.assertRaises(C.ForbiddenWrite):
            C._post_json(ctx, "/api/auth/security", {"cmd": "clear_credentials"})

    def test_low_level_authed_post_also_refuses(self):
        with self.assertRaises(C.ForbiddenWrite):
            C._http_post_raw_authed("h", "/api/wifi/provision", {})

    def test_allowed_write_passes(self):
        ctx = {"http_post_json": lambda p, f: (200, {"ok": True})}
        self.assertEqual(C._post_json(ctx, "/api/unit_pref", {"unit": "F"}), (200, {"ok": True}))
        self.assertEqual(C._post_json(ctx, "/api/auth/security", {"cmd": "set_policy"})[0], 200)

    def test_negative_denylist_emptied_means_write_goes_through(self):
        # Proves the tests above are not vacuous: with the list emptied the
        # refusal disappears.
        empty = {"path_suffixes": (), "fields": ()}
        ctx = {"http_post_json": lambda p, f: (200, {"ok": True})}
        with mock.patch.object(C, "_WIFI_WRITE_DENYLIST", empty):
            self.assertEqual(C._post_json(ctx, "/api/wifi/provision", {})[0], 200)
            self.assertEqual(C._post_json(ctx, "/x", {"cmd": "clear_credentials"})[0], 200)


class SmokeStashTest(unittest.TestCase):
    def test_fl01_stashes_report(self):
        from kilnctrl.bench_test import cases_smoke as S

        class Srv:
            def debug_check_partition_table(self, host=None):
                return "partition report"
        ctx = {"srv": Srv(), "host": "h"}
        with mock.patch.object(S.J, "judge_partition_table_match",
                               return_value=R.CaseResult(R.Verdict.PASS)):
            S._case_fl01(ctx)
        self.assertEqual(ctx["_fl01_partitions"], "partition report")

    def test_fl07_stashes_cfgfs(self):
        from kilnctrl import dashboard_http_client, partition_http_client
        from kilnctrl.bench_test import cases_smoke as S
        data = {"mounted": True, "files": 7}
        ctx = {"host": "h", "partitions_csv_path": os.devnull}
        with mock.patch.object(partition_http_client, "get_partitions", return_value=[]), \
                mock.patch.object(dashboard_http_client, "get_cfgfs_status", return_value=data), \
                mock.patch.object(dashboard_http_client, "get_cfgfs_format_pending", return_value={}), \
                mock.patch.object(S.J, "judge_cfgfs_state", return_value=R.CaseResult(R.Verdict.PASS)):
            S._case_fl07(ctx)
        self.assertEqual(ctx["_fl07_cfgfs"], data)


class OrderTest(unittest.TestCase):
    def setUp(self):
        self.order = R.SUITES["nightly"]

    def idx(self, cid):
        return self.order.index(cid)

    def test_observers_follow_hosts(self):
        for obs, host in (("WEB-DASH-03", "HP-01"), ("WEB-DASH-09", "HP-01"),
                          ("WEB-OTA-02", "HP-01"), ("WEB-DASH-06", "HP-04")):
            self.assertGreater(self.idx(obs), self.idx(host), obs)
            self.assertEqual(R.get_case(obs).depends_on, host)

    def test_dash09_between_hp01_and_hp02(self):
        self.assertGreater(self.idx("WEB-DASH-09"), self.idx("HP-01"))
        self.assertLess(self.idx("WEB-DASH-09"), self.idx("HP-02"))

    def test_auth_on_group(self):
        s3 = self.idx("WEB-SEC-03")
        self.assertEqual(self.idx("WEB-LOG-02"), s3 + 1)
        self.assertEqual(self.idx("WEB-X-02"), s3 + 2)
        self.assertLess(self.idx("WEB-X-02"), self.idx("OT-B01"))

    def test_log03_alias_depends_on_sec05_and_orders_after(self):
        self.assertEqual(R.get_case("WEB-LOG-03").depends_on, "WEB-SEC-05")
        full = R.SUITES["full"]
        self.assertGreater(full.index("WEB-LOG-03"), full.index("WEB-SEC-05"))
        self.assertEqual(full[-2:], ["WEB-SEC-05", "WEB-LOG-03"])


if __name__ == "__main__":
    unittest.main()
