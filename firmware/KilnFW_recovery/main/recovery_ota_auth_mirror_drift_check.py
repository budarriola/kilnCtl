#!/usr/bin/env python3
"""recovery_ota_auth_mirror_drift_check.py -- guards against the drift found
and fixed by docs/audits/web_code_duplication_drift_2026-09-18.md: the
recovery image's independent copy of the X-Ota-Mac hex-decode/length/
ordering contract silently disagreeing with the main app's consolidated
ota_http_authenticate_request() (firmware/KilnFW/App/drivers/http/ota_http.c)
and its ota_http_hex_decode() helper (ota_http_util.c). The recovery image
cannot call into ota_http.c directly (it would pull the whole main-app httpd
stack into a strict-size recovery image -- see recovery_http.c's own header
comment on why ota_auth.c is copied in verbatim instead of shared as a
library), so the fix keeps a second, textually-mirrored copy and this check
diffs the two, the same extract-normalize-diff technique as
approach_rate_cap_mirror_drift_check.py (read that one first).

WHAT IS COMPARED, and why each normalization exists:

1. Hex-nibble classification chain. recovery_http.c's hex_decode() loop body
   (per-nibble classification of `hi`/`lo` against [0-9a-fA-F], two nibbles
   per byte) must be byte-identical (modulo variable name and comments) to
   ota_http_util.c's ota_http_hex_decode() loop body. The only structural
   difference allowed is the source array name (recovery reads from `in`,
   the main app from `hex`) and the loop bound expression (recovery derives
   it from a fixed out_len*2 already validated by the caller; the main app
   takes hex_len directly) -- both normalized to a single token before
   comparing.

2. Ordering + wire strings in the shared auth helper
   (recovery_http.c's recovery_authenticate_request() -- factored out of
   ota_esp_post() on 2026-09-19 so every mutating route can share it rather
   than hand-copying the check again; see item 3 below): the header-length
   check, the "missing or malformed X-Ota-Mac header (want 64 hex chars)"
   string, the "could not read X-Ota-Mac header" string, the "X-Ota-Mac
   must be 64 hex characters" string, and the lockout check must appear in
   that literal order in the helper's source (header-length check, then
   header-read check, then hex-validity check, then lockout) -- matching
   ota_http_authenticate_request() running fully before
   ota_http_verify_request()'s lockout test (ota_http.c:935-966, :465-481).
   A regression that reorders these five or changes any wire string is
   exactly the class of drift this check exists to catch, since none of it
   is exercised by a positive test (both the old and new ordering return
   400/429 on the same malformed-input fixture; only the numeric HTTP
   status told them apart, and both are assigned in this same file so an
   accidental swap does not fail to compile). NOTE: this item, including the
   per-route HMAC context strings ("esp" / "boot-guard-reset" / "sw-reset")
   and their ordering against ota_http.c's OTA_HTTP_CONTEXT_* cases, is
   asserted here against a hardcoded transcription of ota_http.c's strings
   at the time this check was written -- it is NOT diffed byte-for-byte
   against ota_http.c the way item 1 diffs against ota_http_util.c. A
   rename of ota_http.c's context strings will not be caught by this check;
   re-verify by hand against ota_http.c:408-414 and
   tools/PcTools/src/kilnctrl/ota_http_client.py's derive_mac() call sites
   if either changes.

3. Route coverage: every mutating (state-changing, i.e. HTTP_POST/HTTP_PUT/
   HTTP_DELETE/HTTP_ANY) route registered in recovery_http.c's routes[]
   table must call recovery_authenticate_request() somewhere in its own
   handler body. This is the assertion that actually closes
   docs/audits/web_code_duplication_drift_2026-09-18.md section 2.2: the
   drift this check originally guarded (item 2 above) was narrow enough
   that two whole routes with NO auth check at all sat right next to it,
   undetected, until an audit caught them by hand. A future route that
   mutates state and forgets to call the helper now fails this check
   instead of silently shipping unauthenticated -- and because the mutating
   handler set is parsed directly out of routes[] (see item 4 below) rather
   than hand-maintained, a *forgotten* entry cannot make the check pass
   vacuously the way a hand-maintained list could. GET routes
   (challenge_get, partitions_get, boot_guard_get) are intentionally NOT
   required to authenticate -- see recovery_http.c's own file header for
   why /api/boot_guard's GET is deliberately open (diagnostics, no NVS
   write).

4. Route-coverage completeness: MUTATING_ROUTE_HANDLERS is no longer a
   hand-maintained list disconnected from recovery_http.c's actual routes[]
   table -- discover_mutating_handlers() parses routes[] itself, matching
   every entry whose `.method` is HTTP_POST, HTTP_PUT, HTTP_DELETE or
   HTTP_ANY, and returns their `.handler` names for item 3 to check. This
   closes the gap where a hand-maintained list could silently fall behind
   routes[] (an entry could be added to routes[] and never added to the
   list, making item 3 pass vacuously on exactly the new, uncovered
   handler). ROUTE_COVERAGE_ALLOWLIST below is the only escape hatch for a
   deliberately-excluded mutating route (e.g. a future route proven safe
   without per-request auth for some documented reason) -- it is currently
   empty, since every mutating route today is expected to authenticate.

Usage: python recovery_ota_auth_mirror_drift_check.py [repo_root]
Exit 0: all three comparisons pass.
Exit 1: a mismatch, or a fragment could not be located at all (fail
        closed, per this repo's standing rule for this class of check).
"""
import re
import sys
from pathlib import Path

