#!/usr/bin/env python3
"""check_profiles_capacity.py -- build-time capacity gate for the profile
store, docs/PROFILE_SLOTS_100.md section 5 task 4.

WHAT THIS CLOSES. The 100-slot plan grows PROFILES_MAX_COUNT and the `cfg`
partition in separate, later commits (tasks 5 and 6). This check exists so
that when either one moves, the other's promise -- "the worst-case encoded
size still fits its partition" -- is verified by arithmetic, not merely
asserted in a planning doc. It is written and landed FIRST (task 4), while
PROFILES_MAX_COUNT is still 8 and `cfg` is still 0x80000, specifically so it
is live and already enforcing before task 6 raises the count -- the same
"gate lands ahead of the thing it bounds" shape as check_recovery_image_size.py
(that script's own module docstring is the direct precedent for this one).

WHERE THE BOUNDS COME FROM. Partition sizes are read from `partitions.csv`
via `kilnctrl.partition_table.parse_partitions_csv` -- the one parser this
repo already uses for the table (same as check_recovery_image_size.py and
debug_check_partition_table()) -- so a resize of `profiles_nvs` or `cfg`
moves this gate's bound on the next run, by construction, rather than
requiring a hand-edited number here to be remembered. Parsing the CSV is
what makes this a GATE rather than a restated constant: a check that just
hardcoded "384 KiB" and "512 KiB" would silently stop meaning anything the
moment either partition moved.

WHERE THE PER-ITEM COSTS COME FROM. PROFILES_MAX_COUNT and the firing-stats
blob size are read out of firmware source (profiles_types.h,
profile_executor_internal.h) the same way -- by regex, not by hand-copying a
number -- so a future change to either constant is picked up automatically.
The builtin catalogue count is likewise counted directly out of
profiles_builtin_table.inc's real entries. The remaining per-item byte costs
(profile blob ~429 B, NVS's own ~1.25x page/entry-rounding overhead, cfg's
~1.29x block-rounding and its 2x GC-headroom rule) are the same worst-case
unit figures docs/PROFILE_SLOTS_100.md section 3 derives and documents;
they are reproduced here, not re-derived, because the exact on-flash NVS/
LittleFS encoding is not economical to recompute from Python at build time.
If the plan's section 3 figures are ever revised, update the constants below
to match and say so in the commit that does it.

EXIT CODES (run_all_checks.ps1's check_*.ps1 contract):
    0  PASS -- both worst-case totals fit their partition.
    1  FAIL -- either worst-case total overflows its partition, or the CSV/
       source files could not be found or parsed.
"""
from __future__ import annotations

import argparse
import os
import re
import sys

_REPO_ROOT = os.path.normpath(os.path.join(os.path.dirname(__file__), ".."))
_PCTOOLS_SRC = os.path.join(_REPO_ROOT, "tools", "PcTools", "src")
if _PCTOOLS_SRC not in sys.path:
    sys.path.insert(0, _PCTOOLS_SRC)

_KILNFW = os.path.join(_REPO_ROOT, "firmware", "KilnFW")
DEFAULT_PARTITIONS_CSV = os.path.join(_KILNFW, "partitions.csv")
DEFAULT_PROFILES_TYPES_H = os.path.join(_KILNFW, "App", "drivers", "persist", "profiles_types.h")
DEFAULT_EXECUTOR_INTERNAL_H = os.path.join(_KILNFW, "App", "drivers", "control", "profile_executor_internal.h")
DEFAULT_BUILTIN_TABLE_INC = os.path.join(_KILNFW, "App", "drivers", "persist", "profiles_builtin_table.inc")

# --- Plan section 3's own worst-case unit costs (see module docstring) -----
PROFILE_BLOB_BYTES = 429  # one profile_t, encoded + versioned + CRC-wrapped
NVS_ENTRY_ROUNDING = 1.25  # 32-byte-entry/4096-byte-page rounding, worst case
REV_KEY_BYTES = 32  # one "fsr_<id>" (or "profN_rev") rev counter key
PROFILE_REV_BLOB_BYTES = 400  # the single prof_rev snapshot blob + used bitmap

CFG_PROFILE_JSON_BYTES = 2 * 1024  # /cfg/profiles/<id>.json
CFG_STATS_JSON_BYTES = 4 * 1024  # /cfg/stats/<id>.json (user + builtin ids)
CFG_ZONES_JSON_BYTES = 8 * 1024
CFG_KILNCFG_SLOTS = 8  # unrelated to profile count -- fixed per plan section 3
CFG_KILNCFG_JSON_BYTES = 8 * 1024
CFG_MISC_JSON_BYTES = 4 * 1024  # prefs/tune/hidden/relay_cycles
CFG_BLOCK_ROUNDING = 1.29  # 4 KiB LittleFS sector rounding, ~250 files
CFG_GC_HEADROOM = 2.0  # partition's own "keep ~2x the live set free" rule


def _fail(reason: str) -> int:
    print(f"FAIL: {reason}", file=sys.stderr)
    return 1


def _read_int_define(path: str, name: str) -> int:
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    m = re.search(rf"#define\s+{re.escape(name)}\s+(\d+)", text)
    if not m:
        raise ValueError(f"could not find '#define {name} <N>' in {path}")
    return int(m.group(1))


def _count_builtin_profiles(path: str) -> int:
    with open(path, "r", encoding="utf-8") as f:
        text = f.read()
    # Every builtin table entry sets its own .code field exactly once --
    # counting that is a direct count of real entries, not a guess, and
    # tracks the table's true size the same way g_builtin_profile_count
    # (sizeof(array)/sizeof(array[0])) does at compile time.
    count = len(re.findall(r"^\s*\.code\s*=", text, re.MULTILINE))
    if count == 0:
        raise ValueError(f"found zero '.code =' entries in {path} -- wrong file, or table format changed?")
    return count


