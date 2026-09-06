# Hardware Abstraction Tree — Plan

Status: opened 2026-09-05. Conventions, interface shapes, holdout list and
allowlist policy that a future reader needs have moved to
`firmware/hwAbstraction/README.md` ("Conventions" section) — this doc keeps
only what's open, phase-by-phase history, and decisions/gotchas not yet
captured there.

## Open

- **Hardware timing verification, never confirmed after the Phase 1b/2026-09-06
  uart collapse:** safety-link reply timing, display frame time, thermo read
  latency under a full-screen redraw. Host tests cannot see this — it needs
  a bench run. Budget for reference: reply window 345 ms, worst-case
  measured-on-paper reply flight ≈40 ms (see git history / commit `274afff`
  and friends for the arithmetic if it needs re-deriving).
- **`esp_random.h`** (10 files) was never classified — assign it to
  `hal_sysinfo` before treating the Phase 4 header set as fully closed out.
- **Pico flash** of the full HAL tree on the real board. The ESP side is
  done: `a410edd` flashed via `flash_firmware()` 2026-09-06, verified
  running factory, tuned gains and coupling matrix read back intact. The
  Pico has not been flashed since the SaftyFW gpio/wdt/flash migrations;
  blocked until another session's uncommitted `safety_guards.c` S8 change
  leaves the tree.
- Everything else named in this doc as a deliberate, permanent holdout
  (`firmware/hwAbstraction/README.md`'s "Permanent holdouts" list — Wi-Fi/
  httpd/LVGL/heap portability, OTA partition writes, the SaftyFW bootloader,
  `firmware/UnitTestFw`) is **owner-decided out of scope**, not open work.

If the hardware timing pass and the `esp_random.h` classification are
judged not worth doing, nothing else in this doc is open — in that case
this file should lose its `_PLAN` suffix per standing practice (finished
plans get renamed or folded into documentation). Proposed, not done here:
rename to `docs/HW_ABSTRACTION.md` and fold the phase history below into a
shorter changelog, once an owner confirms the two bullets above are closed
or explicitly deferred.

## Goal

One explicit hardware-abstraction tree serving both processors, so that
(a) porting off the ESP32-S3 or RP2040 is a backend swap, not a rewrite, and
(b) host tests link one honest fake backend instead of parse-only stub
headers plus hand-picked source lists.

## Tree shape

See `firmware/hwAbstraction/README.md` for the current directory layout —
it is kept there since it changes with the tree, not with this plan.

Builds: `esp/` is an ESP-IDF component (`firmware/hwAbstraction/idf/
hwabstraction_esp/`) added to KilnFW's `EXTRA_COMPONENT_DIRS`. `pico/` is a
plain CMake static lib (`hwabstraction_pico`) linked by all three SaftyFW
targets (`SaftyFW`, `SaftyFW_slotA`, `SaftyFW_slotB`) via one
`add_library(... STATIC ...)` — safe because the only runtime registration
(`irq_set_exclusive_handler`) happens from init, not a constructor. The
bootloader has its own CMakeLists, no FreeRTOS, and is excluded. `host/` is
compiled by both `build_host_tests.ps1` scripts via a response-file `cl`
invocation (Option A — see "Host build shape" below for why ctest/CMake was
rejected).

Not every interface exists on both sides, deliberately: kv is ESP-only;
flash/scratch/adc pico-only. Rationale for each: `firmware/hwAbstraction/
README.md`.

## Owner decisions (all taken, 2026-09-05)

Keep `firmware/UnitTestFw` untouched (no build/flash consumer, its
espInterfaces copy has diverged from KilnFW's, but it stays — do not
re-propose deletion). `drivers/` layering target layout approved as `App/
drivers/{hw,owners,control,safety,persist,net,http,ui,bridge,sim,common}/`
(subdirectories of the existing component, not sibling components — see
"drivers/ layering" below for why). Tree location/naming: `firmware/
hwAbstraction/{interface,esp,pico,host}`. Opaque-storage option (a): fixed
`_Alignas(8)` storage in the header, backend `_Static_assert`. hal_uart
two-primitive shape (`send` + `send_blocking`).

