#!/usr/bin/env python3
"""stub_signature_drift_check.py -- App/test/stubs/*.h hand-writes stand-ins
for real ESP-IDF headers (driver/ledc.h, nvs.h, ...) so the host build can
compile firmware .c files without ESP-IDF installed (see stubs/nvs.h's own
header comment, and stubs/driver/ledc.h's: "only has to exist and parse").
Nothing has ever checked that a stub's function prototypes still match the
real header it stands in for -- a stale stub (wrong argument count, a
renamed real function) can silently pass host tests that never call the
mismatched function, and has been the root cause of a "concurrent agent stub
collision" before (see MEMORY: project_concurrent_agent_stub_collision.md).

This compares NAME + ARITY (argument count) only, not full type equality --
matching the ROADMAP.md B8 item's own scope call ("full type-compare is
over-engineering; name+arity catches the real drift class"): a function
whose signature drifted enough to matter either gained/lost a parameter, or
was renamed/removed, both of which name+arity catches; a parameter that
merely changed type (e.g. int -> int32_t) with the same count almost never
matters to a stub whose bodies never run their real logic.

For each stub header under App/test/stubs/, this script:
  1. Extracts every function prototype/definition it declares (regex-based,
     see extract_prototypes() below for what it does and does not handle).
  2. Locates the real ESP-IDF header of the same relative path (e.g.
     stubs/driver/ledc.h -> <idf_root>/components/*/include/driver/ledc.h)
     by walking the IDF component tree once and indexing every header by
     its suffix path.
  3. Extracts the real header's prototypes the same way, and reports any
     stub function whose name exists in the real header but with a
     DIFFERENT argument count, or whose name does not exist in the real
     header at all (a function the stub invented, or the real one renamed).

IDF discovery: --idf-root overrides; otherwise this script tries (in order)
the IDF_PATH environment variable, then the idf_path recorded in
App/../build/project_description.json (the same file the configured
interpreter's idf.py itself consults -- see MEMORY:
project_kilnfw_idf_build_invocation.md), then a couple of common install
locations. If none resolve to a real, existing esp-idf checkout, this
SKIPS gracefully (prints why, exits 0) -- a machine with no IDF installed
must never see this check as a failure; that would be exactly the kind of
spurious-on-machines-without-IDF result ROADMAP.md's B8 item calls out.

Usage: python stub_signature_drift_check.py [--idf-root PATH] [--fatal-on-clean]

Exit codes:
  0 -- skipped (no IDF found), or ran clean, or ran with mismatches but
       --fatal-on-clean was not requested (first-light "report only" mode).
  1 -- ran and found mismatches, with --fatal-on-clean passed.
"""
import argparse
import json
import re
import sys
from pathlib import Path

THIS_DIR = Path(__file__).resolve().parent
STUBS_DIR = THIS_DIR / "stubs"

# Headers that are wholly this project's own invention (freertos/*, esp_
# app_desc.h's build_info shim, sdkconfig.h, the bx_worker/uart_protocol/
# i2c_owner/esp_spi_owner/uart_owner in-repo ownership headers, and the lwip/
# psa vendor shims) have no single corresponding "real ESP-IDF header" this
# script can locate by suffix path -- skip them rather than report a false
# "not found in IDF" for every prototype they declare.
SKIP_STUBS = {
    "sdkconfig.h",
    "build_info.h",
    "bx_worker_stub.h",
    "esp_spi_owner.h",
    "i2c_owner.h",
    "uart_owner.h",
    "uart_protocol.h",
}
SKIP_DIRS = {"freertos", "lwip", "psa"}

