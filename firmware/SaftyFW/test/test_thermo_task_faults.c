// Host tests for the REAL thermo_task.c loop (docs/audits/
// HOST_TEST_COVERAGE_GAPS_2026-10-09.md, campaign 3).
//
// thermo_task_fn() is the only producer of the safety thermocouple snapshot
// that S5 (and S1/S12/...) consume. Until now nothing on the host executed
// it: the MAX31856 fault bits, the SPI-failure path, the plausibility
// downgrade, the CR1-verified downgrade and the DRDY-silence branch were all
// reviewed by reading, never run. This file drives the real task body through
// the task harness (stubs/task_harness): the stub xTaskCreate captures the
// task function, thermo_task_start() is called for real, and the world is
// scripted one loop iteration at a time from the ulTaskNotifyTake() hook --
// a fake MAX31856 behind the real max31856.c and fake_spi.c, a controllable
// ~DRDY pin and ISR.
//
// What a "fail-safe state" means here: each published snapshot is mapped to a
// safety_guard_input_t exactly the way safety_core.c's build_input() does
// (tc_valid = valid && fresh, fault_bits/spi_failed/cj_invalid passed
// through) and ticked through the REAL safety_guards.c. A fault is only
// counted as handled when it ends in an S5 trip (SAFETY_TRIP_SENSOR_INVALID),
// never in a stale "good" reading reaching the guards.
#define _CRT_SECURE_NO_WARNINGS
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "task_harness.h"

#include "fake_spi.h"
#include "hal_spi.h"
#include "max31856.h"
#include "board_pins.h"

#include "../src/safety_guards.h"
#include "../src/tasks/tick_timing.h"
#include "../src/tasks/thermo_task.h"

#define TEST_CS_GPIO    11u
#define TEST_FAULT_GPIO 12u

// Test seams defined in test_thermo_task_stubs.c.
void  tt_stub_reset(void);
void  tt_stub_set_tc_offset_c(float offset);
void  tt_stub_set_tc_type_set(bool is_set);
unsigned tt_stub_checkin_count_thermo(void);

typedef enum {
    DRDY_EDGE = 0,    // conversion complete: ~DRDY falls, ISR fires, notification pending
    DRDY_SILENT_HIGH, // no edge, pin stays high (pull-up): chip not converting
    DRDY_SILENT_LOW,  // no edge reached the task but the pin reads asserted (missed edge)
} drdy_mode_t;

typedef struct {
    uint8_t     burst[7]; // rx of the 7-byte CJTH burst: [0] address echo, [1..6] registers
    bool        queue_burst;
    bool        spi_timeout; // the burst transfer times out instead
    drdy_mode_t drdy;
} step_t;

#define MAX_STEPS 40

static const step_t   *s_steps;
static unsigned        s_nsteps;
static unsigned        s_iter;
static bool            s_publish_fails;
static thermo_snapshot_t s_out[MAX_STEPS];
static uint32_t        s_wait_ticks[MAX_STEPS];
static uint32_t        s_edge_advance_ticks = 150;

static void wait_hook(uint32_t timeout_ticks)
{
    th_set_sem_take_fails(false);
    if (s_iter > 0) {
        thermo_snapshot_t snap;
        (void)thermo_task_get_snapshot(&snap);
        s_out[s_iter - 1] = snap;
    }
    if (s_iter >= s_nsteps) {
        th_abort();
    }
    const step_t *st = &s_steps[s_iter];
    s_wait_ticks[s_iter] = timeout_ticks;
    th_set_sem_take_fails(s_publish_fails);

    if (st->spi_timeout) {
        fake_spi_inject_enqueue_timeout(max31856_spi_bus_for_test(), 1);
    } else if (st->queue_burst) {
        (void)fake_spi_script_rx(max31856_spi_device_for_test(), st->burst, sizeof(st->burst));
    }

    switch (st->drdy) {
    case DRDY_EDGE:
        th_gpio_set_level(SAFTYFW_PIN_THERMO_DRDY, true);
        th_fire_drdy_isr();
        th_advance_tick(s_edge_advance_ticks);
        break;
    case DRDY_SILENT_HIGH:
        th_gpio_set_level(SAFTYFW_PIN_THERMO_DRDY, true);
        break;
    case DRDY_SILENT_LOW:
        th_gpio_set_level(SAFTYFW_PIN_THERMO_DRDY, false);
        th_advance_tick(s_edge_advance_ticks);
        break;
    }
    s_iter++;
}

