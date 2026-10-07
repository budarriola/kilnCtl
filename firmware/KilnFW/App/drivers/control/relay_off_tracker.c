#include "relay_off_tracker.h"

#include <stdbool.h>

#include "hal_time.h"
#include "on_off_trigger_decide.h"

#define TRACKED_RELAYS 8u

static uint8_t s_on_mask;             /* bits seen written ON and not yet written OFF */
static uint8_t s_have_off_mask;       /* bits with a recorded ON-to-OFF time */
static uint64_t s_last_off_us[TRACKED_RELAYS];

void relay_off_tracker_note_write(uint8_t mask, uint8_t value)
{
    uint64_t now_us = hal_time_now_us();
    for (uint8_t i = 0; i < TRACKED_RELAYS; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(mask & bit)) {
            continue;
        }
        if (value & bit) {
            s_on_mask |= bit;
        } else if (s_on_mask & bit) {
            s_on_mask &= (uint8_t)~bit;
            s_last_off_us[i] = now_us;
            s_have_off_mask |= bit;
        }
    }
}

float relay_off_tracker_held_s(uint8_t mask)
{
    uint64_t now_us = hal_time_now_us();
    float held = ON_OFF_HOLD_SETTLED_S;
    for (uint8_t i = 0; i < TRACKED_RELAYS; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(mask & bit)) {
            continue;
        }
        if (s_on_mask & bit) {
            return 0.0f;
        }
        if (s_have_off_mask & bit) {
            float age = (float)((double)(now_us - s_last_off_us[i]) / 1000000.0);
            if (age < held) {
                held = age;
            }
        }
    }
    return held;
}

void relay_off_tracker_reset_all(void)
{
    s_on_mask = 0;
    s_have_off_mask = 0;
    for (uint8_t i = 0; i < TRACKED_RELAYS; i++) {
        s_last_off_us[i] = 0;
    }
}
