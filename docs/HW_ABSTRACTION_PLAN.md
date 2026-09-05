# Hardware Abstraction Tree — Plan

Status: proposed 2026-09-05, detailed same day after a seven-agent research
sweep (three structural, four deep-dive). No prior HAL doc exists.
Owner decision needed before Phase 1 starts: approve tree location + naming.

## Goal

One explicit hardware-abstraction tree serving both processors, so that
(a) porting off the ESP32-S3 or RP2040 is a backend swap, not a rewrite, and
(b) host tests link one honest fake backend instead of today's parse-only
stub headers plus hand-picked source lists.

## Tree shape

```
firmware/hwAbstraction/
  interface/          portable headers only: hal_status.h hal_spi.h hal_i2c.h
                      hal_uart.h hal_gpio.h hal_kv.h hal_flash.h hal_scratch.h
                      hal_time.h hal_wdt.h hal_pwm.h hal_sysinfo.h
                      No vendor types. C11. Opaque handles, link-time backend.
  esp/                ESP-IDF backends (KilnFW), one dir per interface:
    spi/ i2c/ uart/ gpio/ kv/ time/ wdt/ pwm/ sysinfo/
  pico/               pico-sdk backends (SaftyFW):
    spi/ uart/ gpio/ adc/ flash/ scratch/ wdt/ time/
  host/               fake backends for MSVC host tests (both firmwares)
```

Builds: `esp/` becomes an ESP-IDF component added to `EXTRA_COMPONENT_DIRS`
(firmware/KilnFW/CMakeLists.txt:9). `pico/` a plain CMake static lib pulled by
firmware/SaftyFW/CMakeLists.txt. `host/` compiled by both build_host_tests.ps1
scripts, replacing the stub-header include-path trick interface by interface.

Not every interface exists on both sides, deliberately: kv is ESP-only,
flash/scratch/adc pico-only (rationale below).

## Interface design (from measured API/call-site surveys)

Common status enum `hal_status_t`: OK / TIMEOUT / BUSY / INVALID_ARG /
NO_MEM / NOT_SUPPORTED / NOT_FOUND / IO. Replaces esp_err_t vs bool split.

### hal_spi
Derived from esp_spi_owner (queued owner task, slot pool, wedge latch) and
SaftyFW spi_owner (mutex + direct blocking call — deliberately no task).
Interface must NOT assume an owner task exists; both models implement it.

```c
hal_spi_bus_init/deinit;  hal_spi_device_attach(bus, dev, cfg)  // clock, mode,
                                                  // cs_pin or HAL_CS_NONE
hal_spi_transfer(dev, tx, tx_len, rx, rx_len, timeout_ms)
hal_spi_transfer_polling(...)   // ESP: polling_transmit; pico: == transfer
hal_spi_transfer_async(dev, tx, len, timeout, cb, ctx)  // optional capability;
                                // NOT_SUPPORTED unless backend enables it
hal_spi_bus_is_wedged(bus)      // ESP latched-wedge flag; pico/host: false
```

Facts pinning this shape: only caller of async is panel_spi_blit.c:432,
feature-gated off by default; polling used only by MAX31856 (esp
MAX31856.c:168,193); CS is bit-banged on both sides (esp cs_pin<0 =
hardware CS passthrough — keep as HAL_CS_NONE). ESP contract "never store
request state on caller's stack" (slot pool) carries into any async backend.
Vendor types to hide: spi_host_device_t, Queue/Task/SemaphoreHandle_t,
spi_device_handle_t (esp_spi_owner.h:20-23,103); pico header is already
vendor-clean.

### hal_i2c
Simplest surface; ESP-only today (SaftyFW has no I2C). All three consumers
(SX1509.c:113, FT6336U.c:52, NS2009.c:51) use one identical shape:

```c
hal_i2c_bus_init/deinit;  hal_i2c_device_attach(bus, dev, addr, clock_hz)
hal_i2c_transfer(dev, tx, tx_len, rx, rx_len, timeout_ms)
```

Preserve i2c_owner.c's two hard-won behaviors in the esp backend:
static (not heap) per-call semaphore (2026-08-20 SRAM-starvation fix), and
worker-enforced timeout with unbounded caller wait (use-after-free avoidance,
i2c_owner.c:252-260). A pico backend is future work, modeled on spi_owner's
mutex pattern.

### hal_uart — adopts the PICO shape, not the ESP shape
Measured: ESP's blocking `uart_owner_transfer` is called with rx=NULL by 100%
of call sites (only uart_protocol.c:107); its reply-read path is dead code.
Pico's uart_owner is non-blocking send (whole-frame-or-drop + drop counter)
plus poll-drain receive. So the portable primitive is byte-level non-blocking:

