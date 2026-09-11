#!/usr/bin/env python3
"""safety_cfg_param_table_mirror_drift_check.py -- the ESP's safety-cfg
commissioning surface (safety_cfg_http.c's generic POST /api/safety/
commissioning handler, via apply_pairs()/confirm_commit_landed()) only knows
how to stage, commit and CONFIRM a param id that appears in safety_cfg_
store.c's SAFETY_CFG_PARAM_TABLE -- that table is a hand-maintained MIRROR of
the Pico's own authoritative id table, SaftyFW's config_params.c
CONFIG_PARAM_TABLE. The two are edited in different repos-worth of files by
different commits, with no shared header enforcing agreement, which is
exactly the "reset one side of a pair" bug shape docs/audits/ and CLAUDE.md's
own standing-practice note both call out.

A drift in either direction is a real propagation gap, not a cosmetic
mismatch:
  - Pico gains a param id the ESP table doesn't know: config_params_get()
    happily reports it in GET_CONFIG_PAGE, but safety_cfg_store_lookup()
    (which the ESP web GUI's generic commissioning endpoint calls before
    ever staging a SET_PARAM) returns "unknown" -- apply_pairs() refuses the
    id outright, so nothing set through this UI can ever reach that field.
    A producer with no writer.
  - ESP table gains an id the Pico table doesn't know: apply_pairs() will
    happily stage and commit it (safety_link_send_set_param() doesn't
    validate against the Pico's table -- only the Pico's own config_params_
    set() does, at COMMIT_CONFIG time), the Pico's config_params_set()
    switch falls through its `default: return false`, and the field is
    silently dropped from the record. A write with no consumer -- and worse,
    confirm_commit_landed()'s own safety_cfg_store_lookup(id) call (looking
    the id up in the SAME ESP-side table that manufactured it) will find it
    and "confirm" a value that was never actually written into config_store_
    record_t at all, because the read-back path (safety_cfg_store_refetch())
    walks the Pico's OWN advertised param list -- see NOTE below on why this
    script treats a name-only drift (same id, different name) as a real
    finding too, since a misnamed field is exactly the "silently misroutes a
    commissioned value" bug config_params.c's own header comment warns about.

WHAT IS COMPARED: the {id: (type, name)} set implied by
firmware/SaftyFW/src/config_params.c's CONFIG_PARAM_TABLE array (the
authoritative source -- COMMISSIONING.md sec 2.1) against
firmware/KilnFW/App/drivers/safety/safety_cfg_store.c's SAFETY_CFG_PARAM_TABLE
array. Both are parsed with a regex tolerant of the surrounding comments --
this is a structural id/type/name diff, not a byte-identical text compare
(the two files' comment styles differ on purpose; see each file's own header
comment for why they are maintained as independent statements of the same
id set, with a *different* test -- test_config_store.c on the Pico side --
already covering the OTHER half of this drift class, Pico's own get()/set()
switches vs Pico's own enumeration table). This script is the one check nothing
currently runs: the two SIDES of the link.

Usage: python safety_cfg_param_table_mirror_drift_check.py [repo_root]
Exit 0: every id in either table exists in the other, with matching type
        and name.
Exit 1: a mismatch was found (named), or either table could not be located
        at all (fail closed, not a vacuous pass).
"""
import re
import sys
from pathlib import Path

PICO_REL = "firmware/SaftyFW/src/config_params.c"
ESP_REL = "firmware/KilnFW/App/drivers/safety/safety_cfg_store.c"

# Pico side: "{ 0x0104u, KILNLINK_PARAM_TYPE_F32 }, // abs_max_temp_c"
PICO_ROW_RE = re.compile(
    r"\{\s*(0x[0-9A-Fa-f]+)u?\s*,\s*(KILNLINK_PARAM_TYPE_\w+)\s*\}\s*,\s*//\s*([A-Za-z0-9_]+(?:\[\d+\])?(?:\.[A-Za-z0-9_]+)?)"
)

# ESP side: "{ 0x0104, KILNLINK_PARAM_TYPE_F32, \"abs_max_temp_c\" },"
ESP_ROW_RE = re.compile(
    r"\{\s*(0x[0-9A-Fa-f]+)\s*,\s*(KILNLINK_PARAM_TYPE_\w+)\s*,\s*\"([^\"]+)\"\s*\}"
)


def extract_table(text: str, start_marker: str, row_re: "re.Pattern") -> dict:
    start = text.find(start_marker)
    if start < 0:
        return None
    # Table runs until the closing "};" of the array initializer.
    end = text.find("\n};", start)
    if end < 0:
        return None
    body = text[start:end]
    rows = {}
    for m in row_re.finditer(body):
        pid = int(m.group(1), 16)
        rows[pid] = (m.group(2), m.group(3))
    return rows


