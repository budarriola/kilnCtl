/* hal_spi_esp.c -- ESP-IDF backend for interface/hal_spi.h.
 *
 * Phase 1b ("adapt") CORRECTED 2026-09-05: this used to duplicate
 * esp_spi_owner.c's whole design (queue, slot pool, wedge latch, timeout)
 * against driver/spi_master.h directly, so the firmware carried two
 * independent implementations of the same safety-relevant single-writer SPI
 * arbiter -- see docs/HW_ABSTRACTION.md Phase 2 status (commit
 * 26b16a6) and Phase 1b's own "esp owners implement hal_spi/hal_i2c/hal_uart
 * at the edge" line: the owner IS the backend body, not a second thing next
 * to it. This file is now a thin adapter: every hal_spi_* call maps directly
 * to the matching spi_owner_* call in esp_spi_owner.c, which keeps the
 * queue, the heap-backed slot pool, and the wedge latch. Nothing here holds
 * request state; the two impl structs below hold only what is needed to
 * find the right spi_owner_t/spi_device_handle_t from an opaque hal handle.
 *
 * No INTERFACE MISMATCH found for hal_spi.h: hal_spi_bus_cfg_t already
 * carries queue_len/task_priority/stack_depth/core_id/dma_use_psram/
 * async_flush/max_transfer_sz/dma_chan -- every parameter spi_owner_init()
 * and spi_bus_initialize() take -- so this adapter needs no hardcoded
 * stand-ins for caller-supplied task sizing.
 *
 * esp_spi_owner.c has no device-attach or bus-bringup entry point of its
 * own (spi_owner_init() only starts the queue/task/pool once the underlying
 * spi_host_device_t already exists) -- spi_bus_initialize()/
 * spi_bus_add_device() are plain ESP-IDF calls every real caller
 * (MAX31856.c, panel_spi_bringup.c) makes directly today, so this adapter
 * still makes them directly too. That is orchestration (bus/device
 * bring-up), not a second copy of the owner's arbitration logic.
 */
#include "hal_spi.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

#include "esp_spi_owner.h"
#include "hal_esp_common.h"
#include "hal_spi_esp_owner.h"

static const char *TAG = "hal_spi_esp";

typedef struct {
    spi_owner_t owner_storage; /* used only when this bus_t created its own owner */
    spi_owner_t *owner;        /* &owner_storage normally; an adopted external
                                 * spi_owner_t* when hal_spi_bus_adopt() was used
                                 * instead of hal_spi_bus_init() -- see
                                 * interface/hal_spi.h's doc comment on that
                                 * function. NULL when not initialized. */
    spi_host_device_t host;
    bool owner_owned; /* false when adopted: don't spi_owner_deinit() an
                        * owner this hal_spi_bus_t instance didn't create --
                        * some other driver (e.g. MAX31856.c's thermo_bus)
                        * still uses it. Mirrors hal_i2c_esp's owner_owned. */
} hal_spi_esp_bus_impl_t;

_Static_assert(sizeof(hal_spi_esp_bus_impl_t) <= sizeof(((hal_spi_bus_t *)0)->storage),
               "hal_spi_esp_bus_impl_t exceeds HAL_SPI_BUS_STORAGE_BYTES reservation");

typedef struct {
    spi_device_handle_t device;
    int cs_pin;
    /* Back-pointer to the owning bus's impl, captured at attach time --
     * hal_spi_transfer()/_polling()'s signatures (interface/hal_spi.h, NOT
     * widened here) take only the device, so this is the only way a
     * transfer can reach the spi_owner_t hal_spi_bus_init() created. */
    hal_spi_esp_bus_impl_t *bus;
} hal_spi_esp_device_impl_t;

_Static_assert(sizeof(hal_spi_esp_device_impl_t) <= sizeof(((hal_spi_device_t *)0)->storage),
               "hal_spi_esp_device_impl_t exceeds HAL_SPI_DEVICE_STORAGE_BYTES reservation");

static hal_spi_esp_bus_impl_t *bus_impl_of(hal_spi_bus_t *bus) {
    return (hal_spi_esp_bus_impl_t *)(void *)bus->storage;
}

