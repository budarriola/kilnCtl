/* Host-native test for benchproto_link.{c,h} -- the task-registration and
 * reliability (retry/dedup) state machine. No board, no RTOS, no clock: this
 * file feeds decoded benchproto_frame_t values and pending-request state
 * directly into the pure functions and asserts on the resulting
 * benchproto_link_action_t / benchproto_link_status_t / bool, exactly the
 * kind of thing CommonFW/README.md's "no I/O and no time" rule is meant to
 * make possible.
 *
 * Build (MSVC host compiler, no CMake needed for this one-shot check):
 *   cl /nologo /W4 /I ..\include test_benchproto_link.c ..\src\benchproto_link.c
 */

#include <stdio.h>
#include <string.h>

#include "benchproto/benchproto_link.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

enum { DEV_HOST = 0, DEV_TARGET = 1 };

static benchproto_frame_t make_frame(benchproto_msg_type_t type, uint16_t msg_index, uint8_t src_device,
                                      uint8_t src_task, uint8_t dst_device, uint8_t dst_task)
{
    benchproto_frame_t f;
    memset(&f, 0, sizeof(f));
    f.msg_type = type;
    f.msg_index = msg_index;
    f.src_device = src_device;
    f.src_task = src_task;
    f.dst_device = dst_device;
    f.dst_task = dst_task;
    f.length = 0;
    f.payload = NULL;
    return f;
}

/* -- task registration -------------------------------------------------------- */

static void test_register_unregister(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);

    CHECK(!benchproto_link_is_registered(&link, 3), "task 3 starts unregistered");
    CHECK(benchproto_link_register_task(&link, 3) == BENCHPROTO_LINK_OK, "register task 3 OK");
    CHECK(benchproto_link_is_registered(&link, 3), "task 3 now registered");

    CHECK(benchproto_link_register_task(&link, 3) == BENCHPROTO_LINK_ERR_ALREADY_REGISTERED,
          "re-registering task 3 -> ERR_ALREADY_REGISTERED");

    CHECK(benchproto_link_unregister_task(&link, 3) == BENCHPROTO_LINK_OK, "unregister task 3 OK");
    CHECK(!benchproto_link_is_registered(&link, 3), "task 3 no longer registered");
    CHECK(benchproto_link_unregister_task(&link, 3) == BENCHPROTO_LINK_ERR_NOT_FOUND,
          "unregistering an already-gone task -> ERR_NOT_FOUND");
}

static void test_register_exhausts_slots(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);

    for (unsigned i = 0; i < BENCHPROTO_MAX_TASKS; ++i) {
        CHECK(benchproto_link_register_task(&link, (uint8_t)(i + 1)) == BENCHPROTO_LINK_OK,
              "filling every task slot succeeds");
    }
    CHECK(benchproto_link_register_task(&link, 250) == BENCHPROTO_LINK_ERR_NO_SLOTS,
          "one more registration past capacity -> ERR_NO_SLOTS");
}

static void test_msg_index_increments_and_wraps(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);

    uint16_t first = benchproto_link_next_msg_index(&link);
    uint16_t second = benchproto_link_next_msg_index(&link);
    CHECK(first == 0, "first msg_index issued is 0");
    CHECK(second == 1, "second msg_index issued is 1");

    link.next_tx_index = 0xFFFF;
    uint16_t last = benchproto_link_next_msg_index(&link);
    uint16_t wrapped = benchproto_link_next_msg_index(&link);
    CHECK(last == 0xFFFF, "counter reaches 0xFFFF");
    CHECK(wrapped == 0, "counter wraps to 0 after 0xFFFF, same as a plain uint16_t");
}

/* -- pending request / retry -------------------------------------------------- */

