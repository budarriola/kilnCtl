#!/usr/bin/env python3
"""recovery_ota_auth_mirror_drift_check.py -- originally guarded the drift
found and fixed by docs/audits/web_code_duplication_drift_2026-09-18.md: the
recovery image's independent copy of the X-Ota-Mac hex-decode/length/
ordering contract silently disagreeing with the main app's (then-)
consolidated ota_http_authenticate_request()
(firmware/KilnFW/App/drivers/http/ota_http.c) and its ota_http_hex_decode()
helper (ota_http_util.c).

**2026-09-29 update:** the main app's AP-password HMAC scheme (X-Ota-Mac,
the challenge/nonce/lockout dance, ota_http_hex_decode(),
ota_http_verify_request()) was retired outright for the 9 main-app admin OTA
routes (WEB_AUTH_PLAN.md item 2b, owner decision "Retire; open when login
off") -- ROUTE_TIER_ADMIN is now their only gate, on or off.
ota_http_hex_decode() no longer exists in ota_http_util.c, so there is
nothing left in the main app to diff the recovery image's hex_decode()
against; the cross-file hex-decode comparison (formerly item 1 below) has
been removed for that reason, not because the recovery-side contract itself
stopped mattering.

The recovery image (firmware/KilnFW_recovery/) is a genuinely separate,
standalone firmware image with its own independent auth code and is
UNCHANGED by the above -- it still requires the AP-password HMAC on its own
mutating routes, and recovery_http.c's own ota_auth.c copy (still mirrored
verbatim against firmware/KilnFW/App/drivers/net/ota_auth.c, now unused by
any main-app HTTP route but kept in place rather than deleted, precisely so
a check like this one still has a byte-identical mirror to point at if this
check or a future one needs it again) is untouched. What this check still
enforces below (ordering/wire-strings, route coverage, per-route context
strings) is entirely internal to recovery_http.c and remains meaningful on
its own -- it no longer needs a live main-app counterpart to diff against
for that guarantee.

WHAT IS COMPARED, and why each normalization exists:

1. (Retired 2026-09-29 -- see the module docstring update above.) This used
   to diff recovery_http.c's hex_decode() loop body (per-nibble
   classification of `hi`/`lo` against [0-9a-fA-F], two nibbles per byte)
   against ota_http_util.c's ota_http_hex_decode() loop body; the latter no
   longer exists in the main app, so there is nothing left to diff against.
   recovery_http.c's own hex_decode() is untouched and still correct on its
   own -- this item is simply gone, not failing.

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
   accidental swap does not fail to compile).

   NOTE on context strings: ORDER_MARKERS above checks recovery_
   authenticate_request()'s own body and has no notion of which context
   string a given *caller* passes it -- it would stay green even if
   boot_guard_reset_post() were reverted to call with "esp" instead of its
   own "boot-guard-reset". That specific hazard is covered separately by
   check_route_contexts()/ROUTE_CONTEXT_MARKERS below, which pins each of
   the three mutating handlers (ota_esp_post/boot_guard_reset_post/
   sw_reset_post) to its own expected context literal in its own call site.
   Both ORDER_MARKERS and ROUTE_CONTEXT_MARKERS's three context strings
   ("esp" / "boot-guard-reset" / "sw-reset") are asserted here against a
   hardcoded transcription of ota_http.c's OTA_HTTP_CONTEXT_* strings
   (ota_http.c:408-414) and ota_http_client.py's derive_mac() call sites
   (:513, :630) at the time this check was written -- NOT diffed
   byte-for-byte against either file the way item 1 diffs hex_decode()
   against ota_http_util.c. A rename of any of those strings on either side
   will not be caught by this check; re-verify by hand against both files
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
Exit 0: ordering/wire-strings, route coverage, and per-route context strings
        all pass.
Exit 1: a mismatch, or a fragment could not be located at all (fail
        closed, per this repo's standing rule for this class of check).
"""
import re
import sys
from pathlib import Path

RECOVERY_REL = "firmware/KilnFW_recovery/main/recovery_http.c"


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text



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


# --- per-route context-string check -----------------------------------------

# Each mutating handler must call recovery_authenticate_request() with its
# own, distinct HMAC context literal -- see the module docstring item 2 NOTE.
# This is a hardcoded transcription of ota_http.c:408-414's OTA_HTTP_CONTEXT_*
# strings and ota_http_client.py's derive_mac() call sites (:513, :630), not a
# diff against either file (unlike item 1's hex_decode() comparison) -- if
# either changes, re-verify by hand and update this table.
ROUTE_CONTEXT_MARKERS = [
    ("ota_esp_post", 'recovery_authenticate_request(req, &auth_err, "esp"'),
    ("boot_guard_reset_post", 'recovery_authenticate_request(req, &auth_err, "boot-guard-reset"'),
    ("sw_reset_post", 'recovery_authenticate_request(req, &auth_err, "sw-reset"'),
]


def check_route_contexts(recovery_text: str) -> list:
    """Returns a list of problems (empty if none): each handler in
    ROUTE_CONTEXT_MARKERS must call recovery_authenticate_request() with its
    own pinned context literal in its own body -- catches a handler reverted
    to (or copy-pasted with) the wrong context string, e.g.
    boot_guard_reset_post() calling with "esp" instead of
    "boot-guard-reset", which the ordering/route-coverage checks alone do
    not: both only look for *a* call to the helper, not *which* context
    string it passes."""
    problems = []
    for handler, expected_marker in ROUTE_CONTEXT_MARKERS:
        handler_match = re.search(
            r"static esp_err_t " + re.escape(handler) + r"\(httpd_req_t \*req\)\n\{\n(.*?)\n\}\n",
            recovery_text,
            re.DOTALL,
        )
        if not handler_match:
            problems.append(f"could not locate handler {handler}() in recovery_http.c -- "
                             "update ROUTE_CONTEXT_MARKERS/this check's regex rather than "
                             "letting it pass vacuously")
            continue
        body = strip_comments(handler_match.group(1))
        if expected_marker not in body:
            problems.append(
                f"{handler}() does not call recovery_authenticate_request() with its own "
                f"context ({expected_marker!r} not found) -- a copy-pasted or reverted "
                "context string would authenticate with the wrong HMAC input and either "
                "reject every legitimate caller or, worse, accept a MAC computed for a "
                "different route"
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

    if not recovery_path.is_file():
        print("RECOVERY OTA-AUTH MIRROR DRIFT CHECK: FAILED (setup)")
        print(f"  recovery file not found: {recovery_path}")
        return 1

    recovery_text = recovery_path.read_text(encoding="utf-8")

    failed = False

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

    context_problems = check_route_contexts(recovery_text)
    if context_problems:
        print("RECOVERY OTA-AUTH MIRROR DRIFT CHECK: FAILED (per-route context strings)")
        for p in context_problems:
            print(f"  {p}")
        failed = True

    if failed:
        return 1

    mutating_count = len(discover_mutating_handlers(recovery_text))
    print(
        "RECOVERY OTA-AUTH MIRROR DRIFT CHECK: OK (header->hex->lockout ordering and wire "
        f"strings confirmed; {mutating_count} mutating routes all call "
        "recovery_authenticate_request())"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
