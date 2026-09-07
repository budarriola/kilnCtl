// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. No-ops: the tests call wifi_prov.c's do_*()
// bodies directly, never wifi_prov_post_and_wait()/post_event()/owner_task(), so the
// real queue is never exercised -- these only need to compile and link.
#ifndef TEST_STUB_FREERTOS_QUEUE_H
#define TEST_STUB_FREERTOS_QUEUE_H

#include <stddef.h>
#include <string.h>

#include "freertos/FreeRTOS.h"

typedef struct queue_s *QueueHandle_t;

/* Test-visible: the item_size an xQueueCreate() call was given, so
 * xQueueSend() below knows how many bytes it can safely memcpy out of
 * `item` into g_stub_last_queue_item -- added 2026-08-21 for
 * test_uart_log_bridge.c, which needs to inspect what uart_log_vprintf()
 * actually built (uart_log_bridge_early_init() is the only xQueueCreate()
 * caller these host tests exercise). Kept as a plain global, not per-handle
 * state, since every stub queue is the same fake singleton handle already
 * (see xQueueCreate's `static int dummy` below) -- one process-wide "the
 * last queue created" is all any current test needs. */
static unsigned long g_stub_queue_item_size = 0;

/* Test-visible opt-in: added 2026-08-24 for test_uart_log_bridge.c's eviction
 * tests, which need to tell a full queue from an empty one -- the always-
 * pdFALSE behavior below can't do that. OFF (0) by default, so every test
 * that never touches this (test_wifi_prov.c, and this file's own three
 * pre-existing tests) keeps the exact original semantics with no changes on
 * its part. A test that wants a real bounded FIFO sets this to 1 before
 * calling xQueueCreate() (xQueueCreate() is what latches the capacity below
 * from its `len` argument, so the ring must be enabled first).
 * Defined in the test .c that wants it, same convention as
 * g_stub_last_queue_item -- an extern here would multiply-define across the
 * several test_*.c translation units that all include this header. Declared
 * up here, ahead of xQueueCreate() below, since that function reads it. */
extern int g_stub_queue_ring_enabled;

/* Ring backing storage, same 2026-08-24 need. Capacity is bounded by this
 * array size but actually SET per xQueueCreate() call from its own `len`
 * argument (clamped to this bound) -- so the ring faithfully models whatever
 * size the code under test asks for (uart_log_bridge.c's
 * UART_LOG_BRIDGE_QUEUE_LEN=64 today) instead of hard-coding that number into
 * the stub. 128 slots x 256 bytes comfortably covers every current caller. */
#define TEST_STUB_QUEUE_RING_MAX_CAPACITY 128
extern unsigned char g_stub_queue_ring[TEST_STUB_QUEUE_RING_MAX_CAPACITY][256];
extern unsigned long g_stub_queue_ring_item_len[TEST_STUB_QUEUE_RING_MAX_CAPACITY];
extern int g_stub_queue_ring_capacity; /* latched by xQueueCreate() when enabled */
extern int g_stub_queue_ring_count;
extern int g_stub_queue_ring_head;

static inline QueueHandle_t xQueueCreate(unsigned long len, unsigned long item_size)
{
    g_stub_queue_item_size = item_size;
    if (g_stub_queue_ring_enabled) {
        g_stub_queue_ring_capacity =
            (len <= TEST_STUB_QUEUE_RING_MAX_CAPACITY) ? (int)len : TEST_STUB_QUEUE_RING_MAX_CAPACITY;
        g_stub_queue_ring_count = 0;
        g_stub_queue_ring_head = 0;
    } else {
        (void)len;
    }
    static int dummy;
    return (QueueHandle_t)&dummy;
}

/* Test-visible: how many times something tried to post to the command
 * queue. wifi_prov_note_possible_static_reachability()'s test uses this to
 * confirm whether it DECIDED to post CMD_CONFIRM_STATIC_REACHABLE, without
 * needing a real queue to actually dispatch it (the tests call
 * do_confirm_static_reachable() directly to check what the post would have
 * done). Defined in test_wifi_prov.c. */
extern int g_stub_queue_send_calls;

/* Test-visible: raw bytes of the last item passed to xQueueSend(), copied
 * out before this stub reports the (still-fake) pdFALSE failure below --
 * added alongside g_stub_queue_item_size, same 2026-08-21 need. 256 bytes is
 * comfortably above any current queue item (uart_log_entry_t is ~130). Not
 * defined here (would violate the header-only convention every other stub
 * in this directory follows) -- each test .c that wants it defines the
 * backing array itself, like g_stub_queue_send_calls already does. */
extern unsigned char g_stub_last_queue_item[256];

static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)q;
    (void)ticks;
    g_stub_queue_send_calls++;
    if (item && g_stub_queue_item_size > 0 && g_stub_queue_item_size <= sizeof(g_stub_last_queue_item)) {
        memcpy(g_stub_last_queue_item, item, g_stub_queue_item_size);
    }
    if (g_stub_queue_ring_enabled) {
        if (g_stub_queue_ring_count >= g_stub_queue_ring_capacity) {
            return pdFALSE; /* ring full, exactly like a real bounded queue */
        }
        int tail = (g_stub_queue_ring_head + g_stub_queue_ring_count) % TEST_STUB_QUEUE_RING_MAX_CAPACITY;
        unsigned long n = (item && g_stub_queue_item_size <= sizeof(g_stub_queue_ring[0]))
                               ? g_stub_queue_item_size
                               : 0;
        if (item && n > 0) {
            memcpy(g_stub_queue_ring[tail], item, n);
        }
        g_stub_queue_ring_item_len[tail] = n;
        g_stub_queue_ring_count++;
        return pdTRUE;
    }
    return pdFALSE; /* never actually delivered -- see file header */
}

static inline BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks)
{
    (void)q;
    (void)ticks;
    if (g_stub_queue_ring_enabled) {
        if (g_stub_queue_ring_count <= 0) {
            return pdFALSE; /* ring empty */
        }
        int head = g_stub_queue_ring_head;
        if (out) {
            memcpy(out, g_stub_queue_ring[head], g_stub_queue_ring_item_len[head]);
        }
        g_stub_queue_ring_head = (head + 1) % TEST_STUB_QUEUE_RING_MAX_CAPACITY;
        g_stub_queue_ring_count--;
        return pdTRUE;
    }
    (void)out;
    return pdFALSE;
}

/* Added 2026-09-07 for uart_log_bridge.c's ERROR-over-WARN eviction scan
 * (log_eviction_2026-09-07.md, point 3), which needs to know how many
 * entries to drain/reinspect. Real FreeRTOS provides this already; this
 * stub only needs to answer for ring mode, since that's the only mode any
 * test exercises a full/partial queue in. */
static inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q)
{
    (void)q;
    return g_stub_queue_ring_enabled ? (UBaseType_t)g_stub_queue_ring_count : 0;
}

static inline void vQueueDelete(QueueHandle_t q) { (void)q; }

#endif // TEST_STUB_FREERTOS_QUEUE_H
