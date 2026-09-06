/* hal_i2c_esp.c -- ESP-IDF backend for interface/hal_i2c.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against ESP-IDF
 * v6.0.2, grounded in the real KilnFW consumers of espInterfaces/i2c_owner.c
 * -- SX1509.c (IO expander), FT6336U.c (capacitive touch), NS2009.c (legacy
 * resistive touch) -- and i2c_scan.c's/SX1509.c's direct i2c_master_probe()
 * calls. Not wired into any CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_esp_backends.ps1.
 *
 * 2026-09-05 review fix: hal_i2c_bus_cfg_t (interface/hal_i2c.h) now carries
 * queue_len/task_priority/stack_depth/core_id, mirroring hal_spi_bus_cfg_t,
 * so each attached device's owner task CAN be sized independently by its
 * caller (SX1509.c:414's real call passes (8, 5, 4096, tskNO_AFFINITY);
 * FT6336U.c:113/NS2009.c:83 both pass (8, 5, 3072, tskNO_AFFINITY)) and
 * registered for stack-margin reporting via hal_i2c_get_task_handle() below.
 * 0 in any cfg field means "backend default" -- HAL_I2C_ESP_DEFAULT_*
 * below, matching SX1509's larger sizing so a caller that leaves a field at
 * 0 is never under-provisioned relative to today.
 *
 * Also fixed 2026-09-05: the ALREADY_INIT branch (bus_id shared by two
 * devices, e.g. SX1509 + touch on one i2c_port_num_t) used to return HAL_OK
 * with impl->bus left NULL and no queue/task created for THIS hal_i2c_bus_t
 * instance, so every later attach/transfer/probe on it returned
 * HAL_NOT_READY forever. Now recovers the real handle via
 * i2c_master_get_bus_handle() and falls through to create this bus_t's own
 * queue/task, exactly as hal_spi_bus_init() already did for its identical
 * ALREADY_INIT case.
 *
 * Two hard-won behaviors from i2c_owner.c this backend must preserve
 * (interface/hal_i2c.h's own contract comment, and
 * docs/HW_ABSTRACTION_PLAN.md "hal_i2c"):
 *  1. A STATIC (not heap) per-call completion semaphore in
 *     hal_i2c_transfer() -- the 2026-08-20 SRAM-starvation fix. Do not
 *     regress to xSemaphoreCreateBinary() per call.
 *  2. Worker-enforced timeout_ms with the CALLER waiting unbounded on the
 *     completion semaphore -- use-after-free avoidance: the caller must
 *     never give up on a transfer while the owner task still holds a
 *     pointer into this call's stack frame (the request struct and
 *     done_sem_storage below).
 * Also preserved: the bus-reset-then-retry-once recovery on a failed
 * transfer (skipping ESP_ERR_INVALID_ARG, a caller bug the reset can't
 * fix), exactly as i2c_owner_task()'s recovery branch.
 *
 * hal_i2c_probe() is a direct, unqueued i2c_master_probe() call on the bus
 * handle -- matching every real caller (SX1509.c:532,585,625; i2c_scan.c)
 * exactly: none of them route a probe through the owner queue, since a
 * probe is not a `i2c_master_dev_handle_t`-scoped transfer.
 */
#include "hal_i2c.h"

#include <string.h>

#include "driver/i2c_master.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "hal_esp_common.h"

static const char *TAG = "hal_i2c_esp";

/* Backend defaults used when hal_i2c_bus_cfg_t's corresponding field is 0
 * ("backend default" per hal_i2c.h) -- matches SX1509.c:414's call (the
 * larger of the two live stack_depth values in the tree today, so a caller
 * that leaves this at 0 is never under-provisioned relative to today). A
 * caller that needs FT6336U/NS2009's smaller 3072 sizing now passes it
 * explicitly via cfg->stack_depth instead of being stuck with a hardcoded
 * value -- this is the fix for the widened interface's whole point. */
#define HAL_I2C_ESP_DEFAULT_TASK_STACK    4096
#define HAL_I2C_ESP_DEFAULT_TASK_PRIORITY 5
#define HAL_I2C_ESP_DEFAULT_QUEUE_LEN     8

typedef struct {
    i2c_master_dev_handle_t device;
    const uint8_t *tx_buffer;
    size_t tx_length;
    uint8_t *rx_buffer;
    size_t rx_length;
    uint32_t timeout_ms;
    SemaphoreHandle_t done_sem;
    esp_err_t *result_out;
    bool shutdown;
} hal_i2c_esp_request_t;

