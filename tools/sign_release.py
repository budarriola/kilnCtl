#!/usr/bin/env python3
"""Ed25519 signing of release.json (docs/GITHUB_RELEASE_UPDATE_PLAN.md WP11).

  sign_release.py keygen  --out KEY.pem        generate a keypair OFFLINE; prints the public key
  sign_release.py pubkey  --key KEY.pem        print the public key (hex + C array)
  sign_release.py sign    --manifest release.json [--key KEY.pem] [--out release.json.sig]
  sign_release.py verify  --manifest release.json --sig release.json.sig (--pub HEX | --key KEY.pem)

The signature is the raw 64-byte RFC 8032 Ed25519 signature over the EXACT bytes of release.json
(published as the `release.json.sig` asset). The private key is never read from the repo: `sign`
takes --key or, when omitted, the path in the environment variable KILNCTL_RELEASE_SIGNING_KEY.
The private key file is a PKCS#8 PEM; `keygen` refuses to overwrite an existing file and writes it
without a passphrase, so keep it on offline or encrypted storage (docs/RELEASING.md).
Needs the `cryptography` package (already in the PcTools venv).
"""
import argparse
import os
import sys

ENV_KEY = "KILNCTL_RELEASE_SIGNING_KEY"
SIG_NAME = "release.json.sig"


class SignError(Exception):
    pass


def _crypto():
    try:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric import ed25519
        from cryptography.exceptions import InvalidSignature
    except ImportError as e:  # pragma: no cover
        raise SignError("the 'cryptography' package is required: %s" % e)
    return serialization, ed25519, InvalidSignature


def generate_key(path):
    """Write a new PKCS#8 PEM private key to `path` (never overwrites); return the public key bytes."""
    serialization, ed25519, _ = _crypto()
    if os.path.exists(path):
        raise SignError("%s already exists; refusing to overwrite a key" % path)
    key = ed25519.Ed25519PrivateKey.generate()
    pem = key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                            serialization.NoEncryption())
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, "wb") as f:
        f.write(pem)
    return public_bytes(key)


def public_bytes(private_key):
    serialization, _, _ = _crypto()
    return private_key.public_key().public_bytes(serialization.Encoding.Raw, serialization.PublicFormat.Raw)


def load_private(path):
    serialization, ed25519, _ = _crypto()
    try:
        with open(path, "rb") as f:
            key = serialization.load_pem_private_key(f.read(), password=None)
    except (OSError, ValueError, TypeError) as e:
        raise SignError("cannot load signing key %s: %s" % (path, type(e).__name__))
    if not isinstance(key, ed25519.Ed25519PrivateKey):
        raise SignError("%s is not an Ed25519 private key" % path)
    return key


def key_path_from_env(environ=None):
    """The signing key path from KILNCTL_RELEASE_SIGNING_KEY, or None when unset/empty."""
    v = (environ if environ is not None else os.environ).get(ENV_KEY, "").strip()
    return v or None


def sign_bytes(data, key):
    sig = key.sign(data)
    assert len(sig) == 64
    return sig


def sign_file(manifest_path, key_path, out_path=None):
    """Sign `manifest_path`'s bytes; write the 64-byte signature next to it; return (out_path, pub)."""
    key = load_private(key_path)
    with open(manifest_path, "rb") as f:
        data = f.read()
    if not data:
        raise SignError("%s is empty" % manifest_path)
    sig = sign_bytes(data, key)
    out_path = out_path or os.path.join(os.path.dirname(os.path.abspath(manifest_path)), SIG_NAME)
    with open(out_path, "wb") as f:
        f.write(sig)
    # Self-check: what we wrote must verify under the matching public key.
    if not verify_bytes(data, sig, public_bytes(key)):
        raise SignError("internal error: fresh signature does not verify")
    return out_path, public_bytes(key)


def verify_bytes(data, sig, pub):
    _, ed25519, InvalidSignature = _crypto()
    if len(sig) != 64 or len(pub) != 32:
        return False
    try:
        ed25519.Ed25519PublicKey.from_public_bytes(pub).verify(sig, data)
        return True
    except InvalidSignature:
        return False


def c_array(pub):
    return "{ " + ", ".join("0x%02x" % b for b in pub) + " },"


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    k = sub.add_parser("keygen")
    k.add_argument("--out", required=True)
    p = sub.add_parser("pubkey")
    p.add_argument("--key")
    s = sub.add_parser("sign")
    s.add_argument("--manifest", required=True)
    s.add_argument("--key")
    s.add_argument("--out")
    v = sub.add_parser("verify")
    v.add_argument("--manifest", required=True)
    v.add_argument("--sig", required=True)
    v.add_argument("--pub")
    v.add_argument("--key")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "keygen":
            pub = generate_key(a.out)
            print("private key written to %s (keep it OFFLINE, never commit it)" % a.out)
            print("public key hex: %s" % pub.hex())
            print("update_signing_keys.c entry: %s" % c_array(pub))
        elif a.cmd == "pubkey":
            kp = a.key or key_path_from_env()
            if not kp:
                raise SignError("no --key and %s is not set" % ENV_KEY)
            pub = public_bytes(load_private(kp))
            print("public key hex: %s" % pub.hex())
            print("update_signing_keys.c entry: %s" % c_array(pub))
        elif a.cmd == "sign":
            kp = a.key or key_path_from_env()
            if not kp:
                raise SignError("no --key and %s is not set" % ENV_KEY)
            out, pub = sign_file(a.manifest, kp, a.out)
            print("signed %s -> %s (public key %s)" % (a.manifest, out, pub.hex()))
        elif a.cmd == "verify":
            if a.pub:
                pub = bytes.fromhex(a.pub)
            elif a.key:
                pub = public_bytes(load_private(a.key))
            else:
                raise SignError("verify needs --pub or --key")
            with open(a.manifest, "rb") as f:
                data = f.read()
            with open(a.sig, "rb") as f:
                sig = f.read()
            if not verify_bytes(data, sig, pub):
                print("INVALID signature", file=sys.stderr)
                return 1
            print("signature OK")
    except (SignError, OSError, ValueError) as e:
        print("sign_release: %s" % e, file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
