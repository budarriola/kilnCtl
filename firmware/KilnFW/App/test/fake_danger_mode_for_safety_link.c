// Minimal stand-ins for danger_mode_active() and heat_enable_is_held(),
// used only by kilnctl_host_tests_safety_link.exe (test_safety_link_
// compile.c pulls in safety_link_frames.c, which since 2026-09-15 (Opus
// re-review N1) calls both from safety_build_and_send_context()). The real
// danger_mode.c pulls in kiln_io_owner.h/profile_executor_state.h, and the
// real heat_enable.c defines heat_enable_service_pending_release() -- which
// this test file already fakes itself (see its own header comment) -- so
// linking either real .c would either widen this executable's established
// stub surface or collide (LNK2005) with that existing fake. Both are out
// of scope for the two static wire-decode decisions this executable
// targets; a fixed-false/fixed-not-held fake is enough to satisfy the
// linker. Real coverage of context-flag production lives in test_heat_
// enable.c (heat_enable.c, real) and any future dedicated danger-mode host
// test.
#include <stdbool.h>
#include <stdint.h>
#include "heat_enable.h"
#include "freertos/queue.h"

bool danger_mode_active(void)
{
    return false;
}

bool heat_enable_is_held(heat_enable_claimant_t who)
{
    (void)who;
    return false;
}

/* Ditto for uart_log_bridge_relay_safety() -- safety_link_poll.c (also
 * #included by test_safety_link_compile.c) calls it from the new
 * safety_link_service_log_relay() (2026-09-20, tools/PcTools/TODO.md log
 * relay item), but the real function lives in uart_log_bridge.c, which
 * this executable does not link (widening its stub surface is out of scope
 * for the two static wire-decode decisions it targets -- same reasoning as
 * above). A fixed fake that reports "queued" and drops the bytes is enough
 * to satisfy the linker; real relay behavior (prefixing, filtering,
 * dropped-count) is covered by test_uart_log_bridge.c against the real
 * uart_log_bridge.c. */
bool uart_log_bridge_relay_safety(uint8_t level, const char *text, uint8_t text_len)
{
    (void)level;
    (void)text;
    (void)text_len;
    return true;
}

/* freertos/queue.h's xQueueReceive()/xQueueCreate() stub ring, needed now
 * that safety_link_poll.c's safety_link_service_log_relay() (above) calls
 * xQueueReceive() on link->log_inbox -- see test_uart_log_bridge.c's
 * identical block for the full rationale. Left disabled (0) so every
 * existing call in this executable keeps the original always-pdFALSE/
 * always-fail stub behavior; this test file has no positive-path log-relay
 * test of its own (that lives in test_uart_log_bridge.c / test_safety_
 * link.c), so an always-empty queue is the correct, inert default. */
int g_stub_queue_ring_enabled = 0;
unsigned char g_stub_queue_ring[TEST_STUB_QUEUE_RING_MAX_CAPACITY][256];
unsigned long g_stub_queue_ring_item_len[TEST_STUB_QUEUE_RING_MAX_CAPACITY];
int g_stub_queue_ring_capacity = 0;
int g_stub_queue_ring_count = 0;
int g_stub_queue_ring_head = 0;