typedef struct {
    i2c_master_bus_handle_t bus;
    QueueHandle_t request_queue;
    TaskHandle_t task_handle;
    SemaphoreHandle_t shutdown_done;
    bool initialized;
} hal_i2c_esp_bus_impl_t;

_Static_assert(sizeof(hal_i2c_esp_bus_impl_t) <= sizeof(((hal_i2c_bus_t *)0)->storage),
               "hal_i2c_esp_bus_impl_t exceeds HAL_I2C_BUS_STORAGE_BYTES reservation");

typedef struct {
    i2c_master_dev_handle_t device;
    /* Back-pointer to the owning bus's request queue, captured at attach
     * time -- ESP-IDF's i2c_master_dev_handle_t carries no such link, and
     * hal_i2c_transfer()'s signature (interface/hal_i2c.h) takes only the
     * device, not the bus, so this is the only way a transfer can reach the
     * SAME owner task/queue that device_attach() used, preserving the
     * single-owner-task-per-bus serialization and bus-reset-retry recovery
     * i2c_owner_transfer() provides today. */
    QueueHandle_t request_queue;
} hal_i2c_esp_device_impl_t;

_Static_assert(sizeof(hal_i2c_esp_device_impl_t) <= sizeof(((hal_i2c_device_t *)0)->storage),
               "hal_i2c_esp_device_impl_t exceeds HAL_I2C_DEVICE_STORAGE_BYTES reservation");

static hal_i2c_esp_bus_impl_t *bus_impl_of(hal_i2c_bus_t *bus) {
    return (hal_i2c_esp_bus_impl_t *)(void *)bus->storage;
}

static hal_i2c_esp_device_impl_t *device_impl_of(hal_i2c_device_t *dev) {
    return (hal_i2c_esp_device_impl_t *)(void *)dev->storage;
}

/* One raw attempt at whatever the request describes -- pulled out of the
 * task loop so the retry-after-reset path can call it twice, exactly as
 * i2c_owner_do_transfer(). */
static esp_err_t hal_i2c_esp_do_transfer(const hal_i2c_esp_request_t *request) {
    if (request->tx_buffer && request->tx_length > 0 && request->rx_buffer && request->rx_length > 0) {
        return i2c_master_transmit_receive(request->device, request->tx_buffer, request->tx_length,
                                            request->rx_buffer, request->rx_length, request->timeout_ms);
    }
    if (request->tx_buffer && request->tx_length > 0) {
        return i2c_master_transmit(request->device, request->tx_buffer, request->tx_length,
                                    request->timeout_ms);
    }
    if (request->rx_buffer && request->rx_length > 0) {
        return i2c_master_receive(request->device, request->rx_buffer, request->rx_length,
                                   request->timeout_ms);
    }
    return ESP_ERR_INVALID_ARG;
}

