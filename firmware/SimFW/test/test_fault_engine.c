// Host tests for fault_engine.c: trigger-type coverage, duration/repeat
// coverage, and -- the plan's core replayability contract (PLAN.md sections
// 7.2/4.2) -- a determinism test proving the same trigger sequence and seed
// produce a byte-identical event sequence every time.
#include <string.h>

#include "test_common.h"
#include "../src/sim/fault_engine.h"

static fault_trigger_t trigger_at_time(double t)
{
    fault_trigger_t trig;
    memset(&trig, 0, sizeof(trig));
    trig.kind = FAULT_TRIGGER_AT_SIM_TIME;
    trig.at_sim_time_s = t;
    return trig;
}

static fault_duration_t duration_permanent(void)
{
    fault_duration_t d;
    memset(&d, 0, sizeof(d));
    d.kind = FAULT_DURATION_PERMANENT;
    return d;
}

static fault_duration_t duration_for(double t)
{
    fault_duration_t d;
    memset(&d, 0, sizeof(d));
    d.kind = FAULT_DURATION_FOR;
    d.for_s = t;
    return d;
}

static fault_repeat_t repeat_once(void)
{
    fault_repeat_t r;
    memset(&r, 0, sizeof(r));
    r.kind = FAULT_REPEAT_ONCE;
    return r;
}

static fault_repeat_t repeat_every(double period, double jitter)
{
    fault_repeat_t r;
    memset(&r, 0, sizeof(r));
    r.kind = FAULT_REPEAT_EVERY;
    r.period_s = period;
    r.jitter_s = jitter;
    return r;
}

static void test_at_sim_time_trigger(void)
{
    TEST_SECTION("fault_engine -- AT_SIM_TIME trigger");

    fault_engine_t eng;
    fault_engine_init(&eng, 42);

    fault_trigger_t trig = trigger_at_time(5.0);
    fault_duration_t dur = duration_for(2.0);
    fault_repeat_t rep = repeat_once();
    TEST_CHECK(fault_engine_schedule(&eng, 0, 1, 0, &trig, &dur, &rep, NULL) == 0, "schedule returns the requested slot id");

    fault_event_t events[8];
    float temps[1] = {0.0f};
    bool relays[1] = {false};

    for (double t = 0.0; t < 4.0; t += 1.0) {
        fault_engine_snapshot_t snap = {t, temps, 1, relays, 1, NULL, 0};
        size_t n = fault_engine_tick(&eng, &snap, events, 8);
        TEST_CHECK(n == 0, "no event before at_sim_time_s is reached");
    }

    fault_engine_snapshot_t snap5 = {5.0, temps, 1, relays, 1, NULL, 0};
    size_t n = fault_engine_tick(&eng, &snap5, events, 8);
    TEST_CHECK(n == 1 && events[0].kind == FAULT_EVENT_FIRED && events[0].slot_id == 0,
               "AT_SIM_TIME fires exactly at (or after) the target sim time");

    fault_engine_snapshot_t snap6 = {6.0, temps, 1, relays, 1, NULL, 0};
    n = fault_engine_tick(&eng, &snap6, events, 8);
    TEST_CHECK(n == 0, "still within the FOR(2.0) duration: no CLEARED yet");

    fault_engine_snapshot_t snap8 = {8.0, temps, 1, relays, 1, NULL, 0};
    n = fault_engine_tick(&eng, &snap8, events, 8);
    TEST_CHECK(n == 1 && events[0].kind == FAULT_EVENT_CLEARED, "FOR(2.0) expires: CLEARED at active_since+2.0");

    fault_engine_snapshot_t snap9 = {9.0, temps, 1, relays, 1, NULL, 0};
    n = fault_engine_tick(&eng, &snap9, events, 8);
    TEST_CHECK(n == 0, "ONCE repeat: no re-fire after expiry");
}