static void test_pending_retry_exhaustion(void)
{
    benchproto_pending_request_t pending;
    benchproto_pending_begin(&pending, DEV_TARGET, 3, 5);
    CHECK(pending.active && pending.attempt == 1, "pending starts active at attempt 1");

    unsigned successful_retries = 0;
    while (benchproto_pending_note_retry(&pending)) {
        successful_retries++;
        CHECK(pending.attempt == successful_retries + 1, "attempt counter increments on each retry");
    }
    /* BENCHPROTO_MAX_RETRIES total attempts allowed means
     * BENCHPROTO_MAX_RETRIES - 1 successful *retries* past the first
     * attempt before note_retry() finally refuses. */
    CHECK(successful_retries == BENCHPROTO_MAX_RETRIES - 1,
          "note_retry() allows exactly BENCHPROTO_MAX_RETRIES - 1 further attempts");
    CHECK(!pending.active, "pending is cleared once retries are exhausted");
}

static void test_pending_clear(void)
{
    benchproto_pending_request_t pending;
    benchproto_pending_begin(&pending, DEV_TARGET, 3, 5);
    benchproto_pending_clear(&pending);
    CHECK(!pending.active, "pending_clear() deactivates");
    CHECK(!benchproto_pending_note_retry(&pending), "note_retry() on an inactive pending returns false");
}

/* -- benchproto_link_on_frame: sender side (ACK/NACK matching) -------------- */

static void test_on_frame_ack_matches_pending(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_HOST);
    benchproto_pending_request_t pending;
    benchproto_pending_begin(&pending, DEV_TARGET, 3, 42);

    benchproto_frame_t ack = make_frame(BENCHPROTO_MSG_ACK, 42, DEV_TARGET, 3, DEV_HOST, 9);
    CHECK(benchproto_link_on_frame(&link, &pending, &ack) == BENCHPROTO_LINK_ACTION_ACK_MATCHED,
          "ACK from the awaited (device, task) with the awaited msg_index -> ACK_MATCHED");
}

static void test_on_frame_nack_matches_pending(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_HOST);
    benchproto_pending_request_t pending;
    benchproto_pending_begin(&pending, DEV_TARGET, 3, 42);

    benchproto_frame_t nack = make_frame(BENCHPROTO_MSG_NACK, 42, DEV_TARGET, 3, DEV_HOST, 9);
    CHECK(benchproto_link_on_frame(&link, &pending, &nack) == BENCHPROTO_LINK_ACTION_NACK_MATCHED,
          "NACK from the awaited (device, task) with the awaited msg_index -> NACK_MATCHED");
}

static void test_on_frame_ack_wrong_index_ignored(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_HOST);
    benchproto_pending_request_t pending;
    benchproto_pending_begin(&pending, DEV_TARGET, 3, 42);

    benchproto_frame_t stale_ack = make_frame(BENCHPROTO_MSG_ACK, 41, DEV_TARGET, 3, DEV_HOST, 9);
    CHECK(benchproto_link_on_frame(&link, &pending, &stale_ack) == BENCHPROTO_LINK_ACTION_IGNORE,
          "ACK for a different msg_index than the one awaited -> IGNORE (a stale/crossed reply)");
}

static void test_on_frame_ack_no_pending_ignored(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_HOST);

    benchproto_frame_t ack = make_frame(BENCHPROTO_MSG_ACK, 1, DEV_TARGET, 3, DEV_HOST, 9);
    CHECK(benchproto_link_on_frame(&link, NULL, &ack) == BENCHPROTO_LINK_ACTION_IGNORE,
          "ACK with pending == NULL (pure responder) -> IGNORE");

    benchproto_pending_request_t inactive;
    memset(&inactive, 0, sizeof(inactive));
    CHECK(benchproto_link_on_frame(&link, &inactive, &ack) == BENCHPROTO_LINK_ACTION_IGNORE,
          "ACK with an inactive pending -> IGNORE (nothing outstanding to match)");
}

/* -- benchproto_link_on_frame: receiver side (DATA/BROADCAST) --------------- */

