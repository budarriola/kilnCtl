/* hal_i2c.h -- portable I2C bus/device interface. ESP-only today (SaftyFW
 * has no I2C); a pico backend is future work modeled on spi_owner's mutex
 * pattern. See docs/HW_ABSTRACTION_PLAN.md "hal_i2c".
 *
 * Threading/ownership contract:
 *  - Single-writer per bus, same as hal_spi: one in-flight transfer per bus
 *    at a time, enforced by the backend (i2c_owner.c's semaphore today).
 *  - Buffer-copy-in: tx/rx buffers need only remain valid for the call.
 *  - The ESP backend must preserve i2c_owner.c's two hard-won behaviors:
 *    a STATIC (not heap) per-call semaphore (2026-08-20 SRAM-starvation
 *    fix -- do not regress to heap allocation per call), and a
 *    worker-enforced timeout with the caller waiting unbounded
 *    (use-after-free avoidance: the caller must never give up on a
 *    transfer while the worker still holds a reference to its stack
 *    buffer).
 *  - Preserve the SX1509 asymmetry: caller-side wait 200 ms vs device-side
 *    retry timeout 6 s are deliberately different; do not collapse them to
 *    one timeout value when adapting a consumer.
 */
#ifndef KILNCTL_HAL_I2C_H
#define KILNCTL_HAL_I2C_H

#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

/* HAL_ALIGNAS8 is defined in hal_status.h (included above) so it is shared
 * across hal_uart.h/hal_i2c.h/hal_spi.h instead of copied in each. */

#ifdef __cplusplus
extern "C" {
#endif

/* Reservations per docs/HW_ABSTRACTION_PLAN.md "Opaque handles": bus 128 B
 * (headroom for a future static per-call semaphore), device 16 B (measured
 * today: i2c_owner_t ~20 B on 32-bit -- bus reservation covers growth). */
#define HAL_I2C_BUS_STORAGE_BYTES    128
#define HAL_I2C_DEVICE_STORAGE_BYTES 16

typedef struct {
    HAL_ALIGNAS8 uint8_t storage[HAL_I2C_BUS_STORAGE_BYTES];
} hal_i2c_bus_t;

typedef struct {
    HAL_ALIGNAS8 uint8_t storage[HAL_I2C_DEVICE_STORAGE_BYTES];
} hal_i2c_device_t;

/* Same ALREADY_INIT decision as hal_spi_bus_init (see hal_spi.h): a bus
 * already initialized (e.g. re-entry after a soft reset) returns HAL_OK
 * with an INFO log, not HAL_BUSY, for the same reason -- benign re-entry,
 * not caller error, not a signal to retry. */
hal_status_t hal_i2c_bus_init(hal_i2c_bus_t *bus, int bus_id,
                               int scl_pin, int sda_pin);
hal_status_t hal_i2c_bus_deinit(hal_i2c_bus_t *bus);

hal_status_t hal_i2c_device_attach(hal_i2c_bus_t *bus, hal_i2c_device_t *dev,
                                    uint8_t addr, uint32_t clock_hz);

hal_status_t hal_i2c_transfer(hal_i2c_device_t *dev,
                               const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_len,
                               uint32_t timeout_ms);

/* Probes for a device at addr with no transaction beyond the ACK itself.
 * Returns HAL_NOT_FOUND on no ACK -- this is the documented reason
 * HAL_NOT_FOUND exists in hal_status.h's table. ESP: i2c_master_probe.
 * SX1509.c:532/585 and i2c_scan.c:30 are today's callers of the pre-HAL
 * equivalent; both probe-before-attach to avoid turning an absent part into
 * a bus-reset-and-retry per transfer (see SX1509.c's probe comment). */
hal_status_t hal_i2c_probe(hal_i2c_bus_t *bus, uint8_t addr,
                            uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_I2C_H */
