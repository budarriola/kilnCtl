# Hardware Abstraction Tree — Plan

Status: proposed 2026-09-05. Five research/verification rounds the same day
(7 + 6 + 10 + 6 + 7 sonnet agents: structural surveys, call-site censuses,
boot-order and RAM audits, header drafts, error-code mapping, host-build
shape, enforcement-check census, then a "what has not been double-checked"
pass that compiled the header drafts on MSVC, re-censused NVS and scratch
use, read the IDF GPIO driver source, recomputed the link budget and audited
the drivers/ directory). Every number below is measured against the tree at
commit `c43323a` unless marked otherwise. No prior HAL doc exists.

Owner decision taken 2026-09-05: keep firmware/UnitTestFw untouched (it has
no build or flash consumers and its espInterfaces copy has diverged by
786/576/225 lines, but it stays).

Owner decisions taken 2026-09-05 (second round): the drivers/ layering
target layout is approved as written; order of work is untangle the six
upward includes, then the directory reorg, then HAL Phase 1a; no move starts
until the currently dirty KilnFW files land (wait, do not commit them from
this pass); plan only for now — no code changes yet. Third round, same day:
tree location and naming approved as `firmware/hwAbstraction/{interface,
esp,pico,host}`; opaque-storage option a (fixed `_Alignas(8)` storage in the
header, backend `_Static_assert`) approved; hal_uart two-primitive shape
(`send` + `send_blocking`) approved. No owner decision remains open; the
plan is complete and waits only on the dirty-tree condition above.

## Goal

One explicit hardware-abstraction tree serving both processors, so that
(a) porting off the ESP32-S3 or RP2040 is a backend swap, not a rewrite, and
(b) host tests link one honest fake backend instead of today's parse-only
stub headers plus hand-picked source lists.

## Tree shape

```
firmware/hwAbstraction/
  interface/          portable headers only: hal_status.h hal_spi.h hal_i2c.h
                      hal_uart.h hal_gpio.h hal_adc.h hal_kv.h hal_flash.h
                      hal_scratch.h hal_time.h hal_wdt.h hal_pwm.h hal_sysinfo.h
                      No vendor types. C11 (stdalign.h, _Static_assert).
                      Opaque handles, link-time backend, no vtables.
  esp/                ESP-IDF backends (KilnFW), one dir per interface:
    spi/ i2c/ uart/ gpio/ kv/ time/ wdt/ pwm/ sysinfo/
    board_kiln_s3.h   board descriptor (forwards CONFIG_KILNCTL_*, see below)
  pico/               pico-sdk backends (SaftyFW):
    spi/ uart/ gpio/ adc/ flash/ scratch/ wdt/ time/
    board_safety_rp2040.h
  host/               fake backends for MSVC host tests (both firmwares)
```

Builds: `esp/` becomes an ESP-IDF component added to `EXTRA_COMPONENT_DIRS`
(firmware/KilnFW/CMakeLists.txt:10). `pico/` a plain CMake static lib pulled
by firmware/SaftyFW/CMakeLists.txt. Verified shape (2026-09-05): SaftyFW
builds three app executables — `SaftyFW` (:149), `SaftyFW_slotA` and
`SaftyFW_slotB` (:414-415 via `saftyfw_add_slot_executable` :334-403) — with
identical link lists (:199-214 vs :364-379) and one FreeRTOSConfig.h, so a
single `add_library(hwabstraction_pico STATIC ...)` with
`target_link_libraries(... PUBLIC hardware_gpio hardware_irq hardware_uart
hardware_spi hardware_adc hardware_flash pico_flash pico_multicore
FreeRTOS-Kernel FreeRTOS-Kernel-Heap4)` is linked by all three. A STATIC lib
is safe because the only runtime registration is `irq_set_exclusive_handler`
(uart_owner.c:259), called from init, not a constructor. The bootloader has
its own CMakeLists and no FreeRTOS; it is excluded and keeps its raw SDK
calls. `host/` is
compiled by both build_host_tests.ps1 scripts (Option A below), replacing the
stub-header include-path trick interface by interface.

Not every interface exists on both sides, deliberately: kv is ESP-only;
flash/scratch/adc pico-only (rationale in each section).

## Interface design

### hal_status_t

The eight-value sketch from round 1 was checked against every consumer that
branches on a specific error code. Four additions are required, three
retentions confirmed:

| value | why it must exist |
|---|---|
| OK, TIMEOUT, BUSY, INVALID_ARG, NO_MEM, IO | baseline |
| WEDGED | esp_spi_owner's latched wedge (esp_spi_owner.c:394,559) is a distinct, sticky state; must not be folded into INVALID_STATE/NOT_READY |
| NOT_READY | the pervasive `!initialized` guards in every owner; distinct from WEDGED (recoverable by init vs not) |
| VERIFY_FAILED | ESP_ERR_INVALID_RESPONSE on register read-back mismatch (SX1509, MAX31856); callers retry on this and abort on IO |
| INVALID_SIZE | panel_spi_blit.c:324 vs :335 distinguish "wrong length" from "bad argument" in the done_cb contract |
| NOT_FOUND | probe loops continue-on-NOT_FOUND (SX1509.c:629); merging into IO would abort the scan |
| NOT_SUPPORTED | panel_spi_blit.c:434 async fallback; uart_owner.c:154 |

ESP_FAIL and any raw IDF code not in the table map to IO. Add
`const char *hal_status_to_name(hal_status_t)`: gpio_probe forwards
`esp_err_to_name()` as a **string** to PcTools (devices_gpio_probe.py:77-108
matches on the text), never the numeric code — the wire format is unchanged
if the names are stable.

Corrections to earlier drafts: MAX31856.c does not branch wedge-vs-timeout
in control flow, only in its logged string; kiln_io_owner and thermo_owner
collapse every error to IO_FAIL. SaftyFW owners return bool and that is
adequate for their callers — the pico backend maps bool to OK/IO.

### Opaque handles — option (a), fixed-size aligned storage

SX1509.h:158, FT6336U.h:102 and NS2009.h:72 embed `i2c_owner_t` **by value**;
MAX31856.h:261 embeds `spi_owner_t`. An opaque pointer would force heap
allocation per device, so handles are:

```c
#define HAL_SPI_BUS_STORAGE_BYTES 64
typedef struct { alignas(8) uint8_t storage[HAL_SPI_BUS_STORAGE_BYTES]; } hal_spi_bus_t;
```

Each backend `.c` carries
`_Static_assert(sizeof(struct impl) <= sizeof(((hal_spi_bus_t*)0)->storage), "...")`
so growth fails the build, never memory. Measured today (32-bit, handles 4 B):
`spi_owner_t` ≈ 48 B, `i2c_owner_t` ≈ 20 B, `uart_owner_t` ≈ 32 B; no owner
embeds a `StaticSemaphore_t` (80 B on ESP-IDF) today — all use heap-created
handles. Reservations: spi bus 64 / device 32; i2c bus 128 (headroom for a
future static per-call semaphore) / device 16; uart 64. Pico backends have no
per-instance struct at all (file-scope statics) and simply under-fill.

Verified: no struct that contains an owner (SX1509Class, FT6336UClass,
NS2009Class, MAX31856BusClass) is memcpy'd, memcmp'd, written to NVS or a
backup, or sent over the bridge — every memcpy in those drivers is on
register byte buffers. The storage size is therefore a build-time ABI only.

Host toolchain, verified by compiling the drafts: `alignas` from
<stdalign.h> compiles under `/std:c11` and `/std:c17` and fails with no
`/std:` flag (`error C2061: syntax error: identifier 'alignas'`). The
`_Alignas(8)` keyword form also compiles under `/std:c11`. Use the keyword
form in the headers. KilnFW's build_host_tests.ps1 passes `/std:c11` on most
but not all of its 23 `cl` invocations; SaftyFW's main build passes no
`/std:` (only the fuzz build uses `/std:c17`). Phase 0 sets `/std:c11` on
every invocation that includes an interface header.

### hal_spi

