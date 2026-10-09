#!/usr/bin/env python3
"""release_gates.py -- release gate record and release-notes body for tools/make_release.ps1.

Gate record: docs/release_gates.json (tracked, hand-edited). Schema 1:

  {"schema": 1,
   "gates": [{"id": "soak-24h", "title": "...", "status": "open"|"pass",
              "evidence": "...", "source": "doc/section that defines it"}]}

Rules: ids are unique lowercase [a-z0-9-]; status is exactly "open" or "pass"; a "pass"
gate must carry non-empty evidence (a log path, commit, or date + what was observed).
There is no "waived": an owner waiver is recorded by editing the gate to "pass" with the
waiver as its evidence, so the git history shows who decided and when.

Commands (stdlib only):
  status --file F [--tag T]    print every gate; exit 0 (1 if the file is invalid)
  check  --file F --tag T [--allow-open]
                               print open gates; exit 1 if the tag is a stable tag
                               (no -suffix) with open gates and --allow-open is absent.
                               A pre-release tag (v1.0.0-pre.1) never refuses.
  notes  --root R --tag T [--out FILE] [--max N]
                               default release body: git log --oneline since the previous
                               semver tag merged into HEAD, or the last N (default 50)
                               commits when there is none.
"""
import argparse
import json
import re
import subprocess
import sys

SEMVER_RE = re.compile(r"^v(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.]+))?$")
ID_RE = re.compile(r"^[a-z0-9][a-z0-9-]*$")
STATUSES = ("open", "pass")


class GateError(Exception):
    pass