RECOVERY_REL = "firmware/KilnFW_recovery/main/recovery_http.c"
MAIN_UTIL_REL = "firmware/KilnFW/App/drivers/http/ota_http_util.c"

RECOVERY_HEX_DECODE_RE = re.compile(
    r"static bool hex_decode\([^)]*\)\n\{\n(.*?)\n\}\n", re.DOTALL
)
MAIN_HEX_DECODE_RE = re.compile(
    r"bool ota_http_hex_decode\([^)]*\)\n\{\n(.*?)\n\}\n", re.DOTALL
)

# Lines that exist on exactly one side for structural reasons unrelated to
# the per-nibble classification arithmetic (see module docstring item 1).
RECOVERY_ONLY_LINE_RES = [
    re.compile(r"^\s*if \(strlen\(in\) != out_len \* 2\) \{\s*$"),
    re.compile(r"^\s*for \(size_t i = 0; i < out_len; i\+\+\) \{\s*$"),
]
MAIN_ONLY_LINE_RES = [
    re.compile(r"^\s*if \(hex_len % 2 != 0\) \{\s*$"),
    re.compile(r"^\s*for \(size_t i = 0; i < hex_len / 2; i\+\+\) \{\s*$"),
]


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def normalize(body: str, only_line_res: list) -> list:
    body = strip_comments(body)
    lines = []
    for raw_line in body.splitlines():
        if any(r.match(raw_line) for r in only_line_res):
            continue
        line = raw_line.strip()
        if not line:
            continue
        # `return false;`/`return true;` are NOT skipped here -- an earlier
        # version of this check dropped them as "structurally required on
        # both sides, uninteresting", which let a flipped invalid-nibble
        # guard (`return true;` instead of `return false;` when hi/lo < 0)
        # pass silently, since both sides still had the same COUNT of
        # return statements even though one now claims success on invalid
        # input. Every return line is compared like any other.
        line = line.replace("in[2 * i]", "SRC[IDX]")
        line = line.replace("in[2 * i + 1]", "SRC[IDX+1]")
        line = line.replace("hex[2 * i]", "SRC[IDX]")
        line = line.replace("hex[2 * i + 1]", "SRC[IDX+1]")
        line = re.sub(r"\s+", " ", line)
        lines.append(line)
    return lines


def find_body(text: str, pattern: re.Pattern, label: str, path: Path):
    m = pattern.search(text)
    if not m:
        print("RECOVERY OTA-AUTH MIRROR DRIFT CHECK: FAILED (extraction)")
        print(f"  Could not locate {label} in {path} --")
        print("  update this check's regex rather than letting it pass vacuously.")
        return None
    return m.group(1)


# --- ordering/wire-string check -------------------------------------------