## Host build shape — Option A, decided

Extend both existing `build_host_tests.ps1` hand-lists with `hwAbstraction/
host/*.c` and drop the retired stub `/I` entries, rather than moving to
CommonFW's CMake/ctest project (never invoked by anything in `tools/`;
`build_saftyfw_host_tests`/`run_all_checks.ps1`/`verify.ps1` all shell the
`.ps1` scripts, and `buildlock.py` exists because concurrent `.ps1` runs
corrupt shared `.obj` paths — moving to ctest would break every path-keyed
check at once). Command-line length is the real constraint: KilnFW's
source list was already near cmd.exe's 8,191-char limit at the normal
checkout path (worse at a worktree path). Verified empirically that a long
inline `cl` command line is silently truncated mid-argument
(`LNK1104: cannot open file '>.obj'`) while `cl @sources.rsp` with one
quoted path per line builds clean — landed (`63e5689`).

## Phases — status

**Phase 0 — scaffold + contract. DONE (`c626727`).** hal_status.h (full
error table, `hal_status_to_name`), interface headers for spi/i2c/uart/gpio/
adc with the opaque-storage pattern, `/std:c11` demonstrated and
negative-tested via `compile_headers.ps1`. `ALREADY_INIT` on an
already-up bus returns `HAL_OK` with an INFO log (benign JTAG-reset
re-entry), documented in both headers.

**Phase 1a — move only. DONE.** espInterfaces owners relocated
per-interface, colocated under each processor's own HAL backend directory
(`esp/spi/`, `esp/i2c/`, `esp/uart/`; `pico/spi/`, `pico/uart/`) rather than
a shared `owners/` subtree — each processor's copy stays independent.
`owner_slot_pool.c/h` moved with `esp_spi_owner` (it was never really at
`espInterfaces`). Header collision resolved: `uart_owner.h` existed on both
sides, renamed to `hal_uart_esp_internal.h` / `hal_uart_pico_internal.h`.
The bare-named `esp` ESP-IDF component was renamed to `hwabstraction_esp`
via a thin wrapper directory (`idf/hwabstraction_esp/`) rather than moving
the real sources, since other trees key off `esp/`'s exact path.

**Phase 1b — adapt. DONE.** Each moved owner implements its Phase-0
interface. Notable path taken (not the obvious one): `hal_uart_attach()`
was introduced as a transitional shim, then deleted once `uart_owner_t`
grew a real embedded `hal_uart_t` (2026-09-06 "uart collapse") — one handle
per port now, not two. `uart_owner_task()`'s whole request/reply queue
(TX and the already-dead RX branch) is gone; surviving tasks are
`uart_owner_event_task` and `uart_protocol_rx_task`. ESP-side SPI/I2C were
each rewritten from an initial "duplicates the owner" draft into a thin
adapter that calls the real owner (`hal_spi_esp.c`, `hal_i2c_esp.c`) — see
`firmware/hwAbstraction/README.md`'s "adopt" convention for the bus-sharing
rule this produced (`hal_spi_bus_adopt()` for MAX31856 vs. the display
owner). Pico SPI got the same thin-adapter treatment
(`hal_spi_pico.c` over `spi_owner.c`); Pico UART's `uart_owner.c` stays as
the backend edge itself (already below `hal_uart.h`, not above it — no
conversion needed or done). relay_owner, current_sense/current_task,
FT6336U and MAX31856 (both processors) are now real clients. Full
per-driver adapter notes (interface mismatches found, e.g. `hal_i2c.h`
having no device-detach primitive) live in each backend file's own
`// INTERFACE MISMATCH` comment, not here.