Derived from esp_spi_owner (queued owner task, slot pool, wedge latch) and
SaftyFW spi_owner (mutex + direct blocking call — deliberately no task).
Interface must NOT assume an owner task exists; both models implement it.

```c
typedef struct {
    uint32_t clock_hz;         // 4 MHz MAX31856; display higher
    hal_spi_mode_t mode;       // 0 display, 1 MAX31856
    size_t queue_size;         // ESP device queue depth (1 today); pico ignores
    int hw_cs;                 // hardware CS line or HAL_CS_NONE (-1)
    uint32_t input_delay_ns;   // 50 for the thermo bus; pico ignores
    int cs_pin;                // bit-banged CS around each transfer, or HAL_CS_NONE
} hal_spi_device_cfg_t;

hal_status_t hal_spi_bus_init(bus, bus_id, const hal_spi_bus_cfg_t *cfg);   // see ALREADY_INIT note
hal_status_t hal_spi_bus_deinit(bus);
hal_status_t hal_spi_device_attach(bus, dev, &cfg);
hal_status_t hal_spi_transfer(dev, tx, tx_len, rx, rx_len, timeout_ms);
hal_status_t hal_spi_transfer_polling(...);   // ESP: polling_transmit; pico: == transfer
hal_status_t hal_spi_transfer_async(dev, tx, len, timeout_ms, cb, ctx);  // NOT_SUPPORTED unless enabled
bool         hal_spi_bus_is_wedged(bus);      // pico/host: always false
```

Facts pinning this shape: only caller of async is panel_spi_blit.c:432,
feature-gated off by default; polling used only by MAX31856 (MAX31856.c:168,
193); CS is bit-banged on both sides. The ESP contract "never store request
state on the caller's stack" (slot pool) carries into any async backend.
Vendor types hidden: spi_host_device_t, Queue/Task/SemaphoreHandle_t,
spi_device_handle_t (esp_spi_owner.h:20-23,103); the pico header is already
vendor-clean.

Bus-init semantics that must survive (main_boot_early.c:421-474):
`spi_bus_initialize` returns ESP_ERR_INVALID_STATE after a JTAG reset because
the bus is still up — benign today. `hal_spi_bus_init` needs a defined,
non-fatal ALREADY_INIT return (map to OK with a log, or BUSY — decide in
Phase 0 and document). Only the first init's DMA configuration takes effect,
so the display's DMA settings must be the ones applied — the display and
thermo devices share one bus.

Hot-path constraints (display): ~214 `spi_owner_transfer` calls per full
frame in 1440 B chunks, DMA scratch in internal RAM; the flush callback must
never call an lv_* mutator (51e1ef5). One FIFO owner queue is shared by
display and thermo with no priority — the safety-link thermo read
(safety_link_frames.c:392) transits it inside the ~345 ms reply budget, and
SPI_OWNER_TRANSFER_TIMEOUT_MS is 1000. All ~214 calls funnel through
panel_spi.c:345 with the owner passed by pointer; esp_spi_owner.c:418 builds
exactly one ~56 B `spi_owner_request_t` per call for the queue. The wrapper
keeps the handle pointer-passed and must not build a HAL-level request
struct that is then translated into the owner's — one request build per
call, not two. Measure frame time on hardware after Phase 1b, not just host.

### hal_i2c

ESP-only today (SaftyFW has no I2C). All three consumers (SX1509.c:113,
FT6336U.c:52, NS2009.c:51) use one identical shape:

```c
hal_i2c_bus_init/deinit;  hal_i2c_device_attach(bus, dev, addr, clock_hz)
hal_i2c_transfer(dev, tx, tx_len, rx, rx_len, timeout_ms)
```

Preserve i2c_owner.c's two hard-won behaviors in the esp backend: static (not
heap) per-call semaphore (2026-08-20 SRAM-starvation fix), and worker-enforced
timeout with unbounded caller wait (use-after-free avoidance,
i2c_owner.c:252-260). Also preserve the SX1509 asymmetry: the caller waits
200 ms while the device-side retry timeout is 6 s — collapsing them changes
the observed probe behavior. A pico backend is future work, modeled on
spi_owner's mutex pattern.

### hal_uart — two primitives, ESP backend unchanged

Round 1 proposed adopting the pico non-blocking shape only. Round 4 found the
blocker: uart_protocol.c:107 (`frame_and_send`) relies on
`uart_owner_transfer` blocking until the bytes are **on the wire**
(uart_write_bytes + uart_wait_tx_done, uart_owner.c:124,127) because the ACK
timer at uart_protocol.c:752-755 starts after it returns. A fire-and-forget
send would start the timer before the last byte left the FIFO. Resolution:

```c
hal_uart_init(u, cfg)                  // tx_io, rx_io, baud
hal_uart_send(u, data, len)            // non-blocking, whole-buffer-or-BUSY
hal_uart_send_blocking(u, data, len, timeout_ms)  // returns when the last byte has left
hal_uart_recv(u, out, max)             // non-blocking, returns 0..max
hal_uart_get_rx_error_count / get_tx_dropped / hal_uart_restart
```

Backends: ESP `send_blocking` = today's write + wait_tx_done, unchanged; ESP
`send` = uart_write_bytes without the wait. Pico `send_blocking` = `send`
plus a drain of the software ring (`get_tx_used()==0`, uart_owner.c:464-471,
race-free under `save_and_disable_interrupts`), documented as
ring-drained-not-wire-complete: the Pico never originates an ACK-timed send
(link_task.c:2124-2125 — it answers BROADCAST with BROADCAST, fire-and-
forget), so no pico consumer needs true wire completion. If one ever does,
the primitive is the PL011 `UARTFR.BUSY` bit, not TXFE — TXFE clears while
the last byte is still shifting out. No such check exists in SaftyFW today.
Host fake: instant.

Reply budget, recomputed: FRAME_STUFFED_MAX = (8+253+2)×2+2 = 528 B;
baud 230400 (Kconfig:749-751 default, no sdkconfig.defaults override,
check_safety_baud_sync.ps1:25-31 reads the Kconfig default);
528×10×1000×2/230400 + 300 = 345 ms exactly. The ESP's blocking send
completes before the reply clock starts (uart_protocol.c:751-755
`xSemaphoreTake` after `frame_and_send`; same shape in
safety_link_commands.c:513-538), so the ~23 ms send is outside the window.
Inside it: Pico poll ≤10 ms (LINK_TASK_POLL_MS, link_task.c:163), no SPI on
the Pico reply path, reply flight ≈23 ms — worst case ≈40 ms against 345.
Consequently only uart_protocol.c:107 changes its call and nothing is
re-budgeted. The only `uart_owner_transfer` callers pass rx=NULL
(uart_protocol.c:107 and the UnitTestFw mirror), so the reply-read path is
dead and is dropped.

Reset pairing, verified: `uart_owner_restart` has one caller
(uart_bridge_system.c:66) and is deliberately RX-only (UART_PROTOCOL.md:577-
587); the dedup ring records only CRC-valid frames (uart_protocol.c:321) and
framing resyncs on the next 0x7E, so no protocol state derives from what the
flush discards. The Pico has no runtime RX reset at all (init-time zero only,
uart_owner.c:255-256). `hal_uart_restart` inherits the same contract and the
same doc comment.

uart_protocol.c (framing, CRC16, ACK/retry/dedup, broadcast; 2867 lines with
its header) stays ABOVE the interface. Its RX loop's
read-what's-buffered-with-zero-timeout contract (uart_protocol.c:328-435) is
exactly `hal_uart_recv`. The ESP backend keeps its event task
(FIFO_OVF/BUFFER_FULL flush, frame-error counters); the pico backend keeps its
IRQ SPSC rings. Neither leaks through the interface.
uart_owner_tx_policy.c/h stays with the pico backend as a PRIVATE include.

### hal_gpio — clean-room; no IRQ surface in v1

```c
hal_gpio_init_out(num, idle_level)   // latch set BEFORE direction switch
hal_gpio_init_in(num, pull)
hal_gpio_set(num, level);  bool hal_gpio_get(num)
hal_gpio_set_direction(num, dir); hal_gpio_set_pull(num, pull)   // gpio_probe only
```

