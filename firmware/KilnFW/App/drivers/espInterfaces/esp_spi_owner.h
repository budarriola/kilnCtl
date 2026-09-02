#ifndef SPI_OWNER_H
#define SPI_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/spi_master.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    spi_host_device_t host;
    QueueHandle_t request_queue;
    TaskHandle_t task_handle;
    SemaphoreHandle_t shutdown_done; /* given by the worker right before it exits */
    bool initialized;
    bool shutdown_requested;
    /* Latched by spi_owner_transfer() the first time it gives up waiting on
     * the owner task (DISPLAY_ST7796_PLAN.md 9.9 / TODO.md: unbounded
     * portMAX_DELAY there defeated every caller-side timeout above it).
     * Once set, every subsequent transfer -- display AND thermocouple alike,
     * both routed through this one owner -- fails fast with
     * ESP_ERR_INVALID_STATE instead of queuing more work behind a task that
     * is presumed stuck.
     *
     * IMPORTANT (opus review, commit f3a1600, corrected 2026-09-01): this is
     * NOT "recovery is a reset, same as any other watchdog trip" -- nothing
     * resets. The owner task, if genuinely wedged, is blocked on a semaphore
     * or a driver call, not spinning with interrupts disabled, so it keeps
     * yielding to the scheduler; the ESP-IDF idle-task TWDT this firmware
     * configures (watchdog_cfg.c) never sees it as starved, and no task in
     * this firmware ever calls esp_task_wdt_add() on itself, so nothing ever
     * trips a watchdog over a wedged SPI owner. Left alone, `wedged` is a
     * silent, permanent, operator-invisible failure of both the display and
     * every thermocouple channel routed through this owner -- see
     * spi_owner_is_wedged() below for the fix (an operator-visible surface)
     * and spi_owner_deinit() for the other half (clearing it on a real
     * re-init) -- see spi_owner_deinit()'s own doc comment in
     * esp_spi_owner.c for the caller contract that recovery path requires
     * (opus review, commit 9fc55d9, M6): it is not safe to call while any
     * transfer could still be in flight. Given the request/semaphore no longer live on the caller's
     * stack (see spi_owner_request_t below), a timed-out request is orphaned
     * rather than corrupting anything if it later completes -- so this flag
     * is a caution against piling more work behind a slow/stuck owner, not a
     * memory-safety necessity, and a future change could choose to retry
     * instead of latch permanently. Latching is kept for now because a
     * transfer that blows a 1000ms budget (~1700x its own worst-case
     * transfer time -- see esp_spi_owner.c's timeout comment) has already
     * demonstrated the bus is behaving abnormally, and repeatedly re-trying
     * a display flush against a bus in that state is not obviously better
     * than failing fast and letting the operator-visible flag below drive
     * whatever recovery action (reboot, bus reset) actually fixes it. */
    volatile bool wedged;

    /* DISPLAY_ST7796_PLAN.md 9.3: set from spi_owner_init()'s dma_use_psram
     * parameter (in turn CONFIG_KILNCTL_SPI_DMA_USE_PSRAM, default OFF).
     * When true, spi_owner_task() sets SPI_TRANS_DMA_USE_PSRAM on every
     * non-polling (display flush) transaction's flags so the driver DMAs
     * straight out of LVGL's PSRAM buffer instead of bounce-copying through
     * internal DRAM first. Never applied to polling transfers (MAX31856
     * register pokes, <=17 bytes, never PSRAM-backed) -- see esp_spi_owner.c.
     */
    bool dma_use_psram;

    /* Module-owned pool backing spi_owner_transfer()'s per-request result
     * storage and completion semaphore -- see this header's own top-of-file
     * note in esp_spi_owner.c and owner_slot_pool.h's invariant. Sized to
     * `queue_len` at spi_owner_init() time (heap-allocated there, since
     * queue_len is a runtime parameter, not a compile-time constant like
     * kiln_io_owner.c's/thermo_owner.c's fixed SLOT_COUNT macros). NEVER the
     * caller's stack: spi_owner_transfer() giving up on a timeout must not
     * leave the owner task holding a pointer into a frame that function has
     * already returned from. */
    struct spi_owner_slot *slots;
    uint8_t *slot_refcount;
    size_t slot_count;
    SemaphoreHandle_t slot_lock;
} spi_owner_t;

