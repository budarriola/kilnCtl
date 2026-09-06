/* hal_uart_esp.c -- ESP-IDF backend for interface/hal_uart.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against ESP-IDF
 * v6.0.2 (C:\esp\v6.0.2\esp-idf), grounded in the real KilnFW consumers:
 * espInterfaces/uart_owner.c (safety_link.c's SAFETY link) and
 * uart_bridge*.c / uart_protocol.c (the PC link). Not wired into any
 * CMakeLists yet -- see
 * firmware/hwAbstraction/test/compile_esp_backends.ps1 for the syntax-only
 * compile check standing in for that until Phase 1a's real move lands.
 *
 * 2026-09-05 review fix: hal_uart_cfg_t (interface/hal_uart.h) now carries
 * queue_len/task_priority/stack_depth/core_id, taken from the real
 * uart_owner_init() call sites (safety_link.c:416-418 passes
 * UART_OWNER_QUEUE_LEN/TASK_PRIORITY/STACK_SIZE, themselves
 * CONFIG_KILNCTL_UART_OWNER_* Kconfig values), so each of the two live
 * owners (PC link, safety link) CAN be sized/prioritized independently by
 * its caller, and the event task can be registered for stack-margin
 * reporting via hal_uart_get_task_handle() below (CLAUDE.md "Register every
 * new task for stack-margin reporting"). 0 in any cfg field means "backend
 * default" -- HAL_UART_ESP_DEFAULT_EVENT_TASK_STACK/_PRIORITY below match
 * KILNCTL_UART_OWNER_STACK_SIZE's real Kconfig default (3072,
 * drivers/Kconfig:914-929), not an arbitrary backend pick.
 *
 * Design note on why this backend does NOT reproduce uart_owner_t's
 * request-queue-plus-worker-task architecture: that architecture exists
 * solely to serialize uart_owner_transfer()'s single blocking TX(+legacy
 * RX) call across many concurrent bridge tasks sharing one owner. The
 * Phase-0 interface deliberately replaces that one call with two primitives
 * that map straight onto the ESP-IDF driver's OWN internal serialization
 * (its TX ring buffer already guarantees FIFO ordering across callers) --
 * hal_uart_send/_send_blocking call uart_write_bytes()/uart_wait_tx_done()
 * directly, with no HAL-owned request queue or worker task in between. This
 * matches the header's own doc comment ("ESP: uart_write_bytes without a
 * wait" / "today's write + uart_wait_tx_done, unchanged") literally, and
 * avoids inventing a second layer of queuing on top of one the driver
 * already provides. The event task (FIFO_OVF/BUFFER_FULL flush, line-error
 * counting) is kept, unchanged in behavior from uart_owner_event_task().
 *
 * hal_uart_recv's read-what's-buffered-with-zero-timeout contract is the
 * exact fix behind the "528 B rx-buffer blocking bug" precedent (project
 * memory): uart_get_buffered_data_len() first, then uart_read_bytes() with
 * ticks_to_wait=0, so this call can never block waiting for more bytes than
 * are already sitting in the driver's ring.
 */
#include "hal_uart.h"

#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "hal_esp_common.h"

static const char *TAG = "hal_uart_esp";

/* Same reasoning/values as espInterfaces/uart_owner.c's identical macros:
 * generous headroom over one worst-case stuffed frame so a burst can't
 * overrun the ring before the event task/hal_uart_recv's caller drains it.
 * See that file's header comment for the full derivation -- unchanged here. */
#define HAL_UART_ESP_RX_RING_BUF_SIZE 4096
#define HAL_UART_ESP_TX_RING_BUF_SIZE 4096
#define HAL_UART_ESP_EVENT_QUEUE_LEN  16

/* 2026-09-05 review fix: hal_uart_cfg_t (interface/hal_uart.h) now carries
 * queue_len/task_priority/stack_depth/core_id. 0 in any field means
 * "backend default" -- the defaults below match the real
 * KILNCTL_UART_OWNER_* Kconfig defaults (drivers/Kconfig:914-929: stack
 * 3072), not the old hardcoded 4096 this backend used before a caller could
 * ask for anything else. */