Clients: CS/DC/reset/fault pins in panel_spi, MAX31856 (both), SX1509,
safety_link, boot_button, monitor_task LED; pico relay_owner (GPIO6 —
relay_owner stays the state-machine owner above hal_gpio), discrete_task,
DRDY. Roughly 30 raw GPIO call sites in 9 files on each side.

IRQ decision, verified: exactly two IRQ registrations exist. Pico
thermo_task.c:314-317 (DRDY, `gpio_set_irq_enabled_with_callback` — a shared
per-core dispatcher, so a second registrant would clobber the first) and ESP
uart_bridge_io.c:633-643 (SX1509 ~INT, `gpio_isr_handler_add`, per-pin;
SX1509.c:459-478 configures the pin as a polled input and uart_bridge_io owns
the ISR). Both stay raw, with Phase 4 allowlist entries. Trigger for adding
`hal_gpio_irq_attach`: a second pico consumer.

Init ordering audit. Pico src/main.c:151-153 does put-then-set_dir
(correct). bootloader/main.c:189-191 does set_dir-then-put — REVERSED; out
of scope here, flagged for the bootloader pass. ESP: read against IDF
v6.0.2 (`C:\esp\v6.0.2\esp-idf`), `gpio_set_level` writes `out_w1ts`
unconditionally (esp_driver_gpio/src/gpio.c:248-253 →
gpio_ll_set_level, gpio_ll.h:310-318) and `gpio_set_direction` never
touches the OUT register, so level-then-config is glitch-free on its own —
no "direction IN first" step needed. The ESP `hal_gpio_init_out` is simply
set_level then config. Three ESP sites do it the wrong way round today and
have a brief CS-asserted window at boot: panel_spi_bringup.c CS (:219 then
:224), panel reset (:263 then :268), MAX31856.c CS (:517 then :523).
SX1509.c reset (:437 then :445) is already correct; safety_link.c:409/414
drives 0 onto a register that resets to 0, benign. These three sites are
fixed by adopting `hal_gpio_init_out`, which is a behavior change to note in
the Phase 3 item 6 commit, not a silent one.

gpio_probe.c needs a runtime pin number plus set_direction/set_pull/set/get;
its pin denylist stays above the HAL.

### hal_adc — pico-only

Wraps current_task.c:98-104 and current_sense.c:157-165:

```c
hal_adc_init(); hal_adc_gpio_enable(pin); hal_adc_select(channel); uint16_t hal_adc_read_raw()
```

Oversampling and discard-first-sample stay above. The host fake_adc is the
ONLY off-target route to S3/S4/S9/S11/S14 — no CTs are fitted on the bench
board, so those guards cannot be exercised on hardware. main.c:327-336's
adc_owner is still a TODO; hal_adc is where it lands.

### hal_kv — ESP-only, wraps NVS. Pico explicitly excluded.

NVS census, re-done function by function: `nvs_open_from_partition` ~25
files, plain `nvs_open` (default partition) in relay_cycles.c:96 and
run_state.c:131, `nvs_get/set_blob` ~20 files (size-probe form at
kiln_cfg_store.c:207), `nvs_get/set_str` in time_sync.c and wifi_prov_nvs.c,
`nvs_erase_key` in crash_report.c:335 and profiles_http.c:596,
`nvs_flash_init_partition`/`erase_partition` ~13 files + factory_reset.c:125,
`nvs_get_stats` in ui_page_diagnostics.c:570. Zero production callers of
the scalar `nvs_get/set_u8/u32/i16/i32`, iterators, `nvs_erase_all`,
`nvs_find_key` or `nvs_flash_deinit`. Namespaces: kiln_cfg (13 files),
boot_guard, touch_cal, watchdog_cfg, wifi_cfg. Partitions: kiln_nvs,
wifi_nvs, profiles_nvs — note touch_cal_store.c:17 has its own local
`#define NVS_PARTITION "kiln_nvs"`, easy to miss. flash_worker_lint.py's
allowlist has 22 entries. API covering 100% of observed use:

```c
hal_kv_open(namespace, mode, partition_or_NULL) / close / commit   // NULL = default partition
hal_kv_get/set_str, _blob              // get_blob(NULL buf) = size probe
hal_kv_erase_key(h, key)               // crash_report, profiles_http
hal_kv_init_partition(name)            // idempotent, with erase-retry
hal_kv_erase_partition(name)           // factory_reset's scoped erase
hal_kv_stats(partition, ...)           // nvs_report's enumeration
```

The round-1 draft listed `_u8/_u32` and omitted `erase_key`; both corrected.