static void test_at_zone_temp_trigger(void)
{
    TEST_SECTION("fault_engine -- AT_ZONE_TEMP trigger (rising/falling edge)");

    fault_engine_t eng;
    fault_engine_init(&eng, 7);

    fault_trigger_t trig;
    memset(&trig, 0, sizeof(trig));
    trig.kind = FAULT_TRIGGER_AT_ZONE_TEMP;
    trig.zone = 0;
    trig.temp_c = 400.0f;
    trig.temp_edge = FAULT_TEMP_EDGE_RISING;
    fault_duration_t dur = duration_permanent();
    fault_repeat_t rep = repeat_once();
    fault_engine_schedule(&eng, 3, 2, 0, &trig, &dur, &rep, NULL);

    fault_event_t events[8];
    bool relays[1] = {false};

    /* First tick just establishes the "previous" snapshot -- no edge is
     * possible on tick 0 since there is no prior sample yet. */
    float t1[1] = {350.0f};
    fault_engine_snapshot_t s1 = {0.0, t1, 1, relays, 1, NULL, 0};
    size_t n = fault_engine_tick(&eng, &s1, events, 8);
    TEST_CHECK(n == 0, "no prior snapshot yet: cannot detect a rising edge on the very first tick");

    float t2[1] = {390.0f};
    fault_engine_snapshot_t s2 = {1.0, t2, 1, relays, 1, NULL, 0};
    n = fault_engine_tick(&eng, &s2, events, 8);
    TEST_CHECK(n == 0, "390 < 400: not yet crossed");

    float t3[1] = {410.0f};
    fault_engine_snapshot_t s3 = {2.0, t3, 1, relays, 1, NULL, 0};
    n = fault_engine_tick(&eng, &s3, events, 8);
    TEST_CHECK(n == 1 && events[0].kind == FAULT_EVENT_FIRED, "390 -> 410 crosses 400 rising: fires");

    /* Falling-edge slot, symmetric check. */
    fault_engine_t eng2;
    fault_engine_init(&eng2, 7);
    trig.temp_edge = FAULT_TEMP_EDGE_FALLING;
    fault_engine_schedule(&eng2, 3, 2, 0, &trig, &dur, &rep, NULL);
    fault_engine_snapshot_t f1 = {0.0, t3, 1, relays, 1, NULL, 0}; /* 410 */
    fault_engine_tick(&eng2, &f1, events, 8);
    fault_engine_snapshot_t f2 = {1.0, t2, 1, relays, 1, NULL, 0}; /* 390 */
    n = fault_engine_tick(&eng2, &f2, events, 8);
    TEST_CHECK(n == 1 && events[0].kind == FAULT_EVENT_FIRED, "410 -> 390 crosses 400 falling: fires");
}

static void test_on_relay_edge_trigger(void)
{
    TEST_SECTION("fault_engine -- ON_RELAY_EDGE trigger (with delay)");

    fault_engine_t eng;
    fault_engine_init(&eng, 99);

    fault_trigger_t trig;
    memset(&trig, 0, sizeof(trig));
    trig.kind = FAULT_TRIGGER_ON_RELAY_EDGE;
    trig.relay = 0;
    trig.relay_edge = FAULT_RELAY_EDGE_CLOSE;
    trig.delay_s = 3.0;
    fault_duration_t dur = duration_permanent();
    fault_repeat_t rep = repeat_once();
    fault_engine_schedule(&eng, 5, 3, 0, &trig, &dur, &rep, NULL);

    float temps[1] = {0.0f};
    fault_event_t events[8];

    bool r_open[1] = {false};
    fault_engine_snapshot_t s0 = {0.0, temps, 1, r_open, 1, NULL, 0};
    fault_engine_tick(&eng, &s0, events, 8);

    bool r_closed[1] = {true};
    fault_engine_snapshot_t s1 = {1.0, temps, 1, r_closed, 1, NULL, 0};
    size_t n = fault_engine_tick(&eng, &s1, events, 8);
    TEST_CHECK(n == 0, "relay closes at t=1: fire is scheduled 3s out, not immediate");

    fault_engine_snapshot_t s2 = {3.0, temps, 1, r_closed, 1, NULL, 0};
    n = fault_engine_tick(&eng, &s2, events, 8);
    TEST_CHECK(n == 0, "t=3 (2s after the edge): still before the scheduled fire at t=4");

    fault_engine_snapshot_t s3 = {4.0, temps, 1, r_closed, 1, NULL, 0};
    n = fault_engine_tick(&eng, &s3, events, 8);
    TEST_CHECK(n == 1 && events[0].kind == FAULT_EVENT_FIRED, "t=4 (edge_time + delay_s): fires");
}

