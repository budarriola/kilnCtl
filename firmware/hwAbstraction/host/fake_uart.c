/* fake_uart.c -- host fake backend for hal_uart.h. See fake_uart.h. */
#include "fake_uart.h"

#include <string.h>

#define FAKE_UART_MAGIC 0x46414B55u /* "FAKU" */

typedef struct {
    uint32_t magic;
    int      slot;
} fake_uart_handle_tag_t;

_Static_assert(sizeof(fake_uart_handle_tag_t) <= HAL_UART_STORAGE_BYTES,
               "fake_uart_handle_tag_t must fit hal_uart_t storage");

typedef struct {
    bool           in_use;
    hal_uart_cfg_t cfg;

    uint8_t  tx_buf[FAKE_UART_TX_CAPTURE_CAP];
    size_t   tx_len;
    uint32_t tx_dropped;

    uint8_t  rx_ring[FAKE_UART_RX_RING_CAP];
    size_t   rx_head;
    size_t   rx_count;
    uint32_t rx_dropped;

    uint32_t rx_error_count;
} fake_uart_slot_t;

static fake_uart_slot_t s_slots[FAKE_UART_MAX_INSTANCES];

static fake_uart_slot_t *get_slot(const hal_uart_t *u)
{
    if (u == NULL) return NULL;
    fake_uart_handle_tag_t tag;
    memcpy(&tag, u->storage, sizeof(tag));
    if (tag.magic != FAKE_UART_MAGIC) return NULL;
    if (tag.slot < 0 || tag.slot >= FAKE_UART_MAX_INSTANCES) return NULL;
    if (!s_slots[tag.slot].in_use) return NULL;
    return &s_slots[tag.slot];
}

void fake_uart_reset_all(void)
{
    memset(s_slots, 0, sizeof(s_slots));
}

bool fake_uart_is_live(const hal_uart_t *u)
{
    return get_slot(u) != NULL;
}

const uint8_t *fake_uart_tx_capture(const hal_uart_t *u, size_t *out_len)
{
    fake_uart_slot_t *s = get_slot(u);
    if (!s) {
        if (out_len) *out_len = 0;
        return NULL;
    }
    if (out_len) *out_len = s->tx_len;
    return s->tx_buf;
}

hal_status_t fake_uart_script_rx(hal_uart_t *u, const uint8_t *data, size_t len)
{
    fake_uart_slot_t *s = get_slot(u);
    if (!s) return HAL_NOT_READY;
    if (data == NULL && len > 0) return HAL_INVALID_ARG;
    for (size_t i = 0; i < len; i++) {
        if (s->rx_count >= FAKE_UART_RX_RING_CAP) {
            s->rx_head = (s->rx_head + 1) % FAKE_UART_RX_RING_CAP;
            s->rx_count--;
            s->rx_dropped++;
        }
        size_t tail = (s->rx_head + s->rx_count) % FAKE_UART_RX_RING_CAP;
        s->rx_ring[tail] = data[i];
        s->rx_count++;
    }
    return HAL_OK;
}

size_t fake_uart_rx_dropped_count(const hal_uart_t *u)
{
    fake_uart_slot_t *s = get_slot(u);
    return s ? s->rx_dropped : 0;
}

size_t fake_uart_rx_pending_count(const hal_uart_t *u)
{
    fake_uart_slot_t *s = get_slot(u);
    return s ? s->rx_count : 0;
}

hal_status_t hal_uart_init(hal_uart_t *u, const hal_uart_cfg_t *cfg)
{
    if (u == NULL || cfg == NULL) return HAL_INVALID_ARG;

    int free_slot = -1;
    for (int i = 0; i < FAKE_UART_MAX_INSTANCES; i++) {
        if (!s_slots[i].in_use) { free_slot = i; break; }
    }
    if (free_slot < 0) return HAL_NO_MEM;

    fake_uart_slot_t *s = &s_slots[free_slot];
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    s->cfg = *cfg;

    fake_uart_handle_tag_t tag;
    tag.magic = FAKE_UART_MAGIC;
    tag.slot = free_slot;
    memset(u->storage, 0, sizeof(u->storage));
    memcpy(u->storage, &tag, sizeof(tag));
    return HAL_OK;
}

hal_status_t hal_uart_deinit(hal_uart_t *u)
{
    fake_uart_slot_t *s = get_slot(u);
    if (!s) return HAL_NOT_READY;
    s->in_use = false;
    memset(u->storage, 0, sizeof(u->storage));
    return HAL_OK;
}

