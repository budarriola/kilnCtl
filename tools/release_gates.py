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
  bench-evidence --fw-build B [--logs-dir D] [--max-age-days 7] [--suites ota,lcd,safety]
                               release gate bench-pass-7d: newest qualifying summary.json per suite
                               (full suite case list, all PASS bar designed-INCONCLUSIVE, see EXPECTED_INCONCLUSIVE);
                               exit 1 if any suite has none (names the nearest miss). Read-only.
  notes  --root R --tag T [--out FILE] [--max N]
                               default release body: git log --oneline since the previous
                               semver tag merged into HEAD, or the last N (default 50)
                               commits when there is none.
"""
import argparse
import json
import os
import re
import subprocess
import sys
import time

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


def _load_manifest():
    import importlib.util
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "release_manifest.py")
    spec = importlib.util.spec_from_file_location("release_manifest", path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules.setdefault("release_manifest", mod)
    spec.loader.exec_module(mod)
    return mod


def _compat_at(root, rev, rm):
    def reader(rel):
        r = subprocess.run(["git", "-C", root, "show", "%s:%s" % (rev, rel.replace(os.sep, "/"))],
                           capture_output=True)
        if r.returncode != 0:
            raise rm.ReleaseError("git show %s:%s failed: %s" % (rev, rel, r.stderr.decode("utf-8", "replace").strip()))
        return r.stdout
    return rm.read_compat(root, reader)


def schema_section(root, tag, prev):
    out = ["Schema changes:", ""]
    if not prev:
        return out + ["First release, no diff.", ""]
    rm = _load_manifest()
    try:
        new_rev = tag if _git_ok(root, "rev-parse", "-q", "--verify", "refs/tags/" + tag) else "HEAD"
        old, new = _compat_at(root, prev, rm), _compat_at(root, new_rev, rm)
    except rm.ReleaseError as e:
        return out + ["- cannot compute schema diff versus %s: %s" % (prev, e), ""]
    changed = False
    for key in ("zones_cfg_version", "kilnlink_version", "uart_version"):
        if old[key] != new[key]:
            changed = True
            out.append("- %s: %s -> %s  ROLLBACK HAZARD: older firmware cannot read this" % (key, old[key], new[key]))
    if old["partitions_sha256"] != new["partitions_sha256"]:
        changed = True
        out.append("- partitions_sha256: %s -> %s  requires USB reflash" % (old["partitions_sha256"][:12], new["partitions_sha256"][:12]))
    if not changed:
        out.append("- no schema version or partition table changes versus %s" % prev)
    return out + [""]


def _git_ok(root, *args):
    return subprocess.run(["git", "-C", root] + list(args), capture_output=True).returncode == 0


#: Operator-facing behaviour changes that every generated release body repeats until removed
#: (review 2026-10-10 F6). Not derivable from commits or schema versions.
BEHAVIOUR_NOTES = [
    "Wi-Fi: the station minimum auth mode is WPA2-PSK. A board joined to a WPA/TKIP-only (WPA1) "
    "access point stops associating after this upgrade and falls back to AP mode; reconfigure the "
    "access point to WPA2 or join another network.",
]


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
    out += [""] + schema_section(root, tag, prev)
    out += ["Behaviour changes:", ""] + ["* " + n for n in BEHAVIOUR_NOTES] + [""]
    out += ["See release.json (compat) and docs/RELEASING.md gate 9; SHA256SUMS for hashes.", ""]
    return "\n".join(out)


def _load_summary(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError):
        return None
    return doc if isinstance(doc, dict) else None


#: Per-suite cases whose INCONCLUSIVE verdict is by design and does not disqualify a run.
#: Everything else must PASS: SKIP, NOT_RUN and any other INCONCLUSIVE mean the case was not
#: actually proven. SP-10 (safety) is INCONCLUSIVE by design (acc9944a).
EXPECTED_INCONCLUSIVE = {"safety": frozenset({"SP-10"})}


def _registry_suites():
    """Full case list per suite, from the bench_test registry (the runner's own source of truth)."""
    src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "PcTools", "src")
    if src not in sys.path:
        sys.path.insert(0, src)
    from kilnctrl.bench_test import SUITES
    return {k: list(v) for k, v in SUITES.items()}


def _norm_build(v):
    """Whitespace-collapsed build timestamp (__DATE__ pads single-digit days)."""
    return " ".join(v.split()) if isinstance(v, str) else v