def main() -> int:
    repo_root = Path(sys.argv[1]) if len(sys.argv) > 1 else Path(__file__).resolve().parents[4]
    pico_path = repo_root / PICO_REL
    esp_path = repo_root / ESP_REL

    if not pico_path.is_file():
        print(f"FAIL: cannot find {PICO_REL} under {repo_root}", file=sys.stderr)
        return 1
    if not esp_path.is_file():
        print(f"FAIL: cannot find {ESP_REL} under {repo_root}", file=sys.stderr)
        return 1

    pico_text = pico_path.read_text(encoding="utf-8")
    esp_text = esp_path.read_text(encoding="utf-8")

    pico_rows = extract_table(pico_text, "static const config_param_id_type_t CONFIG_PARAM_TABLE[]", PICO_ROW_RE)
    esp_rows = extract_table(esp_text, "static const safety_cfg_table_row_t SAFETY_CFG_PARAM_TABLE", ESP_ROW_RE)

    if not pico_rows:
        print(f"FAIL: could not parse CONFIG_PARAM_TABLE out of {PICO_REL} -- "
              "regex found zero rows. Fail closed: either the table moved/was "
              "reformatted (update this script's regex) or something is "
              "genuinely broken.", file=sys.stderr)
        return 1
    if not esp_rows:
        print(f"FAIL: could not parse SAFETY_CFG_PARAM_TABLE out of {ESP_REL} -- "
              "regex found zero rows.", file=sys.stderr)
        return 1

    # Sanity floor: both tables are known (2026-09-10) to hold >= 60 rows.
    # A regex that starts matching only a handful of rows because the table
    # was reformatted must fail loudly, not report a false "they agree".
    if len(pico_rows) < 60 or len(esp_rows) < 60:
        print(f"FAIL: suspiciously small table(s) parsed -- pico={len(pico_rows)} "
              f"esp={len(esp_rows)} rows, expected >= 60 each. Treating this as a "
              "parse failure, not a real drift.", file=sys.stderr)
        return 1

    problems = []

    for pid, (ptype, pname) in sorted(pico_rows.items()):
        if pid not in esp_rows:
            problems.append(
                f"  0x{pid:04X} ({pname}, {ptype}): on the Pico's CONFIG_PARAM_TABLE "
                "but ABSENT from the ESP's SAFETY_CFG_PARAM_TABLE -- this field can "
                "never be set from the web GUI's generic commissioning endpoint "
                "(safety_cfg_store_lookup() will report it unknown and apply_pairs() "
                "refuses it). Producer with no writer.")
            continue
        etype, ename = esp_rows[pid]
        if etype != ptype:
            problems.append(
                f"  0x{pid:04X}: type mismatch -- Pico says {ptype} ({pname}), "
                f"ESP says {etype} ({ename}). A write built against the ESP's type "
                "will be refused at COMMIT_CONFIG (or worse, silently reinterpreted) "
                "on the Pico.")
        if ename != pname:
            problems.append(
                f"  0x{pid:04X}: name mismatch -- Pico calls it {pname!r}, ESP calls "
                f"it {ename!r}. Same id, different name is how a wrong id/type pairing "
                "gets typo'd into an existing slot (config_params.c's own header "
                "comment on this hazard).")

    for pid, (etype, ename) in sorted(esp_rows.items()):
        if pid not in pico_rows:
            problems.append(
                f"  0x{pid:04X} ({ename}, {etype}): on the ESP's SAFETY_CFG_PARAM_TABLE "
                "but ABSENT from the Pico's CONFIG_PARAM_TABLE -- the web GUI's generic "
                "commissioning endpoint will stage and COMMIT this id, the Pico's "
                "config_params_set() switch falls through to `default: return false` "
                "and silently drops it from the record, and confirm_commit_landed() "
                "(which looks the id up in this SAME ESP-side table) can misreport the "
                "write as confirmed. Write with no consumer.")

    if problems:
        print("SAFETY-CFG PARAM TABLE MIRROR DRIFT DETECTED", file=sys.stderr)
        print(f"({PICO_REL} vs {ESP_REL})\n", file=sys.stderr)
        for p in problems:
            print(p, file=sys.stderr)
        return 1

    print(f"OK: {len(pico_rows)} param ids agree (id, type, name) between "
          f"{PICO_REL} and {ESP_REL}.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