// --- burst builders ----------------------------------------------------------

static void put_burst(uint8_t out[7], float cj_c, float tc_c, uint8_t sr)
{
    int16_t cj = (int16_t)(cj_c * 256.0f);
    int32_t tc_lsb = (int32_t)(tc_c / 0.0078125f); // 19-bit signed, <<5 in the 24-bit field
    uint32_t tc24 = ((uint32_t)tc_lsb << 5) & 0xFFFFFFu;
    out[0] = 0x00u;
    out[1] = (uint8_t)((uint16_t)cj >> 8);
    out[2] = (uint8_t)((uint16_t)cj & 0xFFu);
    out[3] = (uint8_t)(tc24 >> 16);
    out[4] = (uint8_t)(tc24 >> 8);
    out[5] = (uint8_t)tc24;
    out[6] = sr;
}

static step_t good_step(float tc_c)
{
    step_t s;
    memset(&s, 0, sizeof(s));
    put_burst(s.burst, 25.0f, tc_c, 0x00u);
    s.queue_burst = true;
    s.drdy = DRDY_EDGE;
    return s;
}

static step_t sr_step(float tc_c, uint8_t sr)
{
    step_t s = good_step(tc_c);
    s.burst[6] = sr;
    return s;
}

// --- scenario runner ---------------------------------------------------------

static void setup_chip(bool cr1_matches)
{
    th_reset();
    fake_spi_reset_all();
    tt_stub_reset();
    th_set_tick(1000);
    (void)max31856_bus_init();
    (void)max31856_init(TEST_CS_GPIO, TEST_FAULT_GPIO);
    uint8_t readback[2] = { 0x00u, cr1_matches ? 0x23u : 0x00u };
    (void)fake_spi_script_rx(max31856_spi_device_for_test(), readback, sizeof(readback));
    (void)max31856_configure(MAX31856_TC_TYPE_K);
    (void)thermo_task_start();
}

static void run_steps(const step_t *steps, unsigned n)
{
    s_steps = steps;
    s_nsteps = n;
    s_iter = 0;
    memset(s_out, 0, sizeof(s_out));
    memset(s_wait_ticks, 0, sizeof(s_wait_ticks));
    th_set_wait_hook(wait_hook);
    th_run_captured_task();
    th_set_sem_take_fails(false);
    s_publish_fails = false;
    if (getenv("TT_DEBUG")) {
        for (unsigned i = 0; i < n; i++) {
            printf("  dbg[%u] valid=%d tc=%g cj=%g cjv=%d fb=%02x spif=%d ts=%u wait=%u\n", i, s_out[i].valid,
                   (double)s_out[i].tc_c, (double)s_out[i].cj_c, s_out[i].cj_valid, s_out[i].fault_bits,
                   s_out[i].spi_failed, (unsigned)s_out[i].timestamp_ms, (unsigned)s_wait_ticks[i]);
        }
    }
}

// Maps a published snapshot to a guard input exactly like safety_core.c's
// build_input() (tc_valid = valid && fresh; every other field passed through).
static safety_guard_input_t guard_input_from(const thermo_snapshot_t *t, uint32_t now_ms)
{
    safety_guard_input_t in;
    memset(&in, 0, sizeof(in));
    in.dt_s = 5.0f;
    in.tc_valid = t->valid && snapshot_is_fresh(now_ms, t->timestamp_ms, 2000u);
    in.tc_c = t->tc_c;
    in.cj_c = t->cj_c;
    in.cj_invalid = !t->cj_valid;
    in.fault_bits = t->fault_bits;
    in.spi_failed = t->spi_failed;
    in.current_sensing_commissioned = true;
    return in;
}

static safety_guard_cfg_t guard_cfg(void)
{
    safety_guard_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tc_placement_valid = true;
    cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
    cfg.abs_max_temp_c = 1300.0f;
    cfg.tc_source = SAFETY_TC_SOURCE_OWN_J7;
    return cfg;
}