static const hal_spi_esp_bus_impl_t *bus_impl_of_const(const hal_spi_bus_t *bus) {
    return (const hal_spi_esp_bus_impl_t *)(const void *)bus->storage;
}

static hal_spi_esp_device_impl_t *device_impl_of(hal_spi_device_t *dev) {
    return (hal_spi_esp_device_impl_t *)(void *)dev->storage;
}

/* -- bus lifecycle -------------------------------------------------------- */

static int hal_spi_esp_dma_chan(int dma_chan) {
    if (dma_chan == HAL_SPI_DMA_AUTO) {
        return SPI_DMA_CH_AUTO;
    }
    if (dma_chan == HAL_SPI_DMA_NONE) {
        return SPI_DMA_DISABLED;
    }
    return dma_chan; /* explicit channel number, passed through as-is */
}

hal_status_t hal_spi_bus_init(hal_spi_bus_t *bus, int bus_id, const hal_spi_bus_cfg_t *cfg) {
    if (!bus || !cfg) {
        return HAL_INVALID_ARG;
    }

    hal_spi_esp_bus_impl_t *impl = bus_impl_of(bus);
    memset(impl, 0, sizeof(*impl));
    impl->host = (spi_host_device_t)bus_id;

    spi_bus_config_t bus_config = {
        .mosi_io_num = cfg->mosi_pin,
        .miso_io_num = cfg->miso_pin,
        .sclk_io_num = cfg->sck_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)cfg->max_transfer_sz, /* 0 -> IDF default, matches header contract */
    };
    esp_err_t err = spi_bus_initialize(impl->host, &bus_config, hal_spi_esp_dma_chan(cfg->dma_chan));
    if (err != ESP_OK) {
        /* No ALREADY_INIT recovery here any more -- see interface/hal_spi.h's
         * doc comment on hal_spi_bus_init()/hal_spi_bus_adopt() for why: a
         * second hal_spi_bus_init() on an already-open host used to log this
         * and carry on, but that still created a SECOND independent
         * spi_owner_t/task arbitrating the same physical bus underneath
         * whichever caller got there first, violating the single-writer
         * invariant this whole design exists to protect. A caller sharing a
         * bus another caller already brought up must call hal_spi_bus_adopt()
         * on the existing hal_spi_bus_t instead of ever reaching this
         * function on that bus_id again. */
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s -- if host %d is already open, use "
                      "hal_spi_bus_adopt() instead of hal_spi_bus_init()",
                 esp_err_to_name(err), bus_id);
        memset(impl, 0, sizeof(*impl));
        return hal_esp_err_to_status(err);
    }

    esp_err_t owner_err =
        spi_owner_init(&impl->owner_storage, impl->host, (UBaseType_t)cfg->queue_len,
                        (UBaseType_t)cfg->task_priority, cfg->stack_depth,
                        cfg->core_id == HAL_CORE_ANY ? tskNO_AFFINITY : (BaseType_t)cfg->core_id,
                        cfg->dma_use_psram, cfg->async_flush);
    /* Fixed 2026-09-05: this used to test `cfg->core_id == 0`, which made ESP
     * core 0 unrepresentable -- a caller who genuinely wanted core 0 silently
     * got tskNO_AFFINITY instead. HAL_CORE_ANY (hal_status.h, -1, numerically
     * equal to FreeRTOS's tskNO_AFFINITY) is the sentinel now; 0 means core
     * 0. Every real caller sets core_id to HAL_CORE_ANY explicitly, same as
     * today's tskNO_AFFINITY usage. */
    if (owner_err != ESP_OK) {
        ESP_LOGE(TAG, "spi_owner_init failed: %s", esp_err_to_name(owner_err));
        memset(impl, 0, sizeof(*impl));
        return hal_esp_err_to_status(owner_err);
    }
    impl->owner = &impl->owner_storage;
    impl->owner_owned = true;

    return HAL_OK;
}

