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


if __name__ == "__main__":
    unittest.main()
