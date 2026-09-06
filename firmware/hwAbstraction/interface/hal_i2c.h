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

/* Owner-task sizing, mirroring hal_spi_bus_cfg_t's field names/shape
 * (interface/hal_spi.h) -- the real i2c_owner_init() takes queue_len,
 * task_priority, stack_depth and core_id per call site (SX1509.c:414 passes
 * (8, 5, 4096, tskNO_AFFINITY); FT6336U.c:113/NS2009.c:83 both pass (8, 5,
 * 3072, tskNO_AFFINITY)) so each attached device's owner task can be sized
 * independently and registered for stack-margin reporting with a
 * caller-known stack_depth. 0 in any field means "backend default", equal
 * to the real value for this instance class today (queue_len 8,
 * task_priority 5, stack_depth 4096 -- the larger of the two live call
 * sites, so no real caller is under-provisioned relative to today). core_id:
 * HAL_CORE_ANY (hal_status.h) or an explicit core number. */
typedef struct {
    int scl_pin;
    int sda_pin;
    uint32_t queue_len;
    int task_priority;
    uint32_t stack_depth;
    int core_id;
} hal_i2c_bus_cfg_t;

/* Same ALREADY_INIT decision as hal_spi_bus_init (see hal_spi.h): a bus
 * already initialized (e.g. re-entry after a soft reset) returns HAL_OK
 * with an INFO log, not HAL_BUSY, for the same reason -- benign re-entry,
 * not caller error, not a signal to retry. On this path the backend must
 * still recover a usable bus handle and still create THIS hal_i2c_bus_t's
 * own request queue/owner task -- a fresh hal_i2c_bus_t has neither yet
 * even though the underlying peripheral is already up (mirrors
 * hal_spi_bus_init's identical ALREADY_INIT handling). */
hal_status_t hal_i2c_bus_init(hal_i2c_bus_t *bus, int bus_id,
                               const hal_i2c_bus_cfg_t *cfg);
hal_status_t hal_i2c_bus_deinit(hal_i2c_bus_t *bus);

/* Returns the backend's owner-task handle (FreeRTOS TaskHandle_t on ESP,
 * cast to void*) for stack-margin registration BY THE CALLER -- see
 * hal_uart_get_task_handle's doc comment (hal_uart.h) for the full
 * rationale, identical here. NULL if bus has no task (not initialized, or a
 * backend with no owner task, e.g. host today). */
void *hal_i2c_get_task_handle(const hal_i2c_bus_t *bus);

hal_status_t hal_i2c_device_attach(hal_i2c_bus_t *bus, hal_i2c_device_t *dev,
                                    uint8_t addr, uint32_t clock_hz);

/* Reverses hal_i2c_device_attach(): releases the underlying device slot
 * (ESP: i2c_master_bus_rm_device()) so a probe-succeeded/identity-failed
 * cold path can back out cleanly instead of leaking a device handle on the
 * shared bus. Safe to call on a dev that was never attached or already
 * detached -- returns HAL_NOT_READY rather than crashing, mirroring
 * hal_i2c_transfer()'s not-ready check. Never touches the bus itself. */
hal_status_t hal_i2c_device_detach(hal_i2c_device_t *dev);

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