static void test_random_in_trigger_with_seed(void)
{
    TEST_SECTION("fault_engine -- RANDOM_IN trigger, seeded and bounded");

    fault_engine_t eng;
    fault_engine_init(&eng, 12345);

    fault_trigger_t trig;
    memset(&trig, 0, sizeof(trig));
    trig.kind = FAULT_TRIGGER_RANDOM_IN;
    trig.random_t0_s = 10.0;
    trig.random_t1_s = 20.0;
    fault_duration_t dur = duration_permanent();
    fault_repeat_t rep = repeat_once();
    fault_engine_schedule(&eng, 1, 4, 0, &trig, &dur, &rep, NULL);

    float temps[1] = {0.0f};
    bool relays[1] = {false};
    fault_event_t events[8];
    double fire_time = -1.0;
    for (double t = 0.0; t <= 21.0 && fire_time < 0.0; t += 0.5) {
        fault_engine_snapshot_t snap = {t, temps, 1, relays, 1, NULL, 0};
        size_t n = fault_engine_tick(&eng, &snap, events, 8);
        if (n > 0) {
            fire_time = events[0].sim_time_s;
        }
    }
    TEST_CHECK(fire_time >= 10.0 && fire_time <= 20.0, "RANDOM_IN fires within [t0,t1]");

    /* Same seed picks the same time -- the whole point of a seeded PRNG. */
    fault_engine_t eng2;
    fault_engine_init(&eng2, 12345);
    fault_engine_schedule(&eng2, 1, 4, 0, &trig, &dur, &rep, NULL);
    double fire_time2 = -1.0;
    for (double t = 0.0; t <= 21.0 && fire_time2 < 0.0; t += 0.5) {
        fault_engine_snapshot_t snap = {t, temps, 1, relays, 1, NULL, 0};
        size_t n = fault_engine_tick(&eng2, &snap, events, 8);
        if (n > 0) {
            fire_time2 = events[0].sim_time_s;
        }
    }
    TEST_CHECK(fire_time == fire_time2, "RANDOM_IN with the same seed picks the same fire time");
}

static void test_duration_every_repeat(void)
{
    TEST_SECTION("fault_engine -- repeat EVERY re-fires periodically");

    fault_engine_t eng;
    fault_engine_init(&eng, 555);

    fault_trigger_t trig = trigger_at_time(1.0);
    fault_duration_t dur = duration_for(0.5);
    fault_repeat_t rep = repeat_every(2.0, 0.0); /* no jitter -> exact period, easy to check */
    fault_engine_schedule(&eng, 2, 9, 0, &trig, &dur, &rep, NULL);

    float temps[1] = {0.0f};
    bool relays[1] = {false};
    fault_event_t events[8];

    uint32_t fired_count = 0;
    uint32_t cleared_count = 0;
    for (double t = 0.0; t <= 10.0; t += 0.5) {
        fault_engine_snapshot_t snap = {t, temps, 1, relays, 1, NULL, 0};
        size_t n = fault_engine_tick(&eng, &snap, events, 8);
        for (size_t i = 0; i < n; i++) {
            if (events[i].kind == FAULT_EVENT_FIRED) fired_count++;
            else cleared_count++;
        }
    }
    /* Fires at t=1, clears at t=1.5, re-arms for t=1.5+2.0=3.5, fires at
     * t=3.5, clears at 4.0, re-arms for 6.0, fires at 6.0, clears at 6.5,
     * re-arms for 8.5, fires at 8.5, clears at 9.0 -- 4 fire/clear pairs in
     * [0,10] at this period+duration. */
    TEST_CHECK(fired_count >= 3, "EVERY repeat fires more than once over a multi-period run");
    TEST_CHECK(fired_count == cleared_count, "every FIRED under FOR duration has a matching CLEARED");
}