def load_gates(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise GateError("cannot read gates file %s: %s" % (path, e))
    if not isinstance(doc, dict) or doc.get("schema") != 1:
        raise GateError("gates file: schema must be 1")
    gates = doc.get("gates")
    if not isinstance(gates, list) or not gates:
        raise GateError("gates file: 'gates' must be a non-empty list")
    seen = set()
    for i, g in enumerate(gates):
        if not isinstance(g, dict):
            raise GateError("gate #%d is not an object" % i)
        gid = g.get("id")
        if not isinstance(gid, str) or not ID_RE.match(gid):
            raise GateError("gate #%d: bad id %r" % (i, gid))
        if gid in seen:
            raise GateError("duplicate gate id %s" % gid)
        seen.add(gid)
        for k in ("title", "source"):
            if not isinstance(g.get(k), str) or not g[k].strip():
                raise GateError("gate %s: %s must be a non-empty string" % (gid, k))
        if g.get("status") not in STATUSES:
            raise GateError("gate %s: status must be one of %s, got %r" % (gid, STATUSES, g.get("status")))
        ev = g.get("evidence")
        if not isinstance(ev, str):
            raise GateError("gate %s: evidence must be a string" % gid)
        if g["status"] == "pass" and not ev.strip():
            raise GateError("gate %s: a pass gate needs non-empty evidence" % gid)
    return gates


def is_prerelease(tag):
    m = SEMVER_RE.match(tag or "")
    if not m:
        raise GateError("tag %r is not semver" % (tag,))
    return m.group(4) is not None


def open_gates(gates):
    return [g for g in gates if g["status"] != "pass"]


def format_status(gates):
    lines = []
    for g in gates:
        lines.append("  [%-4s] %-22s %s" % (g["status"].upper(), g["id"], g["title"]))
        if g["evidence"].strip():
            lines.append("           evidence: %s" % g["evidence"].strip())
    return "\n".join(lines)


def check(gates, tag, allow_open):
    """Return (exit_code, message). Never refuses a pre-release tag."""
    pre = is_prerelease(tag)
    op = open_gates(gates)
    head = "Release gates: %d of %d pass, %d OPEN." % (len(gates) - len(op), len(gates), len(op))
    if not op:
        return 0, head
    listing = "OPEN GATES:\n" + "\n".join("  - %s: %s" % (g["id"], g["title"]) for g in op)
    if pre:
        return 0, "%s\n%s\n(pre-release tag %s: publishing with open gates is allowed.)" % (head, listing, tag)
    if allow_open:
        return 0, "%s\n!!! STABLE TAG %s WITH %d OPEN GATE(S) (-AllowOpenGates) !!!\n%s" % (
            head, tag, len(op), listing)
    return 1, ("%s\n%s\nstable tag %s cannot publish with open gates; close them in "
               "docs/release_gates.json or pass -AllowOpenGates." % (head, listing, tag))


def _git(root, *args):
    r = subprocess.run(["git", "-C", root] + list(args), capture_output=True, text=True)
    if r.returncode != 0:
        raise GateError("git %s failed: %s" % (" ".join(args), r.stderr.strip()))
    return r.stdout


def _semver_key(m):
    """SemVer 2.0 precedence: a release sorts above its pre-releases; pre-release identifiers
    compare numerically when numeric (pre.10 > pre.9), numeric below alphanumeric."""
    pre = m.group(4)
    ids = tuple((0, int(p), "") if p.isdigit() else (1, 0, p) for p in pre.split(".")) if pre else ()
    return (int(m.group(1)), int(m.group(2)), int(m.group(3)), pre is None, ids)


def previous_tag(root, tag):
    """Highest semver tag merged into HEAD, other than `tag` itself; None if there is none."""
    best, best_key = None, None
    for t in _git(root, "tag", "--merged", "HEAD", "--list", "v*").split():
        m = SEMVER_RE.match(t)
        if not m or t == tag:
            continue
        key = _semver_key(m)
        if best_key is None or key > best_key:
            best, best_key = t, key
    return best


def build_notes(root, tag, max_commits=50):
    prev = previous_tag(root, tag)
    if prev:
        log = _git(root, "log", "--oneline", "%s..HEAD" % prev)
        rng = "since %s" % prev
    else:
        log = _git(root, "log", "--oneline", "-n", str(max_commits), "HEAD")
        rng = "last %d commits (no previous semver tag)" % max_commits
    lines = [l for l in log.splitlines() if l.strip()]
    out = ["kilnCtl %s" % tag, "", "Changes %s (%d):" % (rng, len(lines)), ""]
    out += ["- " + l for l in lines] or ["- (none)"]
    out += ["", "Schema versions are in release.json (compat); rollback hazards versus the "
                "previous release: docs/RELEASING.md gate 9. See SHA256SUMS for hashes.", ""]
    return "\n".join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("status")
    s.add_argument("--file", required=True)
    s.add_argument("--tag")
    c = sub.add_parser("check")
    c.add_argument("--file", required=True)
    c.add_argument("--tag", required=True)
    c.add_argument("--allow-open", action="store_true")
    n = sub.add_parser("notes")
    n.add_argument("--root", required=True)
    n.add_argument("--tag", required=True)
    n.add_argument("--out")
    n.add_argument("--max", type=int, default=50)
    a = ap.parse_args(argv)
    try:
        if a.cmd == "status":
            gates = load_gates(a.file)
            print(format_status(gates))
            if a.tag:
                print(check(gates, a.tag, True)[1])
            else:
                print("Release gates: %d of %d pass." % (len(gates) - len(open_gates(gates)), len(gates)))
            return 0
        if a.cmd == "check":
            gates = load_gates(a.file)
            print(format_status(gates))
            code, msg = check(gates, a.tag, a.allow_open)
            print(msg)
            return code
        if a.cmd == "notes":
            text = build_notes(a.root, a.tag, a.max)
            if a.out:
                with open(a.out, "w", encoding="utf-8", newline="\n") as f:
                    f.write(text)
            else:
                sys.stdout.write(text)
            return 0
    except GateError as e:
        print("release_gates: %s" % e, file=sys.stderr)
        return 1
    return 1


if __name__ == "__main__":
    sys.exit(main())
