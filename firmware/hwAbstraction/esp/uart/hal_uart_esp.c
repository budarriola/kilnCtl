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

/* Floor for hal_uart_send_blocking()'s final uart_wait_tx_done() wait -- see
 * that function's 2026-09-05 review-fix comment. 2 ticks (portTICK_PERIOD_MS
 * is 1ms on this project's FreeRTOSConfig) is comfortably more than the
 * microseconds needed to drain the last few bytes of one frame at 230400+
 * baud, and small enough to never matter next to any real ack_timeout_ms
 * (200ms default, UART_PROTO_DEFAULT_ACK_TIMEOUT_MS). */
#define HAL_UART_ESP_MIN_WAIT_TX_DONE_TICKS 2

/* 2026-09-05 review fix: hal_uart_cfg_t (interface/hal_uart.h) now carries
 * queue_len/task_priority/stack_depth/core_id. 0 in any field means
 * "backend default" -- the defaults below match the real
 * KILNCTL_UART_OWNER_* Kconfig defaults (drivers/Kconfig:914-929: stack
 * 3072), not the old hardcoded 4096 this backend used before a caller could
 * ask for anything else. */
#define HAL_UART_ESP_DEFAULT_EVENT_TASK_STACK    3072
#define HAL_UART_ESP_DEFAULT_EVENT_TASK_PRIORITY 5
/* Event queue length default lives in HAL_UART_ESP_EVENT_QUEUE_LEN above --
 * a second, duplicate HAL_UART_ESP_DEFAULT_EVENT_QUEUE_LEN (also 16, unused
 * anywhere) used to live here; removed rather than kept as dead code. */

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