**Phase 2 — host backend. DONE.** All 12 fakes (spi/i2c/uart/gpio/adc/kv/
time/flash/scratch/wdt/pwm/sysinfo) implemented, each with a proven
negative test (~785+ assertions total). `/WX` builds clean alongside `/W3`.
Real drivers now host-tested against fakes where a client exists
(MAX31856 on both processors, FT6336U, current_sense, relay_owner,
config_store_flash, boot_guard, kiln_cfg_store, and more — see git log for
the full list). Two "obvious-looking, actually wrong" ports were tried and
explicitly abandoned rather than forced: porting `test_esp_spi_owner.c`
onto fake_spi before `esp_spi_owner.c` called into `hal_spi_*` (would have
built a second, disconnected harness — done for real once the Phase 1b
adapter landed), and porting the two Pico owners onto fake_spi/fake_uart
while they were still either below the HAL already or had no HAL client at
all (same reasoning — done for real once `hal_spi_pico.c` existed).

**Phase 3 — absorb the stragglers. DONE, all 8 items:**
1. hal_scratch (pico) — direct scratch pokers become clients; slot-4
   reservation is a checked refusal, not a comment.
2. hal_flash (pico) + config_store_flash rebase — ARMED gate, seq/CRC log,
   REFUSE policy all preserved at the hal_flash layer.
3. hal_kv (ESP), full 19-file NVS migration — including the `hal_kv_mount_probe()`
   fix (a diagnostics-only read path must never itself erase a partition;
   `nvs_report.c` was doing so via `hal_kv_init_partition()`'s erase-retry)
   and the `zone_normals_cfg` 16-char-key defect (see the README's
   Conventions section — this was a real, previously-silent persistence bug
   found by this migration, not a HAL-only concern).
4. hal_sysinfo — dashboard/diagnostics/partition-info/OTA/crash-report/boot
   reset-reason and build-descriptor reads consolidated. `main_network_http.c`
   (needs `esp_partition_t::subtype`), `main_boot_early.c`'s
   `esp_core_dump_image_check()` (three-way outcome, not just bool) and
   `ui_page_diagnostics.c`'s `esp_chip_info()` stay out of scope by design.
5. hal_time — 44 `esp_timer_get_time` sites across 19 files migrated; no
   stragglers remain outside the deliberate `wifi_prov`/`lvgl_port`
   timer-object allowlist (those own real periodic `esp_timer_create`
   objects, out of scope for a get-time-only swap).
6. hal_gpio stragglers — boot_button, monitor_task, gpio_probe,
   panel_spi_bringup (`340557a`), safety_link.c's three GPIO calls.
7. hal_wdt/hal_pwm (ESP) — backlight_pwm.c and watchdog_cfg.c migrated;
   `esp_restart()` sites migrated to `hal_wdt_reboot()` except
   `ota_http_esp.c`'s rollback reboot (goes through
   `esp_ota_mark_app_invalid_rollback_and_reboot()`, an OTA write path, out
   of scope). board_temps.c migrated to hal_sysinfo's temp reader, retiring
   a second `temperature_sensor_handle_t` ownership hazard.
8. hal_wdt/hal_gpio (pico) — watchdog_task, update_task (plus its later
   hal_flash rebase for `flash_range_erase/_program`/XIP_BASE, adding
   `hal_flash_map()` for zero-copy XIP-style reads), discrete_task,
   max31856.c. Remaining named holdouts: main.c, console_uart.c,
   thermo_task.c (see README).

**Phase 4 — enforcement (`check_hal_include_boundary.ps1`). DONE, both
stages.** Ratchet stage: count-based baseline for `driver/`, `hardware/`,
`nvs.h`, `nvs_flash.h`, `esp_timer.h`; strict per-file allowlists from day
one for `esp_ota_ops.h` and `esp_wifi.h`/`esp_netif.h` (small, self-contained
sets). Strict stage step 1: `driver/*.h` (split per header), `hardware/`
and `esp_timer.h` converted to strict per-file allowlists. Strict stage
step 2: the last two count-ratchet headers, `nvs.h`/`nvs_flash.h`, also
converted to strict per-file allowlists (one `nvs.h` site, three
`nvs_flash.h` sites, all documented `// NVS_DEFAULT_PART_NAME`-only
holdouts). `$RatchetHeaders` is now empty but the ratchet mechanism itself
is kept (shared with `hwAbstraction/**`'s own upward-scan and exercised by
the negative test). `test_check_hal_include_boundary.ps1` was rewritten for
the strict allowlists as part of step 2 (assertions 7/8) — the step-1 TODO
to do this is closed. Negative-tested at every stage (inject a
non-allowlisted include, confirm the check fails naming the file; revert,
confirm it passes).

