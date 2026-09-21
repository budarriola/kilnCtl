# Internal-DRAM exhaustion from `s_profiles_fallback` after the 100-slot change (2026-09-20)

**Status:** root-caused and fixed in source (this commit); the bench board was
running the affected build (`5f58ba09`) when this was written and is reflashed
as part of the same commissioning pass.

## Symptom

First bench flash of `5f58ba09` (the build carrying `PROFILES_MAX_COUNT = 100`,
docs/PROFILE_SLOTS_100_PLAN.md). The ESP booted, joined Wi-Fi, served HTTP, then
panicked once within the first minutes and rebooted. The crash record the board
kept (`GET /api/crash_report`) was `exc_task: "wifi"`, `IllegalInstruction`,
`exc_pc 0xfffffffd`, `backtrace_corrupted: true` -- useless on its own. The
full coredump was fetched over HTTP (746400 B, sha256 `c3ae7301...`, archived
as `firmware/KilnFW/coredump_archive/coredump-c3ae73013c78.bin` with its
provenance `.json`) and symbolized against `elf_archive/KilnCtrl-f3efd5d89f49.elf`,
the ELF that was actually flashed.

The symbolized crashing task was the Wi-Fi `ppTask`: `pm_wake_up` ->
`phy_track_pll_init` -> `ESP_ERROR_CHECK(esp_timer_create(...))` -> `abort()`
with `ESP_ERR_NO_MEM` (`0x101`). `esp_timer_create()` allocates its handle
from internal DRAM only.

After the reboot the board stayed up, but `get_heap_status` showed internal
DRAM at 8447 B free / 7680 B largest block / **263 B minimum-ever free** --
below both `KILN_DRAM_FREE_ALARM_BYTES` (11903) and
`KILN_DRAM_LARGEST_ALARM_BYTES` (8704) in `dram_margin.h`, whose documented
healthy bench trough was ~11415 B free. Whether the next Wi-Fi power-management
wake found a free 40-byte block was luck.

## Root cause

`.dram0.bss` grew from 116024 B (`73c1da94`, previous flashed build) to
156824 B (`5f58ba09`): +40800 B of zero-initialised statics in internal DRAM,
taken from the heap before `app_main` runs.

`xtensa-esp-elf-nm -S --size-sort` on the two ELFs put 42416 B of that in one
symbol:

```
3fcb7c88 0000a5b0 b s_profiles_fallback$11        (5f58ba09, internal .dram0.bss)
```

`profiles_http.c`, `profiles_storage_ensure()`:

```c
static profiles_state_t s_profiles_fallback;
```

It exists only so `profiles_storage_ensure()` can hand back a never-NULL
pointer if the real (heap/PSRAM) state was never allocated. It is never
touched on a healthy boot. `sizeof(profiles_state_t)` scales with
`PROFILES_MAX_COUNT * sizeof(profile_t)`, so the 10-to-100 slot change grew
this one never-used instance by ~38 kB, silently, in the tightest memory
region on the chip. Nothing in the 100-slot plan's review looked at
`.dram0.bss`: every existing budget check grades task stacks or heap use at
runtime, not the static image.

## Fix

```c
#include "esp_attr.h"
...
        static EXT_RAM_BSS_ATTR profiles_state_t s_profiles_fallback;
```

`CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY=y` is already set, so
`EXT_RAM_BSS_ATTR` places the static in `.ext_ram.bss`, which is mapped and
zeroed before `app_main`. The never-NULL guarantee is unchanged; the internal
DRAM cost is zero.

Measured on the fixed ELF:

| section | 73c1da94 | 5f58ba09 | fixed |
|---|---|---|---|
| `.dram0.bss` | 116024 | 156824 | 114408 |
| `.ext_ram.bss` | -- | 40708 | 83124 |

`s_profiles_fallback$11` now sits at `0x3c283bb8` (PSRAM).

## Guard added

`firmware/KilnFW/App/test/check_kilnfw_dram_bss_budget.{ps1,py}` grades the
built ELF's `.dram0.bss` size against a 120000 B ceiling (`objdump -h`, no
heuristics), SKIPs (exit 3) on a missing, 0-byte or unparsable ELF, and FAILs
naming the overage with the `nm --size-sort` recipe to find the culprit.

Negative tests run before landing:

- against `elf_archive/KilnCtrl-f3efd5d89f49.elf` (the `5f58ba09` image):
  FAIL, `156824 B, over the 120000 B ceiling by 36824 B`, exit 1.
- against the fixed ELF with `-CeilingBytes 1000`: FAIL, exit 1.
- missing path and a 0-byte file: SKIP, exit 3 (never 0).
- fixed ELF at the default ceiling: PASS, headroom 5592 B.

## Why the pre-flash tooling did not catch it

- `check_recovery_image_size.ps1` and the app-vs-partition size check grade
  flash footprint. `.bss` occupies no flash, so a 40 kB `.bss` jump is
  invisible to both.
- The stack-budget checks walk call graphs out of the ELF; they never look
  at data sections.
- `dram_margin.h`'s alarms are runtime, on-board, and only fire once the
  board is already in the state this incident produced.

## Lessons

- Any struct whose size is a function of `PROFILES_MAX_COUNT`,
  `PROFILE_MAX_SEGMENTS` or another capacity constant must not be a plain
  `static` in internal DRAM. Check `nm --size-sort` on the ELF whenever a
  capacity constant changes.
- A "fallback so the pointer is never NULL" static is the classic shape for
  this: it is by construction never exercised, so no test measures its cost.
- The crash record's `IllegalInstruction` / `0xfffffffd` framing was a red
  herring, as it was for the 2026-09-04 `safety_poll` panic: an
  `ESP_ERROR_CHECK` abort inside a Wi-Fi library task presents this way. Fetch
  the real coredump and symbolize against the archived ELF for the running
  build before reasoning from the summary fields.
