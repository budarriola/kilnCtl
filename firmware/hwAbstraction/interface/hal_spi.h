/* hal_spi.h -- portable SPI bus/device interface.
 *
 * Derived from esp_spi_owner (queued owner task, slot pool, wedge latch) and
 * SaftyFW's spi_owner (mutex + direct blocking call, deliberately no task).
 * This interface must NOT assume an owner task exists -- both models
 * implement it. See docs/HW_ABSTRACTION_PLAN.md "hal_spi".
 *
 * Threading/ownership contract:
 *  - Single-writer per bus: exactly one task/context may call
 *    hal_spi_transfer* on a given hal_spi_bus_t at a time. The ESP backend
 *    enforces this by serializing through its owner queue; the pico/host
 *    backends enforce it by mutex. Callers must not assume a second queued
 *    caller is safe without that serialization -- there is no reentrant
 *    fast path.
 *  - Buffer-copy-in for the SYNCHRONOUS variants (hal_spi_transfer,
 *    hal_spi_transfer_polling): tx/rx buffers are only guaranteed valid for
 *    the duration of the call. For hal_spi_transfer_async, this does NOT
 *    hold: the backend does not copy the tx bytes, only request BOOKKEEPING
 *    state (the ESP slot pool holds the request struct/semaphore, never the
 *    caller's stack) -- the caller's tx (and rx, if used) buffers must
 *    remain valid and unmodified until the completion callback fires,
 *    exactly as esp_spi_owner requires today. A caller that reuses or frees
 *    an async buffer before its callback runs corrupts the in-flight
 *    transfer.
 *  - Wedge semantics: hal_spi_bus_is_wedged() reports a latched, sticky
 *    failure that persists until the bus is re-initialized; it is distinct
 *    from a single transfer returning HAL_TIMEOUT/HAL_BUSY. pico/host
 *    backends have no wedge concept and always report false.
 *  - One request build per call: a wrapper must not construct a HAL-level
 *    request struct that is then translated into a second backend-level
 *    request struct -- match esp_spi_owner's one ~56 B request per call.
 */
#ifndef KILNCTL_HAL_SPI_H
#define KILNCTL_HAL_SPI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "hal_status.h"

/* HAL_ALIGNAS8 is defined in hal_status.h (included above) so it is shared
 * across hal_uart.h/hal_i2c.h/hal_spi.h instead of copied in each. */

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque storage, option (a): fixed-size aligned storage owned by the
 * caller (often embedded by value in a driver struct, e.g. MAX31856.h's
 * spi_owner_t member), sized generously so a backend growing its impl
 * struct is a build-time _Static_assert failure, never a memory-safety bug.
 * Reservations per docs/HW_ABSTRACTION_PLAN.md "Opaque handles": bus 64 B,
 * device 32 B (measured today: spi_owner_t ~48 B on 32-bit). */
#define HAL_SPI_BUS_STORAGE_BYTES    64
#define HAL_SPI_DEVICE_STORAGE_BYTES 32

typedef struct {
    HAL_ALIGNAS8 uint8_t storage[HAL_SPI_BUS_STORAGE_BYTES];
} hal_spi_bus_t;

typedef struct {
    HAL_ALIGNAS8 uint8_t storage[HAL_SPI_DEVICE_STORAGE_BYTES];
} hal_spi_device_t;

#define HAL_CS_NONE (-1)

typedef enum {
    HAL_SPI_MODE_0 = 0,  /* display */
    HAL_SPI_MODE_1 = 1,  /* MAX31856 */
} hal_spi_mode_t;

typedef struct {
    uint32_t clock_hz;         /* 4 MHz MAX31856; display higher */
    hal_spi_mode_t mode;
    size_t queue_size;         /* ESP device queue depth (1 today); pico ignores */
    int hw_cs;                 /* hardware CS line, or HAL_CS_NONE */
    uint32_t input_delay_ns;   /* 50 ns for the thermo bus; pico ignores */
    int cs_pin;                /* bit-banged CS around each transfer, or HAL_CS_NONE */
} hal_spi_device_cfg_t;

/* Argument order matches every real callback in this tree (panel_spi_blit.c's
 * spi_owner_async_done_cb_t: `done_cb(cb_ctx, result)`), ctx first -- not
 * hal_status_t first as Phase 0 originally had it. */
