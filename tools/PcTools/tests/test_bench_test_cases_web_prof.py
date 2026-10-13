"""Fake-board tests for cases_web_prof (WEB-PROF/STIM/ZONE judges)."""
import json
import os
import unittest

from kilnctrl import bench_test  # noqa: F401 - wires judges
from kilnctrl.bench_test import registry as R
from kilnctrl.bench_test.registry import CaseResult, Verdict

HTML_OK = ("favToggleBtn /api/profile/favorite addSegBtn saveBtn kcConfirm /api/profile/delete "
           "modeExportBtn /api/profile/import /api/profile/import validateImportJson "
           "modeDeleteBtn bulkActionBtn bulkCancelBtn bulkDelete selectionCheckbox "
           "ioTargetOptionsHtml relay_zone_owned_mask")


class Web:
    def __init__(self, html):
        self.html = html

    def goto(self, path):
        return self.html


class FakeBoard:
    """Stateful profile board speaking the ctx seams."""

    def __init__(self, user=(), builtin=(128, 129), executor="idle",
                 leave_on_delete=False):
        self.profiles = {i: {"name": f"U{i}", "segments": [], "zone_mask": 1} for i in user}
        self.builtin = list(builtin)
        self.executor = executor
        self.leave_on_delete = leave_on_delete
        self.favs = set()
        self.hidden = set()
        self.writes = []
        self.deleted = []

    def _list(self):
        out = [{"id": i, "name": p["name"], "builtin": False} for i, p in sorted(self.profiles.items())]
        out += [{"id": b, "name": f"B{b}", "builtin": True} for b in self.builtin if b not in self.hidden]
        return out

    def get(self, path):
        if path == "/api/profile_exec":
            return 200, {"state": self.executor}
        if path == "/api/autotune":
            return 200, {"state": "idle"}
        if path == "/api/profiles":
            return 200, self._list()
        if path == "/api/profiles/favorites":
            return 200, {"ids": sorted(self.favs)}
        if path.startswith("/api/profiles/builtin"):
            return 200, [{"id": b, "hidden": b in self.hidden} for b in self.builtin]
        if path.startswith("/api/profile?id="):
            n = int(path.split("=")[1])
            if n not in self.profiles:
                return 404, None
            p = self.profiles[n]
            return 200, {"id": n, "name": p["name"], "zone_mask": p["zone_mask"],
                         "segment_count": len(p["segments"]), "segments": p["segments"]}
        if path.startswith("/api/profile/export?id="):
            n = int(path.split("=")[1])
            if n not in self.profiles:
                return 404, None
            p = self.profiles[n]
            return 200, {"kind": "kilnctl_profile", "version": 2, "name": p["name"],
                         "zone_mask": p["zone_mask"], "segments": p["segments"]}
        return 404, None

    def _store(self, n, name, zm, segs):
        self.profiles[n] = {"name": name, "zone_mask": zm, "segments": segs}

    def post(self, path, f):
        self.writes.append((path, dict(f)))
        if path == "/api/profile":
            n = int(f["id"])
            if n < 0:
                free = [i for i in range(100) if i not in self.profiles]
                if not free:
                    return 400, {"ok": False}
                n = free[0]
            segs = [{"seg_kind": int(f[f"seg{i}_kind"]), "target_c": float(f[f"seg{i}_target"]),
                     "ramp_c_per_hr": float(f[f"seg{i}_ramp"]), "dwell_min": float(f[f"seg{i}_dwell"])}
                    for i in range(int(f["seg_count"]))]
            self._store(n, f["name"], int(f["zone_mask"]), segs)
            return 200, {"ok": True, "id": n}
        if path == "/api/profile/favorite":
            (self.favs.add if f["favorite"] == "1" else self.favs.discard)(int(f["id"]))
            return 200, {"ok": True, "id": int(f["id"]), "favorite": f["favorite"] == "1"}
        if path == "/api/profile/builtin/hide":
            (self.hidden.add if f["hidden"] == "1" else self.hidden.discard)(int(f["id"]))
            return 200, {"ok": True, "hidden": f["hidden"] == "1"}
        if path == "/api/profile/builtin/restore":
            self.hidden.clear()
            return 200, {"ok": True}
        return 404, None

    def post_raw(self, path, f):
        if path == "/api/profile/delete":
            n = int(f["id"])
            if n in self.builtin:
                return 400, "no"
            self.deleted.append(n)
            if not self.leave_on_delete:
                self.profiles.pop(n, None)
            return 200, "ok"
        s, b = self.post(path, f)
        return s, json.dumps(b)

    def post_body(self, path, body):
        n = [i for i in range(100) if i not in self.profiles][0]
        self._store(n, body["name"], body["zone_mask"], body["segments"])
        return 200, json.dumps({"ok": True, "id": n})

    def ctx(self):
        return {"suite": "web", "http_get_json": self.get, "http_post_json": self.post, "http_post_raw": self.post_raw,
                "http_post_json_body": self.post_body, "web_client": Web(HTML_OK)}


