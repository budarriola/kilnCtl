#include "relay_off_tracker.h"

#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/portmacro.h"
#include "hal_time.h"
#include "on_off_trigger_decide.h"

#define TRACKED_RELAYS 8u

static uint8_t s_on_mask;             /* bits seen written ON and not yet written OFF */
static uint8_t s_have_off_mask;       /* bits with a recorded ON-to-OFF time */
static uint64_t s_last_off_us[TRACKED_RELAYS];
/* Writers run on two tasks (kiln_io_owner's owner_task and the executor/HTTP
 * callers of apply_relay()/aux_apply_relay()/profile_executor_run()), and the
 * 64-bit timestamps are not atomic on the 32-bit core, so every access is a
 * short critical section. The clock is read before entering it. */
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;

void relay_off_tracker_note_write(uint8_t mask, uint8_t value)
{
    uint64_t now_us = hal_time_now_us();
    portENTER_CRITICAL(&s_mux);
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
    portEXIT_CRITICAL(&s_mux);
}

float relay_off_tracker_held_s(uint8_t mask)
{
    uint64_t now_us = hal_time_now_us();
    uint8_t on_mask;
    uint8_t have_off_mask;
    uint64_t last_off_us[TRACKED_RELAYS];
    portENTER_CRITICAL(&s_mux);
    on_mask = s_on_mask;
    have_off_mask = s_have_off_mask;
    for (uint8_t i = 0; i < TRACKED_RELAYS; i++) {
        last_off_us[i] = s_last_off_us[i];
    }
    portEXIT_CRITICAL(&s_mux);

    float held = ON_OFF_HOLD_SETTLED_S;
    for (uint8_t i = 0; i < TRACKED_RELAYS; i++) {
        uint8_t bit = (uint8_t)(1u << i);
        if (!(mask & bit)) {
            continue;
        }
        if (on_mask & bit) {
            return 0.0f;
        }
        if (have_off_mask & bit) {
            /* now_us was read before the snapshot, so a write that landed in
             * between can be newer than now_us: clamp to 0 rather than wrap. */
            uint64_t d_us = (now_us > last_off_us[i]) ? (now_us - last_off_us[i]) : 0u;
            float age = (float)((double)d_us / 1000000.0);
            if (age < held) {
                held = age;
            }
        }
    }
    return held;
}

void relay_off_tracker_reset_all(void)
{
    portENTER_CRITICAL(&s_mux);
    s_on_mask = 0;
    s_have_off_mask = 0;
    for (uint8_t i = 0; i < TRACKED_RELAYS; i++) {
        s_last_off_us[i] = 0;
    }
    portEXIT_CRITICAL(&s_mux);
}
