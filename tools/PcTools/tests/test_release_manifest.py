"""Tests for tools/release_manifest.py (stdlib unittest; pytest collects it too)."""
import importlib.util
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOL = os.path.normpath(os.path.join(_HERE, "..", "..", "release_manifest.py"))
_spec = importlib.util.spec_from_file_location("release_manifest", _TOOL)
rm = importlib.util.module_from_spec(_spec)
sys.modules["release_manifest"] = rm
_spec.loader.exec_module(rm)

COMMIT = "a" * 40


def make_tree(root):
    """Synthetic source tree holding just the files the tool parses."""
    files = {
        rm.ZONES_HEADER: "/* x */\n#define ZONES_CFG_VERSION 26\n#define ZONES_CFG_VERSION_OLD 3\n",
        rm.KILNLINK_HEADER: "#define KILNLINK_PROTOCOL_VERSION 16\n#define KILNLINK_MIN_COMPATIBLE 7\n",
        rm.UART_HEADER: "#define UART_PROTOCOL_VERSION ((uint16_t)13)\n",
        rm.PARTITIONS_CSV: "# Name, Type, SubType, Offset, Size\napp, app, ota_0, 0x210000, 0x400000\n",
    }
    for rel, text in files.items():
        p = os.path.join(root, rel)
        os.makedirs(os.path.dirname(p), exist_ok=True)
        with open(p, "w", newline="\n") as f:
            f.write(text)


def make_app(path, size=1000, date=b"Oct  4 2026", time=b"12:34:56"):
    blob = bytearray(size)
    blob[0] = 0xE9
    struct.pack_into("<I", blob, 0x20, 0xABCD5432)
    blob[0x20 + 80:0x20 + 80 + len(time)] = time
    blob[0x20 + 96:0x20 + 96 + len(date)] = date
    with open(path, "wb") as f:
        f.write(blob)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="relmanifest_")
        self.addCleanup(shutil.rmtree, self.tmp, True)
        self.root = os.path.join(self.tmp, "src")
        make_tree(self.root)
        self.app = os.path.join(self.tmp, "KilnCtrl.bin")
        make_app(self.app)
        self.rec = os.path.join(self.tmp, "recovery.bin")
        with open(self.rec, "wb") as f:
            f.write(b"R" * 500)
        self.out = os.path.join(self.tmp, "out")

    def gen(self, **kw):
        args = dict(root=self.root, out_dir=self.out, tag="v1.0.0", repo="budarriola/kilnCtl",
                    app_bin=self.app, recovery_bin=self.rec, commit=COMMIT, dirty_files=[])
        args.update(kw)
        return rm.generate(**args)


class ParseTests(unittest.TestCase):
    def test_define_forms(self):
        self.assertEqual(rm.parse_define_int("#define A 26\n", "A"), 26)
        self.assertEqual(rm.parse_define_int("#define A ((uint16_t)13) // c\n", "A"), 13)
        self.assertEqual(rm.parse_define_int("#define A 0x10u\n", "A"), 16)
        self.assertIsNone(rm.parse_define_int("#define AB 5\n", "A"))
        self.assertIsNone(rm.parse_define_int("// #define A 5\n", "A"))

    def test_semver(self):
        for ok in ("v1.0.0", "v10.20.30", "v1.0.0-rc.1", "v0.9.0-beta"):
            rm.check_tag(ok)
        for bad in ("1.0.0", "v1.0", "v1.0.0.0", "v1.0.0-", "v1.0.0 ", "", None, "v1.0.0-a_b"):
            with self.assertRaises(rm.ReleaseError, msg=repr(bad)):
                rm.check_tag(bad)


class GenerateTests(Base):
    def test_round_trip(self):
        m = self.gen()
        self.assertEqual(m["schema"], 1)
        self.assertFalse(m["dirty"])
        self.assertEqual(m["compat"]["zones_cfg_version"], 26)
        self.assertEqual(m["compat"]["kilnlink_version"], 16)
        self.assertEqual(m["compat"]["uart_version"], 13)
        self.assertEqual(m["build_date"], "2026-10-04T12:34:56")
        self.assertEqual([i["name"] for i in m["images"]], ["app", "recovery"])
        self.assertEqual(rm.validate(self.out), [])

    def test_dirty_refused(self):
        with self.assertRaises(rm.ReleaseError):
            self.gen(dirty_files=[" M foo.c"])
        self.assertFalse(os.path.exists(os.path.join(self.out, "release.json")))

    def test_bad_tag_refused(self):
        with self.assertRaises(rm.ReleaseError):
            self.gen(tag="2026.10.04")

    def test_size_gate(self):
        make_app(self.app, size=0x400001)
        with self.assertRaises(rm.ReleaseError):
            self.gen()
        make_app(self.app, size=0x400000)
        self.gen()  # exactly at the gate passes

    def test_partitions_hash_crlf_normalised(self):
        a = self.gen()["compat"]["partitions_sha256"]
        p = os.path.join(self.root, rm.PARTITIONS_CSV)
        with open(p, "rb") as f:
            data = f.read()
        with open(p, "wb") as f:
            f.write(data.replace(b"\n", b"\r\n"))
        self.assertEqual(self.gen()["compat"]["partitions_sha256"], a)

    def test_missing_version_refused(self):
        with open(os.path.join(self.root, rm.ZONES_HEADER), "w") as f:
            f.write("nothing\n")
        with self.assertRaises(rm.ReleaseError):
            self.gen()

    def test_each_compat_version_required(self):
        for rel in (rm.ZONES_HEADER, rm.KILNLINK_HEADER, rm.UART_HEADER):
            with self.subTest(rel=rel):
                path = os.path.join(self.root, rel)
                with open(path, "r") as f:
                    orig = f.read()
                try:
                    with open(path, "w", newline="\n") as f:
                        f.write("/* nothing */\n")
                    with self.assertRaises(rm.ReleaseError):
                        self.gen()
                finally:
                    with open(path, "w", newline="\n") as f:
                        f.write(orig)

    def test_zero_version_refused(self):
        path = os.path.join(self.root, rm.UART_HEADER)
        with open(path, "w", newline="\n") as f:
            f.write("#define UART_PROTOCOL_VERSION ((uint16_t)0)\n")
        with self.assertRaises(rm.ReleaseError):
            self.gen()

    def test_validate_requires_all_versions(self):
        self.gen()
        mp = os.path.join(self.out, "release.json")
        with open(mp) as f:
            m = json.load(f)
        for k in ("zones_cfg_version", "kilnlink_version", "uart_version"):
            bad = json.loads(json.dumps(m))
            del bad["compat"][k]
            with open(mp, "w") as f:
                json.dump(bad, f)
            self.assertTrue(any(k in e for e in rm.validate(self.out)), k)
            bad["compat"][k] = 0
            with open(mp, "w") as f:
                json.dump(bad, f)
            self.assertTrue(any(k in e for e in rm.validate(self.out)), k)

    def test_no_build_date_refused(self):
        with open(self.app, "wb") as f:
            f.write(b"\0" * 400)
        with self.assertRaises(rm.ReleaseError):
            self.gen()
        self.gen(build_date="2026-10-04T00:00:00")


