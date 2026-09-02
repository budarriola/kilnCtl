# DRAM / PSRAM Plan — reclaiming internal SRAM on the ESP32-S3

Plan doc for moving what can be moved out of internal DRAM and into the 8 MB
octal PSRAM on the N16R8 module, so that internal SRAM stops being the binding
constraint on this firmware.

Conventions this doc follows, matching `PID_EXPANSION_PLAN.md`: **the code is
truth, not the checkboxes.** Nothing below is marked done until a commit is
named. Every number is either measured and attributed, or explicitly labelled
as an estimate.

Status at time of writing (2026-09-01): **nothing in this plan is built.** This
is a planning document only.

**Update 2026-09-02 — one large item landed, incidentally.** `4c0d703` moved the
profile-history ring out of `s_exec`'s inline `.bss` into PSRAM
(`heap_caps_malloc(MALLOC_CAP_SPIRAM)`, allocated lazily on first run) while
widening history to per-zone data for the web graph. Verified in `KilnCtrl.map`:
`s_exec`'s `.bss` went from ~23.5 kB to `0x9E0` (2528 bytes), a **~21 kB
internal-DRAM saving** — so a feature that would naively have *cost* ~46 kB
instead freed 21 kB. Measured on the live board before that change:
`heap_internal.free 16847`, `largest_free_block 7680`, `min_free 8875`,
against `heap_spiram.free 8073848` of 8388608 — i.e. internal DRAM was under the
11.9 kB line while 8 MB of PSRAM sat unused. Post-fix `min_free` is *estimated*
at ~30–32 kB from static map analysis; **not yet measured on hardware.**

Two cautions for whoever continues this plan:
- The largest remaining internal `.bss` consumers are `s_at` (10416 B, autotune
  engine) and `s_store` (~7 kB, `kiln_cfg_store`) — both app-owned and
  theoretically movable. But `g_cnxMgr` (3944 B) and `gWpaSm` (852 B) are
  ESP-IDF Wi-Fi/WPA state that **must stay internal**: Wi-Fi DMA descriptors are
  not PSRAM-reachable on this chip. Roughly 67 kB of the 101 kB `.dram0.bss` is
  library object code and is not movable at all.
- **Internal-DRAM pressure was NOT the cause of the httpd `accept(23)` wedge**,
  despite looking exactly like it. That was an uncounted permanent socket
  disabling `lru_purge_enable`'s recovery path (`a5567ae`); ENFILE comes from a
  fixed static socket array with no heap on the path, while a genuine memory
  failure would report ENOMEM. Do not cite that wedge as justification for
  DRAM work — the DRAM problem is real, but it is a separate one.

---

## 1. Why this exists, and what it is *not* about

The motivating symptom is internal-DRAM exhaustion: the board has been observed
down to ~11.9 kB free internal heap, at which point `esp_http_server` starts
resetting sockets and pages fail to load mid-transfer.

**This plan is scoped to internal-DRAM reclamation only.** Two adjacent ideas
were considered and deliberately excluded:

- **Moving the embedded web assets into PSRAM.** Rejected — it makes things
  worse, not better. The pages are embedded via `EMBED_TXTFILES` and served
  with `httpd_resp_send(req, _binary_..._start, len)` straight out of
  memory-mapped flash (see `App/drivers/diagnostics_http.c`). They occupy
  **zero DRAM and zero PSRAM today.** Relocating them would spend ~266 kB of
  PSRAM plus a boot-time copy in order to obtain the behaviour that is already
  in place. The DRAM pressure comes from task stacks, lwIP/Wi-Fi buffers and
  httpd working memory — never from the page bytes.
- **`CONFIG_SPIRAM_XIP_FROM_PSRAM`.** Currently `is not set`, and it should
  stay that way for now. It moves rodata and instruction fetch to PSRAM, which
  on octal PSRAM at 40 MHz is at best a wash against the flash cache, and it is
  a large, hard-to-attribute performance change. It is not a DRAM-reclamation
  tool.

Flash is not a constraint either, and no part of this plan should be justified
by flash pressure: `KilnCtrl.bin` is 1,924,496 B against a 3,145,728 B app slot
(39% free, ~1.22 MB headroom).

---