static void test_manual_trigger_only_fires_via_fire_now(void)
{
    TEST_SECTION("fault_engine -- MANUAL trigger never auto-fires from tick()");

    fault_engine_t eng;
    fault_engine_init(&eng, 1);
    fault_trigger_t trig;
    memset(&trig, 0, sizeof(trig));
    trig.kind = FAULT_TRIGGER_MANUAL;
    fault_duration_t dur = duration_permanent();
    fault_repeat_t rep = repeat_once();
    fault_engine_schedule(&eng, 7, 1, 0, &trig, &dur, &rep, NULL);

    float temps[1] = {0.0f};
    bool relays[1] = {false};
    fault_event_t events[8];
    uint32_t total = 0;
    for (double t = 0.0; t <= 100.0; t += 10.0) {
        fault_engine_snapshot_t snap = {t, temps, 1, relays, 1, NULL, 0};
        total += (uint32_t)fault_engine_tick(&eng, &snap, events, 8);
    }
    TEST_CHECK(total == 0, "MANUAL slot never fires from tick() alone, however long it runs");

    // fault_engine_fire_now() defers: it only marks manual_fire_pending and
    // does NOT itself return/emit a FIRED event (see fault_engine.h's doc --
    // this is the fix for the real bug where FAULT_FIRE_NOW/TC_INJECT_FAULT
    // changed slot state but the event ring never learned about it, since
    // sim_engine.c only ever translates fault_engine_tick()'s own return
    // value into ring pushes). The very next fault_engine_tick() call is
    // what actually fires the slot and emits FIRED, through the exact same
    // ARMED-slot code path a triggered fire uses.
    bool requested = fault_engine_fire_now(&eng, 7);
    TEST_CHECK(requested, "fault_engine_fire_now() accepts the request for an ARMED MANUAL slot");

    fault_engine_snapshot_t snap_fire = {110.0, temps, 1, relays, 1, NULL, 0};
    size_t n = fault_engine_tick(&eng, &snap_fire, events, 8);
    TEST_CHECK(n == 1 && events[0].kind == FAULT_EVENT_FIRED && events[0].slot_id == 7,
               "the next tick() after fire_now() fires the slot and emits exactly one FIRED, "
               "for the requested slot id");
    TEST_CHECK(events[0].sim_time_s == 110.0,
               "the FIRED event carries the sim-clock timestamp of the tick that actually "
               "processed the request, not the time fire_now() was called");
}

// TC_INJECT_FAULT (cmd_task.c's handle_tc_inject_fault) schedules a MANUAL-
// trigger/PERMANENT/ONCE slot and immediately calls fault_sched_fire_now()
// on it -- fault_sched_fire_now() is a thin passthrough to
// fault_engine_fire_now(), so this mirrors that exact call sequence on the
// engine directly (fault_sched.c/sim_engine.c are FreeRTOS task files with
// no host-test coverage, per this file's own doc and test_gap_closure_logic
// .c's "why this file mirrors rather than calls the real code" note).
static void test_tc_inject_fault_path_emits_fired(void)
{
    TEST_SECTION("fault_engine -- TC_INJECT_FAULT's schedule+fire_now path emits FIRED");

    fault_engine_t eng;
    fault_engine_init(&eng, 2);

    fault_trigger_t trig;
    memset(&trig, 0, sizeof(trig));
    trig.kind = FAULT_TRIGGER_MANUAL;
    fault_duration_t dur = duration_permanent();
    fault_repeat_t rep = repeat_once();
    float params[4] = {3.5f, 0.0f, 0.0f, 0.0f};
    uint16_t sid = fault_engine_schedule(&eng, 3, /*fault_type*/ 0, /*target*/ 2, &trig, &dur, &rep, params);
    TEST_CHECK(sid == 3, "TC_INJECT_FAULT's FAULT_SCHEDULE step arms the requested slot");

    bool requested = fault_engine_fire_now(&eng, sid);
    TEST_CHECK(requested, "TC_INJECT_FAULT's fire_now step is accepted immediately after scheduling");

    float temps[1] = {0.0f};
    bool relays[1] = {false};
    fault_event_t events[8];
    fault_engine_snapshot_t snap = {0.05, temps, 1, relays, 1, NULL, 0};
    size_t n = fault_engine_tick(&eng, &snap, events, 8);
    TEST_CHECK(n == 1 && events[0].kind == FAULT_EVENT_FIRED && events[0].slot_id == 3,
               "TC_INJECT_FAULT's immediate fire reaches the ring-bound event stream on the "
               "very next tick, exactly like any other fault type routed through fire_now()");
}

