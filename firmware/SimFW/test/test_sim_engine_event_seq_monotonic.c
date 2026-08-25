// Host test pinning the event ring's sequence counter as MONOTONIC for the
// whole boot lifetime -- sim_engine.c's apply_reset() must never zero
// s_ring_next_seq.
//
// THE DEFECT THIS CLOSES (bench-found 2026-08-24, and the fourth in one
// family: state reset on one side of a producer/consumer pair but not the
// other). The counter used to restart at 0 on every SYS RESET_SIM. An EVT
// frame still in flight when a scenario ended landed in the PC's
// connection-lifetime buffer just after that scenario's trailing
// zero-timeout drain, and the NEXT scenario's first poll collected it --
// carrying a seq the new run would legitimately produce itself. Straggler
// and genuine same-run event were then indistinguishable after the fact, so
// a stray event could be attributed to a run it was never part of, and
// could decide that run's PASS/FAIL.
//
// What actually surfaced it was mild by comparison: fixture_fault_lifecycle
// reporting seqs [0, 1, 7] under `kilnsim testmgr`, the 7 belonging to the
// scenario before it and showing up as a sequence gap. That was luck of
// numbering. A straggler numbered 0 or 1 would have been silently absorbed.
//
// With the counter monotonic, every event in a boot has a unique seq, so a
// PC client that records the high-water seq at reset can reject a straggler
// by its number alone -- which is what kilnsim's manager now does, on top of
// flushing the buffer.
//
// Why this file mirrors rather than calls the real code: same reason
// test_sim_engine_ring_drain_reset.c's header gives -- sim_engine.c pulls in
// FreeRTOS and pico-sdk-adjacent headers and is not in
// build_host_tests.ps1's source list. Because a mirror would keep passing if
// someone put the reset back into the real file, this test is deliberately
// only HALF the guard: tools/check_event_seq_monotonic.ps1 scans the real
// sim_engine.c (and virtual_simfw.c) for a reintroduced zeroing, with
// comments stripped first. Neither half is sufficient alone.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "test_common.h"

#define MONO_RING_SIZE 8u

typedef struct {
    uint32_t seq;
    uint8_t type;
} mono_event_t;

// Mirrors sim_engine.c's ring_push() + apply_reset() pair, at the level of
// detail this invariant is about: what a reset does, and does not, do to the
// producer counter.
typedef struct {
    mono_event_t ring[MONO_RING_SIZE];
    uint32_t next_seq;
} mono_ring_t;

static uint32_t mono_push(mono_ring_t *r, uint8_t type)
{
    uint32_t seq = r->next_seq++;
    r->ring[seq % MONO_RING_SIZE].seq = seq;
    r->ring[seq % MONO_RING_SIZE].type = type;
    return seq;
}

// Mirrors apply_reset()'s treatment of the ring: nothing. The model state,
// sim_time_us and MANUAL overrides all reset around it; the ring's producer
// counter and its contents are deliberately left alone.
static void mono_apply_reset(mono_ring_t *r)
{
    (void)r;
}

static void test_seq_never_restarts_across_resets(void)
{
    TEST_SECTION("event seq -- a reset does not restart the sequence counter");

    mono_ring_t r;
    memset(&r, 0, sizeof(r));

    uint32_t a0 = mono_push(&r, 1);
    uint32_t a1 = mono_push(&r, 2);
    TEST_CHECK(a0 == 0u && a1 == 1u, "a fresh boot starts its first run at seq 0,1");

    mono_apply_reset(&r);

    uint32_t b0 = mono_push(&r, 1);
    uint32_t b1 = mono_push(&r, 2);
    TEST_CHECK(b0 == 2u, "the run after a reset continues the count, it does not restart at 0");
    TEST_CHECK(b1 == 3u, "and keeps counting");
    TEST_CHECK(b0 != a0 && b0 != a1 && b1 != a0 && b1 != a1,
               "no seq from the second run collides with one from the first");
    TEST_CHECK(r.next_seq == 4u, "producer counter reflects every event pushed since boot");
}

static void test_straggler_is_identifiable_by_seq_alone(void)
{
    TEST_SECTION("event seq -- a straggler from the previous run is rejectable by number alone");

    // The exact bench shape: run 1 produces some events, one of which is
    // still in flight as the run ends. The PC records the high-water seq at
    // reset and then judges everything it collects afterwards.
    mono_ring_t r;
    memset(&r, 0, sizeof(r));

    for (int i = 0; i < 7; i++) {
        (void)mono_push(&r, 1);
    }
    uint32_t straggler = mono_push(&r, 2); // produced by run 1, delivered late
    TEST_CHECK(straggler == 7u, "the in-flight event carries run 1's seq 7");

    // PC side: at reset, remember the last seq it could possibly have been
    // shown. Anything at or below that floor belongs to a past run.
    uint32_t floor_seq = straggler; // high-water mark observed before reset

    mono_apply_reset(&r);

    uint32_t fresh0 = mono_push(&r, 1);
    uint32_t fresh1 = mono_push(&r, 2);

    TEST_CHECK(straggler <= floor_seq, "the straggler falls at or below the floor -- rejected");
    TEST_CHECK(fresh0 > floor_seq && fresh1 > floor_seq,
               "both genuine post-reset events sit strictly above the floor -- kept");
    TEST_CHECK(fresh0 == 8u && fresh1 == 9u, "and they are numbered 8,9, not 0,1");
}

static void test_the_old_behaviour_was_genuinely_ambiguous(void)
{
    TEST_SECTION("event seq -- documents WHY: with a zeroing reset, straggler and fresh event collide");

    // Same scenario, but with the pre-2026-08-24 reset that zeroed the
    // counter. This test exists so the invariant's cost and benefit are
    // written down executably rather than only in prose -- if someone
    // reintroduces the zeroing, this is what they are reintroducing.
    mono_ring_t r;
    memset(&r, 0, sizeof(r));

    for (int i = 0; i < 7; i++) {
        (void)mono_push(&r, 1);
    }
    uint32_t straggler = mono_push(&r, 2);
    uint32_t floor_seq = straggler;

    r.next_seq = 0; // THE OLD apply_reset(), mirrored exactly

    uint32_t fresh0 = mono_push(&r, 1);
    uint32_t fresh1 = mono_push(&r, 2);

    TEST_CHECK(fresh0 == 0u && fresh1 == 1u, "the old behaviour restarts the run at 0,1");
    TEST_CHECK(fresh0 <= floor_seq && fresh1 <= floor_seq,
               "a seq floor cannot work at all under the old behaviour -- it would reject the "
               "genuine events too");

    // And the collision that made misattribution silent: had the straggler
    // been an early event rather than the 8th, its seq would be one a fresh
    // run also produces.
    mono_ring_t r2;
    memset(&r2, 0, sizeof(r2));
    uint32_t early_straggler = mono_push(&r2, 2); // run 1's first event, delivered late
    r2.next_seq = 0;
    uint32_t collides = mono_push(&r2, 1); // run 2's first event
    TEST_CHECK(early_straggler == collides,
               "under the old behaviour the two are the SAME number -- no gap, no way to tell "
               "them apart, and the gap detector that caught the bench case would have seen "
               "nothing");
}

void run_test_sim_engine_event_seq_monotonic(void)
{
    test_seq_never_restarts_across_resets();
    test_straggler_is_identifiable_by_seq_alone();
    test_the_old_behaviour_was_genuinely_ambiguous();
}
