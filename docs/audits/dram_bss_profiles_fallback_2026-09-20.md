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

## Follow-up 2026-09-21

Moved `s_routes` (`firmware/KilnFW/App/drivers/http/http_auth_http.c:57`, the
HTTP route-dispatch metadata table, `kiln_http_route_ctx_t[KILN_HTTP_MAX_ROUTES]`,
192 * 100 B) to `EXT_RAM_BSS_ATTR` (PSRAM), same fix shape as
`s_profiles_fallback` above. An `nm --size-sort` diff between
`elf_archive/KilnCtrl-1204ef14664b.elf` (`d459d124`, 2026-09-16) and
`da37ffa2` showed +28769 B of internal `.bss` growth over that span, of
which `s_routes` accounted for 19200 B. Moving it to PSRAM raises total
internal DRAM free by that ~19 kB deterministically; the effect on the idle
largest-free-block figure is a runtime property of `get_heap_status` and
must be read back after flashing, not assumed from this static measurement.

Safety review before moving it: confirmed every access is from
`kiln_http_register()` (HTTP server bringup, called from application code
after `app_main` -- PSRAM is mapped by then) and `kiln_http_prehandler()` (an
ordinary `esp_http_server` task callback on request dispatch). No
`IRAM_ATTR`, no ISR or DMA context, no `spi_flash`/flash-cache-disabled
section, and no pointer into the table is ever handed to a flash write.

Measured on the built ELF (`objdump -h`, `.dram0.bss`): 114408 B before
(`da37ffa2`) -> 95272 B after this change, a reduction of 19136 B --
consistent with the table's 19200 B internal-DRAM footprint (a few dozen
bytes of residual variance from other build-generated symbols).
`tools/run_all_checks.ps1 -AllowFewerChecks` in an isolated worktree: 124
passed, 0 skipped, 0 failed, including
`check_kilnfw_dram_bss_budget.ps1` and the three full target builds.
`check_kilnfw_dram_bss_budget.py`'s ceiling was lowered from 120000 B to
101000 B to reflect the new, lower baseline.

Left for a later pass, both still in internal `.dram0.bss` and both smaller
than `s_routes` was:

- `s_bulk_pairs` (`firmware/KilnFW/App/drivers/safety/safety_cfg_write.c:683`, 1846 B)
- `s_scan_buf` (`firmware/KilnFW/App/drivers/net/pico_image_source.c:30`, 1080 B)