# One line, name(args) -- catches both a `static inline TYPE name(args) {`
# / `static inline TYPE name(args)\n{` definition (the stub's own style,
# brace same line or next line -- the trailing [;{]? below is optional and
# unanchored to end-of-line for exactly that reason) and a bare
# `TYPE name(args);` declaration (the real header's usual style). Note this
# means it will also match a plain call statement that happens to sit at
# the start of a line inside an inline function body (e.g.
# "    nvs_close(h);") -- STATEMENT_START_KEYWORDS below filters the most
# common false-positive shapes (return/if/while/...), and CONTROL_KEYWORDS
# filters the rest by name. What is left is a heuristic, not a parser --
# deliberately, per ROADMAP.md's B8 scope ("full type-compare is
# over-engineering; name+arity catches the real drift class").
PROTO_RE = re.compile(
    r"""
    ^\s*
    (?:static\s+inline\s+|static\s+|inline\s+|extern\s+)*   # qualifiers
    [A-Za-z_][A-Za-z0-9_ \t\*]*?                             # return type
    \b([A-Za-z_][A-Za-z0-9_]*)\s*                            # 1: function name
    \(([^;{)]*)\)                                            # 2: args (no nested parens)
    \s*[;{]?
    \s*$
    """,
    re.VERBOSE,
)

CONTROL_KEYWORDS = {
    "if", "for", "while", "switch", "return", "sizeof", "defined",
    "typedef", "struct", "enum", "union", "do", "else",
}

# A line whose first token is one of these is a statement inside a function
# body, never a declaration/definition -- skip it outright rather than let
# PROTO_RE misparse e.g. "    return nvs_get_blob(h, key, buf, len);" as a
# declaration of nvs_get_blob().
STATEMENT_START_KEYWORDS = {
    "return", "if", "else", "while", "for", "switch", "do", "break",
    "continue", "goto", "case", "default",
}


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def count_args(arg_str: str) -> int:
    arg_str = arg_str.strip()
    if arg_str == "" or arg_str == "void":
        return 0
    # Split on top-level commas (no nested parens expected after PROTO_RE's
    # own exclusion of '(' from the arg capture, so a plain split is safe).
    return len([a for a in arg_str.split(",") if a.strip() != ""])


def extract_prototypes(text: str):
    """Returns {name: arity} for every function-looking declaration/
    definition found. Best-effort: a macro invocation that happens to look
    like NAME(args); can produce a false entry, which is why CONTROL_
    KEYWORDS is filtered and callers should treat this as a heuristic, not
    a parser."""
    text = strip_comments(text)
    out = {}
    for line in text.splitlines():
        stripped = line.strip()
        first_token = re.match(r"[A-Za-z_][A-Za-z0-9_]*", stripped)
        if first_token and first_token.group(0) in STATEMENT_START_KEYWORDS:
            continue
        m = PROTO_RE.match(line)
        if not m:
            continue
        name, args = m.group(1), m.group(2)
        if name in CONTROL_KEYWORDS:
            continue
        out[name] = count_args(args)
    return out


def find_idf_root(explicit: str | None):
    if explicit:
        p = Path(explicit)
        return p if p.is_dir() else None

    import os
    env = os.environ.get("IDF_PATH")
    if env and Path(env).is_dir():
        return Path(env)

    # build/project_description.json's "idf_path" -- the same file idf.py
    # itself writes/consults for the configured toolchain (see MEMORY:
    # project_kilnfw_idf_build_invocation.md).
    proj_desc = THIS_DIR.parent.parent / "build" / "project_description.json"
    if proj_desc.is_file():
        try:
            data = json.loads(proj_desc.read_text(encoding="utf-8", errors="replace"))
            idf_path = data.get("idf_path")
            if idf_path and Path(idf_path).is_dir():
                return Path(idf_path)
        except (json.JSONDecodeError, OSError):
            pass

    for candidate in ("C:/esp/v6.0.2/esp-idf", "C:/esp/esp-idf", "/opt/esp-idf"):
        p = Path(candidate)
        if p.is_dir():
            return p

    return None


def index_idf_headers(idf_root: Path):
    """Maps every suffix path reachable under an `include/` directory in the
    IDF component tree to its absolute path, e.g. 'driver/ledc.h' ->
    <idf_root>/components/esp_driver_ledc/include/driver/ledc.h, plus the
    bare filename ('ledc.h' -> same path) as a fallback for stubs at the top
    level (nvs.h, esp_timer.h, ...) whose real header also sits directly
    under some component's include/ root."""
    by_suffix = {}
    by_basename = {}
    components_dir = idf_root / "components"
    if not components_dir.is_dir():
        return by_suffix, by_basename
    for include_dir in components_dir.glob("*/include"):
        for header in include_dir.rglob("*.h"):
            rel = header.relative_to(include_dir).as_posix()
            by_suffix.setdefault(rel, header)
            by_basename.setdefault(header.name, header)
    return by_suffix, by_basename


