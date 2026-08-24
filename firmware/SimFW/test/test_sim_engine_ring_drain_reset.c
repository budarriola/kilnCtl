// Host test pinning sim_event_ring_drain()'s (sim_engine.c) resync logic
// against a ring-producer reset -- the CONFIRMED BUG this pass fixes.
//
// THE BUG (bench-found 2026-08-24, kilnsim selftest's event_sequence_
// continuity check failing PASS/FAIL/PASS/FAIL/FAIL against a healthy
// fixture): a fault would genuinely fire (FAULT/LIST confirmed the slot
// went ACTIVE) but its FAULT_FIRED EVT frame never reached the PC, with all
// three of telemetry.c's own loss counters (evt_seq_gap_count,
// evt_send_drop_count, evt_ring_hwm) staying clean -- because nothing in
// the send path ever ran at all: telemetry.c's drain call
// (sim_event_ring_drain()) itself silently, permanently returned 0.
//
// ROOT CAUSE: apply_reset() (sim_engine.c, called by SYS RESET_SIM and
// MODEL LOAD_PRESET) zeroes the ring's producer counter (s_ring_next_seq)
// but has no way to reach into telemetry.c's independent consumer cursor
// (s_evt_next_seq) -- by design (sim_snapshot.h: "multiple concurrent
// consumers... sim_engine owns the scheme", drain cursors live in each
// consumer, not the producer). Before this pass's fix,
// sim_event_ring_drain() only resynced a cursor that had fallen BEHIND
// oldest_available (`start_seq < oldest_available`, forward-wrap case). A
// cursor already advanced past 0 at the moment of a reset (e.g. 120) sits
// ABOVE the freshly-zeroed s_ring_next_seq, satisfying neither the old
// resync check nor the drain loop's own `start_seq + count <
// s_ring_next_seq` condition -- so the loop found 0 events, forever, until
// the ring produced its way back up past the cursor's stale pre-reset
// value (which could be a long time at this fixture's actual event rate:
// only relay edges and fault fires ever push, nowhere near the 256-entry
// ring's fill rate telemetry's own periodic TELEMETRY frame enjoys). This
// exactly matches virtual_simfw.c's own documented hazard
// (reset_client_evt_cursors(), tools/virtual_simfw/src/virtual_simfw.c
// lines ~761-772) -- that reference model resyncs every client's cursor
// explicitly on reset; this firmware had no equivalent for its one real
// consumer.
//
// THE FIX: sim_event_ring_drain() also resyncs to oldest_available when
// start_seq > s_ring_next_seq -- only reachable via a producer reset
// underneath an already-advanced cursor (s_ring_next_seq is otherwise
// monotonic non-decreasing), so this cannot misfire during ordinary
// production/drain.
//
// Why this file mirrors rather than calls the real code: sim_engine.c
// pulls in FreeRTOS (queue.h/semphr.h/task.h) and pico-sdk-adjacent headers
// and is not part of this host-test harness's source list
// (build_host_tests.ps1 compiles only src/sim/'s pure modules plus a
// handful of task-layer mirrors) -- same reasoning
// test_i2c_owner_io_read.c's header comment documents for i2c_owner.c.
// mirror_ring_drain() below is a deliberately small, byte-for-byte copy of
// sim_event_ring_drain()'s body (sim_engine.c) with the FreeRTOS mutex
// take/give stripped (this test is single-threaded) -- keep the two in
// sync by hand if either changes.
#include <stdbool.h>
#include <stdint.h>

#include "test_common.h"

#define MIRROR_RING_SIZE 8u /* small on purpose -- exercises the forward-wrap
                              * path with a handful of pushes, same as the
                              * real SIM_EVENT_RING_SIZE (256) would but
                              * without needing hundreds of test events */

typedef struct {
    uint32_t seq;
} mirror_event_t;

// Byte-for-byte mirror of sim_event_ring_drain()'s decision logic
// (sim_engine.c), minus the mutex (single-threaded host test) and the ring
// storage itself reduced to just `seq` (nothing in this function's contract
// touches the event payload beyond copying the slot, which a bare seq
// stands in for perfectly well here).
static uint32_t mirror_ring_drain(uint32_t ring_next_seq, mirror_event_t ring[MIRROR_RING_SIZE],
                                   mirror_event_t *out, uint32_t max_out, uint32_t *inout_next_seq,
                                   bool apply_reset_resync_fix)
{
    uint32_t oldest_available = (ring_next_seq > MIRROR_RING_SIZE) ? (ring_next_seq - MIRROR_RING_SIZE) : 0u;
    uint32_t start_seq = *inout_next_seq;

    if (apply_reset_resync_fix) {
        // THE FIX, mirrored exactly.
        if (start_seq < oldest_available || start_seq > ring_next_seq) {
            start_seq = oldest_available;
        }
    } else {
        // THE BUG, mirrored exactly: only the forward-wrap branch, no
        // check for a cursor left stranded above ring_next_seq by a reset.
        if (start_seq < oldest_available) {
            start_seq = oldest_available;
        }
    }

    uint32_t count = 0;
    while (start_seq + count < ring_next_seq && count < max_out) {
        out[count] = ring[(start_seq + count) % MIRROR_RING_SIZE];
        count++;
    }

    if (count > 0u) {
        *inout_next_seq = start_seq + count;
    } else {
        *inout_next_seq = start_seq;
    }

    return count;
}

