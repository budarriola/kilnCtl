// Host-test stub -- see stubs/esp_err.h for why these exist. Added 2026-08-21
// for wifi_prov.c's host tests. No-ops: the tests call wifi_prov.c's do_*()
// bodies directly, never post_and_wait()/post_event()/owner_task(), so the
// real queue is never exercised -- these only need to compile and link.
#ifndef TEST_STUB_FREERTOS_QUEUE_H
#define TEST_STUB_FREERTOS_QUEUE_H

#include <stddef.h>

#include "freertos/FreeRTOS.h"

typedef struct queue_s *QueueHandle_t;

static inline QueueHandle_t xQueueCreate(unsigned long len, unsigned long item_size)
{
    (void)len;
    (void)item_size;
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

static inline BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t ticks)
{
    (void)q;
    (void)item;
    (void)ticks;
    g_stub_queue_send_calls++;
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
