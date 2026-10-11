// test_test_trip.c -- TEST_TRIP WP2 (docs/TEST_TRIP_PLAN.md) and F6.
//
// Pure decisions (link_frame.c) are tested directly. link_task.c and
// safety_core.c are not host-compilable, so their wiring is pinned by a
// source-text scan, same precedent as test_safety_core_clear_trip_binding.c.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "../src/tasks/link_frame.h"

#define OWN_BOOT 0x5Au
#define MAGIC KILNLINK_TEST_TRIP_MAGIC

static uint8_t decide(link_test_trip_state_t *st, uint32_t now, uint8_t magic, uint8_t req, uint8_t boot,
                      uint16_t peer, bool busy, bool tripped)
{
    return link_frame_decide_test_trip(st, now, magic, req, boot, OWN_BOOT, peer, busy, tripped);
}

static void test_decide_test_trip(void)
{
    TEST_SECTION("TEST_TRIP acceptance (plan section 4.1)");
    link_test_trip_state_t st = {false, 0u, 0u};

    TEST_CHECK(link_frame_trip_mask_for_reason(SAFETY_TRIP_TEST) == 0x0008u, "reason 4 mask is 0x0008");
    TEST_CHECK(decide(&st, 100, 0x00, 1, OWN_BOOT, 18, false, false) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_BAD_FRAME,
               "bad magic refused REFUSED_BAD_FRAME");
    TEST_CHECK(!st.have_accept, "a refusal records nothing");
    TEST_CHECK(decide(&st, 100, MAGIC, 1, OWN_BOOT, 17, false, false) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_PEER_VERSION,
               "peer 17 refused");
    TEST_CHECK(decide(&st, 100, MAGIC, 1, OWN_BOOT, 0, false, false) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_PEER_VERSION,
               "unknown peer refused");
    TEST_CHECK(decide(&st, 100, MAGIC, 1, OWN_BOOT + 1, 18, false, false) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_BOOT_ID,
               "wrong boot_id refused");
    TEST_CHECK(decide(&st, 100, MAGIC, 1, OWN_BOOT, 18, true, false) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_UPDATING,
               "update in progress refused");
    TEST_CHECK(decide(&st, 100, MAGIC, 1, OWN_BOOT, 18, false, true) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_ALREADY_TRIPPED,
               "already tripped refused");
    TEST_CHECK(!st.have_accept, "refusals never consume the rate limit");

    TEST_CHECK(decide(&st, 1000, MAGIC, 7, OWN_BOOT, 18, false, false) == KILNLINK_TEST_TRIP_OUTCOME_ACCEPTED,
               "valid request accepted");
    TEST_CHECK(decide(&st, 1500, MAGIC, 7, OWN_BOOT, 18, false, false) == KILNLINK_TEST_TRIP_OUTCOME_DUPLICATE,
               "same request_id inside the window is DUPLICATE");
    TEST_CHECK(decide(&st, 1500, MAGIC, 8, OWN_BOOT, 18, false, false) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_RATE_LIMIT,
               "different request_id inside 10 s is REFUSED_RATE_LIMIT");
    TEST_CHECK(decide(&st, 10999, MAGIC, 8, OWN_BOOT, 18, false, false) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_RATE_LIMIT,
               "still limited at 9.999 s");
    TEST_CHECK(decide(&st, 11000, MAGIC, 8, OWN_BOOT, 18, false, false) == KILNLINK_TEST_TRIP_OUTCOME_ACCEPTED,
               "accepted again at exactly 10 s");
    TEST_CHECK(decide(&st, 11001, MAGIC, 9, OWN_BOOT, 18, false, true) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_ALREADY_TRIPPED,
               "tripped check outranks the rate limit");
    /* Tick-counter wrap: 32-bit ms subtraction stays correct. */
    link_test_trip_state_t w = {true, 0xFFFFFF00u, 3u};
    TEST_CHECK(decide(&w, 0x100u, MAGIC, 4, OWN_BOOT, 18, false, false) == KILNLINK_TEST_TRIP_OUTCOME_REFUSED_RATE_LIMIT,
               "rate limit survives ms wrap");
}

