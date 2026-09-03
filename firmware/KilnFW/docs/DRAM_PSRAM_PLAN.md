# DRAM / PSRAM Plan — reclaiming internal SRAM on the ESP32-S3

Plan doc for moving what can be moved out of internal DRAM and into the 8 MB
octal PSRAM on the N16R8 module, so that internal SRAM stops being the binding
constraint on this firmware.

Conventions this doc follows, matching `PID_EXPANSION_PLAN.md`: **the code is
truth, not the checkboxes.** Nothing below is marked done until a commit is
named. Every number is either measured and attributed, or explicitly labelled
as an estimate.

**Update 2026-09-02 (fifth pass) — doc cleanup only, no source change.**
Sections 4.1 and 4.2 were already fully landed by `a698dc0` (heap-status MCP
tool + `MALLOC_CAP_DMA` breakout; all sixteen previously-uninstrumented tasks
registered with `stack_margin`) but this doc still described both as open
work with a stale task list. Corrected in place. `run_all_checks.ps1` is now
29/29 (was reported 27/27 below; `check_stack_margin_registration.ps1` and
`check_nvs_write_guard_coverage.ps1` both count). Board access was off-limits
this pass too (mid-firing) -- everything left in this plan past §4 is gated
on a boot/soak; see §7.3's table and §9 for what to do first once hardware is
available again.

**Update 2026-09-02 (fourth pass) — section 9 closed (unflashed), a second
write-guard gap closed, and the section 4.3/7 measurement procedure now
exists as code, tested without hardware.** Board access was off-limits this
pass (mid-firing, heaters energized) -- everything below is source/host-test
only.

1. **Section 9 (`profile_exec_wdt`, 368 B free of 2560 B, 14.4% CRITICAL) --
   fixed, unflashed.** `profile_executor_start.c`'s `xTaskCreatePinnedToCore()`
   for this task now requests 4096 B (was 2560), matching `profile_executor`'s
   own configured size -- the measured worst case (~2192 B used) left too
   little slack for code paths (`run_state_note()` on the safety-link-abort
   branch, `safety_link_get_status()`, `guard9_assert_stale_tick_fault()`,
   `heat_enable_reconcile()`) this pass's static read did not rule out as
   exercised. Not a guess: the new size was chosen against the one number
   this task actually reported, with headroom modeled on its sibling task's
   measured percentage. `stack_margin_register()`'s constant was updated in
   the same commit, per that function's own "must match the xTaskCreate*()
   argument" convention. Still needs the next boot's `stack_margin` read to
   confirm the new worst case, same as every other unflashed change in this
   plan -- see the reflash step in section 7's update below, this task's
   number should be pulled in the same session.

2. **Write-path coverage re-audit found two more of the same gap class the
   third pass fixed, both now closed:** `run_state.c`'s and `relay_cycles.c`'s
   own `migrate_from_default_partition()` functions write NVS
   (`nvs_set_blob()`/`nvs_commit()`) but had never received the
   `caller_stack_is_external()` guard those files' main write paths
   (`persist_locked()`) already carry -- the third pass's fix covered the
   file's headline write function and treated the file as done, without
   re-checking every OTHER write call site in it. A third instance of the
   identical pattern turned up in `profiles_http.c`: `nvs_erase_slot()`
   writes NVS next to the already-guarded `nvs_save_slot()` but had no guard
   of its own. All three are init-time (`run_state_init()`/
   `relay_cycles_init()`, both called once from `app_main`'s own
   internal-stack task before any PSRAM-stacked task exists) or
   httpd-worker-only (`nvs_erase_slot()`'s two callers are both delete-profile
   HTTP handlers) today, so none of them was live -- but each would have read
   as "this file is covered" to a file-level audit, which is exactly the
   false-confidence class the third pass's own finding described. Guards
   added, matching the established refusal message/pattern.

   **New standing check:** `tools/check_nvs_write_guard_coverage.ps1`
   (auto-discovered by `run_all_checks.ps1`, now part of the 29/29 suite) parses every function
   body in the six files that have opted into the `caller_stack_is_external()`
   convention (`kiln_cfg_store.c`, `safety_cfg_store.c`, `profiles_http.c`,
   `relay_cycles.c`, `run_state.c`, `profile_executor_firing_stats.c`) and
   fails by function name if any of them calls `nvs_set_*()`/`nvs_commit()`/
   `esp_partition_write()`/`esp_partition_erase_range()` without also calling
   `caller_stack_is_external()` in the same body. This is scoped to files
   already on the convention, not the 12 unguarded modules
   (`adaptive_tune.c`, `boot_guard.c`, `crash_report.c`, `ota_http.c`,
   `ota_record.c`, `profiles_builtin.c`, `time_sync.c`, `touch_cal_store.c`,
   `unit_pref.c`, `watchdog_cfg.c`, `wifi_prov.c`, `zones_config_store.c`) --
   the third pass's per-candidate trace found none of those reachable from a
   section 7.3 relocation candidate, so adding the guard there is a
   different task's call. Proved red: commented out `nvs_erase_slot()`'s
   guard, ran the check, got `NVS WRITE GUARD COVERAGE CHECK FAILED: ...
   profiles_http.c : nvs_erase_slot()`; reverted, check passed again.

