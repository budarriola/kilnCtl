# firmware/hwAbstraction

One hardware-abstraction tree shared by KilnFW (ESP32-S3) and SaftyFW
(RP2040). Full history and phase-by-phase record: `docs/HW_ABSTRACTION.md`.

```
hwAbstraction/
  interface/   portable headers only (backend-independent): hal_status.h
               hal_spi.h hal_i2c.h hal_uart.h hal_gpio.h hal_adc.h hal_kv.h
               hal_flash.h hal_scratch.h hal_time.h hal_wdt.h hal_pwm.h
               hal_sysinfo.h. No vendor types. C11 (stdalign.h,
               _Static_assert). Opaque handles, link-time backend, no vtables.
  common/      vendor-neutral shared code (hal_status.c).
  esp/         ESP-IDF backends, plus the SPI/I2C/UART owners and
               uart_protocol relocated in from App/drivers/owners/:
                 esp/spi/  esp_spi_owner.{c,h}, owner_slot_pool.{c,h}, hal_spi_esp.c, hal_spi_esp_owner.h
                 esp/i2c/  i2c_owner.{c,h}, hal_i2c_esp.c, hal_i2c_esp_owner.h
                 esp/uart/ uart_owner.{c,h} (embeds a hal_uart_t), uart_protocol.{c,h}, hal_uart_esp.c
                 esp/gpio/, esp/kv/, esp/time/, esp/wdt/, esp/pwm/, esp/sysinfo/, esp/common/
               esp/CMakeLists.txt is comment-only, not a real ESP-IDF
               component (a bare "esp" component name collides in IDF's flat
               namespace) -- the real component is the thin wrapper at
               idf/hwabstraction_esp/ below, which lists these same sources
               with a "../../esp/" prefix.
  idf/         ESP-IDF component-registration wrappers.
                 idf/hwabstraction_esp/  the real idf_component_register for
                                         esp/ above, named "hwabstraction_esp"
                                         (symmetric with SaftyFW's
                                         "hwabstraction_pico" library target).
  pico/        pico-sdk backends, plus the SPI/UART owners relocated in from
               SaftyFW/src/:
                 pico/spi/    spi_owner.{c,h}, hal_spi_pico.c
                 pico/uart/   uart_owner.c, uart_owner_tx_policy.{c,h},
                              hal_uart_pico_internal.h (renamed from uart_owner.h)
                 pico/gpio/, pico/adc/, pico/flash/, pico/scratch/, pico/wdt/, pico/time/
  host/        fake backends for MSVC host tests: fake_spi/fake_i2c/fake_uart/
               fake_gpio/fake_adc/fake_kv/fake_time/fake_flash/fake_scratch/
               fake_wdt/fake_pwm/fake_sysinfo (12 total, ~785+ assertions,
               each with a proven negative test). Linked into both
               build_host_tests.ps1 scripts per firmware.
  test/        compile_headers.ps1 (MSVC syntax/ABI check for interface/),
               compile_esp_backends.ps1, compile_pico_backends.ps1,
               test_host_fakes.ps1.
```

Status: all planned interfaces have both a real backend (ESP and/or Pico, as
applicable) and a host fake; every named production consumer has been
migrated (see the plan doc's Phase 3 status list for the item-by-item
record). `firmware/UnitTestFw` stays untouched (owner decision) and is not
part of this tree. `firmware/SaftyFW`'s bootloader stays on raw pico-sdk
calls (separate binary, ~64K budget, no FreeRTOS) and is not migrated.

## Conventions (durable — read before adding a backend or a client)

- **No vtables, no function pointers.** Backend is chosen at link time; one
  backend per build.
- **Opaque handles are fixed-size aligned storage, not pointers.** An opaque
  pointer would force heap allocation per device, and several drivers embed
  a handle *by value* (SX1509, FT6336U, NS2009, MAX31856). Shape:
  `typedef struct { alignas(8) uint8_t storage[N]; } hal_xxx_t;` with a
  `_Static_assert(sizeof(struct impl) <= sizeof(storage), ...)` in the
  backend `.c` so growth is a build error, never a memory bug. Use the
  `_Alignas(8)` keyword form, not `<stdalign.h>`'s `alignas` macro — the
  keyword compiles under plain `/std:c11` on every `cl` invocation in this
  tree; the macro form does not without it, and not every invocation passes
  `/std:`.