hal_status_t hal_spi_bus_adopt(hal_spi_bus_t *bus, int bus_id, hal_spi_bus_t *existing) {
    if (!bus || !existing) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_bus_impl_t *existing_impl = bus_impl_of(existing);
    if (!existing_impl->owner || !existing_impl->owner->initialized) {
        ESP_LOGE(TAG, "hal_spi_bus_adopt: existing bus is not initialized");
        return HAL_NOT_READY;
    }
    if (bus_id >= 0 && existing_impl->host != (spi_host_device_t)bus_id) {
        ESP_LOGE(TAG, "hal_spi_bus_adopt: host mismatch (existing=%d, requested=%d)",
                 (int)existing_impl->host, bus_id);
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_bus_impl_t *impl = bus_impl_of(bus);
    memset(impl, 0, sizeof(*impl));
    impl->owner = existing_impl->owner;
    impl->host = existing_impl->host;
    impl->owner_owned = false;
    return HAL_OK;
}

hal_status_t hal_spi_bus_deinit(hal_spi_bus_t *bus) {
    if (!bus) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_bus_impl_t *impl = bus_impl_of(bus);
    if (!impl->owner || !impl->owner->initialized) {
        return HAL_NOT_READY;
    }

    /* Adopted bus (hal_spi_bus_adopt(), owner_owned == false): this bus_t
     * did not create the owner or the host peripheral, so it must not tear
     * either down -- some other driver (e.g. MAX31856.c's thermo_bus) still
     * uses them. Only this instance's own local storage is cleared. */
    esp_err_t err = ESP_OK;
    if (impl->owner_owned) {
        /* CALLER CONTRACT, unchanged from spi_owner_deinit(): the caller must
         * guarantee no hal_spi_transfer* call is still in flight anywhere
         * before calling this -- see esp_spi_owner.h's spi_owner_deinit()
         * doc comment for the full use-after-free reasoning. Unreachable
         * today: nothing in this firmware calls the equivalent deinit. */
        err = spi_owner_deinit(impl->owner);
    }

    /* spi_bus_free() is deliberately NOT called here: multiple hal_spi_bus_t
     * instances/devices can share one underlying host peripheral (the
     * display and thermo devices share one bus per
     * docs/HW_ABSTRACTION.md's "Bus-init semantics"), and freeing the
     * host out from under a sibling hal_spi_bus_t this backend has no way to
     * see would be unsafe. This matches spi_owner_deinit()/MAX31856_bus_deinit(),
     * neither of which calls spi_bus_free() unconditionally either. */
    memset(impl, 0, sizeof(*impl));
    return hal_esp_err_to_status(err);
}

bool hal_spi_bus_is_wedged(const hal_spi_bus_t *bus) {
    if (!bus) {
        return false;
    }
    const hal_spi_esp_bus_impl_t *impl = bus_impl_of_const(bus);
    if (!impl->owner) {
        return false;
    }
    return spi_owner_is_wedged(impl->owner);
}

void *hal_spi_get_task_handle(const hal_spi_bus_t *bus) {
    if (!bus) {
        return NULL;
    }
    const hal_spi_esp_bus_impl_t *impl = bus_impl_of_const(bus);
    if (!impl->owner || !impl->owner->initialized) {
        return NULL;
    }
    return (void *)impl->owner->task_handle;
}

/* See hal_spi_esp_owner.h -- ESP-only bridge for the display driver, which
 * still takes a raw spi_owner_t* and shares this same physical bus/host with
 * the (now HAL-migrated) MAX31856 channels. */
spi_owner_t *hal_spi_esp_get_owner(hal_spi_bus_t *bus) {
    if (!bus) {
        return NULL;
    }
    hal_spi_esp_bus_impl_t *impl = bus_impl_of(bus);
    if (!impl->owner || !impl->owner->initialized) {
        return NULL;
    }
    return impl->owner;
}

hal_status_t hal_spi_device_attach(hal_spi_bus_t *bus, hal_spi_device_t *dev,
                                    const hal_spi_device_cfg_t *cfg) {
    if (!bus || !dev || !cfg) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_bus_impl_t *bus_impl = bus_impl_of(bus);
    if (!bus_impl->owner || !bus_impl->owner->initialized) {
        return HAL_NOT_READY;
    }
    hal_spi_esp_device_impl_t *dev_impl = device_impl_of(dev);
    memset(dev_impl, 0, sizeof(*dev_impl));

    /* CS is bit-banged around each transfer on both real buses today
     * (DISPLAY_ST7796_PLAN.md 9.4) -- spics_io_num stays -1 whenever cs_pin
     * is a real GPIO, matching MAX31856.c:558/panel_spi_bringup.c:301
     * exactly. hw_cs (a real spics_io_num) is supported for a caller that
     * opts into hardware CS instead. */
    spi_device_interface_config_t dev_config = {
        .clock_speed_hz = (int)cfg->clock_hz,
        .mode = (uint8_t)cfg->mode,
        .spics_io_num = cfg->hw_cs,
        .queue_size = (int)cfg->queue_size,
        .input_delay_ns = (int)cfg->input_delay_ns,
    };
    esp_err_t err = spi_bus_add_device(bus_impl->host, &dev_config, &dev_impl->device);
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }
    dev_impl->cs_pin = cfg->cs_pin;
    dev_impl->bus = bus_impl;
    return HAL_OK;
}

