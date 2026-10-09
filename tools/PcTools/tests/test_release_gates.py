"""Tests for tools/release_gates.py (stdlib unittest; pytest collects it too)."""
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.normpath(os.path.join(_HERE, "..", "..", ".."))
_TOOL = os.path.join(_TOOLS, "tools", "release_gates.py") if os.path.basename(_TOOLS) != "tools" \
    else os.path.join(_TOOLS, "release_gates.py")
_spec = importlib.util.spec_from_file_location("release_gates", _TOOL)
rg = importlib.util.module_from_spec(_spec)
sys.modules["release_gates"] = rg
_spec.loader.exec_module(rg)

_REAL_FILE = os.path.normpath(os.path.join(os.path.dirname(_TOOL), "..", "docs", "release_gates.json"))


def gate(gid, status="open", evidence=""):
    return {"id": gid, "title": "t " + gid, "status": status, "evidence": evidence, "source": "s"}


def write_gates(d, gates, schema=1):
    p = os.path.join(d, "g.json")
    with open(p, "w") as f:
        json.dump({"schema": schema, "gates": gates}, f)
    return p


class GatesFile(unittest.TestCase):
    def test_tracked_file_is_valid_and_honest(self):
        gates = rg.load_gates(_REAL_FILE)
        self.assertGreaterEqual(len(gates), 10)
        # the seeded record must not claim the unmet gates are passing
        by_id = {g["id"]: g for g in gates}
        for gid in ("soak-24h", "wp8-tls-fetch", "ota-matrix", "live-github-hop", "full-checks"):
            self.assertEqual(by_id[gid]["status"], "open", gid)

    def test_rejects_bad_files(self):
        with tempfile.TemporaryDirectory() as d:
            for bad in (
                [],                                              # empty list
                [gate("a"), gate("a")],                          # duplicate id
                [gate("A")],                                     # bad id
                [gate("a", status="waived")],                    # unknown status
                [gate("a", status="pass", evidence="  ")],       # pass without evidence
                [dict(gate("a"), title="")],                     # blank title
            ):
                with self.assertRaises(rg.GateError, msg=repr(bad)):
                    rg.load_gates(write_gates(d, bad))
            with self.assertRaises(rg.GateError):
                rg.load_gates(write_gates(d, [gate("a")], schema=2))
            with self.assertRaises(rg.GateError):
                rg.load_gates(os.path.join(d, "missing.json"))


class Check(unittest.TestCase):
    def test_all_pass(self):
        code, msg = rg.check([gate("a", "pass", "e")], "v1.0.0", False)
        self.assertEqual(code, 0)
        self.assertIn("0 OPEN", msg)

    def test_stable_refused_with_open(self):
        code, msg = rg.check([gate("a", "pass", "e"), gate("b")], "v1.0.0", False)
        self.assertEqual(code, 1)
        self.assertIn("- b:", msg)

    def test_stable_allowed_loudly(self):
        code, msg = rg.check([gate("b")], "v1.0.0", True)
        self.assertEqual(code, 0)
        self.assertIn("!!!", msg)
        self.assertIn("- b:", msg)

    def test_prerelease_publishes_but_prints_open(self):
        code, msg = rg.check([gate("b")], "v1.0.0-pre.1", False)
        self.assertEqual(code, 0)
        self.assertIn("- b:", msg)
        self.assertIn("pre-release", msg)

    def test_bad_tag(self):
        with self.assertRaises(rg.GateError):
            rg.check([gate("b")], "1.0", False)
        with self.assertRaises(rg.GateError):
            rg.check([gate("b")], "V1.0.0", False)

    def test_cli_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            p = write_gates(d, [gate("b")])
            run = lambda *a: subprocess.run([sys.executable, _TOOL, *a], capture_output=True, text=True)
            self.assertEqual(run("check", "--file", p, "--tag", "v1.0.0").returncode, 1)
            self.assertEqual(run("check", "--file", p, "--tag", "v1.0.0", "--allow-open").returncode, 0)
            self.assertEqual(run("check", "--file", p, "--tag", "v1.0.0-pre.1").returncode, 0)
            self.assertEqual(run("check", "--file", os.path.join(d, "x.json"), "--tag", "v1.0.0-pre.1").returncode, 1)
            self.assertEqual(run("status", "--file", p, "--tag", "v1.0.0").returncode, 0)