class ValidateTests(Base):
    def setUp(self):
        super().setUp()
        self.gen()

    def _manifest(self):
        with open(os.path.join(self.out, "release.json")) as f:
            return json.load(f)

    def _write(self, m):
        with open(os.path.join(self.out, "release.json"), "w") as f:
            json.dump(m, f)

    def test_tampered_image(self):
        with open(os.path.join(self.out, "KilnCtrl-v1.0.0.bin"), "ab") as f:
            f.write(b"x")
        errs = rm.validate(self.out)
        self.assertTrue(any("size" in e for e in errs))
        self.assertTrue(any("sha256" in e for e in errs))

    def test_same_size_corruption(self):
        p = os.path.join(self.out, "KilnRecovery-v1.0.0.bin")
        with open(p, "wb") as f:
            f.write(b"Q" * 500)
        self.assertTrue(any("sha256" in e for e in rm.validate(self.out)))

    def test_missing_file(self):
        os.remove(os.path.join(self.out, "KilnRecovery-v1.0.0.bin"))
        self.assertTrue(any("missing" in e for e in rm.validate(self.out)))

    def test_dirty_true(self):
        m = self._manifest()
        m["dirty"] = True
        self._write(m)
        self.assertTrue(any("dirty" in e for e in rm.validate(self.out)))

    def test_bad_tag_and_commit(self):
        m = self._manifest()
        m["tag"] = "1.0"
        m["commit"] = "xyz"
        self._write(m)
        errs = rm.validate(self.out)
        self.assertTrue(any("semver" in e for e in errs))
        self.assertTrue(any("commit" in e for e in errs))

    def test_sums_missing_entry(self):
        p = os.path.join(self.out, "SHA256SUMS")
        with open(p) as f:
            lines = [ln for ln in f if "Recovery" not in ln]
        with open(p, "w") as f:
            f.writelines(lines)
        self.assertTrue(any("does not cover" in e for e in rm.validate(self.out)))

    def test_sums_bad_digest(self):
        p = os.path.join(self.out, "SHA256SUMS")
        with open(p) as f:
            text = f.read()
        with open(p, "w") as f:
            f.write("0" * 64 + text[64:])
        self.assertTrue(any("mismatch" in e for e in rm.validate(self.out)))

    def test_size_gate_in_validate(self):
        self.assertTrue(any("gate" in e for e in rm.validate(self.out, max_app_size=10)))

    def test_unreadable(self):
        self.assertTrue(rm.validate(os.path.join(self.tmp, "nope")))


class CliGitTests(Base):
    """End to end through the CLI against a real throwaway git repo."""

    def git(self, *a):
        subprocess.run(["git", "-C", self.root, "-c", "user.name=t", "-c", "user.email=t@t"] + list(a),
                       check=True, capture_output=True)

    def setUp(self):
        super().setUp()
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "init")

    def run_cli(self, *extra):
        return subprocess.run([sys.executable, _TOOL, "generate", "--root", self.root, "--out", self.out,
                               "--tag", "v1.2.3", "--app", self.app, "--recovery", self.rec] + list(extra),
                              capture_output=True, text=True)

    def test_clean_then_dirty(self):
        r = self.run_cli()
        self.assertEqual(r.returncode, 0, r.stderr)
        v = subprocess.run([sys.executable, _TOOL, "validate", self.out], capture_output=True, text=True)
        self.assertEqual(v.returncode, 0, v.stderr)
        with open(os.path.join(self.root, rm.ZONES_HEADER), "a") as f:
            f.write("/* edit */\n")
        r = self.run_cli()
        self.assertEqual(r.returncode, 1)
        self.assertIn("dirty", r.stderr)

    def test_bad_tag_cli(self):
        r = subprocess.run([sys.executable, _TOOL, "generate", "--root", self.root, "--out", self.out,
                            "--tag", "release-1", "--app", self.app], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)


if __name__ == "__main__":
    unittest.main()