def run(cid, ctx):
    return R.get_case(cid).judge(ctx)


class ProfTransient(unittest.TestCase):
    def test_pass_and_leaves_nothing(self):
        for cid in ("WEB-PROF-03", "WEB-PROF-04", "WEB-PROF-05", "WEB-PROF-06", "WEB-PROF-07"):
            b = FakeBoard(user=(0, 1, 5))
            r = run(cid, b.ctx())
            self.assertEqual(r.verdict, Verdict.PASS, (cid, r.reason))
            self.assertEqual(sorted(b.profiles), [0, 1, 5], cid)

    def test_picks_lowest_free_slot(self):
        b = FakeBoard(user=(0, 1, 5))
        r = run("WEB-PROF-03", b.ctx())
        self.assertEqual(r.observed["slot"], 2)

    def test_no_free_slot_inconclusive_and_no_write(self):
        b = FakeBoard(user=range(100))
        r = run("WEB-PROF-03", b.ctx())
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(b.writes, [])

    def test_executor_running_skips_without_write(self):
        b = FakeBoard(executor="running")
        r = run("WEB-PROF-04", b.ctx())
        self.assertEqual(r.verdict, Verdict.SKIP)
        self.assertEqual(b.writes, [])

    def test_leftover_after_delete_is_error_fail(self):
        b = FakeBoard(user=(0,), leave_on_delete=True)
        r = run("WEB-PROF-03", b.ctx())
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertTrue(r.reason.startswith("ERROR:"), r.reason)

    def test_never_deletes_preexisting_slot(self):
        b = FakeBoard(user=(0, 1))
        run("WEB-PROF-05", b.ctx())
        self.assertNotIn(0, b.deleted)
        self.assertNotIn(1, b.deleted)
        self.assertIn(0, b.profiles)

    def test_overwrite_of_occupied_slot_fails(self):
        b = FakeBoard(user=(0, 1))
        orig_post = b.post

        def bad(path, f):  # board ignores id=-1 and clobbers slot 0
            if path == "/api/profile" and f["id"] == "-1":
                f = dict(f, id="0")
            return orig_post(path, f)
        c = b.ctx()
        c["http_post_json"] = bad
        r = run("WEB-PROF-03", c)
        self.assertEqual(r.verdict, Verdict.FAIL)


    def test_overwritten_slot_is_never_deleted(self):
        b = FakeBoard(user=(0, 1))
        orig_post = b.post

        def bad(path, f):
            if path == "/api/profile" and f["id"] == "-1":
                f = dict(f, id="0")
            return orig_post(path, f)
        c = b.ctx()
        c["http_post_json"] = bad
        r = run("WEB-PROF-05", c)
        self.assertEqual(r.verdict, Verdict.FAIL)
        self.assertNotIn(0, b.deleted)
        self.assertIn(0, b.profiles)


class ProfOther(unittest.TestCase):
    def test_prof02_pass_and_restores(self):
        b = FakeBoard()
        r = run("WEB-PROF-02", b.ctx())
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(b.favs, set())

    def test_prof08_pass_and_restores(self):
        b = FakeBoard()
        r = run("WEB-PROF-08", b.ctx())
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)
        self.assertEqual(b.hidden, set())

    def test_prof09_fail_when_html_missing(self):
        b = FakeBoard()
        c = b.ctx()
        c["web_client"] = Web("<html></html>")
        self.assertEqual(run("WEB-PROF-09", c).verdict, Verdict.FAIL)
        self.assertEqual(run("WEB-PROF-09", b.ctx()).verdict, Verdict.PASS)

    def test_prof11_skip_without_creds(self):
        os.environ.pop("KILNCTL_WEB_USER_USERNAME", None)
        self.assertEqual(run("WEB-PROF-11", {}).verdict, Verdict.SKIP)


