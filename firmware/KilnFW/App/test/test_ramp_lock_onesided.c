// Reproduction + regression test for the ramp-lock hot-start stall defect
// (owner-confirmed, adversarial review) and its fix, both in profile_
// executor.c's control tick:
//   - the lock decision loop building lock_ok/lagging (anchored on the
//     one-sided condition itself, not a line number -- see
//     ramp_lock_decision_mirror_drift_check.py)
//   - the ramp-stepping gate (`} else if (lock_ok || stretched_this_tick) {
//     s_exec.segment_elapsed_s += ...; ... }` -- PID_EXPANSION_PLAN.md sec
//     7.2's auto-stretch branch, added 2026-09-03)
//
// Both live directly in executor_task_entry()'s `for (;;) { vTaskDelay(...);
// ... }` body -- a real FreeRTOS task loop that would spin forever if called
// from a host test, and profile_executor.c's own test file (test_profile_
// executor_prestart.c) documents this as "the honest limit of what this
// harness can prove" for exactly that reason: there is no seam to call just
// one tick's worth of the control loop without restructuring the module,
// which this task was not asked to do.
//
// This file therefore does what test_closed_loop.c already does for
// pid_fuzzy_prepare_gains() (another `static`/inline-only piece of
// profile_executor.c): a hand-written MIRROR of the two code fragments
// above, textually identical to what profile_executor.c contains (checked
// by ramp_lock_decision_mirror_drift_check.py and
// ramp_stepping_gate_mirror_drift_check.py, both anchored on distinctive
// text rather than line numbers) rather than a call into the production
// functions themselves. A future edit to either fragment in
// profile_executor.c that isn't mirrored here would silently diverge from
// what this file tests -- same caveat test_closed_loop.c states for its own
// mirror. This file's two mirrored helpers are named lock_lagging_mask()
// and step_schedule().
//
// step_schedule() covers only the ZONE_RAMP ramp sub-case of the gate
// (`else if (lock_ok || stretched_this_tick) { ...; if (!s_exec.dwelling)
// { <ramp math> } }`) -- the io_blocking/relay-segment branch, the
// already-dwelling `else` branch (dwell-credit accounting), and the
// dwelling-ENTRY bookkeeping inside the ramp branch (setting
// s_exec.dwelling = true and spending dwell credit once target_c reaches
// seg->target_c) are all out of scope: none of that state is exercised by
// what this file's tests check (whether the schedule advances at all under
// a one-sided lock), and reconstructing it would need s_exec itself. The
// auto-stretch RATE decision (ramp_assist_stretch_rate_c_per_s(), which
// reads sustained-lag bookkeeping this host test cannot construct) is
// likewise not reproduced -- stretched_this_tick and stretch_rate_c_per_s
// are accepted as caller-supplied parameters, exactly as lock_ok already
// was, and every test in this file passes stretched_this_tick=false /
// stretch_rate_c_per_s=-1.0f (production's own "no stretch" sentinel), so
// the gate's behavior in every test below is identical to before sec 7.2
// existed. Only the gate's own shape -- the `|| stretched_this_tick`
// disjunct and the stretch-rate substitution when it fires -- is mirrored.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "test_common.h"

#define TEST_ZONE_COUNT 2
#define TEST_RAMP_LOCK_BAND_C 25.0f /* EXEC_RAMP_LOCK_BAND_C's default, PROFILE_EXECUTOR_RAMP_LOCK_BAND_C
                                     * (profile_executor.h) -- the owner's correction: this is 25C, not
                                     * PROGRESS_BAND_C's 3C, which several docs/the simulator have wrong. */

typedef struct {
    bool  active;
    bool  faulted;
    bool  sensor_ok;
    float actual_c;
    bool  monitor_only; /* SPARE_RELAY_ONOFF_PLAN.md sec 10: zone_runtime_t.monitor_only */
} mirror_zone_t;