typedef void (*hal_spi_async_cb_t)(void *ctx, hal_status_t result);

/* Bus-level config, carrying both the pins AND the owner-task parameters
 * esp_spi_owner.h's spi_owner_init() takes from callers today (see
 * MAX31856.c:375's call). A bare (sck,mosi,miso) tuple, Phase 0's original
 * shape, would have dropped queue_len/task_priority/stack_depth/core_id --
 * caller control over the owner task and its registration for stack-margin
 * reporting (CLAUDE.md "Register every new task for stack-margin reporting")
 * must survive the HAL wrap, not just the pins. */
typedef struct {
    int sck_pin;
    int mosi_pin;
    int miso_pin;
    size_t queue_len;         /* ESP: spi_owner_init's queue_len (8 today,
                                * MAX31856.c:375). Pico ignores. */
    int task_priority;        /* ESP: spi_owner_init's task_priority (5
                                * today). Pico ignores (no owner task). */
    uint32_t stack_depth;     /* ESP: spi_owner_init's stack_depth (4096
                                * today). MUST still be registered for
                                * stack-margin reporting by the backend that
                                * creates the task -- this struct only carries
                                * the value, it does not do the registration. */
    int core_id;               /* ESP: spi_owner_init's core_id. Use
                                 * HAL_CORE_ANY (hal_status.h, -1, numerically
                                 * equal to FreeRTOS's tskNO_AFFINITY -- every
                                 * real caller today) or an explicit core
                                 * number; 0 IS core 0, not "unset" -- see
                                 * HAL_CORE_ANY's own comment for the bug this
                                 * distinction fixes. Pico ignores. */
    bool dma_use_psram;        /* DISPLAY_ST7796_PLAN.md 9.3; ESP only. */
    bool async_flush;          /* DISPLAY_ST7796_PLAN.md 9.6; ESP only. */
    size_t max_transfer_sz;    /* ESP: spi_bus_config_t.max_transfer_sz --
                                 * largest single transfer the bus DMA must
                                 * support. MAX31856.c:344-355 initializes
                                 * this bus first with a thermocouple-sized
                                 * value; the display shares the same bus_id
                                 * and needs a larger value for full-frame
                                 * blits. Since only the first init's config
                                 * wins (see ALREADY_INIT below), whichever
                                 * caller initializes first must pass the
                                 * larger of the two requirements. 0 lets the
                                 * backend pick its own default. Pico ignores. */
    int dma_chan;              /* HAL-defined sentinel, NOT the raw ESP
                                 * spi_common_dma_t enum -- that enum is
                                 * SPI_DMA_DISABLED=0, SPI_DMA_CH1=1,
                                 * SPI_DMA_CH2=2, SPI_DMA_CH_AUTO=3 (see
                                 * esp-idf/components/esp_driver_spi/include/
                                 * driver/spi_common.h), so 0 means DISABLED
                                 * there, not "auto". A zero-initialized
                                 * hal_spi_bus_cfg_t must default to letting
                                 * the backend pick (auto), not silently kill
                                 * DMA on the ST7796 blit path this field was
                                 * added for -- hence HAL_SPI_DMA_AUTO == 0
                                 * here instead. Use HAL_SPI_DMA_AUTO,
                                 * HAL_SPI_DMA_NONE, or an explicit positive
                                 * channel (1..2). The ESP backend maps
                                 * AUTO -> SPI_DMA_CH_AUTO(3) and
                                 * NONE -> SPI_DMA_DISABLED(0); an explicit
                                 * channel N maps to the raw enum value N.
                                 * Pico ignores. */
} hal_spi_bus_cfg_t;

/* Sentinels for hal_spi_bus_cfg_t.dma_chan -- deliberately NOT the same
 * numbering as ESP-IDF's spi_common_dma_t (see the field comment above).
 * HAL_SPI_DMA_AUTO == 0 so a zero-initialized cfg struct defaults to
 * "let the backend pick", never "disabled". */
#define HAL_SPI_DMA_AUTO 0
#define HAL_SPI_DMA_NONE (-1)

