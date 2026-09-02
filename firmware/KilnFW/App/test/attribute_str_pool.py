#!/usr/bin/env python3
"""Attribute the merged `.str1.1` string-literal pool to source object files.

FLASH_BUDGET_PLAN.md section 4.1 background: `esp_idf_size --archive_details
libesp_stdio.a` reports 239,329 B under a single symbol,
`.rodata.console_access.str1.1`. That is NOT esp_stdio's own data -- `.str1.1`
is GCC's mergeable string-literal section, and the linker pools every such
input section from every object file into one output section, naming the
whole pool after whichever input section happened to be placed first
(`console_access` in `stdio_vfs.c.obj`, alphabetically/link-order first, not
biggest). The 239 kB is a sum over the entire firmware's string literals
(predominantly ESP_LOG* format strings and tags), not a cost owned by
esp_stdio.

This script attributes that pool back to the *input* object files, so the
real owners are known by name and size, using the ground truth available
before ld performs the merge: the "Linker script and memory map" section of
KilnCtrl.map lists every input `*.str1.1` section with its own size, in the
form

    .rodata.<symbol>.str1.1
                    0x00000000       0x39 esp-idf/efuse/libefuse.a(esp_efuse_api.c.obj)

(address 0x0 because ld folds these into the output section rather than
placing them individually; exactly one entry -- the pool's first contributor
-- shows the real placed address instead of 0, and is included in the sum
like any other). Summing per-object gives the RAW, PRE-DEDUP contribution of
each object to the pool.

DEDUP CAVEAT (read before trusting the totals): GCC's mergeable-string
sections let the linker coalesce byte-identical strings across *different*
object files into one copy in the final output. This script's per-object sums
are computed BEFORE that coalescing -- ld's map file does not record which
input bytes survived into the merged output, only what each object
contributed on the way in. So:

    sum(per-object raw sizes) >= 239,329 B (the actual merged pool size)

The gap between the two is the volume of literal text duplicated across
object files (identical ESP_LOG tags, repeated format strings, etc.) that ld
folded away. This script reports both numbers and the gap explicitly; it does
NOT claim reconciliation it cannot prove, and does not attempt real string
interning across objects (that would require reading string content out of
each .o's section bytes via objdump/readelf, not just sizes out of the map --
out of scope for this measurement pass).

Usage:
    python attribute_str_pool.py [path/to/KilnCtrl.map]

Exits non-zero (and prints a specific diagnostic) if:
  - the map file does not exist,
  - no .str1.1 entries are found at all (map format changed / wrong file),
  - the merged pool total (.rodata.console_access.str1.1's own placed size)
    cannot be located.
"""
import re
import sys
from pathlib import Path
from collections import defaultdict

SECTION_RE = re.compile(r'^\s*\.rodata\.\S+\.str1\.(1|4)$')
ENTRY_RE = re.compile(r'^\s*0x([0-9a-fA-F]+)\s+0x([0-9a-fA-F]+)\s+(\S+)\s*$')
RELAX_RE = re.compile(r'^\s*0x([0-9a-fA-F]+)\s+\(size before relaxing\)\s*$')
MERGED_POOL_NAME = ".rodata.console_access.str1.1"