3. **Section 4.3/7 measurement procedure now exists as code**, not just
   prose: `tools/PcTools/src/kilnctrl/stack_margin_baseline.py` (pure,
   no link access) turns a `get_stack_margin()` + `get_fw_version()` reading
   into a timestamped, commit-tagged JSON record; `worst_case_across_
   conditions()` combines records from multiple load conditions into the
   single number each task's real worst case actually is (the minimum
   `hwm_bytes` seen, preferring a live reading over a dead one so a boot-race
   dead entry under one condition can't masquerade as "the" worst case); and
   `render_markdown_table()` emits section 3.1's exact column shape, ready to
   paste in. `tools/PcTools/scripts/capture_stack_margin_baseline.py` is the
   thin CLI wrapper that does the real UART round trip (`kiln_call`-style
   `m.connect()` / `m._info.get_stack_margin()` / `m._info.get_fw_version()`)
   and writes into `firmware/KilnFW/docs/stack_margin_baseline/`; run it once
   per condition (`idle`, `mid_firing`, `web_ui_open` -- see the module's own
   docstring for why each matters, matching this section's existing
   reasoning) and then with `--report` for the combined table. Tested against
   fabricated `StackMarginEntry`/`FirmwareVersion` objects only
   (`tools/PcTools/tests/test_stack_margin_baseline.py`, 10 tests, all
   passing standalone and inside the full `run_pctools_tests` suite) --
   no board was contacted while writing or testing this. Proved a mutation
   can fail: broke the dead-vs-alive preference in
   `worst_case_across_conditions()`, `test_worst_case_prefers_a_live_
   reading_over_a_dead_one` failed with `assert False is True`; reverted.

   This module is the CODE half of the procedure; it does not itself capture
   anything against the still-mid-firing board, and the next pass with
   hardware access is the one that actually runs it three times and pastes
   the resulting table into section 3.1.

Status (2026-09-02): the history ring moved to PSRAM (`4c0d703`, ~21 kB
reclaimed), Phase 1 step 1 landed in both `sdkconfig.defaults` and the
generated `sdkconfig` (8192, unsoaked), the PSRAM-stack write guards and the
`stack_margin` instrumentation blocker are closed. **No task stack has been
relocated.** Phase 2 (§6) and every §7.3 candidate are still open.

**Update 2026-09-02 (third pass) — coverage audit found a real gap, closed it;
every section 7.3 candidate is BLOCKED, none relocated.**

