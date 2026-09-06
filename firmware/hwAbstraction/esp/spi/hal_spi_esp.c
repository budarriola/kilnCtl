/* hal_spi_esp.c -- ESP-IDF backend for interface/hal_spi.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against ESP-IDF
 * v6.0.2, grounded in the real KilnFW consumers of espInterfaces/
 * esp_spi_owner.c: panel_spi*.c/lvgl_port*.c (the ST7796/ILI9488 display,
 * queued + async-flush path) and MAX31856.c/thermo_owner.c (the 5-channel
 * thermocouple bus, polling path). Bus bring-up mirrors
 * main_boot_early.c:421-474's spi_bus_initialize() call, folded into
 * hal_spi_bus_init() per this interface's unified bus-init contract. Not
 * wired into any CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_esp_backends.ps1.
 *
 * No INTERFACE MISMATCH found for hal_spi.h: unlike hal_uart.h/hal_i2c.h,
 * hal_spi_bus_cfg_t already carries queue_len/task_priority/stack_depth/
 * core_id/dma_use_psram/async_flush/max_transfer_sz/dma_chan -- every
 * parameter spi_owner_init() and spi_bus_initialize() take today -- so this
 * backend needs no hardcoded stand-ins for caller-supplied task sizing.
 *
 * Preserved behaviors (interface/hal_spi.h's contract comment and
 * docs/HW_ABSTRACTION_PLAN.md "hal_spi"):
 *  - Single-writer-per-bus via one owner task and its request queue,
 *    exactly as esp_spi_owner.c: display and thermocouple transfers funnel
 *    through the same FIFO queue with no priority.
 *  - Wedge latch: a queue-send timeout or an owner-task completion timeout
 *    (SPI_OWNER_TRANSFER_TIMEOUT_MS, unchanged at 1000 ms) latches
 *    hal_spi_bus_is_wedged() true; every subsequent transfer fails fast
 *    without touching the queue until hal_spi_bus_deinit()+_init() clears
 *    it. A pool-exhaustion failure does NOT latch wedged (matches
 *    spi_owner_transfer_impl()'s "transient resource condition, not
 *    evidence the bus itself is stuck" reasoning).
 *  - Request state is never stored on the caller's stack: a heap-backed
 *    slot pool (sized queue_len+1, same off-by-one reasoning as
 *    esp_spi_owner.c's spi_owner_init() comment -- the owner task frees a
 *    queue slot before it releases the matching pool slot) holds each
 *    request's completion semaphore (StaticSemaphore_t, not heap-allocated
 *    per transfer) and result, so a caller that times out and returns
 *    leaves nothing dangling for a late completion to write into.
 *  - One request build per call: hal_spi_transfer/_polling/_async build
 *    exactly one spi_owner_esp_request_t each, matching the ~56 B
 *    single-request-per-call invariant.
 *  - DMA sentinel translation: HAL_SPI_DMA_AUTO(0) -> SPI_DMA_CH_AUTO,
 *    HAL_SPI_DMA_NONE(-1) -> SPI_DMA_DISABLED, an explicit channel N -> N
 *    (see hal_spi_bus_cfg_t.dma_chan's own doc comment for why these are
 *    deliberately NOT the same numbering as ESP-IDF's spi_common_dma_t).
 *  - ALREADY_INIT: spi_bus_initialize() returning ESP_ERR_INVALID_STATE
 *    (observed after a JTAG/OpenOCD reset, main_boot_early.c's own comment)
 *    maps to HAL_OK with an INFO log, not a failure -- but hal_spi_bus_init
 *    still creates and starts THIS bus_t's own owner task/queue/slot pool
 *    in that case, since a fresh hal_spi_bus_t has none yet even though the
 *    underlying host peripheral is already up.
 *
 * Not implemented as a queued/ISR completion (spi_device_queue_trans() +
 * get_trans_result()): hal_spi_transfer_async(), like
 * spi_owner_transfer_async(), is a synchronous transfer run on the owner
 * task's own thread with the callback fired from there before the request
 * is released -- "the caller returns early, not that transfers interleave"
 * (DISPLAY_ST7796_PLAN.md 9.6, reproduced verbatim in esp_spi_owner.c's own
 * comment). Async is gated off (HAL_NOT_SUPPORTED) unless
 * cfg->async_flush was true at hal_spi_bus_init() time, matching
 * CONFIG_KILNCTL_SPI_ASYNC_FLUSH's default-off behavior exactly.
 */
