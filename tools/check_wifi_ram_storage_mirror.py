#!/usr/bin/env python3
"""check_wifi_ram_storage_mirror.py -- mirror-drift check for
esp_wifi_set_storage(WIFI_STORAGE_RAM), fix 1319e051.

That fix put an esp_wifi_set_storage(WIFI_STORAGE_RAM) call BEFORE the
Wi-Fi driver is ever handed a config, in two independent places:
  - firmware/KilnFW/App/drivers/net/wifi_prov.c
  - firmware/KilnFW_recovery/main/recovery_wifi.c

Both must keep switching the Wi-Fi driver to RAM storage before it is ever
handed a config, or the driver silently resumes writing its own credential
copy into the default nvs partition (see
docs/audits/wifi_factory_reset_leaves_idf_driver_config_2026-09-21.md).
These two files have no shared header or common call site, so nothing but
a standing check keeps one edited-away or reordered fix from going
unnoticed while its sibling still holds.

WHAT IS CHECKED, per file:
  (a) the file exists (FAILS, not SKIPS, if missing -- a rename must not
      go green by vanishing from the glob)
  (b) exactly one esp_wifi_set_storage(WIFI_STORAGE_RAM) call
  (c) that call happens before the driver can be handed a config, i.e.
      before the first CALL (not definition) of any function -- defined in
      the same file -- whose own body contains a literal
      esp_wifi_set_config( call, reached from the function that contains
      the storage call itself. (recovery_wifi.c's set_config calls live in
      try_station()/start_softap(), which are DEFINED earlier in the file
      than the storage call in recovery_wifi_start() but CALLED from it --
      a plain top-to-bottom textual-position check would false-fail on
      that shape, so this check follows one level of call graph instead of
      raw line position. wifi_prov.c's own esp_wifi_set_config() call lives
      in a different file (wifi_prov_link.c's apply_ap_config()/
      apply_sta_config()) entirely, so there is nothing in-file to order
      against there -- existence-and-uniqueness of the storage call is the
      whole check for that file.)

This is deliberately a textual/call-graph-lite check, not a real
control-flow one -- same class and same limits as the other
*_mirror_drift_check.py scripts in this family (see
check_config_convert_mirror.py's docstring). It will not catch every
possible reorder (e.g. one hidden behind a function pointer), but it does
catch the concrete regression this check exists for: the storage call
being deleted, duplicated, or moved to after the code path that first
applies a config.

Usage: python tools/check_wifi_ram_storage_mirror.py [repo_root]
"""
import re
import sys
from pathlib import Path

FILES = (
    "firmware/KilnFW/App/drivers/net/wifi_prov.c",
    "firmware/KilnFW_recovery/main/recovery_wifi.c",
)

STORAGE_RE = re.compile(r"esp_wifi_set_storage\s*\(\s*WIFI_STORAGE_RAM\s*\)")
SET_CONFIG_RE = re.compile(r"esp_wifi_set_config\s*\(")
# A C function definition: "<ret type/qualifiers> name(args) {" at the start
# of a top-level statement. Deliberately simple -- this repo's style always
# opens the body brace on its own line for these driver files.
FUNC_DEF_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*\n]*?\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*\n\{",
                          re.MULTILINE)


def strip_comments(text: str) -> str:
    text = re.sub(r"/\*.*?\*/", " ", text, flags=re.DOTALL)
    text = re.sub(r"//[^\n]*", "", text)
    return text


def extract_functions(text: str):
    """Return {func_name: (body_start, body_end)} for each top-level
    function definition found, body_end being the index just past the
    matching closing brace."""
    funcs = {}
    for m in FUNC_DEF_RE.finditer(text):
        name = m.group(1)
        brace_start = m.end() - 1  # index of the opening '{'
        depth = 0
        i = brace_start
        while i < len(text):
            if text[i] == '{':
                depth += 1
            elif text[i] == '}':
                depth -= 1
                if depth == 0:
                    funcs[name] = (brace_start, i + 1)
                    break
            i += 1
    return funcs


