# Hardware Abstraction Tree

Status: completed 2026-09-06 (opened 2026-09-05). Conventions, interface
shapes, holdout list and allowlist policy that a future reader needs live in
`firmware/hwAbstraction/README.md` ("Conventions" section) — this doc keeps
the goal, tree shape, decisions, changelog, and remaining open items.

## Still open

- ~~Pico flash~~ — done 2026-09-06: S8 committed as `1d6198e`, Pico
  flashed `c7f0ed5` from a clean worktree (`C:/wt/pico-flash`,
  `debug_program(peer="pico", elf_path=...)`), link up, protocol v10, no
  trip. ESP side `a410edd` earlier the same day.
- **Bench timing re-verification**, never confirmed after the Phase 1b/
  2026-09-06 uart collapse: safety-link reply timing, display frame time,
  thermo read latency under a full-screen redraw. Host tests cannot see
  this — it needs a bench run. Budget for reference: reply window 345 ms,
  worst-case measured-on-paper reply flight ≈40 ms (see git history / commit
  `274afff` and friends for the arithmetic if it needs re-deriving). Also
  folds in the GPIO boot-glitch fix (level-then-config) on the three sites
  that had it backwards — a real behavior change to verify on hardware.

  2026-09-06 attempt (kilnctrl MCP only): heap/crash check clean
  (`get_heap_status`: no unacknowledged crash report, uptime 10199 s).
  `safety_get_link_stats()` exposes only cumulative counters (sent/received/
  timeouts/etc.), no latency field. Timed 101x `safety_ping` via one
  `kiln_batch` end-to-end: ~592 ms/call average — but that includes this
  MCP client's own call overhead on top of the actual UART round trip, so it
  is NOT comparable to the pre-HAL 345 ms budget / ≈40 ms paper figure and is
  not reported as a like-for-like reply-latency number. No board-exposed
  metric isolates link-only reply time; a bench run with an external timer
  (e.g. saleae, currently not connected this session) is still needed.
  Display frame time and thermo read latency: no `kiln_find` hit exposes a
  frame/flush-time or read-latency metric (`thermo_read` returns a value, no
  timing; no lvgl/display diagnostics tool exists in this server's 147
  tools). No measurement path via kilnctrl MCP today — still open, and would
  need either a new diagnostic endpoint or saleae/logic-analyzer capture.
- **Remaining SaftyFW hardware/ includes**: `main.c`, `console_uart.c`,
  `thermo_task.c` (see `firmware/hwAbstraction/README.md`).

Everything else named in this doc as a deliberate, permanent holdout
(`firmware/hwAbstraction/README.md`'s "Permanent holdouts" list — Wi-Fi/
httpd/LVGL/heap portability, OTA partition writes, the SaftyFW bootloader,
`firmware/UnitTestFw`) is owner-decided out of scope, not open work.

## Goal

One explicit hardware-abstraction tree serving both processors, so that
(a) porting off the ESP32-S3 or RP2040 is a backend swap, not a rewrite, and
(b) host tests link one honest fake backend instead of parse-only stub
headers plus hand-picked source lists.

## Tree shape

See `firmware/hwAbstraction/README.md` for the current directory layout —
it is kept there since it changes with the tree, not with this doc.

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

## Changelog

- **Phase 0 — scaffold + contract (`c626727`).** hal_status.h, interface
  headers for spi/i2c/uart/gpio/adc with the opaque-storage pattern,
  `/std:c11` demonstrated and negative-tested via `compile_headers.ps1`.
- **Phase 1a — move only.** espInterfaces owners relocated per-interface,
  colocated under each processor's own HAL backend directory rather than a
  shared `owners/` subtree. Header collision (`uart_owner.h` on both sides)
  resolved by renaming to `hal_uart_esp_internal.h` / `hal_uart_pico_internal.h`.
  The bare-named `esp` ESP-IDF component renamed to `hwabstraction_esp` via
  a thin wrapper directory.