def _evaluate_run(doc, fw_build, max_age_s, now, suite_cases):
    """Return (reasons, counts, age_s). reasons is empty for a qualifying run.

    Judged from per-case verdicts, not exit code: the runner exits 3 for any
    SKIP/INCONCLUSIVE/NOT_RUN, so exit code alone cannot tell a designed INCONCLUSIVE from a gap."""
    reasons = []
    suite = doc.get("suite")
    cases = doc.get("cases") if isinstance(doc.get("cases"), dict) else {}
    counts = {}
    for c in cases.values():
        v = c.get("verdict") if isinstance(c, dict) else None
        counts[str(v)] = counts.get(str(v), 0) + 1
    if doc.get("tainted") is not False:
        reasons.append("tainted")
    if doc.get("preflight_ok") is not True:
        reasons.append("preflight refused")
    if not cases:
        reasons.append("zero cases")
    full = list(suite_cases.get(suite, []))
    if not full:
        reasons.append("unknown suite")
    else:
        requested = doc.get("requested_cases")
        if not isinstance(requested, list) or set(requested) != set(full):
            reasons.append("partial run (requested %s of %d cases)" % (
                len(requested) if isinstance(requested, list) else "?", len(full)))
        missing = [c for c in full if c not in cases]
        if missing:
            reasons.append("cases not run: %s" % ",".join(missing[:5]))
    allowed = EXPECTED_INCONCLUSIVE.get(suite, frozenset())
    bad = []
    for cid, c in cases.items():
        v = c.get("verdict") if isinstance(c, dict) else None
        if v == "PASS" or (v == "INCONCLUSIVE" and cid in allowed):
            continue
        bad.append("%s=%s" % (cid, v))
    if bad:
        reasons.append("not PASS: %s" % ",".join(bad[:5]))
    before = doc.get("board_before") if isinstance(doc.get("board_before"), dict) else {}
    if _norm_build(before.get("esp_fw_build")) != _norm_build(fw_build):
        reasons.append("other build (%s)" % before.get("esp_fw_build"))
    ended = doc.get("ended")
    age = now - ended if isinstance(ended, (int, float)) else None
    if age is None or age > max_age_s:
        reasons.append("stale")
    return reasons, counts, age


def bench_evidence(logs_dir, fw_build, suites, max_age_days, now=None, suite_cases=None):
    """Return (exit_code, lines). One line per suite; nonzero if any suite has no qualifying run."""
    import glob
    now = time.time() if now is None else now
    if suite_cases is None:
        suite_cases = _registry_suites()
    max_age_s = max_age_days * 86400.0
    lines, bad = [], False
    for suite in suites:
        best, near = None, None
        # run dirs are <ts>_<suite> or <ts>_<suite>_<tag>; the summary's own "suite" field is authoritative
        paths = set(glob.glob(os.path.join(logs_dir, "*_%s" % suite, "summary.json")))
        paths |= set(glob.glob(os.path.join(logs_dir, "*_%s_*" % suite, "summary.json")))
        for path in sorted(paths):
            doc = _load_summary(path)
            if doc is None or doc.get("suite") != suite:
                continue
            reasons, counts, age = _evaluate_run(doc, fw_build, max_age_s, now, suite_cases)
            rec = (doc.get("ended") if isinstance(doc.get("ended"), (int, float)) else 0,
                   doc.get("run_id") or os.path.basename(os.path.dirname(path)), counts, age, reasons)
            if not reasons:
                if best is None or rec[0] > best[0]:
                    best = rec
            elif near is None or (len(rec[4]), -rec[0]) < (len(near[4]), -near[0]):
                near = rec
        if best:
            _, rid, counts, age, _ = best
            lines.append("%-8s OK    %s age=%.1fd cases=%s" % (suite, rid, age / 86400.0, _fmt_counts(counts)))
        else:
            bad = True
            if near:
                _, rid, counts, age, reasons = near
                lines.append("%-8s FAIL  nearest miss %s: %s (cases=%s)" % (
                    suite, rid, "; ".join(reasons), _fmt_counts(counts)))
            else:
                lines.append("%-8s FAIL  no runs found for suite under %s" % (suite, logs_dir))
    return (1 if bad else 0), lines


def _fmt_counts(counts):
    return ",".join("%s:%d" % (k, counts[k]) for k in sorted(counts)) or "none"


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
    b = sub.add_parser("bench-evidence")
    b.add_argument("--fw-build")
    b.add_argument("--app-bin", help="derive --fw-build from this KilnCtrl.bin's esp_app_desc_t")
    b.add_argument("--logs-dir", default="logs/bench_test")
    b.add_argument("--max-age-days", type=float, default=7)
    b.add_argument("--suites", default="ota,lcd,safety")
    a = ap.parse_args(argv)
    if a.cmd == "bench-evidence":
        if bool(a.fw_build) == bool(a.app_bin):
            print("bench-evidence: give exactly one of --fw-build / --app-bin")
            return 2
        if a.app_bin:
            src = os.path.join(os.path.dirname(os.path.abspath(__file__)), "PcTools", "src")
            if src not in sys.path:
                sys.path.insert(0, src)
            from kilnctrl.esp_app_desc import parse_app_desc_file
            try:
                a.fw_build = parse_app_desc_file(a.app_bin).build_timestamp
            except Exception as e:
                print("bench-evidence: cannot read build from %s: %s" % (a.app_bin, e))
                return 2
            print("fw_build from %s: %s" % (a.app_bin, a.fw_build))
        code, out = bench_evidence(a.logs_dir, a.fw_build, [s for s in a.suites.split(",") if s], a.max_age_days)
        print(chr(10).join(out))
        return code
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