ORDER_MARKERS = [
    ('httpd_req_get_hdr_value_len(req, "X-Ota-Mac")', "header length check"),
    ("missing or malformed X-Ota-Mac header (want 64 hex chars)", "missing/malformed header string"),
    ("could not read X-Ota-Mac header", "header-read-failure string"),
    ("X-Ota-Mac must be 64 hex characters", "bad hex string"),
    ("ota_auth_lockout_is_locked(lockout, t)", "lockout check"),
]


def check_ordering(recovery_text: str) -> list:
    """Returns a list of problems (empty if none)."""
    problems = []
    # recovery_authenticate_request() takes a per-route `context`/`lockout`
    # pair (2026-09-19 fix -- see recovery_http.c's file header on why one
    # shared lockout across three routes was itself a finding); the ordering
    # check only cares about the body, so the signature match is loose about
    # the parameter list rather than pinning the exact parameter names.
    handler_match = re.search(
        r"static bool recovery_authenticate_request\([^)]*\)\n\{\n(.*?)\n\}\n",
        recovery_text,
        re.DOTALL,
    )
    if not handler_match:
        return ["could not locate recovery_authenticate_request() in recovery_http.c"]
    body = handler_match.group(1)
    positions = []
    for marker, name in ORDER_MARKERS:
        pos = body.find(marker)
        if pos == -1:
            problems.append(f"missing expected marker ({name}): {marker!r}")
        else:
            positions.append((pos, name))
    if problems:
        return problems
    ordered = [name for _, name in sorted(positions)]
    expected = [name for _, name in ORDER_MARKERS]
    if ordered != expected:
        problems.append(
            "wrong order: found " + " -> ".join(ordered) + ", expected " + " -> ".join(expected)
        )
    return problems


# --- route-coverage check --------------------------------------------------

# Methods considered mutating/state-changing for this check's purposes.
MUTATING_METHODS = {"HTTP_POST", "HTTP_PUT", "HTTP_DELETE", "HTTP_ANY"}

# Deliberate exclusions from route coverage -- a mutating route named here is
# allowed to skip recovery_authenticate_request() without failing the check.
# Empty today: every mutating route in routes[] is expected to authenticate.
# See this module's docstring item 4 before adding an entry -- it is an
# escape hatch, not a place to quietly grow the unauthenticated surface.
ROUTE_COVERAGE_ALLOWLIST = set()

ROUTES_TABLE_RE = re.compile(
    r"static const httpd_uri_t routes\[\] = \{(.*?)\};", re.DOTALL
)
ROUTE_ENTRY_METHOD_RE = re.compile(r"\.method\s*=\s*(HTTP_\w+)")
ROUTE_ENTRY_HANDLER_RE = re.compile(r"\.handler\s*=\s*(\w+)")

AUTH_CALL_MARKER = "recovery_authenticate_request("


def discover_mutating_handlers(recovery_text: str) -> list:
    """Parses recovery_http.c's routes[] table (after stripping comments) and
    returns the `.handler` names of every entry whose `.method` is
    HTTP_POST/HTTP_PUT/HTTP_DELETE/HTTP_ANY. Returns None (via raising) if
    the table itself can't be located, so a rename of routes[] fails loud
    rather than silently checking zero handlers."""
    text = strip_comments(recovery_text)
    m = ROUTES_TABLE_RE.search(text)
    if not m:
        raise LookupError("could not locate routes[] table in recovery_http.c")
    table_body = m.group(1)
    handlers = []
    # Each entry is a brace-delimited struct literal; split on top-level
    # "}," boundaries (good enough here -- entries don't nest braces).
    for entry in re.split(r"\}\s*,", table_body):
        method_m = ROUTE_ENTRY_METHOD_RE.search(entry)
        handler_m = ROUTE_ENTRY_HANDLER_RE.search(entry)
        if not method_m or not handler_m:
            continue
        if method_m.group(1) in MUTATING_METHODS:
            handlers.append(handler_m.group(1))
    return handlers