/* ALREADY_INIT decision, SUPERSEDED 2026-09-06 (same class of bug as
 * hal_i2c_esp_owner.h's 2026-09-06 fix): this used to say a second
 * hal_spi_bus_init() on an already-open bus_id (observed on the ESP after a
 * JTAG reset, spi_bus_initialize() returning ESP_ERR_INVALID_STATE) was
 * treated as benign and returned HAL_OK. That recovery only re-shared the
 * raw host peripheral -- it did NOT stop a second caller from getting its
 * OWN spi_owner_t/task, i.e. a second independent single-writer arbiter on
 * one physical SPI bus, which is exactly the invariant this interface
 * exists to protect (see the "Single-writer per bus" contract note above).
 * On this board it fired every boot (main_boot_early.c opens the shared
 * KilnFW thermo/display bus first with the display's larger config,
 * MAX31856_bus_init() used to call hal_spi_bus_init() again and hit this
 * path) and logged a spurious `spi_common` error even though nothing was
 * actually broken by it -- but the mechanism was fragile by luck, not by
 * design. hal_spi_bus_init() on an already-open bus_id is now a plain, loud
 * error (whatever the backend's underlying re-init call reports) -- no
 * recovery. A caller sharing a bus another caller already brought up must
 * use hal_spi_bus_adopt() below instead of calling hal_spi_bus_init() at
 * all. Only the first (real) hal_spi_bus_init() on a shared bus_id's
 * configuration takes effect, so that first caller must still request the
 * largest max_transfer_sz any sharer will need (the display's, not the
 * thermocouple driver's, on the KilnFW shared bus) and the deepest queue_len
 * any sharer needs. */
hal_status_t hal_spi_bus_init(hal_spi_bus_t *bus, int bus_id,
                               const hal_spi_bus_cfg_t *cfg);

/* Adopts an already-initialized hal_spi_bus_t (`existing`) into `bus`,
 * sharing the same underlying host peripheral/owner rather than
 * re-initializing it -- the replacement for the ALREADY_INIT recovery
 * removed above, and the SPI analog of hal_i2c_esp_adopt() (see
 * firmware/hwAbstraction/esp/i2c/hal_i2c_esp_owner.h's header comment for
 * the fuller rationale on why "recover from a failed re-init" and "adopt
 * what's already there" are not the same thing). `existing` must already be
 * a live, successfully-initialized bus -- HAL_NOT_READY if not, HAL_INVALID_ARG
 * if either pointer is NULL. The adopted `bus` never owns the peripheral or
 * the underlying owner/task: hal_spi_bus_deinit(bus) tears down only this
 * instance's own local bookkeeping, never the shared resources -- some other
 * caller (whoever's hal_spi_bus_init() actually created them) still needs
 * them. bus_id is passed through for a backend that wants to sanity-check it
 * against `existing`'s own host (the ESP backend does); the host fake
 * ignores it, same as hal_spi_bus_init(). */
hal_status_t hal_spi_bus_adopt(hal_spi_bus_t *bus, int bus_id, hal_spi_bus_t *existing);

hal_status_t hal_spi_bus_deinit(hal_spi_bus_t *bus);
bool         hal_spi_bus_is_wedged(const hal_spi_bus_t *bus);

/* Returns the backend's owner-task handle (FreeRTOS TaskHandle_t on ESP,
 * cast to void*) for stack-margin registration BY THE CALLER -- see
 * hal_uart_get_task_handle's doc comment (hal_uart.h) for the full
 * rationale, identical here. NULL if bus has no task (not initialized, or a
 * backend with no owner task, e.g. host/pico's mutex-based model today). */
void *hal_spi_get_task_handle(const hal_spi_bus_t *bus);

hal_status_t hal_spi_device_attach(hal_spi_bus_t *bus, hal_spi_device_t *dev,
                                    const hal_spi_device_cfg_t *cfg);

hal_status_t hal_spi_transfer(hal_spi_device_t *dev,
                               const uint8_t *tx, size_t tx_len,
                               uint8_t *rx, size_t rx_len,
                               uint32_t timeout_ms);

/* ESP: polling_transmit (used only by MAX31856); pico: identical to
 * hal_spi_transfer. */
hal_status_t hal_spi_transfer_polling(hal_spi_device_t *dev,
                                       const uint8_t *tx, size_t tx_len,
                                       uint8_t *rx, size_t rx_len,
                                       uint32_t timeout_ms);