static void hal_i2c_esp_task(void *arg) {
    hal_i2c_esp_bus_impl_t *impl = (hal_i2c_esp_bus_impl_t *)arg;
    hal_i2c_esp_request_t request;
    uint32_t consecutive_failures = 0;

    while (true) {
        if (xQueueReceive(impl->request_queue, &request, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (request.shutdown) {
            break;
        }

        esp_err_t result = hal_i2c_esp_do_transfer(&request);

        /* Bus-reset-then-retry-once recovery, unchanged from
         * i2c_owner_task(): a wedged bus (SDA held low mid-transfer, a
         * marginal wire) otherwise fails every subsequent transfer forever,
         * even to a healthy device, until something resets it. */
        if (result != ESP_OK && result != ESP_ERR_INVALID_ARG) {
            esp_err_t reset_err = i2c_master_bus_reset(impl->bus);
            if (reset_err != ESP_OK) {
                consecutive_failures++;
                ESP_LOGE(TAG, "transfer failed (%s); bus reset ALSO failed: %s (%lu consecutive)",
                         esp_err_to_name(result), esp_err_to_name(reset_err),
                         (unsigned long)consecutive_failures);
            } else {
                esp_err_t retry_result = hal_i2c_esp_do_transfer(&request);
                if (retry_result == ESP_OK) {
                    ESP_LOGW(TAG, "transfer failed (%s); bus reset recovered it on retry",
                             esp_err_to_name(result));
                    result = retry_result;
                    consecutive_failures = 0;
                } else {
                    consecutive_failures++;
                    ESP_LOGE(TAG,
                             "transfer failed (%s); bus reset done but retry ALSO failed (%s) "
                             "(%lu consecutive)",
                             esp_err_to_name(result), esp_err_to_name(retry_result),
                             (unsigned long)consecutive_failures);
                    result = retry_result;
                }
            }
        } else {
            consecutive_failures = 0;
        }

        if (request.result_out) {
            *request.result_out = result;
        }
        if (request.done_sem) {
            xSemaphoreGive(request.done_sem);
        }
    }

    if (impl->shutdown_done) {
        xSemaphoreGive(impl->shutdown_done);
    }
    vTaskDelete(NULL);
}

hal_status_t hal_i2c_bus_init(hal_i2c_bus_t *bus, int bus_id, const hal_i2c_bus_cfg_t *cfg) {
    if (!bus || !cfg) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_bus_impl_t *impl = bus_impl_of(bus);
    memset(impl, 0, sizeof(*impl));

    uint32_t queue_len = cfg->queue_len ? cfg->queue_len : HAL_I2C_ESP_DEFAULT_QUEUE_LEN;
    int task_priority = cfg->task_priority ? cfg->task_priority : HAL_I2C_ESP_DEFAULT_TASK_PRIORITY;
    uint32_t stack_depth = cfg->stack_depth ? cfg->stack_depth : HAL_I2C_ESP_DEFAULT_TASK_STACK;
    BaseType_t core_id = (cfg->core_id == HAL_CORE_ANY) ? tskNO_AFFINITY : (BaseType_t)cfg->core_id;

    i2c_master_bus_config_t bus_config = {
        .i2c_port = (i2c_port_num_t)bus_id,
        .sda_io_num = (gpio_num_t)cfg->sda_pin,
        .scl_io_num = (gpio_num_t)cfg->scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&bus_config, &impl->bus);
    bool already_up = false;
    if (err == ESP_ERR_INVALID_STATE) {
        /* ALREADY_INIT per hal_i2c.h/hal_spi.h's shared decision: benign
         * re-entry (e.g. a JTAG-reset re-bringup), not caller error. BUT
         * this hal_i2c_bus_t instance still has no bus handle, request
         * queue or owner task yet -- returning HAL_OK here without
         * recovering them (the pre-fix bug) left impl->bus/request_queue
         * NULL, so every later attach/transfer/probe on THIS bus_t
         * returned HAL_NOT_READY forever, even though the underlying port
         * was fine (hit whenever two devices, e.g. SX1509 + touch, share
         * one i2c_port_num_t). Recover the existing handle via
         * i2c_master_get_bus_handle() and fall through to create this
         * bus_t's own queue/task, exactly as hal_spi_bus_init() already
         * does for the identical ALREADY_INIT case. */
        err = i2c_master_get_bus_handle((i2c_port_num_t)bus_id, &impl->bus);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "i2c bus %d already initialized but i2c_master_get_bus_handle failed: %s",
                     bus_id, esp_err_to_name(err));
            return hal_esp_err_to_status(err);
        }
        ESP_LOGI(TAG, "i2c bus %d already initialized; recovered handle, treating as OK", bus_id);
        already_up = true;
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2c_new_master_bus failed: %s", esp_err_to_name(err));
        return hal_esp_err_to_status(err);
    }

    impl->request_queue = xQueueCreate(queue_len, sizeof(hal_i2c_esp_request_t));
    if (!impl->request_queue) {
        if (!already_up) {
            i2c_del_master_bus(impl->bus);
        }
        impl->bus = NULL;
        return HAL_NO_MEM;
    }

    impl->shutdown_done = xSemaphoreCreateBinary();
    if (!impl->shutdown_done) {
        vQueueDelete(impl->request_queue);
        impl->request_queue = NULL;
        if (!already_up) {
            i2c_del_master_bus(impl->bus);
        }
        impl->bus = NULL;
        return HAL_NO_MEM;
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(hal_i2c_esp_task, "hal_i2c_owner",
                                                       stack_depth, impl,
                                                       task_priority, &impl->task_handle,
                                                       core_id);
    if (task_created != pdPASS) {
        vQueueDelete(impl->request_queue);
        impl->request_queue = NULL;
        vSemaphoreDelete(impl->shutdown_done);
        impl->shutdown_done = NULL;
        if (!already_up) {
            i2c_del_master_bus(impl->bus);
        }
        impl->bus = NULL;
        return HAL_NO_MEM;
    }

    impl->initialized = true;
    return HAL_OK;
}

void *hal_i2c_get_task_handle(const hal_i2c_bus_t *bus) {
    if (!bus) {
        return NULL;
    }
    const hal_i2c_esp_bus_impl_t *impl = (const hal_i2c_esp_bus_impl_t *)(const void *)bus->storage;
    if (!impl->initialized) {
        return NULL;
    }
    return (void *)impl->task_handle;
}

hal_status_t hal_i2c_bus_deinit(hal_i2c_bus_t *bus) {
    if (!bus) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_bus_impl_t *impl = bus_impl_of(bus);
    if (!impl->initialized) {
        return HAL_NOT_READY;
    }

    hal_i2c_esp_request_t shutdown_request;
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
    if (impl->bus) {
        i2c_del_master_bus(impl->bus);
    }

    memset(impl, 0, sizeof(*impl));
    return HAL_OK;
}

hal_status_t hal_i2c_device_attach(hal_i2c_bus_t *bus, hal_i2c_device_t *dev, uint8_t addr,
                                    uint32_t clock_hz) {
    if (!bus || !dev) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_bus_impl_t *bus_impl = bus_impl_of(bus);
    if (!bus_impl->initialized) {
        return HAL_NOT_READY;
    }
    hal_i2c_esp_device_impl_t *dev_impl = device_impl_of(dev);
    memset(dev_impl, 0, sizeof(*dev_impl));

    i2c_device_config_t dev_config = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = clock_hz,
    };
    esp_err_t err = i2c_master_bus_add_device(bus_impl->bus, &dev_config, &dev_impl->device);
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }
    dev_impl->request_queue = bus_impl->request_queue;
    return HAL_OK;
}