Write-context safety is part of the contract: expose
`hal_kv_write_safe_here()` (today's caller_stack_is_external predicate) so
the six copy-pasted guards and flash_worker_lint.py's sanctioned patterns
(worker dispatch / local guard / init-time-only, plus wifi_prov's
dedicated-writer-task variant) survive verbatim. The lint is rekeyed on
hal_kv_set_* in the same commit. PSRAM-stacked tasks writing NVS panic every
time (documented); the worker-dispatch route is the fix, not a HAL concern.
Versioned-blob migration chains (zones v19, kiln_cfg v2, profiles per-slot,
wifi legacy list) are call-site logic on top of get_blob.

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
hal_flash_safe_execute(cb, arg, timeout_ms)  // pico: flash_safe_execute; host: direct call
```

safe_execute is first-class — the multicore-XIP hazard is real on RP2040.
Distinct failure reasons (TIMEOUT / NOT_PERMITTED / INSUFFICIENT_RESOURCES)
map onto hal_status_t. This brings config_store_flash.c under host test for
the first time. flash_layout.h constants are shared with the bootloader from
day one.

### hal_scratch — pico watchdog-scratch registry

All 8 RP2040 scratch slots are claimed (startup_diag.h:13-28): [0]/[1] trip
reason+magic, [2]/[3] startup diag+magic, [4] RESERVED by pico-sdk
watchdog_enable (fenced by comment only), [5] shared by watchdog_overdue_diag
(tag 0xD9) and the stack-overflow hook (tag 0xE3, mutually exclusive by
construction), [6] boot-stage marker (overwrite-only), [7] clear_trip packed
checkpoint. Registry: static claim table {slot, owner, tag} with a
compile-time uniqueness assert; slot 4 hard-reserved in code; typed accessors
write_u32/read_u32(&magic_ok)/clear supporting both access modes. The four
direct pokers (main.c, boot_reason.c, watchdog_overdue_diag.c,
clear_trip_diag.c) become clients. Slot map re-verified against every
`watchdog_hw->scratch` access (boot_reason.c:28-50, main.c:127/439/440,
watchdog_overdue_diag.c:23-48, clear_trip_diag.c:17-35, startup_diag.h:111
macro); slot 4's pico-sdk claim confirmed at hardware_watchdog/watchdog.c:82.
Slot 5's two writers are cross-documented in code (main.c:84-89). Hazard the
registry must also own: pico-sdk `watchdog_reboot(pc, sp, delay)` writes
scratch[4..7] whenever `pc != 0` (watchdog.c:90-112). SaftyFW's single call
(update_task.c:998) passes `pc = 0`, so nothing is stomped today; hal_wdt's
reboot wrapper takes no pc/sp arguments so the non-zero path cannot be
reached.

### hal_time / hal_wdt / hal_pwm / hal_sysinfo

- hal_time: monotonic us/ms. `esp_timer_get_time` is called at 44 sites in
  19 files (not ~15 modules as first counted). Host fake = controllable clock
  with advance-by-N and injectable jumps.
- hal_sysinfo: reset reason, running partition, build descriptor, chip
  temperature, `esp_random`, core-dump presence. Dedupes crash_report.c
  (which reads esp_core_dump), ui_page_diagnostics.c:522, dashboard,
  partition_info and main_network_http's independent
  esp_ota_get_running_partition calls. esp_partition stays read-only here;
  OTA writes are out of scope.
- hal_wdt: esp task-WDT reconfigure (watchdog_cfg.c sole user), `esp_restart`,
  and pico watchdog_enable/update/reboot. Thin.
- hal_pwm: backlight LEDC only. Thin, last.

### Board descriptors — forward, never freeze

settings.h forwards `CONFIG_KILNCTL_*` and ~30 files read those macros
directly, bypassing settings.h. `board_kiln_s3.h` must **forward** the
Kconfig values, not copy them: sdkconfig is gitignored and wins over
sdkconfig.defaults (Kconfig:962-965), so a frozen copy would silently
diverge from what the bench board builds with. Kconfig bools that gate
compiled code stay Kconfig, not board-descriptor: BACKLIGHT_PWM_ENABLE,
SPI_HARDWARE_CS, SPI_ASYNC_FLUSH, SPI_DMA_USE_PSRAM, SIM_PLANT,
ENABLE_GPIO_PROBE, TOUCH_FT6336U. The host stub sdkconfig.h is deliberately
incomplete and forces ENABLE_GPIO_PROBE=1 for tests — keep that.

## Boot order — what the backends must preserve

KilnFW main_boot_early.c: i2c bus :319-333 → SX1509 + kiln_io :341-355 →
boot_guard / rtc_watchdog :373-375 → spi_bus_initialize :421-474 →
MAX31856_start_all :521-534 → thermo_owner_start :558 → ILI9488_start
:582-610 (borrows the thermo bus owner; must precede lvgl_port_start) → touch
:625-664. main_control_bringup.c gates profile_executor and autotune on
`!boot_guard_is_recovery_mode()` (:160, :187) — any new task started early
must gate the same way.

SaftyFW main.c: GPIO6 driven low first :151-153 → console_uart :161 →
dbgpause :221 → watchdog :230 → scratch reads → config_store_boot_load :324
→ spi_owner_init / uart_owner_init :347-350 → tasks :421-436 (self-pin
affinity, enforced by task_priorities.h's `#error`) → scheduler :459.

Neither backend may reorder these; the HAL init calls are drop-in at the same
lines.

## RAM and flash

Owner task stacks total ≈43 kB internal DRAM: i2c ×3 (4096/3072/3072), spi
4096, uart_owner 4×3072, uart_proto_rx 2×8192. They must stay internal —
the lvgl stack-corruption precedent (51e1ef5) is exactly this region. KilnFW
owner tasks use `tskNO_AFFINITY`; whether a backend uses the `WithCaps`
(PSRAM-stack) create variant must be explicit per task, never defaulted.
Flash: bin 2,018,720 B in a 3,145,728 B slot, 1,127,008 B headroom — not a
constraint. No `.lf` linker fragments exist to move.

## Host fakes (Phase 2 specs)

Only test_esp_spi_owner.c asserts on hardware stub state today (g_stub_spi_*
counters, gpio set_level count, FreeRTOS queue-ring stub driving
spi_owner_task synchronously). Fakes preserve those observable behaviors,
then exceed the stubs:

- fake_spi (must): ordered transfer record (buf/len/flags/cs/polling-vs-
  queued); injectable enqueue-timeout, completion-timeout, pool exhaustion
  with distinct side effects (wedge latch vs not, refcount invariants);
  synchronous run-owner-loop driver. Should: async callback with
  fire-after-LAST-chunk semantics (an existing test guards that mutant).
- fake_i2c (must): ordered record/replay, NACK/timeout injection.
- fake_uart (must): real ring fill/drain model — backpressure and drop-counter
  behavior testable; `send_blocking` completes instantly; synchronous drive of
  the owner path. This creates the "PRODUCTION SEAM MISSING" substitution
  point test_safety_guards.c:1100-1139 documents for uart_owner_send.
- fake_gpio (must): per-pin level AND direction history (order-checkable CS
  and latch-before-direction sequencing), link-shared state instead of
  today's static-per-TU counters.
- fake_adc (must): scripted sample sequences with realistic quantization; the
  only route to the current-sense guards.
- fake_kv (must): RAM namespace/key store, reset between tests; error
  injection (wrong-type, corruption, no-space).
- fake_time (must): explicit advance; quantized/dithered inputs — the
  idealized-input bug class must not be reintroduced.
- fake_flash (must): sector model with erase-before-program enforcement and
  safe_execute timeout injection.
- Cross-cutting: a current-caller identity hook (parity with
  bx_caller_is_worker_task) so re-entrancy and lock-order bugs the
  single-threaded host cannot exercise are at least assertable. The fakes
  model no lock; the two documented deadlock classes (flash-worker
  re-entrancy, executor/autotune lock order) remain hardware-only findings.

Host build shape — Option A, decided: extend both existing
build_host_tests.ps1 hand-lists with `hwAbstraction/host/*.c` and drop the
retired stub `/I` entries. CommonFW's CMake/ctest project (CMakeLists.txt:88-
409) is never invoked by anything in `tools/`; `build_saftyfw_host_tests`,
run_all_checks.ps1 and verify.ps1 all shell the `.ps1` scripts, and
buildlock.py exists because concurrent `.ps1` runs corrupt shared `.obj`
paths. Moving to ctest would break every path-keyed check at once. Command-
line length is the real constraint: KilnFW's 81-entry main source list is
already ≈7,054 characters of cmd.exe's 8,191 limit at the normal checkout
path (≈10,780 at a worktree path — the documented overflow). Adding 5-10 fake
files crosses the limit at the normal path. Verified empirically: a 90-file
inline line (18,209 chars) through the same `cmd /c vcvarsall && cl` wrapper
is silently truncated mid-argument (`LNK1104: cannot open file '>.obj'`),
while `cl @sources.rsp` with one quoted path per line — including paths with
spaces — builds clean. Phase 2 therefore switches the main invocation to a
response file first.

What is host-compiled today, verified against both scripts: esp_spi_owner.c
is compiled into the main KilnFW executable via test_esp_spi_owner.c:41's
direct `#include` of the `.c` (build_host_tests.ps1:79), and
owner_slot_pool.c is in two executables (:108, :394). i2c_owner.c,
uart_owner.c and uart_protocol.c are host-compiled nowhere. SaftyFW's
build_host_tests.ps1 compiles uart_owner_tx_policy.c (:83) but neither
spi_owner.c nor uart_owner.c. The script's `$totalExpected` comment says 22
executables; 23 exist (exe23 added 2026-09-05) — fix the count in Phase 2.