static void test_decide_clear_v3(void)
{
    TEST_SECTION("F6: CLEAR_TRIP bound to the Pico boot_id");
    const safety_trip_t r = SAFETY_TRIP_TEST;
    const uint16_t m = link_frame_trip_mask_for_reason(r);

    TEST_CHECK(link_frame_boot_id_supported(18) && !link_frame_boot_id_supported(17) && !link_frame_boot_id_supported(0),
               "boot_id gate is >= 18");
    TEST_CHECK(link_frame_decide_clear_trip_v3(r, m, true, true, OWN_BOOT, OWN_BOOT, 18, 0) == LINK_CLEAR_TRIP_ACCEPT,
               "V3 clear with matching boot_id accepted");
    TEST_CHECK(link_frame_decide_clear_trip_v3(r, m, true, true, OWN_BOOT + 1, OWN_BOOT, 18, 0) ==
                   LINK_CLEAR_TRIP_REFUSE_BOOT_ID,
               "V3 clear with a different boot_id refused");
    TEST_CHECK(link_frame_decide_clear_trip_v3(r, m, true, true, OWN_BOOT + 1, OWN_BOOT, 0, 99999u) ==
                   LINK_CLEAR_TRIP_REFUSE_BOOT_ID,
               "a mismatched boot_id is refused whatever the peer version / grace");
    TEST_CHECK(link_frame_decide_clear_trip_v3(r, m, true, false, 0, OWN_BOOT, 18, 0) ==
                   LINK_CLEAR_TRIP_REFUSE_BOOT_ID_REQUIRED,
               "an 18 peer sending the V2 form is refused");
    TEST_CHECK(link_frame_decide_clear_trip_v3(r, m, true, false, 0, OWN_BOOT, 17, 0) == LINK_CLEAR_TRIP_ACCEPT,
               "a 17 peer's V2 clear still accepted");
    TEST_CHECK(link_frame_decide_clear_trip_v3(r, m, false, false, 0, OWN_BOOT, 0, 29999u) ==
                   LINK_CLEAR_TRIP_REFUSE_PEER_UNKNOWN,
               "unknown peer, legacy form, 29.999 s: refused");
    TEST_CHECK(link_frame_decide_clear_trip_v3(r, m, false, false, 0, OWN_BOOT, 0, 30000u) == LINK_CLEAR_TRIP_ACCEPT,
               "unknown peer, legacy form, 30 s: fallback accepts");
    TEST_CHECK(link_frame_decide_clear_trip_v3(r, m, true, true, OWN_BOOT, OWN_BOOT, 0, 0) == LINK_CLEAR_TRIP_ACCEPT,
               "unknown peer sending the boot-bound form is accepted at once");
    /* The older refusals still come first and are unchanged. */
    TEST_CHECK(link_frame_decide_clear_trip_v3(SAFETY_TRIP_NONE, m, true, true, OWN_BOOT, OWN_BOOT, 18, 0) ==
                   LINK_CLEAR_TRIP_REFUSE_NOTHING_TRIPPED,
               "nothing tripped still refused");
    TEST_CHECK(link_frame_decide_clear_trip_v3(r, 0x0001u, true, true, OWN_BOOT, OWN_BOOT, 18, 0) ==
                   LINK_CLEAR_TRIP_REFUSE_MASK_MISMATCH,
               "mask mismatch still refused");
    TEST_CHECK(link_frame_decide_clear_trip_v3(SAFETY_TRIP_INEFFECTIVE,
                                               link_frame_trip_mask_for_reason(SAFETY_TRIP_INEFFECTIVE), true, true,
                                               OWN_BOOT, OWN_BOOT, 18, 0) == LINK_CLEAR_TRIP_REFUSE_INEFFECTIVE,
               "S9 still unclearable");
}

static char *read_src(const char *rel)
{
    const char *candidates[] = { rel };
    return test_read_source_anchored(__FILE__, rel, candidates, 1);
}