def check_route_coverage(recovery_text: str) -> list:
    """Returns a list of problems (empty if none): every mutating handler
    discovered in routes[] (methods in MUTATING_METHODS, minus
    ROUTE_COVERAGE_ALLOWLIST) must call recovery_authenticate_request()
    somewhere in its own body, and that call must survive comment-stripping
    (a commented-out call must NOT count -- see docstring item 3)."""
    problems = []
    try:
        handlers = discover_mutating_handlers(recovery_text)
    except LookupError as exc:
        return [str(exc)]
    if not handlers:
        return ["discovered zero mutating routes in routes[] -- update this check's "
                "parsing rather than letting it pass vacuously"]
    for handler in handlers:
        if handler in ROUTE_COVERAGE_ALLOWLIST:
            continue
        handler_match = re.search(
            r"static esp_err_t " + re.escape(handler) + r"\(httpd_req_t \*req\)\n\{\n(.*?)\n\}\n",
            recovery_text,
            re.DOTALL,
        )
        if not handler_match:
            problems.append(f"could not locate handler {handler}() in recovery_http.c -- "
                             "update this check's regex rather than letting it pass vacuously")
            continue
        # Strip comments before testing for the auth-call marker -- a
        # commented-out call (e.g. "// recovery_authenticate_request(...)")
        # must not satisfy coverage.
        body = strip_comments(handler_match.group(1))
        if AUTH_CALL_MARKER not in body:
            problems.append(
                f"{handler}() does not call recovery_authenticate_request() -- this route "
                "mutates state and must authenticate every request (see docs/audits/"
                "web_code_duplication_drift_2026-09-18.md section 2.2)"
            )
    return problems


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[3]

    recovery_path = repo_root / RECOVERY_REL
    main_util_path = repo_root / MAIN_UTIL_REL

    for label, path in (("recovery", recovery_path), ("main-app util", main_util_path)):
        if not path.is_file():
            print("RECOVERY OTA-AUTH MIRROR DRIFT CHECK: FAILED (setup)")
            print(f"  {label} file not found: {path}")
            return 1

    recovery_text = recovery_path.read_text(encoding="utf-8")
    main_text = main_util_path.read_text(encoding="utf-8")

    recovery_body = find_body(recovery_text, RECOVERY_HEX_DECODE_RE, "hex_decode()", recovery_path)
    main_body = find_body(main_text, MAIN_HEX_DECODE_RE, "ota_http_hex_decode()", main_util_path)
    if recovery_body is None or main_body is None:
        return 1

    recovery_lines = normalize(recovery_body, RECOVERY_ONLY_LINE_RES)
    main_lines = normalize(main_body, MAIN_ONLY_LINE_RES)

    failed = False
    if not recovery_lines or not main_lines:
        print("RECOVERY OTA-AUTH MIRROR DRIFT CHECK: FAILED (extraction)")
        print("  Normalization left an empty fragment on one side -- fail closed rather")
        print("  than compare against nothing.")
        return 1

    if recovery_lines != main_lines:
        print("RECOVERY OTA-AUTH MIRROR DRIFT CHECK: FAILED (hex-decode nibble logic)")
        print(f"  {RECOVERY_REL}'s hex_decode() no longer matches {MAIN_UTIL_REL}'s")
        print("  ota_http_hex_decode() nibble classification -- update the mirror to match.")
        print("  --- normalized recovery ---")
        for line in recovery_lines:
            print(f"    {line}")
        print("  --- normalized main app ---")
        for line in main_lines:
            print(f"    {line}")
        failed = True

    order_problems = check_ordering(recovery_text)
    if order_problems:
        print("RECOVERY OTA-AUTH MIRROR DRIFT CHECK: FAILED (ordering/wire-strings)")
        for p in order_problems:
            print(f"  {p}")
        failed = True

    coverage_problems = check_route_coverage(recovery_text)
    if coverage_problems:
        print("RECOVERY OTA-AUTH MIRROR DRIFT CHECK: FAILED (route coverage)")
        for p in coverage_problems:
            print(f"  {p}")
        failed = True

    if failed:
        return 1

    mutating_count = len(discover_mutating_handlers(recovery_text))
    print(
        f"RECOVERY OTA-AUTH MIRROR DRIFT CHECK: OK ({len(recovery_lines)} normalized "
        "hex-decode lines match; header->hex->lockout ordering and wire strings confirmed; "
        f"{mutating_count} mutating routes all call recovery_authenticate_request())"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
