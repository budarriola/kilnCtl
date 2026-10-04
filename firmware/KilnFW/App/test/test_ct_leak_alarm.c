/* Host tests for ct_leak_alarm.c -- the H9 CT alarm (docs/SAFETY_CASE.md H9). */

#include <math.h>
#include <string.h>

#include "test_common.h"
#include "../drivers/safety/ct_leak_alarm.h"
#include "../drivers/control/ct_noise_floor.h"
#include "../drivers/http/readiness_http.h"

static ct_leak_alarm_input_t base_in(uint32_t now)
{
    ct_leak_alarm_input_t in;
    memset(&in, 0, sizeof(in));
    in.now_ms = now;
    in.link_up = true;
    in.ct_installed = true;
    in.summed = false;
    in.activity = false;
    in.relays_off_ms = 60000u;
    for (int i = 0; i < CT_LEAK_CHANNELS; i++) {
        in.k_ct_v_per_a[i] = 1.0f;
    }
    return in;
}

/* Drive `ticks` samples 500 ms apart starting at *t; returns true if any raised. */
static bool drive(ct_leak_alarm_state_t *s, ct_leak_alarm_input_t *in, uint32_t *t, uint32_t span_ms)
{
    bool raised = false;
    uint32_t end = *t + span_ms;
    while (*t < end) {
        in->now_ms = *t;
        raised |= ct_leak_alarm_tick(s, in);
        *t += 500;
    }
    return raised;
}

