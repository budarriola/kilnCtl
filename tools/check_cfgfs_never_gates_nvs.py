#!/usr/bin/env python3
"""check_cfgfs_never_gates_nvs.py -- INVERTED 2026-10-06 (owner decision
2026-10-05, docs/CONFIG_FILESYSTEM.md section "NVS dual-write close").

HISTORY. This check used to prove the opposite contract: while the `cfg`
partition was unproven, NVS was authoritative and every `*_cfg_fs_save()` call
was best-effort, so the check failed any call site whose `if` on the file
write's result returned early (skipping the NVS write below it).

NOW. Config saves go to the `cfg` file ONLY; NVS is a legacy, read-only source
that an upgrading board migrates from. The file write is therefore the one and
only persistence, and its result is load-bearing: a call site that discards it
(bare call, or `(void)` cast) would report a save as done when nothing was
written, and would advance no rev, with no log. This check fails any production
`*_cfg_fs_save(` / `*_cfg_fs_delete(` call whose result is discarded, and any
captured result that the rest of the function never reads (assigned and
forgotten). A call whose result is passed on (returned, or tested) passes.

Deliberately narrow, like its predecessor: it does not prove the NVS write is
gone (see check_cfgfs_no_nvs_config_writes if added later); it proves the
checkable half, that nobody ignores the one write that now matters.

SCOPE: firmware/KilnFW/App, excluding the cfg_fs bridge modules themselves
(*_cfg_fs.c, cfg_fs*.c) and firmware/KilnFW/App/test/. Comment lines are skipped.
"""
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
SCAN_ROOT = REPO_ROOT / "firmware" / "KilnFW" / "App"

CALL_RE = re.compile(
    r"^\s*(?P<void>\(void\)\s*)?(?:(?:esp_err_t|int)\s+)?(?:(?P<var>\w+)\s*=\s*)?(?:return\s+)?"
    r"(?P<fn>\w+_cfg_fs_(?:save|delete))\s*\("
)
RETURN_RE = re.compile(r"^\s*return\s+\w+_cfg_fs_(?:save|delete)\s*\(")
FUNC_END_RE = re.compile(r"^\}\s*$")


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
            stripped = line.strip()
            if stripped.startswith("*") or stripped.startswith("//") or stripped.startswith("/*"):
                continue
            m = CALL_RE.match(line)
            if not m:
                continue
            hits += 1
            where = f"{path.relative_to(REPO_ROOT)}:{i + 1}"
            if RETURN_RE.match(line):
                continue  # result handed straight to the caller
            var = m.group("var")
            if m.group("void") or not var:
                violations.append(f"{where}: result of {m.group('fn')}() is discarded -- the cfg file "
                                  f"write is the only persistence now and its failure must be handled.")
                continue
            use_re = re.compile(r"\b%s\b" % re.escape(var))
            used = False
            for j in range(i + 1, len(lines)):
                if FUNC_END_RE.match(lines[j]):
                    break
                if use_re.search(lines[j]):
                    used = True
                    break
            if not used:
                violations.append(f"{where}: `{var}` (result of {m.group('fn')}()) is assigned and never read.")

    if hits == 0:
        print("check_cfgfs_never_gates_nvs: FAIL -- found zero *_cfg_fs_save/_delete( call sites under "
              f"{SCAN_ROOT.relative_to(REPO_ROOT)}. The scan itself is broken (path moved, rename, "
              "or the feature was removed) -- a check that silently stops finding its own subject is "
              "worse than no check.", file=sys.stderr)
        return 2

    print(f"check_cfgfs_never_gates_nvs: scanned {hits} *_cfg_fs_save/_delete( call site(s).")
    if violations:
        print("check_cfgfs_never_gates_nvs: FAIL", file=sys.stderr)
        for v in violations:
            print(f"  {v}", file=sys.stderr)
        return 1

    print("check_cfgfs_never_gates_nvs: PASS -- every cfg file write result is consumed.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