typedef struct {
    spi_device_handle_t device;
    const uint8_t *tx_buffer;
    size_t tx_length;
    uint8_t *rx_buffer;
    size_t rx_length;
    int cs_pin;
    int slot; /* index into owner->slots[] -- module-owned result storage and
               * semaphore, assigned by spi_owner_transfer() via
               * owner_slot_pool_alloc(). Never a pointer into the caller's
               * stack -- see owner_slot_pool.h's top comment for why. */
    bool shutdown;
    /* DISPLAY_ST7796_PLAN.md 9.5: MAX31856 register transfers are <=17 bytes
     * and fit the SPI FIFO; measured IDF overhead on S3 is 11us for
     * spi_device_polling_transmit() versus 26us for the queued/ISR path
     * spi_device_transmit() takes. Set only by spi_owner_transfer_polling()
     * (thermocouple call sites); display flush transfers -- larger, DMA'd --
     * keep going through spi_device_transmit(). This changes ONLY which
     * ESP-IDF entry point spi_owner_task() calls; the owner task itself is
     * still the sole issuer of transfers on the bus (the single-owner
     * invariant, DISPLAY_ST7796_PLAN.md section 8), and a polling transmit
     * only ever runs on the owner task's own thread, so it "busy-waits" on
     * that task and nowhere else -- never from an ISR or from a caller's own
     * task context, where a busy-wait would be harmful. */
    bool use_polling;
} spi_owner_request_t;

/* Operator-visible accessor for spi_owner_t::wedged (opus review, commit
 * f3a1600, part (b) of the G2 fix) -- callers that surface board health
 * (dashboard/status JSON, diagnostics_http.c) read this instead of reaching
 * into the struct directly, matching the accessor pattern the rest of this
 * codebase uses for owner-task health flags. */
bool spi_owner_is_wedged(const spi_owner_t *owner);

/* dma_use_psram: DISPLAY_ST7796_PLAN.md 9.3, CONFIG_KILNCTL_SPI_DMA_USE_PSRAM
 * passed through by the caller (default OFF; see spi_owner_t::dma_use_psram
 * above for what it changes). Not read from Kconfig inside this file so the
 * behavior stays a plain, host-testable struct field rather than a
 * compile-time #if buried in the owner task. */
esp_err_t spi_owner_init(spi_owner_t *owner,
                             spi_host_device_t host,
                             UBaseType_t queue_len,
                             UBaseType_t task_priority,
                             uint32_t stack_depth,
                             BaseType_t core_id,
                             bool dma_use_psram);
esp_err_t spi_owner_deinit(spi_owner_t *owner);
esp_err_t spi_owner_transfer(spi_owner_t *owner,
                                 spi_device_handle_t device,
                                 const uint8_t *tx_buffer,
                                 size_t tx_length,
                                 uint8_t *rx_buffer,
                                 size_t rx_length,
                                 int cs_pin);

/* DISPLAY_ST7796_PLAN.md 9.5: identical contract to spi_owner_transfer()
 * above (same queuing, same bounded 9.9 timeout, same wedge semantics) --
 * the only difference is that the owner task issues this transfer with
 * spi_device_polling_transmit() instead of spi_device_transmit(). Intended
 * for MAX31856 register transfers only (<=17 bytes; measured 11us on S3 vs
 * 26us for the queued path) -- NOT for display flush transfers, which stay
 * on spi_owner_transfer() so a large transfer keeps using the ISR/DMA
 * completion path rather than busy-waiting the owner task for its whole
 * duration. */
esp_err_t spi_owner_transfer_polling(spi_owner_t *owner,
                                 spi_device_handle_t device,
                                 const uint8_t *tx_buffer,
                                 size_t tx_length,
                                 uint8_t *rx_buffer,
                                 size_t rx_length,
                                 int cs_pin);

#ifdef __cplusplus
}
#endif

#endif // SPI_OWNER_H