```c
hal_uart_init(u, cfg)            // instance, tx_io, rx_io, baud
hal_uart_send(u, data, len)      // whole-buffer-or-BUSY, never partial
hal_uart_recv(u, out, max)       // non-blocking, returns 0..max
hal_uart_get_rx_error_count / get_tx_dropped / hal_uart_restart
```

uart_protocol.c (framing, CRC16, ACK/retry/dedup, broadcast) stays ABOVE the
interface unchanged, rebased onto hal_uart_send/recv. Its RX loop's
read-what's-buffered-with-zero-timeout contract (the 2026 full-buffer-block
bug fix, uart_protocol.c:328-435) must be expressible: hal_uart_recv is
already exactly that contract. ESP backend internally keeps its event task
(FIFO_OVF/BUFFER_FULL flush, frame-error counters); pico backend keeps its
IRQ SPSC rings. Neither synchronization scheme leaks through the interface.

### hal_gpio — clean-room; no owner exists on either side
```c
hal_gpio_init_out(pin, num, idle_level)  // latch set BEFORE direction switch,
                                         // both sides require never-undriven
hal_gpio_init_in(pin, num, pull)
hal_gpio_set(pin, level);  hal_gpio_get(pin)
```
Clients: CS/DC/reset/IRQ/fault pins in panel_spi, MAX31856 (both), SX1509,
safety_link, boot_button, monitor_task LED; pico relay_owner (GPIO6 —
relay_owner itself stays as the state-machine owner above hal_gpio),
discrete_task, DRDY. No interrupt-callback surface in v1 (no current client
needs one through the seam; pico IRQs stay inside backends).

### hal_kv — ESP-ONLY, wraps NVS. Pico explicitly excluded.
NVS census: 21 KilnFW modules, namespaces kiln_cfg (dominant, 13 modules) /
wifi_cfg / boot_guard / fire_stats / touch_cal / watchdog_cfg, plus named
partitions (profiles, zones load-from). API covering 100% of observed use:

```c
hal_kv_open(namespace, mode, partition_or_NULL) / close / commit
hal_kv_get/set_u8, _u32, _str, _blob   // get_blob(NULL buf) = size probe
hal_kv_init_partition(name)            // idempotent, with erase-retry
hal_kv_erase_partition(name)           // factory_reset's scoped erase
hal_kv_stats(partition, ...)           // nvs_report's enumeration
```