def git(root, *a):
    subprocess.run(["git", "-C", root, "-c", "user.name=t", "-c", "user.email=t@t", *a],
                   check=True, capture_output=True)


class Notes(unittest.TestCase):
    def make_repo(self, d, n):
        git(d, "init", "-q")
        for i in range(n):
            git(d, "commit", "-q", "--allow-empty", "-m", "commit %d" % i)

    def test_no_tag_is_bounded(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_repo(d, 7)
            text = rg.build_notes(d, "v1.0.0", max_commits=3)
            self.assertEqual(text.count("\n- "), 3)
            self.assertIn("no previous semver tag", text)
            self.assertIn("commit 6", text)
            self.assertNotIn("commit 3", text)

    def test_range_from_previous_semver_tag(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_repo(d, 2)
            git(d, "tag", "v1.0.0-pre.1")
            git(d, "commit", "-q", "--allow-empty", "-m", "after pre1")
            git(d, "tag", "v1.0.0-pre.2")
            git(d, "tag", "V1.0_Purchased_This_Board")  # not semver, ignored
            git(d, "tag", "vnext")                       # not semver, ignored
            git(d, "commit", "-q", "--allow-empty", "-m", "after pre2")
            self.assertEqual(rg.previous_tag(d, "v1.0.0"), "v1.0.0-pre.2")
            text = rg.build_notes(d, "v1.0.0")
            self.assertIn("since v1.0.0-pre.2", text)
            self.assertIn("after pre2", text)
            self.assertNotIn("after pre1", text)

    def test_release_sorts_above_prerelease(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_repo(d, 1)
            git(d, "tag", "v1.0.0-pre.9")
            git(d, "tag", "v1.0.0")
            git(d, "tag", "v0.9.0")
            self.assertEqual(rg.previous_tag(d, "v1.0.1"), "v1.0.0")
            self.assertEqual(rg.previous_tag(d, "v1.0.0"), "v1.0.0-pre.9")

    def test_prerelease_numeric_identifiers(self):
        with tempfile.TemporaryDirectory() as d:
            self.make_repo(d, 1)
            git(d, "tag", "v1.0.0-pre.9")
            git(d, "tag", "v1.0.0-pre.10")
            self.assertEqual(rg.previous_tag(d, "v1.0.0"), "v1.0.0-pre.10")


class SchemaDiffNotes(unittest.TestCase):
    Z = os.path.join("firmware", "KilnFW", "App", "drivers", "persist", "zones_config_json.h")
    K = os.path.join("firmware", "CommonFW", "include", "kilnlink", "kilnlink_version.h")
    U = os.path.join("firmware", "KilnFW", "App", "drivers", "common", "uart_task_ids.h")
    P = os.path.join("firmware", "KilnFW", "partitions.csv")

    def write(self, d, zones=22, link=16, uart=13, parts="app,0x10000\n"):
        for rel, txt in ((self.Z, "#define ZONES_CFG_VERSION %d\n" % zones),
                         (self.K, "#define KILNLINK_PROTOCOL_VERSION %d\n" % link),
                         (self.U, "#define UART_PROTOCOL_VERSION %d\n" % uart),
                         (self.P, parts)):
            path = os.path.join(d, rel)
            os.makedirs(os.path.dirname(path), exist_ok=True)
            with open(path, "w", newline="\n") as f:
                f.write(txt)

    def repo(self, d, **second):
        git(d, "init", "-q")
        git(d, "config", "user.email", "t@t")
        git(d, "config", "user.name", "t")
        self.write(d)
        git(d, "add", "-A")
        git(d, "commit", "-qm", "one")
        git(d, "tag", "v1.0.0-pre.1")
        if second is not None:
            self.write(d, **second)
            git(d, "add", "-A")
            git(d, "commit", "-q", "--allow-empty", "-m", "two")
        git(d, "tag", "v1.0.0-pre.2")

    def test_bumped(self):
        with tempfile.TemporaryDirectory() as d:
            self.repo(d, zones=23, parts="app,0x20000\n")
            n = rg.build_notes(d, "v1.0.0-pre.2")
            self.assertIn("Schema changes:", n)
            self.assertIn("zones_cfg_version: 22 -> 23  ROLLBACK HAZARD: older firmware cannot read this", n)
            self.assertIn("requires USB reflash", n)
            self.assertEqual(n.count("ROLLBACK HAZARD"), 1)

    def test_unchanged_has_no_hazard(self):
        with tempfile.TemporaryDirectory() as d:
            self.repo(d)
            n = rg.build_notes(d, "v1.0.0-pre.2")
            self.assertIn("Schema changes:", n)
            self.assertNotIn("ROLLBACK HAZARD", n)
            self.assertNotIn("requires USB reflash", n)

    def test_no_previous_tag(self):
        with tempfile.TemporaryDirectory() as d:
            git(d, "init", "-q")
            git(d, "config", "user.email", "t@t")
            git(d, "config", "user.name", "t")
            self.write(d)
            git(d, "add", "-A")
            git(d, "commit", "-qm", "one")
            git(d, "tag", "v1.0.0-pre.1")
            n = rg.build_notes(d, "v1.0.0-pre.1")
            self.assertIn("irst release, no diff", n)
            self.assertNotIn("ROLLBACK HAZARD", n)


class BenchEvidence(unittest.TestCase):
    NOW = 1_800_000_000.0
    BUILD = "Oct 01 2026 10:00:00"
    SUITES = {"ota": ["OT-1", "OT-2"], "lcd": ["L-1", "L-2"], "safety": ["SP-1", "SP-10"]}

    def _run(self, root, suite, age_days=1, build=None, tainted=False, verdicts=None, requested=None,
             preflight_ok=True, name=None, dirsuffix=None):
        """Writes a summary shaped like the real runner's: exit 3 whenever anything is not PASS."""
        full = self.SUITES[suite]
        if verdicts is None:
            verdicts = {c: ("INCONCLUSIVE" if c == "SP-10" else "PASS") for c in full}
        vs = set(verdicts.values())
        exit_code = 2 if not preflight_ok else 1 if ("FAIL" in vs or tainted) else 3 if vs - {"PASS"} else 0
        d = os.path.join(root, "%s_%s%s" % (name or "20261001T000000Z", suite, dirsuffix or ""))
        os.makedirs(d)
        doc = {"suite": suite, "run_id": os.path.basename(d), "tainted": tainted, "exit_code": exit_code,
               "preflight_ok": preflight_ok,
               "requested_cases": list(full if requested is None else requested),
               "ended": self.NOW - age_days * 86400,
               "board_before": {"esp_fw_build": build or self.BUILD},
               "cases": {c: {"verdict": v} for c, v in verdicts.items()}}
        with open(os.path.join(d, "summary.json"), "w") as f:
            json.dump(doc, f)

    def _go(self, root, suites=("ota", "lcd", "safety")):
        return rg.bench_evidence(root, self.BUILD, list(suites), 7, now=self.NOW, suite_cases=self.SUITES)

    def _all(self, root, skip=None, **bad):
        for s in ("ota", "lcd", "safety"):
            if s != skip:
                self._run(root, s, **(bad if s == "ota" else {}))

    def test_happy_safety_with_designed_inconclusive(self):
        with tempfile.TemporaryDirectory() as d:
            self._all(d)
            code, lines = self._go(d)
            self.assertEqual(code, 0, lines)
            self.assertEqual(len(lines), 3)

    def _expect(self, reason, **bad):
        with tempfile.TemporaryDirectory() as d:
            self._all(d, **bad)
            code, lines = self._go(d)
            self.assertEqual(code, 1)
            self.assertIn(reason, "\n".join(lines))

    def test_stale(self):
        self._expect("stale", age_days=8)

    def test_other_build(self):
        self._expect("other build", build="Sep 01 2026 00:00:00")

    def test_tainted(self):
        self._expect("tainted", tainted=True)

    def test_failed_case(self):
        self._expect("not PASS", verdicts={"OT-1": "PASS", "OT-2": "FAIL"})

    def test_skip_disqualifies(self):
        self._expect("OT-2=SKIP", verdicts={"OT-1": "PASS", "OT-2": "SKIP"})

    def test_undesigned_inconclusive_disqualifies(self):
        self._expect("OT-2=INCONCLUSIVE", verdicts={"OT-1": "PASS", "OT-2": "INCONCLUSIVE"})

    def test_not_run_disqualifies(self):
        with tempfile.TemporaryDirectory() as d:
            self._all(d, skip="safety")
            self._run(d, "safety", verdicts={"SP-1": "NOT_RUN", "SP-10": "INCONCLUSIVE"})
            code, lines = self._go(d)
            self.assertEqual(code, 1)
            self.assertIn("SP-1=NOT_RUN", "\n".join(lines))

    def test_partial_requested_cases(self):
        self._expect("partial run", requested=["OT-1"], verdicts={"OT-1": "PASS"})

    def test_requested_full_but_case_missing(self):
        self._expect("cases not run", verdicts={"OT-1": "PASS"})

    def test_zero_cases(self):
        self._expect("zero cases", verdicts={}, requested=[])

    def test_preflight_refused(self):
        self._expect("preflight refused", preflight_ok=False)

    def test_inconclusive_allowlist_is_per_case(self):
        with tempfile.TemporaryDirectory() as d:
            self._all(d, skip="safety")
            self._run(d, "safety", verdicts={"SP-1": "INCONCLUSIVE", "SP-10": "PASS"})
            code, lines = self._go(d)
            self.assertEqual(code, 1)
            self.assertIn("SP-1=INCONCLUSIVE", "\n".join(lines))

    def test_tagged_run_dir_found(self):
        with tempfile.TemporaryDirectory() as d:
            self._all(d, skip="lcd")
            self._run(d, "lcd", dirsuffix="_post_flash_abc")
            code, lines = self._go(d)
            self.assertEqual(code, 0, lines)

    def test_missing_suite(self):
        with tempfile.TemporaryDirectory() as d:
            self._all(d, skip="lcd")
            code, lines = self._go(d)
            self.assertEqual(code, 1)
            self.assertIn("no runs found", "\n".join(lines))

    def test_malformed_json_skipped(self):
        with tempfile.TemporaryDirectory() as d:
            self._all(d)
            bad = os.path.join(d, "20261002T000000Z_ota")
            os.makedirs(bad)
            with open(os.path.join(bad, "summary.json"), "w") as f:
                f.write("{not json")
            code, _ = self._go(d)
            self.assertEqual(code, 0)

    def test_real_registry_safety_matches_allowlist(self):
        suites = rg._registry_suites()
        self.assertIn("SP-10", suites["safety"])
        self.assertIn("SP-10", rg.EXPECTED_INCONCLUSIVE["safety"])

    def test_cli_main_end_to_end(self):
        import io, contextlib, time
        suites = rg._registry_suites()
        with tempfile.TemporaryDirectory() as d:
            now = time.time()
            for s in ("ota", "lcd"):
                dd = os.path.join(d, "20261001T000000Z_" + s)
                os.makedirs(dd)
                with open(os.path.join(dd, "summary.json"), "w") as f:
                    json.dump({"suite": s, "run_id": "r_" + s, "tainted": False, "exit_code": 0, "ended": now - 3600,
                               "preflight_ok": True, "requested_cases": suites[s],
                               "board_before": {"esp_fw_build": "B1"},
                               "cases": {c: {"verdict": "PASS"} for c in suites[s]}}, f)

            def run(sl):
                buf = io.StringIO()
                with contextlib.redirect_stdout(buf):
                    code = rg.main(["bench-evidence", "--fw-build", "B1", "--logs-dir", d, "--suites", sl])
                return code, buf.getvalue()
            code, out = run("ota,lcd")
            self.assertEqual(code, 0, out)
            self.assertIn("r_ota", out)
            code, out = run("ota,safety")
            self.assertEqual(code, 1)
            self.assertIn("no runs found", out)

    def test_app_bin_and_arg_validation(self):
        import contextlib, io, tempfile
        def run(args):
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                code = rg.main(["bench-evidence"] + args)
            return code, buf.getvalue()
        self.assertEqual(run([])[0], 2)
        self.assertEqual(run(["--fw-build", "B", "--app-bin", "x"])[0], 2)
        with tempfile.TemporaryDirectory() as d:
            bad = os.path.join(d, "KilnCtrl.bin")
            with open(bad, "wb") as f:
                f.write(b"junk")
            code, out = run(["--app-bin", bad, "--logs-dir", d])
            self.assertEqual(code, 2)
            self.assertIn("cannot read build", out)

    def test_build_compare_ignores_date_padding(self):
        self.assertEqual(rg._norm_build("Sep  3 2026 20:13:41"), rg._norm_build("Sep 3 2026 20:13:41"))


if __name__ == "__main__":
    unittest.main()