static void test_on_frame_data_deliver_then_duplicate(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    CHECK(benchproto_link_register_task(&link, 3) == BENCHPROTO_LINK_OK, "setup: register task 3");

    benchproto_frame_t data = make_frame(BENCHPROTO_MSG_DATA, 7, DEV_HOST, 1, DEV_TARGET, 3);
    CHECK(benchproto_link_on_frame(&link, NULL, &data) == BENCHPROTO_LINK_ACTION_DELIVER,
          "first DATA frame to a registered task -> DELIVER");

    /* Caller "delivers" then marks it -- the first attempt's outcome. */
    CHECK(benchproto_link_mark_delivered(&link, 3, DEV_HOST, 1, 7) == BENCHPROTO_LINK_OK,
          "mark_delivered() after a successful hand-off -> OK");

    /* The sender's ACK for the first attempt was lost; it retransmits the
     * identical DATA frame (same msg_index). */
    CHECK(benchproto_link_on_frame(&link, NULL, &data) == BENCHPROTO_LINK_ACTION_DUPLICATE_REACK,
          "retransmit of an already-delivered DATA frame -> DUPLICATE_REACK, not DELIVER again");
}

static void test_on_frame_data_unroutable(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    /* Deliberately nothing registered. */

    benchproto_frame_t data = make_frame(BENCHPROTO_MSG_DATA, 1, DEV_HOST, 1, DEV_TARGET, 99);
    CHECK(benchproto_link_on_frame(&link, NULL, &data) == BENCHPROTO_LINK_ACTION_NACK_UNROUTABLE,
          "DATA frame to an unregistered task_id -> NACK_UNROUTABLE");
}

static void test_on_frame_data_wrong_device_ignored(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    CHECK(benchproto_link_register_task(&link, 3) == BENCHPROTO_LINK_OK, "setup: register task 3");

    /* dst_device doesn't match our own_device -- shouldn't happen
     * point-to-point, but must be handled safely. */
    benchproto_frame_t data = make_frame(BENCHPROTO_MSG_DATA, 1, DEV_HOST, 1, DEV_HOST, 3);
    CHECK(benchproto_link_on_frame(&link, NULL, &data) == BENCHPROTO_LINK_ACTION_IGNORE,
          "DATA frame addressed to a different dst_device -> IGNORE");
}

static void test_on_frame_broadcast_unregistered_is_ignored_not_nacked(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    /* Nothing registered. */

    benchproto_frame_t bcast = make_frame(BENCHPROTO_MSG_BROADCAST, 1, DEV_HOST, 1, DEV_TARGET, 99);
    CHECK(benchproto_link_on_frame(&link, NULL, &bcast) == BENCHPROTO_LINK_ACTION_IGNORE,
          "BROADCAST to an unregistered task_id -> IGNORE, never NACK_UNROUTABLE "
          "(a broadcast receiver is never obliged to reply either way)");
}

static void test_on_frame_broadcast_never_deduped(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    CHECK(benchproto_link_register_task(&link, 5) == BENCHPROTO_LINK_OK, "setup: register task 5");

    benchproto_frame_t bcast = make_frame(BENCHPROTO_MSG_BROADCAST, 9, DEV_HOST, 2, DEV_TARGET, 5);
    CHECK(benchproto_link_on_frame(&link, NULL, &bcast) == BENCHPROTO_LINK_ACTION_DELIVER,
          "first BROADCAST -> DELIVER");
    /* Per the DELIVER action's contract, a BROADCAST is never marked
     * delivered -- so an identical repeat (a genuine duplicate on the
     * wire, or simply another broadcast that happens to reuse the index)
     * is DELIVER again, not DUPLICATE_REACK. This is deliberate: broadcasts
     * have no dedup, see benchproto_link.h. */
    CHECK(benchproto_link_on_frame(&link, NULL, &bcast) == BENCHPROTO_LINK_ACTION_DELIVER,
          "a second identical BROADCAST -> DELIVER again, never deduped");
}