// Ticks the real guards with the same input `ticks` times (dt 5 s each).
// Returns true if S5 (SENSOR_INVALID) latched.
static bool guards_trip_on_sensor_invalid(const thermo_snapshot_t *t, uint32_t now_ms, int ticks,
                                          int *s5_streak_out)
{
    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg = guard_cfg();
    safety_guard_input_t in = guard_input_from(t, now_ms);
    for (int i = 0; i < ticks; i++) {
        (void)safety_guards_tick(&s, &cfg, &in);
    }
    if (s5_streak_out) {
        *s5_streak_out = (int)s.s5_bad_streak;
    }
    return s.is_tripped && s.reason == SAFETY_TRIP_SENSOR_INVALID;
}

static bool guards_quiet(const thermo_snapshot_t *t, uint32_t now_ms, int ticks)
{
    int streak = -1;
    bool trip = guards_trip_on_sensor_invalid(t, now_ms, ticks, &streak);
    return !trip && streak == 0;
}

// --- tests -------------------------------------------------------------------

static void test_healthy_baseline(void)
{
    TEST_SECTION("thermo_task loop -- healthy baseline");
    setup_chip(true);
    step_t steps[3] = { good_step(100.0f), good_step(100.0f), good_step(100.0f) };
    run_steps(steps, 3);

    TEST_CHECK(th_irq_registered() && th_irq_pin() == SAFTYFW_PIN_THERMO_DRDY,
               "task arms the ~DRDY falling-edge IRQ on the DRDY pin");
    TEST_CHECK(s_wait_ticks[0] == 302u,
               "DRDY wait is 2 x the 151 ms conversion time once the chip is configured");
    TEST_CHECK(s_out[0].valid && s_out[2].valid, "good conversions publish valid snapshots");
    TEST_CHECK(s_out[2].tc_c == 100.0f && s_out[2].cj_c == 25.0f,
               "tc and cj decode to the scripted 100.0 / 25.0 degC");
    TEST_CHECK(s_out[2].cj_valid && !s_out[2].spi_failed && s_out[2].fault_bits == 0,
               "cj_valid set, no spi_failed, no fault bits");
    TEST_CHECK(tt_stub_checkin_count_thermo() == 3,
               "the task checks in to the watchdog once per loop iteration");
    TEST_CHECK(guards_quiet(&s_out[2], s_out[2].timestamp_ms, 40),
               "a healthy snapshot never starts an S5 streak or trips");
}

static void check_chip_fault_trips(const char *name, uint8_t sr)
{
    char msg[160];
    setup_chip(true);
    // The chip returns junk register contents alongside the fault flag.
    step_t steps[3] = { sr_step(100.0f, sr), sr_step(100.0f, sr), sr_step(100.0f, sr) };
    run_steps(steps, 3);
    const thermo_snapshot_t *t = &s_out[2];

    snprintf(msg, sizeof(msg), "%s: SR bit reaches the snapshot fault_bits", name);
    TEST_CHECK((t->fault_bits & sr) == sr, msg);
    snprintf(msg, sizeof(msg), "%s: tc_c is NaN, never the junk register value", name);
    TEST_CHECK(isnan(t->tc_c), msg);
    snprintf(msg, sizeof(msg), "%s: snapshot is not valid", name);
    TEST_CHECK(!t->valid, msg);
    snprintf(msg, sizeof(msg), "%s: the published snapshot is not a spi_failed (the bus worked)", name);
    TEST_CHECK(!t->spi_failed, msg);
    int streak = 0;
    bool trip = guards_trip_on_sensor_invalid(t, t->timestamp_ms, 12, &streak);
    snprintf(msg, sizeof(msg), "%s: 60 s of it trips S5 SENSOR_INVALID through the real guards", name);
    TEST_CHECK(trip, msg);
}

static void test_chip_fault_bits(void)
{
    TEST_SECTION("thermo_task loop -- MAX31856 fault flags reach the guard as a trip");
    check_chip_fault_trips("open circuit (OPEN)", MAX31856_FAULT_OPEN);
    // Short to GND or VCC on a TC input trips the OV/UV comparator (OVUV).
    check_chip_fault_trips("short to GND/VCC (OVUV)", MAX31856_FAULT_OVUV);
    check_chip_fault_trips("TC outside type range (TCRANGE)", MAX31856_FAULT_TCRANGE);
    check_chip_fault_trips("OPEN with CJRANGE", (uint8_t)(MAX31856_FAULT_OPEN | MAX31856_FAULT_CJRANGE));
}