#define HAL_UART_ESP_DEFAULT_EVENT_TASK_STACK    3072
#define HAL_UART_ESP_DEFAULT_EVENT_TASK_PRIORITY 5
#define HAL_UART_ESP_DEFAULT_EVENT_QUEUE_LEN     16

typedef struct {
    uart_port_t port;
    QueueHandle_t event_queue;
    TaskHandle_t event_task_handle;
    bool initialized;
    volatile uint32_t rx_error_count;
    volatile uint32_t tx_dropped_count;
} hal_uart_esp_impl_t;

_Static_assert(sizeof(hal_uart_esp_impl_t) <= sizeof(((hal_uart_t *)0)->storage),
               "hal_uart_esp_impl_t exceeds HAL_UART_STORAGE_BYTES reservation");

static hal_uart_esp_impl_t *impl_of(hal_uart_t *u) {
    return (hal_uart_esp_impl_t *)(void *)u->storage;
}

/* Identical policy to uart_owner_event_task(): count every line error (the
 * frame layer is self-synchronising and CRC-checked, so flushing on every
 * glitch would delete good frames queued behind a bad one -- see
 * uart_owner.c's header comment, reproduced there in full), only flush on
 * FIFO_OVF/UART_BUFFER_FULL where bytes are already lost and framing is
 * broken by definition. */