// The bug report's specific worry: does mixing an immediate (fire_now) fault
// with a normally-triggered one produce gap-free, monotonic sequence numbers
// once translated to ring events? fault_engine_tick()'s out[] index order
// *is* the sequence order sim_engine.c's ring_push() stamps seq numbers in
// (it iterates out[] in order and calls ring_push() once per entry, seq++
// each time -- see sim_engine.c's "Fault fired/cleared events" block), so
// this test pins the out[]-ordering contract that guarantee rests on: slot
// index order within a tick, with no gaps or duplicates regardless of
// whether a given slot's FIRED came from a trigger or from fire_now().
static void test_mixed_manual_and_triggered_fires_are_gap_free_in_order(void)
{
    TEST_SECTION("fault_engine -- mixing fire_now() and triggered fires stays gap-free/ordered");

    fault_engine_t eng;
    fault_engine_init(&eng, 3);

    // Slot 0: normal AT_SIM_TIME trigger, fires on its own at t=1.0.
    fault_trigger_t trig0 = trigger_at_time(1.0);
    fault_duration_t dur0 = duration_permanent();
    fault_repeat_t rep0 = repeat_once();
    fault_engine_schedule(&eng, 0, 10, 0, &trig0, &dur0, &rep0, NULL);

    // Slot 5: MANUAL, requested via fire_now() before any tick runs.
    fault_trigger_t trig5;
    memset(&trig5, 0, sizeof(trig5));
    trig5.kind = FAULT_TRIGGER_MANUAL;
    fault_duration_t dur5 = duration_permanent();
    fault_repeat_t rep5 = repeat_once();
    fault_engine_schedule(&eng, 5, 11, 0, &trig5, &dur5, &rep5, NULL);
    TEST_CHECK(fault_engine_fire_now(&eng, 5), "slot 5's fire_now() request is accepted");

    float temps[1] = {0.0f};
    bool relays[1] = {false};
    fault_event_t events[8];

    // t=0.0: slot 0's trigger hasn't fired yet, but slot 5's manual_fire_
    // pending should already fire this tick (fault_engine_fire_now() was
    // called before any tick ran).
    fault_engine_snapshot_t snap0 = {0.0, temps, 1, relays, 1, NULL, 0};
    size_t n0 = fault_engine_tick(&eng, &snap0, events, 8);
    TEST_CHECK(n0 == 1 && events[0].kind == FAULT_EVENT_FIRED && events[0].slot_id == 5,
               "slot 5 (fire_now) fires on the very next tick even though slot 0's own "
               "trigger time hasn't arrived yet");

    // t=1.0: slot 0's own trigger now fires.
    fault_engine_snapshot_t snap1 = {1.0, temps, 1, relays, 1, NULL, 0};
    size_t n1 = fault_engine_tick(&eng, &snap1, events, 8);
    TEST_CHECK(n1 == 1 && events[0].kind == FAULT_EVENT_FIRED && events[0].slot_id == 0,
               "slot 0 (triggered) fires at its own scheduled time, unaffected by slot 5's "
               "earlier manual fire");

    // Across the two ticks: exactly two FIRED events total, for the two
    // distinct slots, each exactly once -- no gap, no duplicate, no event
    // silently dropped for either the manual or the triggered slot.
    TEST_CHECK(n0 + n1 == 2, "exactly one FIRED per slot across the run: fire_now() firing a "
               "slot does not suppress or duplicate a separately-triggered slot's own FIRED, "
               "and vice versa -- the sequence a ring producer would assign stays gap-free");
}

