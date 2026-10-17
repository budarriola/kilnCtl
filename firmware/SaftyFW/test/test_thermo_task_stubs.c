// Stubs for the thermo_task host exe: the few collaborators thermo_task.c
// links against that are not under test (config_store, relay_owner,
// watchdog check-in). Each exposes a seam so a test can drive it.
#include <string.h>

#include "config_store.h"
#include "max31856.h"
#include "relay_owner.h"
#include "watchdog_task.h"
#include "task_harness.h"
#include "pico/time.h"

// A2 (2026-10-14): hardware-timer clock = harness tick (1 tick = 1 ms) + a skew the
// test can set, so it can prove snapshots are stamped from THIS clock, not the tick.
uint32_t g_test_hw_skew_ms = 0;
absolute_time_t get_absolute_time(void)
{
    absolute_time_t t;
    t.us = ((uint64_t)th_get_tick() + g_test_hw_skew_ms) * 1000u;
    return t;
}
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t.us / 1000u); }

static float    s_tc_offset_c;
static uint16_t s_tc_sheath_tau_s;
static bool     s_tc_type_set = true;
static unsigned s_checkins[WATCHDOG_CHECKIN_COUNT];

void tt_stub_reset(void)
{
    s_tc_offset_c = 0.0f;
    s_tc_sheath_tau_s = 0u;
    s_tc_type_set = true;
    memset(s_checkins, 0, sizeof(s_checkins));
}
void tt_stub_set_tc_offset_c(float offset) { s_tc_offset_c = offset; }
void tt_stub_set_tc_sheath_tau_s(uint16_t tau) { s_tc_sheath_tau_s = tau; }
void tt_stub_set_tc_type_set(bool is_set) { s_tc_type_set = is_set; }
unsigned tt_stub_checkin_count_thermo(void) { return s_checkins[WATCHDOG_CHECKIN_THERMO_TASK]; }

uint8_t config_store_get_tc_type(void) { return MAX31856_TC_TYPE_K; }
float   config_store_get_tc_offset_c(void) { return s_tc_offset_c; }
uint16_t config_store_get_tc_sheath_tau_s(void) { return s_tc_sheath_tau_s; }
bool    config_store_is_tc_type_set(void) { return s_tc_type_set; }
bool    config_store_get_full_record(config_store_record_t *out)
{
    memset(out, 0, sizeof(*out));
    return true;
}

relay_owner_state_t relay_owner_get_state(void) { return RELAY_OWNER_STATE_INIT; }
bool                relay_owner_is_energized(void) { return false; }

void watchdog_task_checkin(watchdog_checkin_id_t id)
{
    if ((unsigned)id < WATCHDOG_CHECKIN_COUNT) {
        s_checkins[id]++;
    }
}