Stub headers retired by Phase 2: driver/spi_master.h, driver/i2c_master.h,
driver/uart.h, driver/gpio.h, nvs.h, nvs_flash.h, esp_timer.h,
esp_spi_owner.h, i2c_owner.h, uart_owner.h. Kept (no HAL covers them):
freertos/*, esp_log/err/attr/event/netif/wifi/http_server/heap_caps/
ota_ops/partition/app_desc/core_dump/crc/random/task_wdt/system, sdkconfig.h,
build_info.h, bx_worker_stub.h, uart_protocol.h, psa/, lwip/. SaftyFW has
no stub directory — it links the real `.c` files; its spi_owner.c is not in
the host list today at all (zero off-target coverage until fake_spi).

Progress 2026-09-05: fake_gpio/fake_adc/fake_uart implemented and standalone
MSVC-tested (133 assertions + one proven negative test) at
firmware/hwAbstraction/host/ and firmware/hwAbstraction/test/test_host_fakes.ps1;
not yet wired into build_host_tests.ps1 (the response-file switch above is
still open), and fake_spi/fake_i2c/fake_kv/fake_time/fake_flash are unstarted.

## Migration inventories (measured)

Blast radius: KilnFW ≈75-80 owner call sites in 22 files, of which
uart_protocol_* accounts for >50 sites in 16 files; SaftyFW ≈25 sites in 5
files; bootloader/main.c is a separate binary and untouched. Semantic
consumers (branch on specific status or timing, not just OK/fail): MAX31856.c,
panel_spi_blit.c, safety_link.c, gpio_probe.c, uart_bridge_io.c (wait=0),
uart_bridge_thermo.c (computed wait). Everything else is a mechanical rename.

- espInterfaces move set: esp_spi_owner.c/h, i2c_owner.c/h, uart_owner.c/h,
  uart_protocol.c/h. **owner_slot_pool.c/h lives at App/drivers/, not
  espInterfaces** — it must move with esp_spi_owner or the component gains a
  dependency cycle on drivers.
- SaftyFW move set: src/spi_owner.c/h, src/tasks/uart_owner.c/h (845 lines
  with header), uart_owner_tx_policy.c/h.
- Header collision: `uart_owner.h` exists on both sides. Rename to
  hal_uart_esp_internal.h / hal_uart_pico_internal.h in Phase 1a, before
  either lands in a shared include path.
- safety_link.c rebase is small: three raw GPIO calls (gpio_config:409,
  gpio_set_level:315,414, gpio_set_pull_mode:493) → hal_gpio; its UART use is
  already entirely through uart_owner/uart_protocol.
- console_uart.c (pico, 51 lines, write-only, no IRQ): leave it, with a Phase
  4 allowlist entry. It is dependency-free by design (boot banner before
  anything else). Note stdio does NOT route through it
  (pico_enable_stdio_uart 0).
- NVS migration set: 19 files / 22 lint-allowlist entries. Mechanical;
  flash_worker_lint.py migrates in the same commit.
- PcTools and `.claude/`: zero runtime path dependencies on the moved files —
  every reference is a docstring citation. flash_provenance.py's
  SENSITIVE_PATTERNS are symbol-keyed and survive.

## Phase 1 — exact edit list

Include fixups: MAX31856.h:47, panel_spi.h:52, FT6336U.h:42, NS2009.h:37,
SX1509.h:54, safety_link.h:167-168, safety_link_internal.h:30,
main_internal.h:47-48, main_network_http.c:51-52,
test_uart_protocol_link_delegate.c:38, test_esp_spi_owner.c:41 (this one
`#include`s the `.c`), SaftyFW main.c:37, max31856.c:17, link_task.c:63.

CMake: drivers/CMakeLists.txt:367-372 and :375-379 (drop the moved sources);
KilnFW CMakeLists.txt:10 (EXTRA_COMPONENT_DIRS); SaftyFW CMakeLists.txt:62,
:120 plus `target_link_libraries` on all three executables.

Scripts that MUST change: check_c_files_in_cmakelists (add the new roots to
its scan rule); check_duplicate_symbols.ps1:94 (add the new component obj
dir); check_no_duplicate_crc.ps1:25 (comment only — the :123 entry is
UnitTestFw and stays); check_safety_baud_sync.ps1:33 (uart_owner.c path);
check_stack_margin_registration.ps1:55-57 and :133 (esp_spi_owner.c:297's
registration leaves the scanned tree — the check goes vacuous, not red,
without this); wire_protocol_fingerprint_check.py:153;
stub_signature_drift_check.py:68-70 (verify, likely rekey).

Scripts that must NOT change (verified not path-keyed on the move set):
check_relay_writes_through_owner, check_safety_call_results_checked,
check_host_embed_symbols_defined, the SaftyFW check_isolation family
(check_link_impl_isolation lives at firmware/SaftyFW/tools/), flash_worker_lint
(Phase 3), both build_host_tests.ps1 (Phase 2), stub headers (Phase 2).
Round 1 listed four of these as path-keyed; that was wrong.

Doc citations to update: ARCHITECTURE.md:61-64, DISPLAY_ST7796_PLAN.md:152,
DRAM_PSRAM_PLAN.md:162,856, UART_PROTOCOL.md:69, KilnFW README.md:99,
drivers/README.md:20-21, CommonFW README.md:32,256,263,524, LINK_PROTOCOL.md
(8 lines), BENCHPROTO.md:162, SaftyFW TODO.md:226, PcTools README.md:4.
Dated snapshots and commit-message quotes stay as written.
check_doc_citations skips unresolved paths silently — watch its
`$skippedNoFile` count before and after.

Git-history checklist (from the two 2026-08-28 moves): kill any process
holding the directories first (a running MCP server blocked the last one);
move children incrementally — a directory rename can fail with Permission
denied where per-file moves succeed; verify with `git ls-files -s` that every
entry is a rename, not delete+add; rebuild any venv in place; run
check_source_path_drift.ps1 (b3ac6e6); widen singular globs; make every
empty-glob match fail loud (01c7ef7); walk `.claude/settings.json` entries one
at a time; delete stale `.obj` files in the host-test build dirs.

## Phases

Each phase compiles, passes run_all_checks.ps1 (with `-ExecutionPolicy
Bypass`), host tests, AND `build_kilnfw` + the SaftyFW target build before
commit (host tests are not a target build). Negative-test every new check and
every fake (prove it can fail). Prefix-rename every symbol widened from
static even when grep is clean. Land Phase 1a and the hal_kv migration alone,
when no other session is mid-edit in drivers/.

**Phase 0 — scaffold + contract. DONE, commit `c626727`.** Fixed per opus
review on that commit: hal_uart_cfg_t port field + hal_uart_deinit,
hal_i2c_probe, hal_spi_bus_init's cfg struct (owner-task params folded in)
and hal_spi_async_cb_t's ctx-first argument order, alignas/`_Alignas`
consistency across all three headers, and compile_headers.ps1's vswhere-based
vcvars discovery. Tree,
hal_status.h (full table above, plus hal_status_to_name), interface headers
for spi/i2c/uart/gpio/adc with the opaque-storage pattern and
threading/ownership contracts doc-commented per header (single-writer,
buffer-copy-in, drop policy, wedge semantics, latch-before-direction).
`/std:c11` demonstrated via firmware/hwAbstraction/test/compile_headers.ps1
(passes, and negative-tested by breaking a header and observing the harness
fail, then restoring it). No callers change.

ALREADY_INIT decision: `hal_spi_bus_init`/`hal_i2c_bus_init` on a bus that
is already up return `HAL_OK` with an INFO log, not `HAL_BUSY` — this is a
benign, expected re-entry (JTAG-reset case) rather than caller error or a
retry signal; documented in both headers' doc comments.

check_c_files_in_cmakelists.ps1 was NOT extended: its scan roots are
`firmware/KilnFW/App/*.c` and `firmware/SaftyFW/src/**/*.c` only, so
`firmware/hwAbstraction/**/*.c` is out of its charter as written and the new
files aren't referenced by it either way. Revisit this in Phase 1a, when
`esp/`/`pico/` sources actually need to land in a real CMakeLists and this
check's scan rule (or an equivalent) should start covering
`firmware/hwAbstraction/`.

**Phase 1a — move only.** The move set above, byte-identical bodies,
header renames, include fixups, CMake, the MUST-change scripts, the doc
citations, the git-history checklist. Nothing else in the diff. Verify every
check script actually ran (not vacuous) and the ESP bin's `fw_build` changed
on the board after `flash_firmware()`.

**Phase 1b — adapt.** Each moved owner implements its Phase-0 interface:
types change at the edge, bodies stay. hal_uart_send_blocking lands here and
uart_protocol.c:107 switches to it. Measure on hardware after this phase:
safety-link reply timing, display frame time, thermo read latency under a
full-screen redraw. relay_owner becomes a hal_gpio client; hal_adc wraps
current_task/current_sense.

ESP-side gpio/adc bodies landed early (ahead of the Phase 1a move, not wired
into CMakeLists yet, `firmware/hwAbstraction/esp/{gpio,adc,common}/`,
syntax-checked by `firmware/hwAbstraction/test/compile_esp_backends.ps1`):
hal_gpio_esp.c covers the real ESP consumers listed above under "hal_gpio".
hal_adc_esp.c is speculative only -- relay_owner/current_task/current_sense
above are all SaftyFW (RP2040) files; no ESP consumer exists for hal_adc
(matching this section's header and title), and ESP-IDF's adc_oneshot/
adc_cali model (unit handle, per-channel atten/bitwidth config, a separate
calibration handle) does not fit hal_adc.h's pico-shaped, handle-less,
raw-sample-only signature without widening it. See hal_adc_esp.c's
INTERFACE MISMATCH comment for the four specific gaps.

**Phase 2 — host backend.** Response file for the main cl invocation first.
Implement hwAbstraction/host/ per the fake specs; switch both
build_host_tests.ps1 to link fakes for migrated interfaces; retire the ten
stub headers; port test_esp_spi_owner.c off g_stub_* onto fake_spi's records.
New tests unlocked immediately: pico spi_owner against fake_spi, pico
uart_owner ring logic against fake_uart, current-sense guards against
fake_adc, config_store_flash against fake_flash.

**Phase 3 — absorb the stragglers, one interface per commit:**
1. hal_scratch (pico) — highest safety value, smallest diff; the slot-4 code
   reservation closes a fenced-by-comment-only hazard.
2. hal_flash (pico) + config_store_flash rebase + its first host tests.
3. hal_kv (esp) + 19-file mechanical migration + flash_worker_lint rekey.
   Land alone.
4. hal_sysinfo — dedupe partition/reset-reason/core-dump call sites.
5. hal_time — 44 sites, mass mechanical.
6. safety_link.c's three GPIO calls; boot_button, monitor_task, gpio_probe
   (runtime-pin API), panel_spi_bringup (two-step init_out).
7. hal_wdt, hal_pwm, board_temps. Low value; skip if effort budget runs out.

**Phase 4 — enforcement: `tools/check_hal_include_boundary.ps1`.** Ratchet
stage DONE (2026-09-05): script + baseline + strict `esp_ota_ops.h`/
`esp_wifi.h`/`esp_netif.h` allowlists + negative test all land and
`run_all_checks.ps1` passes. Measured against today's tree: `driver/` 36,
`hardware/` 16, `nvs.h` 23, `nvs_flash.h` 24, `esp_timer.h` 23 (baseline
recorded at these values), `esp_ota_ops.h` 8 files (matches the estimate
below exactly), `esp_wifi.h`/`esp_netif.h` 5 files, all wifi_prov family.
Concurrent commits move files in this tree constantly, so a false ratchet
trip from an unrelated rename/split is expected at some point and the fix
is a reviewed `-UpdateBaseline` run, not a reflex loosening of the check.

Include census today (files, KilnFW/App excluding test/stubs/build):
`driver/` 36, `esp_timer.h` 23, `nvs.h` 23 / `nvs_flash.h` 24,
`esp_ota_ops.h` 8, `esp_partition.h` 9, `esp_system.h` 9, `esp_random.h` 10,
`esp_core_dump.h` 2, `esp_task_wdt.h` 1, `esp_wifi.h` 3, `esp_netif` 3,
`esp_http_server.h` 13, `esp_heap_caps.h` 50, `soc/`+`hal/` 1
(rtc_watchdog.c), `freertos/` 80. SaftyFW/src: `hardware/` 16, `pico/` 5,
`FreeRTOS` 13. Enforced literally against today's tree the allowlist would
be 60-75 distinct files — decorative, not auditable. So the check is
written in two stages:

- **Ratchet now (any phase):** count-based baseline in the style of
  check_stack_margin_baseline — fail if the number of files including
  `driver/`, `hardware/`, `nvs.h`, `nvs_flash.h` or `esp_timer.h` outside
  `firmware/hwAbstraction/` rises above the recorded baseline. Enforce
  `esp_ota_ops.h` (8 files) and `esp_wifi.h`/`esp_netif` (wifi_prov family
  only) as strict per-file rules from day one — small, self-contained sets.
- **Strict after Phase 3:** any `#include` of the enforced set outside
  hwAbstraction/{esp,pico} is a violation unless the file is on a
  path-keyed allowlist of `@{ RelPath; Header; Reason; ExpiresAtPhase }`
  hashtables (full repo-relative paths, never a directory prefix — SaftyFW
  has no `espInterfaces/`-style subdirectory, so a prefix rule would be
  asymmetric). Expected final entries: main_boot_early.c (bus init),
  wifi_prov family, lvgl_port, console_uart.c, thermo_task.c and
  uart_bridge_io.c (the two raw IRQ owners), rtc_watchdog.c (`soc/`, `hal/`),
  bootloader/. `esp_heap_caps.h`, `esp_http_server.h` and `freertos/` are
  not in the enforced set at all (out of scope).