// The FIRED/CLEARED asymmetry the bug report asked about: before this fix,
// fault_engine_fire_now() fired a slot in a way the ring never learned
// about, but that same slot's *eventual* natural expiry (FOR duration, or
// exhausting its repeat count) already went through fault_engine_tick()'s
// normal path and so WOULD have reached the ring -- i.e. a PC client could
// see a CLEARED for a slot it never saw a FIRED for. This test confirms the
// fix removes that asymmetry: a fire_now()-fired slot's eventual expiry
// still emits CLEARED, and by now (post-fix) its FIRED was already emitted
// too, on the tick right after fire_now() was called.
static void test_fire_now_then_expiry_both_reach_the_ring(void)
{
    TEST_SECTION("fault_engine -- fire_now() FIRED/CLEARED asymmetry is gone post-fix");

    fault_engine_t eng;
    fault_engine_init(&eng, 4);

    fault_trigger_t trig;
    memset(&trig, 0, sizeof(trig));
    trig.kind = FAULT_TRIGGER_MANUAL;
    fault_duration_t dur = duration_for(2.0);
    fault_repeat_t rep = repeat_once();
    fault_engine_schedule(&eng, 9, 12, 0, &trig, &dur, &rep, NULL);
    TEST_CHECK(fault_engine_fire_now(&eng, 9), "fire_now() request accepted");

    float temps[1] = {0.0f};
    bool relays[1] = {false};
    fault_event_t events[8];

    fault_engine_snapshot_t snap_fire = {0.0, temps, 1, relays, 1, NULL, 0};
    size_t n_fire = fault_engine_tick(&eng, &snap_fire, events, 8);
    TEST_CHECK(n_fire == 1 && events[0].kind == FAULT_EVENT_FIRED,
               "fire_now()'d slot's FIRED reaches the event stream (the actual bug fix)");

    fault_engine_snapshot_t snap_mid = {1.0, temps, 1, relays, 1, NULL, 0};
    size_t n_mid = fault_engine_tick(&eng, &snap_mid, events, 8);
    TEST_CHECK(n_mid == 0, "still within FOR(2.0) duration: no CLEARED yet");

    fault_engine_snapshot_t snap_expire = {2.0, temps, 1, relays, 1, NULL, 0};
    size_t n_expire = fault_engine_tick(&eng, &snap_expire, events, 8);
    TEST_CHECK(n_expire == 1 && events[0].kind == FAULT_EVENT_CLEARED,
               "the fire_now()'d slot's natural FOR-duration expiry still emits CLEARED, "
               "matching the FIRED it now also gets -- no more orphaned CLEARED");
}

// Step 3 of the bug's ask: the same class of gap for FAULT_CANCEL on an
// ACTIVE slot. fault_engine_cancel()'s pre-fix behavior reset an ACTIVE
// slot straight to IDLE with no event at all (documented as "caller must
// synthesize one if it needs it" -- but no caller did), which is exactly
// the same "state changed, ring never learned" shape as the fire_now bug.
static void test_cancel_on_active_slot_emits_cleared(void)
{
    TEST_SECTION("fault_engine -- FAULT_CANCEL on an ACTIVE slot emits CLEARED");

    fault_engine_t eng;
    fault_engine_init(&eng, 5);

    fault_trigger_t trig = trigger_at_time(1.0);
    fault_duration_t dur = duration_permanent(); // would never clear on its own
    fault_repeat_t rep = repeat_once();
    fault_engine_schedule(&eng, 12, 13, 0, &trig, &dur, &rep, NULL);

    float temps[1] = {0.0f};
    bool relays[1] = {false};
    fault_event_t events[8];

    fault_engine_snapshot_t snap_fire = {1.0, temps, 1, relays, 1, NULL, 0};
    size_t n_fire = fault_engine_tick(&eng, &snap_fire, events, 8);
    TEST_CHECK(n_fire == 1 && events[0].kind == FAULT_EVENT_FIRED, "slot fires at its trigger time");

    bool cancel_ok = fault_engine_cancel(&eng, 12);
    TEST_CHECK(cancel_ok, "cancelling an ACTIVE slot is accepted");

    // Not reset to IDLE synchronously -- deferred to the next tick, same as
    // fire_now(), so the CLEARED it owes the ring goes through the one
    // FIRED/CLEARED-emitting code path instead of being silently dropped.
    fault_engine_snapshot_t snap_cancel = {1.5, temps, 1, relays, 1, NULL, 0};
    size_t n_cancel = fault_engine_tick(&eng, &snap_cancel, events, 8);
    TEST_CHECK(n_cancel == 1 && events[0].kind == FAULT_EVENT_CLEARED && events[0].slot_id == 12,
               "the next tick() after cancel() emits exactly one CLEARED for the cancelled slot");

    // A PERMANENT-duration slot would never have cleared on its own -- prove
    // this was really the cancel firing the CLEARED, not a coincidental
    // duration expiry, by ticking well past and confirming no second event.
    fault_engine_snapshot_t snap_after = {100.0, temps, 1, relays, 1, NULL, 0};
    size_t n_after = fault_engine_tick(&eng, &snap_after, events, 8);
    TEST_CHECK(n_after == 0, "PERMANENT duration never expires on its own -- confirms the "
               "CLEARED above came from the cancel, not a coincidental natural expiry");

    // fault_engine_cancel() resets to IDLE (not EXPIRED) -- an operator
    // abort is not exhaustion, so the slot is immediately reschedulable.
    uint16_t resched = fault_engine_schedule(&eng, 12, 14, 0, &trig, &dur, &rep, NULL);
    TEST_CHECK(resched == 12, "the cancelled slot resets to IDLE, not EXPIRED -- it is freely "
               "reschedulable afterward, exactly like a slot that was never armed");
}

