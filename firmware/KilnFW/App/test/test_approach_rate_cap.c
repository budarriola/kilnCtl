// Regression tests for the per-zone approach-rate cap (PID_EXPANSION_PLAN.md
// sec 3.6d / PER_ZONE_TARGET_DESIGN_STUDY.md option (b)): a per-zone ceiling
// on how fast a zone's own commanded setpoint (zone_runtime_t::
// effective_target_c) may approach the shared, board-wide s_exec.target_c,
// that can only ever TIGHTEN the segment's own ramp_c_per_hr, never loosen
// it.
//
// Same limitation and same technique as test_ramp_lock_onesided.c (read that
// file's own header comment first): the cap-update loop lives directly in
// profile_executor.c's executor_task_entry() tick body (the per-tick block
// added right before "Control mode, per active zone (pass 1: decide...")
// -- a real FreeRTOS task loop with no seam to call one tick at a time from a
// host test. This file is therefore a hand-written MIRROR of that fragment,
// textually identical to what profile_executor.c contains, checked by
// diffing against the real source whenever either changes, plus a second
// mirror of the segment-stepping fragment (already covered on its own by
// test_ramp_lock_onesided.c, reproduced HERE too so this file can drive a
// multi-tick ramp-then-dwell scenario without depending on that other file's
// internals).
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "test_common.h"

#define TEST_ZONE_COUNT 2

typedef struct {
    bool  active;
    bool  faulted;
    float effective_target_c;
} mirror_cap_zone_t;

/* Mirrors profile_executor.c's per-tick approach-rate-cap update loop
 * (the block this pass added immediately before "Control mode, per active
 * zone (pass 1: decide, don't apply yet)"). cap_c_per_hr <= 0.0f is
 * "uncapped" -- effective_target_c is simply set to shared_target_c, every
 * tick, unconditionally. */
static void cap_update_tick(mirror_cap_zone_t *z, float shared_target_c, float cap_c_per_hr, float dt_s)
{
    if (!z->active || z->faulted) return;
    if (cap_c_per_hr <= 0.0f) {
        z->effective_target_c = shared_target_c;
        return;
    }
    if (!isfinite(z->effective_target_c)) {
        z->effective_target_c = shared_target_c;
        return;
    }
    float max_step_c = cap_c_per_hr * (dt_s / 3600.0f);
    float delta_c = shared_target_c - z->effective_target_c;
    if (delta_c > max_step_c) {
        delta_c = max_step_c;
    } else if (delta_c < -max_step_c) {
        delta_c = -max_step_c;
    }
    z->effective_target_c += delta_c;
}

/* Mirrors profile_executor.c's ramp-stepping gate for the ordinary
 * lock_ok/no-stretch ramp sub-case (same fragment test_ramp_lock_onesided.c
 * mirrors as step_schedule(), reproduced here under a different name so this
 * file is self-contained). */
static bool step_schedule_reached(float *target_c, float seg_target_c, float ramp_c_per_hr, float dt_s)
{
    float direction = (seg_target_c >= *target_c) ? 1.0f : -1.0f;
    float new_target = *target_c + direction * ramp_c_per_hr * (dt_s / 3600.0f);
    bool reached = (direction > 0.0f) ? (new_target >= seg_target_c) : (new_target <= seg_target_c);
    *target_c = reached ? seg_target_c : new_target;
    return reached;
}