## 2. Current configuration (measured, from `sdkconfig`)

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_40M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=16384
CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL=32768
CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP=y
CONFIG_SPIRAM_ALLOW_STACK_EXTERNAL_MEMORY=y
# CONFIG_SPIRAM_XIP_FROM_PSRAM is not set
```

PSRAM is already enabled, already backing `malloc`, and Wi-Fi/lwIP are already
being pushed at it. The remaining levers are the *threshold* at which the
allocator prefers PSRAM, and the set of task stacks still pinned to internal
SRAM.

Note that `sdkconfig` is gitignored in this project. Any configuration change
below must be made in `sdkconfig.defaults`, or a clean clone builds something
different from what was measured.

---

## 3. Baseline data

### 3.1 Live stack high-water marks

Captured 2026-09-01 from the running board via the `get_stack_margin` MCP tool:

| task | free at worst | allocated | headroom |
|---|---|---|---|
| `httpd_worker` | 2772 B | 8192 B | 33.8% |
| `safety_owner_task` | 2164 B | 3072 B | 70.4% |
| `safety_owner_evt` | 2336 B | 3072 B | 76.0% |
| `safety_proto_rx` | 4632 B | 8192 B | 56.5% |
| `safety_poll` | 4920 B | 8192 B | 60.1% |
| `profile_executor` | 1388 B | 4096 B | 33.9% |
| `profile_exec_wdt` | **368 B** | 2560 B | **14.4% — CRITICAL** |
| `bx_flash_worker` | 3676 B | 8192 B | 44.9% |
| `uart_owner_task` | 2164 B | 3072 B | 70.4% |
| `uart_owner_evt_task` | 2344 B | 3072 B | 76.3% |

Only instrumented tasks appear here. Coverage is partial — see section 4.2.

### 3.2 Internal heap

**Not yet measurable.** There is no heap-reporting tool on the `kilnctrl` MCP
surface, and no HTTP endpoint returning `heap_caps_get_free_size()` broken down
by capability. This is the single largest gap in the plan: Phase 1 below cannot
be evaluated without it, because its entire effect lands on the internal heap
rather than on anything currently observable.

---

## 4. Phase 0 — tooling (blocking prerequisite)

Nothing else in this plan should start before this phase lands. Every remaining
phase is a change whose only visible effect is on internal DRAM, and internal
DRAM is currently unobservable. Without Phase 0 the rest is unfalsifiable.

### 4.1 `get_heap_info` — board tool plus HTTP endpoint

New MCP tool in the `system` group, backed by a new read-only HTTP endpoint,
reporting at minimum:

- `heap_caps_get_free_size()` and `heap_caps_get_largest_free_block()` for
  `MALLOC_CAP_INTERNAL`, `MALLOC_CAP_SPIRAM`, `MALLOC_CAP_DMA`, and the default
  capability set;
- `heap_caps_get_minimum_free_size()` — the low-water mark — for each. The
  instantaneous free figure is nearly useless for this work, since the
  exhaustion event is transient and load-dependent;
- total PSRAM size and current PSRAM usage.

Low-water is the metric every acceptance criterion below is written against.

### 4.2 Extend `stack_margin` instrumentation coverage

The table in 3.1 covers 10 tasks. The firmware creates substantially more:
`kiln_io_owner`, `thermo_owner`, `screen_idle`, `spi_owner`, `i2c_owner`,
`autotune_engine`, `telemetry_log`, `link_watchdog`, `info_uart_bridge`,
`gpio_probe`, the LVGL task, `boot_button`, `danger_mode`, and the OTA reboot
tasks. Phase 2 right-sizes stacks against measured watermarks, so any task
without a watermark cannot be right-sized. Register the uninstrumented ones
through the existing `stack_margin.h` mechanism.

### 4.3 Baseline capture

With 4.1 and 4.2 in place, capture internal-heap low-water and the full stack
table under a realistic load: a firing in progress, the web UI open on at least
two clients, and the safety link running. Record the result in this document as
the reference every later phase is compared against.

A baseline taken on an idle board would understate the pressure and is not
acceptable for this purpose. The exhaustion symptom that motivates this plan
only appears under load; a baseline that does not reproduce the conditions
cannot show whether a phase helped.

---

## 5. Phase 1 — lower `SPIRAM_MALLOC_ALWAYSINTERNAL`

Currently 16384: every heap allocation smaller than 16 kB is served from
internal DRAM. That threshold captures most of this firmware's allocation
population, which is what makes it simultaneously the highest-leverage single
change available and the riskiest.

**Change.** Step the threshold down — 8192, then 4096, then 2048 — measuring at
each step rather than jumping straight to the lowest value. Re-examine
`SPIRAM_MALLOC_RESERVE_INTERNAL` (32768) in the same pass.

**Why this is believed safe.** Allocations that genuinely require internal
memory — DMA descriptors and buffers, anything touched while the cache is
disabled — request it explicitly through `MALLOC_CAP_DMA` or
`MALLOC_CAP_INTERNAL`, and are unaffected by this threshold. It changes only
where an unqualified `malloc` lands.

**Why it is nonetheless the riskiest phase.** The failure mode is latent. Code
that has always silently received internal memory, and that carries an
undeclared internal-memory requirement, will keep working right up until it
does not — plausibly during a firing. This change must be soaked, not
smoke-tested. Do not land it immediately before a real firing.

**Hazard to check explicitly.** Anything performing NVS or SPI-flash writes
from a buffer that this threshold newly relocates to PSRAM. That combination
has already bitten this project once; see the flash-worker rationale in
`uart_bridge_ext.c` for the established pattern and why it exists.

**Acceptance.** Internal-heap low-water under the 4.3 load improves materially
against baseline, with no new panics, no new coredump entries, and no
httpd-socket resets across a soak that includes a full firing.

---

## 6. Phase 2 — right-size internal stacks

Reclaims DRAM with no PSRAM-hazard exposure whatsoever, because nothing
relocates. Ordered before the relocation phase deliberately: it is strictly
safer, and shrinking a stack before moving it means there is less to move.

Candidates from 3.1, all sitting at 70% or more headroom:

| task | allocated | worst-case free | note |
|---|---|---|---|
| `safety_owner_task` | 3072 B | 2164 B | trim candidate |
| `safety_owner_evt` | 3072 B | 2336 B | trim candidate |
| `uart_owner_task` | 3072 B | 2164 B | trim candidate |
| `uart_owner_evt_task` | 3072 B | 2344 B | trim candidate |

Trim to the measured worst case plus a stated safety factor, and write both the
factor and the measurement date into the comment at each `xTaskCreate*` call.
That is this codebase's existing convention, and it is the guard against the
"sized by comment, not measurement" failure this project has hit before. Do not
trim against watermarks captured on an idle board — the worst case has to have
actually occurred before it can be measured.

Expected yield is modest, in the low single-digit kB, but it is nearly free.

**Out of scope for trimming:** `profile_exec_wdt` at 14.4%, and `httpd_worker`
and `profile_executor` at roughly 34%. Those go the other way if they move at
all.

---

## 7. Phase 3 — relocate remaining internal task stacks to PSRAM

Per-task, most caution required, smallest blast radius per individual change.

### 7.1 Already on PSRAM — no work

Created via `xTaskCreatePinnedToCoreWithCaps(..., MALLOC_CAP_SPIRAM)`:
`autotune_engine`, `safety_poll`, `uart_protocol_rx`, `telemetry_log`,
`link_watchdog`, `info_uart_bridge`, `gpio_probe`, `factory_reset_reboot`.

### 7.2 Deliberately internal — do not touch

Each of these already carries a comment in its source explaining why. A PSRAM
stack on a task that writes flash asserts every time; several of these also run
in windows where the cache is disabled, where a PSRAM stack is unreachable by
construction.

- `bx_flash_worker` (`uart_bridge_ext.c`) — the flash worker itself
- `danger_mode`
- `boot_button`
- `recovery_exit_reboot`, `ota_rollback_reboot`, `ota_pico_rollback`
- the LVGL task (statically allocated, `lvgl_port.c`)

If a future pass proposes moving any of these, that proposal must first explain
why the existing comment is wrong.

### 7.3 Candidates — evaluate individually

| task | stack | must trace before moving |
|---|---|---|
| `kiln_io_owner` | 4096 B | relay-state persistence path |
| `thermo_owner` | 4096 B | any calibration or NVS write |
| `profile_executor` | 4096 B | run-state breadcrumb writes to `kiln_nvs` |
| `spi_owner` | — | shares the SPI bus with flash |
| `i2c_owner` | — | expected clean |
| `uart_owner` and `uart_owner_evt` | 3072 B each | expected clean |
| `screen_idle` | 3072 B | display-settings persistence |

**The work of this phase is the tracing, not the call-site edit.** The edit is a
single substitution per task. The question that must be answered first, for each
task independently, is whether any code path reachable from that task's entry
point writes flash, disables the cache, or runs in an ISR-adjacent context.

`profile_executor` deserves the most care: it writes the run-state breadcrumb,
and it is also the task whose failure during a firing matters most.

Move one task per commit, with a soak between. Do not batch.

---

## 8. Out of scope

- The responsive and modern web-UI rework. Fully independent of this plan;
  neither blocks the other. Tracked separately.
- The on-board ST7796 / LVGL display. Untouched here.
- Any relocation of the embedded web assets — see section 1.
- `CONFIG_SPIRAM_XIP_FROM_PSRAM` — see section 1.

---

## 9. Open item carried out of the baseline capture

**`profile_exec_wdt` at 368 B free of 2560 B (14.4%).** Found while capturing
the 3.1 baseline. It is unrelated to PSRAM and belongs to no phase of this
plan, but it should not be lost.

This is guard 9's watchdog task — the thing that is supposed to still be
running when other things are not. It should be enlarged, and the reason it is
running that close to the edge should be understood rather than papered over
with a bigger number. Worth handling before this plan starts, not after.