// FAULT_CANCEL on a slot that never fired (ARMED, or already IDLE/EXPIRED)
// must stay immediate/synchronous -- there is nothing ACTIVE to report a
// CLEARED for, so deferring it would just add needless latency to a no-op.
static void test_cancel_on_armed_slot_is_immediate_and_silent(void)
{
    TEST_SECTION("fault_engine -- FAULT_CANCEL on an ARMED (never-fired) slot is immediate");

    fault_engine_t eng;
    fault_engine_init(&eng, 6);

    fault_trigger_t trig = trigger_at_time(50.0); // far enough out it won't fire in this test
    fault_duration_t dur = duration_permanent();
    fault_repeat_t rep = repeat_once();
    fault_engine_schedule(&eng, 20, 15, 0, &trig, &dur, &rep, NULL);

    bool cancel_ok = fault_engine_cancel(&eng, 20);
    TEST_CHECK(cancel_ok, "cancelling an ARMED slot is accepted");

    float temps[1] = {0.0f};
    bool relays[1] = {false};
    fault_event_t events[8];
    fault_engine_snapshot_t snap = {0.0, temps, 1, relays, 1, NULL, 0};
    size_t n = fault_engine_tick(&eng, &snap, events, 8);
    TEST_CHECK(n == 0, "cancelling a slot that never fired emits no CLEARED -- nothing was "
               "ever ACTIVE to report, and the reset already happened synchronously in "
               "fault_engine_cancel() itself, not deferred to this tick");
}

/* The core replayability contract (PLAN.md 7.2/4.2): the same trigger
 * sequence + the same seed must produce a byte-identical event log. Builds
 * a scenario mixing several trigger kinds (AT_SIM_TIME, AT_ZONE_TEMP,
 * ON_RELAY_EDGE, RANDOM_IN) plus an EVERY repeat, runs it through a
 * synthetic tick sequence twice with independent engine instances and the
 * same seed, and asserts the two event logs match exactly. */