static void test_mark_delivered_unknown_task(void)
{
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    CHECK(benchproto_link_mark_delivered(&link, 3, DEV_HOST, 1, 1) == BENCHPROTO_LINK_ERR_NOT_FOUND,
          "mark_delivered() for a task_id that was never registered -> ERR_NOT_FOUND");
}

static void test_dedup_ring_depth(void)
{
    /* Fill the dedup ring past its depth with distinct msg_indexes for the
     * same (src_device, src_task), then confirm the oldest entry has been
     * evicted -- BENCHPROTO_DEDUP_DEPTH is deliberately shallow, ported
     * from UART_PROTOCOL.md's original design. */
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    CHECK(benchproto_link_register_task(&link, 4) == BENCHPROTO_LINK_OK, "setup: register task 4");

    for (uint16_t i = 0; i < BENCHPROTO_DEDUP_DEPTH; ++i) {
        benchproto_frame_t data = make_frame(BENCHPROTO_MSG_DATA, i, DEV_HOST, 1, DEV_TARGET, 4);
        CHECK(benchproto_link_on_frame(&link, NULL, &data) == BENCHPROTO_LINK_ACTION_DELIVER,
              "each distinct msg_index while filling the ring -> DELIVER");
        CHECK(benchproto_link_mark_delivered(&link, 4, DEV_HOST, 1, i) == BENCHPROTO_LINK_OK,
              "mark_delivered() while filling the ring -> OK");
    }

    /* msg_index 0 (the oldest) should now have been evicted by the ring
     * wrapping around after BENCHPROTO_DEDUP_DEPTH more entries. */
    for (uint16_t i = BENCHPROTO_DEDUP_DEPTH; i < 2u * BENCHPROTO_DEDUP_DEPTH; ++i) {
        benchproto_frame_t data = make_frame(BENCHPROTO_MSG_DATA, i, DEV_HOST, 1, DEV_TARGET, 4);
        CHECK(benchproto_link_on_frame(&link, NULL, &data) == BENCHPROTO_LINK_ACTION_DELIVER,
              "pushing the ring past its depth -> DELIVER for each new index");
        CHECK(benchproto_link_mark_delivered(&link, 4, DEV_HOST, 1, i) == BENCHPROTO_LINK_OK,
              "mark_delivered() while pushing the ring -> OK");
    }

    benchproto_frame_t retry_of_evicted = make_frame(BENCHPROTO_MSG_DATA, 0, DEV_HOST, 1, DEV_TARGET, 4);
    CHECK(benchproto_link_on_frame(&link, NULL, &retry_of_evicted) == BENCHPROTO_LINK_ACTION_DELIVER,
          "msg_index 0 has been evicted from the shallow ring -> classified DELIVER again, "
          "not DUPLICATE_REACK (a known, documented limitation of a fixed-depth ring, same as "
          "UART_PROTOCOL.md's original design)");
}

/* -- benchproto_link_reset_device: the SESSION_RESET fix ------------------- */

