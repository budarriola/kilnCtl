// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. No-ops: the tests call wifi_prov.c's do_*()
// bodies directly, never post_and_wait()/post_event()/owner_task(), so the
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

static inline QueueHandle_t xQueueCreate(unsigned long len, unsigned long item_size)
{
    (void)len;
    g_stub_queue_item_size = item_size;
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
    return pdFALSE; /* never actually delivered -- see file header */
}

static inline BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t ticks)
{
    (void)q;
    (void)out;
    (void)ticks;
    return pdFALSE;
}

static inline void vQueueDelete(QueueHandle_t q) { (void)q; }

#endif // TEST_STUB_FREERTOS_QUEUE_H
