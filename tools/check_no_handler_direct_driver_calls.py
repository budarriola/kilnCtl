#!/usr/bin/env python3
"""check_no_handler_direct_driver_calls.py -- docs/HTTP_HANDLER_OWNERSHIP.md's
Batch A mechanical follow-up.

WHY THIS EXISTS. That plan's "Method" section found five HTTP-handler call
sites (`dashboard_http.c` x2, `ota_http.c` x2, and one that turned out to be a
comment-only false positive in `diagnostics_http.c`) reading hardware directly
-- `MAX31856_read_all()`/`kiln_io_read()` -- one layer under the owner-task
accessors (`thermo_owner_command_read_all()`, `kiln_io_owner_command_read()`)
that every other read/write in these files already goes through. Batch A
routed all four real sites through their owner accessor. Nothing mechanical
stopped a future handler from reintroducing a direct call (or a direct
`esp_wifi_*` call, mirroring Phase 4's already-clean `wifi_provision_http.c`)
later -- this is that mechanism, following `check_relay_authority_paths.py`'s
shape (source-text scan, C-comment stripping so a comment mentioning a
driver-function name -- as `diagnostics_http.c`'s own header comment does --
never produces a false positive or a line-number-drift bug).

What counts as an offense: inside any `firmware/KilnFW/App/drivers/http/*.c`
file, a call to `MAX31856_read_all()`, `MAX31856_read()`,
`MAX31856_start_all()`, `MAX31856_configure()`, `kiln_io_read()`,
`kiln_io_set_relay()`, `kiln_io_set_relay_mask()`, `kiln_io_all_relays_off()`,
`kiln_io_set_io()`, or any `esp_wifi_*()` function -- or a bare *reference* to
one of those names (a function-pointer assignment or a `#define` alias), which
reaches the driver the same way one indirection later; see `REF_RE` below.
The plan is explicit that
there is no legitimate-bypass exception for an HTTP handler the way
`main.c`'s panic path and `profile_executor.c`'s watchdog are legitimate for
relay-off writes (`check_relay_authority_paths.py`'s FW_ALL_OFF_ALLOWLIST) --
so for the thermo/kiln_io half of this check that allowlist is empty by
design, and any future finding there needs a new owner accessor, not an
allowlist entry.

`esp_wifi_*` is different: `factory_reset.c`'s `execute_scope_job()` and
`log_net80211_key_count()` call `esp_wifi_restore()`/`esp_wifi_set_storage()`
directly and deliberately (`docs/audits/wifi_factory_reset_driver_storage_2026-09-21.md`)
to clear the IDF Wi-Fi driver's own `nvs.net80211` copy of STA/AP config on a
Wi-Fi-scoped factory reset -- there is no `wifi_prov_*()` wrapper for this
because `wifi_prov` is a provisioning-state API, not a driver-storage-reset
one, and `wifi_provision_http.c` itself (Phase 4) makes zero direct
`esp_wifi_*` calls, confirming the wrapper covers every *provisioning*
operation. `FW_WIFI_ALLOWLIST` below names that one function narrowly, the
same shape as the relay checker's `FW_ALL_OFF_ALLOWLIST` -- anywhere else is
still flagged with no exception.

Deliberately NOT flagged (per the plan's own "Non-findings" section): a call
into `zones_config_*` or `safety_link_*` -- both are already the sanctioned
owning-module API for their domain, not a bypass -- and a bare mention of a
driver type/constant (`MAX31856_CHANNEL_COUNT`, `MAX31856Reading`,
`MAX31856_config_default()`, which is a pure struct initializer with no bus
I/O) that is not itself one of the calls named above. String literals (e.g.
a log message that happens to name a driver function, as
`factory_reset.c`'s own `log_net80211_key_count()` does) are stripped before
scanning, the same way comments are, so quoting a function name in a log
string never trips this check.

Usage: python tools/check_no_handler_direct_driver_calls.py [--root REPO_ROOT]
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

SCAN_DIR = "firmware/KilnFW/App/drivers/http"

_NAMES = (
    r"MAX31856_read_all|MAX31856_read|MAX31856_start_all|MAX31856_configure|"
    r"kiln_io_read|kiln_io_set_relay_mask|kiln_io_set_relay|kiln_io_all_relays_off|"
    r"kiln_io_set_io|esp_wifi_\w+"
)

CALL_RE = re.compile(r"\b(" + _NAMES + r")\s*\(")

#: A direct call is the obvious bypass; taking the function's ADDRESS is the
#: same bypass one indirection later. Neither
#: `static read_all_fn_t f = MAX31856_read_all;` nor
#: `#define RAW_READ_ALL MAX31856_read_all` ever writes `MAX31856_read_all(`
#: on any line, so CALL_RE alone passes both (confirmed by negative test
#: 2026-09-22). This second pattern matches the bare name NOT followed by
#: `(` -- verified to produce zero hits across all 54 current
#: firmware/KilnFW/App/drivers/http/*.c files, so it costs no false positives
#: today. Comments and string literals are stripped before either pattern
#: runs, so a prose or log-message mention still never trips it.
REF_RE = re.compile(r"\b(" + _NAMES + r")\b(?!\s*\()")

# (filename, function-name substring) -- a direct esp_wifi_restore()/
# esp_wifi_set_storage() call is allowed only inside one of these functions in
# one of these files. Anywhere else (including a DIFFERENT function in the
# same file, or any MAX31856_*/kiln_io_* call anywhere) is flagged -- see the
# module doc comment above for why this one pair is narrowly allowlisted.
FW_WIFI_ALLOWLIST = {
    ("factory_reset.c", "execute_scope_job"),
    ("factory_reset.c", "log_net80211_key_count"),
}

#: Crude but sufficient for this codebase's style -- same regex
#: check_relay_authority_paths.py uses to find "what function am I currently
#: inside" by scanning upward for the nearest function-definition-shaped line.
FUNC_DEF_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*]*\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;]*$")


def _enclosing_function(lines: list[str], call_line_idx: int) -> str | None:
    for i in range(call_line_idx, -1, -1):
        line = lines[i]
        if line.startswith(("    ", "\t", "}")) or not line.strip():
            continue
        m = FUNC_DEF_RE.match(line)
        if m:
            return m.group(1)
    return None


def _strip_c_comments(text: str) -> str:
    # Same approach as check_relay_authority_paths.py's _strip_c_comments:
    # block comments replaced by the same number of newlines they spanned (so
    # reported line numbers stay aligned with the original file), then line
    # comments stripped.
    def _blank_block(m: re.Match) -> str:
        return "\n" * m.group(0).count("\n")

    text = re.sub(r"/\*.*?\*/", _blank_block, text, flags=re.DOTALL)
    text = re.sub(r"//.*", "", text)
    return text


def _strip_c_strings(text: str) -> str:
    # A log message quoting a driver function name (factory_reset.c's own
    # log_net80211_key_count()'s ESP_LOGW format string names esp_wifi_
    # restore()) must not trip this check. Blank out double-quoted string
    # contents, preserving length so column offsets within a line are
    # unaffected and no line count shifts (unlike comment-stripping, a
    # string never spans a stripped newline in this codebase's style, so no
    # newline-count bookkeeping is needed here).
    return re.sub(r'"(?:[^"\\]|\\.)*"', lambda m: '"' + " " * (len(m.group(0)) - 2) + '"', text)


def find_files(root: Path) -> list[Path]:
    base = root / SCAN_DIR
    if not base.exists():
        return []
    return sorted(base.glob("*.c"))


def check_file(path: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8")
    stripped = _strip_c_strings(_strip_c_comments(raw))
    stripped_lines = stripped.splitlines()
    raw_lines = raw.splitlines()
    violations: list[str] = []
    for i, line in enumerate(stripped_lines):
        m = CALL_RE.search(line)
        by_reference = False
        if not m:
            m = REF_RE.search(line)
            by_reference = m is not None
        if not m:
            continue
        fn = m.group(1)
        how = "reference to" if by_reference else "direct"
        indirect = (" (taken by reference -- reaching the driver through a"
                    " function pointer or macro is the same bypass)"
                    if by_reference else " call")
        source_line = raw_lines[i].strip() if i < len(raw_lines) else line.strip()
        if fn.startswith("esp_wifi_"):
            enclosing = _enclosing_function(stripped_lines, i)
            if (path.name, enclosing) in FW_WIFI_ALLOWLIST:
                continue
            violations.append(
                f"{path}:{i + 1}: {how} {fn}(){indirect} in an HTTP handler file "
                f"(in {enclosing or '<unknown function>'}()) -- no wifi_prov_*() "
                f"wrapper exists for this, and it is not one of the narrowly "
                f"allowlisted factory-reset driver-storage-reset call sites; "
                f"either route it through wifi_prov or add a justified "
                f"allowlist entry: {source_line}"
            )
        else:
            violations.append(
                f"{path}:{i + 1}: {how} {fn}(){indirect} in an HTTP handler file -- "
                f"route through thermo_owner_command_read_all()/"
                f"kiln_io_owner_command_read() (or the matching owner accessor) "
                f"instead, per docs/HTTP_HANDLER_OWNERSHIP.md: {source_line}"
            )
    return violations


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root", default=None, help="repo root (default: two levels up from this file)"
    )
    args = parser.parse_args()
    root = Path(args.root) if args.root else Path(__file__).resolve().parents[1]

    violations: list[str] = []
    for path in find_files(root):
        violations.extend(check_file(path))

    if violations:
        print("check_no_handler_direct_driver_calls: direct driver call(s) found in an HTTP handler:")
        for v in violations:
            print(f"  {v}")
        return 1

    print("check_no_handler_direct_driver_calls: OK -- no firmware/KilnFW/App/drivers/http/*.c "
          "file calls a MAX31856_*/kiln_io_*/esp_wifi_* primitive directly; every read/write "
          "goes through its owner-task accessor.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
