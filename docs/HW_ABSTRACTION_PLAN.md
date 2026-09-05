# Hardware Abstraction Tree — Plan

Status: proposed 2026-09-05. No prior HAL doc exists; this is net-new direction.
Owner decision needed before Phase 1 starts: approve tree location + naming.

## Goal

One explicit hardware-abstraction tree serving both processors, so that
(a) porting off the ESP32-S3 or RP2040 is a backend swap, not a rewrite, and
(b) host tests link one honest fake backend instead of today's parse-only
stub headers plus hand-picked source lists.

## Tree shape

```
firmware/hwAbstraction/
  interface/          portable headers only: hw_spi.h hw_i2c.h hw_uart.h hw_gpio.h
                      hw_pwm.h hw_kv.h (NVS-like) hw_ota.h hw_wdt.h hw_time.h
                      hw_heap.h hw_sysinfo.h (reset reason, partitions) hw_scratch.h
                      No vendor types. C11, no allocation in the interface itself.
  esp/                ESP-IDF backends (KilnFW). One dir per interface:
    spi/ i2c/ uart/ gpio/ pwm/ kv/ ota/ wdt/ time/ heap/ sysinfo/ wifi/
  pico/               pico-sdk backends (SaftyFW):
    spi/ uart/ gpio/ adc/ wdt/ flash/ scratch/ time/
  host/               fake backends for MSVC host tests (both firmwares):
    record/replay transfer fakes, RAM kv store, fake clock, scratch array
```

Builds: `esp/` becomes an ESP-IDF component added to `EXTRA_COMPONENT_DIRS`
(firmware/KilnFW/CMakeLists.txt:9). `pico/` a plain CMake static lib pulled by
firmware/SaftyFW/CMakeLists.txt. `host/` compiled by both build_host_tests.ps1
scripts in place of the stub-header include-path trick, incrementally.

## What research established (2026-09-05, three-agent sweep)

- KilnFW already has an unlabeled proto-HAL: `App/drivers/espInterfaces/`
  (esp_spi_owner, i2c_owner, uart_owner, uart_protocol). Consumers call
  `*_owner_transfer`, not IDF. These move/rename into `hwAbstraction/esp/`.
- SaftyFW likewise: `spi_owner.c`, `tasks/uart_owner.c`, `relay_owner.c` are
  clean single-owner seams; `safety_core.c` and all guard/policy files are
  already pure.
- Worst un-abstracted surfaces (ESP): NVS — ~20 modules open namespaces
  directly, no seam; `esp_timer_get_time` and `esp_heap_caps.h` in ~30 files;
  partition/reset-reason reporting re-implemented in 3-4 places;
  `safety_link*.c` runs its own raw UART+GPIO stack parallel to uart_owner.
- Worst un-abstracted (pico): four files (main.c, boot_reason.c,
  watchdog_overdue_diag.c, clear_trip_diag.c) share watchdog scratch registers
  by convention only; console_uart.c duplicates uart_owner's init pattern;
  config_store_flash.c has no host-side coverage at all.
- Path blast radius on any move (known break class — see CLAUDE.md splits
  section): both build_host_tests.ps1 (literal file lists), plus
  check_c_files_in_cmakelists, check_duplicate_symbols, check_no_duplicate_crc
  (names espInterfaces literally), check_safety_baud_sync,
  check_relay_writes_through_owner, check_safety_call_results_checked,
  check_host_embed_symbols_defined, check_stack_margin_registration, SaftyFW
  tools/check_isolation family, and ARCHITECTURE.md `App/drivers/foo.c:line`
  citations. run_all_checks.ps1 itself is discovery-based, unaffected.
- `firmware/UnitTestFw/.../espInterfaces/` is a third copy for the unrelated
  test-rig board. Decision: leave it alone in this plan; it may adopt the tree
  later.

## Phases

Each phase compiles, passes run_all_checks.ps1, host tests, AND `build_kilnfw`
+ SaftyFW target build before commit (host tests are not a target build).
Negative-test every new check or fake (prove it can fail). Renamed symbols get
prefix-renamed even when grep is clean.

**Phase 0 — scaffold + contract.** Create tree, write `interface/` headers for
spi/i2c/uart/gpio only (the proven seams). Doc comment per header states
threading/ownership contract copied from the existing owner headers. No
callers change. Add the new source roots to check_c_files_in_cmakelists.

**Phase 1 — move the proven owners.** Relocate espInterfaces/* to
hwAbstraction/esp/, SaftyFW spi_owner/uart_owner/relay_owner(+ a new gpio
backend wrapping it) to hwAbstraction/pico/, adapting each to implement the
Phase-0 interface (thin: keep bodies, change signatures/types at the edge).
Update every path-keyed script named above in the same commit. This is the
high-blast-radius phase; do it alone, nothing else in the diff.

**Phase 2 — host backend.** Implement hwAbstraction/host/ fakes for the
Phase-0 interfaces (transfer record/replay, RAM kv). Switch the two
build_host_tests.ps1 scripts to link fakes instead of stub headers for the
migrated interfaces; leave remaining stubs for not-yet-migrated surfaces.
Brings config_store_flash.c (pico flash backend) under host test for the
first time.

**Phase 3 — absorb the stragglers, one interface each:**
- `hw_kv`: ESP NVS wrapper; migrate the ~20 direct-NVS modules mechanically.
  Pico side: config_store_flash behind `hw_flash`/`hw_kv`.
- `hw_scratch`: pico watchdog-scratch registry owning slot assignment; the
  four direct pokers become clients.
- `hw_time`: wrap esp_timer_get_time / pico time; mass mechanical migration.
- `hw_sysinfo`: one place for reset reason + running partition; dedupe the 3-4
  reporting call sites.
- safety_link*.c onto hw_uart + hw_gpio (its own dedicated port instance).
- console_uart.c onto hw_uart (second pico instance).
- backlight_pwm (hw_pwm), board_temps, watchdog_cfg (hw_wdt) — single-file,
  low value, do last or skip.

**Phase 4 — enforcement.** New check: no `driver/*.h`, `hardware/*.h`,
`nvs.h`, `esp_wifi.h` etc. includes outside hwAbstraction/{esp,pico}
(allowlist for main_boot_early bus init until it too migrates). Negative-test
it. Retire superseded stub headers.

## Deliberately out of scope

- Wi-Fi/httpd/LVGL portability. wifi_prov is already a sole owner; a portable
  Wi-Fi interface only pays off when a real second target exists. Note as
  future `hwAbstraction/interface/hw_wifi.h`, do not build now.
- No function-pointer vtables. Link-time backend selection (one backend per
  build) — zero runtime cost, matches safety-code style.
- UnitTestFw's espInterfaces copy.
- Bootloader (SaftyFW/bootloader) — same treatment later, separate pass.

## Risks

- Phase 1 file moves are exactly the "splits break filename-keyed checks"
  class: budget a full grep for old names + identifiers, then run every
  check script, not just builds.
- Owner modules embed FreeRTOS types in public structs (i2c_owner.h); the
  interface must hide them (opaque handle) or host fakes drag FreeRTOS in.
- Concurrent sessions share this tree: land Phase 1 when no other session is
  mid-edit in drivers/ (thermal_guard pass was live 2026-09-04).
