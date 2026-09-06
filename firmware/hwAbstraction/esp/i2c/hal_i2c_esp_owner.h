/* hal_i2c_esp_owner.h -- ESP-only adopt bridge: wraps an ALREADY-INITIALIZED
 * i2c_master_bus_handle_t + i2c_owner_t into a hal_i2c_bus_t, for a second
 * HAL consumer sharing a physical I2C port with a driver that owns its own
 * i2c_owner_t directly (SX1509.c's `e->owner`, initialized on the shared
 * I2C_NUM_0 port -- see SX1509.h/SX1509_internal.h).
 *
 * Why this exists (2026-09-06 hardware bug): hal_i2c_bus_init()'s old
 * ALREADY_INIT recovery path called i2c_new_master_bus() first and only
 * fell back to i2c_master_get_bus_handle() after that call FAILED with
 * ESP_ERR_INVALID_STATE. IDF's cleanup on that failed call left the shared
 * I2C_NUM_0 port half-released ("acquire bus failed" / "Bus not freed
 * entirely" in the boot log), so every LATER SX1509/i2c_owner transfer on
 * the port returned ESP_ERR_INVALID_RESPONSE: LCD D/C (on the SX1509)
 * failed, the SPI owner queue starved waiting on it, MAX31856 channel locks
 * timed out, and boot took 79s. hal_i2c_bus_init() must never be called
 * again on a port that is already open -- see its own header comment; this
 * adopt bridge is the replacement for that call on a port a driver already
 * brought up itself.
 *
 * Distinct from the ALREADY_INIT recovery this replaces: it never touches
 * i2c_new_master_bus()/i2c_master_get_bus_handle() at all, and it never
 * creates a second i2c_owner_t (a second independent owner task/queue on
 * one physical port breaks the single-writer invariant hal_i2c/i2c_owner
 * exist to protect -- the SAME class of bug hal_spi_esp_owner.h's own
 * header comment documents for the SPI bus).
 *
 * `owner` must already be initialized (i2c_owner_init() succeeded on it --
 * e.g. SX1509_start() has already run). `handle` is checked against
 * owner->bus defensively; adopt fails with HAL_INVALID_ARG if they disagree
 * or owner is not yet initialized.
 *
 * The resulting hal_i2c_bus_t has bus_owned = false and owner_owned = false:
 * hal_i2c_bus_deinit() on it clears only this bus_t's own local storage. It
 * never calls i2c_owner_deinit() or i2c_del_master_bus() on the shared
 * resources this instance did not create -- those stay alive for whoever
 * else (SX1509, in the case above) still owns them. Do NOT register the
 * returned bus's hal_i2c_get_task_handle() for stack-margin reporting a
 * second time -- the owner task is already registered by whoever created
 * it (SX1509_start() registers "i2c_owner_sx1509"). */
#ifndef HAL_I2C_ESP_OWNER_H
#define HAL_I2C_ESP_OWNER_H

/* i2c_owner.h pulls in driver/i2c_master.h for i2c_master_bus_handle_t --
 * not included directly here, to avoid a redundant second hit against
 * tools/check_hal_include_boundary.ps1's hwAbstraction driver/ ratchet. */
#include "hal_i2c.h"
#include "i2c_owner.h"

#ifdef __cplusplus
extern "C" {
#endif

hal_status_t hal_i2c_esp_adopt(hal_i2c_bus_t *bus, i2c_master_bus_handle_t handle,
                                i2c_owner_t *owner);

#ifdef __cplusplus
}
#endif

#endif /* HAL_I2C_ESP_OWNER_H */
