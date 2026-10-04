#!/usr/bin/env python3
"""release_manifest.py -- generate and validate a kilnCtl release manifest.

Stdlib only; runs with tools\\PcTools\\.venv\\Scripts\\python.exe (or any
Python 3.8+).  Design: GITHUB_RELEASE_UPDATE_PLAN.md sections 6-8.

Subcommands
-----------
generate  Copy the images into --out as KilnCtrl-<tag>.bin /
          KilnRecovery-<tag>.bin, then write release.json (schema 1) and
          SHA256SUMS next to them.  Refuses a dirty git tree, a non-semver
          tag, and an application image larger than --max-app-size
          (default 0x400000, the 4 MB `app` partition).
validate  Check release.json + SHA256SUMS against the files in a directory:
          schema, tag shape, commit shape, dirty=false, every image present
          with matching size and sha256, size gate, SHA256SUMS agreement.

Exit 0 on success, 1 on a refusal / validation failure (named on stderr).
"""
import argparse
import datetime
import hashlib
import json
import os
import re
import struct
import subprocess
import sys

SCHEMA = 1
SEMVER_RE = re.compile(r"^v\d+\.\d+\.\d+(-[0-9A-Za-z.]+)?$")
COMMIT_RE = re.compile(r"^[0-9a-f]{40}$")
REPO_RE = re.compile(r"^[A-Za-z0-9._-]{1,39}/[A-Za-z0-9._-]{1,100}$")
DEFAULT_MAX_APP_SIZE = 0x400000  # 4 MB `app` partition (plan: stage is separate)

ZONES_HEADER = os.path.join("firmware", "KilnFW", "App", "drivers", "persist",
                            "zones_config_json.h")
KILNLINK_HEADER = os.path.join("firmware", "CommonFW", "include", "kilnlink",
                               "kilnlink_version.h")
UART_HEADER = os.path.join("firmware", "KilnFW", "App", "drivers", "common",
                           "uart_task_ids.h")
PARTITIONS_CSV = os.path.join("firmware", "KilnFW", "partitions.csv")


class ReleaseError(Exception):
    """A refusal or validation failure; message names the cause."""


# ---------------------------------------------------------------- helpers

def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def check_tag(tag):
    if not isinstance(tag, str) or not SEMVER_RE.match(tag):
        raise ReleaseError("tag %r is not semver (^v\\d+\\.\\d+\\.\\d+(-[0-9A-Za-z.]+)?$)" % (tag,))


def parse_define_int(text, name):
    """Value of `#define NAME <int>` (tolerating casts/parens), or None."""
    m = re.search(r"^[ \t]*#[ \t]*define[ \t]+" + re.escape(name) +
                  r"[ \t]+\(*(?:\(\w+\))?[ \t]*(0[xX][0-9A-Fa-f]+|\d+)[uUlL]*\)*[ \t]*(?:/[/*].*)?$",
                  text, re.MULTILINE)
    if not m:
        return None
    return int(m.group(1), 0)


def _read(root, rel):
    path = os.path.join(root, rel)
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError as e:
        raise ReleaseError("cannot read %s: %s" % (rel, e))


def read_compat(root):
    zones = parse_define_int(_read(root, ZONES_HEADER), "ZONES_CFG_VERSION")
    if zones is None:
        raise ReleaseError("ZONES_CFG_VERSION not found in " + ZONES_HEADER)
    compat = {"zones_cfg_version": zones}
    link = parse_define_int(_read(root, KILNLINK_HEADER), "KILNLINK_PROTOCOL_VERSION")
    if link is not None:
        compat["kilnlink_version"] = link
    uart = parse_define_int(_read(root, UART_HEADER), "UART_PROTOCOL_VERSION")
    if uart is not None:
        compat["uart_version"] = uart
    # sha256 of the CSV exactly as committed, with CRLF normalised so a
    # Windows autocrlf checkout and a Linux checkout agree.
    with open(os.path.join(root, PARTITIONS_CSV), "rb") as f:
        data = f.read().replace(b"\r\n", b"\n")
    compat["partitions_sha256"] = hashlib.sha256(data).hexdigest()
    return compat