/* docs/ON_OFF_ZONE_PLAN.md sec 1: stand-in for the real zones_config_
 * accessors.c predicate of the same name (this file cannot link that module
 * without pulling in all of NVS -- same reasoning sensor_ok/actual_c are
 * caller-supplied fields on mirror_zone_t rather than real accessor calls).
 * Defaults false (every existing test in this file leaves it untouched) so
 * this is bit-identical to before this predicate existed. */
static bool s_mirror_on_off[TEST_ZONE_COUNT];
static bool zone_is_on_off(uint8_t zone_index)
{
    return s_mirror_on_off[zone_index];
}

/* Stand-in for profile_executor_zone_drives_run() (neither on/off nor
 * monitor-only), the one shared predicate the production loop calls. The
 * mirrored loop publishes its zone array through s_mirror_zones so this
 * stand-in can read monitor_only the way production reads the live zone. */
static const mirror_zone_t *s_mirror_zones;
static bool profile_executor_zone_drives_run(uint8_t zi)
{
    return !zone_is_on_off(zi) && !s_mirror_zones[zi].monitor_only;
}

/* Mirrors profile_executor.c's per-tick lock_ok/lagging loop (anchored on
 * the one-sided condition itself, not a line number -- see
 * ramp_lock_decision_mirror_drift_check.py). old_fabsf selects the PRE-FIX formula
 * (bit-identical hot/cold via fabsf) vs the FIX (one-sided: only a zone
 * COLDER than target by more than the band holds the lock). */
static uint8_t lock_lagging_mask(const mirror_zone_t zones[TEST_ZONE_COUNT], float target_c, bool old_fabsf)
{
    uint8_t lagging = 0;
    s_mirror_zones = zones;
    for (uint8_t zi = 0; zi < TEST_ZONE_COUNT; zi++) {
        if (!zones[zi].active || zones[zi].faulted) continue;
        if (!profile_executor_zone_drives_run(zi)) continue;
        bool held;
        if (old_fabsf) {
            held = !zones[zi].sensor_ok || fabsf(zones[zi].actual_c - target_c) > TEST_RAMP_LOCK_BAND_C;
        } else {
            held = !zones[zi].sensor_ok || (target_c - zones[zi].actual_c) > TEST_RAMP_LOCK_BAND_C;
        }
        if (held) lagging |= (uint8_t)(1u << zi);
    }
    return lagging;
}

/* Mirrors profile_executor.c's ramp-stepping gate, ZONE_RAMP ramp sub-case
 * only (`} else if (lock_ok || stretched_this_tick) { s_exec.segment_
 * elapsed_s += ...; if (!s_exec.dwelling) { <ramp math> } }` --
 * PID_EXPANSION_PLAN.md sec 7.2). The io_blocking/relay-segment branch, the
 * already-dwelling branch, and the dwelling-entry bookkeeping are out of
 * scope -- see this file's header comment. Advances segment_elapsed_s and
 * steps target_c toward seg_target_c, but ONLY when lock_ok OR
 * stretched_this_tick (production's own disjunct); the rate used is
 * stretch_rate_c_per_s (converted to C/hr) while stretched_this_tick,
 * ramp_c_per_hr otherwise -- bit-identical to before sec 7.2 whenever
 * stretched_this_tick is false. */
static void step_schedule(float *target_c, uint32_t *segment_elapsed_s, float seg_target_c,
                          float ramp_c_per_hr, float dt_s, bool lock_ok,
                          bool stretched_this_tick, float stretch_rate_c_per_s)
{
    if (!lock_ok && !stretched_this_tick) {
        return; /* the lock -- schedule frozen exactly as profile_executor.c freezes it */
    }
    *segment_elapsed_s += (uint32_t)(dt_s + 0.5f);
    float rate_c_per_hr = stretched_this_tick ? (stretch_rate_c_per_s * 3600.0f) : ramp_c_per_hr;
    float direction = (seg_target_c >= *target_c) ? 1.0f : -1.0f;
    float new_target = *target_c + direction * rate_c_per_hr * (dt_s / 3600.0f);
    bool reached = (direction > 0.0f) ? (new_target >= seg_target_c) : (new_target <= seg_target_c);
    *target_c = reached ? seg_target_c : new_target;
}

