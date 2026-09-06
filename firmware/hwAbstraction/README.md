# firmware/hwAbstraction

One hardware-abstraction tree shared by KilnFW (ESP32-S3) and SaftyFW
(RP2040). Full design and phased rollout: `docs/HW_ABSTRACTION_PLAN.md`.

```
hwAbstraction/
  interface/   portable headers only (backend-independent). DONE (Phase 0).
  common/      vendor-neutral shared code (hal_status.c).
  esp/         ESP-IDF backends (Phase 1b), plus the SPI/I2C/UART owners and
               uart_protocol moved in from App/drivers/owners/ (Phase 1a WP1):
                 esp/spi/  esp_spi_owner.{c,h}, owner_slot_pool.{c,h}
                 esp/i2c/  i2c_owner.{c,h}
                 esp/uart/ uart_owner.{c,h}, uart_protocol.{c,h}
               esp/CMakeLists.txt is comment-only, not a real ESP-IDF
               component (a bare "esp" component name collides in IDF's flat
               namespace) -- the real component is the thin wrapper at
               idf/hwabstraction_esp/ below, which lists these same sources
               with a "../../esp/" prefix.
  idf/         ESP-IDF component-registration wrappers.
                 idf/hwabstraction_esp/  the real idf_component_register for
                                         esp/ above, named "hwabstraction_esp"
                                         (symmetric with SaftyFW's
                                         "hwabstraction_pico" library target)
                                         instead of the bare "esp".
  pico/        pico-sdk backends (Phase 1b), plus the SPI/UART owners moved in
               from SaftyFW/src/ (Phase 1a WP2):
                 pico/spi/  spi_owner.{c,h}
                 pico/uart/ uart_owner.c, uart_owner_tx_policy.{c,h},
                            hal_uart_pico_internal.h (renamed from uart_owner.h)
  host/        fake backends for MSVC host tests (Phase 2).
  test/        compile_headers.ps1 -- MSVC syntax/ABI check for interface/.
```

Status: Phase 1a WP1/WP2 in progress -- owner modules are being relocated
here per-interface (colocated under esp/<bus>/ and pico/<bus>/ alongside that
interface's HAL backend, not under a separate owners/ subtree), with each
processor's file staying in its own tree. Phase 1b backend files above are
already landed for several interfaces. See the plan's "Phases" section for
what each later phase adds.

Known coupling: `firmware/SaftyFW/CMakeLists.txt`'s `hwabstraction_pico`
target privately includes SaftyFW's own project root (for
`FreeRTOSConfig.h`) and `SaftyFW/src/` (for `board_pins.h`, needed by three
`// TEMPORARY (HAL Phase 1b)` sites -- `pico/spi/spi_owner.c`,
`pico/uart/uart_owner.c`, `pico/uart/hal_uart_pico.c`). As a result
`hwabstraction_pico` is **not buildable outside a SaftyFW-shaped project**:
it depends on headers that live outside this tree, not just on the pico-sdk.
See `docs/HW_ABSTRACTION_PLAN.md`'s Phase 1b section ("Open item
(2026-09-05 review finding)") for the plan to narrow this.