def zones_body(**kw):
    z = {"generation": 4, "thermo_count": 1, "relay_count": 4, "relay_names": list("abcd"),
         "relay_zone_owned_mask": 1, "relay_types": [0, 0, 0, 0],
         "timing_profiles": [{"index": 0}, {"index": 1}],
         "safety_ceiling": {"pico_known": True, "target_c": 1300, "pico_current_c": 1300},
         "zones": [{"index": 0, "relay_mask": 1, "timing_profile": 1, "zone_type": 0, "failsafe_state": 0,
                    "tc_type": 1, "relay_type": 0, "pid_kp": 1.5, "pid_ki": 0.1, "pid_kd": 0.0,
                    "settings_source_groups": {g: 255 for g in ("limits", "relaytiming", "control", "guards", "tc")}}]}
    z.update(kw)
    return z


class ZoneTests(unittest.TestCase):
    def ctx(self, z, html="tprofile _timingprofile profileOptionsHtml tcTypeSelectHtml( failsafestate"):
        return {"suite": "web", "http_get_json": lambda p: (200, z) if p == "/api/zones" else (200, {"state": "idle"}),
                "web_client": Web(html)}

    def test_stim02(self):
        self.assertEqual(run("WEB-STIM-02", self.ctx(zones_body())).verdict, Verdict.PASS)
        bad = zones_body()
        bad["zones"][0]["timing_profile"] = 9
        self.assertEqual(run("WEB-STIM-02", self.ctx(bad)).verdict, Verdict.FAIL)

    def test_zone11_exact_key_paths(self):
        z = zones_body()
        z["zones"][0]["tuning_valid"] = True
        z["zones"][0]["tuning_method"] = "x"
        z["zones"][0]["tuning_settled"] = True
        html = "tuningQuality firingStatsCurrent firingStatsHistory"

        def mk(pe, fh):
            def g(p):
                if p == "/api/zones":
                    return 200, z
                if p == "/api/profile_exec":
                    return 200, pe
                return 200, fh
            return {"http_get_json": g, "web_client": Web(html),
                    "_results": {"HP-01": CaseResult(Verdict.PASS)}}
        good_pe = {"zones": [{"firing_stats": {"sample_count": 5}}]}
        good_fh = {"records": [{"zones": [{"firing_stats": {"sample_count": 7}}]}]}
        zero = {"zones": [{"firing_stats": {"sample_count": 0}}]}
        self.assertEqual(run("WEB-ZONE-11", mk(good_pe, {"records": []})).verdict, Verdict.PASS)
        self.assertEqual(run("WEB-ZONE-11", mk(zero, good_fh)).verdict, Verdict.PASS)
        # a sample_count NOT at zones[].firing_stats must not count
        self.assertEqual(run("WEB-ZONE-11", mk({"x": {"firing_stats": {"sample_count": 9}}}, {})).verdict, Verdict.FAIL)

    def test_zone11_tuning_field_names(self):
        html = "tuningQuality firingStatsCurrent firingStatsHistory"
        pe = {"zones": [{"firing_stats": {"sample_count": 5}}]}

        def ctx(zz):
            z = zones_body()
            z["zones"][0].update(zz)
            return {"http_get_json": lambda p: (200, z) if p == "/api/zones" else (200, pe if p == "/api/profile_exec" else {}),
                    "web_client": Web(html), "_results": {"HP-01": CaseResult(Verdict.PASS)}}
        # healthy board after HP-01: real firmware keys
        self.assertEqual(run("WEB-ZONE-11", ctx({"tuning_valid": True, "tuning_method": 1, "tuning_settled": True})).verdict, Verdict.PASS)
        # old invented names are not accepted; the real ones are required
        self.assertEqual(run("WEB-ZONE-11", ctx({"tuning_valid": True, "method": "x", "settled": True})).verdict, Verdict.FAIL)
        self.assertEqual(run("WEB-ZONE-11", ctx({"tuning_valid": True, "tuning_method": 1})).verdict, Verdict.FAIL)

    def test_zone06_relay_states_are_active(self):
        html = "atStartBtn atAbortBtn atAcceptBtn atAckUnsettled /api/autotune/start /api/autotune/abort /api/autotune/accept"

        def ctx(state):
            return {"http_get_json": lambda p: (200, {"state": state}), "web_client": Web(html),
                    "_results": {"AT-02": CaseResult(Verdict.PASS)}}
        for ok in ("idle", "done", "aborted"):
            self.assertEqual(run("WEB-ZONE-06", ctx(ok)).verdict, Verdict.PASS)
        for active in ("relay_approach", "relay_cycling", "settling", "stepping"):
            self.assertEqual(run("WEB-ZONE-06", ctx(active)).verdict, Verdict.FAIL)

    def test_zone06_at01_relay_states_are_active(self):
        html = "atStartBtn atAbortBtn atAcceptBtn atAckUnsettled /api/autotune/start /api/autotune/abort /api/autotune/accept"

        def ctx(state):
            return {"http_get_json": lambda p: (200, {"state": state}), "web_client": Web(html),
                    "_results": {"AT-01": CaseResult(Verdict.PASS)}}
        self.assertEqual(run("WEB-ZONE-06", ctx("done")).verdict, Verdict.PASS)
        for active in ("relay_approach", "relay_cycling"):
            self.assertEqual(run("WEB-ZONE-06", ctx(active)).verdict, Verdict.FAIL)

    def test_zone03_fewer_relays_ok_and_short_zones_fail(self):
        z = zones_body(relay_count=2)
        self.assertEqual(run("WEB-ZONE-03", self.ctx(z)).verdict, Verdict.PASS)
        z = zones_body(relay_count=5)
        self.assertEqual(run("WEB-ZONE-03", self.ctx(z)).verdict, Verdict.FAIL)
        z = zones_body()
        z["zones"] = []
        self.assertEqual(run("WEB-ZONE-03", self.ctx(z)).verdict, Verdict.FAIL)

    def test_zone03(self):
        self.assertEqual(run("WEB-ZONE-03", self.ctx(zones_body())).verdict, Verdict.PASS)
        bad = zones_body()
        bad["zones"][0]["zone_type"] = 7
        self.assertEqual(run("WEB-ZONE-03", self.ctx(bad)).verdict, Verdict.FAIL)
        # firmware emits failsafe_state as a JSON bool (zones_http_get.c)
        b = zones_body()
        b["zones"][0]["failsafe_state"] = False
        self.assertEqual(run("WEB-ZONE-03", self.ctx(b)).verdict, Verdict.PASS)
        b["zones"][0]["failsafe_state"] = None
        self.assertEqual(run("WEB-ZONE-03", self.ctx(b)).verdict, Verdict.FAIL)

    def test_zone02_pass_identity(self):
        z = zones_body()
        state = {"gen": 4}
        c = self.ctx(z)

        def get(p):
            if p == "/api/zones":
                return 200, dict(z, generation=state["gen"])
            return 200, {"state": "idle"}

        def post_raw(p, f):
            raise AssertionError("WEB-ZONE-02 must never POST /api/zones (review 4 H2)")
        c["http_get_json"] = get
        c["http_post_raw"] = post_raw
        c["http_post_json"] = post_raw
        r = run("WEB-ZONE-02", c)
        self.assertEqual(r.verdict, Verdict.PASS, r.reason)

    def test_zone05_detects_changed_gain(self):
        z = zones_body()
        st = {"kp": 1.5}

        def get(p):
            if p == "/api/zones":
                zz = zones_body()
                zz["zones"][0]["pid_kp"] = st["kp"]
                return 200, zz
            return 200, {"state": "idle"}

        def post(p, f):
            st["kp"] = 9.0  # board corrupts the gain
            return 200, {"ok": True}
        c = self.ctx(z)
        c["http_get_json"], c["http_post_json"] = get, post
        self.assertEqual(run("WEB-ZONE-05", c).verdict, Verdict.FAIL)

    def test_zone13_inconclusive_and_fail(self):
        base = {"mask": 1, "committed_mask": 1, "k_mask": 1, "zone": [0, 1, 2],
                "committed_zone": [0, 1, 2], "k": [1, 1, 1]}

        def g(b):
            return {"http_get_json": lambda p: (200, b)}
        self.assertEqual(run("WEB-ZONE-13", g(base)).verdict, Verdict.PASS)
        self.assertEqual(run("WEB-ZONE-13", g(dict(base, committed_mask=0))).verdict, Verdict.INCONCLUSIVE)
        self.assertEqual(run("WEB-ZONE-13", g(dict(base, committed_zone=[2, 1, 2]))).verdict, Verdict.FAIL)

    def test_observers_not_run_without_host(self):
        c = self.ctx(zones_body())
        c["_results"] = {}
        self.assertEqual(run("WEB-ZONE-08", c).verdict, Verdict.NOT_RUN)
        self.assertEqual(run("WEB-ZONE-11", c).verdict, Verdict.NOT_RUN)

    def test_zone12_missing_key_fails(self):
        keys = ("coupling_tau_c0", "coupling_tau_c1", "coupling_tau_c2", "coupling_dead_time_c0",
                "coupling_dead_time_c1", "coupling_dead_time_c2", "model_fit_temp_c", "model_fit_ambient_c")
        d = {"ok": True, "generation": 4,
             "zones": [dict({"index": i}, **{k: 1.0 for k in keys}) for i in range(3)]}
        c = {"http_get_json": lambda p: (200, d) if "diag" in p else (404, None)}
        self.assertEqual(run("WEB-ZONE-12", c).verdict, Verdict.PASS)
        del d["zones"][1]["model_fit_temp_c"]
        self.assertEqual(run("WEB-ZONE-12", c).verdict, Verdict.FAIL)


