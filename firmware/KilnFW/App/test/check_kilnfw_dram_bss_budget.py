#!/usr/bin/env python3
"""Size of KilnFW's internal-DRAM `.dram0.bss` section, graded out of the built ELF.

WHY THIS EXISTS
---------------
The ESP32-S3 has roughly 241 kB of internal DRAM available as heap after the
static image is laid out, and every byte of `.dram0.bss` (zero-initialised
statics that did NOT opt into PSRAM via `EXT_RAM_BSS_ATTR`) comes straight
out of that heap before `app_main` runs. Several things can ONLY come from
internal DRAM -- `esp_timer_create()`, Wi-Fi/lwIP internals, DMA buffers,
every httpd worker stack -- so `.dram0.bss` growth is a heap regression that
the ordinary "did it build, did it link" gates never see.

2026-09-20, the first bench flash of the 100-profile-slot build (`5f58ba09`,
see docs/audits/dram_bss_profiles_fallback_2026-09-20.md): `.dram0.bss`
grew from 116024 B to 156824 B (+40800 B) in one step. 42416 B of that was
one static, `profiles_http.c`'s `s_profiles_fallback` -- a `profiles_state_t`
whose every `profile_t` had just grown with the slot count -- sitting in
internal `.bss` even though it exists only so a pointer is never NULL and is
never touched on a healthy boot. The board came up with 8447 B of internal
heap free (263 B low-water) and the Wi-Fi `ppTask` aborted on
`esp_timer_create() == ESP_ERR_NO_MEM` inside `phy_track_pll_init()`. Fixed
by placing that one static in PSRAM `.bss` (`EXT_RAM_BSS_ATTR`), which took
`.dram0.bss` to 114408 B; this check exists so the next such static is caught
at build time rather than by a panic on the bench.

WHAT IT MEASURES
----------------
`objdump -h` on the ELF, the `.dram0.bss` section's size in bytes, compared
to CEILING_BYTES. That is the whole measurement -- no call graph, no
heuristics. `.ext_ram.bss` (PSRAM) is reported alongside for context but is
NOT graded: PSRAM is 8 MB and moving a static there is exactly the fix this
check wants to encourage.

CEILING, not a percentage
-------------------------
CEILING_BYTES is a ceiling on the current worst case plus a small allowance,
same convention as check_httpd_task_stack_budget.py: it exists to catch the
section getting materially WORSE, not to be relitigated every time a few
hundred bytes of ordinary state land. Raising it is allowed, but the change
that raises it has to say what the new bytes are and why they cannot live in
PSRAM -- and has to re-check the live board's `heap_internal` low-water
against dram_margin.h's KILN_DRAM_FREE_ALARM_BYTES first.

LIMITS, stated honestly
-----------------------
  * `.dram0.bss` is only the zero-initialised statics. `.dram0.data`
    (initialised statics) and the heap allocations made at boot are not
    graded here; the live `get_heap_status` measurement remains the
    authority for "is the board actually healthy right now".
  * A 0-byte or partially-written ELF (a concurrent build mid-link) makes
    objdump fail; that is reported as SKIP (exit 3), never as a pass.

Exit codes: 0 pass, 1 FAIL, 3 SKIP (no ELF / no objdump / unparsable ELF).
"""

import argparse
import os
import re
import subprocess
import sys

sys.path.insert(0, os.path.dirname(__file__))
import check_main_task_stack_budget as base  # noqa: E402  (REPO_ROOT, DEFAULT_ELF, find_objdump)

REPO_ROOT = base.REPO_ROOT
DEFAULT_ELF = base.DEFAULT_ELF

