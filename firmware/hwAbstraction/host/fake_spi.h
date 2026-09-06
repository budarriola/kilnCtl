/* fake_spi.h -- host fake backend for hal_spi.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION.md "Host fakes (Phase 2 specs)" -- fake_spi
 * (must): ordered transfer record (buf/len/flags/cs/polling-vs-queued);
 * injectable enqueue-timeout, completion-timeout, pool exhaustion with
 * distinct side effects (wedge latch vs not, refcount invariants);
 * synchronous run-owner-loop driver. Should: async callback with
 * fire-after-LAST-chunk semantics.
 *
 * Handle storage: hal_spi_bus_t/hal_spi_device_t opaque storage is stamped
 * with a {magic, slot} tag in hal_spi_bus_init()/hal_spi_device_attach(),
 * same as fake_uart -- the record/queue state a useful fake needs (an
 * ordered transfer log, a scripted RX byte source, a pending-async-completion
 * queue) does not fit HAL_SPI_BUS_STORAGE_BYTES/HAL_SPI_DEVICE_STORAGE_BYTES.
 *
 * Async model: this fake never calls a completion callback from inside
 * hal_spi_transfer_async() itself -- a real async backend (the ESP owner
 * queue) completes on a different task, and a test relying on "the callback
 * already ran by the time transfer_async() returns" would pass here and
 * deadlock/misbehave on hardware. Instead, hal_spi_transfer_async() enqueues
 * a pending completion and the test calls fake_spi_pump() to fire callbacks
 * deterministically, oldest-enqueued first (matching the ESP queue's FIFO
 * owner-task order) -- this is the "synchronous run-owner-loop driver" the
 * plan calls for.
 */
#ifndef KILNCTL_FAKE_SPI_H
#define KILNCTL_FAKE_SPI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "hal_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_SPI_MAX_BUSES        4
#define FAKE_SPI_MAX_DEVICES      8
#define FAKE_SPI_TRANSFER_LOG_CAP 64
#define FAKE_SPI_MAX_TX_BYTES     256
#define FAKE_SPI_MAX_RX_BYTES     256
/* Max number of distinct scripted rx responses queued per device -- each
 * fake_spi_script_rx() call enqueues ONE response consumed by ONE later
 * transfer, not a byte stream (a transfer's rx length is decided by its
 * caller, so responses cannot be split/merged like fake_uart's ring). */
#define FAKE_SPI_RX_SCRIPT_QUEUE_CAP 8
#define FAKE_SPI_MAX_PENDING_ASYNC 8

typedef enum {
    FAKE_SPI_XFER_NORMAL = 0,
    FAKE_SPI_XFER_POLLING,
    FAKE_SPI_XFER_ASYNC,
} fake_spi_xfer_kind_t;

typedef struct {
    fake_spi_xfer_kind_t kind;
    int      dev_slot;
    uint8_t  tx[FAKE_SPI_MAX_TX_BYTES];
    size_t   tx_len;
    uint8_t  rx[FAKE_SPI_MAX_RX_BYTES];
    size_t   rx_len;
    uint32_t timeout_ms;
    /* Added 2026-09-06 (opus review of MAX31856.c's HAL Phase 1b migration):
     * the device's cs_pin exactly as it was at hal_spi_device_attach() time
     * (hal_spi_device_cfg_t.cs_pin -- the bit-banged CS pin, HAL_CS_NONE if
     * this device uses hardware CS or was never attached). Every backend
     * that bit-bangs CS is expected to drive this same pin for the
     * transfer -- a driver that silently dropped its per-channel CS (as
     * MAX31856.c's software-CS branch briefly did) is otherwise invisible to
     * a host test, since a transfer with no CS asserted still "succeeds" and
     * a scripted rx response still decodes to a plausible-looking value. */
    int      cs_pin;
} fake_spi_transfer_record_t;

/* Clears every bus/device slot, the transfer log, and all injected faults.
 * Call between test cases. */
