/* hal_spi_esp_owner.h -- ESP-only escape hatch from a hal_spi_bus_t back to
 * the raw esp_spi_owner.c spi_owner_t underneath it.
 *
 * HAL Phase 1b migrated MAX31856.c (the thermocouple channels) to
 * interface/hal_spi.h, but the display driver (ILI9488_start(),
 * panel_spi_bringup.c) still takes a raw spi_owner_t* -- it shares the same
 * physical SPI bus/host as the thermocouples (one bus, four chip selects;
 * docs/HW_ABSTRACTION_PLAN.md "Bus-init semantics"), and per this migration's
 * scope (see MAX31856.c's own header comment / docs/HW_ABSTRACTION_PLAN.md
 * Phase 2 status) the display is left on spi_owner_t for now -- migrating it
 * too is a separate, listed follow-up, not folded into this SPI-arbiter-
 * duplication fix.
 *
 * That means whoever brings up the shared bus (MAX31856_bus_init(), which
 * now owns a hal_spi_bus_t, not a raw spi_owner_t) must still be able to
 * hand the display driver a pointer to the SAME owner task/queue/pool --
 * two independent spi_owner_t instances arbitrating one physical bus would
 * violate the single-writer invariant this whole design exists to protect.
 * This accessor reaches into hal_spi_esp.c's private bus impl (both files
 * live in firmware/hwAbstraction/esp/spi/, and hal_spi_esp_bus_impl_t is
 * hal_spi_esp.c's own opaque-storage layout, never exposed through the
 * portable interface/hal_spi.h) and returns the one spi_owner_t inside it --
 * NULL if `bus` was never initialized. ESP-only; no pico/host equivalent
 * needed until (if ever) a second backend also has a raw non-HAL consumer to
 * bridge to. Delete this header the day the display migrates to hal_spi.h
 * too. */
#ifndef HAL_SPI_ESP_OWNER_H
#define HAL_SPI_ESP_OWNER_H

#include "esp_spi_owner.h"
#include "hal_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

spi_owner_t *hal_spi_esp_get_owner(hal_spi_bus_t *bus);

#ifdef __cplusplus
}
#endif

#endif /* HAL_SPI_ESP_OWNER_H */