class AuditTwoProfTests(unittest.TestCase):
    """Web judge contract audit 2: L2, L7, L8, L9, M1 wrap."""

    def _z09_with_autotune(self, state):
        c = self._zone09(False)
        base = c["http_get_json"]
        c["http_get_json"] = lambda p: (200, {"state": state}) if p == "/api/autotune" else base(p)
        return c

    def test_l2_done_autotune_does_not_block_mutating_gate(self):
        for st in ("done", "aborted"):
            r = run("WEB-ZONE-09", self._z09_with_autotune(st))
            self.assertNotEqual(r.verdict, Verdict.SKIP, r.reason)

    def test_l2_running_autotune_still_blocks(self):
        self.assertEqual(run("WEB-ZONE-09", self._z09_with_autotune("relay_cycling")).verdict, Verdict.SKIP)

    def test_l7_live_blocks_do_not_fail_zone05(self):
        n = {"i": 0}

        def get(p):
            if p == "/api/zones":
                n["i"] += 1
                zz = zones_body()
                zz["safety_ceiling"]["pico_current_c"] = 1300 + n["i"]
                return 200, zz
            return 200, {"state": "idle"}
        c = {"suite": "web", "http_get_json": get, "http_post_json": lambda p, f: (200, {"ok": True}),
             "web_client": Web("")}
        r = run("WEB-ZONE-05", c)
        self.assertNotEqual(r.verdict, Verdict.FAIL, r.reason)

    def _zone09(self, warn):
        st = {"en": False}
        n = {"posts": 0}

        def get(p):
            if p == "/api/adaptive_tune":
                return 200, {"zones": [{"zone": 0, "enabled": st["en"]}]}
            if p == "/api/zones":
                return 200, zones_body()
            return 200, {"state": "idle"}

        def post(p, f):
            st["en"] = f["enabled"] == "1"
            n["posts"] += 1
            return 200, ({"ok": True, "warning": "applied live, save failed"} if warn and n["posts"] == 1 else {"ok": True})
        return {"suite": "web", "http_get_json": get, "http_post_json": post}

    def test_l8_persist_warning_fails_zone09(self):
        self.assertEqual(run("WEB-ZONE-09", self._zone09(True)).verdict, Verdict.FAIL)
        self.assertEqual(run("WEB-ZONE-09", self._zone09(False)).verdict, Verdict.PASS)

    def test_l9_sweep_started_between_reads_is_not_aborted(self):
        reads = {"n": 0}
        posted = []

        def get(p):
            if p == "/api/zones/current_sweep/status":
                reads["n"] += 1
                return 200, {"state": "idle" if reads["n"] == 1 else "running"}
            return 200, {"state": "idle"}
        c = {"suite": "web", "http_get_json": get, "http_post_json": lambda p, f: posted.append(p) or (200, {"ok": True}),
             "web_client": Web("sweepStartBtn sweepAbortBtn")}
        r = run("WEB-ZONE-10", c)
        self.assertEqual(r.verdict, Verdict.INCONCLUSIVE, r.reason)
        self.assertEqual(posted, [])


if __name__ == "__main__":
    unittest.main()