/* -- transfers -------------------------------------------------------------- */

hal_status_t hal_spi_transfer(hal_spi_device_t *dev, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                               size_t rx_len, uint32_t timeout_ms) {
    (void)timeout_ms; /* spi_owner_transfer() takes no caller-supplied timeout
                        * either -- the owner's own fixed
                        * SPI_OWNER_TRANSFER_TIMEOUT_MS margin applies. */
    if (!dev) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_device_impl_t *dev_impl = device_impl_of(dev);
    if (!dev_impl->bus || !dev_impl->device) {
        return HAL_NOT_READY;
    }
    esp_err_t err = spi_owner_transfer(dev_impl->bus->owner, dev_impl->device, tx, tx_len, rx,
                                        rx_len, dev_impl->cs_pin);
    return hal_esp_err_to_status(err);
}

hal_status_t hal_spi_transfer_polling(hal_spi_device_t *dev, const uint8_t *tx, size_t tx_len,
                                       uint8_t *rx, size_t rx_len, uint32_t timeout_ms) {
    /* MAX31856 register transfers only (<=17 bytes) -- dispatches to
     * spi_owner_transfer_polling(), which issues spi_device_polling_transmit()
     * instead of spi_device_transmit() on the owner task's own thread. */
    (void)timeout_ms;
    if (!dev) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_device_impl_t *dev_impl = device_impl_of(dev);
    if (!dev_impl->bus || !dev_impl->device) {
        return HAL_NOT_READY;
    }
    esp_err_t err = spi_owner_transfer_polling(dev_impl->bus->owner, dev_impl->device, tx, tx_len,
                                                rx, rx_len, dev_impl->cs_pin);
    return hal_esp_err_to_status(err);
}

/* Per-call bridge between spi_owner_transfer_async()'s callback shape
 * (`cb(ctx, esp_err_t)`) and hal_spi_async_cb_t's (`cb(ctx, hal_status_t)`)
 * -- ESP-IDF error codes must not leak through the portable interface.
 *
 * FIXED 2026-09-06 (interface/hal_spi.h's hal_spi_transfer_async() contract,
 * bullets 1 and 4): this used to sidestep the translation problem by NOT
 * queuing at all -- it called spi_owner_transfer() synchronously and invoked
 * `cb` itself before returning, which broke the documented contract two
 * ways: (a) the transfer completed (or failed) before the caller ever saw
 * this function return, so a caller relying on "queued now, notified later"
 * (e.g. clearing an in-flight flag only in `cb`) could observe `cb` having
 * already run by the time it checked that flag, and (b) this function's own
 * return value was unconditionally HAL_OK even when the synchronous
 * transfer had just failed, so a caller that passed cb=NULL had no way to
 * observe a failed transfer at all. Both are gone now: this function does
 * not touch the bus itself, only enqueues through spi_owner_transfer_async()
 * exactly like a real async backend must, and returns whatever status that
 * enqueue attempt produced -- `cb` fires later, from spi_owner_task()'s own
 * thread, never here.
 *
 * The one caller-visible allocation this requires is a small per-call
 * heap block carrying `cb`/`ctx` across to the owner task's thread (freed
 * by the trampoline right before it returns) -- spi_owner_request_t has
 * exactly one void* async_ctx slot and no room for a second function
 * pointer plus a portable-status translation without either widening that
 * struct (extending esp_spi_owner.c's already-reviewed, 56 B-per-request
 * shape for a caller this adapter currently has zero production traffic
 * from -- see hal_spi.h's contract comment: panel_spi_blit.c bypasses this
 * adapter entirely) or doing exactly this. Freed on every path: a failed
 * enqueue frees it immediately below (before returning), a successful one
 * lets the trampoline free it once `cb` has run. */