The second pass below (`f1afc8a`, `a99bc15`) added `caller_stack_is_external()`
to `kiln_cfg_store.c`, `safety_cfg_store.c` and `profiles_http.c` and treated
that as closing the write-side hazard. It did not: those are 3 of the
firmware's ~17 modules that write NVS/flash. A full sweep (`grep` for
`nvs_set_*`/`nvs_commit`/`esp_partition_write`/`esp_partition_erase` across
`App/drivers/`, then tracing each hit back to its callers) found the guard
missing from `adaptive_tune.c`, `boot_guard.c`, `crash_report.c`,
`ota_record.c`, `profiles_builtin.c`, `time_sync.c`, `touch_cal_store.c`,
`unit_pref.c`, `watchdog_cfg.c`, `wifi_prov.c`, `zones_config_store.c` — and,
critically, **`run_state.c`, `relay_cycles.c` and
`profile_executor_firing_stats.c`, all three reached directly from
`profile_executor.c`'s own tick/halt path**: `run_state_note()` /
`run_state_note_progress()` (transitions + the 300 s RUNNING refresh),
`relay_cycles_maybe_persist()` (every tick) / `relay_cycles_flush()` (halt),
and `firing_stats_persist()` (halt). This is exactly the task section 7.3
names as the highest-care relocation candidate, and exactly the write this
section's "profile_executor deserves the most care" line was written about —
the belt-and-suspenders check the second pass believed was in place for it did
not exist. Fixed this pass (guard added, matching the established pattern,
each with a host test proving it refuses under a simulated PSRAM stack and a
mutation-tested proof the check can fail): `run_state.c`, `relay_cycles.c`,
`profile_executor_firing_stats.c`. The other 11 unguarded modules are NOT
fixed — none of them is reachable from a section 7.3 candidate task by this
pass's trace (see each candidate's note below), so they are out of scope for
*this* plan, but they are the same latent hazard for whatever writes to them
today and any future relocation must re-check this list, not assume it is
still complete.

Per-candidate verdict from this pass (full task-tree trace, no board access —
hardware was mid-soak, see the top-level task record):

- **`profile_executor`** — NOT flash-write clean before this pass (see above);
  clean now that the three guards exist. Stack high-water mark IS measured
  (1388 B free / 4096 B, 33.9%, section 3.1). Still **BLOCKED**: a measured
  HWM plus a closed write-guard gap is necessary but not sufficient —
  relocating the task this plan itself calls the most care-requiring one, on
  the strength of a static trace with no hardware soak, is exactly the
  "unverified relocation" this task was told to avoid. Leave for a future
  pass with board access.
- **`kiln_io_owner`** — flash-write clean: full trace of `owner_task()`'s
  command switch (`kiln_io_owner.c:326-420`+) shows a bounded enum of
  GPIO/I2C-expander operations only, no NVS/store call anywhere in the file,
  no function-pointer dispatch to widen that later. **BLOCKED on instrumentation** at the time
  of that pass; unblocked by the cap raise below (`e263b14`). Still not in
  §3.1's measured table — needs one boot to read the number.
- **`thermo_owner`** — flash-write clean: full trace of `owner_task()`'s
  command switch (`thermo_owner.c:102-180`+) shows MAX31856 SPI register
  operations only (config/thresholds/CJ-offset/read), no NVS/store call
  anywhere in the file. Same as `kiln_io_owner`: unblocked, unmeasured.