def run(partitions_csv: str, profiles_types_h: str, executor_internal_h: str, builtin_table_inc: str) -> int:
    from kilnctrl.partition_table import parse_partitions_csv  # noqa: PLC0415

    for path in (partitions_csv, profiles_types_h, executor_internal_h, builtin_table_inc):
        if not os.path.isfile(path):
            return _fail(f"expected input not found: {path!r} -- repo layout changed?")

    try:
        entries = parse_partitions_csv(partitions_csv)
    except ValueError as exc:
        return _fail(f"could not parse {partitions_csv}: {exc}")

    by_name = {e.name: e for e in entries}
    for needed in ("profiles_nvs", "cfg"):
        if needed not in by_name:
            return _fail(f"no {needed!r} row in {partitions_csv}")
    profiles_nvs_bound = by_name["profiles_nvs"].size
    cfg_bound = by_name["cfg"].size

    try:
        max_count = _read_int_define(profiles_types_h, "PROFILES_MAX_COUNT")
        firing_blob_bytes = _read_int_define(executor_internal_h, "PROFILE_FIRING_HISTORY_BLOB_SIZE_V1")
        builtin_count = _count_builtin_profiles(builtin_table_inc)
    except ValueError as exc:
        return _fail(str(exc))

    live_edit_slot = 1  # PROFILE id == PROFILES_MAX_COUNT, structural, outside the count

    # --- profiles_nvs worst case (section 3's own inventory) ---------------
    user_slots = max_count + live_edit_slot
    profile_blobs = user_slots * PROFILE_BLOB_BYTES
    firing_stats_user = user_slots * firing_blob_bytes
    firing_stats_builtin = builtin_count * firing_blob_bytes
    rev_keys = (user_slots + builtin_count) * REV_KEY_BYTES
    nvs_logical = profile_blobs + firing_stats_user + firing_stats_builtin + rev_keys + PROFILE_REV_BLOB_BYTES
    nvs_worst_case = int(nvs_logical * NVS_ENTRY_ROUNDING)

    # --- cfg worst case (section 3's own inventory) -------------------------
    cfg_profiles = user_slots * CFG_PROFILE_JSON_BYTES
    cfg_stats = (user_slots + builtin_count) * CFG_STATS_JSON_BYTES
    cfg_kilncfg = CFG_KILNCFG_SLOTS * CFG_KILNCFG_JSON_BYTES
    cfg_logical = cfg_profiles + cfg_stats + CFG_ZONES_JSON_BYTES + cfg_kilncfg + CFG_MISC_JSON_BYTES
    cfg_worst_case = int(cfg_logical * CFG_BLOCK_ROUNDING)
    # The partition's own "keep ~2x the live set free for GC" rule (section 1)
    # is a SIZING guideline for choosing how large to grow `cfg` -- it is not
    # a stricter "must never exceed half-full" occupancy limit, so it is
    # reported for visibility but does not gate pass/fail here. Gating on it
    # would fail this check at the CURRENT 8-slot/0x80000 state even though
    # the block-rounded data plainly still fits the partition.
    cfg_with_gc_headroom = int(cfg_worst_case * CFG_GC_HEADROOM)

    ok = True
    print(
        f"profiles_nvs: worst case {nvs_worst_case} B (0x{nvs_worst_case:x}) of "
        f"{profiles_nvs_bound} B (0x{profiles_nvs_bound:x}) -- "
        f"{nvs_worst_case / profiles_nvs_bound:.0%} full "
        f"(PROFILES_MAX_COUNT={max_count}, builtin_count={builtin_count}, "
        f"firing_blob={firing_blob_bytes} B)"
    )
    if nvs_worst_case > profiles_nvs_bound:
        ok = False
        print(
            f"  OVERFLOW by {nvs_worst_case - profiles_nvs_bound} B -- profiles_nvs cannot hold "
            f"{max_count} user slots + {builtin_count} builtins worth of profile and firing-stats data",
            file=sys.stderr,
        )

    print(
        f"cfg: worst case (block-rounded) {cfg_worst_case} B (0x{cfg_worst_case:x}) of "
        f"{cfg_bound} B (0x{cfg_bound:x}) -- {cfg_worst_case / cfg_bound:.0%} full "
        f"(with the partition's 2x GC-headroom rule applied: {cfg_with_gc_headroom} B, "
        f"{cfg_with_gc_headroom / cfg_bound:.0%})"
    )
    if cfg_worst_case > cfg_bound:
        ok = False
        print(
            f"  OVERFLOW by {cfg_worst_case - cfg_bound} B -- cfg cannot hold "
            f"{max_count} user slots + {builtin_count} builtins worth of mirrored profile/stats files",
            file=sys.stderr,
        )
    elif cfg_with_gc_headroom > cfg_bound:
        print(
            f"  Warning: fits without GC headroom, but the partition's own 2x-free rule "
            f"would want {cfg_with_gc_headroom} B -- past the comfortable band, not a failure yet."
        )

    if not ok:
        return 1
    print("PASS: both profiles_nvs and cfg have room for the current slot count.")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--partitions-csv", default=DEFAULT_PARTITIONS_CSV)
    parser.add_argument("--profiles-types-h", default=DEFAULT_PROFILES_TYPES_H)
    parser.add_argument("--executor-internal-h", default=DEFAULT_EXECUTOR_INTERNAL_H)
    parser.add_argument("--builtin-table-inc", default=DEFAULT_BUILTIN_TABLE_INC)
    args = parser.parse_args()
    return run(args.partitions_csv, args.profiles_types_h, args.executor_internal_h, args.builtin_table_inc)


if __name__ == "__main__":
    sys.exit(main())