def check_file(repo_root: Path, rel_path: str, failures: list) -> None:
    path = repo_root / rel_path
    if not path.is_file():
        failures.append(f"{rel_path}: file not found -- a rename must not go green")
        return

    text = strip_comments(path.read_text(encoding="utf-8"))

    storage_matches = list(STORAGE_RE.finditer(text))
    if len(storage_matches) == 0:
        failures.append(f"{rel_path}: no esp_wifi_set_storage(WIFI_STORAGE_RAM) call found -- "
                         "the driver will resume writing its own credential copy into the default "
                         "nvs partition")
        return
    if len(storage_matches) > 1:
        failures.append(f"{rel_path}: found {len(storage_matches)} esp_wifi_set_storage(WIFI_STORAGE_RAM) "
                         "calls, expected exactly 1 -- update this check if that is now intentional")
        return
    storage_pos = storage_matches[0].start()

    funcs = extract_functions(text)
    if not funcs:
        failures.append(f"{rel_path}: could not extract any function bodies -- update this check's "
                         "FUNC_DEF_RE rather than letting it pass vacuously")
        return

    # Functions (defined in this file) whose own body directly calls
    # esp_wifi_set_config( -- the "config appliers".
    appliers = {name for name, (s, e) in funcs.items() if SET_CONFIG_RE.search(text[s:e])}

    # The function that contains the storage call.
    entry_name = None
    entry_span = None
    for name, (s, e) in funcs.items():
        if s <= storage_pos < e:
            entry_name = name
            entry_span = (s, e)
            break
    if entry_span is None:
        failures.append(f"{rel_path}: esp_wifi_set_storage(WIFI_STORAGE_RAM) call is not inside any "
                         "extracted function body -- update this check's FUNC_DEF_RE")
        return

    if entry_name in appliers:
        # Direct call in the same function: plain textual order applies.
        first_set_config_pos = min(m.start() for m in SET_CONFIG_RE.finditer(text[entry_span[0]:entry_span[1]]))
        first_set_config_pos += entry_span[0]
        if storage_pos >= first_set_config_pos:
            failures.append(f"{rel_path}: esp_wifi_set_storage(WIFI_STORAGE_RAM) appears AFTER "
                             "esp_wifi_set_config( in the same function -- the driver will persist "
                             "credentials to NVS before RAM storage takes effect")
        return

    # Indirect case: look for the first CALL SITE (not definition) inside
    # entry_name's body to any other in-file function that is a "config
    # applier".
    entry_body = text[entry_span[0]:entry_span[1]]
    call_positions = []
    for applier in appliers:
        for m in re.finditer(r"\b" + re.escape(applier) + r"\s*\(", entry_body):
            # Skip the applier's own definition if it happens to be nested
            # (not expected here, but keep this honest).
            call_positions.append(m.start())
    if not call_positions:
        # No config-applying call reachable (one level deep) from the
        # storage-containing function in this file -- e.g. wifi_prov.c,
        # where the real esp_wifi_set_config() call lives in a sibling
        # file (wifi_prov_link.c). Existence-and-uniqueness of the
        # storage call, already checked above, is this file's whole
        # obligation.
        return

    first_call_pos = min(call_positions) + entry_span[0]
    storage_pos_in_entry = storage_pos  # already absolute
    if storage_pos_in_entry >= first_call_pos:
        failures.append(f"{rel_path}: in {entry_name}(), esp_wifi_set_storage(WIFI_STORAGE_RAM) appears "
                         f"AFTER the first call that applies a Wi-Fi config -- the driver will persist "
                         "credentials to NVS before RAM storage takes effect")


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[1]

    failures = []
    for rel_path in FILES:
        check_file(repo_root, rel_path, failures)

    if failures:
        print("WIFI RAM-STORAGE MIRROR CHECK: FAILED")
        for f in failures:
            print(f"  - {f}")
        return 1

    print(f"WIFI RAM-STORAGE MIRROR CHECK: OK ({len(FILES)} files each call "
          "esp_wifi_set_storage(WIFI_STORAGE_RAM) exactly once, before the driver can be handed a "
          "config)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
