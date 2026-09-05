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
 *  - Buffer-copy-in: tx/rx buffers passed to hal_spi_transfer* are only
 *    guaranteed valid for the duration of the call. Async variants (where
 *    supported) copy request state into backend-owned storage (the ESP slot
 *    pool) rather than referencing the caller's stack -- a backend must
 *    never store request state on the caller's stack.
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

/* See hal_uart.h for why this exists: `_Alignas` is C11-only, `alignas` is
 * the C++ spelling. Kept in sync across hal_uart.h/hal_i2c.h/hal_spi.h. */
#ifdef __cplusplus
#define HAL_ALIGNAS8 alignas(8)
#else
#define HAL_ALIGNAS8 _Alignas(8)
#endif

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
    int core_id;               /* ESP: spi_owner_init's core_id (tskNO_AFFINITY
                                 * today, i.e. -1). Pico ignores. */
    bool dma_use_psram;        /* DISPLAY_ST7796_PLAN.md 9.3; ESP only. */
    bool async_flush;          /* DISPLAY_ST7796_PLAN.md 9.6; ESP only. */
} hal_spi_bus_cfg_t;

/* ALREADY_INIT decision (Phase 0, per the plan's open question at
 * "Bus-init semantics"): hal_spi_bus_init() on a bus that is already up
 * (observed on the ESP after a JTAG reset, spi_bus_initialize() returning
 * ESP_ERR_INVALID_STATE) returns HAL_OK, not HAL_BUSY, and logs at INFO.
 * Rationale: this is a benign, expected re-entry on this hardware (not a
 * caller error and not evidence the bus is unusable), so BUSY -- which
 * elsewhere in this table means "retry me" -- would be misleading; callers
 * that treat init as idempotent (main_boot_early.c's bring-up sequence)
 * should not have to special-case a second return value. Only the first
 * init's DMA configuration takes effect, so callers must still ensure the
 * first hal_spi_bus_init() on a shared bus applies the configuration that
 * needs to win (the display's, on the KilnFW thermo/display shared bus). */
hal_status_t hal_spi_bus_init(hal_spi_bus_t *bus, int bus_id,
                               const hal_spi_bus_cfg_t *cfg);
hal_status_t hal_spi_bus_deinit(hal_spi_bus_t *bus);
bool         hal_spi_bus_is_wedged(const hal_spi_bus_t *bus);

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

/* HAL_NOT_SUPPORTED unless the backend enables it (feature-gated off by
 * default -- panel_spi_blit.c is the only caller today). */
hal_status_t hal_spi_transfer_async(hal_spi_device_t *dev,
                                     const uint8_t *tx, size_t len,
                                     uint32_t timeout_ms,
                                     hal_spi_async_cb_t cb, void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_SPI_H */
