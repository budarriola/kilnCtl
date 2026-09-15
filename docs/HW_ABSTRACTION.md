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

  2026-09-06 follow-up: `GET /api/diagnostics/timing` (`a55d791`) already
  closed the display/thermo half of this with `display_flush_us`/
  `thermo_read_us` (`lvgl_port_get_flush_stats_ex`/`MAX31856_get_read_all_
  stats`). The remaining safety-link-reply half is now instrumented the same
  way: `link_reply_us` (`{count,last,min,max,mean}`, plus a `timeouts`
  sub-field that reuses `safety_link_stats_t::timeouts` rather than
  duplicating it) is measured in `safety_exchange()`
  (`firmware/KilnFW/App/drivers/safety/safety_link_inbox.c`) with
  `hal_time_now_us()`, from the instant a `GET_STATUS` request is handed to
  `uart_protocol_send_broadcast()` to the instant `safety_drain_inbox_for_
  status()` reports the matching reply decoded. Correlation is by
  `safety_exchange()`'s own `xact_lock` serialization, not a wire seq/msg id
  — `SAFETY_CMD_GET_STATUS` carries none (see `safety_link_stats_t::
  link_reply_us_count`'s doc comment in `safety_link.h` for why that is
  still a sound match). Wired into the same `GET /api/diagnostics/timing`
  endpoint and into `get_heap_status`'s printed output
  (`tools/PcTools/src/kilnctrl/`). Host-tested in
  `test_safety_link_compile.c` against a fake inbox and `fake_time.c`'s
  scripted clock, including a negative test that breaks the drain/reply
  match and confirms the counters do NOT advance.

  **No hardware measurement exists yet** — this only adds the on-board
  metric; the board has not been reflashed with it as of this writing. Once
  flashed, `get_heap_status`'s `link_reply_us` block is what replaces the
  pre-HAL paper figures (345 ms reply window, ≈40 ms flight) and the
  contaminated MCP-timed `safety_ping` (~592 ms, client/HTTP overhead
  included) above — do not treat this paragraph itself as that measurement.

  **2026-09-14 hardware measurement, item CLOSES.** Board at `c8f7506b`
  (3 commits behind then-HEAD `bc55f4e5`, tree clean; Pico `d957d5fd`,
  dirty, protocol v14), `reset_reason='software (esp_restart)'`,
  uptime 1536-1821 s over the run, S8 rate guard commissioned 33.3 C/min
  (armed), no trip, no unacknowledged crash on either processor throughout.

  Confirmed the span first: `link_reply_us` is timestamped in
  `safety_exchange()` from send to matched reply, entirely inside the ESP —
  it does NOT fire on the routine cached-status background poll that backs
  `safety_get_status`/`safety_get_diag` (watched `link_reply_us.count` hold
  flat across 25+ s of normal cached-status polling while the link's own
  cmd histogram kept advancing underneath it). It DOES fire, one sample per
  call, on `safety_ping()` ("force an immediate poll"), which is the only
  MCP-reachable trigger for a live `safety_exchange()`. So the number is
  genuinely link+ESP-dispatch time with the MCP/HTTP client's own overhead
  excluded (unlike the 2026-09-06 ~592 ms `safety_ping`-timed-by-the-client
  figure) — confirmed, not assumed, and the fix for the reason the earlier
  measurement was reachable via MCP but not truly comparable.

  Traffic: baseline `count=1926` (accumulated since this boot before any
  MCP-driven `safety_ping`) plus ~213 more MCP-forced samples this session
  (`count` end `2139`), for `n=2139` since this boot — over the requested
  1000. Board-reported distribution at end of run: `min=6507 last=182565
  max=321187 mean=134633 timeouts=3` (µs; 3 timeouts out of 2503 `sent`
  per `safety_get_link_stats`, all arising from this session's own
  back-to-back `safety_ping` bursts — a call rate far above the link's
  normal 500 ms poll cadence — not from an ordinary caller). The firmware
  struct carries no percentile field; a hand-collected subsample of 22
  distinct individual `last` readings taken across the run (sorted, µs):
  56639, 62411, 107109, 110554, 111855, 123605, 126142, 126627, 126805,
  131965, 140179, 144278, 152937, 170986, 182565, 212234, 214374, 214746,
  221155, 221596, 255502, 321187 — median ≈142 ms, p90 ≈222 ms (20th of 22),
  p99/max ≈321 ms (subsample mean 161 ms, somewhat above the board's
  all-since-boot mean of 135 ms because the subsample over-represents the
  back-to-back-`safety_ping` bursts used to drive traffic).

  Judged against the 345 ms budget: mean (135 ms) sits comfortably inside
  it, but the observed max (321 ms, 93% of budget) does not — it climbed
  from 203 ms (pre-session baseline) to 321 ms specifically as this
  session's `safety_ping` call rate rose, i.e. tail latency is a function
  of link contention/queueing, not a fixed constant, and does not have
  much headroom left under stress. This is worse than the ≈40 ms paper
  flight estimate by roughly 8x even at the mean, confirming the paper
  figure was never a like-for-like number either. **Item closes** in the
  sense the doc asked for — link-only reply latency is now isolated from
  client overhead and measured on hardware, all samples observed stayed
  under budget, and no trip/crash resulted — but the margin is thin enough
  under load that a caller issuing back-to-back forced polls (rather than
  the normal 500 ms cadence) should not be treated as free of budget risk.
  A production consumer of `link_reply_us` should alarm on `max`, not
  `mean`, if this is ever used as a live health signal.
- ~~Remaining SaftyFW hardware/ includes~~ — closed 2026-09-06:
  `main.c`, `console_uart.c`, `thermo_task.c` re-reviewed line by line.
  `main.c`'s GPIO6-low latch/direction pair matches `hal_gpio_init_out()`'s
  shape exactly, but `relay_owner.h`/`.c` already document *why* it stays
  raw: it runs before the scheduler (and hal_gpio's own client list) exist,
  and `relay_owner_start()` re-asserts the same fail-safe default through
  `hal_gpio_init_out()` once the task starts, which is what makes the
  de-energized-at-init property host-testable at all. `main.c`'s
  TIMER_DBGPAUSE register and watchdog boot-reason/scratch[5] reads, and
  `thermo_task.c`'s DRDY IRQ registration, have no HAL primitive that fits
  without widening `hal_wdt.h`/`hal_scratch.h`/`hal_gpio.h` past what any
  real second consumer needs (`hal_gpio.h` is explicitly "clean-room, no
  IRQ surface in v1"). `console_uart.c` stays a tiny, dependency-free
  write-only diagnostic by design. All three files' allowlist entries in
  `tools/check_hal_include_boundary.ps1` are unchanged and each raw
  `#include "hardware/..."` now carries an inline one-line justification
  comment pointing at `firmware/hwAbstraction/README.md`'s "Permanent
  holdouts" list, which already named all three. Negative-tested: removing
  `thermo_task.c`'s allowlist entry makes the checker fail as expected;
  entry restored.

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
