#!/usr/bin/env python3
"""check_cfgfs_never_gates_nvs.py -- docs/RELEASE_HARDENING_PLAN.md section 10
("the `cfg` partition is inert and should either be finished or explicitly
parked", decided PARKED 2026-09-17, see that section and
docs/CONFIG_FILESYSTEM.md).

WHY THIS EXISTS. The `cfg` LittleFS partition is unformatted/unmounted on
every board actually running today (the bench board's live firmware is
1057+ commits behind the code that even attempts the mount -- see the plan
section for how that was established). NVS stays authoritative and
unconditional specifically BECAUSE every dual-write call site treats the
`cfg` write as best-effort: it calls the domain's `*_cfg_fs_save()` bridge
function, discards or only logs its result, and performs the NVS write
regardless of whether the file write succeeded. That is what makes the
partition safe to leave inert -- if any call site were ever rewritten to
skip or condition the NVS write on the file write's result, the
authoritative copy would silently stop being written on every board where
`cfg` cannot mount, which is every board today.

This is the mechanical guard the plan section calls for: "a producer whose
output nothing ever reads" (or, here, whose *failure* nothing is allowed to
act on) is a documented recurring defect class in this codebase. This check
does not (and cannot, statically) prove NVS is written -- it proves the much
narrower, checkable half of the contract: that the return value of every
production `*_cfg_fs_save(` call is never used as an `if`/ternary condition
in the same function, i.e. nothing in the call's own function body branches
on it. That is the shape every existing call site already follows (see
zones_config_store.c's `nvs_save()`, kiln_cfg_store.c, profiles_http.c,
relay_cycles.c, unit_pref.c, display_power_cfg.c, ramp_assist_cfg.c,
time_sync.c, adaptive_tune.c, profile_executor_firing_stats.c -- 11
production call sites as of 2026-09-17).

SCOPE: firmware/KilnFW/App, excluding the cfg_fs bridge modules themselves
(*_cfg_fs.c, cfg_fs*.c -- their whole job is to report success/failure, that
is not the hazard) and excluding firmware/KilnFW/App/test/.

A call site is a "hit" (recorded regardless of pass/fail) when a line
matches ``<ident> = <name>_cfg_fs_save(`` or a bare ``<name>_cfg_fs_save(``
call. For a captured-variable hit, the check scans forward from that line to
the end of the enclosing function (approximated: the next line whose only
content is a closing brace in column 0) looking for an ``if (...)`` whose
condition references the captured variable. Every existing call site follows
the same shape -- log a warning inside that `if` and fall through, NVS write
unconditional afterward -- so a bare "is it referenced in a condition" test
produces false positives on that legitimate pattern. What actually matters is
whether the `if`'s own block EXITS the function (``return``) instead of
falling through to the NVS write below it: only that shape can make the NVS
write conditional on the file write's result, so that is what this check
flags. The block is scanned by brace depth from the `if (` line, not by
indentation, so it is robust to reformatting.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_ROOT = REPO_ROOT / "firmware" / "KilnFW" / "App"

CALL_RE = re.compile(r"^\s*(?:(?:esp_err_t|int)\s+)?(?:(\w+)\s*=\s*)?(\w+_cfg_fs_save)\s*\(")
FUNC_END_RE = re.compile(r"^\}\s*$")
IF_RE_TMPL = r"if\s*\([^)]*\b{}\b"


def is_exempt(path: Path) -> bool:
    name = path.name
    if name.endswith("_cfg_fs.c") or name.startswith("cfg_fs"):
        return True
    if "\\test\\" in str(path) or "/test/" in str(path):
        return True
    return False


def main() -> int:
    violations = []
    hits = 0
    for path in sorted(SCAN_ROOT.rglob("*.c")):
        if is_exempt(path):
            continue
        try:
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
        except OSError as exc:
            print(f"WARN: could not read {path}: {exc}", file=sys.stderr)
            continue
        for i, line in enumerate(lines):
            m = CALL_RE.match(line)
            if not m:
                continue
            hits += 1
            var = m.group(1)
            if not var:
                continue  # bare call / (void)-discarded -- nothing to gate on
            if_re = re.compile(IF_RE_TMPL.format(re.escape(var)))
            for j in range(i + 1, len(lines)):
                if FUNC_END_RE.match(lines[j]):
                    break
                stripped = lines[j]
                if not if_re.search(stripped):
                    continue
                # Found the `if` referencing our variable. Walk its block by
                # brace depth (starting from this line, which may or may not
                # already contain the opening `{`) and flag it only if the
                # block contains a `return` -- that is the only shape that
                # can skip the NVS write expected to follow.
                depth = stripped.count("{") - stripped.count("}")
                block_lines = [stripped]
                k = j
                if depth <= 0 and "{" not in stripped:
                    # Brace-less single-statement if -- next non-blank line
                    # is the whole "block".
                    k = j + 1
                    while k < len(lines) and not lines[k].strip():
                        k += 1
                    if k < len(lines):
                        block_lines.append(lines[k])
                else:
                    while depth > 0 and k + 1 < len(lines):
                        k += 1
                        block_lines.append(lines[k])
                        depth += lines[k].count("{") - lines[k].count("}")
                block_text = "\n".join(block_lines)
                if re.search(r"\breturn\b", block_text):
                    violations.append(
                        f"{path.relative_to(REPO_ROOT)}:{i + 1}: `{var}` (result of {m.group(2)}()) "
                        f"gates a `return` at line {j + 1}: {stripped.strip()!r} -- "
                        f"the cfg_fs write result must never gate the NVS write."
                    )
                break

    if hits == 0:
        print("check_cfgfs_never_gates_nvs: FAIL -- found zero *_cfg_fs_save( call sites under "
              f"{SCAN_ROOT.relative_to(REPO_ROOT)}. The scan itself is broken (path moved, rename, "
              "or the whole dual-write feature was removed) -- a check that silently stops finding "
              "its own subject is worse than no check.", file=sys.stderr)
        return 2

    print(f"check_cfgfs_never_gates_nvs: scanned {hits} *_cfg_fs_save( call site(s).")
    if violations:
        print("check_cfgfs_never_gates_nvs: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        return 1

    print("check_cfgfs_never_gates_nvs: PASS -- no call site branches on the cfg_fs write result.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