#include "hal_spi.h"

#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hal_esp_common.h"

static const char *TAG = "hal_spi_esp";

/* Unchanged from esp_spi_owner.c's SPI_OWNER_TRANSFER_TIMEOUT_MS -- see
 * that file's header comment for the full margin derivation (~1700x the
 * largest real transfer time; the realistic trigger is owner-task
 * starvation behind a flash erase/OTA write, not a genuinely dead bus). */
#define HAL_SPI_ESP_TRANSFER_TIMEOUT_MS 1000u

typedef struct {
    spi_device_handle_t device;
    const uint8_t *tx_buffer;
    size_t tx_length;
    uint8_t *rx_buffer;
    size_t rx_length;
    int cs_pin;
    int slot;
    bool shutdown;
    bool use_polling;
    bool async;
    hal_spi_async_cb_t async_cb;
    void *async_ctx;
} hal_spi_esp_request_t;

/* Module-owned result-slot pool -- see esp_spi_owner.c's spi_owner_slot_t /
 * spi_owner_slot_pool_init()/_deinit() for the full invariant this
 * reproduces (never the caller's stack; a late completion after a
 * caller-side timeout lands somewhere still valid). Reimplemented locally
 * rather than depending on KilnFW's App/drivers/owner_slot_pool.[ch] --
 * this tree does not include drivers/ code, and a self-contained
 * refcount-of-2 alloc/release pair is small enough not to need it. */
typedef struct {
    esp_err_t result;
    StaticSemaphore_t sem_storage;
    SemaphoreHandle_t sem;
} hal_spi_esp_slot_t;

typedef struct {
    spi_host_device_t host;
    QueueHandle_t request_queue;
    TaskHandle_t task_handle;
    SemaphoreHandle_t shutdown_done;
    bool initialized;
    volatile bool wedged;
    bool dma_use_psram;
    bool async_flush;
    hal_spi_esp_slot_t *slots;
    uint8_t *slot_refcount; /* 0 = free, else refcount (starts at 2) */
    size_t slot_count;
    SemaphoreHandle_t slot_lock;
} hal_spi_esp_bus_impl_t;

_Static_assert(sizeof(hal_spi_esp_bus_impl_t) <= sizeof(((hal_spi_bus_t *)0)->storage),
               "hal_spi_esp_bus_impl_t exceeds HAL_SPI_BUS_STORAGE_BYTES reservation");

typedef struct {
    spi_device_handle_t device;
    int cs_pin;
    /* Back-pointer to the owning bus's impl, captured at attach time --
     * ESP-IDF's spi_device_handle_t carries no such link, and
     * hal_spi_transfer()/_polling()'s signatures (interface/hal_spi.h,
     * NOT widened here) take only the device, so this is the only way a
     * transfer can reach the owner task/queue/slot pool that
     * hal_spi_bus_init() created. Same shape as hal_i2c_esp.c's identical
     * device->bus-queue back-pointer. */
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

/* -- slot pool ---------------------------------------------------------- */

static int slot_pool_alloc(uint8_t *refcount, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (refcount[i] == 0) {
            refcount[i] = 2; /* one for the waiter (or nobody, if async), one for the owner task */
            return (int)i;
        }
    }
    return -1;
}

/* Returns true if this release brought the slot's refcount to 0 (i.e. it is
 * now free to reuse). */
static bool slot_pool_release(uint8_t *refcount, size_t count, int idx) {
    if (idx < 0 || (size_t)idx >= count) {
        return false;
    }
    if (refcount[idx] > 0) {
        refcount[idx]--;
    }
    return refcount[idx] == 0;
}

static esp_err_t slot_pool_init(hal_spi_esp_bus_impl_t *impl, size_t count) {
    impl->slots = calloc(count, sizeof(hal_spi_esp_slot_t));
    impl->slot_refcount = calloc(count, sizeof(uint8_t));
    if (!impl->slots || !impl->slot_refcount) {
        free(impl->slots);
        free(impl->slot_refcount);
        impl->slots = NULL;
        impl->slot_refcount = NULL;
        return ESP_ERR_NO_MEM;
    }

    impl->slot_lock = xSemaphoreCreateMutex();
    if (!impl->slot_lock) {
        free(impl->slots);
        free(impl->slot_refcount);
        impl->slots = NULL;
        impl->slot_refcount = NULL;
        return ESP_ERR_NO_MEM;
    }

    for (size_t i = 0; i < count; i++) {
        impl->slots[i].sem = xSemaphoreCreateBinaryStatic(&impl->slots[i].sem_storage);
        if (!impl->slots[i].sem) {
            for (size_t j = 0; j < i; j++) {
                vSemaphoreDelete(impl->slots[j].sem);
            }
            vSemaphoreDelete(impl->slot_lock);
            impl->slot_lock = NULL;
            free(impl->slots);
            free(impl->slot_refcount);
            impl->slots = NULL;
            impl->slot_refcount = NULL;
            return ESP_ERR_NO_MEM;
        }
    }

    impl->slot_count = count;
    return ESP_OK;
}