Write-context safety is a first-class part of the contract, not left to
callers: expose `hal_kv_write_safe_here()` (today's caller_stack_is_external
predicate) so the six copy-pasted guards and flash_worker_lint.py's three
sanctioned patterns (worker dispatch / local guard / init-time-only, plus
wifi_prov's dedicated-writer-task fourth variant) survive verbatim. The lint
is updated to key on hal_kv_set_* instead of nvs_set_*. Versioned-blob
migration chains (zones v19, kiln_cfg v2, profiles per-slot, wifi legacy
list) are call-site logic on top of get_blob — no new primitive.

Why pico is excluded: config_store is not a KV store. It is a fixed 512-byte
record, seq-numbered, CRC'd, 8-slot round-robin log in one 4K sector, with a
hard safety interlock (write refused while relay ARMED via
relay_owner_get_state()). Forcing it through hal_kv would either strip the
ARMED gate to a caller-side check or bloat the interface with slot/seq/ARMED
concepts one platform needs. It sits on hal_flash instead.

### hal_flash — pico backend for config_store_flash.c
```c
HAL_FLASH_ERASE_SIZE (4096) / HAL_FLASH_PROGRAM_SIZE constants
hal_flash_read(offset, buf, len)       // pico: XIP pointer read, no lockout
hal_flash_erase(offset, len); hal_flash_program(offset, buf, len)
hal_flash_safe_execute(cb, arg, timeout_ms)  // pico: flash_safe_execute
                                             // (multicore lockout); host: direct call
```
safe_execute is first-class in the interface — the multicore-XIP hazard is
real on RP2040 and must not be assumed away. Distinct failure reasons
(TIMEOUT / NOT_PERMITTED / INSUFFICIENT_RESOURCES) map onto hal_status_t.
This brings config_store_flash.c under host test for the first time (today it
is the one persistence file with zero off-target coverage).

### hal_scratch — pico watchdog-scratch registry
All 8 RP2040 scratch slots are claimed today (startup_diag.h:13-28 is the
budget table): [0]/[1] trip reason+magic, [2]/[3] startup diag+magic,
[4] RESERVED pico-sdk watchdog_enable (fenced by comment only!), [5] shared
by watchdog_overdue_diag (tag 0xD9) and stack-overflow hook (tag 0xE3,
mutually exclusive by construction), [6] boot-stage marker (overwrite-only),
[7] clear_trip packed checkpoint. Registry: static claim table
{slot, owner, tag} with compile-time uniqueness assert (same pattern as
stack_margin registration); slot 4 hard-reserved in code, not comment; typed
accessors write_u32/read_u32(&magic_ok)/clear supporting both access modes
(write-then-clear-at-boot, and [6]'s overwrite-every-stage). The four direct
pokers (main.c, boot_reason.c, watchdog_overdue_diag.c, clear_trip_diag.c)
become clients.

### hal_time / hal_wdt / hal_pwm / hal_sysinfo
- hal_time: monotonic us/ms. Wraps esp_timer_get_time (~15 modules) and pico
  time. Mass-mechanical migration; host fake = controllable clock with
  advance-by-N and injectable jumps (clock_health tests).
- hal_sysinfo: reset reason + running-partition/build info, one place; dedupes
  crash_report.c, ui_page_diagnostics.c:520, dashboard/partition_info/
  main_network_http's independent esp_ota_get_running_partition calls.
- hal_wdt: esp task-WDT reconfigure (watchdog_cfg.c sole user) and pico
  watchdog_enable/update/reboot. Thin.
- hal_pwm: backlight LEDC only. Thin, last.

## Host fakes (Phase 2 specs, from stub-usage survey)

Only test_esp_spi_owner.c asserts on hardware stub state today (g_stub_spi_*
counters, gpio set_level count, FreeRTOS queue-ring stub driving
spi_owner_task synchronously). Fakes must preserve those observable
behaviors, then exceed the stubs:

- fake_spi (must): ordered transfer record (buf/len/flags/cs/polling-vs-
  queued); injectable enqueue-timeout, completion-timeout, pool exhaustion
  with distinct side effects (wedge latch vs not, refcount invariants);
  synchronous run-owner-loop driver. Should: async callback with
  fire-after-LAST-chunk semantics (an existing test guards that mutant).
- fake_i2c (must): ordered record/replay, NACK/timeout injection.
- fake_uart (must): real ring fill/drain model — backpressure and drop-
  counter behavior testable; synchronous drive of the owner path. This
  finally creates the "PRODUCTION SEAM MISSING" substitution point
  test_safety_guards.c:1100-1139 documents for uart_owner_send.
- fake_gpio (must): per-pin level history (order-checkable CS sequencing),
  link-shared state instead of today's fragile static-per-TU counters.
- fake_kv (must): RAM namespace/key store, reset between tests; error
  injection (wrong-type, corruption, no-space) for config fallback paths.
- fake_time (must): explicit advance; support quantized/dithered inputs —
  the idealized-input bug class (unquantized synthetic data hides branches)
  is documented across adaptive_tune/autotune tests; fakes must not reintroduce it.
- Cross-cutting: expose a current-caller identity hook (parity with
  bx_caller_is_worker_task) so re-entrancy/lock-order bugs the single-
  threaded host cannot exercise are at least assertable.

## Migration inventories (measured)

- espInterfaces move set: esp_spi_owner, i2c_owner, uart_owner,
  uart_protocol (+ owner_slot_pool). These become hwAbstraction/esp backends
  plus the retained protocol layer above.
- uart_owner.c is the only ESP file needing a real rewrite for hal_uart
  (uart_driver_install/param_config/set_pin/write_bytes/wait_tx_done/
  flush_input/driver_delete + event queue). uart_protocol.c changes only its
  two raw calls (uart_get_buffered_data_len, uart_read_bytes → hal_uart_recv).
- safety_link.c rebase is small: three raw GPIO calls (gpio_config:409,
  gpio_set_level:315,414, gpio_set_pull_mode:493) → hal_gpio; its UART use is
  already entirely through uart_owner/uart_protocol.
- console_uart.c (pico, 51 lines, write-only, no IRQ): second hal_uart
  instance with a blocking-write capability — or, cheaper, leave it: it is
  dependency-free by design (boot banner must work before anything else).
  Decision at Phase 3: leave unless the enforcement check needs it moved.
  Note stdio does NOT route through it (pico_enable_stdio_uart 0).
- NVS migration set: 21 modules (census in agent report; kiln_cfg namespace
  13 of them). Mechanical, but flash_worker_lint.py allowlist migrates in
  the same commit.

## Phases

Each phase compiles, passes run_all_checks.ps1, host tests, AND `build_kilnfw`
+ SaftyFW target build before commit (host tests are not a target build).
Negative-test every new check and every fake (prove it can fail). Prefix-
rename every symbol widened from static even when grep is clean.

**Phase 0 — scaffold + contract.** Tree, hal_status.h, and interface headers
for spi/i2c/uart/gpio exactly as sketched above. Threading/ownership contract
doc-commented per header, copied from the owner headers (single-writer,
buffer-copy-in, drop policy, wedge semantics). No callers change. Add new
source roots to check_c_files_in_cmakelists.ps1.

**Phase 1 — move the proven owners.** Relocate espInterfaces/* →
hwAbstraction/esp/, SaftyFW spi_owner/uart_owner (+ new gpio backend) →
hwAbstraction/pico/, each adapted to implement the Phase-0 interface (keep
bodies; change types at the edge). relay_owner stays where it is (app-level
state machine, becomes a hal_gpio client). Update every path-keyed script in
the SAME commit: both build_host_tests.ps1, check_c_files_in_cmakelists,
check_duplicate_symbols (component-name-derived obj path),
check_no_duplicate_crc (names espInterfaces literally), check_safety_baud_sync
(names uart_owner.c path), check_relay_writes_through_owner,
check_safety_call_results_checked (allowlist + path-prefix reconstruction),
check_host_embed_symbols_defined, check_stack_margin_registration, SaftyFW
check_isolation family, ARCHITECTURE.md citations. Move-only diff, nothing
else. Land when no other session is mid-edit in drivers/.

**Phase 2 — host backend.** Implement hwAbstraction/host/ per the fake specs.
Switch both build_host_tests.ps1 to link fakes for migrated interfaces;
port test_esp_spi_owner.c off g_stub_* onto fake_spi's records. New tests
unlocked immediately: pico spi_owner against fake_spi, pico uart_owner ring
logic against fake_uart, config_store_flash against fake hal_flash.

**Phase 3 — absorb the stragglers, one interface per commit:**
1. hal_scratch (pico) — highest safety value, smallest diff; slot-4 code
   reservation closes a real fenced-by-comment-only hazard.
2. hal_flash (pico) + config_store_flash rebase + first host tests for it.
3. hal_kv (esp) + 21-module mechanical migration + flash_worker_lint rekey.
4. hal_sysinfo — dedupe partition/reset-reason call sites.
5. hal_time — mass mechanical.
6. safety_link.c's three GPIO calls; boot_button, monitor_task, gpio_probe.
7. hal_wdt, hal_pwm, board_temps. Low value; skip if effort budget runs out.
   console_uart: decide (lean = leave, with an enforcement allowlist entry).

**Phase 4 — enforcement.** New check: no driver/*.h, hardware/*.h, nvs.h,
esp_wifi.h, esp_timer.h, esp_ota_ops.h includes outside
hwAbstraction/{esp,pico} — allowlist: main_boot_early.c bus init (until
migrated), wifi_prov family (out of scope), lvgl_port/heap_caps (out of
scope), console_uart.c if left. Negative-test it. Retire superseded stub
headers. Update flash_worker_lint call-pattern keys.

## Deliberately out of scope

- Wi-Fi/httpd/LVGL/heap portability. wifi_prov is already a sole owner
  (self-declared, wifi_prov.c:473); esp_heap_caps in ~30 files is pervasive
  but a heap seam pays nothing until a real second target exists. Noted as
  future hal_wifi/hal_heap; not built now.
- No vtables/function pointers. Link-time backend selection, one backend per
  build — zero runtime cost, matches safety-code style.
- UnitTestFw's espInterfaces copy (third copy, unrelated rig) — untouched;
  check_no_duplicate_crc's exclusion entries for it stay.
- SaftyFW bootloader (own flash/watchdog/uart use) — same treatment later,
  separate pass; flash_layout.h constants shared with hal_flash from day one.
- ESP i2c bus init in main_boot_early.c stays put until hal_i2c owns bus
  bring-up (Phase 3+ decision).

## Risks

- Phase 1 is the "splits break filename-keyed checks" class at maximum size:
  full grep for old filenames AND renamed identifiers, then run every check
  script, not just builds. Nine of twelve prior splits broke a check silently.
- Owner structs embed FreeRTOS/IDF types in public headers (i2c_owner.h:20-23
  worst); interfaces hide them behind opaque structs sized per-backend — get
  the opaque-storage pattern (aligned max-size byte array vs per-backend
  header) decided in Phase 0, it is hard to change later.
- hal_uart's whole-frame-or-drop contract on ESP means uart_protocol's
  blocking-send behavior moves up a layer; verify safety-poll timing budget
  (~345 ms reply window) unchanged on hardware after Phase 1, not just host.
- fake fidelity trap: a fake that models no lock and no quantization
  re-creates the three documented host-test blind-spot classes. The fake
  specs above are requirements, not niceties.
- Concurrent sessions share this tree; Phase 1 and the hal_kv migration are
  the two land-alone diffs.