def resolve_real_header(stub_path: Path, by_suffix, by_basename):
    rel = stub_path.relative_to(STUBS_DIR).as_posix()
    if rel in by_suffix:
        return by_suffix[rel]
    return by_basename.get(stub_path.name)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--idf-root", default=None)
    ap.add_argument("--fatal-on-clean", action="store_true",
                     help="Exit 1 if mismatches are found. Without this flag "
                          "mismatches are reported but exit 0 (first-light "
                          "warn-only mode) -- flip this on once the tree is "
                          "confirmed clean.")
    args = ap.parse_args()

    idf_root = find_idf_root(args.idf_root)
    if idf_root is None:
        print("stub_signature_drift_check: no ESP-IDF installation found "
              "(checked --idf-root, $IDF_PATH, build/project_description.json, "
              "common install paths) -- SKIPPING. This is not a failure: the "
              "check needs a real IDF tree to compare stubs against, and none "
              "is required just to build/run the host tests.")
        return 0

    by_suffix, by_basename = index_idf_headers(idf_root)
    if not by_suffix:
        print(f"stub_signature_drift_check: {idf_root} does not look like an "
              f"ESP-IDF checkout (no components/*/include found) -- SKIPPING.")
        return 0

    mismatches = []
    not_found_headers = []
    scanned = 0

    for stub_path in sorted(STUBS_DIR.rglob("*.h")):
        rel_parts = stub_path.relative_to(STUBS_DIR).parts
        if stub_path.name in SKIP_STUBS or (len(rel_parts) > 1 and rel_parts[0] in SKIP_DIRS):
            continue
        real_header = resolve_real_header(stub_path, by_suffix, by_basename)
        if real_header is None:
            not_found_headers.append(stub_path.relative_to(STUBS_DIR).as_posix())
            continue
        scanned += 1
        stub_protos = extract_prototypes(stub_path.read_text(encoding="utf-8", errors="replace"))
        real_protos = extract_prototypes(real_header.read_text(encoding="utf-8", errors="replace"))
        for name, stub_arity in stub_protos.items():
            if name not in real_protos:
                continue  # real header may just not declare it as a plain prototype (macro, etc.) -- not proof of drift
            real_arity = real_protos[name]
            if stub_arity != real_arity:
                # Recover the stub's line number for a precise file:line report.
                lineno = None
                for i, line in enumerate(
                        stub_path.read_text(encoding="utf-8", errors="replace").splitlines(), start=1):
                    if re.search(r"\b" + re.escape(name) + r"\s*\(", line):
                        lineno = i
                        break
                loc = f"{stub_path.relative_to(THIS_DIR).as_posix()}:{lineno or '?'}"
                mismatches.append(
                    f"{loc}: {name}() takes {stub_arity} arg(s) in the stub, "
                    f"{real_arity} in {real_header} (arity drift)"
                )

    print(f"stub_signature_drift_check: IDF root {idf_root}")
    print(f"stub_signature_drift_check: {scanned} stub header(s) matched to real IDF headers "
          f"({len(not_found_headers)} skipped, no real-header match found: "
          f"{', '.join(not_found_headers) if not_found_headers else 'none'})")

    if not mismatches:
        print("stub_signature_drift_check: clean -- no name+arity drift found")
        return 0

    print(f"stub_signature_drift_check: {len(mismatches)} mismatch(es) found:")
    for m in mismatches:
        print(f"  {m}")

    if args.fatal_on_clean:
        return 1
    print("(warn-only mode -- pass --fatal-on-clean, or edit the check_*.ps1 "
          "wrapper, once these are fixed and the tree is confirmed clean)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