static void test_cold_junction_faults_are_not_s5(void)
{
    TEST_SECTION("thermo_task loop -- cold-junction and threshold flags are not S5 bad reads");
    {
        setup_chip(true);
        step_t steps[3] = { sr_step(100.0f, MAX31856_FAULT_CJRANGE), sr_step(100.0f, MAX31856_FAULT_CJRANGE),
                            sr_step(100.0f, MAX31856_FAULT_CJRANGE) };
        run_steps(steps, 3);
        const thermo_snapshot_t *t = &s_out[2];
        TEST_CHECK(isnan(t->cj_c) && !t->cj_valid, "CJRANGE: cj_c is NaN and cj_valid is false");
        TEST_CHECK(t->valid && t->tc_c == 100.0f,
                   "CJRANGE: the TC reading itself stays valid (the part's TC is fine)");
        TEST_CHECK(t->fault_bits == MAX31856_FAULT_CJRANGE, "CJRANGE reaches fault_bits");
        TEST_CHECK(guards_quiet(t, t->timestamp_ms, 40), "CJRANGE alone never starts S5's streak");
    }
    {
        setup_chip(true);
        uint8_t sr = (uint8_t)(MAX31856_FAULT_TCHIGH | MAX31856_FAULT_TCLOW | MAX31856_FAULT_CJHIGH |
                               MAX31856_FAULT_CJLOW);
        step_t steps[2] = { sr_step(100.0f, sr), sr_step(100.0f, sr) };
        run_steps(steps, 2);
        const thermo_snapshot_t *t = &s_out[1];
        TEST_CHECK(t->valid && t->tc_c == 100.0f, "threshold comparator flags do not NaN or invalidate a TC");
        TEST_CHECK(t->fault_bits == sr, "threshold flags are passed through to the guard");
        TEST_CHECK(guards_quiet(t, t->timestamp_ms, 40), "threshold flags alone never start S5's streak");
    }
}

static void test_implausible_values(void)
{
    TEST_SECTION("thermo_task loop -- implausible finite values are downgraded");
    setup_chip(true);
    step_t steps[6] = {
        good_step(1372.0f),  // K upper bound, inclusive
        good_step(1373.0f),  // just above
        good_step(2000.0f),  // wildly implausible (e.g. a short reading garbage)
        good_step(-200.0f),  // K lower bound, inclusive
        good_step(-201.0f),  // just below
        good_step(-400.0f),
    };
    run_steps(steps, 6);
    TEST_CHECK(s_out[0].valid, "1372 C (K upper bound) is plausible");
    TEST_CHECK(!s_out[1].valid, "1373 C is implausible for K -> invalid");
    TEST_CHECK(!s_out[2].valid && s_out[2].tc_c == 2000.0f,
               "2000 C invalid; the finite value stays visible in the snapshot");
    TEST_CHECK(s_out[3].valid, "-200 C (K lower bound) is plausible");
    TEST_CHECK(!s_out[4].valid && !s_out[5].valid, "below -200 C is invalid");

    int streak = 0;
    TEST_CHECK(guards_trip_on_sensor_invalid(&s_out[2], s_out[2].timestamp_ms, 12, &streak),
               "an implausible 2000 C reading trips S5 instead of being treated as a real temperature");
}

static void test_commissioning_band(void)
{
    TEST_SECTION("thermo_task loop -- uncommissioned tc_type uses the wide union band");
    setup_chip(true);
    tt_stub_set_tc_type_set(false);
    step_t steps[3] = { good_step(1500.0f), good_step(1820.0f), good_step(1900.0f) };
    run_steps(steps, 3);
    TEST_CHECK(s_out[0].valid, "uncommissioned: 1500 C passes the union band");
    TEST_CHECK(s_out[1].valid, "uncommissioned: 1820 C (union upper bound) passes");
    TEST_CHECK(!s_out[2].valid, "uncommissioned: 1900 C is garbage -> invalid");

    setup_chip(true);
    tt_stub_set_tc_type_set(true);
    step_t k[1] = { good_step(1500.0f) };
    run_steps(k, 1);
    TEST_CHECK(!s_out[0].valid, "commissioned K: the same 1500 C is above 1372 -> invalid");
}