static void test_reset_device_clears_colliding_msg_index(void)
{
    /* Mirrors the exact bug this function exists to fix (SimFW's
     * cmd_ids.h SIMFW_CMD_SYS_SESSION_RESET doc comment / PROTOCOL.md sec
     * 4): a PC client's own tx msg_index counter restarts at 0 on every
     * reconnect while this side's dedup ring survives (only initialized
     * once, at boot) -- so a post-reconnect request can collide with a
     * ring entry left over from the PREVIOUS session. */
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    CHECK(benchproto_link_register_task(&link, 3) == BENCHPROTO_LINK_OK, "setup: register task 3");

    /* Previous session: msg_index 0 delivered and marked. */
    benchproto_frame_t old_session = make_frame(BENCHPROTO_MSG_DATA, 0, DEV_HOST, 0, DEV_TARGET, 3);
    CHECK(benchproto_link_on_frame(&link, NULL, &old_session) == BENCHPROTO_LINK_ACTION_DELIVER,
          "previous session's first message -> DELIVER");
    CHECK(benchproto_link_mark_delivered(&link, 3, DEV_HOST, 0, 0) == BENCHPROTO_LINK_OK,
          "previous session's first message marked delivered");

    /* New session (reconnect): PC's counter restarted at 0 -- an identical
     * (src_device, src_task, msg_index) tuple, but a GENUINELY NEW request,
     * not a retry. Without a reset, this is indistinguishable from the
     * old session's retry. */
    benchproto_frame_t new_session = make_frame(BENCHPROTO_MSG_DATA, 0, DEV_HOST, 0, DEV_TARGET, 3);
    CHECK(benchproto_link_on_frame(&link, NULL, &new_session) == BENCHPROTO_LINK_ACTION_DUPLICATE_REACK,
          "without a reset, the new session's msg_index 0 collides -> DUPLICATE_REACK (the bug)");

    /* SESSION_RESET clears the ring for DEV_HOST before the new session's
     * first ordinary command is even sent. */
    benchproto_link_reset_device(&link, DEV_HOST);

    CHECK(benchproto_link_on_frame(&link, NULL, &new_session) == BENCHPROTO_LINK_ACTION_DELIVER,
          "after reset_device(), the SAME colliding msg_index -> DELIVER, not suppressed (the fix)");
}

static void test_reset_device_leaves_other_devices_untouched(void)
{
    /* reset_device(DEV_HOST) must not disturb dedup state recorded for a
     * DIFFERENT src_device on the same link -- a reset scoped to one
     * client must not affect another's in-flight dedup protection. */
    enum { DEV_OTHER = 5 };
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    CHECK(benchproto_link_register_task(&link, 3) == BENCHPROTO_LINK_OK, "setup: register task 3");

    benchproto_frame_t from_other = make_frame(BENCHPROTO_MSG_DATA, 0, DEV_OTHER, 0, DEV_TARGET, 3);
    CHECK(benchproto_link_on_frame(&link, NULL, &from_other) == BENCHPROTO_LINK_ACTION_DELIVER,
          "setup: DEV_OTHER's message -> DELIVER");
    CHECK(benchproto_link_mark_delivered(&link, 3, DEV_OTHER, 0, 0) == BENCHPROTO_LINK_OK,
          "setup: DEV_OTHER's message marked delivered");

    benchproto_link_reset_device(&link, DEV_HOST); /* resets a DIFFERENT device */

    CHECK(benchproto_link_on_frame(&link, NULL, &from_other) == BENCHPROTO_LINK_ACTION_DUPLICATE_REACK,
          "resetting DEV_HOST leaves DEV_OTHER's own dedup entry intact -> still DUPLICATE_REACK");
}