/* hal_spi_transfer_async() contract, defined here BEFORE
 * CONFIG_KILNCTL_SPI_ASYNC_FLUSH is ever turned on (default OFF today,
 * verified against firmware/KilnFW/sdkconfig -- the sync path
 * (hal_spi_transfer/_polling) is what actually runs on the board, and
 * nothing in this contract may change sync behaviour). No production
 * caller reaches this function today: panel_spi_blit.c's real async flush
 * goes around the HAL entirely, straight to spi_owner_transfer_async() via
 * hal_spi_esp_get_owner() (see hal_spi_esp_owner.h) -- this entry point is
 * currently dead on real hardware, which is exactly why the contract has to
 * be nailed down now rather than inferred from a working caller later.
 *
 * 1. Return value means QUEUED, not completed. HAL_OK means the request was
 *    accepted by the backend and `cb` WILL fire later for it -- the transfer
 *    itself has not necessarily happened yet. Any other return
 *    (HAL_NOT_SUPPORTED, HAL_NOT_READY, HAL_NO_MEM, HAL_INVALID_ARG, ...)
 *    means the request was NOT queued: `cb` will never fire for this call,
 *    and the caller must treat it as a synchronous failure (same as
 *    hal_spi_transfer() returning non-OK).
 *
 * 2. Buffer ownership: see the "Buffer-copy-in" bullet in this file's
 *    top-of-file threading contract -- `tx` must stay valid and unmodified
 *    until `cb` fires (HAL_OK case only; nothing to keep alive if the call
 *    itself failed). rx is not supported by this call (write-only, matching
 *    a display flush); pass a real `tx`/`len` only.
 *
 * 3. Callback context: `cb` fires from the backend's OWN owner/worker
 *    context -- the ESP backend's spi_owner_task (a FreeRTOS task, never an
 *    ISR); the host fake only from an explicit fake_spi_pump() call in test
 *    code, standing in for that same worker context. `cb` must never be
 *    invoked from an interrupt context by any backend.
 *
 * 4. Completion cannot be delivered before hal_spi_transfer_async() itself
 *    returns: a backend must not call `cb` synchronously, inline, before
 *    handing control back to the caller on a HAL_OK return. (This is the
 *    bug hal_spi_esp.c's adapter had: it used to perform the transfer
 *    synchronously and invoke `cb` before returning -- see that file's
 *    history for the INTERFACE MISMATCH this contract closes.) A caller may
 *    therefore safely do bookkeeping after the call returns (e.g. set a
 *    "one flush in flight" flag) without racing an already-fired callback.
 *
 * 5. Ordering vs. later transfers on the same device/bus: this request
 *    joins the SAME single-writer queue as hal_spi_transfer()/
 *    _transfer_polling() (the "Single-writer per bus" contract above) --
 *    every backend must issue queued requests strictly FIFO, one at a time,
 *    never interleaved. A hal_spi_transfer()/_polling() call made by the
 *    caller AFTER this call returns HAL_OK is therefore guaranteed to be
 *    issued on the wire only after this async request's own transfer has
 *    completed, never before and never concurrently -- callers must not
 *    call a synchronous transfer that read-modifies state this async
 *    transfer is still writing before its `cb` has fired, but they need not
 *    add their own queue: the backend's ordering already covers it.
 *
 * 6. Full queue / exhausted resources: if the backend's queue or its
 *    completion-tracking pool is full, hal_spi_transfer_async() returns
 *    HAL_NO_MEM (never blocks waiting for room) and does not queue the
 *    request -- `cb` never fires. A backend MAY treat repeated exhaustion
 *    as bus-wedging (hal_spi_bus_is_wedged() latches true) the same way a
 *    completion timeout does; a single transient HAL_NO_MEM does not by
 *    itself imply the bus is wedged. HAL_NOT_SUPPORTED unless the backend
 *    enables this call at all (feature-gated off by default): the pico
 *    backend always returns HAL_NOT_SUPPORTED (no async capability, mutex +
 *    direct blocking call only), and the ESP backend returns it whenever
 *    the owning spi_owner_t was not created with async_flush enabled
 *    (CONFIG_KILNCTL_SPI_ASYNC_FLUSH). */
hal_status_t hal_spi_transfer_async(hal_spi_device_t *dev,
                                     const uint8_t *tx, size_t len,
                                     uint32_t timeout_ms,
                                     hal_spi_async_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_SPI_H */