Mechanics, copied from the existing checks: strip comments with
check_isolation.ps1's `Get-CodeOnlyLines` (its `#include`-line exception is
load-bearing); enumerate with check_c_files_in_cmakelists.ps1's filter
extended to `.h`; hard floor `if ($allFiles.Count -lt 200) { throw }` so a
broken glob cannot pass. `esp_random.h` (10 files) is unclassified by this
plan — assign it to hal_sysinfo before the strict stage.

Negative test — a new precedent, none of the existing checks has one: copy a
clean non-allowlisted file (e.g. drivers/pid.c) into the scratchpad, inject
`#include "driver/gpio.h"`, call the scan function on that path list and
assert exactly one violation; then assert zero on the unmodified copy. Lives
at firmware/KilnFW/App/test/test_check_hal_include_boundary.ps1. Also update
flash_worker_lint's call-pattern keys. No existing check hardcodes the owner
filenames as scan scope (only check_safety_baud_sync:33 and the UnitTestFw
allowlist literals already listed under Phase 1).

## Deliberately out of scope

- Wi-Fi/httpd/LVGL/heap portability. wifi_prov is already a sole owner
  (wifi_prov.c:473); esp_heap_caps in 51 files is pervasive but a heap seam
  pays nothing until a real second target exists. Future hal_wifi/hal_heap.
- time_sync (SNTP), ota_pico_relay, OTA partition writes, mdns (unused).
- No vtables/function pointers. Link-time backend selection, one backend per
  build.
- firmware/UnitTestFw stays untouched (owner decision 2026-09-05). Its
  espInterfaces copy has diverged from KilnFW's by 786/576/225 lines on the
  exact files Phase 1a moves, it has no build/flash consumer in tools/ or
  run_all_checks, and ROADMAP.md:92,222 disclaims it — none of that changes
  the decision. No move, no fold into hwAbstraction/, and
  check_no_duplicate_crc's exclusion entries for it stay. Do not re-propose
  deletion.
- SaftyFW bootloader (bare-metal, ~64K budget, no flash_safe_execute, raw
  uart1 putc/getc): same treatment later, separate pass; shares only
  flash_layout.h. Its reversed GPIO init order is noted above.
- ESP i2c and spi bus init in main_boot_early.c stay put until the HAL owns
  bus bring-up (Phase 3+ decision).

## Risks

- Phase 1a is the "splits break filename-keyed checks" class at maximum size:
  full grep for old filenames AND renamed identifiers, then run every check
  script and confirm each one can still go red. Nine of twelve prior splits
  broke a check silently; check_stack_margin_registration is the one most
  likely to go vacuous here.
- Opaque storage sizes are a header ABI: growing a backend past its
  reservation is a build error by design, but shrinking the reservation
  later touches every consumer struct. Size generously in Phase 0.
- Timing on hardware after Phase 1b (safety-link reply, frame time) is a
  pass/fail gate, not a nice-to-have; host tests cannot see it.
- Fake fidelity trap: a fake that models no lock and no quantization
  re-creates the documented host-test blind-spot classes. The fake specs are
  requirements.
- Concurrent sessions share this tree; Phase 1a, the drivers/ reorg and the
  hal_kv migration are land-alone diffs. State on 2026-09-05: stash empty;
  worktree `s14-cal-gate` holds 7289cc8 (safety_core.c +7, unmerged) and must
  be rebased before any SaftyFW move that touches safety_core.c; the other
  worktrees (agent-a082f1859f1d7800a, C:/wt/merge, C:/wt/sfck) carry nothing
  relevant. Uncommitted KilnFW edits at the time — drivers/dram_margin.h,
  profiles_builtin.h, profiles_builtin_table.inc, main.c, test/stubs/nvs.h,
  test/test_profiles_builtin.c, sdkconfig.defaults — would collide with a
  drivers/ reorg; wait for them to land or coordinate. SaftyFW's dirty files
  (config_store.h, safety_guards.c/h, test_safety_guards.c) are outside the
  HAL move set.