def _git(root, *args):
    try:
        r = subprocess.run(["git", "-C", root] + list(args), capture_output=True,
                           text=True, check=False)
    except OSError as e:
        raise ReleaseError("git not runnable: %s" % e)
    if r.returncode != 0:
        raise ReleaseError("git %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def git_commit(root):
    return _git(root, "rev-parse", "HEAD").strip()


def git_dirty_files(root):
    """Porcelain lines; logs/release (this tool's own output) is excluded."""
    out = _git(root, "status", "--porcelain", "--", ".", ":(exclude)logs/release")
    return [ln for ln in out.splitlines() if ln.strip()]


def app_build_date(path):
    """ISO build timestamp from the esp_app_desc_t in an ESP app image, or None."""
    try:
        with open(path, "rb") as f:
            blob = f.read(0x20 + 112)
    except OSError:
        return None
    if len(blob) < 0x20 + 112 or struct.unpack_from("<I", blob, 0x20)[0] != 0xABCD5432:
        return None
    t = blob[0x20 + 80:0x20 + 96].split(b"\0")[0].decode("ascii", "replace").strip()
    d = blob[0x20 + 96:0x20 + 112].split(b"\0")[0].decode("ascii", "replace").strip()
    try:
        return datetime.datetime.strptime(d + " " + t, "%b %d %Y %H:%M:%S").strftime("%Y-%m-%dT%H:%M:%S")
    except ValueError:
        return None


# --------------------------------------------------------------- generate

def generate(root, out_dir, tag, repo, app_bin, recovery_bin=None, elf_zip=None,
             channel="stable", build_date=None, published=None,
             max_app_size=DEFAULT_MAX_APP_SIZE, commit=None, dirty_files=None):
    """Write the release set into out_dir; return the manifest dict.

    commit/dirty_files default to the real git state of `root`; tests inject.
    """
    check_tag(tag)
    if not REPO_RE.match(repo or ""):
        raise ReleaseError("repo %r is not owner/name" % (repo,))
    if channel not in ("stable", "pre"):
        raise ReleaseError("channel must be stable or pre")
    if dirty_files is None:
        dirty_files = git_dirty_files(root)
    if dirty_files:
        raise ReleaseError("working tree is dirty (%d change(s), first: %s); releases are built from a clean tree"
                           % (len(dirty_files), dirty_files[0]))
    if commit is None:
        commit = git_commit(root)
    if not COMMIT_RE.match(commit):
        raise ReleaseError("commit %r is not 40 lowercase hex" % (commit,))
    if not os.path.isfile(app_bin):
        raise ReleaseError("application image not found: %s" % app_bin)
    app_size = os.path.getsize(app_bin)
    if app_size > max_app_size:
        raise ReleaseError("application image is %d bytes, over the %d byte (0x%X) gate"
                           % (app_size, max_app_size, max_app_size))
    if app_size == 0:
        raise ReleaseError("application image is empty")
    if build_date is None:
        build_date = app_build_date(app_bin)
    if build_date is None:
        raise ReleaseError("build date not found in the image's esp_app_desc; pass build_date")
    if published is None:
        published = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")

    os.makedirs(out_dir, exist_ok=True)
    files = []  # (published name, source)

    def stage(src, name):
        dst = os.path.join(out_dir, name)
        if os.path.abspath(src) != os.path.abspath(dst):
            with open(src, "rb") as fi, open(dst, "wb") as fo:
                for chunk in iter(lambda: fi.read(1 << 20), b""):
                    fo.write(chunk)
        files.append(name)
        return dst

    app_name = "KilnCtrl-%s.bin" % tag
    images = [{"name": "app", "file": app_name, "size": app_size,
               "sha256": sha256_file(stage(app_bin, app_name)),
               "includes": ["pico_slotA", "pico_slotB"]}]
    if recovery_bin:
        if not os.path.isfile(recovery_bin):
            raise ReleaseError("recovery image not found: %s" % recovery_bin)
        rec_name = "KilnRecovery-%s.bin" % tag
        dst = stage(recovery_bin, rec_name)
        images.append({"name": "recovery", "file": rec_name, "size": os.path.getsize(dst),
                       "sha256": sha256_file(dst), "apply": "jtag_only"})
    if elf_zip:
        if not os.path.isfile(elf_zip):
            raise ReleaseError("ELF zip not found: %s" % elf_zip)
        stage(elf_zip, "KilnCtrl-%s.elf.zip" % tag)

    manifest = {
        "schema": SCHEMA, "tag": tag, "channel": channel, "published": published,
        "repo": repo, "commit": commit, "dirty": False, "build_date": build_date,
        "compat": read_compat(root), "images": images,
        "notes_url": "https://github.com/%s/releases/tag/%s" % (repo, tag),
    }
    with open(os.path.join(out_dir, "release.json"), "w", encoding="utf-8", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    files.append("release.json")
    with open(os.path.join(out_dir, "SHA256SUMS"), "w", encoding="ascii", newline="\n") as f:
        for name in sorted(files):
            f.write("%s  %s\n" % (sha256_file(os.path.join(out_dir, name)), name))
    return manifest


# --------------------------------------------------------------- validate

def validate(directory, max_app_size=DEFAULT_MAX_APP_SIZE):
    """Return a list of problems (empty means valid)."""
    errs = []
    mpath = os.path.join(directory, "release.json")
    try:
        with open(mpath, "r", encoding="utf-8") as f:
            m = json.load(f)
    except (OSError, ValueError) as e:
        return ["cannot load release.json: %s" % e]
    if not isinstance(m, dict):
        return ["release.json is not an object"]
    if m.get("schema") != SCHEMA:
        errs.append("schema is %r, expected %d" % (m.get("schema"), SCHEMA))
    tag = m.get("tag")
    try:
        check_tag(tag)
    except ReleaseError as e:
        errs.append(str(e))
    if m.get("channel") not in ("stable", "pre"):
        errs.append("channel %r invalid" % (m.get("channel"),))
    if not REPO_RE.match(str(m.get("repo", ""))):
        errs.append("repo %r invalid" % (m.get("repo"),))
    if not COMMIT_RE.match(str(m.get("commit", ""))):
        errs.append("commit %r is not 40 lowercase hex" % (m.get("commit"),))
    if m.get("dirty") is not False:
        errs.append("dirty must be false")
    for k in ("published", "build_date"):
        if not m.get(k):
            errs.append("%s missing" % k)
    compat = m.get("compat")
    if not isinstance(compat, dict):
        errs.append("compat missing")
    else:
        if not isinstance(compat.get("zones_cfg_version"), int):
            errs.append("compat.zones_cfg_version missing or not an int")
        if not re.match(r"^[0-9a-f]{64}$", str(compat.get("partitions_sha256", ""))):
            errs.append("compat.partitions_sha256 missing or malformed")
    images = m.get("images")
    if not isinstance(images, list) or not images:
        errs.append("images missing or empty")
        images = []
    if not any(isinstance(i, dict) and i.get("name") == "app" for i in images):
        errs.append("no image named app")
    listed = set()
    for img in images:
        if not isinstance(img, dict):
            errs.append("image entry is not an object")
            continue
        fname = img.get("file", "")
        if not fname or os.path.basename(fname) != fname:
            errs.append("image file %r invalid" % (fname,))
            continue
        listed.add(fname)
        p = os.path.join(directory, fname)
        if not os.path.isfile(p):
            errs.append("image file missing: %s" % fname)
            continue
        size = os.path.getsize(p)
        if img.get("size") != size:
            errs.append("%s: size %r in manifest, %d on disk" % (fname, img.get("size"), size))
        if img.get("sha256") != sha256_file(p):
            errs.append("%s: sha256 mismatch against manifest" % fname)
        if img.get("name") == "app" and size > max_app_size:
            errs.append("%s: %d bytes exceeds the %d byte gate" % (fname, size, max_app_size))
    sums_path = os.path.join(directory, "SHA256SUMS")
    try:
        with open(sums_path, "r", encoding="ascii") as f:
            lines = [ln for ln in f.read().splitlines() if ln.strip()]
    except OSError as e:
        errs.append("cannot read SHA256SUMS: %s" % e)
        lines = []
    seen = set()
    for ln in lines:
        mm = re.match(r"^([0-9a-f]{64})  (\S.*)$", ln)
        if not mm:
            errs.append("SHA256SUMS: malformed line %r" % ln)
            continue
        digest, name = mm.groups()
        seen.add(name)
        p = os.path.join(directory, name)
        if not os.path.isfile(p):
            errs.append("SHA256SUMS names missing file %s" % name)
        elif sha256_file(p) != digest:
            errs.append("SHA256SUMS: digest mismatch for %s" % name)
    for need in sorted(listed | {"release.json"}):
        if need not in seen:
            errs.append("SHA256SUMS does not cover %s" % need)
    return errs


# -------------------------------------------------------------------- CLI

def _int0(s):
    return int(s, 0)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    g = sub.add_parser("generate")
    g.add_argument("--root", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
    g.add_argument("--out", required=True)
    g.add_argument("--tag", required=True)
    g.add_argument("--repo", default="budarriola/kilnCtl")
    g.add_argument("--channel", default="stable")
    g.add_argument("--app", required=True, help="KilnCtrl.bin")
    g.add_argument("--recovery", help="recovery.bin")
    g.add_argument("--elf-zip", help="zipped KilnCtrl.elf")
    g.add_argument("--build-date")
    g.add_argument("--max-app-size", type=_int0, default=DEFAULT_MAX_APP_SIZE)
    v = sub.add_parser("validate")
    v.add_argument("directory")
    v.add_argument("--max-app-size", type=_int0, default=DEFAULT_MAX_APP_SIZE)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "generate":
            m = generate(os.path.abspath(a.root), a.out, a.tag, a.repo, a.app, a.recovery,
                         a.elf_zip, a.channel, a.build_date, None, a.max_app_size)
            errs = validate(a.out, a.max_app_size)
            if errs:
                raise ReleaseError("generated set failed validation: " + "; ".join(errs))
            print("release %s commit %s: %d image(s) written to %s"
                  % (m["tag"], m["commit"][:12], len(m["images"]), a.out))
        else:
            errs = validate(a.directory, a.max_app_size)
            if errs:
                for e in errs:
                    print("INVALID: " + e, file=sys.stderr)
                return 1
            print("release manifest valid: " + a.directory)
    except ReleaseError as e:
        print("REFUSED: %s" % e, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