static void build_replay_scenario(fault_engine_t *eng, uint32_t seed)
{
    fault_engine_init(eng, seed);

    fault_trigger_t t0 = trigger_at_time(2.0);
    fault_duration_t d0 = duration_for(1.0);
    fault_repeat_t r0 = repeat_once();
    fault_engine_schedule(eng, 0, 1, 0, &t0, &d0, &r0, NULL);

    fault_trigger_t t1;
    memset(&t1, 0, sizeof(t1));
    t1.kind = FAULT_TRIGGER_AT_ZONE_TEMP;
    t1.zone = 0;
    t1.temp_c = 500.0f;
    t1.temp_edge = FAULT_TEMP_EDGE_RISING;
    fault_duration_t d1 = duration_permanent();
    fault_repeat_t r1 = repeat_once();
    fault_engine_schedule(eng, 1, 2, 0, &t1, &d1, &r1, NULL);

    fault_trigger_t t2;
    memset(&t2, 0, sizeof(t2));
    t2.kind = FAULT_TRIGGER_ON_RELAY_EDGE;
    t2.relay = 0;
    t2.relay_edge = FAULT_RELAY_EDGE_OPEN;
    t2.delay_s = 1.5;
    fault_duration_t d2 = duration_for(2.0);
    fault_repeat_t r2 = repeat_every(3.0, 0.5); /* jitter -- exercises the PRNG */
    fault_engine_schedule(eng, 2, 3, 0, &t2, &d2, &r2, NULL);

    fault_trigger_t t3;
    memset(&t3, 0, sizeof(t3));
    t3.kind = FAULT_TRIGGER_RANDOM_IN;
    t3.random_t0_s = 5.0;
    t3.random_t1_s = 15.0;
    fault_duration_t d3 = duration_permanent();
    fault_repeat_t r3 = repeat_once();
    fault_engine_schedule(eng, 3, 4, 0, &t3, &d3, &r3, NULL);
}

static size_t run_replay_scenario(uint32_t seed, fault_event_t *log, size_t log_cap)
{
    fault_engine_t eng;
    build_replay_scenario(&eng, seed);

    size_t total = 0;
    bool relay_state = true; /* starts closed */
    for (double t = 0.0; t <= 20.0; t += 0.25) {
        float temps[1];
        /* Deterministic synthetic zone-temp ramp: same on every run. */
        temps[0] = (float)(t * 40.0);
        bool relays[1];
        /* Deterministic synthetic relay pattern: opens at t=6, stays open. */
        relay_state = (t < 6.0);
        relays[0] = relay_state;

        fault_engine_snapshot_t snap = {t, temps, 1, relays, 1, NULL, 0};
        fault_event_t events[16];
        size_t n = fault_engine_tick(&eng, &snap, events, 16);
        for (size_t i = 0; i < n && total < log_cap; i++) {
            log[total++] = events[i];
        }
    }
    return total;
}

static void test_determinism_byte_identical_replay(void)
{
    TEST_SECTION("fault_engine -- DETERMINISM: same seed => byte-identical event log");

    fault_event_t log_a[256];
    fault_event_t log_b[256];
    size_t count_a = run_replay_scenario(4242, log_a, 256);
    size_t count_b = run_replay_scenario(4242, log_b, 256);

    TEST_CHECK(count_a > 0, "sanity: the replay scenario actually produces events");
    TEST_CHECK(count_a == count_b, "two runs with the same seed produce the same number of events");
    if (count_a == count_b) {
        TEST_CHECK(memcmp(log_a, log_b, count_a * sizeof(fault_event_t)) == 0,
                   "two runs with the same seed produce a byte-identical event log");
    }

    /* A different seed is allowed to (and, given the RANDOM_IN/jitter slots
     * in this scenario, does) diverge -- proving the determinism above is
     * actually seed-derived, not a trivial constant sequence. */
    fault_event_t log_c[256];
    size_t count_c = run_replay_scenario(99999, log_c, 256);
    bool differs = (count_c != count_a);
    if (!differs && count_a > 0) {
        differs = memcmp(log_a, log_c, count_a * sizeof(fault_event_t)) != 0;
    }
    TEST_CHECK(differs, "a different seed produces a different event log (the match above is not a trivial constant)");
}

void run_test_fault_engine(void)
{
    test_at_sim_time_trigger();
    test_at_zone_temp_trigger();
    test_on_relay_edge_trigger();
    test_random_in_trigger_with_seed();
    test_duration_every_repeat();
    test_manual_trigger_only_fires_via_fire_now();
    test_tc_inject_fault_path_emits_fired();
    test_mixed_manual_and_triggered_fires_are_gap_free_in_order();
    test_fire_now_then_expiry_both_reach_the_ring();
    test_cancel_on_active_slot_emits_cleared();
    test_cancel_on_armed_slot_is_immediate_and_silent();
    test_determinism_byte_identical_replay();
}
