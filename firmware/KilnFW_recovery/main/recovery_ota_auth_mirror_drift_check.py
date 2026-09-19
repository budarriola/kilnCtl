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

2. Ordering + wire strings in the POST /api/ota/esp handler
   (recovery_http.c's ota_esp_post()): the header-length check, the
   "missing or malformed X-Ota-Mac header (want 64 hex chars)" string, the
   "could not read X-Ota-Mac header" string, the "X-Ota-Mac must be 64 hex
   characters" string, and the lockout check must appear in that literal
   order in the handler source (header-length check, then header-read
   check, then hex-validity check, then lockout) -- matching
   ota_http_authenticate_request() running fully before
   ota_http_verify_request()'s lockout test (ota_http.c:935-966, :465-481).
   A regression that reorders these five or changes any wire string is
   exactly the class of drift this check exists to catch, since none of it
   is exercised by a positive test (both the old and new ordering return
   400/429 on the same malformed-input fixture; only the numeric HTTP
   status told them apart, and both are assigned in this same file so an
   accidental swap does not fail to compile).

Usage: python recovery_ota_auth_mirror_drift_check.py [repo_root]
Exit 0: both comparisons pass.
Exit 1: a mismatch, or either fragment could not be located at all (fail
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
    ("ota_auth_lockout_is_locked(&s_lockout, t)", "lockout check"),
]


def check_ordering(recovery_text: str) -> list:
    """Returns a list of problems (empty if none)."""
    problems = []
    handler_match = re.search(
        r"static esp_err_t ota_esp_post\(httpd_req_t \*req\)\n\{\n(.*?)\n\}\n",
        recovery_text,
        re.DOTALL,
    )
    if not handler_match:
        return ["could not locate ota_esp_post() in recovery_http.c"]
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

    if failed:
        return 1

    print(
        f"RECOVERY OTA-AUTH MIRROR DRIFT CHECK: OK ({len(recovery_lines)} normalized "
        "hex-decode lines match; header->hex->lockout ordering and wire strings confirmed)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