- Reset-one-side class: verified paired for hal_uart_restart (single caller
  uart_bridge_system.c:66, RX-only by design per UART_PROTOCOL.md:577-587,
  and uart_protocol's dedup only records CRC-valid frames, so a ring flush
  cannot strand it). Re-run the four-instance checklist if the restart ever
  grows a TX side or the Pico gains a runtime RX reset (it has none today).
- hal_wdt reboot: `watchdog_reboot(pc, sp, delay)` with pc≠0 overwrites
  scratch[4..7], which hal_scratch's slot map owns (slot 5 has two writers,
  main.c:84-89). The only caller today passes pc=0 (update_task.c:998); the
  wrapper takes no pc/sp arguments so the hazard cannot be reintroduced.
- GPIO boot glitch: three ESP sites set direction before level today
  (panel_spi_bringup.c:219/224 CS, :263/268 reset; MAX31856.c:517/523 CS),
  briefly driving the line low. hal_gpio's level-then-config contract fixes
  them on migration; that is a deliberate behaviour change, so check the
  MAX31856 and panel bring-up on hardware after Phase 1b rather than assuming
  "same as before".
- drivers/ reorg: 185 literal SRCS in CMakeLists (no glob) and every
  filename-keyed check script move at once. Loud failure (build breaks) for
  CMake; silent failure for checks — same mitigation as Phase 1a.

## drivers/ layering (KilnFW only)

firmware/KilnFW/App/drivers/ holds 332 files and most of them are not
drivers. Census by role (2026-09-05):

| Role | Files | Examples |
|---|---|---|
| hw (real device drivers) | 24 | MAX31856, SX1509, FT6336U, NS2009, panel_spi*, backlight_pwm |
| owners (bus/IO arbitration) | 15 | espInterfaces/*, kiln_io_owner, owner_slot_pool |
| control | 36 | profile_executor*, pid*, autotune*, feedforward*, thermal_guard |
| safety | 26 | safety_link*, safety_cfg*, danger_mode, crash_report |
| persist | 22 | zones_config*, profiles_store*, touch_cal_store, boot_guard |
| net | 19 | wifi_prov*, time_sync, ota_* |
| http | 34 | *_http.c handlers |
| ui | 48 | ui_page_*, lvgl_port, theme |
| bridge | 17 | uart_bridge_*, gpio_probe |
| sim | 2 | |
| misc | 5 | event_log |

**Layout deviation from the original plan (coordinator decision, 2026-09-05,
third round):** target directories are subdirectories of the existing
`drivers` component -- `App/drivers/{hw,owners,control,safety,persist,net,
http,ui,bridge,sim,common}/` -- **not** sibling `App/<layer>/` components as
the "Target:" line originally read. `drivers` stays the single ESP-IDF
component; `CMakeLists.txt`, `Kconfig`, `README.md` and
`gen_build_info.cmake` all stay at `App/drivers/` unmoved. Reasons: (1) one
component means zero `REQUIRES`/component-boundary changes anywhere else in
the tree; (2) `Kconfig`'s options already span net/ui/safety with no single
natural new owner, and splitting it per layer would need a matching split of
every place that reads it (`check_safety_baud_sync.ps1` and others);
(3) `README.md` documents the whole former `drivers/` tree, not one layer;
(4) it collapses what would otherwise be ten new per-layer `CMakeLists.txt`
fragments (or an `App/CMakeLists.txt` restructuring) into one in-place
literal-path rewrite of the existing `drivers/CMakeLists.txt` SRCS list --
far less CMake churn for the same layering benefit. `INCLUDE_DIRS` lists the
eleven subdirs instead of ten sibling directories; bare `#include "x.h"`
still works with zero path-qualification, per the zero-basename-collision
check in `tools/drivers_reorg/DRYRUN.md` section 3.

A new bottom-most tier, `common` (below `hw`), holds pure leaf
headers/utilities with no dependency on anything above it: `uart_task_ids.h`,
`settings.h`, `stack_margin.{c,h}`, `stack_margin_calc.h`, `http_form.h`,
`web_encoding.{c,h}`, `httpd_socket_budget.h`, `dram_margin.h`,
`bx_worker_reentrancy.h`. Each was checked before placement and confirmed to
include only ESP-IDF/libc headers; this single mapping.csv change (with `sim`
promoted to the top tier alongside ui/http/bridge, since the sim backend
drives the system from above) resolved 51 of the 73 upward includes found in
the second-round dry run without touching any source file -- see
`tools/drivers_reorg/DRYRUN.md` section 5 for the 22 that remain and a
proposed one-line fix for each.

Target: `App/drivers/{hw,owners,control,safety,persist,net,http,ui,bridge,
sim,common}/` with `misc` folded into whichever layer owns each file (only
`event_log*` -- folded into persist). Preparation for this move (full
358-file mapping with resolved answers for every ambiguous placement, a
`git mv` + CMakeLists-rewrite script, and a dry-run report naming the 18
path-keyed check scripts and the 22 remaining upward includes) lives in
`tools/drivers_reorg/` -- see `tools/drivers_reorg/DRYRUN.md`. Nothing has
been applied yet. Allowed include direction is strictly downward:
ui/http/bridge/sim → control/safety/persist/net → owners/hw → common →
hwAbstraction. Six upward-include patterns existed before this dry-run pass
and were already untangled as ordinary commits (items 1-6 below); the 22
that remain after the second-round mapping revision are catalogued with
proposed fixes in `tools/drivers_reorg/DRYRUN.md` rather than repeated here:

1. DONE (2026-09-05): 18+ control/safety/persist files included zones_http.h
   for what are really config accessors. Split into zones_config_accessors.h
   (persist; holds every zones_config_*()/zones_current_sweep_*()/zones_ct_*()
   accessor and the ZONE_*_MAX bound constants) — zones_http.h now includes it
   and keeps only zones_http_start() and zones_http_set_hw(). 51 includers
   switched; zones_config_json.h's own zones_http.h include (for the same
   bound macros) switched too, breaking that circularity.
2. DONE: ota_http.h leaked into kiln_io_owner.c:17, profile_executor.c:25 and
   profile_executor_run.c:21. `ota_http_heat_blocked_by_update()`'s
   declaration moved to new `ota_state.h`; ota_http.h includes it, the three
   callers include ota_state.h instead. Implementation stayed in ota_http.c.
3. DONE: board_temps.c:8,12 included http and net headers from what is a hw
   file. The httpd handler/registration moved to new board_temps_http.c;
   board_temps.c gained board_temps_bind_thermo_bus() so it no longer reaches
   into esp_http_server.h/wifi_provision_http.h itself.
4. DONE: gpio_probe.c:32 and boot_button.c:11 included profile_executor.h
   from bridge/hw for the pure status query (`profile_exec_status_t` +
   `profile_executor_get_status()`). Both now include new, narrow
   `profile_executor_state.h` instead.
5. DONE: backlight_pwm.c:63-64 included screen_idle and display_power_cfg
   (ui/persist) from hw. Replaced the stored `const void *idle` with a
   caller-supplied `backlight_pwm_query_fn` (screen_on/idle_ms/brightness_pct
   out-params); main_boot_early.c's new
   backlight_pwm_query_screen_and_brightness() shim (which already includes
   both headers) is passed to backlight_pwm_init() instead of
   backlight_pwm.c reaching into screen_idle.h/display_power_cfg.h itself.
6. DONE: safety_link*/danger_mode included profile_executor.h; switched to
   the same narrow `profile_executor_state.h` (safety_link_frames.c uses the
   full `profile_exec_status_t` snapshot for SAFETY_CMD_PUSH_CONTEXT, still a
   pure query, no start/stop/command surface pulled in).
7. DONE (2026-09-05): log_store_mount.c included all of uart_bridge.h for
   one dispatch call. New narrow `flash_worker.h` (common tier) declares
   only `uart_bridge_ext_run_on_flash_worker()`; log_store_mount.c and
   uart_bridge.h itself now include it. Same pass reverted decision D (`sim`
   back to the bottom tier with owners/hw, resolving 4 upward includes for
   free) and added a verifier allowlist for kiln_io_owner/relay_authority's
   five deliberate interlock includes -- see `tools/drivers_reorg/DRYRUN.md`
   section 5's 2026-09-05 update for detail and the two new sim_backend.c
   findings it surfaced (proposed, not yet implemented).