void run_test_ramp_lock_onesided(void)
{
    TEST_SECTION("ramp_lock_onesided (profile_executor.c hot-start defect + fix)");

    /* --- REPRODUCTION: the defect, as it stands before the fix ------------
     * z0 at the baseline (matches target, not lagging). z1 hot-started 50C
     * ABOVE target (the owner's exact scenario: re-fire 40 min after a
     * previous run, one zone still hot). Only passive cooling -- 0.02C/tick,
     * far too slow to close 50C in any test-sized window -- moves it, same
     * as a real kiln with no active cooling. Under the OLD fabsf formula
     * this must stall the WHOLE schedule (both target_c and
     * segment_elapsed_s frozen) for the entire run, because z1 alone keeps
     * holding lock_ok false forever. */
    {
        mirror_zone_t zones[TEST_ZONE_COUNT] = {
            {.active = true, .faulted = false, .sensor_ok = true, .actual_c = 45.0f}, /* z0: cold baseline, at target */
            {.active = true, .faulted = false, .sensor_ok = true, .actual_c = 95.0f}, /* z1: hot start, 50C above target */
        };
        float target_c = 45.0f;
        uint32_t segment_elapsed_s = 0;
        const float seg_target_c = 200.0f;
        const float ramp_c_per_hr = 600.0f; /* 10C/min -- fast on purpose so a real advance would be obvious quickly */
        const float dt_s = 10.0f;

        for (int i = 0; i < 500; i++) {
            uint8_t lagging = lock_lagging_mask(zones, target_c, /*old_fabsf=*/true);
            bool lock_ok = (lagging == 0);
            step_schedule(&target_c, &segment_elapsed_s, seg_target_c, ramp_c_per_hr, dt_s, lock_ok, /*stretched_this_tick=*/false, /*stretch_rate_c_per_s=*/-1.0f);
            zones[1].actual_c -= 0.02f; /* passive cooling only, ~50 min to close 50C at this rate */
        }

        char detail[160];
        snprintf(detail, sizeof(detail),
                "PRE-FIX (fabsf) reproduction: target_c=%.2f (started 45.00), segment_elapsed_s=%u after 500 ticks (5000s)",
                (double)target_c, (unsigned)segment_elapsed_s);
        TEST_CHECK(target_c == 45.0f, detail);
        TEST_CHECK(segment_elapsed_s == 0, "schedule wall-clock frozen too -- this IS the stall, not just a slow ramp");
    }

    /* --- THE FIX: the identical hot-start scenario, one-sided formula ----- */
    {
        mirror_zone_t zones[TEST_ZONE_COUNT] = {
            {.active = true, .faulted = false, .sensor_ok = true, .actual_c = 45.0f},
            {.active = true, .faulted = false, .sensor_ok = true, .actual_c = 95.0f},
        };
        float target_c = 45.0f;
        uint32_t segment_elapsed_s = 0;
        const float seg_target_c = 200.0f;
        const float ramp_c_per_hr = 600.0f;
        const float dt_s = 10.0f;

        /* Only 20 ticks (200s), not 500: z1 here is a toy model that only
         * ever passively cools (real hardware would turn a zone that falls
         * below the (rising) target back into an actively-heating one,
         * which this simple mirror does not simulate) -- run long enough
         * to the ramp outrun z1's slow cooling and correctly re-engage the
         * lock as a genuine cold-lag case (physically right: z1 really is
         * behind a target that outpaced it), the same as the flipped-cold
         * scenario below proves in isolation. The claim this block makes is
         * narrower and unambiguous: the lock does NOT hold immediately/
         * permanently at the hot start the way the pre-fix formula does. */
        for (int i = 0; i < 20; i++) {
            uint8_t lagging = lock_lagging_mask(zones, target_c, /*old_fabsf=*/false);
            bool lock_ok = (lagging == 0);
            step_schedule(&target_c, &segment_elapsed_s, seg_target_c, ramp_c_per_hr, dt_s, lock_ok, /*stretched_this_tick=*/false, /*stretch_rate_c_per_s=*/-1.0f);
            zones[1].actual_c -= 0.02f;
            zones[0].actual_c = target_c; /* z0 tracks the setpoint perfectly -- keeps it a non-issue zone
                                           * throughout, isolating the assertion to z1's hot-start behavior
                                           * (without this it would itself start lagging COLD once the ramp
                                           * carries target_c more than 25C past its frozen 45.0). */
        }

        char detail[160];
        snprintf(detail, sizeof(detail), "one-sided fix: target_c=%.2f after 20 ticks (started 45.00, hot zone still cooling toward %.2f)",
                (double)target_c, (double)zones[1].actual_c);
        TEST_CHECK(target_c > 70.0f, detail); /* well past the old formula's permanent freeze at 45.0 */
        TEST_CHECK(segment_elapsed_s == 200, "schedule wall-clock advances every tick once the hot zone no longer holds the lock");
    }

    /* --- PROVE THE LOCK'S REAL PURPOSE SURVIVES: a genuinely COLD-lagging
     * zone must still hold the lock under the one-sided formula, exactly as
     * it did before -- only the HOT direction changed. z1 here starts 50C
     * BELOW target (mirrors the original defect's shape but flipped) and
     * heats slowly (0.02C/tick, same magnitude as the hot zone's passive
     * cooling above) -- nowhere near enough to track a 10C/min ramp, so it
     * should remain the lagging zone and the schedule should stay frozen. */
    {
        mirror_zone_t zones[TEST_ZONE_COUNT] = {
            {.active = true, .faulted = false, .sensor_ok = true, .actual_c = 45.0f},
            {.active = true, .faulted = false, .sensor_ok = true, .actual_c = -5.0f}, /* z1: 50C COLD start */
        };
        float target_c = 45.0f;
        uint32_t segment_elapsed_s = 0;
        const float seg_target_c = 200.0f;
        const float ramp_c_per_hr = 600.0f;
        const float dt_s = 10.0f;

        for (int i = 0; i < 500; i++) {
            uint8_t lagging = lock_lagging_mask(zones, target_c, /*old_fabsf=*/false);
            bool lock_ok = (lagging == 0);
            step_schedule(&target_c, &segment_elapsed_s, seg_target_c, ramp_c_per_hr, dt_s, lock_ok, /*stretched_this_tick=*/false, /*stretch_rate_c_per_s=*/-1.0f);
            zones[1].actual_c += 0.02f; /* slow but real heating -- still can't keep up with a 10C/min commanded ramp */
        }

        char detail[160];
        snprintf(detail, sizeof(detail),
                "cold-lagging zone still holds the lock under the one-sided fix: target_c=%.2f (started 45.00)",
                (double)target_c);
        TEST_CHECK(target_c == 45.0f, detail);
        TEST_CHECK(segment_elapsed_s == 0, "a genuinely cold-lagging zone freezes the schedule exactly as before -- "
                                            "the fix is direction-selective, not a removal of the lock");
    }

    /* --- NEGATIVE-TEST SEAM: an invalid sensor must still hold the lock,
     * regardless of formula (the `!sensor_ok[zi] ||` clause the fix left
     * untouched). Same lagging-zone-holds-schedule assertion as above, but
     * driven by sensor_ok=false on an otherwise ON-TARGET zone that the
     * one-sided formula alone would never flag. */
    {
        mirror_zone_t zones[TEST_ZONE_COUNT] = {
            {.active = true, .faulted = false, .sensor_ok = true, .actual_c = 45.0f},
            {.active = true, .faulted = false, .sensor_ok = false, .actual_c = 45.0f}, /* z1: on target but sensor invalid */
        };
        float target_c = 45.0f;
        uint32_t segment_elapsed_s = 0;
        for (int i = 0; i < 10; i++) {
            uint8_t lagging = lock_lagging_mask(zones, target_c, /*old_fabsf=*/false);
            bool lock_ok = (lagging == 0);
            step_schedule(&target_c, &segment_elapsed_s, 200.0f, 600.0f, 10.0f, lock_ok, /*stretched_this_tick=*/false, /*stretch_rate_c_per_s=*/-1.0f);
        }
        TEST_CHECK(segment_elapsed_s == 0, "an invalid sensor holds the lock under the fix, same as before -- "
                                            "the !sensor_ok clause was left exactly as-is");
    }

    /* docs/ON_OFF_ZONE_PLAN.md sec 1: an on/off zone sitting at ambient (or
     * with no thermocouple at all -- sensor_ok=false here models that) with
     * an active, real ramp lagging must NOT freeze the schedule. z1 is on/
     * off and would hold the lock forever under the old rule (invalid
     * sensor); z0 is a real, healthy heater tracking target exactly. */
    {
        mirror_zone_t zones[TEST_ZONE_COUNT] = {
            {.active = true, .faulted = false, .sensor_ok = true, .actual_c = 45.0f}, /* z0: healthy heater, on target */
            {.active = true, .faulted = false, .sensor_ok = false, .actual_c = 20.0f}, /* z1: on/off, no TC, ambient */
        };
        s_mirror_on_off[0] = false;
        s_mirror_on_off[1] = true;
        float target_c = 45.0f;
        uint32_t segment_elapsed_s = 0;
        for (int i = 0; i < 10; i++) {
            uint8_t lagging = lock_lagging_mask(zones, target_c, /*old_fabsf=*/false);
            bool lock_ok = (lagging == 0);
            step_schedule(&target_c, &segment_elapsed_s, 200.0f, 600.0f, 10.0f, lock_ok, /*stretched_this_tick=*/false, /*stretch_rate_c_per_s=*/-1.0f);
        }
        TEST_CHECK(segment_elapsed_s > 0, "an on/off zone (even with sensor_ok=false / no TC) never holds the "
                                          "ramp lock -- only the real heater zone's own state matters");
        s_mirror_on_off[0] = false;
        s_mirror_on_off[1] = false; /* reset for any test added after this one */
    }

    /* docs/SPARE_RELAY_ONOFF_PLAN.md sec 10: a monitor-only zone (relay
     * converted to an aux) cannot heat, so sitting 100 C cold must not hold
     * the ramp lock. Control: the identical zone as a heater DOES hold it. */
    {
        for (int monitor_only = 0; monitor_only < 2; monitor_only++) {
            mirror_zone_t zones[TEST_ZONE_COUNT] = {
                {.active = true, .faulted = false, .sensor_ok = true, .actual_c = 45.0f},
                {.active = true, .faulted = false, .sensor_ok = true, .actual_c = -55.0f, .monitor_only = (monitor_only != 0)},
            };
            float target_c = 45.0f;
            uint32_t segment_elapsed_s = 0;
            for (int i = 0; i < 10; i++) {
                uint8_t lagging = lock_lagging_mask(zones, target_c, /*old_fabsf=*/false);
                bool lock_ok = (lagging == 0);
                step_schedule(&target_c, &segment_elapsed_s, 200.0f, 600.0f, 10.0f, lock_ok, /*stretched_this_tick=*/false, /*stretch_rate_c_per_s=*/-1.0f);
            }
            if (monitor_only) {
                TEST_CHECK(segment_elapsed_s > 0, "a cold monitor-only zone must NOT hold the ramp lock");
            } else {
                TEST_CHECK(segment_elapsed_s == 0, "the same cold zone as a HEATER holds the lock (control case)");
            }
        }
    }
}