hal_status_t hal_i2c_transfer(hal_i2c_device_t *dev, const uint8_t *tx, size_t tx_len, uint8_t *rx,
                               size_t rx_len, uint32_t timeout_ms) {
    if (!dev) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_device_impl_t *dev_impl = device_impl_of(dev);
    if (!dev_impl->device || !dev_impl->request_queue) {
        return HAL_NOT_READY;
    }

    /* Static, stack-resident semaphore -- preserves i2c_owner_transfer()'s
     * 2026-08-20 SRAM-starvation fix (hal_i2c.h's own contract comment):
     * xSemaphoreCreateBinary() per call heap-allocates from internal SRAM on
     * the hottest transfer path on the bus (NS2009 touch polling). Do not
     * regress to that. */
    StaticSemaphore_t done_sem_storage;
    SemaphoreHandle_t done_sem = xSemaphoreCreateBinaryStatic(&done_sem_storage);
    if (!done_sem) {
        return HAL_NO_MEM;
    }

    esp_err_t result = ESP_FAIL;
    hal_i2c_esp_request_t request;
    memset(&request, 0, sizeof(request));
    request.device = dev_impl->device;
    request.tx_buffer = tx;
    request.tx_length = tx_len;
    request.rx_buffer = rx;
    request.rx_length = rx_len;
    request.timeout_ms = timeout_ms;
    request.done_sem = done_sem;
    request.result_out = &result;

    if (xQueueSend(dev_impl->request_queue, &request, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(done_sem);
        return HAL_TIMEOUT;
    }

    /* Worker-enforced timeout_ms with the CALLER waiting unbounded --
     * use-after-free avoidance (hal_i2c.h's own contract comment): a queue
     * backlog ahead of this request must not let this call give up and
     * unwind request/done_sem_storage's stack frame while
     * hal_i2c_esp_task() still holds pointers into them. */
    if (xSemaphoreTake(done_sem, portMAX_DELAY) != pdTRUE) {
        vSemaphoreDelete(done_sem);
        return HAL_TIMEOUT;
    }

    vSemaphoreDelete(done_sem);
    return hal_esp_err_to_status(result);
}

hal_status_t hal_i2c_probe(hal_i2c_bus_t *bus, uint8_t addr, uint32_t timeout_ms) {
    if (!bus) {
        return HAL_INVALID_ARG;
    }
    hal_i2c_esp_bus_impl_t *impl = bus_impl_of(bus);
    if (!impl->initialized) {
        return HAL_NOT_READY;
    }
    esp_err_t err = i2c_master_probe(impl->bus, addr, timeout_ms);
    return hal_esp_err_to_status(err);
}