# 2026-09-21: measured 95272 B after also moving s_routes (http_auth_http.c)
# to EXT_RAM_BSS_ATTR (114408 B before that move, after the 2026-09-20
# s_profiles_fallback fix; 116024 B on the previous flashed build, 73c1da94,
# before the 100-slot change). 101000 leaves ~5.7 kB for ordinary growth
# while still refusing anything on the order of the jumps this check was
# written for. See module docstring before raising.
#
# 2026-09-30: ordinary growth since 2026-09-21 had already brought this to
# 99672 B (within the 101000 B ceiling). The same day, a bench run
# (logs/bench_test/20261001T011515Z_stack, firmware c471101c) found
# `lvgl`'s task LOW at 1296 B free of 8192 B (15.8%) -- raised to 10240 B
# (lvgl_port.c's s_lvgl_task_stack) for ~32.7% free at the same measured
# worst-case usage. This stack cannot move to PSRAM: it is explicitly, by
# design, internal SRAM only (see lvgl_port.c's own comment on
# s_lvgl_task_stack and ui_page_edit_firing.c's apply_cb() -- settings/
# profile pages write NVS from this task, and a PSRAM-stacked task must
# never write NVS), so the +2048 B initially landed here, taking
# .dram0.bss to 101720 B against the 101000 B ceiling -- live board
# `heap_internal` headroom (13523 B min_free, c471101c) made that a real
# risk against dram_margin.h's KILN_DRAM_FREE_ALARM_BYTES (11903 B), not
# just a ceiling number. Fix: adaptive_tune_zones[] (2700 B,
# App/drivers/control/adaptive_tune.c) moved to PSRAM via
# EXT_RAM_BSS_ATTR -- every access to it (zone_tick/run_end, init/
# load_enable_flags, set_enabled/clear_ki_baseline, the getters including
# the HTTP status read, revert, the model/Ki-diagnosis helpers, and the
# autotune accept path; not a closed list) runs in ordinary task context
# under adaptive_tune_lock, never an ISR or IRAM code, and every flash/NVS/
# cfg persistence path copies through a separate local or job blob rather
# than passing the array itself as a buffer, so it is never a DMA target and
# never touched with the flash cache disabled. That move more than pays for
# lvgl's +2048 B, bringing
# .dram0.bss back down to a measured 99016 B -- back under the original
# 101000 B ceiling (1984 B headroom), which is restored here rather than
# left raised.
CEILING_BYTES = 101000

GRADED_SECTION = ".dram0.bss"
CONTEXT_SECTIONS = (".dram0.data", ".ext_ram.bss")

# objdump -h row: "Idx Name Size VMA LMA File off Algn"
SECTION_ROW_RE = re.compile(r"^\s*\d+\s+(\S+)\s+([0-9a-fA-F]{8})\s+([0-9a-fA-F]{8})")


def section_sizes(objdump, elf):
    """{section_name: size_bytes} from `objdump -h`. Raises base.ElfParseError."""
    try:
        out = subprocess.run([objdump, "-h", elf], capture_output=True, text=True,
                             check=False)
    except OSError as e:
        raise base.ElfParseError(f"could not run {objdump}: {e}")
    if out.returncode != 0 or not out.stdout.strip():
        raise base.ElfParseError(
            f"objdump -h failed on {elf} (rc={out.returncode}): "
            f"{(out.stderr or '').strip()[:200] or 'no output'} -- 0-byte or mid-write ELF?")
    sizes = {}
    for line in out.stdout.splitlines():
        m = SECTION_ROW_RE.match(line)
        if m:
            sizes[m.group(1)] = int(m.group(2), 16)
    if not sizes:
        raise base.ElfParseError(f"objdump -h printed no section rows for {elf}")
    return sizes


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--elf", default=DEFAULT_ELF)
    ap.add_argument("--ceiling-bytes", type=int, default=None,
                    help="override CEILING_BYTES (used by the negative test)")
    args = ap.parse_args()
    ceiling = args.ceiling_bytes if args.ceiling_bytes else CEILING_BYTES
    name = "check_kilnfw_dram_bss_budget"

    if not os.path.isfile(args.elf):
        print(f"{name}: SKIP: no ELF at {args.elf}.")
        print("  Build KilnFW (build_kilnfw / idf.py build) and re-run; unmeasured, not passing.")
        return 3

    objdump = base.find_objdump()
    if not objdump:
        print(f"{name}: SKIP: xtensa-esp32s3-elf-objdump not found (set XTENSA_OBJDUMP). "
              "Unmeasured, not passing.")
        return 3

    try:
        sizes = section_sizes(objdump, args.elf)
    except base.ElfParseError as e:
        print(f"{name}: SKIP: {e}")
        return 3

    if GRADED_SECTION not in sizes:
        print(f"{name}: FAIL -- {GRADED_SECTION} section not present in {args.elf}; "
              f"sections seen: {', '.join(sorted(sizes))}")
        return 1

    bss = sizes[GRADED_SECTION]
    ctx = "  ".join(f"{s}={sizes[s]}" for s in CONTEXT_SECTIONS if s in sizes)
    print(f"{name}: {GRADED_SECTION} = {bss} B, ceiling {ceiling} B "
          f"(headroom {ceiling - bss} B). Context: {ctx}")
    print(f"  ELF: {args.elf}")
    if bss > ceiling:
        print(f"{name}: FAIL -- {GRADED_SECTION} is {bss} B, over the {ceiling} B ceiling by "
              f"{bss - ceiling} B. Internal DRAM heap shrinks by exactly this much on every boot.")
        print("  Find the new static(s): xtensa-esp-elf-nm -S --size-sort <elf> | grep ' [bB] ' | tail")
        print("  Prefer EXT_RAM_BSS_ATTR (PSRAM .bss) for anything large that is not touched by an")
        print("  ISR, DMA, or a flash-write path. Raise CEILING_BYTES only per the module docstring.")
        return 1
    print(f"{name}: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