static void test_spi_failure(void)
{
    TEST_SECTION("thermo_task loop -- SPI transfer failure");
    setup_chip(true);
    step_t t_out;
    memset(&t_out, 0, sizeof(t_out));
    t_out.spi_timeout = true;
    t_out.drdy = DRDY_EDGE;
    step_t steps[3] = { t_out, t_out, t_out };
    run_steps(steps, 3);
    const thermo_snapshot_t *t = &s_out[2];
    TEST_CHECK(t->spi_failed && !t->valid, "a timed-out burst publishes spi_failed and invalid");
    TEST_CHECK(isnan(t->tc_c) && isnan(t->cj_c) && !t->cj_valid,
               "no register data is invented: tc and cj are NaN, cj_valid false");
    TEST_CHECK(t->fault_bits == 0, "no stale SR bits are carried through a failed transfer");
    int streak = 0;
    TEST_CHECK(guards_trip_on_sensor_invalid(t, t->timestamp_ms, 12, &streak),
               "sustained SPI failure trips S5");
}

static void test_cr1_unverified(void)
{
    TEST_SECTION("thermo_task loop -- tc_type not verified by CR1 readback");
    setup_chip(false);
    TEST_CHECK(!max31856_tc_type_verified(), "precondition: CR1 readback mismatched");
    step_t steps[3] = { good_step(100.0f), good_step(100.0f), good_step(100.0f) };
    run_steps(steps, 3);
    TEST_CHECK(!s_out[2].valid, "a clean-looking reading is invalid while the part's tc_type is unverified");
    TEST_CHECK(s_out[2].tc_c == 100.0f && s_out[2].fault_bits == 0,
               "the downgrade is validity only: the reading and fault bits are untouched");
    int streak = 0;
    TEST_CHECK(guards_trip_on_sensor_invalid(&s_out[2], s_out[2].timestamp_ms, 12, &streak),
               "unverified tc_type ends in an S5 trip, not heat permission on an unconfirmed type");
}

static void test_drdy_silence_and_missed_edge(void)
{
    TEST_SECTION("thermo_task loop -- ~DRDY silence and missed-edge recovery");
    {
        setup_chip(true);
        step_t silent;
        memset(&silent, 0, sizeof(silent));
        silent.drdy = DRDY_SILENT_HIGH;
        step_t steps[3] = { good_step(100.0f), silent, silent };
        run_steps(steps, 3);
        TEST_CHECK(s_out[0].valid, "precondition: first conversion publishes valid");
        const thermo_snapshot_t *t = &s_out[2];
        TEST_CHECK(!t->valid && isnan(t->tc_c) && isnan(t->cj_c) && !t->cj_valid,
                   "DRDY silence with the pin high: invalid, NaN, no cj");
        TEST_CHECK(!t->spi_failed && t->fault_bits == 0, "silence is not reported as an SPI failure or fault");
        TEST_CHECK(t->timestamp_ms > s_out[0].timestamp_ms, "silence still publishes a fresh timestamp");
        int streak = 0;
        TEST_CHECK(guards_trip_on_sensor_invalid(t, t->timestamp_ms, 12, &streak),
                   "a chip that stopped converting trips S5");
    }
    {
        setup_chip(true);
        step_t missed = good_step(100.0f);
        missed.drdy = DRDY_SILENT_LOW;
        step_t steps[2] = { missed, missed };
        run_steps(steps, 2);
        TEST_CHECK(s_out[1].valid && s_out[1].tc_c == 100.0f,
                   "pin asserted with no notification: the pending result is read anyway");
    }
}

static void test_tc_offset(void)
{
    TEST_SECTION("thermo_task loop -- tc offset");
    setup_chip(true);
    tt_stub_set_tc_offset_c(2.5f);
    step_t steps[2] = { good_step(100.0f), sr_step(100.0f, MAX31856_FAULT_OPEN) };
    run_steps(steps, 2);
    TEST_CHECK(s_out[0].tc_c == 102.5f, "the commissioned offset is added to a good reading");
    TEST_CHECK(isnan(s_out[1].tc_c) && !s_out[1].valid, "NaN + offset stays NaN: an open TC never becomes a number");
}