- **`screen_idle`** — flash-write clean: the entire task body
  (`screen_idle.c:63-131`) calls only `NS2009_read()` and flips its own
  in-RAM `screen_on`/`last_activity_tick` fields; it does NOT touch touch
  calibration storage or issue the display clear itself (that's the LVGL
  task, by design — see the file's own comment on why). This pass's trace
  found the table's "display-settings persistence" caution for this task to
  be stale/inaccurate. Unblocked, unmeasured.
- **`spi_owner`** (`esp_spi_owner.c`) — no NVS/flash call anywhere in the
  file; it is a raw SPI-transfer relay, not a command dispatcher that could
  reach arbitrary code. Its hazard is bus contention with the flash's own
  SPI use during a cache-disabled window, a DIFFERENT risk than the "stack
  unreachable while cache is down" class this section is about, and this
  pass did not attempt to characterize it. Unmeasured, and still blocked on
  that separate, uncharacterized hazard.
- **`i2c_owner`** — no NVS/flash call anywhere in the file (`i2c_owner.c`),
  same shape as `spi_owner`. Unblocked, unmeasured.
- **`uart_owner_*` (×2 instances)** — unchanged: still blocked by section 6
  on the unmeasured Pico OTA relay path. Not re-examined this pass.

Net effect: **zero tasks relocated this pass.** The three NVS-write guards are
the only change that touches board behavior at all, and they are pure
refusals that only fire on a misuse that cannot occur on today's task
assignment (every caller of `run_state_note()`/`relay_cycles_*`/
`firing_stats_persist()` today runs on an internal-SRAM stack) — inert on the
board exactly like the second pass's three guards were, no soak required for
this part. `STACK_MARGIN_MAX_TASKS` was NOT raised this pass — **the cap-raise pass
below did it afterwards (28 → 40, `e263b14`), and found the real blocker was
a 29th registration silently losing its slot, not a missing registration.**
The first hardware step for the next pass is therefore just the measurement:
boot the board, pull a full (now paginated) `get_stack_margin()`, record HWMs
for `kiln_io_owner`/`thermo_owner`/`spi_owner`/`i2c_owner`/`screen_idle` in
§3.1, and only then reconsider them as relocation candidates — their
flash-write cleanliness is already established and does not need re-doing.
`profile_executor` needs a soak on top of that regardless of HWM.

**Update 2026-09-02 (second pass) — two safety nets landed, no soak needed.**
1. `check_sdkconfig_defaults_applied.ps1` (picked up by
   `tools/run_all_checks.ps1`) now fails loudly whenever a deliberately
   changed key (currently just `CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL`)
   disagrees between `sdkconfig.defaults` and the gitignored, generated
   `sdkconfig`. It was red when written (generated `sdkconfig` still held
   16384); that file has since been regenerated and now agrees at 8192. If it
   goes red again, regenerate -- do not silence it by editing the check.
2. `kiln_cfg_store.c`'s `nvs_save_store()` now refuses (does not crash) when
   called from a PSRAM-stacked task, matching `safety_cfg_store.c`'s
   existing `caller_stack_is_external()` guard; `profiles_http.c`'s
   `nvs_save_slot()` already got the same guard in a concurrent commit
   (`a99bc15`). This is the belt-and-suspenders check section 7.2 calls for
   -- it does not require a soak, since it only ever fires on a genuine
   misuse.
3. Section 6 (right-size internal stacks) is **blocked**, but this session's
   static trace narrowed the unknown instead of leaving it untouched. The
   Pico OTA relay (`ota_pico_relay.c`) runs on its OWN dedicated task
   (`relay_task_fn`, `OTA_PICO_RELAY_TASK_STACK` = 6144 B) -- its bulk-send
   path (`safety_link_send_update_frame()` ->
   `uart_protocol_send_broadcast()`, `uart_protocol.c:820`) builds each
   frame in a local `raw[HEADER_LEN + UART_PROTO_MAX_PAYLOAD + 2]` buffer
   (~263 B) and writes it out SYNCHRONOUSLY, on the relay task's own stack --
   it does not run on `uart_owner_task`/`safety_owner_task` at all. So
   whatever `CONFIG_KILNCTL_UART_OWNER_STACK_SIZE`'s comment is actually
   defending against is not the bulk transfer's send path; it can only be
   the RECEIVE-side dispatch of the safety processor's replies during that
   transfer, which is frame-size-bounded (253 B max payload) the same as any
   other traffic on that link, OTA or not -- there is no larger buffer any
   received frame can force. This is evidence the reserved margin may be
   defending against frequency (many back-to-back frames) rather than depth,
   but it is not a substitute for the real measurement: still **blocked**
   until someone runs a real Pico update with `stack_margin` live, exactly
   as stated below. Do not trim `KILNCTL_UART_OWNER_STACK_SIZE` on the
   strength of this trace alone.

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
by flash pressure. `FLASH_BUDGET_PLAN.md` §4.2 owns that number (1,936,320 B
against a 3,145,728 B app slot at `eb17ea5`, 38.4% free); do not restate it
here.

---

## 2. Current configuration (measured, from `sdkconfig`, 2026-09-02)

```
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_40M=y
CONFIG_SPIRAM_USE_MALLOC=y
CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL=8192
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

Only instrumented tasks appear here. All long-lived tasks are now
instrumented (§4.2, `a698dc0`) — what remains missing is a boot to actually
read their HWM numbers, not further registration work.

### 3.2 Internal heap

**Already measurable — an earlier draft of this doc was wrong about this.**

`App/drivers/dashboard_http.c:386-397` already calls
`heap_caps_get_free_size()`, `heap_caps_get_largest_free_block()`,
`heap_caps_get_minimum_free_size()` and `heap_caps_get_total_size()` for both
`MALLOC_CAP_INTERNAL` and `MALLOC_CAP_SPIRAM`, and serialises them into the
dashboard JSON at lines 837-839 as `heap_internal` / `heap_spiram` objects with
`free` / `largest_free_block` / `min_free` / `total`. `TODO.md` §14 already used
this data live under load.

An earlier draft claimed no such endpoint existed and built a blocking Phase 0
around rebuilding it. That was a planning defect — verify before declaring a
prerequisite. What is genuinely missing is much smaller; see 4.1.

---

## 4. Phase 0 — tooling (small, and mostly already done)

Every remaining phase is a change whose only visible effect is on internal
DRAM, so it has to be observable before it is worth attempting. Per 3.2 most of
that observability already exists; what is left is a thin client layer and one
missing capability.

### 4.1 Wrap the existing heap data — DONE (`a698dc0`)

An MCP tool (`get_heap_status`, `tools/PcTools/src/kilnctrl/dashboard_http_client.py`
+ `mcp_server_info.py`, registered in the facade taxonomy) wraps the existing
`heap_internal`/`heap_spiram`/`heap_dma` JSON. `MALLOC_CAP_DMA` was broken out
as its own `heap_dma` object in `dashboard_http.c` for Phase 1's use.
`min_free` (low-water) is the metric every acceptance criterion below is
written against — the instantaneous free figure is nearly useless here, since
the exhaustion event is transient and load-dependent.

### 4.2 Extend `stack_margin` instrumentation coverage — DONE (`a698dc0`)

All sixteen previously-uninstrumented long-lived tasks are registered:
`kiln_io_owner`, `thermo_owner`, `screen_idle`, `spi_owner`, `i2c_owner`,
`autotune_engine`, `telemetry_log`, `link_watchdog`, `info_uart_bridge`,
`gpio_probe`, `lvgl`, `boot_button`, `danger_mode`, `recovery_exit`,
`ota_rollback_reboot`, `ota_pico_rollback`. `STACK_MARGIN_MAX_TASKS` covers
the full boot-time registration count (raised 28→40 by `e263b14`, see §7.2).
`tools/check_stack_margin_registration.ps1` guards regressions. Coverage in
3.1 is no longer partial for tasks that exist in source — what is still
missing there is real HWM numbers for the newly-registered tasks, which
needs a boot (see §7.3).

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

**Step 1 landed (`a0b8711`): `sdkconfig.defaults` now sets 16384 -> 8192.
NOT soaked yet.**

**The generated `sdkconfig` now carries 8192 too** (`sdkconfig:1886`,
checked 2026-09-02), so the step is no longer inert in the build — it was
while `sdkconfig` still held the stale 16384. It remains **unsoaked**: no
measurement has been taken against it, and §4.3's baseline was never
captured, so there is nothing to compare a post-change number to.

`sdkconfig.defaults` already carries a 2026-08-20 note, not previously cited
in this plan, that ALWAYSINTERNAL=4096 was tried and reverted on hardware:
the LVGL display task failed to start, more UART inboxes failed, and the
PC-link safety watchdog hit `ESP_ERR_NO_MEM`. That attempt is confounded —
`SPIRAM_TRY_ALLOCATE_WIFI_LWIP` changed in the same experiment — so it does
not rule out 4096 on its own, but the step below must acknowledge it and
watch for the same three symptoms.

Currently 8192 (was 16384): every heap allocation smaller than that is served
from internal DRAM. That threshold captures most of this firmware's allocation
population, which is what makes it simultaneously the highest-leverage single
change available and the riskiest.

**Change.** Step the threshold down — 8192 (done, unsoaked), then 4096, then
2048 — measuring at each step rather than jumping straight to the lowest
value. Re-examine `SPIRAM_MALLOC_RESERVE_INTERNAL` (32768) in the same pass.

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

**Soak definition** — an earlier draft left "soak" undefined, which makes the
riskiest phase in this plan unfalsifiable. Concretely: at least one complete
firing from cold to cooldown, with the web UI open on two clients throughout,
plus 24 h of idle-with-Wi-Fi-connected afterwards. Latent allocation failures
surface under sustained fragmentation, not in a smoke test.

**Do not run this soak concurrently with a partition-table revision.**
`FLASH_BUDGET_PLAN.md` §5 moves flash regions and erases `otadata`; this phase
changes where allocations land. Both can produce boot failures and flash-path
asserts. Interleaved, neither is attributable.

---

## 6. Phase 2 — right-size internal stacks — BLOCKED

All four candidate tasks below share one Kconfig knob,
`CONFIG_KILNCTL_UART_OWNER_STACK_SIZE` (`App/drivers/Kconfig:554-569`,
default 3072, confirmed reaching the build). It was already trimmed
4096 -> 3072 in `8ad7d5b`, and that commit's own comment reserves the
remaining margin for a Pico OTA relay transfer that has never been measured.
**Blocked** until someone runs a real Pico update with stack-margin
instrumentation live — there is no further headroom to spend against an
unmeasured worst case.

Reclaims DRAM with no PSRAM-hazard exposure whatsoever, because nothing
relocates. Ordered before the relocation phase deliberately: it is strictly
safer, and shrinking a stack before moving it means there is less to move.

Candidates from 3.1, all sitting at 70% or more headroom.

**These are four distinct tasks, not two under two names.** `uart_owner_init()`
has two call sites — `safety_link.c:395` (isolated safety link) and
`main.c:1529` (PC link) — each creating its own `uart_owner_task` /
`uart_owner_evt_task` pair from the same code at `uart_owner.c:235,253`. The
`stack_margin` labels disambiguate them: `safety_link.c:419-420` registers the
safety pair as `safety_owner_*`, `main.c:1556-1557` registers the PC pair under
the raw names. A review of this doc asserted these were the same two tasks
double-counted; that was checked against both call sites and is wrong. Both
pairs are real and both consume internal DRAM.

Since one `xTaskCreate*` site serves both instances, a stack-size change here
affects **both links at once** — a per-instance size would need the depth
passed in as a parameter.

| task | link | allocated | worst-case free |
|---|---|---|---|
| `safety_owner_task` | safety | 3072 B | 2164 B |
| `safety_owner_evt` | safety | 3072 B | 2336 B |
| `uart_owner_task` | PC | 3072 B | 2164 B |
| `uart_owner_evt_task` | PC | 3072 B | 2344 B |

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
`autotune_engine`, `safety_poll`, **`uart_proto_rx`**, `telemetry_log`,
`link_watchdog`, `info_uart_bridge`, `gpio_probe`, `factory_reset_reboot`.

Note on `uart_proto_rx` (`uart_protocol.c:480-481`): the task is named
`uart_proto_rx`; `uart_protocol_rx_task` is the C function, and an earlier draft
of this doc used the function name as if it were the task name. Like
`uart_owner` (§6) it is instantiated **twice** — `safety_link.c:478` and
`main.c:1579` — so two stacks and two TCBs exist.

**Update: the PC-link instance is now registered too** (`main.c:1579`,
`stack_margin_register("uart_proto_rx", ...)`) — the "uninstrumented,
invisible in 3.1" note above is stale. Both instances now report. This
closes that half of 4.2; the other tasks listed in 4.2 still need
registering.

`STACK_MARGIN_MAX_TASKS` is 40 (`stack_margin.h:83`), raised from 28 by
`e263b14` against a real boot-time registration count of 29 — see the
cap-raise note in §7.2. The `/api/status` JSON headroom claim from an earlier
pass (~100 B of 4096 B) was never verified against a live board and is not
relied on anywhere here; this registry is not exposed in `/api/status` at all
(only over `INFO_CMD_GET_STACK_MARGIN`).

### 7.2 Deliberately internal — do not touch

Each of these already carries a comment in its source explaining why. The
mechanism is worth stating precisely, because "writes flash" is the symptom
rather than the rule: **PSRAM is reached *through* the flash cache**, so a task
whose stack lives in PSRAM cannot run at all while the cache is down.
`uart_bridge_ext.c:94-99` states it directly — *"A PSRAM STACK MUST NOT BE LIVE
WHILE THE FLASH CACHE IS DOWN... ESP-IDF asserts on it: `assert failed:
spi_flash_disable_interrupts_caches_and_other_cpu`"* — and the same file records
it reproduced twice on hardware (the LVGL task, 2026-08-20, and the flash worker
via a profile push over the UART bridge).

- `bx_flash_worker` (`uart_bridge_ext.c`) — the flash worker itself
- `danger_mode`
- `boot_button`
- `recovery_exit_reboot`, `ota_rollback_reboot`, `ota_pico_rollback`
- the LVGL task (statically allocated, `lvgl_port.c`)

If a future pass proposes moving any of these, that proposal must first explain
why the existing comment is wrong.

**Update 2026-09-02 (cap-raise pass) — the instrumentation blocker is
closed; every 7.3 candidate is now reachable, none relocated.** All five
uninstrumented-and-clean candidates already had a `stack_margin_register()`
call site (added in the Phase 0 pass referenced by `stack_margin.h`'s own
header comment) — this was never a missing-registration bug. The actual
blocker: `STACK_MARGIN_MAX_TASKS` was 28 and the real boot-time registration
count was 29, not 28 — `i2c_owner_init()` (`espInterfaces/i2c_owner.c`) is
called by two live drivers on this board (`SX1509.c`, the IO expander, and
`NS2009.c`, the touch controller; `FT6336U.c` also calls it but is dead code
— "FT6336U_start's caller is nobody", `FT6336U.c:15` — so its call never
executes), so that one source call site fires twice at boot. One
registration silently lost the coin flip every boot (`stack_margin.c`'s
`ESP_LOGE`, non-fatal, nobody watching), and which task lost depended on
init order, not on anything about that task — which is exactly consistent
with kiln_io_owner/thermo_owner/spi_owner/i2c_owner/screen_idle *all*
reading as unmeasured despite being registered: whichever one the boot
sequence happened to reach after the 28th slot filled would drop out, and
init order here puts most of §7.3's candidates late relative to the fixed
28-slot pre-Phase-0 tasks.

Fixed this pass: `STACK_MARGIN_MAX_TASKS` raised 28 → 40 (`stack_margin.h`;
12 slots × 28 B/slot = 336 B static DRAM, spent deliberately — see that
header's own comment for the arithmetic). A new standing guard,
`tools/check_stack_margin_registration.ps1`, now runs in
`run_all_checks.ps1` and fails if any of this plan's tracked tasks loses its
`stack_margin_register()` call site, or if the cap falls behind the real
call-site count again — proved red by commenting out `kiln_io_owner`'s
call site (`STACK MARGIN REGISTRATION CHECK FAILED: ... kiln_io_owner`),
then reverted.

Separately, the ONLY wire exposure of this registry —
`INFO_CMD_GET_STACK_MARGIN` (`uart_bridge_info.c`, reachable via the
`kilnctrl` MCP tool's `get_stack_margin`) — turned out to silently drop
most entries: `BRIDGE_REPLY_MAX` is 253 bytes and the reply builder's own
comment assumed "today's <=6 registered tasks" when the real count was
already 28; only the first ~10 short-named entries fit one reply, and nothing
on the wire told a caller the rest were missing. Fixed this pass:
`build_stack_margin_reply()` now pages (`start_index` request byte,
`truncated`/`next_start_index` response bytes — see `uart_task_ids.h`'s
`INFO_CMD_GET_STACK_MARGIN` doc), and `KilnInfo.get_stack_margin()`
(`tools/PcTools/src/kilnctrl/info.py`) loops pages internally, so a caller
still gets one complete list. No `/api/status` JSON change was needed —
this measurement was never exposed there, only over this UART path.

| task | stack | flash-write trace (2026-09-02 third pass) | HWM reachable? | verdict |
|---|---|---|---|---|
| `kiln_io_owner` | 4096 B | CLEAN — bounded GPIO/I2C-expander command switch, no NVS call, no function-pointer dispatch | yes (registered; cap/reply-paging fixed this pass) | needs a real boot to read the number, no longer BLOCKED on instrumentation |
| `thermo_owner` | 4096 B | CLEAN — bounded MAX31856 SPI register command switch, no NVS call | yes | needs a real boot to read the number |
| `profile_executor` | 4096 B | was NOT clean (`run_state.c`/`relay_cycles.c`/`profile_executor_firing_stats.c` had no PSRAM-stack guard); guards added third pass, now clean | yes (1388 B free, 33.9%) | BLOCKED — needs a real soak, not just a closed guard gap |
| `spi_owner` | — | no NVS call, but shares the SPI bus with flash (a different, uncharacterized hazard) | yes | needs a real boot to read the number; bus-contention question still open |
| `i2c_owner` | — | CLEAN — no NVS call anywhere in the file | yes | needs a real boot to read the number |
| `uart_owner_*` ×2 instances | 3072 B each | not re-examined this pass | yes | BLOCKED by §6 (unmeasured Pico OTA relay path) |
| `screen_idle` | 3072 B | CLEAN — full task body only reads touch and flips in-RAM flags; does NOT touch calibration storage (this table's old "display-settings persistence" caution was stale) | yes | needs a real boot to read the number |

**No task was relocated this pass — instrumentation only, as directed.** The
raised cap and paging fix are source/build changes only — **the board is
still running the pre-raise (28-slot) image; nothing in this pass has been
flashed.** The next pass's first step is hardware access: reflash with the
cap raise, boot the board, pull a full `get_stack_margin()` (now paginated,
so the whole registry — not just the first ~10 entries — comes back), and
record real HWM numbers for
`kiln_io_owner`, `thermo_owner`, `spi_owner`, `i2c_owner`, `screen_idle`
next to `profile_executor`'s in section 3.1's table. Only then does moving
any of them stop being "unverified relocation."

`uart_owner_*` appears in both §6 and here on purpose: §6 trims it, this phase
would move it. Trim first, then move — and remember one call site serves both
the safety and PC instances, so either change hits both links.

**The work of this phase is the tracing, not the call-site edit.** The edit is a
single substitution per task. The question that must be answered first, for each
task independently, is whether any code path reachable from that task's entry
point writes flash, disables the cache, or runs in an ISR-adjacent context.

`profile_executor` deserves the most care: it writes the run-state breadcrumb,
and it is also the task whose failure during a firing matters most. The third
pass found that care had not actually been paid — its write path had no
PSRAM-stack guard at all until this pass added one.

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

**`profile_exec_wdt` at 368 B free of 2560 B (14.4%). Fixed 2026-09-02
(fourth pass), unflashed.** Found while capturing the 3.1 baseline. It is
unrelated to PSRAM and belongs to no phase of this plan, but it should not be
lost.

This is guard 9's watchdog task — the thing that is supposed to still be
running when other things are not. `profile_executor_start.c` now creates it
with 4096 B (was 2560), matching `profile_executor`'s own configured size --
see the top-of-file update note for the reasoning and what specifically in
`watchdog_task_entry()`'s body made 2560 too tight. This was a size change
only, not a relocation, so it needed no soak to land in source, but it is
still unflashed (the board is mid-firing) -- the next boot's `stack_margin`
read should confirm the new worst case and can be pulled in the same pass
that captures section 7's cap-raise numbers.
