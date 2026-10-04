#include "ct_leak_alarm.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "ct_noise_floor.h"

static volatile bool s_pub_alarm = false;
static volatile uint8_t s_pub_mask = 0;
static volatile float s_pub_peak_a = 0.0f;

void ct_leak_alarm_reset(ct_leak_alarm_state_t *s)
{
    if (s != NULL) {
        memset(s, 0, sizeof(*s));
    }
}

static bool channel_relevant(const ct_leak_alarm_input_t *in, int ch)
{
    /* Summed topology: one clamp (index 2, GPIO28) sees every zone; the other
     * two inputs are not fitted and are never read. Per-zone: all three. */
    return in->summed ? (ch == CT_LEAK_CHANNELS - 1) : true;
}

bool ct_leak_alarm_tick(ct_leak_alarm_state_t *s, const ct_leak_alarm_input_t *in)
{
    if (s == NULL || in == NULL) {
        return false;
    }
    if (!in->ct_installed) {
        /* The operator has declared there is no CT: nothing to alarm on, and
         * a standing alarm from before the declaration is moot. */
        ct_leak_alarm_reset(s);
        return false;
    }
    if (!in->link_up) {
        /* No fresh reading. Hold whatever state we have, progress nothing. */
        s->above_run = false;
        s->below_run = false;
        return false;
    }
    bool evaluable = !in->activity && in->relays_off_ms != UINT32_MAX &&
                     in->relays_off_ms >= CT_LEAK_RELAYS_OFF_SETTLE_MS;
    if (!evaluable) {
        /* Relays chop or just released: no sample counts. A latched alarm
         * stays latched; only a run of quiet evaluated samples clears it. */
        s->above_run = false;
        s->below_run = false;
        return false;
    }

    uint8_t mask = 0;
    float worst = 0.0f;
    for (int ch = 0; ch < CT_LEAK_CHANNELS; ch++) {
        if (!channel_relevant(in, ch)) {
            continue;
        }
        float a = in->current_a[ch];
        if (isnan(a)) {
            continue; /* no number at all: unknown, not evidence of current */
        }
        if (isinf(a)) {
            /* A saturated/overflowed CT channel with every relay off is a
             * fault worth a human's attention, never a quiet reading: treat it
             * as above the floor. Cap the magnitude so the published peak and
             * the describe() text stay printable. */
            a = 1000.0f;
        }
        if (a > ct_noise_floor_a(in->k_ct_v_per_a[ch])) {
            mask |= (uint8_t)(1u << ch);
            if (a > worst) {
                worst = a;
            }
        }
    }
    s->channel_mask = mask;

    if (mask != 0u) {
        s->below_run = false;
        if (!s->above_run) {
            s->above_run = true;
            s->above_since_ms = in->now_ms;
        }
        if (s->alarm) {
            if (worst > s->peak_a) {
                s->peak_a = worst;
            }
            return false;
        }
        if ((uint32_t)(in->now_ms - s->above_since_ms) >= CT_LEAK_ASSERT_MS) {
            s->alarm = true;
            s->peak_a = worst;
            return true;
        }
        return false;
    }

    s->above_run = false;
    if (s->alarm) {
        if (!s->below_run) {
            s->below_run = true;
            s->below_since_ms = in->now_ms;
        }
        if ((uint32_t)(in->now_ms - s->below_since_ms) >= CT_LEAK_CLEAR_MS) {
            ct_leak_alarm_reset(s);
        }
    }
    return false;
}

void ct_leak_alarm_publish(const ct_leak_alarm_state_t *s)
{
    if (s == NULL) {
        return;
    }
    s_pub_mask = s->channel_mask;
    s_pub_peak_a = s->peak_a;
    s_pub_alarm = s->alarm;
}

bool ct_leak_alarm_is_active(void)
{
    return s_pub_alarm;
}

void ct_leak_alarm_describe(char *out, unsigned cap)
{
    if (out == NULL || cap == 0u) {
        return;
    }
    out[0] = '\0';
    if (!s_pub_alarm) {
        return;
    }
    unsigned mask = s_pub_mask;
    char chans[16];
    chans[0] = '\0';
    unsigned n = 0;
    for (int ch = 0; ch < CT_LEAK_CHANNELS; ch++) {
        if (mask & (1u << ch)) {
            n += (unsigned)snprintf(chans + n, sizeof(chans) - n, "%sch%d", n ? "," : "", ch + 1);
        }
    }
    snprintf(out, cap, "CT current with ALL relays off (%s, peak %.2f A)", chans[0] ? chans : "ch?",
             (double)s_pub_peak_a);
}