- **hal_status_t** carries four values beyond the obvious baseline because
  real callers branch on them: `WEDGED` (sticky, distinct from `NOT_READY`
  — recoverable by re-init vs not), `VERIFY_FAILED` (register read-back
  mismatch; callers retry, unlike plain `IO`), `INVALID_SIZE`, `NOT_FOUND`
  (probe loops continue-on-this; folding into `IO` would abort a scan).
  `hal_status_to_name()` returns a **string** — `gpio_probe` forwards it to
  PcTools the same way it forwarded `esp_err_to_name()`, and PcTools matches
  on the text, never a numeric code.
- **hal_kv (NVS) durability contract:** a `set` MAY already be durable
  before `commit`; `commit` guarantees everything before it. Real
  `nvs_set_*` writes to flash immediately, so "nothing durable until commit"
  is the wrong mental model — `fake_kv_simulate_power_loss()` keeps pending
  writes by default; `fake_kv_set_lossy_uncommitted()` opts into discarding
  them, for a test that wants the other side of the contract.
- **NVS keys have a real 15-character limit** (`NVS_KEY_NAME_MAX_SIZE` is
  16 including the NUL terminator). A 16-char key fails silently on real
  hardware (`ESP_ERR_NVS_KEY_TOO_LONG`, and the pre-HAL caller in this tree
  didn't log it) while the old host stub had no key-length check at all and
  passed. `zones_config_store.c`'s `NVS_KEY_ZONE_NORMALS` was exactly this
  bug (`"zone_normals_cfg"`, 16 chars) — every `zone_normals_save()` had
  silently no-opped on every board ever run. Fixed by renaming to
  `"zone_norm_cfg"` (13 chars); a `NVS_KEY_LEN_CHECK` `_Static_assert` macro
  in the same file now catches any future `NVS_KEY_*` literal over 15 chars
  at compile time. `fake_kv.h`'s `FAKE_KV_MAX_KEY_LEN` (16) is deliberately
  left equal to the real limit — raising it would have hidden this class of
  bug, and it is what caught this one.
- **hal_kv is ESP-only.** SaftyFW's `config_store` is not a KV store — it is
  a fixed 512-byte record, seq-numbered, CRC'd, 8-slot round-robin log in one
  4K sector, gated by a hard safety interlock (write refused while the relay
  is ARMED) and format-versioned with a REFUSE-not-reinterpret policy on a
  too-new record. Forcing it through hal_kv would strip the ARMED gate to a
  caller-side check or bloat the interface with slot/seq/ARMED concepts one
  platform needs. It sits on **hal_flash** instead
  (`config_store_flash.c`), keeping the ARMED gate, the seq/CRC log and the
  REFUSE policy intact at that layer. A `hal_kv_pico.c` draft that tried to
  shoehorn config_store onto `hal_kv.h` was rejected on review for exactly
  this reason.
- **hal_flash program semantics:** programming clears bits only
  (`existing & new` per byte) and programming a non-erased byte is
  *permitted*, not refused — matching real NOR flash, where a missing erase
  is a detectable bit pattern, not an error. `hal_flash_safe_execute()`
  wraps the real RP2040 multicore-XIP lockout (`flash_safe_execute()`); it
  is first-class, not an afterthought, because the hazard is real.
- **hal_uart has two send primitives on purpose:**
  `hal_uart_send()` (non-blocking, whole-buffer-or-BUSY) and
  `hal_uart_send_blocking()` (returns only once the last byte is **on the
  wire**). `uart_protocol.c`'s ACK timer starts right after the blocking
  send returns, so a fire-and-forget send would start the timer before the
  last byte left the FIFO. The Pico backend's `send_blocking` drains the
  software TX ring rather than checking true wire completion
  (`UARTFR.BUSY`, not `TXFE` — `TXFE` clears while the last byte is still
  shifting out) because no Pico consumer originates an ACK-timed send today;
  if one ever does, that is the primitive to add, not to assume.
- **One `hal_uart_t` handle per port.** `hal_uart_attach()` (a transitional
  shim letting `uart_protocol_t` borrow the driver's already-installed
  handle) is gone — `uart_owner_t` embeds the real `hal_uart_t` directly and
  hands out a pointer to it.
- **A driver sharing a port with an existing owner *adopts* the owner's bus
  handle** (`hal_spi_bus_adopt()` / the `hal_i2c_esp_owner.h` /
  `hal_spi_esp_owner.h` bridge headers) — it never calls
  `hal_spi_bus_init()`/`hal_i2c_bus_init()` itself. This is how the display
  and thermocouple bus share one physical SPI host without standing up a
  second competing owner task. The `ALREADY_INIT` re-init recovery path in
  both ESP backends was deleted once adopt existed to replace it.
- **`hal_gpio_init_out()` is latch-then-direction, always.** On ESP-IDF,
  `gpio_set_level()` doesn't touch the direction register and
  `gpio_set_direction()` doesn't touch the level register, so
  level-then-config is glitch-free; several call sites in this tree used to
  do it the other way round (a brief asserted-CS window at boot) and were
  fixed by migrating onto this call. On Pico, the same order applies for the
  same reason. This is a **deliberate, noted behavior change**, not a
  no-op refactor, wherever it fixed a pre-existing wrong-order site.
- **hal_scratch (RP2040 watchdog-scratch registry)** is a *runtime* advisory
  claim table keyed on `(slot, tag)`, not a compile-time one — there is no
  `HAL_SCRATCH_CLAIM()` macro. Slot 4 is hard-refused for claim/write/clear
  (pico-sdk's own `watchdog_enable` reserves it); slot 5 has two legitimate
  writers distinguished by tag, cross-documented in code.
  `vApplicationStackOverflowHook()` writes scratch[5] raw, bypassing the
  registry, because no function call is safe to make from that hook — its
  claim is still *registered*, just not routed through the typed accessors.
- **Two raw GPIO IRQ owners stay raw, permanently:** Pico `thermo_task.c`
  (DRDY, a shared per-core dispatcher — a second registrant would clobber
  the first) and ESP `uart_bridge_io.c` (SX1509 ~INT, per-pin ISR). Adding
  `hal_gpio_irq_attach()` is gated on a *second* consumer ever needing one;
  none exists today.
- **hal_adc is Pico-only, deliberately.** No ESP consumer exists in this
  tree, and ESP-IDF's `adc_oneshot`/`adc_cali` model (a unit handle,
  per-channel atten/bitwidth config, a separate calibration handle) doesn't
  fit this interface's Pico-shaped, handle-less, raw-sample-only signature
  without widening it. A speculative `hal_adc_esp.c` was written and deleted
  for this reason — if an ESP consumer is ever proposed, re-derive the
  interface from ESP-IDF's real shape rather than reusing this one.
- **Permanent holdouts, named individually, none of them TODOs:** `main.c`
  (SaftyFW's GPIO6-low boot step 1, the TIMER_DBGPAUSE register, the
  watchdog-caused-reboot checks — none expressible through `hal_wdt.h`/
  `hal_scratch.h` without widening either past what any real consumer
  needs), `console_uart.c` (Pico, write-only diagnostic, no IRQ, boot-banner
  dependency-free by design), `thermo_task.c` and `uart_bridge_io.c` (the
  two raw GPIO IRQ owners above), `main_boot_early.c` (ESP SPI/I2C bus
  bring-up — the HAL doesn't own bus lifecycle, only transfers),
  `wifi_prov`/`lvgl_port` family (own real periodic `esp_timer_create`
  timer objects / are out-of-scope Wi-Fi-httpd-LVGL portability, not a
  get-time-only swap), `esp_ota_ops.h`/OTA partition writes (out of scope —
  a write seam, not a read seam), SaftyFW's bootloader (separate binary,
  shares only `flash_layout.h`), `firmware/UnitTestFw` (owner decision, do
  not re-propose folding it in).
- **`check_hal_include_boundary.ps1` allowlist policy:** every entry is a
  full repo-relative path (never a directory prefix — SaftyFW has no
  `espInterfaces/`-style subdirectory, so a prefix rule would be asymmetric)
  with `@{ RelPath; Header; Reason; ExpiresAtPhase }`. An entry with no real
  `// TEMPORARY` marker in the code carries no `ExpiresAtPhase` — it is a
  permanent, reviewed holdout, not a deferred migration.

## Known coupling

`firmware/SaftyFW/CMakeLists.txt`'s `hwabstraction_pico` target privately
includes SaftyFW's own project root (for `FreeRTOSConfig.h`). Earlier drafts
of three Pico backend files also reached into `SaftyFW/src/` for
`board_pins.h`; that was removed (pin values now arrive as init-time
parameters from SaftyFW's own callers), so the remaining coupling is just
`FreeRTOSConfig.h` — `hwabstraction_pico` is still not buildable as a fully
standalone library outside a SaftyFW-shaped project, but the board-pins
dependency that used to widen this is gone.