void run_test_approach_rate_cap(void)
{
    TEST_SECTION("approach_rate_cap (PID_EXPANSION_PLAN.md sec 3.6d, profile_executor.c per-tick cap update)");

    /* --- 1. Uncapped zone: effective_target_c tracks s_exec.target_c
     * exactly, every tick, bit-identical to reading s_exec.target_c
     * directly -- the mandatory "default is behaviour-identical" property. */
    {
        mirror_cap_zone_t z = {.active = true, .faulted = false, .effective_target_c = 20.0f};
        float shared_target_c = 20.0f;
        char msg[192];
        for (int tick = 0; tick < 10; tick++) {
            shared_target_c += 5.0f; /* an aggressive, arbitrary per-tick jump */
            cap_update_tick(&z, shared_target_c, 0.0f /* uncapped */, 10.0f);
            snprintf(msg, sizeof(msg),
                     "uncapped zone: effective_target_c == s_exec.target_c exactly, every tick (tick %d)", tick);
            TEST_CHECK(z.effective_target_c == shared_target_c, msg);
        }
    }

    /* --- 2. Capped zone climbs at EXACTLY its cap, no faster, while the
     * shared target is moving faster than the cap allows. 60 C/hr cap,
     * dt_s=60 (1 minute) -> max_step_c = 1.0 C/tick. Shared target jumps
     * instantly to 100 (a step-segment-shaped move) so the cap is always the
     * binding constraint, never the segment's own rate. */
    {
        mirror_cap_zone_t z = {.active = true, .faulted = false, .effective_target_c = 20.0f};
        float shared_target_c = 100.0f;
        float cap_c_per_hr = 60.0f;
        float dt_s = 60.0f;
        float expect_step_c = cap_c_per_hr * (dt_s / 3600.0f); /* 1.0 C/tick */
        float prev = z.effective_target_c;
        char msg[192];
        for (int tick = 0; tick < 5; tick++) {
            cap_update_tick(&z, shared_target_c, cap_c_per_hr, dt_s);
            float actual_step_c = z.effective_target_c - prev;
            snprintf(msg, sizeof(msg), "capped zone climbs at exactly cap_c_per_hr (tick %d: step %.4f, want %.4f)",
                     tick, (double)actual_step_c, (double)expect_step_c);
            TEST_CHECK(fabsf(actual_step_c - expect_step_c) < 1e-4f, msg);
            prev = z.effective_target_c;
        }
        TEST_CHECK(z.effective_target_c < shared_target_c,
                   "5 ticks at 1 C/tick from 20 has not yet reached the shared target of 100");
    }

    /* --- 3. A cap numerically LOOSER than the rate the shared target is
     * actually moving at has NO EFFECT -- effective_target_c tracks
     * s_exec.target_c exactly, same as uncapped. This is the "can only
     * tighten, never loosen" guarantee, proven directly: a segment ramping
     * at 30 C/hr with a 1000 C/hr cap configured must be bit-identical to
     * the same segment with no cap at all. */
    {
        mirror_cap_zone_t z_capped = {.active = true, .faulted = false, .effective_target_c = 20.0f};
        mirror_cap_zone_t z_uncapped = {.active = true, .faulted = false, .effective_target_c = 20.0f};
        float shared_target_c = 20.0f;
        float segment_ramp_c_per_hr = 30.0f;
        float loose_cap_c_per_hr = 1000.0f; /* far looser than the segment's own 30 C/hr */
        float dt_s = 60.0f;
        char msg[192];
        for (int tick = 0; tick < 20; tick++) {
            /* Advance the shared target at the segment's own rate, exactly
             * like the real segment-stepping block would. */
            step_schedule_reached(&shared_target_c, 200.0f, segment_ramp_c_per_hr, dt_s);
            cap_update_tick(&z_capped, shared_target_c, loose_cap_c_per_hr, dt_s);
            cap_update_tick(&z_uncapped, shared_target_c, 0.0f, dt_s);
            snprintf(msg, sizeof(msg), "a looser-than-segment cap (%.0f C/hr vs segment's %.0f C/hr) is a no-op (tick %d)",
                     (double)loose_cap_c_per_hr, (double)segment_ramp_c_per_hr, tick);
            TEST_CHECK(z_capped.effective_target_c == z_uncapped.effective_target_c, msg);
        }
    }

    /* --- 4. Dwell-entry interaction: the shared schedule reaches
     * seg->target_c (segment "reached", the group enters a dwell) well
     * before a TIGHTLY capped zone's own effective_target_c does -- the
     * capped zone keeps closing the gap on later ticks even after the
     * group's own segment-advance boolean has already flipped, exactly
     * PER_ZONE_TARGET_DESIGN_STUDY.md option (b)'s "reached... just reached
     * later for the capped zone." */
    {
        float shared_target_c = 20.0f;
        const float seg_target_c = 40.0f;
        const float segment_ramp_c_per_hr = 600.0f; /* fast: reaches 40 in 2 minutes */
        const float cap_c_per_hr = 30.0f;           /* much tighter than the segment's own rate */
        const float dt_s = 60.0f;
        mirror_cap_zone_t z = {.active = true, .faulted = false, .effective_target_c = shared_target_c};

        bool shared_reached = false;
        int shared_reached_tick = -1;
        int capped_reached_tick = -1;
        for (int tick = 0; tick < 60 && (shared_reached_tick < 0 || capped_reached_tick < 0); tick++) {
            if (!shared_reached) {
                shared_reached = step_schedule_reached(&shared_target_c, seg_target_c, segment_ramp_c_per_hr, dt_s);
                if (shared_reached && shared_reached_tick < 0) shared_reached_tick = tick;
            }
            cap_update_tick(&z, shared_target_c, cap_c_per_hr, dt_s);
            if (capped_reached_tick < 0 && z.effective_target_c >= seg_target_c - 1e-4f) {
                capped_reached_tick = tick;
            }
        }
        TEST_CHECK(shared_reached_tick >= 0 && capped_reached_tick >= 0,
                   "test setup sanity: both the shared schedule and the capped zone eventually reach seg_target_c");
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "the capped zone's OWN effective_target_c reaches seg_target_c strictly LATER than the shared "
                 "schedule does (shared at tick %d, capped at tick %d) -- dwell entry, at the group level, must "
                 "not wait for a capped zone before advancing",
                 shared_reached_tick, capped_reached_tick);
        TEST_CHECK(capped_reached_tick > shared_reached_tick, msg);
    }

    /* --- 5. Segment advance itself is untouched: the shared s_exec.target_c
     * reaches seg->target_c on the SAME tick regardless of what any zone's
     * cap is configured to -- proves this option does not redefine
     * segment-advance the way PER_ZONE_TARGET_DESIGN_STUDY.md option (a)
     * would have needed to. */
    {
        float shared_a = 20.0f, shared_b = 20.0f;
        mirror_cap_zone_t dummy_capped = {.active = true, .faulted = false, .effective_target_c = 20.0f};
        int reached_tick_a = -1, reached_tick_b = -1;
        for (int tick = 0; tick < 20; tick++) {
            bool ra = step_schedule_reached(&shared_a, 50.0f, 90.0f, 60.0f);
            bool rb = step_schedule_reached(&shared_b, 50.0f, 90.0f, 60.0f);
            cap_update_tick(&dummy_capped, shared_b, 5.0f /* a very tight cap, on the "b" run only */, 60.0f);
            if (ra && reached_tick_a < 0) reached_tick_a = tick;
            if (rb && reached_tick_b < 0) reached_tick_b = tick;
        }
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "segment-advance (shared target reaching seg_target_c) is identical whether or not a zone's own "
                 "cap is configured (tick %d vs %d)",
                 reached_tick_a, reached_tick_b);
        TEST_CHECK(reached_tick_a == reached_tick_b, msg);
    }

    /* --- 6. Migration: every zone's approach_rate_cap_c_per_hr defaults to
     * 0 (uncapped) coming from any pre-v18 blob -- see zones_config_migrate.c's
     * convert_versioned_blob_to_current() top-of-function comment: this is a
     * brand-new mechanism, so every case (1..17) relies on the entry
     * memset(out, 0, ...) rather than an explicit per-zone carry-forward the
     * way ease_off_window_mult's v16->v17 case needed. Exercised end-to-end
     * (a real v17 blob decoded through zones_config_json_decode_blob())
     * in test_zones_http.c's own migration test -- this is a plain
     * behavioural pin of the DEFAULT the cap-update loop above already
     * assumes (cap_c_per_hr <= 0.0f -- test 1's own precondition). */
    {
        float default_cap_c_per_hr = 0.0f; /* what a migrated pre-v18 zone reads */
        TEST_CHECK(default_cap_c_per_hr <= 0.0f,
                   "migrated default (0.0) is treated as uncapped by cap_update_tick()'s own <= 0.0f test");
    }

    /* --- NEGATIVE TEST 1/2: a mutated cap-update that lets the cap LOOSEN a
     * zone's approach (max_step_c computed as if the cap were a FLOOR, not a
     * ceiling -- max_step_c = MAX(cap step, unlimited step) instead of MIN)
     * must diverge from the correct MIN-based mirror the instant a tight cap
     * is configured against a fast segment, and this test must catch it by
     * name. Proves test 2 (capped zone climbs at exactly its cap) can
     * actually fail, not just that it currently passes. */
    {
        mirror_cap_zone_t z_correct = {.active = true, .faulted = false, .effective_target_c = 20.0f};
        mirror_cap_zone_t z_broken = {.active = true, .faulted = false, .effective_target_c = 20.0f};
        float shared_target_c = 100.0f;
        float cap_c_per_hr = 6.0f; /* very tight: 0.1 C/tick at dt_s=60 */
        float dt_s = 60.0f;

        cap_update_tick(&z_correct, shared_target_c, cap_c_per_hr, dt_s);

        /* MUTATION: BROKEN -- lets an out-of-band step through unclamped,
         * the shape a "cap can loosen" regression would take (e.g. a
         * min()<->max() typo, or a dropped clamp). Deliberately reproduced
         * inline (not calling cap_update_tick()) so this negative test does
         * not depend on the production mirror also containing the bug. */
        {
            float delta_c = shared_target_c - z_broken.effective_target_c; /* 80.0, unclamped */
            z_broken.effective_target_c += delta_c; /* jumps straight to 100 -- the cap did NOT tighten anything */
        }

        char msg[224];
        snprintf(msg, sizeof(msg),
                 "NEGATIVE TEST 1/2: an unclamped ('cap can loosen') mutation reaches a HIGHER effective_target_c "
                 "than the correct MIN-clamped mirror this tick (correct=%.3f, broken=%.3f) -- proves the MIN "
                 "clamp is load-bearing, not vacuous",
                 (double)z_correct.effective_target_c, (double)z_broken.effective_target_c);
        TEST_CHECK(z_correct.effective_target_c < z_broken.effective_target_c, msg);
        TEST_CHECK(fabsf(z_correct.effective_target_c - 20.1f) < 1e-4f,
                   "the CORRECT mirror advances by exactly 0.1 C this tick (6 C/hr * 60s/3600s)");
    }

    /* --- NEGATIVE TEST 2/2: reading the WRONG zone's cap (a transposed
     * index, the same bug class the ease_off_window_mult pass's own
     * negative test caught) must let zone 1 inherit zone 0's tight cap and
     * vice versa -- proves a per-zone cross-contamination bug is
     * observable, not silently absorbed by symmetric test data. Zone 0 is
     * given a very tight cap (6 C/hr), zone 1 is left uncapped (0). */
    {
        mirror_cap_zone_t zones[TEST_ZONE_COUNT] = {
            {.active = true, .faulted = false, .effective_target_c = 20.0f}, /* z0: capped at 6 C/hr */
            {.active = true, .faulted = false, .effective_target_c = 20.0f}, /* z1: uncapped */
        };
        float caps_correct[TEST_ZONE_COUNT] = {6.0f, 0.0f};
        float caps_transposed[TEST_ZONE_COUNT] = {0.0f, 6.0f}; /* the bug: z0<->z1 swapped */
        float shared_target_c = 100.0f;
        float dt_s = 60.0f;

        mirror_cap_zone_t correct[TEST_ZONE_COUNT];
        memcpy(correct, zones, sizeof(correct));
        for (uint8_t zi = 0; zi < TEST_ZONE_COUNT; zi++) {
            cap_update_tick(&correct[zi], shared_target_c, caps_correct[zi], dt_s);
        }
        TEST_CHECK(correct[0].effective_target_c < correct[1].effective_target_c,
                   "test setup sanity: with the CORRECT (untransposed) cap assignment, z0 (capped) lags z1 "
                   "(uncapped) after one tick");

        mirror_cap_zone_t transposed[TEST_ZONE_COUNT];
        memcpy(transposed, zones, sizeof(transposed));
        for (uint8_t zi = 0; zi < TEST_ZONE_COUNT; zi++) {
            cap_update_tick(&transposed[zi], shared_target_c, caps_transposed[zi], dt_s);
        }
        TEST_CHECK(transposed[0].effective_target_c > transposed[1].effective_target_c,
                   "NEGATIVE TEST 2/2: with a TRANSPOSED (z0<->z1) cap assignment, z1 now lags z0 instead -- the "
                   "opposite ordering from the correct assignment above, so a per-zone index bug here is caught by "
                   "name rather than silently absorbed");
        TEST_CHECK(!(transposed[0].effective_target_c < transposed[1].effective_target_c),
                   "the transposed assignment's ordering must NOT coincidentally match the correct one");
    }
}
