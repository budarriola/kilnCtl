#!/usr/bin/env python3
"""check_ota_http_context_mirror.py -- mirror-drift check for the OTA HMAC
context-string set, which is hand-copied in four places:

  1. ota_http.c's ota_http_verify_request() switch (`ctx_str = "..."`),
     the single source of truth for which context enum maps to which
     wire string.
  2. ota_http.c's _Static_assert table just above that switch, which only
     exists to fail the BUILD if a context string there ever grows past
     OTA_HTTP_CONTEXT_STR_MAX (ota_http.h) without that constant being
     widened in the same change.
  3. firmware/KilnFW/App/test/test_ota_http.c's own ctx_str_for(), a
     deliberately independent test oracle -- and its compute_mac() msg[]
     buffer, sized from OTA_HTTP_CONTEXT_STR_MAX since the fix this check
     accompanies (that buffer used to be sized from its own hand-copied
     literal, 16, while ota_http.c's real buffer used a stale 13 --
     exactly the 3-byte overflow this whole mirror-check family exists to
     catch one class earlier).
  4. tools/PcTools/src/kilnctrl/ota_http_client.py's derive_mac() allow-list,
     the PC-side set of context strings a caller may sign for.

WHAT IS COMPARED:
  - The four sets of context-string literals above must be IDENTICAL.
  - Every literal, in every set, must be <= OTA_HTTP_CONTEXT_STR_MAX as
    read from ota_http.h (parsed, not hardcoded here -- a bump to that
    constant must not require an edit to this check too).

Fails closed (same convention as every other check in this family): if any
source file's shape no longer matches this check's own extraction regexes,
that is a FAILURE, not a silent pass with an empty set.

Usage: python tools/check_ota_http_context_mirror.py [repo_root]
"""
import re
import sys
from pathlib import Path

OTA_HTTP_H_REL = "firmware/KilnFW/App/drivers/http/ota_http.h"
OTA_HTTP_C_REL = "firmware/KilnFW/App/drivers/http/ota_http.c"
TEST_OTA_HTTP_REL = "firmware/KilnFW/App/test/test_ota_http.c"
OTA_HTTP_CLIENT_REL = "tools/PcTools/src/kilnctrl/ota_http_client.py"

CONTEXT_STR_MAX_RE = re.compile(r"#define\s+OTA_HTTP_CONTEXT_STR_MAX\s+(\d+)")

# ota_http.c's switch: `case OTA_HTTP_CONTEXT_...: ctx_str = "literal";`
SWITCH_CASE_RE = re.compile(r'ctx_str\s*=\s*"([^"]+)"')

# ota_http.c's _Static_assert table: `_Static_assert(sizeof("literal") - 1 <= ...`
STATIC_ASSERT_RE = re.compile(r'_Static_assert\(sizeof\("([^"]+)"\)\s*-\s*1\s*<=\s*OTA_HTTP_CONTEXT_STR_MAX')

# test_ota_http.c's ctx_str_for(): `case ...: return "literal";`
TEST_ORACLE_RE = re.compile(r'case\s+OTA_HTTP_CONTEXT_\w+:\s*return\s*"([^"]+)"')

# ota_http_client.py's derive_mac() allow-list tuple of string literals.
PY_ALLOWLIST_RE = re.compile(
    r'if\s+context\s+not\s+in\s*\((.*?)\):', re.DOTALL)
PY_STR_LITERAL_RE = re.compile(r'"([^"]+)"')


