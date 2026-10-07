"""Tests for tools/sign_release.py (stdlib unittest; pytest collects it too).

Every key is generated at test time in a temp directory; no private key is ever committed.
"""
import importlib.util
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOL = os.path.normpath(os.path.join(_HERE, "..", "..", "sign_release.py"))
_spec = importlib.util.spec_from_file_location("sign_release", _TOOL)
sr = importlib.util.module_from_spec(_spec)
sys.modules["sign_release"] = sr
_spec.loader.exec_module(sr)

MANIFEST = b'{"schema":1,"tag":"v1.0.0","repo":"budarriola/kilnCtl"}\n'


class SignReleaseTest(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp(prefix="signrel_")
        self.key = os.path.join(self.d, "test_key.pem")
        self.man = os.path.join(self.d, "release.json")
        with open(self.man, "wb") as f:
            f.write(MANIFEST)
        self.pub = sr.generate_key(self.key)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_keygen_refuses_overwrite_and_pub_is_32_bytes(self):
        self.assertEqual(len(self.pub), 32)
        with self.assertRaises(sr.SignError):
            sr.generate_key(self.key)

    def test_sign_writes_64_bytes_that_verify(self):
        out, pub = sr.sign_file(self.man, self.key)
        self.assertEqual(os.path.basename(out), "release.json.sig")
        sig = open(out, "rb").read()
        self.assertEqual(len(sig), 64)
        self.assertEqual(pub, self.pub)
        self.assertTrue(sr.verify_bytes(MANIFEST, sig, self.pub))

    def test_bad_signature_manifest_and_key_fail(self):
        out, _ = sr.sign_file(self.man, self.key)
        sig = bytearray(open(out, "rb").read())
        self.assertFalse(sr.verify_bytes(MANIFEST + b" ", bytes(sig), self.pub), "tampered manifest")
        sig[0] ^= 1
        self.assertFalse(sr.verify_bytes(MANIFEST, bytes(sig), self.pub), "flipped signature bit")
        other = os.path.join(self.d, "other.pem")
        other_pub = sr.generate_key(other)
        good = open(out, "rb").read()
        self.assertFalse(sr.verify_bytes(MANIFEST, good, other_pub), "different key")
        self.assertFalse(sr.verify_bytes(MANIFEST, good[:63], self.pub), "short signature")

    def test_rfc8032_vector_2(self):
        pub = bytes.fromhex("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c")
        sig = bytes.fromhex(
            "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da"
            "085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00")
        self.assertTrue(sr.verify_bytes(b"\x72", sig, pub))
        self.assertFalse(sr.verify_bytes(b"\x73", sig, pub))

    def test_key_path_comes_from_env_only(self):
        self.assertIsNone(sr.key_path_from_env({}))
        self.assertIsNone(sr.key_path_from_env({sr.ENV_KEY: "  "}))
        self.assertEqual(sr.key_path_from_env({sr.ENV_KEY: self.key}), self.key)

    def test_bad_key_files_refused_without_leaking_contents(self):
        junk = os.path.join(self.d, "junk.pem")
        with open(junk, "wb") as f:
            f.write(b"-----BEGIN SECRET-----\nnot a key\n-----END SECRET-----\n")
        with self.assertRaises(sr.SignError) as cm:
            sr.load_private(junk)
        self.assertNotIn("SECRET", str(cm.exception))
        with self.assertRaises(sr.SignError):
            sr.load_private(os.path.join(self.d, "missing.pem"))

    def test_empty_manifest_refused(self):
        empty = os.path.join(self.d, "empty.json")
        open(empty, "wb").close()
        with self.assertRaises(sr.SignError):
            sr.sign_file(empty, self.key)

    def test_cli_sign_verify_round_trip_and_env_key(self):
        env = dict(os.environ)
        env[sr.ENV_KEY] = self.key
        r = subprocess.run([sys.executable, _TOOL, "sign", "--manifest", self.man], env=env,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        sig = os.path.join(self.d, "release.json.sig")
        self.assertTrue(os.path.isfile(sig))
        r = subprocess.run([sys.executable, _TOOL, "verify", "--manifest", self.man, "--sig", sig,
                            "--pub", self.pub.hex()], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        with open(self.man, "ab") as f:
            f.write(b" ")
        r = subprocess.run([sys.executable, _TOOL, "verify", "--manifest", self.man, "--sig", sig,
                            "--pub", self.pub.hex()], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)

    def test_cli_sign_without_any_key_fails_cleanly(self):
        env = {k: v for k, v in os.environ.items() if k != sr.ENV_KEY}
        r = subprocess.run([sys.executable, _TOOL, "sign", "--manifest", self.man], env=env,
                           capture_output=True, text=True)
        self.assertEqual(r.returncode, 2)
        self.assertIn(sr.ENV_KEY, r.stderr)


if __name__ == "__main__":
    unittest.main()