static void test_recovery_after_fault_clears(void)
{
    TEST_SECTION("thermo_task loop -- recovery after a fault clears");
    setup_chip(true);
    step_t steps[5] = {
        good_step(100.0f),
        sr_step(100.0f, MAX31856_FAULT_OPEN),
        sr_step(100.0f, MAX31856_FAULT_OPEN),
        good_step(101.0f),
        good_step(102.0f),
    };
    run_steps(steps, 5);
    TEST_CHECK(!s_out[1].valid && !s_out[2].valid, "the faulted iterations are invalid");
    TEST_CHECK(s_out[3].valid && s_out[3].tc_c == 101.0f && s_out[3].fault_bits == 0,
               "the first good conversion after the fault is valid again, with no residual fault bits");

    // Guard view: a short dropout then recovery never trips.
    safety_guard_state_t gs;
    safety_guards_reset(&gs);
    safety_guard_cfg_t cfg = guard_cfg();
    bool tripped = false;
    for (int i = 0; i < 5; i++) {
        safety_guard_input_t in = guard_input_from(&s_out[i], s_out[i].timestamp_ms);
        in.dt_s = 0.1f;
        tripped = safety_guards_tick(&gs, &cfg, &in) || tripped;
    }
    TEST_CHECK(!tripped && gs.s5_bad_streak == 0, "a 2-sample dropout then recovery: no trip, streak back to 0");

    // Once S5 has latched, a recovered sensor does NOT clear it.
    safety_guards_reset(&gs);
    safety_guard_input_t bad = guard_input_from(&s_out[1], s_out[1].timestamp_ms);
    for (int i = 0; i < 12; i++) {
        (void)safety_guards_tick(&gs, &cfg, &bad);
    }
    safety_guard_input_t good = guard_input_from(&s_out[4], s_out[4].timestamp_ms);
    (void)safety_guards_tick(&gs, &cfg, &good);
    TEST_CHECK(gs.is_tripped && gs.reason == SAFETY_TRIP_SENSOR_INVALID,
               "a latched S5 trip stays latched when the sensor recovers (needs an explicit clear)");
}

static void test_stale_publish_never_reads_good(void)
{
    TEST_SECTION("thermo_task loop -- failed publish leaves an old snapshot; freshness makes it unusable");
    setup_chip(true);
    step_t good[1] = { good_step(100.0f) };
    run_steps(good, 1);
    thermo_snapshot_t before;
    (void)thermo_task_get_snapshot(&before);
    TEST_CHECK(before.valid, "precondition: a good snapshot is published");

    // The fault arrives but every publish times out on the snapshot mutex.
    s_publish_fails = true;
    step_t faulty[3] = { sr_step(100.0f, MAX31856_FAULT_OPEN), sr_step(100.0f, MAX31856_FAULT_OPEN),
                         sr_step(100.0f, MAX31856_FAULT_OPEN) };
    s_steps = faulty;
    s_nsteps = 3;
    s_iter = 0;
    th_set_wait_hook(wait_hook);
    // hook toggles the failure flag per iteration; restore it from s_publish_fails each time.
    th_run_captured_task();
    th_set_sem_take_fails(false);
    s_publish_fails = false;

    thermo_snapshot_t after;
    (void)thermo_task_get_snapshot(&after);
    TEST_CHECK(after.valid && after.timestamp_ms == before.timestamp_ms,
               "documented behaviour: a failed publish leaves the previous good snapshot in place");

    uint32_t t0 = after.timestamp_ms;
    TEST_CHECK(guard_input_from(&after, t0 + 1999u).tc_valid,
               "freshness boundary: age 1999 ms is still fresh");
    TEST_CHECK(!guard_input_from(&after, t0 + 2000u).tc_valid,
               "freshness boundary: age 2000 ms is stale -> tc_valid false");
    int streak = 0;
    TEST_CHECK(guards_trip_on_sensor_invalid(&after, t0 + 5000u, 12, &streak),
               "a snapshot that stops updating ends in an S5 trip via the staleness check, never a frozen good reading");
}

static void test_stuck_reading_documented(void)
{
    TEST_SECTION("thermo_task loop -- stuck (frozen) readings are the guard's job");
    setup_chip(true);
    step_t steps[8];
    for (int i = 0; i < 8; i++) {
        steps[i] = good_step(412.0f);
    }
    run_steps(steps, 8);
    TEST_CHECK(s_out[7].valid && s_out[7].tc_c == 412.0f,
               "documented: thermo_task does no stuck-value detection; identical readings stay valid");
}

void run_test_thermo_task_faults(void)
{
    test_healthy_baseline();
    test_chip_fault_bits();
    test_cold_junction_faults_are_not_s5();
    test_implausible_values();
    test_commissioning_band();
    test_spi_failure();
    test_cr1_unverified();
    test_drdy_silence_and_missed_edge();
    test_tc_offset();
    test_recovery_after_fault_clears();
    test_stale_publish_never_reads_good();
    test_stuck_reading_documented();
}