static void hal_uart_esp_event_task(void *arg) {
    hal_uart_esp_impl_t *impl = (hal_uart_esp_impl_t *)arg;
    uart_event_t event;

    while (true) {
        if (xQueueReceive(impl->event_queue, &event, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        switch (event.type) {
            case UART_DATA:
                break;
            case UART_FIFO_OVF:
                ESP_LOGW(TAG, "hw fifo overflow on uart%d", impl->port);
                impl->rx_error_count++;
                uart_flush_input(impl->port);
                xQueueReset(impl->event_queue);
                break;
            case UART_BUFFER_FULL:
                ESP_LOGW(TAG, "rx ring buffer full on uart%d", impl->port);
                impl->rx_error_count++;
                uart_flush_input(impl->port);
                xQueueReset(impl->event_queue);
                break;
            case UART_BREAK:
                ESP_LOGW(TAG, "break condition on uart%d (line idle/disconnected?)", impl->port);
                impl->rx_error_count++;
                break;
            case UART_PARITY_ERR:
                ESP_LOGW(TAG, "parity error on uart%d", impl->port);
                impl->rx_error_count++;
                break;
            case UART_FRAME_ERR:
                ESP_LOGW(TAG, "frame error on uart%d (check baud/wiring)", impl->port);
                impl->rx_error_count++;
                break;
            default:
                break;
        }
    }
}

hal_status_t hal_uart_init(hal_uart_t *u, const hal_uart_cfg_t *cfg) {
    if (!u || !cfg) {
        return HAL_INVALID_ARG;
    }

    hal_uart_esp_impl_t *impl = impl_of(u);
    memset(impl, 0, sizeof(*impl));
    impl->port = (uart_port_t)cfg->port;

    uint32_t queue_len = cfg->queue_len ? cfg->queue_len : HAL_UART_ESP_EVENT_QUEUE_LEN;
    int task_priority = cfg->task_priority ? cfg->task_priority : HAL_UART_ESP_DEFAULT_EVENT_TASK_PRIORITY;
    uint32_t stack_depth = cfg->stack_depth ? cfg->stack_depth : HAL_UART_ESP_DEFAULT_EVENT_TASK_STACK;
    BaseType_t core_id = (cfg->core_id == HAL_CORE_ANY) ? tskNO_AFFINITY : (BaseType_t)cfg->core_id;

    uart_config_t uart_config = {
        .baud_rate = (int)cfg->baud,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };

    esp_err_t err = uart_driver_install(impl->port, HAL_UART_ESP_RX_RING_BUF_SIZE,
                                         HAL_UART_ESP_TX_RING_BUF_SIZE,
                                         (int)queue_len, &impl->event_queue, 0);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        return hal_esp_err_to_status(err);
    }

    err = uart_param_config(impl->port, &uart_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
        uart_driver_delete(impl->port);
        return hal_esp_err_to_status(err);
    }

    err = uart_set_pin(impl->port, cfg->tx_io, cfg->rx_io, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
        uart_driver_delete(impl->port);
        return hal_esp_err_to_status(err);
    }

    BaseType_t task_created = xTaskCreatePinnedToCore(hal_uart_esp_event_task,
                                                       "hal_uart_evt",
                                                       stack_depth,
                                                       impl,
                                                       task_priority,
                                                       &impl->event_task_handle,
                                                       core_id);
    if (task_created != pdPASS) {
        ESP_LOGE(TAG, "failed to create hal_uart event task");
        uart_driver_delete(impl->port);
        impl->event_queue = NULL;
        return HAL_NO_MEM;
    }

    impl->initialized = true;
    return HAL_OK;
}

hal_status_t hal_uart_deinit(hal_uart_t *u) {
    if (!u) {
        return HAL_INVALID_ARG;
    }
    hal_uart_esp_impl_t *impl = impl_of(u);
    if (!impl->initialized) {
        return HAL_NOT_READY;
    }

    /* No sentinel-based handshake with the event task (unlike
     * uart_owner_event_task, this task also has no request-queue sibling to
     * synchronize teardown against) -- it only ever blocks on the driver's
     * own event queue, so it is torn down directly before that queue is
     * freed underneath it by uart_driver_delete(), same ordering
     * uart_owner_deinit() uses for the same reason. */
    if (impl->event_task_handle) {
        vTaskDelete(impl->event_task_handle);
    }

    uart_driver_delete(impl->port);

    memset(impl, 0, sizeof(*impl));
    return HAL_OK;
}

hal_status_t hal_uart_send(hal_uart_t *u, const uint8_t *data, size_t len) {
    if (!u || !data) {
        return HAL_INVALID_ARG;
    }
    hal_uart_esp_impl_t *impl = impl_of(u);
    if (!impl->initialized) {
        return HAL_NOT_READY;
    }

    /* Fixed 2026-09-05: this used to call uart_write_bytes() unconditionally
     * and only check afterward whether it queued everything -- but
     * uart_write_bytes() copies whatever WILL fit into the TX ring and
     * returns that count, so a too-big send used to already have `written`
     * bytes on the wire path by the time this returned HAL_BUSY, violating
     * the header's whole-buffer-or-HAL_BUSY / "never partially sends"
     * contract. Check the ring's free space FIRST and queue nothing at all
     * when it will not fit. */
    size_t free_bytes = 0;
    esp_err_t free_err = uart_get_tx_buffer_free_size(impl->port, &free_bytes);
    if (free_err != ESP_OK) {
        return hal_esp_err_to_status(free_err);
    }
    if (free_bytes < len) {
        impl->tx_dropped_count += (uint32_t)len;
        return HAL_BUSY;
    }

    int written = uart_write_bytes(impl->port, (const char *)data, len);
    if (written < 0) {
        return HAL_IO;
    }
    if ((size_t)written != len) {
        /* Should not happen given the free-space check above (single
         * producer per port), but preserve the contract defensively rather
         * than claim success for a partial write. */
        impl->tx_dropped_count += (uint32_t)(len - (size_t)written);
        return HAL_BUSY;
    }
    return HAL_OK;
}

hal_status_t hal_uart_send_blocking(hal_uart_t *u, const uint8_t *data, size_t len,
                                     uint32_t timeout_ms) {
    if (!u || !data) {
        return HAL_INVALID_ARG;
    }
    hal_uart_esp_impl_t *impl = impl_of(u);
    if (!impl->initialized) {
        return HAL_NOT_READY;
    }

    /* Unchanged from uart_owner_task()'s TX branch: write, then block until
     * the last byte has left the wire. This is the primitive
     * uart_protocol.c:107's frame_and_send() needs -- its ACK timer starts
     * right after this call returns. */
    int written = uart_write_bytes(impl->port, (const char *)data, len);
    if (written != (int)len) {
        impl->tx_dropped_count += (uint32_t)(len - (size_t)(written > 0 ? written : 0));
        return HAL_IO;
    }
    esp_err_t err = uart_wait_tx_done(impl->port, pdMS_TO_TICKS(timeout_ms));
    if (err != ESP_OK) {
        return hal_esp_err_to_status(err);
    }
    return HAL_OK;
}

size_t hal_uart_recv(hal_uart_t *u, uint8_t *out, size_t max) {
    if (!u || !out || max == 0) {
        return 0;
    }
    hal_uart_esp_impl_t *impl = impl_of(u);
    if (!impl->initialized) {
        return 0;
    }

    /* read-what's-buffered-with-zero-timeout: get_buffered_data_len() first,
     * then a zero-wait read -- the exact fix behind the 528 B rx-buffer
     * blocking-read incident (project memory). Never blocks waiting for
     * more than is already sitting in the driver's ring. */
    size_t available = 0;
    esp_err_t err = uart_get_buffered_data_len(impl->port, &available);
    if (err != ESP_OK || available == 0) {
        return 0;
    }

    size_t to_read = (available < max) ? available : max;
    int read = uart_read_bytes(impl->port, out, to_read, 0);
    if (read < 0) {
        return 0;
    }
    return (size_t)read;
}

size_t hal_uart_recv_blocking(hal_uart_t *u, uint8_t *buf, size_t cap, uint32_t timeout_ms) {
    if (!u || !buf || cap == 0) {
        return 0;
    }
    hal_uart_esp_impl_t *impl = impl_of(u);
    if (!impl->initialized) {
        return 0;
    }

    /* Real reader's fallback shape (uart_protocol.c:388-397): when nothing
     * is already buffered, block on the FIRST byte only, bounded by
     * timeout_ms; once at least one byte is available, drain whatever else
     * is sitting in the ring right now (bounded by cap) without blocking
     * again -- this call must still return promptly once data has arrived,
     * not wait to fill cap. */
    int first = uart_read_bytes(impl->port, buf, 1, pdMS_TO_TICKS(timeout_ms));
    if (first <= 0) {
        return 0;
    }
    size_t total = (size_t)first;
    if (total < cap) {
        size_t available = 0;
        if (uart_get_buffered_data_len(impl->port, &available) == ESP_OK && available > 0) {
            size_t want = available < (cap - total) ? available : (cap - total);
            int more = uart_read_bytes(impl->port, buf + total, want, 0);
            if (more > 0) {
                total += (size_t)more;
            }
        }
    }
    return total;
}

uint32_t hal_uart_get_rx_error_count(const hal_uart_t *u) {
    if (!u) {
        return 0;
    }
    const hal_uart_esp_impl_t *impl = (const hal_uart_esp_impl_t *)(const void *)u->storage;
    return impl->rx_error_count;
}

uint32_t hal_uart_get_tx_dropped(const hal_uart_t *u) {
    if (!u) {
        return 0;
    }
    const hal_uart_esp_impl_t *impl = (const hal_uart_esp_impl_t *)(const void *)u->storage;
    return impl->tx_dropped_count;
}

hal_status_t hal_uart_restart(hal_uart_t *u) {
    if (!u) {
        return HAL_INVALID_ARG;
    }
    hal_uart_esp_impl_t *impl = impl_of(u);
    if (!impl->initialized) {
        return HAL_NOT_READY;
    }

    /* RX-only by contract -- must never touch TX-side state (tx_dropped_count
     * is left alone). Unchanged from uart_owner_restart(). */
    esp_err_t err = uart_flush_input(impl->port);
    if (err == ESP_OK) {
        impl->rx_error_count = 0;
    }
    return hal_esp_err_to_status(err);
}

void *hal_uart_get_task_handle(const hal_uart_t *u) {
    if (!u) {
        return NULL;
    }
    const hal_uart_esp_impl_t *impl = (const hal_uart_esp_impl_t *)(const void *)u->storage;
    if (!impl->initialized) {
        return NULL;
    }
    return (void *)impl->event_task_handle;
}