8. DONE (2026-09-05): DRYRUN.md section 5 "Fix 1" -- five control/persist
   headers (`profiles_builtin.h`, `profile_executor.h`,
   `profile_executor_state.h`, `profile_feasibility.h`, `run_state.h`)
   included `profiles_http.h` for `profile_t`'s shape alone. New
   `profiles_types.h` (persist, alongside `profiles_builtin.h`) holds just
   the types/constants; `profiles_http.h` includes it back for its own use,
   and the five switched. `profile_executor_run.c` had been relying on
   `profiles_http_get()`'s declaration arriving transitively through
   `profile_executor.h`; it now includes `profiles_http.h` directly, same
   pattern as every other `.c` file (`backup_export.c`, `dashboard_http.c`,
   etc.) that already calls that accessor.
9. DONE (2026-09-05): DRYRUN.md section 5 "Fix 2" -- `autotune_engine_internal.h`,
   `factory_reset.c`, `kiln_cfg_store.c`, `ota_pico_relay.c` and
   `zones_current_sweep_task.c` included the whole httpd-handler
   `ota_http.h` for a handful of interlock/auth/progress query functions
   (none register httpd routes). `ota_http_context_t`, `ota_http_update_
   end()`, `ota_http_check_interlocks()`, `ota_http_heat_blocked_by_
   update()`, `ota_http_esp_phase_t` and `ota_http_get_esp_progress()`
   moved into the existing `ota_state.h` (already net-tier, already home to
   item 2's `ota_http_heat_blocked_by_update()`); `ota_http.h` includes it
   back so httpd-layer callers are unaffected. The three httpd_req_t-shaped
   accessors originally moved here too (`ota_http_authenticate_request()`,
   `ota_http_req_ack_no_safety()`, `ota_http_send_interlock_refusal()`) were
   moved back to `ota_http.h` on review (2026-09-05 follow-up): every
   caller of those three already holds a live `httpd_req_t` and includes
   `esp_http_server.h` regardless, so keeping them in `ota_state.h` bought
   nothing but forced `esp_http_server.h` on the callers (kiln_io_owner.c,
   profile_executor.c et al) that this split exists to keep httpd-free.
   `factory_reset.c` -- the one caller of those three among the five --
   switched its include to `ota_http.h`; the other four (which use none of
   the three) stayed on `ota_state.h`. `zones_current_sweep_task.c`'s
   include turned out to be dead (no `ota_http_*` call in the file at all)
   and was deleted rather than switched. Implementations stayed in
   `ota_http.c`.
10. DONE (2026-09-05): DRYRUN.md section 5 "Fix 3" -- `factory_reset.c` and
    `wifi_prov.c` included the whole `wifi_provision_http.h` for one
    accessor apiece (`wifi_provision_http_get_server()` and
    `wifi_provision_http_start()`). New narrow `wifi_provision_state.h`
    holds only `wifi_provision_http_start()` -- `wifi_provision_http_get_
    server()` stays in `wifi_provision_http.h` since it returns an
    `httpd_handle_t`, so every caller needs `esp_http_server.h` regardless
    and moving it would have dropped no one's dependency (2026-09-05
    follow-up correction). `wifi_prov.c` (which only calls `_start()`)
    switched to the narrow header; `factory_reset.c` and `sim_backend.c`
    (which call `_get_server()`) stayed on/switched to `wifi_provision_http.h`.
    `wifi_provision_http.h` includes the narrow header back so existing
    callers of the wider header are unaffected.
11. DONE (2026-09-05): DRYRUN.md section 5 proposal `zones_config_query.h` --
    `sim_backend.c` included the 1300+ line persist-tier
    `zones_config_accessors.h` for one accessor,
    `zones_config_get_thermo_count()`. New narrow `zones_config_query.h`
    holds just that declaration; `zones_config_accessors.h` includes it
    back, and `sim_backend.c` switched. Grepped every other
    `zones_config_accessors.h` includer for the same single-accessor
    pattern -- none qualified, all use additional accessors (relay_mask,
    control_mode, PID/model getters, etc.) so they keep the full header.
12. DONE (2026-09-05): `profile_executor_run.c` (control) included http-tier
    `profiles_http.h` for `profiles_http_get()`. New `profiles_store.h`
    (persist) declares `profiles_http_get()`/`_save()`/`_delete()`;
    `profiles_http.h` includes it back. Grepped every other `profiles_http.h`
    includer: `backup_export.c`/`backup_import.c`/`dashboard_http.c`/
    `dashboard_exec_http.c`/`readiness_http.c` are mapping.csv-forced http
    tier despite non-`_http` names for two of them, and `uart_bridge_ext*.c`/
    `ui_page_*` are bridge/ui (top) tier -- none needed switching;
    `profile_executor_run.c` was the only non-top-tier includer.
13. DONE (2026-09-05): `sim_backend.c` (mid/sim) included http-tier
    `wifi_provision_http.h` for one accessor, `wifi_provision_http_get_server()`.
    IDF's `esp_http_server.h` defines `httpd_handle_t` as a plain
    `typedef void *httpd_handle_t;`, so a second local typedef of the same
    name would conflict wherever both headers are included -- instead the
    real state accessor moved to `wifi_provision_state.h` as
    `wifi_provision_get_httpd_handle()`, returning untyped `void *`.
    `wifi_provision_http_get_server()` stays declared (plain `extern`, not
    `static inline`) in `wifi_provision_http.h`, now defined in
    `wifi_provision_http.c` as a one-line cast over
    `wifi_provision_get_httpd_handle()` -- kept a real, non-header-defined
    function because six host tests (`test_ota_http.c`,
    `test_partition_info_http.c`, `test_profiles_http.c`,
    `test_profile_executor_prestart.c`, `test_safety_cfg_http.c`,
    `test_wifi_prov.c`) each provide their own definition of this exact
    symbol instead of linking `wifi_provision_http.c`; a `static inline` body
    in the header collided with every one of them (found by the standard
    host-test rebuild, not by inspection). `sim_backend.c` switched to
    `wifi_provision_state.h` (plus a direct `esp_http_server.h` include, which
    it already needed for `httpd_uri_t`/`httpd_register_uri_handler()` and
    was previously getting transitively). `factory_reset.c` is http tier and
    needed no change -- kept `wifi_provision_http.h`.

All placements ambiguous under the original plan are now resolved in
`mapping.csv` (coordinator decisions, third round): the zones_http family
splits between `http` (thin registration + get/post/pid handlers) and
`persist` (`zones_http_internal.h`, since `zones_config_store.c`/
`zones_config_accessors.c` are consumers), zones_current_sweep_* → control,
danger_mode → safety, wifi_status_ui → ui, settings.h → **common** (not
persist -- decision B), event_log → persist. `heat_enable`/`heat_interlock`
→ control, `kiln_io`/`kiln_io_owner`/`relay_authority` → owners,
`relay_cycles` → persist, `thermo_combine` → control,
`tuning_recommendations_fallback.json` → http, `zone_settings_source_chain.h`
→ persist. See `tools/drivers_reorg/DRYRUN.md` section 0 for the full
per-decision rationale.

Sequencing: untangle the six include patterns as ordinary commits first
(each is a small behaviour-preserving edit that builds and passes on its
own) -- already done. Then the directory move as one planned tree, with
`drivers/CMakeLists.txt`'s ~201 literal SRCS entries rewritten **in place**
in the same commit (decision A keeps this one file; no new per-layer
CMakeLists.txt files) and every check_*.ps1 grepped for old paths; then HAL
Phase 1a moves espInterfaces/ out of owners/ into hwAbstraction/. Doing the
reorg before Phase 1a means the HAL move touches paths that are already
final. The Phase 4 include-direction check (ratchet now, strict after Phase
3) is what keeps the layering honest afterwards. SaftyFW's src/ is 48 files
(23 .c, 25 .h) and flat by choice; it is not part of this.