- **Phase 1b — adapt.** Each moved owner implements its Phase-0 interface.
  `hal_uart_attach()` was a transitional shim, deleted once `uart_owner_t`
  grew a real embedded `hal_uart_t` (2026-09-06 "uart collapse"). ESP-side
  SPI/I2C rewritten into thin adapters over the real owners (`hal_spi_esp.c`,
  `hal_i2c_esp.c`); Pico SPI got the same treatment (`hal_spi_pico.c`).
  Pico UART's `uart_owner.c` stays as the backend edge itself. relay_owner,
  current_sense/current_task, FT6336U and MAX31856 (both processors) are
  real clients. Per-driver adapter notes live in each backend file's own
  `// INTERFACE MISMATCH` comment.
- **Phase 2 — host backend.** All 12 fakes (spi/i2c/uart/gpio/adc/kv/time/
  flash/scratch/wdt/pwm/sysinfo) implemented, each with a proven negative
  test (~785+ assertions total). `/WX` builds clean alongside `/W3`. Real
  drivers host-tested against fakes where a client exists.
- **Phase 3 — absorb the stragglers, all 8 items.** hal_scratch (pico);
  hal_flash (pico) + config_store_flash rebase; hal_kv (ESP), full 19-file
  NVS migration (including the `hal_kv_mount_probe()` fix — a
  diagnostics-only read path was erasing a partition via
  `hal_kv_init_partition()`'s erase-retry — and the `zone_normals_cfg`
  16-char-key defect, a real previously-silent persistence bug found by
  this migration); hal_sysinfo consolidation; hal_time (44
  `esp_timer_get_time` sites across 19 files, `wifi_prov`/`lvgl_port`
  timer-object allowlist excepted by design); hal_gpio stragglers
  (boot_button, monitor_task, gpio_probe, panel_spi_bringup `340557a`,
  safety_link.c); hal_wdt/hal_pwm (ESP: backlight_pwm.c, watchdog_cfg.c,
  `esp_restart()` sites except `ota_http_esp.c`'s OTA-write rollback reboot;
  board_temps.c migrated to hal_sysinfo's temp reader); hal_wdt/hal_gpio
  (pico: watchdog_task, update_task plus its hal_flash rebase, discrete_task,
  max31856.c).
- **Phase 4 — enforcement (`check_hal_include_boundary.ps1`), both stages.**
  Ratchet stage then strict per-file allowlists for `driver/*.h`,
  `hardware/`, `esp_timer.h`, `nvs.h`/`nvs_flash.h`, `esp_ota_ops.h`,
  `esp_wifi.h`/`esp_netif.h`. `$RatchetHeaders` is now empty but the ratchet
  mechanism itself is kept. Negative-tested at every stage. `esp_random.h`
  classified 2026-09-06 (closing the plan's last open header): the two real
  call sites migrated onto `hal_sysinfo_random_u32()`/
  `hal_sysinfo_fill_random()`; eight dead includes deleted; `esp_random.h`
  added to `$StrictHeaders` with an empty allowlist.
- **`drivers/` layering (KilnFW only).** `App/drivers/` (332 files, most not
  drivers) reorganised into `App/drivers/{hw,owners,control,safety,persist,
  net,http,ui,bridge,sim,common}/` subdirectories of the same ESP-IDF
  component (`9f18ca5`, 359 renames). Allowed include direction is strictly
  downward: `ui/http/bridge/sim` → `control/safety/persist/net` →
  `owners/hw` → `common` → `hwAbstraction`. All 13 upward-include violations
  found in the pre-move dry run (`tools/drivers_reorg/DRYRUN.md`) were fixed
  as small, behavior-preserving header splits before the move — e.g.
  `zones_http.h`'s config accessors split into `zones_config_accessors.h`,
  `ota_http.h`'s interlock/query functions split into `ota_state.h`,
  `profiles_http.h`'s type shape split into `profiles_types.h`. Full
  per-fix rationale: `tools/drivers_reorg/DRYRUN.md` section 5.

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

## Risks (still relevant if bench verification finds issues)

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
