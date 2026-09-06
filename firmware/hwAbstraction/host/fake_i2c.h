/* fake_i2c.h -- host fake backend for hal_i2c.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION_PLAN.md "Host fakes (Phase 2 specs)" -- fake_i2c
 * (must): ordered record/replay, NACK/timeout injection.
 *
 * Handle storage: hal_i2c_bus_t/hal_i2c_device_t opaque storage is stamped
 * with a {magic, slot} tag, same as fake_spi/fake_uart -- the record/queue
 * state does not fit HAL_I2C_BUS_STORAGE_BYTES/HAL_I2C_DEVICE_STORAGE_BYTES.
 *
 * ACK/NACK is scripted per-ADDRESS on the bus (not per-device), because
 * hal_i2c_probe() takes a bus + raw addr, with no device handle yet --
 * exactly the SX1509.c/i2c_scan.c probe-before-attach pattern hal_i2c.h's
 * comment documents. A device attached at an address inherits that
 * address's ack/nack script for hal_i2c_transfer() too, so a scripted NACK
 * is observable through both hal_i2c_probe() and a subsequent transfer.
 */
#ifndef KILNCTL_FAKE_I2C_H
#define KILNCTL_FAKE_I2C_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "hal_i2c.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_I2C_MAX_BUSES        2
#define FAKE_I2C_MAX_DEVICES      8
#define FAKE_I2C_MAX_ADDR_SCRIPTS 16
#define FAKE_I2C_TRANSFER_LOG_CAP 64
#define FAKE_I2C_MAX_TX_BYTES     64
#define FAKE_I2C_MAX_RX_BYTES     64

typedef struct {
    uint8_t addr;
    int      dev_slot;   /* -1 if this transfer was via a device already
                           * detached, kept for the log entry only */
    uint8_t  tx[FAKE_I2C_MAX_TX_BYTES];
    size_t   tx_len;
    uint8_t  rx[FAKE_I2C_MAX_RX_BYTES];
    size_t   rx_len;
    uint32_t timeout_ms;
} fake_i2c_transfer_record_t;

/* Clears every bus/device slot, per-address scripts, the transfer log, and
 * all injected faults. Call between test cases. */
void fake_i2c_reset_all(void);

bool fake_i2c_bus_is_live(const hal_i2c_bus_t *bus);
bool fake_i2c_device_is_live(const hal_i2c_device_t *dev);

/* Ordered transfer log, oldest first, across all devices on the bus.
 * hal_i2c_probe() calls that NACK are NOT logged here (a probe is not a
 * transfer); only hal_i2c_transfer() calls are recorded. */
size_t fake_i2c_transfer_count(void);
const fake_i2c_transfer_record_t *fake_i2c_transfer(size_t index);

/* Scripts addr to always NACK (hal_i2c_probe -> HAL_NOT_FOUND;
 * hal_i2c_transfer on a device at that addr -> HAL_IO) until
 * fake_i2c_script_ack() or fake_i2c_reset_all() clears it. Overwrites any
 * prior ack/nack/timeout script for this addr. Silently no-ops if the
 * per-bus script table (FAKE_I2C_MAX_ADDR_SCRIPTS) is full and addr is not
 * already scripted. */
void fake_i2c_script_nack(hal_i2c_bus_t *bus, uint8_t addr);

/* Scripts addr to ACK (the default for any address never scripted). Also
 * used to un-script a previous nack/timeout. */
void fake_i2c_script_ack(hal_i2c_bus_t *bus, uint8_t addr);

/* Scripts addr's NEXT hal_i2c_transfer() (device already attached at that
 * addr) to return HAL_TIMEOUT once, then reverts to whatever ack/nack state
 * was scripted before (default: ack). Does not affect hal_i2c_probe(). */
void fake_i2c_script_transfer_timeout(hal_i2c_bus_t *bus, uint8_t addr);

/* Queues bytes returned (FIFO, one hal_i2c_transfer() rx request consumed
 * per call) for a device at addr. Unscripted reads return all zero bytes
 * (not an error). Returns HAL_NOT_READY if bus is not live, HAL_INVALID_ARG
 * for a NULL data pointer with nonzero len, HAL_NO_MEM if that addr's
 * per-bus rx queue is full. */
hal_status_t fake_i2c_script_rx(hal_i2c_bus_t *bus, uint8_t addr,
                                 const uint8_t *data, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_I2C_H */