// Fills ring[i % MIRROR_RING_SIZE].seq = i for i in [0, ring_next_seq) --
// enough for this test's small counts, mirroring what ring_push() would
// have written as the producer advanced from 0 to ring_next_seq.
static void mirror_fill_ring(mirror_event_t ring[MIRROR_RING_SIZE], uint32_t ring_next_seq)
{
    for (uint32_t i = 0; i < ring_next_seq; i++) {
        ring[i % MIRROR_RING_SIZE].seq = i;
    }
}

static void test_normal_drain_no_reset(void)
{
    TEST_SECTION("sim_event_ring_drain -- ordinary drain, no reset involved");

    mirror_event_t ring[MIRROR_RING_SIZE];
    mirror_fill_ring(ring, 3u);
    mirror_event_t out[MIRROR_RING_SIZE];
    uint32_t cursor = 0;

    uint32_t n = mirror_ring_drain(3u, ring, out, MIRROR_RING_SIZE, &cursor, true);
    TEST_CHECK(n == 3u, "drains all 3 produced events");
    TEST_CHECK(out[0].seq == 0 && out[1].seq == 1 && out[2].seq == 2, "events come back in seq order 0,1,2");
    TEST_CHECK(cursor == 3u, "cursor advances to the producer's count");
}

static void test_forward_wrap_still_resyncs(void)
{
    TEST_SECTION("sim_event_ring_drain -- pre-existing forward-wrap resync is untouched by this fix");

    mirror_event_t ring[MIRROR_RING_SIZE];
    uint32_t ring_next_seq = 20u; // ring holds seq 12..19 only (MIRROR_RING_SIZE == 8)
    mirror_fill_ring(ring, ring_next_seq);
    mirror_event_t out[MIRROR_RING_SIZE];
    uint32_t cursor = 0; // a consumer that never drained -- now far behind

    uint32_t n = mirror_ring_drain(ring_next_seq, ring, out, MIRROR_RING_SIZE, &cursor, true);
    TEST_CHECK(n == MIRROR_RING_SIZE, "drains exactly what's still available (oldest..newest)");
    TEST_CHECK(out[0].seq == 12u, "resyncs to the oldest still-available seq, not 0");
    TEST_CHECK(cursor == 20u, "cursor catches all the way up to the producer");
}

static void test_reset_underflow_resyncs_with_fix(void)
{
    TEST_SECTION("sim_event_ring_drain -- THE FIX: cursor stranded above a reset producer resyncs, not stuck forever");

    mirror_event_t ring[MIRROR_RING_SIZE];
    // Simulate: consumer had fully drained up through seq 5 (cursor==5) when
    // apply_reset() zeroed the producer's counter, exactly SYS RESET_SIM's
    // effect on s_ring_next_seq. The producer then pushed 2 fresh
    // post-reset events (seq 0, 1 again).
    uint32_t cursor = 5u;
    uint32_t ring_next_seq = 2u;
    mirror_fill_ring(ring, ring_next_seq);
    mirror_event_t out[MIRROR_RING_SIZE];

    uint32_t n = mirror_ring_drain(ring_next_seq, ring, out, MIRROR_RING_SIZE, &cursor, true);
    TEST_CHECK(n == 2u, "the fix: both post-reset events are delivered on the very next drain");
    TEST_CHECK(out[0].seq == 0u && out[1].seq == 1u, "delivered events are the fresh post-reset seq 0,1");
    TEST_CHECK(cursor == 2u, "cursor resyncs down to the producer's new count");

    // And a second drain with nothing new produced correctly reports 0
    // (proves the fix doesn't turn every call into a false resync loop).
    n = mirror_ring_drain(ring_next_seq, ring, out, MIRROR_RING_SIZE, &cursor, true);
    TEST_CHECK(n == 0u, "no new events since the last drain reports 0, same as ordinary steady state");
    TEST_CHECK(cursor == 2u, "cursor holds steady when nothing new was produced");
}

static void test_reset_underflow_stuck_forever_without_fix(void)
{
    TEST_SECTION("sim_event_ring_drain -- WITHOUT the fix, a stranded cursor never recovers (documents the bug)");

    mirror_event_t ring[MIRROR_RING_SIZE];
    uint32_t cursor = 120u; // a consumer that had drained a lot before the reset
    uint32_t ring_next_seq = 2u; // post-reset producer, same as the real bench symptom
    mirror_fill_ring(ring, ring_next_seq);
    mirror_event_t out[MIRROR_RING_SIZE];

    uint32_t n = mirror_ring_drain(ring_next_seq, ring, out, MIRROR_RING_SIZE, &cursor, /*apply_reset_resync_fix=*/false);
    TEST_CHECK(n == 0u, "the bug: the 2 genuinely-produced post-reset events are silently lost");
    TEST_CHECK(cursor == 120u, "the bug: cursor never moves, so every future drain repeats this forever "
                                "until the producer counts all the way back up past 120");
}

void run_test_sim_engine_ring_drain_reset(void)
{
    test_normal_drain_no_reset();
    test_forward_wrap_still_resyncs();
    test_reset_underflow_resyncs_with_fix();
    test_reset_underflow_stuck_forever_without_fix();
}
