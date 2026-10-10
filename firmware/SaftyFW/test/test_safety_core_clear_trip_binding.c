// test_safety_core_clear_trip_binding.c -- kilnlink audit 2026-10-09 M4.
//
// A CLEAR_TRIP bound to a trip occurrence (4-byte form, protocol 17) must
// never run safety_guards_try_clear() when its trip_seq is not the one
// latched at dequeue. safety_guards_clear_trip_occurrence_matches() and
// safety_guards_decide_clear_trip_outcome() are host-tested in
// test_safety_guards.c, but the gate that actually skips try_clear lives in
// safety_core.c, which is not host-compilable (FreeRTOS/pico-sdk). Same
// source-text-scan precedent as test_safety_core_s8_wiring.c: this reads the
// real text, so dropping the occurrence check from the gate fails here.
//
// It also pins the two link_task.c wiring points: the clear request carries
// the decoded seq into safety_core, and send_diag reads the trip seq BEFORE
// the trip state (the fail-safe order its own comment explains).
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

static char *read_src(const char *rel)
{
    const char *candidates[] = { rel };
    return test_read_source_anchored(__FILE__, rel, candidates, 1);
}

static size_t count_occurrences(const char *hay, const char *needle)
{
    size_t n = 0;
    for (const char *p = strstr(hay, needle); p; p = strstr(p + 1, needle)) {
        n++;
    }
    return n;
}

static void scan_safety_core(void)
{
    TEST_SECTION("M4: safety_core.c gates try_clear on the trip occurrence (source scan)");

    char *text = read_src("../src/tasks/safety_core.c");
    TEST_CHECK(text != NULL, "safety_core.c readable");
    if (!text) {
        return;
    }

    const char *calc = strstr(text, "occurrence_matches = safety_guards_clear_trip_occurrence_matches(");
    TEST_CHECK(calc != NULL, "occurrence_matches comes from the host-tested helper");
    const char *decide = calc ? strstr(calc, "outcome = safety_guards_decide_clear_trip_outcome(") : NULL;
    TEST_CHECK(decide != NULL, "outcome decided after the occurrence check");

    if (calc && decide) {
        size_t len = (size_t)(decide - calc);
        char *region = (char *)malloc(len + 1u);
        TEST_CHECK(region != NULL, "alloc");
        if (region) {
            memcpy(region, calc, len);
            region[len] = '\0';
            const char *args_end = strchr(region, ';');
            TEST_CHECK(args_end != NULL && strstr(region, "s_trip_seq") != NULL &&
                           strstr(region, "s_trip_seq") < args_end,
                       "occurrence compared against the latched s_trip_seq");
            const char *gate = strstr(region, "if (was_tripped && occurrence_matches) {");
            const char *call = strstr(region, "try_clear_result = safety_guards_try_clear(");
            TEST_CHECK(gate != NULL, "try_clear gated on was_tripped && occurrence_matches");
            TEST_CHECK(call != NULL && gate != NULL && gate < call,
                       "the only try_clear call sits inside that gate");
            free(region);
        }
    }
    TEST_CHECK(count_occurrences(text, "try_clear_result = safety_guards_try_clear(") == 1u,
               "exactly one try_clear call site in safety_core.c");
    free(text);
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

static bool body_has(const char *body, size_t len, const char *needle, const char **at)
{
    const char *p = strstr(body, needle);
    if (!p || (size_t)(p - body) >= len) {
        return false;
    }
    if (at) {
        *at = p;
    }
    return true;
}

static void scan_link_task(void)
{
    TEST_SECTION("M4: link_task.c carries trip_seq into safety_core (source scan)");

    char *text = read_src("../src/tasks/link_task.c");
    TEST_CHECK(text != NULL, "link_task.c readable");
    if (!text) {
        return;
    }

    size_t len = 0;
    const char *clear = function_body(text, "static void link_task_handle_clear_trip(", &len);
    TEST_CHECK(clear != NULL, "link_task_handle_clear_trip found");
    if (clear) {
        TEST_CHECK(body_has(clear, len, "link_frame_decide_clear_trip(", NULL) &&
                       body_has(clear, len, "msg.has_trip_seq, s_peer_announce.version)", NULL),
                   "wire decision sees whether the frame was bound and the peer's version");
        TEST_CHECK(body_has(clear, len, "safety_core_request_clear_trip(msg.has_trip_seq, msg.trip_seq)", NULL),
                   "queued request carries the decoded binding and seq");
    }

    const char *diag = function_body(text, "static void link_task_send_diag(", &len);
    TEST_CHECK(diag != NULL, "link_task_send_diag found");
    if (diag) {
        const char *seq_read = NULL;
        const char *state_read = NULL;
        TEST_CHECK(body_has(diag, len, "safety_core_get_trip_event(&diag_trip_seq", &seq_read),
                   "send_diag reads the trip seq");
        TEST_CHECK(body_has(diag, len, "safety_core_get_diag_status(", &state_read),
                   "send_diag reads the trip state");
        TEST_CHECK(seq_read && state_read && seq_read < state_read,
                   "trip seq read BEFORE the trip state (a tear binds to an older seq, refused as stale)");
        TEST_CHECK(body_has(diag, len, ".has_trip_seq = link_frame_trip_seq_supported(s_peer_announce.version)", NULL),
                   "byte30 only sent to a peer that announced protocol >= 17");
    }
    free(text);
}

void run_test_safety_core_clear_trip_binding(void)
{
    scan_safety_core();
    scan_link_task();
}