static void slot_pool_deinit(hal_spi_esp_bus_impl_t *impl) {
    if (impl->slots) {
        for (size_t i = 0; i < impl->slot_count; i++) {
            if (impl->slots[i].sem) {
                vSemaphoreDelete(impl->slots[i].sem);
            }
        }
    }
    if (impl->slot_lock) {
        vSemaphoreDelete(impl->slot_lock);
    }
    free(impl->slots);
    free(impl->slot_refcount);
    impl->slots = NULL;
    impl->slot_refcount = NULL;
    impl->slot_lock = NULL;
    impl->slot_count = 0;
}

/* -- owner task ----------------------------------------------------------- */

static void hal_spi_esp_task(void *arg) {
    hal_spi_esp_bus_impl_t *impl = (hal_spi_esp_bus_impl_t *)arg;
    hal_spi_esp_request_t request;

    while (true) {
        if (xQueueReceive(impl->request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (request.shutdown) {
            break;
        }

        esp_err_t result = ESP_OK;
        if (request.tx_buffer && request.tx_length > 0) {
            bool bitbang_cs = request.cs_pin >= 0;
            if (bitbang_cs) {
                gpio_set_level((gpio_num_t)request.cs_pin, 0);
            }
            spi_transaction_t trans = {
                .length = request.tx_length * 8,
                .tx_buffer = request.tx_buffer,
                .rx_buffer = request.rx_buffer,
                .rxlength = request.rx_length * 8,
            };
            if (impl->dma_use_psram && !request.use_polling) {
                trans.flags |= SPI_TRANS_DMA_USE_PSRAM;
            }
            result = request.use_polling ? spi_device_polling_transmit(request.device, &trans)
                                          : spi_device_transmit(request.device, &trans);
            if (bitbang_cs) {
                gpio_set_level((gpio_num_t)request.cs_pin, 1);
            }
        } else {
            result = ESP_ERR_INVALID_ARG;
        }

        if (request.async && request.async_cb) {
            request.async_cb(request.async_ctx, hal_esp_err_to_status(result));
        }

        hal_spi_esp_slot_t *slot = &impl->slots[request.slot];
        slot->result = result;
        xSemaphoreGive(slot->sem);

        xSemaphoreTake(impl->slot_lock, portMAX_DELAY);
        bool free_now = slot_pool_release(impl->slot_refcount, impl->slot_count, request.slot);
        if (free_now) {
            xSemaphoreTake(slot->sem, 0); /* drain a Give() nobody ever collected */
            slot->result = ESP_OK;
        }
        xSemaphoreGive(impl->slot_lock);
    }

    if (impl->shutdown_done) {
        xSemaphoreGive(impl->shutdown_done);
    }
    vTaskDelete(NULL);
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
    impl->dma_use_psram = cfg->dma_use_psram;
    impl->async_flush = cfg->async_flush;

    spi_bus_config_t bus_config = {
        .mosi_io_num = cfg->mosi_pin,
        .miso_io_num = cfg->miso_pin,
        .sclk_io_num = cfg->sck_pin,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = (int)cfg->max_transfer_sz, /* 0 -> IDF default, matches header contract */
    };
    esp_err_t err = spi_bus_initialize(impl->host, &bus_config, hal_spi_esp_dma_chan(cfg->dma_chan));
    bool already_up = false;
    if (err == ESP_ERR_INVALID_STATE) {
        /* ALREADY_INIT decision (hal_spi.h): benign re-entry, not caller
         * error. Only the FIRST init's config took effect on the underlying
         * peripheral -- this bus_t still needs its own fresh owner
         * task/queue/pool below since it has none yet. */
        ESP_LOGI(TAG, "spi host %d already initialized; treating as OK", bus_id);
        already_up = true;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return hal_esp_err_to_status(err);
    }

    impl->request_queue = xQueueCreate(cfg->queue_len, sizeof(hal_spi_esp_request_t));
    if (!impl->request_queue) {
        ESP_LOGE(TAG, "failed to create request queue");
        return HAL_NO_MEM;
    }

    /* Sized queue_len + 1 -- same dequeue-before-release race margin as
     * esp_spi_owner.c's spi_owner_init() comment: the owner task frees a
     * queue slot before it releases the matching pool slot. */
    esp_err_t pool_err = slot_pool_init(impl, (size_t)cfg->queue_len + 1);
    if (pool_err != ESP_OK) {
        vQueueDelete(impl->request_queue);
        impl->request_queue = NULL;
        ESP_LOGE(TAG, "failed to create result-slot pool");
        return HAL_NO_MEM;
    }

    impl->shutdown_done = xSemaphoreCreateBinary();
    if (!impl->shutdown_done) {
        slot_pool_deinit(impl);
        vQueueDelete(impl->request_queue);
        impl->request_queue = NULL;
        return HAL_NO_MEM;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(
        hal_spi_esp_task, "hal_spi_owner", cfg->stack_depth, impl, cfg->task_priority,
        &impl->task_handle, cfg->core_id == 0 ? tskNO_AFFINITY : cfg->core_id);
    /* NOTE: cfg->core_id follows spi_owner_init()'s convention of passing
     * tskNO_AFFINITY (-1) through directly; the `== 0 ? tskNO_AFFINITY :`
     * guard above only protects a zero-initialized cfg struct (matching
     * hal_spi_bus_cfg_t's other "0 means backend default" fields) from
     * pinning to core 0 by accident -- every real caller sets core_id to
     * tskNO_AFFINITY explicitly, same as today. */
    if (task_created != pdPASS) {
        vQueueDelete(impl->request_queue);
        impl->request_queue = NULL;
        vSemaphoreDelete(impl->shutdown_done);
        impl->shutdown_done = NULL;
        slot_pool_deinit(impl);
        ESP_LOGE(TAG, "failed to create owner task");
        return HAL_NO_MEM;
    }

    impl->initialized = true;
    (void)already_up;
    return HAL_OK;
}

hal_status_t hal_spi_bus_deinit(hal_spi_bus_t *bus) {
    if (!bus) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_bus_impl_t *impl = bus_impl_of(bus);
    if (!impl->initialized) {
        return HAL_NOT_READY;
    }

    /* CALLER CONTRACT, unchanged from spi_owner_deinit(): the caller must
     * guarantee no hal_spi_transfer* call is still in flight anywhere
     * (queued, or parked in its own completion wait) before calling this --
     * see esp_spi_owner.c's spi_owner_deinit() doc comment for the full
     * use-after-free reasoning this mirrors. Unreachable today: nothing in
     * this firmware calls the equivalent deinit. */
    hal_spi_esp_request_t shutdown_request;
    memset(&shutdown_request, 0, sizeof(shutdown_request));
    shutdown_request.shutdown = true;
    if (impl->request_queue) {
        xQueueSend(impl->request_queue, &shutdown_request, portMAX_DELAY);
    }
    if (impl->shutdown_done) {
        if (xSemaphoreTake(impl->shutdown_done, pdMS_TO_TICKS(2000)) != pdTRUE) {
            ESP_LOGW(TAG, "worker did not confirm shutdown in time; deleting queue anyway");
        }
        vSemaphoreDelete(impl->shutdown_done);
    }
    if (impl->request_queue) {
        vQueueDelete(impl->request_queue);
    }
    slot_pool_deinit(impl);

    spi_host_device_t host = impl->host;
    memset(impl, 0, sizeof(*impl));
    /* spi_bus_free() is deliberately NOT called here: multiple hal_spi_bus_t
     * instances / devices can share one underlying host peripheral (the
     * display and thermo devices share one bus per
     * docs/HW_ABSTRACTION_PLAN.md's "Bus-init semantics"), and freeing the
     * host out from under a sibling hal_spi_bus_t this backend has no way
     * to see would be unsafe. This matches spi_owner_deinit(), which never
     * calls spi_bus_free() either -- only the owner task/queue/pool are
     * torn down. */
    (void)host;
    return HAL_OK;
}

bool hal_spi_bus_is_wedged(const hal_spi_bus_t *bus) {
    if (!bus) {
        return false;
    }
    const hal_spi_esp_bus_impl_t *impl = bus_impl_of_const(bus);
    return impl->wedged;
}

hal_status_t hal_spi_device_attach(hal_spi_bus_t *bus, hal_spi_device_t *dev,
                                    const hal_spi_device_cfg_t *cfg) {
    if (!bus || !dev || !cfg) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_bus_impl_t *bus_impl = bus_impl_of(bus);
    if (!bus_impl->initialized) {
        return HAL_NOT_READY;
    }
    hal_spi_esp_device_impl_t *dev_impl = device_impl_of(dev);
    memset(dev_impl, 0, sizeof(*dev_impl));

    /* CS is bit-banged around each transfer on both real buses today
     * (DISPLAY_ST7796_PLAN.md 9.4) -- spics_io_num stays -1 whenever
     * cs_pin is a real GPIO, matching MAX31856.c:558/panel_spi_bringup.c:301
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

static hal_status_t hal_spi_esp_transfer_impl(hal_spi_device_t *dev, const uint8_t *tx, size_t tx_len,
                                               uint8_t *rx, size_t rx_len, uint32_t timeout_ms,
                                               bool use_polling) {
    (void)timeout_ms; /* the owner task's own bounded wait uses the fixed
                        * HAL_SPI_ESP_TRANSFER_TIMEOUT_MS margin, matching
                        * esp_spi_owner.c exactly -- spi_owner_transfer()
                        * takes no caller-supplied timeout either. */
    if (!dev) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_device_impl_t *dev_impl = device_impl_of(dev);
    hal_spi_esp_bus_impl_t *impl = dev_impl->bus;
    if (!impl || !impl->initialized || !dev_impl->device) {
        return HAL_NOT_READY;
    }

    if (impl->wedged) {
        return HAL_WEDGED;
    }

    xSemaphoreTake(impl->slot_lock, portMAX_DELAY);
    int idx = slot_pool_alloc(impl->slot_refcount, impl->slot_count);
    xSemaphoreGive(impl->slot_lock);
    if (idx < 0) {
        ESP_LOGE(TAG, "result-slot pool exhausted -- failing this transfer, not latching wedged");
        return HAL_NO_MEM;
    }

    hal_spi_esp_request_t request;
    memset(&request, 0, sizeof(request));
    request.device = dev_impl->device;
    request.tx_buffer = tx;
    request.tx_length = tx_len;
    request.rx_buffer = rx;
    request.rx_length = rx_len;
    request.cs_pin = dev_impl->cs_pin;
    request.slot = idx;
    request.use_polling = use_polling;

    if (xQueueSend(impl->request_queue, &request, pdMS_TO_TICKS(HAL_SPI_ESP_TRANSFER_TIMEOUT_MS)) !=
        pdTRUE) {
        xSemaphoreTake(impl->slot_lock, portMAX_DELAY);
        slot_pool_release(impl->slot_refcount, impl->slot_count, idx);
        slot_pool_release(impl->slot_refcount, impl->slot_count, idx);
        xSemaphoreGive(impl->slot_lock);
        impl->wedged = true;
        ESP_LOGE(TAG,
                 "request queue did not accept a transfer within %ums -- owner task presumed "
                 "wedged, failing all transfers until hal_spi_bus_deinit()+init()",
                 (unsigned)HAL_SPI_ESP_TRANSFER_TIMEOUT_MS);
        return HAL_TIMEOUT;
    }

    hal_spi_esp_slot_t *slot = &impl->slots[idx];
    esp_err_t result = ESP_ERR_TIMEOUT;
    if (xSemaphoreTake(slot->sem, pdMS_TO_TICKS(HAL_SPI_ESP_TRANSFER_TIMEOUT_MS)) == pdTRUE) {
        result = slot->result;
        xSemaphoreTake(impl->slot_lock, portMAX_DELAY);
        bool free_now = slot_pool_release(impl->slot_refcount, impl->slot_count, idx);
        if (free_now) {
            xSemaphoreTake(slot->sem, 0);
            slot->result = ESP_OK;
        }
        xSemaphoreGive(impl->slot_lock);
        return hal_esp_err_to_status(result);
    }

    /* Request was handed to the owner task, which is still presumably busy
     * -- release only this side's half of the refcount and walk away,
     * exactly as spi_owner_transfer_impl()'s identical branch: the slot
     * stays orphaned (never reused) unless/until the owner task's own
     * release later completes it. */
    xSemaphoreTake(impl->slot_lock, portMAX_DELAY);
    slot_pool_release(impl->slot_refcount, impl->slot_count, idx);
    xSemaphoreGive(impl->slot_lock);

    impl->wedged = true;
    ESP_LOGE(TAG,
             "owner task did not complete a transfer within %ums -- presumed wedged, failing all "
             "transfers until hal_spi_bus_deinit()+init()",
             (unsigned)HAL_SPI_ESP_TRANSFER_TIMEOUT_MS);
    return HAL_TIMEOUT;
}

hal_status_t hal_spi_transfer(hal_spi_device_t *dev, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                               size_t rx_len, uint32_t timeout_ms) {
    return hal_spi_esp_transfer_impl(dev, tx, tx_len, rx, rx_len, timeout_ms, /*use_polling=*/false);
}

hal_status_t hal_spi_transfer_polling(hal_spi_device_t *dev, const uint8_t *tx, size_t tx_len,
                                       uint8_t *rx, size_t rx_len, uint32_t timeout_ms) {
    /* MAX31856 register transfers only (<=17 bytes) -- dispatches to
     * spi_device_polling_transmit() instead of spi_device_transmit() on the
     * owner task's own thread, exactly as spi_owner_transfer_polling(). */
    return hal_spi_esp_transfer_impl(dev, tx, tx_len, rx, rx_len, timeout_ms, /*use_polling=*/true);
}

hal_status_t hal_spi_transfer_async(hal_spi_device_t *dev, const uint8_t *tx, size_t len,
                                     uint32_t timeout_ms, hal_spi_async_cb_t cb, void *ctx) {
    (void)timeout_ms;
    if (!dev) {
        return HAL_INVALID_ARG;
    }
    hal_spi_esp_device_impl_t *dev_impl = device_impl_of(dev);
    hal_spi_esp_bus_impl_t *impl = dev_impl->bus;
    if (!impl || !impl->initialized || !dev_impl->device) {
        return HAL_NOT_READY;
    }

    if (!impl->async_flush) {
        /* Default OFF, matching CONFIG_KILNCTL_SPI_ASYNC_FLUSH: never
         * touches the queue -- callers must fall back to
         * hal_spi_transfer(). */
        return HAL_NOT_SUPPORTED;
    }
    if (impl->wedged) {
        return HAL_WEDGED;
    }

    xSemaphoreTake(impl->slot_lock, portMAX_DELAY);
    int idx = slot_pool_alloc(impl->slot_refcount, impl->slot_count);
    xSemaphoreGive(impl->slot_lock);
    if (idx < 0) {
        ESP_LOGE(TAG, "result-slot pool exhausted -- failing this async transfer, not latching wedged");
        return HAL_NO_MEM;
    }

    hal_spi_esp_request_t request;
    memset(&request, 0, sizeof(request));
    request.device = dev_impl->device;
    request.tx_buffer = tx;
    request.tx_length = len;
    request.cs_pin = dev_impl->cs_pin;
    request.slot = idx;
    request.async = true;
    request.async_cb = cb;
    request.async_ctx = ctx;

    if (xQueueSend(impl->request_queue, &request, pdMS_TO_TICKS(HAL_SPI_ESP_TRANSFER_TIMEOUT_MS)) !=
        pdTRUE) {
        xSemaphoreTake(impl->slot_lock, portMAX_DELAY);
        slot_pool_release(impl->slot_refcount, impl->slot_count, idx);
        slot_pool_release(impl->slot_refcount, impl->slot_count, idx);
        xSemaphoreGive(impl->slot_lock);
        impl->wedged = true;
        ESP_LOGE(TAG,
                 "async request queue did not accept a transfer within %ums -- owner task presumed "
                 "wedged, failing all transfers until hal_spi_bus_deinit()+init()",
                 (unsigned)HAL_SPI_ESP_TRANSFER_TIMEOUT_MS);
        return HAL_TIMEOUT;
    }

    /* Enqueued: this caller does not wait on the slot's completion
     * semaphore at all -- it releases its own half of the refcount right
     * now, exactly as spi_owner_transfer_async(). The owner task's own
     * release (after firing async_cb) brings the refcount the rest of the
     * way to 0. */
    xSemaphoreTake(impl->slot_lock, portMAX_DELAY);
    slot_pool_release(impl->slot_refcount, impl->slot_count, idx);
    xSemaphoreGive(impl->slot_lock);

    return HAL_OK;
}