hal_status_t hal_uart_attach(hal_uart_t *u, int port) {
    if (!u) {
        return HAL_INVALID_ARG;
    }
    if (port < 0 || port >= (int)UART_NUM_MAX) {
        return HAL_INVALID_ARG;
    }
    /* Review fix (2026-09-05): this used to accept any in-range port number
     * unconditionally, so attaching before the real owner had actually
     * called uart_driver_install() on it (a caller-ordering bug, not a
     * hardware fault) would silently produce a "live" handle whose first
     * real send then failed deep inside uart_write_bytes()/uart_get_tx_
     * buffer_free_size() with a raw ESP-IDF error instead of a clear
     * HAL_NOT_READY right here at attach time. uart_is_driver_installed() is
     * the same check ESP-IDF's own driver functions use internally to refuse
     * an uninstalled port. */
    if (!uart_is_driver_installed((uart_port_t)port)) {
        return HAL_NOT_READY;
    }

    /* Transitional Phase-1b path (see interface/hal_uart.h's doc comment):
     * the driver for `port` is already installed/configured by a uart_owner_t
     * that this handle does not own -- no uart_driver_install/param_config/
     * set_pin here, and deliberately no event task of its own (the owner's
     * own event task, identical in behavior to hal_uart_esp_event_task()
     * above, already watches this port's FIFO_OVF/BUFFER_FULL/line errors).
     * rx_error_count/tx_dropped_count on THIS handle therefore only count
     * activity observed through calls made via this handle (today: sends
     * from uart_protocol.c's frame_and_send()), not the owner's own RX-side
     * counters -- the two are intentionally separate views, not a shared
     * counter. hal_uart_deinit must never be called on a handle produced by
     * this function: it would call uart_driver_delete() out from under the
     * owner that still needs the port. */
    hal_uart_esp_impl_t *impl = impl_of(u);
    memset(impl, 0, sizeof(*impl));
    impl->port = (uart_port_t)port;
    impl->event_queue = NULL;
    impl->event_task_handle = NULL;
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

    /* Fixed 2026-09-05: uart_write_bytes() blocks internally, with NO
     * caller-supplied bound, once the TX ring does not have room for the
     * whole buffer -- it waits on the driver's own internal semaphore for
     * space to free up. That made this function ignore `timeout_ms`
     * entirely and hang forever if the ring stayed full (e.g. the other end
     * stopped reading). Poll the ring's free space first -- same
     * uart_get_tx_buffer_free_size() check hal_uart_send() uses -- in a
     * vTaskDelay(1) loop bounded by timeout_ms, so a ring that never drains
     * returns HAL_TIMEOUT with nothing partially queued, instead of hanging.
     *
     * 2026-09-05 fix: the original gate waited for `free_bytes >= len`
     * outright, which for any len larger than the TX ring itself
     * (HAL_UART_ESP_TX_RING_BUF_SIZE, 4096) can NEVER be satisfied even with
     * an idle, fully-draining ring -- free_bytes tops out at the ring size,
     * so that request always ran out the clock and returned HAL_TIMEOUT
     * regardless of timeout_ms, even though uart_write_bytes() itself
     * chunks writes larger than the ring just fine. Send in ring-sized
     * chunks instead: wait for room for the NEXT chunk only (bounded by the
     * remaining timeout budget), write it, then loop for the rest -- a
     * timeout can only ever land BETWEEN chunks, never partway through one,
     * so nothing partial is queued within a chunk. But for a `len` larger
     * than one ring (HAL_UART_ESP_TX_RING_BUF_SIZE), a timeout on a later
     * chunk still leaves the earlier chunk(s) already written to the
     * driver/wire -- this function has no way to un-send those, so a
     * HAL_TIMEOUT return in that case is NOT "nothing queued", it is
     * "queued up through total_written bytes, no way to report or retract
     * that count via this return value". Every real caller's frames today
     * are smaller than the 4096 B ring (one chunk), so this is latent; see
     * hal_uart.h's hal_uart_send_blocking() doc for the caller-facing
     * caveat. */
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);
    size_t total_written = 0;
    while (total_written < len) {
        size_t remaining = len - total_written;
        size_t chunk_target = (remaining < HAL_UART_ESP_TX_RING_BUF_SIZE)
                                   ? remaining
                                   : HAL_UART_ESP_TX_RING_BUF_SIZE;

        for (;;) {
            size_t free_bytes = 0;
            esp_err_t free_err = uart_get_tx_buffer_free_size(impl->port, &free_bytes);
            if (free_err != ESP_OK) {
                impl->tx_dropped_count += (uint32_t)(len - total_written);
                return hal_esp_err_to_status(free_err);
            }
            if (free_bytes >= chunk_target) {
                break;
            }
            if ((xTaskGetTickCount() - start_tick) >= timeout_ticks) {
                impl->tx_dropped_count += (uint32_t)(len - total_written);
                return HAL_TIMEOUT;
            }
            vTaskDelay(1);
        }

        int written = uart_write_bytes(impl->port, (const char *)data + total_written, chunk_target);
        if (written <= 0 || (size_t)written != chunk_target) {
            impl->tx_dropped_count += (uint32_t)(len - total_written - (size_t)(written > 0 ? written : 0));
            return HAL_IO;
        }
        total_written += (size_t)written;
    }

    /* Remaining budget after the free-space wait above, so the total time
     * spent in this call (wait + wait_tx_done) never exceeds timeout_ms in
     * the ordinary case.
     *
     * Review fix (2026-09-05): all bytes are already handed to
     * uart_write_bytes() above -- they are queued in the driver's TX ring,
     * on their way out the wire -- by the time this line runs. If the
     * free-space poll used up the ENTIRE timeout budget (remaining_ticks
     * computes to 0), calling uart_wait_tx_done(port, 0) asks it to check
     * "already fully drained?" with no wait at all; on a ring that still has
     * a few bytes left to shift out, that returns ESP_ERR_TIMEOUT even
     * though nothing is stuck -- the bytes were written and WILL leave the
     * wire within microseconds, this call just didn't wait for it. Without a
     * floor, that HAL_TIMEOUT propagates to uart_protocol.c's frame_and_send()
     * as "the frame was not sent" and it queues a full retransmission (up to
     * UART_PROTO_MAX_RETRIES times) of a frame that is, in fact, already on
     * the wire -- a duplicate the receiver's dedup ring then has to absorb,
     * not a correctness bug, but needless traffic and a misleading TIMEOUT
     * status for a send that actually succeeded. Floor the wait at
     * HAL_UART_ESP_MIN_WAIT_TX_DONE_TICKS (a few ticks -- draining the last
     * few bytes of one worst-case stuffed frame off a >=4096 B/s UART takes
     * well under 1 ms) so uart_wait_tx_done() always gets a real chance to
     * observe completion instead of being asked to answer instantly. */
    TickType_t elapsed_ticks = xTaskGetTickCount() - start_tick;
    TickType_t remaining_ticks = (elapsed_ticks < timeout_ticks) ? (timeout_ticks - elapsed_ticks) : 0;
    if (remaining_ticks < HAL_UART_ESP_MIN_WAIT_TX_DONE_TICKS) {
        remaining_ticks = HAL_UART_ESP_MIN_WAIT_TX_DONE_TICKS;
    }
    esp_err_t err = uart_wait_tx_done(impl->port, remaining_ticks);
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