hal_status_t hal_uart_attach(hal_uart_t *u, int port)
{
    /* Transitional Phase-1b path (interface/hal_uart.h's doc comment):
     * uart_protocol.c attaches to a port a real uart_owner_t already
     * installed the driver for, rather than calling hal_uart_init(), which
     * on the ESP backend would try to uart_driver_install() a second time.
     * The host fake has no real driver to double-install, so this is simply
     * hal_uart_init() with the port recorded (cfg's other fields left at 0 --
     * nothing here reads them) instead of taken from a caller-supplied cfg.
     * Uses the same slot pool and the same {magic, slot} tag scheme as
     * hal_uart_init(), so every other fake_uart_ or hal_uart_ call works
     * identically on a handle produced by either function. */
    if (u == NULL) return HAL_INVALID_ARG;

    int free_slot = -1;
    for (int i = 0; i < FAKE_UART_MAX_INSTANCES; i++) {
        if (!s_slots[i].in_use) { free_slot = i; break; }
    }
    if (free_slot < 0) return HAL_NO_MEM;

    fake_uart_slot_t *s = &s_slots[free_slot];
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    s->cfg.port = port;

    fake_uart_handle_tag_t tag;
    tag.magic = FAKE_UART_MAGIC;
    tag.slot = free_slot;
    memset(u->storage, 0, sizeof(u->storage));
    memcpy(u->storage, &tag, sizeof(tag));
    return HAL_OK;
}

hal_status_t hal_uart_send(hal_uart_t *u, const uint8_t *data, size_t len)
{
    fake_uart_slot_t *s = get_slot(u);
    if (!s) return HAL_NOT_READY;
    if (data == NULL && len > 0) return HAL_INVALID_ARG;
    if (s->tx_len + len > FAKE_UART_TX_CAPTURE_CAP) {
        /* whole-buffer-or-HAL_BUSY: nothing is queued on this path, but the
         * caller's attempted length is still observable via the dropped
         * counter so backpressure is testable. */
        s->tx_dropped += (uint32_t)len;
        return HAL_BUSY;
    }
    memcpy(s->tx_buf + s->tx_len, data, len);
    s->tx_len += len;
    return HAL_OK;
}

hal_status_t hal_uart_send_blocking(hal_uart_t *u, const uint8_t *data,
                                     size_t len, uint32_t timeout_ms)
{
    (void)timeout_ms; /* host: instant, no time model */
    fake_uart_slot_t *s = get_slot(u);
    if (!s) return HAL_NOT_READY;
    if (data == NULL && len > 0) return HAL_INVALID_ARG;
    if (s->tx_len + len > FAKE_UART_TX_CAPTURE_CAP) {
        /* The capture buffer itself is exhausted, not a transient
         * backpressure condition (this primitive has no BUSY concept per
         * hal_uart.h -- it always completes) -- report as a length problem,
         * distinct from send()'s HAL_BUSY. */
        return HAL_INVALID_SIZE;
    }
    memcpy(s->tx_buf + s->tx_len, data, len);
    s->tx_len += len;
    return HAL_OK;
}

size_t hal_uart_recv(hal_uart_t *u, uint8_t *out, size_t max)
{
    fake_uart_slot_t *s = get_slot(u);
    if (!s || out == NULL) return 0;
    size_t n = (max < s->rx_count) ? max : s->rx_count;
    for (size_t i = 0; i < n; i++) {
        out[i] = s->rx_ring[s->rx_head];
        s->rx_head = (s->rx_head + 1) % FAKE_UART_RX_RING_CAP;
    }
    s->rx_count -= n;
    return n;
}

size_t hal_uart_recv_blocking(hal_uart_t *u, uint8_t *buf, size_t cap, uint32_t timeout_ms)
{
    /* Host has no time model / no real blocking: whatever is already
     * scripted via fake_uart_script_rx() is "immediately available", so
     * this never actually waits. If nothing is scripted, this is the
     * HAL_TIMEOUT-equivalent case -- returns 0 bytes, as documented in
     * hal_uart.h. */
    (void)timeout_ms;
    return hal_uart_recv(u, buf, cap);
}

uint32_t hal_uart_get_rx_error_count(const hal_uart_t *u)
{
    fake_uart_slot_t *s = get_slot(u);
    return s ? s->rx_error_count : 0;
}

uint32_t hal_uart_get_tx_dropped(const hal_uart_t *u)
{
    fake_uart_slot_t *s = get_slot(u);
    return s ? s->tx_dropped : 0;
}

hal_status_t hal_uart_restart(hal_uart_t *u)
{
    fake_uart_slot_t *s = get_slot(u);
    if (!s) return HAL_NOT_READY;
    /* RX-only, per hal_uart.h's contract -- must never touch tx_buf,
     * tx_len or tx_dropped. */
    s->rx_head = 0;
    s->rx_count = 0;
    s->rx_dropped = 0;
    s->rx_error_count = 0;
    return HAL_OK;
}

void *hal_uart_get_task_handle(const hal_uart_t *u)
{
    /* Host fake has no owner/event task at all. */
    (void)u;
    return NULL;
}