void run_test_ct_leak_alarm(void)
{
    TEST_SECTION("ct_leak_alarm -- H9 CT current with every relay off");
    ct_leak_alarm_state_t s;
    ct_leak_alarm_input_t in;
    uint32_t t;

    /* Shared floor, not a mirror. */
    TEST_CHECK(fabsf(ct_noise_floor_a(1.0f) - ZONE_SWEEP_NORMAL_NOISE_FLOOR_A) < 1e-6f, "floor at ref k_ct");
    TEST_CHECK(fabsf(ct_noise_floor_a(0.5f) - 2.0f * ZONE_SWEEP_NORMAL_NOISE_FLOOR_A) < 1e-6f, "floor rescales by k_ct");
    TEST_CHECK(ct_noise_floor_a(0.0f) == ZONE_SWEEP_NORMAL_NOISE_FLOOR_A, "unknown k_ct falls back");

    /* Quiet channels never alarm. */
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    t = 0;
    TEST_CHECK(!drive(&s, &in, &t, 60000), "no current: no alarm");
    TEST_CHECK(!s.alarm, "state clear");

    /* Sustained current raises exactly once, only after the debounce. */
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    in.current_a[0] = 0.20f;
    t = 0;
    TEST_CHECK(!drive(&s, &in, &t, CT_LEAK_ASSERT_MS), "not raised before the 10 s debounce");
    TEST_CHECK(!s.alarm, "still clear just before debounce");
    in.now_ms = t;
    TEST_CHECK(ct_leak_alarm_tick(&s, &in), "raises at the debounce");
    t += 500;
    TEST_CHECK(s.alarm && s.channel_mask == 1u, "latched, ch1 flagged");
    in.now_ms = t;
    TEST_CHECK(!ct_leak_alarm_tick(&s, &in), "raise reported once");

    /* publish / describe / is_active */
    ct_leak_alarm_publish(&s);
    TEST_CHECK(ct_leak_alarm_is_active(), "published active");
    char txt[96];
    ct_leak_alarm_describe(txt, sizeof(txt));
    TEST_CHECK(strstr(txt, "ch1") != NULL && strstr(txt, "0.20") != NULL, "describe names channel and peak");
    TEST_CHECK(strchr(txt, '"') == NULL && strchr(txt, '\\') == NULL, "describe is JSON-safe");
    TEST_CHECK(readiness_ct_leak_alarm_status(true) == READY_NOT_DONE, "active reads not_done");
    TEST_CHECK(readiness_ct_leak_alarm_status(false) == READY_OK, "clear reads ok");

    /* Latch survives an unevaluable stretch (a relay turns on) and a link drop. */
    in.relays_off_ms = UINT32_MAX;
    t += 500; in.now_ms = t;
    ct_leak_alarm_tick(&s, &in);
    TEST_CHECK(s.alarm, "relay on does not clear the latch");
    in.relays_off_ms = 60000u;
    in.link_up = false;
    in.current_a[0] = 0.0f;
    drive(&s, &in, &t, 60000);
    TEST_CHECK(s.alarm, "link down holds the latch");
    in.link_up = true;

    /* Clears only after 30 s below the floor. */
    t += 500;
    drive(&s, &in, &t, CT_LEAK_CLEAR_MS - 1000);
    TEST_CHECK(s.alarm, "still latched before the 30 s quiet period");
    drive(&s, &in, &t, 2000);
    TEST_CHECK(!s.alarm, "cleared after 30 s quiet");
    ct_leak_alarm_publish(&s);
    TEST_CHECK(!ct_leak_alarm_is_active(), "published clear");
    ct_leak_alarm_describe(txt, sizeof(txt));
    TEST_CHECK(txt[0] == '\0', "describe empty when clear");

    /* Settle: current right after relay release (decay) never counts. */
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    in.current_a[1] = 0.50f;
    in.relays_off_ms = 0;
    t = 0;
    bool raised = false;
    for (uint32_t i = 0; i < 40; i++) { /* 20 s of wall time, relays_off_ms tracks it */
        in.relays_off_ms = i * 500u;
        in.now_ms = t;
        raised |= ct_leak_alarm_tick(&s, &in);
        t += 500;
    }
    /* evaluable from 5 s; 10 s debounce -> would raise at ~15 s of wall time. */
    TEST_CHECK(raised, "persistent current after settle does raise");
    ct_leak_alarm_reset(&s);
    t = 0;
    raised = false;
    for (uint32_t i = 0; i < 24; i++) { /* only 12 s since release: inside settle+debounce */
        in.relays_off_ms = i * 500u;
        in.now_ms = t;
        raised |= ct_leak_alarm_tick(&s, &in);
        t += 500;
    }
    TEST_CHECK(!raised, "current inside settle+debounce of a release does not raise");

    /* Any relay on, or profile/autotune/sweep activity: never evaluated. */
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    in.current_a[0] = 1.0f;
    in.relays_off_ms = UINT32_MAX;
    t = 0;
    TEST_CHECK(!drive(&s, &in, &t, 60000), "relay on: no alarm");
    in.relays_off_ms = 60000u;
    in.activity = true;
    TEST_CHECK(!drive(&s, &in, &t, 60000), "activity: no alarm");
    in.activity = false;
    TEST_CHECK(drive(&s, &in, &t, 20000), "same current raises once idle with relays off");

    /* ct_installed=0 resets and never alarms. */
    in.ct_installed = false;
    in.now_ms = t;
    ct_leak_alarm_tick(&s, &in);
    TEST_CHECK(!s.alarm, "ct_installed=0 clears a standing alarm");
    t = 0;
    ct_leak_alarm_reset(&s);
    TEST_CHECK(!drive(&s, &in, &t, 60000), "ct_installed=0 never alarms");

    /* Topology: summed reads only channel index 2; per-zone reads all. */
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    in.summed = true;
    in.current_a[0] = 1.0f;
    in.current_a[1] = 1.0f;
    t = 0;
    TEST_CHECK(!drive(&s, &in, &t, 60000), "summed: unfitted channels 0/1 ignored");
    in.current_a[2] = 0.30f;
    TEST_CHECK(drive(&s, &in, &t, 20000) && s.channel_mask == 4u, "summed: channel 2 alarms");
    ct_leak_alarm_reset(&s);
    in.summed = false;
    in.current_a[2] = 0.0f;
    t = 0;
    TEST_CHECK(drive(&s, &in, &t, 20000) && s.channel_mask == 3u, "per-zone: channels 0 and 1 alarm");

    /* Floor: at/below it never trips; rescaled by live k_ct. */
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    in.current_a[0] = 0.04f;
    t = 0;
    TEST_CHECK(!drive(&s, &in, &t, 60000), "reading under the floor is quiet");
    in.k_ct_v_per_a[0] = 0.5f; /* floor doubles to 0.09 A */
    in.current_a[0] = 0.08f;
    TEST_CHECK(!drive(&s, &in, &t, 60000), "rescaled floor respected");
    in.current_a[0] = 0.10f;
    TEST_CHECK(drive(&s, &in, &t, 20000), "above the rescaled floor raises");

    /* A very long relays-off stretch (kiln_io saturates at UINT32_MAX - 1) still evaluates. */
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    in.relays_off_ms = UINT32_MAX - 1u;
    in.current_a[1] = 0.2f;
    t = 0;
    TEST_CHECK(drive(&s, &in, &t, 20000), "a saturated (49.7 day) off-time still evaluates and raises");

    /* NaN is unknown and ignored; +/-inf with every relay off is alarm-worthy. */
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    in.current_a[0] = NAN;
    t = 0;
    TEST_CHECK(!drive(&s, &in, &t, 60000), "NaN ignored");
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    in.current_a[1] = INFINITY;
    t = 0;
    TEST_CHECK(drive(&s, &in, &t, 20000), "an infinite reading with every relay off raises");
    TEST_CHECK(isfinite(s.peak_a) && s.peak_a > 0.0f, "the published peak stays finite");
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    in.current_a[1] = -INFINITY;
    t = 0;
    TEST_CHECK(drive(&s, &in, &t, 20000), "a negative-infinite reading raises too");

    /* A dip below the floor restarts the debounce. */
    ct_leak_alarm_reset(&s);
    in = base_in(0);
    t = 0;
    in.current_a[0] = 0.2f;
    drive(&s, &in, &t, 8000);
    in.current_a[0] = 0.0f;
    drive(&s, &in, &t, 1000);
    in.current_a[0] = 0.2f;
    TEST_CHECK(!drive(&s, &in, &t, 8000), "a quiet sample restarts the 10 s debounce");

    /* NULL safety. */
    TEST_CHECK(!ct_leak_alarm_tick(NULL, &in) && !ct_leak_alarm_tick(&s, NULL), "NULL args are refused");
}