static void test_reset_device_spans_every_registered_task(void)
{
    /* The bug is not confined to one task: the PC's single shared
     * msg_index counter means a reconnect can collide against WHICHEVER
     * tasks happened to receive traffic near the end of the previous
     * session. reset_device() must clear every registered task's ring for
     * that device, not just one. */
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    CHECK(benchproto_link_register_task(&link, 1) == BENCHPROTO_LINK_OK, "setup: register task 1 (SYS-like)");
    CHECK(benchproto_link_register_task(&link, 2) == BENCHPROTO_LINK_OK, "setup: register task 2 (MODEL-like)");

    benchproto_frame_t to_task1 = make_frame(BENCHPROTO_MSG_DATA, 0, DEV_HOST, 0, DEV_TARGET, 1);
    benchproto_frame_t to_task2 = make_frame(BENCHPROTO_MSG_DATA, 1, DEV_HOST, 0, DEV_TARGET, 2);
    CHECK(benchproto_link_on_frame(&link, NULL, &to_task1) == BENCHPROTO_LINK_ACTION_DELIVER, "setup: task1 DELIVER");
    CHECK(benchproto_link_mark_delivered(&link, 1, DEV_HOST, 0, 0) == BENCHPROTO_LINK_OK, "setup: task1 marked");
    CHECK(benchproto_link_on_frame(&link, NULL, &to_task2) == BENCHPROTO_LINK_ACTION_DELIVER, "setup: task2 DELIVER");
    CHECK(benchproto_link_mark_delivered(&link, 2, DEV_HOST, 0, 1) == BENCHPROTO_LINK_OK, "setup: task2 marked");

    /* Confirm both are indeed dedup-protected before the reset. */
    CHECK(benchproto_link_on_frame(&link, NULL, &to_task1) == BENCHPROTO_LINK_ACTION_DUPLICATE_REACK,
          "pre-reset: task1's msg_index 0 is deduped");
    CHECK(benchproto_link_on_frame(&link, NULL, &to_task2) == BENCHPROTO_LINK_ACTION_DUPLICATE_REACK,
          "pre-reset: task2's msg_index 1 is deduped");

    benchproto_link_reset_device(&link, DEV_HOST);

    CHECK(benchproto_link_on_frame(&link, NULL, &to_task1) == BENCHPROTO_LINK_ACTION_DELIVER,
          "post-reset: task1's colliding msg_index -> DELIVER");
    CHECK(benchproto_link_on_frame(&link, NULL, &to_task2) == BENCHPROTO_LINK_ACTION_DELIVER,
          "post-reset: task2's colliding msg_index -> DELIVER");
}

static void test_reset_device_on_empty_ring_is_a_harmless_no_op(void)
{
    /* Idempotent/safe-at-any-time (cmd_ids.h's own contract on
     * SIMFW_CMD_SYS_SESSION_RESET): calling this on a link with nothing
     * recorded yet must not crash or otherwise misbehave. */
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    CHECK(benchproto_link_register_task(&link, 3) == BENCHPROTO_LINK_OK, "setup: register task 3");

    benchproto_link_reset_device(&link, DEV_HOST); /* nothing recorded yet */

    benchproto_frame_t data = make_frame(BENCHPROTO_MSG_DATA, 0, DEV_HOST, 0, DEV_TARGET, 3);
    CHECK(benchproto_link_on_frame(&link, NULL, &data) == BENCHPROTO_LINK_ACTION_DELIVER,
          "reset on an already-empty ring is a no-op, not a crash -- still classifies normally");
}

static void test_reset_device_with_no_tasks_registered_is_safe(void)
{
    /* Nothing registered at all -- reset_device() must not read/write past
     * an empty tasks[] array. */
    benchproto_link_t link;
    benchproto_link_init(&link, DEV_TARGET);
    benchproto_link_reset_device(&link, DEV_HOST); /* must not crash */
    CHECK(!benchproto_link_is_registered(&link, 3), "still nothing registered afterward");
}

int main(void)
{
    test_register_unregister();
    test_register_exhausts_slots();
    test_msg_index_increments_and_wraps();
    test_pending_retry_exhaustion();
    test_pending_clear();
    test_on_frame_ack_matches_pending();
    test_on_frame_nack_matches_pending();
    test_on_frame_ack_wrong_index_ignored();
    test_on_frame_ack_no_pending_ignored();
    test_on_frame_data_deliver_then_duplicate();
    test_on_frame_data_unroutable();
    test_on_frame_data_wrong_device_ignored();
    test_on_frame_broadcast_unregistered_is_ignored_not_nacked();
    test_on_frame_broadcast_never_deduped();
    test_mark_delivered_unknown_task();
    test_dedup_ring_depth();
    test_reset_device_clears_colliding_msg_index();
    test_reset_device_leaves_other_devices_untouched();
    test_reset_device_spans_every_registered_task();
    test_reset_device_on_empty_ring_is_a_harmless_no_op();
    test_reset_device_with_no_tasks_registered_is_safe();

    if (g_failures == 0) {
        printf("ALL PASS\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