static const char *function_body(const char *text, const char *sig, size_t *out_len)
{
    const char *s = strstr(text, sig);
    const char *open = s ? strchr(s, '{') : NULL;
    const char *close = open ? strstr(open, "\n}") : NULL;
    if (!close) {
        return NULL;
    }
    *out_len = (size_t)(close - open);
    return open;
}

static bool has(const char *body, size_t len, const char *needle)
{
    const char *p = strstr(body, needle);
    return p != NULL && (size_t)(p - body) < len;
}

static void scan_wiring(void)
{
    TEST_SECTION("TEST_TRIP wiring (source scan)");
    char *lt = read_src("../src/tasks/link_task.c");
    char *sc = read_src("../src/tasks/safety_core.c");
    TEST_CHECK(lt != NULL && sc != NULL, "sources readable");
    if (!lt || !sc) {
        free(lt);
        free(sc);
        return;
    }
    size_t len = 0;
    const char *diag = function_body(lt, "static void link_task_send_diag(", &len);
    TEST_CHECK(diag != NULL, "send_diag found");
    if (diag) {
        TEST_CHECK(has(diag, len, ".has_boot_id = link_frame_boot_id_supported(s_peer_announce.version)"),
                   "DIAG V3 boot_id only to a peer that announced >= 18");
        TEST_CHECK(has(diag, len, ".pico_boot_id = s_boot_id"), "DIAG V3 carries the Pico's own boot_id");
    }
    const char *tt = function_body(lt, "static void link_task_handle_test_trip(", &len);
    TEST_CHECK(tt != NULL, "test trip handler found");
    if (tt) {
        const char *dec = strstr(tt, "link_frame_decide_test_trip(");
        const char *req = strstr(tt, "safety_core_request_test_trip()");
        TEST_CHECK(dec != NULL && req != NULL && dec < req, "request only after the pure decision");
        TEST_CHECK(has(tt, len, "s_boot_id") && has(tt, len, "s_peer_announce.version") &&
                       has(tt, len, "update_task_transfer_active()"),
                   "decision fed own boot_id, peer version and update-busy");
        TEST_CHECK(has(tt, len, "if (outcome == KILNLINK_TEST_TRIP_OUTCOME_ACCEPTED) {"),
                   "latch requested only for ACCEPTED");
    }
    TEST_CHECK(strstr(lt, "case LINK_FRAME_TEST_TRIP_CMD:") != NULL, "0x2E dispatched");
    const char *cl = function_body(lt, "static void link_task_handle_clear_trip(", &len);
    TEST_CHECK(cl != NULL, "clear handler found");
    if (cl) {
        TEST_CHECK(has(cl, len, "link_frame_decide_clear_trip_v3(") &&
                       has(cl, len, "msg.has_boot_id, msg.pico_boot_id") && has(cl, len, "link_task_peer_unknown_ms()"),
                   "clear decided by the boot_id-bound function with the peer-unknown duration");
    }
    const char *peer = function_body(lt, "static uint32_t link_task_peer_unknown_ms(", &len);
    TEST_CHECK(peer != NULL && has(peer, len, "s_peer_announce.version != 0u"),
               "unknown duration only counts while version is 0");

    const char *take = function_body(sc, "static bool safety_core_test_trip_take(", &len);
    TEST_CHECK(take != NULL && has(take, len, "xQueueReceive(s_test_trip_queue"),
               "build_input drains the test-trip queue");
    TEST_CHECK(strstr(sc, ".test_trip_requested = safety_core_test_trip_take()") != NULL,
               "drained token reaches the guard input");
    const char *rq = function_body(sc, "bool safety_core_request_test_trip(", &len);
    TEST_CHECK(rq != NULL && has(rq, len, "xQueueSend(s_test_trip_queue, &token, 0)"), "request is a 0-tick enqueue");
    TEST_CHECK(strstr(sc, "s_test_trip_queue = xQueueCreate(1,") != NULL, "depth-1 queue created in start");
    free(lt);
    free(sc);
}

void run_test_test_trip(void)
{
    test_decide_test_trip();
    test_decide_clear_v3();
    scan_wiring();
}