typedef struct {
    hal_spi_async_cb_t cb;
    void *ctx;
} hal_spi_esp_async_trampoline_t;

static void hal_spi_esp_async_trampoline(void *ctx, esp_err_t result) {
    hal_spi_esp_async_trampoline_t *tramp = (hal_spi_esp_async_trampoline_t *)ctx;
    hal_spi_async_cb_t user_cb = tramp->cb;
    void *user_ctx = tramp->ctx;
    free(tramp);
    if (user_cb) {
        user_cb(user_ctx, hal_esp_err_to_status(result));
    }
}

hal_status_t hal_spi_transfer_async(hal_spi_device_t *dev, const uint8_t *tx, size_t len,
                                     uint32_t timeout_ms, hal_spi_async_cb_t cb, void *ctx) {
    (void)timeout_ms; /* spi_owner_transfer_async() takes no caller-supplied
                        * timeout either -- same fixed-margin reasoning as
                        * hal_spi_transfer() above. */
    if (!dev) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_device_impl_t *dev_impl = device_impl_of(dev);
    if (!dev_impl->bus || !dev_impl->device) {
        return HAL_NOT_READY;
    }

    hal_spi_esp_async_trampoline_t *tramp = malloc(sizeof(*tramp));
    if (!tramp) {
        /* Contract bullet 6: full queue / exhausted resources -> HAL_NO_MEM,
         * nothing queued, cb never fires. */
        return HAL_NO_MEM;
    }
    tramp->cb = cb;
    tramp->ctx = ctx;

    esp_err_t err = spi_owner_transfer_async(dev_impl->bus->owner, dev_impl->device, tx, len,
                                              dev_impl->cs_pin, hal_spi_esp_async_trampoline, tramp);
    if (err != ESP_OK) {
        /* Not queued -- see spi_owner_transfer_async()'s own contract:
         * ESP_ERR_NOT_SUPPORTED (async_flush off, i.e.
         * CONFIG_KILNCTL_SPI_ASYNC_FLUSH default n -> HAL_NOT_SUPPORTED),
         * ESP_ERR_INVALID_STATE (bus latched wedged), ESP_ERR_NO_MEM (slot
         * pool exhausted), or ESP_ERR_TIMEOUT (the owner's request queue
         * itself did not accept the request within SPI_OWNER_TRANSFER_
         * TIMEOUT_MS -- i.e. the queue was full and stayed full). None of
         * these hand the request to the owner task, so `tramp` must be
         * freed here -- the trampoline will never run for it.
         *
         * hal_spi.h bullet 6's contract is explicit: "full queue / exhausted
         * resources" -> HAL_NO_MEM, never HAL_TIMEOUT (that is reserved for
         * a queued-but-not-yet-completed transfer timing out, which this
         * call never waits for at all). The generic hal_esp_err_to_status()
         * mapper does not know that distinction and maps ESP_ERR_TIMEOUT to
         * HAL_TIMEOUT unconditionally, so it must be special-cased here
         * rather than reused as-is for this one error. */
        if (err == ESP_ERR_TIMEOUT) {
            free(tramp);
            return HAL_NO_MEM;
        }
        free(tramp);
        return hal_esp_err_to_status(err);
    }
    /* Enqueued successfully -- contract bullet 1/4: return now, `cb` fires
     * later from spi_owner_task()'s own thread via the trampoline above,
     * never from this call. */
    return HAL_OK;
}