void fake_spi_reset_all(void);

bool fake_spi_bus_is_live(const hal_spi_bus_t *bus);
bool fake_spi_device_is_live(const hal_spi_device_t *dev);

/* Ordered transfer log, oldest first, across ALL devices on ALL buses --
 * mirrors the single shared owner queue the plan documents (display and
 * thermo share one FIFO). index >= fake_spi_transfer_count() returns NULL. */
size_t fake_spi_transfer_count(void);
const fake_spi_transfer_record_t *fake_spi_transfer(size_t index);

/* Enqueues ONE scripted rx response (up to FAKE_SPI_MAX_RX_BYTES) for this
 * device. Each transfer call (hal_spi_transfer/_polling/_async) that
 * requests rx bytes consumes the oldest still-queued response, FIFO, copying
 * min(scripted_len, requested rx_len) bytes into the caller's rx buffer and
 * zero-filling the remainder. A call with no scripted response left reads
 * back all zero bytes (same as an uninitialized rx buffer would on real
 * silicon -- not an error). Returns HAL_NO_MEM if the per-device queue
 * (FAKE_SPI_RX_SCRIPT_QUEUE_CAP) is full. */
hal_status_t fake_spi_script_rx(hal_spi_device_t *dev, const uint8_t *data, size_t len);

/* Host-fake model of hal_spi_bus_adopt() (interface/hal_spi.h, backed on ESP
 * by hal_spi_esp.c's hal_spi_bus_adopt()): stamps `bus` to share the same
 * underlying slot as an already-live `existing` bus, so transfers/scripts/
 * the transfer log through EITHER handle observe the same state, while
 * hal_spi_bus_deinit() on the adopted `bus` never frees the shared slot
 * (only a deinit of the ORIGINAL bus that owns it does) -- mirrors
 * fake_i2c_bus_adopt()'s contract exactly. Returns HAL_NOT_READY if
 * `existing` is not a live bus, HAL_INVALID_ARG if either pointer is NULL. */
hal_status_t fake_spi_bus_adopt(hal_spi_bus_t *bus, const hal_spi_bus_t *existing);

/* --- Fault injection, per bus. Cleared by fake_spi_reset_all() and by a
 * fresh hal_spi_bus_init() on that bus_id's slot. --- */

/* Next N hal_spi_transfer*() calls on ANY device of this bus return
 * HAL_TIMEOUT without touching rx or the transfer log (an enqueue-timeout:
 * the request was never accepted, so it must not appear in the ordered
 * log). Does not affect hal_spi_transfer_async(), which is a distinct
 * completion-timeout path below. */
void fake_spi_inject_enqueue_timeout(hal_spi_bus_t *bus, int count);

/* Next N pumped async completions on this bus fire with HAL_TIMEOUT instead
 * of HAL_OK, and do NOT latch the wedge (a completion timeout is not the
 * same failure class as pool exhaustion -- see fake_spi_bus_is_wedged). */
void fake_spi_inject_completion_timeout(hal_spi_bus_t *bus, int count);

/* Fires the oldest pending async completion for this bus (FIFO across all
 * devices on the bus, matching the single shared owner queue), invoking its
 * callback synchronously with ctx and the appropriate status. Returns false
 * if nothing is pending. Call in a loop to drain everything queued. */
bool fake_spi_pump(hal_spi_bus_t *bus);

size_t fake_spi_pending_async_count(const hal_spi_bus_t *bus);

/* Async pool exhaustion (FAKE_SPI_MAX_PENDING_ASYNC pending completions on
 * one bus with none pumped) IS distinct from an enqueue/completion timeout:
 * it latches the bus wedge (hal_spi_bus_is_wedged() reads true) because the
 * plan documents wedge as a latched, sticky failure requiring re-init --
 * unlike a single timed-out transfer, exhausting the async slot pool means
 * the bus itself cannot accept more async work until reset. */

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_SPI_H */