## `drivers/` layering (KilnFW only) — DONE

`App/drivers/` (332 files, most not drivers) reorganised into `App/drivers/
{hw,owners,control,safety,persist,net,http,ui,bridge,sim,common}/`
subdirectories of the *same* ESP-IDF component (`9f18ca5`, 359 renames) —
not sibling `App/<layer>/` components, to avoid `REQUIRES`/Kconfig/README
churn for no layering benefit. Allowed include direction is strictly
downward: `ui/http/bridge/sim` → `control/safety/persist/net` →
`owners/hw` → `common` → `hwAbstraction`. All 13 upward-include violations
found in the pre-move dry run (`tools/drivers_reorg/DRYRUN.md`) were fixed
as small, behavior-preserving header splits before the directory move —
e.g. `zones_http.h`'s config accessors split into `zones_config_accessors.h`
(51 includers switched), `ota_http.h`'s interlock/query functions split into
`ota_state.h`, `profiles_http.h`'s type shape split into `profiles_types.h`.
Full per-fix rationale: `tools/drivers_reorg/DRYRUN.md` section 5; the two
`sim_backend.c` findings that section's 2026-09-05 update flagged as
"proposed, not yet implemented" were implemented the same day (fixes 2 and
3: `zones_config_query.h` and `wifi_provision_state.h`'s
`wifi_provision_get_httpd_handle()` — done, see DRYRUN.md items 11 and 13
for the two IDF-`httpd_handle_t`-typedef-collision details that shaped the
final fix).

## Deliberately out of scope

- Wi-Fi/httpd/LVGL/heap portability (future hal_wifi/hal_heap, not planned).
- time_sync (SNTP), ota_pico_relay, OTA partition writes, mdns (unused).
- No vtables/function pointers. Link-time backend selection, one backend
  per build.
- `firmware/UnitTestFw` (owner decision — do not re-propose deletion).
- SaftyFW bootloader (bare-metal, ~64K budget, no `flash_safe_execute`, raw
  uart1 putc/getc) — same treatment later, separate pass; shares only
  `flash_layout.h`. Its reversed GPIO init order (`bootloader/main.c:189-191`,
  set_dir-then-put) is noted but out of scope here.
- ESP i2c/spi bus init in `main_boot_early.c` stays put until the HAL owns
  bus bring-up (would be a Phase 5+ decision; none proposed).

## Risks (still relevant if this work resumes)

- Opaque storage sizes are a header ABI: shrinking a reservation later
  touches every consumer struct. Sized generously in Phase 0 for this
  reason.
- Fake fidelity trap: a fake that models no lock and no quantization
  re-creates the documented host-test blind-spot classes (idealized-input
  bug class). The fake specs were written as requirements for this reason,
  not aspirations.
- Concurrent sessions shared this tree throughout; several Phase 3 items
  were caught mid-flight by unrelated sessions' own migrations on files
  outside a given slice (see the "Concurrent sessions git race" project
  memory) — a full clean build_host_tests.ps1 run was captured once things
  quieted rather than trusted from a run that raced other in-flight edits.
- hal_wdt reboot: `watchdog_reboot(pc, sp, delay)` with `pc≠0` overwrites
  scratch[4..7], which hal_scratch's slot map owns. The only caller passes
  `pc=0`; the wrapper takes no pc/sp arguments so the hazard can't be
  reintroduced through the HAL.
- GPIO boot-glitch fix (level-then-config) is a real behavior change on the
  three sites that had it backwards — verify on hardware rather than
  assuming timing is unchanged (folds into the "Open" hardware-timing item
  above).