def parse_map(map_path: Path):
    """Return (per_object_totals, per_object_counts, merged_pool_size, total_entries).

    IMPORTANT: for a `.str1.*` (mergeable string) input section, the "size"
    printed directly under its address is NOT that section's own original
    size once ld has folded it into a merged output section -- it is that
    section's placement within the *merged* output (0 for everything except
    the section that anchors the output, which instead shows the WHOLE
    pool's final size). ld separately prints each such section's true
    pre-merge size on a trailing "(size before relaxing)" line when merging
    changed it. This shows up on the large majority of `.str1.1` entries in
    this build (1,361 of 1,923 measured against KilnCtrl.map, 2026-09-01),
    not just the pool anchor -- so it must be handled generally, not as a
    special case for the one anchor line. Where present, that pre-relax
    value is the object's true raw contribution; where absent, ld did not
    fold that section and the displayed size already IS the original.
    """
    lines = map_path.read_text(errors="replace").splitlines()

    per_object = defaultdict(int)
    per_object_count = defaultdict(int)
    merged_pool_size = None
    total_entries = 0

    i = 0
    n = len(lines)
    while i < n:
        line = lines[i]
        if SECTION_RE.match(line):
            # The (addr, size, objfile) triple is usually on the next line,
            # but ld wraps to the next line first when the section name is
            # too long to share a line with the address column.
            j = i + 1
            entry_line = lines[j] if j < n else ""
            m = ENTRY_RE.match(entry_line)
            if m:
                addr_hex, size_hex, objfile = m.groups()
                placed_size = int(size_hex, 16)

                # Record the merged pool's own linked/deduped total from the
                # placed size of its anchor entry -- this is unaffected by
                # the relaxing correction below (it IS the final size).
                if line.strip() == MERGED_POOL_NAME:
                    merged_pool_size = placed_size

                # Prefer the true pre-merge size when ld recorded one.
                raw_size = placed_size
                k = j + 1
                if k < n:
                    rm = RELAX_RE.match(lines[k])
                    if rm:
                        raw_size = int(rm.group(1), 16)

                # Collapse the archive-member path to "archive.a(object.c.obj)"
                # -- drop any absolute toolchain/IDF prefix so entries from
                # this project's own code (esp-idf/App/libApp.a(...),
                # esp-idf/drivers/libdrivers.a(...)) read the same as vendor
                # entries.
                key = objfile.replace("\\", "/")
                m2 = re.search(r'([^/]+\.a)\(([^)]+)\)$', key)
                if m2:
                    key = f"{m2.group(1)}({m2.group(2)})"
                per_object[key] += raw_size
                per_object_count[key] += 1
                total_entries += 1
                i = j + 1
                continue
        i += 1

    return per_object, per_object_count, merged_pool_size, total_entries


def main():
    map_path = Path(sys.argv[1]) if len(sys.argv) > 1 else \
        Path(__file__).resolve().parents[2] / "build" / "KilnCtrl.map"

    if not map_path.is_file():
        print(f"ERROR: map file not found: {map_path}", file=sys.stderr)
        print("Build KilnFW first (build_kilnfw) so build/KilnCtrl.map exists.",
              file=sys.stderr)
        return 2

    per_object, per_object_count, merged_pool_size, total_entries = parse_map(map_path)

    if total_entries == 0:
        print("ERROR: found zero .str1.1 input-section entries in the map file.",
              file=sys.stderr)
        print("Either the map file format changed, or this is the wrong file.",
              file=sys.stderr)
        return 3

    if merged_pool_size is None:
        print(f"ERROR: could not locate the merged pool entry "
              f"({MERGED_POOL_NAME}) in the map file.", file=sys.stderr)
        print("The pool may have been renamed by a link-order change; this "
              "script's dedup-gap math depends on finding it.", file=sys.stderr)
        return 4

    raw_total = sum(per_object.values())
    gap = raw_total - merged_pool_size

    ranked = sorted(per_object.items(), key=lambda kv: kv[1], reverse=True)

    print(f"Map file: {map_path}")
    print(f"Merged .str1.1 pool (linked, deduped) size: {merged_pool_size:,} B "
          f"(reported by name as {MERGED_POOL_NAME})")
    print(f"Sum of RAW per-object .str1.1 contributions (pre-dedup): {raw_total:,} B "
          f"across {total_entries} input sections, {len(per_object)} objects")
    print(f"Dedup gap (raw sum - merged pool): {gap:,} B "
          f"({100.0 * gap / raw_total:.1f}% of the raw sum was duplicate text "
          f"folded away by the linker)")
    print()
    print(f"{'rank':>4}  {'raw bytes':>10}  {'entries':>7}  object")
    print(f"{'----':>4}  {'---------':>10}  {'-------':>7}  ------")
    for rank, (obj, size) in enumerate(ranked, 1):
        print(f"{rank:>4}  {size:>10,}  {per_object_count[obj]:>7}  {obj}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