def strip_c_comments(text: str) -> str:
    # Line comments FIRST: this file has `// ... /api/* handler ...` line
    # comments whose text contains a literal "/*" that is not a real block
    # comment opener. Stripping block comments first would pair that stray
    # "/*" (DOTALL, non-greedy) with the next real "*/" anywhere later in
    # the file and silently eat everything in between, including the very
    # switch/table this check exists to read -- exactly the "regex ate more
    # than intended" class this whole check family is meant to catch one
    # level up, so get it right here first.
    text = re.sub(r"//[^\n]*", "", text)
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    return text


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]

    paths = {
        "ota_http.h": repo_root / OTA_HTTP_H_REL,
        "ota_http.c": repo_root / OTA_HTTP_C_REL,
        "test_ota_http.c": repo_root / TEST_OTA_HTTP_REL,
        "ota_http_client.py": repo_root / OTA_HTTP_CLIENT_REL,
    }
    for label, path in paths.items():
        if not path.is_file():
            print("OTA-HTTP CONTEXT MIRROR CHECK: FAILED (setup)")
            print(f"  {label} not found: {path}")
            return 1

    header_text = paths["ota_http.h"].read_text(encoding="utf-8")
    m = CONTEXT_STR_MAX_RE.search(strip_c_comments(header_text))
    if not m:
        print("OTA-HTTP CONTEXT MIRROR CHECK: FAILED (setup)")
        print(f"  could not find #define OTA_HTTP_CONTEXT_STR_MAX in {OTA_HTTP_H_REL} -- "
              "update this check's CONTEXT_STR_MAX_RE rather than letting it pass vacuously")
        return 1
    ctx_str_max = int(m.group(1))

    c_text = strip_c_comments(paths["ota_http.c"].read_text(encoding="utf-8"))
    switch_ctx = set(SWITCH_CASE_RE.findall(c_text))
    assert_ctx = set(STATIC_ASSERT_RE.findall(c_text))

    test_text = strip_c_comments(paths["test_ota_http.c"].read_text(encoding="utf-8"))
    test_ctx = set(TEST_ORACLE_RE.findall(test_text))

    py_text = paths["ota_http_client.py"].read_text(encoding="utf-8")
    m2 = PY_ALLOWLIST_RE.search(py_text)
    if not m2:
        print("OTA-HTTP CONTEXT MIRROR CHECK: FAILED (setup)")
        print(f"  could not find derive_mac()'s allow-list `if context not in (...)` in "
              f"{OTA_HTTP_CLIENT_REL} -- update this check's PY_ALLOWLIST_RE rather than "
              "letting it pass vacuously")
        return 1
    py_ctx = set(PY_STR_LITERAL_RE.findall(m2.group(1)))

    sets = {
        "ota_http.c switch": switch_ctx,
        "ota_http.c _Static_assert table": assert_ctx,
        "test_ota_http.c ctx_str_for()": test_ctx,
        "ota_http_client.py derive_mac() allow-list": py_ctx,
    }

    failures = []
    for label, s in sets.items():
        if not s:
            failures.append(f"{label}: extracted an EMPTY set -- this check's regex for that "
                            "site is broken, not that the site legitimately has zero contexts")

    if not failures:
        union = set().union(*sets.values())
        for label, s in sets.items():
            missing = union - s
            extra = s - union  # always empty by construction, kept for symmetry/clarity
            if missing:
                failures.append(f"{label} is missing: {sorted(missing)}")
            if extra:
                failures.append(f"{label} has unexpected extra entries: {sorted(extra)}")

        for label, s in sets.items():
            too_long = sorted(lit for lit in s if len(lit) > ctx_str_max)
            if too_long:
                failures.append(f"{label} has literal(s) exceeding OTA_HTTP_CONTEXT_STR_MAX "
                                f"({ctx_str_max}): {too_long}")

    if failures:
        print("OTA-HTTP CONTEXT MIRROR CHECK: FAILED")
        for f in failures:
            print(f"  - {f}")
        return 1

    all_literals = switch_ctx
    longest = max(len(lit) for lit in all_literals)
    print(f"OTA-HTTP CONTEXT MIRROR CHECK: OK ({len(all_literals)} context strings agree across "
         f"all four sites; longest is {longest} chars, OTA_HTTP_CONTEXT_STR_MAX is {ctx_str_max})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
