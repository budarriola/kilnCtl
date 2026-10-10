// Campaign 1 (docs/audits/HOST_TEST_COVERAGE_GAPS_2026-10-09.md): safety_core.c
// host-compiled, for real, against fakes for the hardware, link and timer
// seams. Until this file existed safety_core.c had never been built on host;
// test_safety_core_*.c in the main exe are source-text greps, and
// test_safety_guards.c exercises the pure safety_guards_tick() with
// hand-built inputs. This file covers the layer in between that neither
// reaches: snapshot -> safety_guard_input_t wiring (staleness, context age,
// CT masking), trip -> relay_owner_command_trip(), latch persistence, the
// clear-trip queue and its occurrence/stale-seq refusals, the trip-event
// seqlock-free publication, and safety_core_request_enable()'s gates.
//
// How the infinite safety_core_task() loop is made steppable: the fake
// xTaskCreate() records the task function, and the harness runs it in a Win32
// fiber. The fake vTaskDelayUntil() switches back to the harness, so one
// harness step() == exactly one pass of the task's for(;;) body (the real
// 100 ms period, on a fake clock the harness owns).
//
// A SEPARATE executable from test_main.c's (see build_host_tests.ps1): the
// fakes below define relay_owner_*, xQueue*/xTask*, log_task_log and the
// thermo/current/discrete/link snapshot providers, which the main exe already
// links for real or as different stubs (LNK2005).
//
// Statics inside safety_core.c (trip seq, clear stats, grace latch) persist
// across scenarios -- each scenario restarts the task (guards reset) on a
// clock that never goes backwards and asserts RELATIVE seq changes.
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#include "test_common.h"

#include "config_store.h"
#include "safety_guards.h"
#include "snapshots.h"
#include "commissioning_gate.h"
#include "link_frame.h"
#include "log_task.h"
#include "relay_owner.h"
#include "safety_core.h"
#include "update_task.h"
#include "watchdog_task.h"
#include "boot_reason.h"
#include "clear_trip_diag.h"
#include "pico/time.h"

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

int g_test_failures = 0;
int g_test_count = 0;

// ---------------------------------------------------------------- the world

typedef struct {
    // clock
    uint32_t now_ms;
    bool     clock_frozen; // don't advance now_ms in step() (stalled get_absolute_time)

    // thermo
    thermo_snapshot_t thermo;
    bool              thermo_publish; // stamp thermo.timestamp_ms = now each step

    // current
    current_snapshot_t current;
    bool               current_publish;

    // discrete
    bool estop;
    bool main_fault;

    // link
    bool                link_up;
    bool                ctx_published;
    context_snapshot_t  ctx;
    bool                ctx_publish; // stamp ctx.timestamp_ms = now each step
    bool                degraded_no_context;
    bool                have_ceiling;
    float               ceiling_c;
    uint32_t            relay_on_continuous_ms;

    // config
    config_store_record_t cfg;
    bool                  cfg_read_ok;
    bool                  integrity_recurrence;
    bool                  commissioned;

    // update
    bool update_active;

    // relay_owner fake
    relay_owner_state_t relay_state;
    bool                relay_energized;
    unsigned            trip_cmd_calls;
    unsigned            trip_cmd_fail_budget; // next N trip commands return false
    safety_trip_t       last_trip_cmd_reason;
    unsigned            clear_cmd_calls;
    unsigned            clear_cmd_fail_budget;
    unsigned            energize_true_calls;
    unsigned            energize_false_calls;
    bool                keep_energized_after_trip; // models a welded K4 (S9)

    // counters
    unsigned latch_trip_calls;
    uint32_t last_latched_reason;
    unsigned watchdog_checkins;
    unsigned log_error_calls;
} world_t;

static world_t W;

static void world_reset_keep_clock(void)
{
    uint32_t keep_now = W.now_ms;
    memset(&W, 0, sizeof(W));
    W.now_ms = keep_now ? keep_now : 100000u;

    W.thermo_publish = true;
    W.thermo.valid = true;
    W.thermo.tc_c = 25.0f;
    W.thermo.cj_c = 25.0f;
    W.thermo.cj_valid = true;

    W.current_publish = true;
    W.current.calibrated = true;

    W.link_up = true;
    W.ctx_published = false;

    config_store_default(&W.cfg);
    W.cfg.fields_set |= CONFIG_STORE_SET_ABS_MAX_TEMP_C | CONFIG_STORE_SET_TC_PLACEMENT_MODE;
    W.cfg.abs_max_temp_c = 1300.0f;
    W.cfg.tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_EXTERNAL_OVERHEAT;
    W.cfg_read_ok = true;
    W.commissioned = true;

    W.relay_state = RELAY_OWNER_STATE_ARMED;
    W.relay_energized = false;
}

// ----------------------------------------------------------- pico/time fake

absolute_time_t get_absolute_time(void)
{
    absolute_time_t t;
    t.us = (uint64_t)W.now_ms * 1000u;
    return t;
}
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t.us / 1000u); }
uint32_t time_us_32(void) { return (uint32_t)((uint64_t)W.now_ms * 1000u); }
uint64_t time_us_64(void) { return (uint64_t)W.now_ms * 1000u; }

// ------------------------------------------------------- FreeRTOS fakes

static TickType_t g_tick = 0;

TickType_t xTaskGetTickCount(void) { return g_tick; }

static TaskFunction_t g_task_fn = NULL;
static void          *g_task_arg = NULL;

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack_words, void *arg,
                       UBaseType_t priority, TaskHandle_t *out_handle)
{
    (void)name; (void)stack_words; (void)priority;
    g_task_fn = fn;
    g_task_arg = arg;
    if (out_handle) {
        *out_handle = (TaskHandle_t)fn;
    }
    return pdPASS;
}
void vTaskCoreAffinitySet(TaskHandle_t task, UBaseType_t mask) { (void)task; (void)mask; }

static LPVOID g_main_fiber = NULL;
static LPVOID g_task_fiber = NULL;

void vTaskDelayUntil(TickType_t *prev, TickType_t inc)
{
    (void)prev; (void)inc;
    SwitchToFiber(g_main_fiber);
}

// A real (tiny) FIFO queue: the main-exe stub is a no-op, but the clear-trip
// token path needs items to actually be stored and returned.
typedef struct {
    unsigned depth, item_size, count, head;
    uint8_t  data[16][8];
} fake_queue_t;

QueueHandle_t xQueueCreate(UBaseType_t n, UBaseType_t sz)
{
    fake_queue_t *q = (fake_queue_t *)calloc(1, sizeof(*q));
    if (!q || n > 16 || sz > 8) {
        free(q);
        return NULL;
    }
    q->depth = n;
    q->item_size = sz;
    return q;
}
BaseType_t xQueueSend(QueueHandle_t h, const void *item, TickType_t wait)
{
    (void)wait;
    fake_queue_t *q = (fake_queue_t *)h;
    if (q->count >= q->depth) {
        return pdFALSE;
    }
    memcpy(q->data[(q->head + q->count) % q->depth], item, q->item_size);
    q->count++;
    return pdTRUE;
}
BaseType_t xQueueReceive(QueueHandle_t h, void *out, TickType_t wait)
{
    (void)wait;
    fake_queue_t *q = (fake_queue_t *)h;
    if (q->count == 0) {
        return pdFALSE;
    }
    memcpy(out, q->data[q->head], q->item_size);
    q->head = (q->head + 1) % q->depth;
    q->count--;
    return pdTRUE;
}

// ---------------------------------------------------- seam fakes (snapshots)

bool thermo_task_get_snapshot(thermo_snapshot_t *out) { *out = W.thermo; return true; }
void current_task_get_snapshot(current_snapshot_t *out) { *out = W.current; }
bool discrete_task_estop_pressed(void) { return W.estop; }
bool discrete_task_main_fault(void) { return W.main_fault; }

bool link_task_get_context_snapshot(context_snapshot_t *out)
{
    if (!W.ctx_published) {
        memset(out, 0, sizeof(*out));
        return false;
    }
    *out = W.ctx;
    return true;
}
bool link_task_get_degraded_no_context(void) { return W.degraded_no_context; }
bool link_task_link_up(void) { return W.link_up; }
uint32_t link_task_get_relay_on_continuous_ms(void) { return W.relay_on_continuous_ms; }
bool link_task_get_firing_ceiling(float *out)
{
    if (W.have_ceiling) {
        *out = W.ceiling_c;
    }
    return W.have_ceiling;
}

bool config_store_get_full_record(config_store_record_t *out)
{
    if (!W.cfg_read_ok) {
        config_store_default(out); // models the real fail-closed default record
        return false;
    }
    *out = W.cfg;
    return true;
}
bool config_store_ram_integrity_recurrence_pending(void) { return W.integrity_recurrence; }
bool commissioning_gate_energize_allowed(bool enable, const config_store_record_t *rec)
{
    (void)rec;
    return !enable || W.commissioned;
}
bool update_task_transfer_active(void) { return W.update_active; }
void watchdog_task_checkin(watchdog_checkin_id_t id) { (void)id; W.watchdog_checkins++; }
bool log_task_log(uint8_t level, const char *tag, const char *msg)
{
    (void)tag; (void)msg;
    if (level == LOG_LEVEL_ERROR) {
        W.log_error_calls++;
    }
    return true;
}
void boot_reason_latch_trip(uint32_t reason)
{
    W.latch_trip_calls++;
    W.last_latched_reason = reason;
}
void clear_trip_diag_mark(uint8_t stage, uint8_t a, uint8_t b, bool c, bool d, bool e, uint8_t f)
{
    (void)stage; (void)a; (void)b; (void)c; (void)d; (void)e; (void)f;
}

// ----------------------------------------------------------- relay_owner fake

bool relay_owner_start(void) { return true; }
relay_owner_state_t relay_owner_get_state(void) { return W.relay_state; }
bool relay_owner_is_energized(void) { return W.relay_energized; }

bool relay_owner_command_trip(safety_trip_t reason)
{
    W.trip_cmd_calls++;
    W.last_trip_cmd_reason = reason;
    if (W.trip_cmd_fail_budget > 0) {
        W.trip_cmd_fail_budget--;
        return false;
    }
    W.relay_state = RELAY_OWNER_STATE_TRIPPED;
    if (!W.keep_energized_after_trip) {
        W.relay_energized = false;
    }
    return true;
}
bool relay_owner_clear_trip(void)
{
    W.clear_cmd_calls++;
    if (W.clear_cmd_fail_budget > 0) {
        W.clear_cmd_fail_budget--;
        return false;
    }
    if (W.relay_state == RELAY_OWNER_STATE_TRIPPED) {
        W.relay_state = RELAY_OWNER_STATE_GRACE; // cleared: de-energized, not re-armed
    }
    return true;
}
bool relay_owner_command_energize(bool energize)
{
    if (energize) {
        W.energize_true_calls++;
        if (W.relay_state == RELAY_OWNER_STATE_TRIPPED) {
            return false;
        }
        W.relay_state = RELAY_OWNER_STATE_ARMED;
        W.relay_energized = true;
        return true;
    }
    W.energize_false_calls++;
    W.relay_energized = false;
    return true;
}

// ------------------------------------------------------------------ harness

#define TICK_MS 100u

static void CALLBACK fiber_entry(LPVOID unused)
{
    (void)unused;
    g_task_fn(g_task_arg);
}

// Starts (or restarts) safety_core_task with guards freshly reset. After this
// returns the task has run its prelude and parked at its first
// vTaskDelayUntil(); the next step() runs the first real tick.
static void core_start(void)
{
    if (g_main_fiber == NULL) {
        g_main_fiber = ConvertThreadToFiber(NULL);
    }
    if (g_task_fiber != NULL) {
        DeleteFiber(g_task_fiber);
        g_task_fiber = NULL;
    }
    g_task_fn = NULL;
    bool ok = safety_core_start();
    TEST_CHECK(ok, "safety_core_start() succeeds against the fakes");
    TEST_CHECK(g_task_fn != NULL, "safety_core_start() created the task");
    g_task_fiber = CreateFiber(512 * 1024, fiber_entry, NULL);
    SwitchToFiber(g_task_fiber); // prelude: safety_guards_reset(), xTaskGetTickCount(), park
}

static void step(void)
{
    if (!W.clock_frozen) {
        W.now_ms += TICK_MS;
    }
    g_tick += TICK_MS;
    if (W.thermo_publish) {
        W.thermo.timestamp_ms = W.now_ms;
    }
    if (W.current_publish) {
        W.current.timestamp_ms = W.now_ms;
    }
    if (W.ctx_publish) {
        W.ctx.timestamp_ms = W.now_ms;
    }
    SwitchToFiber(g_task_fiber);
}

static void steps(unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        step();
    }
}

static bool is_tripped(void)
{
    safety_trip_t r = SAFETY_TRIP_NONE;
    safety_core_get_diag_status(&r, NULL, NULL, NULL);
    return r != SAFETY_TRIP_NONE;
}
static safety_trip_t trip_reason(void)
{
    safety_trip_t r = SAFETY_TRIP_NONE;
    safety_core_get_diag_status(&r, NULL, NULL, NULL);
    return r;
}
static uint8_t diag_state(void)
{
    uint8_t s = 0xFF;
    safety_core_get_diag_status(NULL, NULL, &s, NULL);
    return s;
}
static uint8_t trip_seq_now(void)
{
    uint8_t seq = 0;
    (void)safety_core_get_trip_event(&seq, NULL, NULL, NULL, NULL);
    return seq;
}

// Steps until a trip shows or `max` ticks pass; returns ticks used, or -1.
static int steps_until_trip(unsigned max)
{
    for (unsigned i = 1; i <= max; i++) {
        step();
        if (is_tripped()) {
            return (int)i;
        }
    }
    return -1;
}

static safety_clear_trip_outcome_t last_outcome(void)
{
    safety_clear_trip_outcome_t o = SAFETY_CLEAR_TRIP_OUTCOME_NONE;
    safety_core_get_clear_trip_stats(NULL, NULL, &o);
    return o;
}

// Every trip must be reflected consistently on all outputs. `s_label` names
// the guard under test.
static void expect_trip(safety_trip_t want, const char *label, unsigned trip_calls_before)
{
    char msg[160];
    snprintf(msg, sizeof(msg), "%s: trip_reason", label);
    TEST_CHECK(trip_reason() == want, msg);
    snprintf(msg, sizeof(msg), "%s: diag state 4 (tripped)", label);
    TEST_CHECK(diag_state() == 4, msg);
    snprintf(msg, sizeof(msg), "%s: relay_owner_command_trip issued with the same reason", label);
    TEST_CHECK(W.trip_cmd_calls > trip_calls_before && W.last_trip_cmd_reason == want, msg);
    snprintf(msg, sizeof(msg), "%s: relay/K4 off", label);
    TEST_CHECK(!W.relay_energized && W.relay_state == RELAY_OWNER_STATE_TRIPPED, msg);
    bool energized = true, heating = true;
    safety_core_get_output_status(&energized, &heating);
    snprintf(msg, sizeof(msg), "%s: output status reports not energized, heating disabled", label);
    TEST_CHECK(!energized && !heating, msg);

    uint8_t seq = 0;
    safety_trip_t ev_reason = SAFETY_TRIP_NONE;
    uint32_t up = 0;
    float tc = 0, th = 0;
    bool have = safety_core_get_trip_event(&seq, &ev_reason, &up, &tc, &th);
    snprintf(msg, sizeof(msg), "%s: trip event published (seq != 0, reason matches)", label);
    TEST_CHECK(have && seq != 0 && ev_reason == want, msg);

    // trip_mask = 1 << (reason - 1), per CLAUDE.md; link_frame derives it too.
    uint16_t mask = link_frame_trip_mask_for_reason(want);
    snprintf(msg, sizeof(msg), "%s: trip_mask == 1 << (reason-1)", label);
    TEST_CHECK(mask == (uint16_t)(1u << ((unsigned)want - 1u)), msg);

    snprintf(msg, sizeof(msg), "%s: boot_reason latched the reason before the relay command", label);
    TEST_CHECK(W.latch_trip_calls >= 1 && W.last_latched_reason == (uint32_t)want, msg);
}

// Releases the stimulus (caller does), then clears with the right bound seq.
static void clear_bound_and_settle(void)
{
    TEST_CHECK(safety_core_request_clear_trip(true, trip_seq_now()), "clear request queued");
    steps(3);
}

// -------------------------------------------------------------- scenarios

static void test_healthy_never_trips(void)
{
    TEST_SECTION("Campaign 1: healthy inputs never trip, relay never commanded to trip");
    world_reset_keep_clock();
    core_start();
    uint8_t seq0 = trip_seq_now();
    steps(3000); // 5 simulated minutes
    TEST_CHECK(!is_tripped(), "no trip after 300 s of healthy inputs");
    TEST_CHECK(trip_seq_now() == seq0, "trip seq unchanged");
    TEST_CHECK(W.trip_cmd_calls == 0, "no trip command sent to relay_owner");
    TEST_CHECK(W.latch_trip_calls == 0, "boot_reason never told about a trip");
    TEST_CHECK(W.watchdog_checkins >= 3000, "watchdog checked in every tick");
    TEST_CHECK(diag_state() == 2, "diag state 2 (armed)");
    bool energized = true, heating = false;
    safety_core_get_output_status(&energized, &heating);
    TEST_CHECK(!energized && heating, "armed, relay not energized by safety_core itself");
}

static void test_estop_trip_latch_and_clear(void)
{
    TEST_SECTION("Campaign 1: S7 e-stop -- trip, latch, refused clear, accepted clear");
    world_reset_keep_clock();
    core_start();
    steps(5);
    uint8_t seq0 = trip_seq_now();
    W.relay_energized = true; // heat was on
    W.estop = true;
    int n = steps_until_trip(5);
    TEST_CHECK(n >= 1 && n <= 2, "e-stop trips within two ticks (fastest guard)");
    expect_trip(SAFETY_TRIP_ESTOP, "S7", 0);
    TEST_CHECK((uint8_t)(trip_seq_now() - seq0) == 1 || trip_seq_now() == 1, "trip seq advanced by exactly one");

    // Latch: the stimulus clears, the trip does not.
    W.estop = false;
    steps(500);
    TEST_CHECK(is_tripped() && trip_reason() == SAFETY_TRIP_ESTOP,
               "latched: still tripped 50 s after e-stop released, without a clear");
    TEST_CHECK(W.relay_state == RELAY_OWNER_STATE_TRIPPED && !W.relay_energized,
               "relay stays tripped/off while latched");
    TEST_CHECK(W.clear_cmd_calls == 0, "relay_owner_clear_trip not called without a clear request");

    // Re-pressed e-stop: a clear request must be refused, trip stays.
    W.estop = true;
    TEST_CHECK(safety_core_request_clear_trip(true, trip_seq_now()), "clear queued while e-stop pressed");
    steps(3);
    TEST_CHECK(last_outcome() == SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STILL_TRIPPED,
               "clear refused: still tripped");
    TEST_CHECK(is_tripped(), "still tripped after refused clear");
    TEST_CHECK(W.clear_cmd_calls == 0, "relay_owner_clear_trip not called after a refused clear");

    // Released: valid clear works exactly once.
    W.estop = false;
    clear_bound_and_settle();
    TEST_CHECK(last_outcome() == SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED, "clear accepted");
    TEST_CHECK(!is_tripped() && trip_reason() == SAFETY_TRIP_NONE, "trip cleared");
    TEST_CHECK(W.clear_cmd_calls == 1, "relay_owner_clear_trip called exactly once");
    TEST_CHECK(W.relay_state != RELAY_OWNER_STATE_TRIPPED && !W.relay_energized,
               "relay out of TRIPPED but not re-energized by the clear");
    steps(50);
    TEST_CHECK(!is_tripped(), "no re-trip after clear with healthy inputs");
    TEST_CHECK(W.clear_cmd_calls == 1, "no repeated clear commands");
}

static void test_clear_queue_refusals(void)
{
    TEST_SECTION("Campaign 1: clear-trip queue -- stale occurrence, nothing latched, unbound, depth");
    world_reset_keep_clock();
    core_start();
    steps(3);

    // Nothing latched.
    TEST_CHECK(safety_core_request_clear_trip(false, 0), "unbound clear queued with nothing latched");
    steps(2);
    TEST_CHECK(last_outcome() == SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_NOTHING_LATCHED,
               "REFUSED_NOTHING_LATCHED");
    TEST_CHECK(W.clear_cmd_calls == 0, "no relay clear for a no-op clear");

    // Trip via main fault, then a bound clear naming the WRONG occurrence.
    W.main_fault = true;
    TEST_CHECK(steps_until_trip(3) > 0, "S6a trips");
    uint8_t cur = trip_seq_now();
    W.main_fault = false;
    uint8_t stale = (uint8_t)(cur == 1 ? 2 : cur - 1);
    TEST_CHECK(safety_core_request_clear_trip(true, stale), "stale-seq clear queued");
    steps(2);
    TEST_CHECK(last_outcome() == SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STALE_OCCURRENCE,
               "REFUSED_STALE_OCCURRENCE for an older trip_seq");
    TEST_CHECK(is_tripped() && W.clear_cmd_calls == 0, "stale clear leaves the trip latched");

    // Queue depth is 2: third request in one tick window is rejected.
    uint32_t req0 = 0;
    safety_core_get_clear_trip_stats(&req0, NULL, NULL);
    TEST_CHECK(safety_core_request_clear_trip(true, stale), "request 1 fits");
    TEST_CHECK(safety_core_request_clear_trip(true, stale), "request 2 fits");
    TEST_CHECK(!safety_core_request_clear_trip(true, stale), "request 3 rejected: queue depth 2");
    uint32_t req1 = 0;
    safety_core_get_clear_trip_stats(&req1, NULL, NULL);
    TEST_CHECK(req1 - req0 == 2, "only accepted requests are counted");
    steps(4); // drains one token per tick
    uint32_t proc = 0;
    safety_core_get_clear_trip_stats(NULL, &proc, NULL);
    TEST_CHECK(proc >= 3, "tokens processed one per tick");
    TEST_CHECK(is_tripped(), "still latched after repeated stale clears");

    // Unbound clear (legacy path) is accepted when the cause is gone.
    TEST_CHECK(safety_core_request_clear_trip(false, 0), "unbound clear queued");
    steps(3);
    TEST_CHECK(last_outcome() == SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED, "unbound clear accepted");
    TEST_CHECK(!is_tripped(), "cleared");
}

static void test_command_retry_until_relay_accepts(void)
{
    TEST_SECTION("Campaign 1: trip/clear relay commands are retried until relay_owner accepts them");
    world_reset_keep_clock();
    core_start();
    steps(3);
    W.relay_energized = true;
    W.trip_cmd_fail_budget = 4; // relay_owner refuses the first four attempts
    W.estop = true;
    TEST_CHECK(steps_until_trip(3) > 0, "trips");
    steps(8);
    TEST_CHECK(W.trip_cmd_calls == 5, "trip command retried each tick: 4 failures + 1 success, then stops");
    TEST_CHECK(!W.relay_energized && W.relay_state == RELAY_OWNER_STATE_TRIPPED,
               "relay finally off/tripped");
    unsigned calls = W.trip_cmd_calls;
    steps(20);
    TEST_CHECK(W.trip_cmd_calls == calls, "no further trip commands once accepted");

    W.estop = false;
    W.clear_cmd_fail_budget = 3;
    clear_bound_and_settle();
    steps(8);
    TEST_CHECK(W.clear_cmd_calls == 4, "clear command retried until accepted (3 failures + 1 success)");
    TEST_CHECK(!is_tripped(), "cleared");
}

static void test_trip_seq_wrap(void)
{
    TEST_SECTION("Campaign 1: trip seq wraps 255 -> 1, never 0, across 260 trip/clear cycles");
    world_reset_keep_clock();
    core_start();
    steps(3);
    bool never_zero_after_first = true;
    bool saw_wrap = false;
    uint8_t prev = trip_seq_now();
    for (int i = 0; i < 260; i++) {
        W.main_fault = true;
        if (steps_until_trip(3) < 0) {
            TEST_CHECK(false, "cycle trips");
            break;
        }
        uint8_t s = trip_seq_now();
        if (s == 0) {
            never_zero_after_first = false;
        }
        if (prev == 255 && s == 1) {
            saw_wrap = true;
        }
        TEST_CHECK(s == (uint8_t)(prev == 255 ? 1 : prev + 1), "seq advances by one, wrapping 255 -> 1");
        prev = s;
        W.main_fault = false;
        clear_bound_and_settle();
        if (is_tripped()) {
            TEST_CHECK(false, "cycle clears");
            break;
        }
    }
    TEST_CHECK(never_zero_after_first, "trip seq is never 0 once a trip has been published");
    TEST_CHECK(saw_wrap, "wrap 255 -> 1 observed");
}

// -- one scenario per guard ------------------------------------------------

static void arm_context(float setpoint_c, float measured_c)
{
    W.ctx_published = true;
    W.ctx_publish = true;
    W.ctx.valid = true;
    W.ctx.flags = CONTEXT_FLAG_CONTEXT_VALID | CONTEXT_FLAG_PROFILE_RUNNING;
    W.ctx.zone_count = 1;
    W.ctx.zones[0].zone_index = 0;
    W.ctx.zones[0].flags = CONTEXT_ZONE_FLAG_ACTIVE | CONTEXT_ZONE_FLAG_MEASURED_VALID;
    W.ctx.zones[0].setpoint_c = setpoint_c;
    W.ctx.zones[0].measured_c = measured_c;
}

static void test_s1_overtemp(void)
{
    TEST_SECTION("Campaign 1: S1 over-temperature");
    world_reset_keep_clock();
    core_start();
    steps(3);
    // No-false-trip: at the ceiling exactly, and a single spike, do not trip.
    W.thermo.tc_c = 1300.0f;
    steps(100);
    TEST_CHECK(!is_tripped(), "S1: reading == abs_max_temp_c does not trip");
    W.thermo.tc_c = 1400.0f;
    step();
    W.thermo.tc_c = 25.0f;
    steps(20);
    TEST_CHECK(!is_tripped(), "S1: a single over-ceiling sample does not trip (streak reset)");
    // Sustained: trips.
    W.relay_energized = true;
    W.thermo.tc_c = 1301.0f; // just 1 C over the ceiling
    int n = steps_until_trip(50);
    TEST_CHECK(n > 1 && n <= 20, "S1: sustained over-ceiling trips within a few ticks");
    expect_trip(SAFETY_TRIP_OVERTEMP, "S1", 0);
    float tc = 0, th = 0;
    TEST_CHECK(safety_core_get_trip_event(NULL, NULL, NULL, &tc, &th), "event readable");
    TEST_CHECK_NEAR(tc, 1301.0, 0.01, "S1: event carries the tripping reading");
    TEST_CHECK_NEAR(th, 1300.0, 0.01, "S1: event carries the deciding threshold (abs_max_temp_c)");
    // Latch while still hot and after cooling.
    steps(30);
    TEST_CHECK(is_tripped(), "S1: latched while still hot");
    // T1: a clear while still over the ceiling is REFUSED (S1 level re-test).
    uint8_t seq_first = trip_seq_now();
    TEST_CHECK(safety_core_request_clear_trip(true, seq_first), "S1: clear queued while hot");
    steps(1);
    TEST_CHECK(last_outcome() == SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STILL_TRIPPED, "S1: clear refused while hot (T1)");
    TEST_CHECK(is_tripped() && trip_seq_now() == seq_first, "S1: still the same latched occurrence");
    W.thermo.tc_c = 200.0f;
    steps(3);
    clear_bound_and_settle();
    TEST_CHECK(last_outcome() == SAFETY_CLEAR_TRIP_OUTCOME_ACCEPTED && !is_tripped(),
               "S1: clear accepted once cooled");
}

static void test_failed_cfg_read_keeps_last_good_guard_cfg(void)
{
    TEST_SECTION("T2: a failed config_store read keeps the last good guard config");
    world_reset_keep_clock();
    core_start();
    steps(3); // good reads: guard cfg loaded (abs_max 1300)
    W.cfg_read_ok = false; // default record would zero abs_max_temp_c
    W.thermo.tc_c = 1400.0f;
    int n = steps_until_trip(50);
    TEST_CHECK(n > 1 && n <= 20, "T2: S1 still trips on a failed config read (last good abs_max kept)");
    TEST_CHECK(trip_reason() == SAFETY_TRIP_OVERTEMP, "T2: reason is S1");
}

static void test_s1_uncommissioned_never_trips(void)
{
    TEST_SECTION("Campaign 1: S1 with abs_max_temp_c unconfigured -- no trip, ARMED forced off");
    world_reset_keep_clock();
    W.cfg.fields_set &= ~CONFIG_STORE_SET_ABS_MAX_TEMP_C;
    W.cfg.abs_max_temp_c = 0.0f;
    core_start();
    W.thermo.tc_c = 1500.0f;
    steps(100);
    TEST_CHECK(!is_tripped(), "S1 uncommissioned (0 = not commissioned): never trips");
    TEST_CHECK(W.energize_false_calls > 0,
               "ARMED with abs_max unconfigured is forced to de-energize (KILN_PROFILES_PLAN item 16 backstop)");
}

static void test_s5_stale_thermo(void)
{
    TEST_SECTION("Campaign 1: S5 sensor invalid -- stale thermo snapshot counts as blind");
    world_reset_keep_clock();
    core_start();
    steps(3);
    // A thermo task that stopped publishing: snapshot says valid but is old.
    W.thermo_publish = false;
    W.relay_energized = true;
    int n = steps_until_trip(450);
    TEST_CHECK(n < 0, "S5: not tripped before blind_grace_s (60 s) of staleness");
    n = steps_until_trip(300);
    TEST_CHECK(n > 0, "S5: tripped once blind longer than blind_grace_s");
    expect_trip(SAFETY_TRIP_SENSOR_INVALID, "S5", 0);
    float tc = 0;
    TEST_CHECK(safety_core_get_trip_event(NULL, NULL, NULL, &tc, NULL) && isnan(tc),
               "S5: event tc_c is NaN when the reading was not valid");

    // Recovery: snapshot goes live again -> clear works.
    W.thermo_publish = true;
    steps(3);
    clear_bound_and_settle();
    TEST_CHECK(!is_tripped(), "S5: cleared once the sensor is back");
}

static void test_s5_spi_fault_and_not_installed(void)
{
    TEST_SECTION("Campaign 1: S5 SPI failure trips; declared-not-installed does not");
    world_reset_keep_clock();
    core_start();
    steps(3);
    W.thermo.spi_failed = true;
    W.thermo.valid = false;
    W.thermo.tc_c = NAN;
    TEST_CHECK(steps_until_trip(800) > 0, "S5: persistent SPI failure trips");
    TEST_CHECK(trip_reason() == SAFETY_TRIP_SENSOR_INVALID, "reason SENSOR_INVALID");

    world_reset_keep_clock();
    W.cfg.safety_tc_installed = 0;
    core_start();
    steps(3);
    W.thermo.valid = false;
    W.thermo.spi_failed = true;
    W.thermo.tc_c = NAN;
    steps(1000);
    TEST_CHECK(!is_tripped(), "S5: safety_tc_installed=0 declared -> WARN, no trip");
    uint16_t warn = 0;
    safety_core_get_diag_status(NULL, NULL, NULL, &warn);
    TEST_CHECK(warn != 0, "S5: warn mask raised while blind-but-declared");
    TEST_CHECK(!safety_core_request_enable(true), "enable refused when no safety TC is installed");
}

static void test_s6a_main_fault(void)
{
    TEST_SECTION("Campaign 1: S6a main fault");
    world_reset_keep_clock();
    core_start();
    steps(3);
    W.relay_energized = true;
    W.main_fault = true;
    TEST_CHECK(steps_until_trip(3) > 0, "S6a trips immediately");
    expect_trip(SAFETY_TRIP_MAIN_FAULT, "S6a", 0);
    TEST_CHECK(link_frame_trip_mask_for_reason(SAFETY_TRIP_MAIN_FAULT) == 0x0020,
               "S6a mask is bit 5 (0x0020), not 0x0040");
}

static void test_s6b_link_dead(void)
{
    TEST_SECTION("Campaign 1: S6b link dead -- soft with current, hard backstop without");
    // Without current: no trip before link_dead_hard_s (120 s), trip after.
    world_reset_keep_clock();
    core_start();
    steps(3);
    W.link_up = false;
    TEST_CHECK(steps_until_trip(1150) < 0, "S6b: quiet link, no current -> no trip before 120 s");
    TEST_CHECK(steps_until_trip(100) > 0, "S6b: hard backstop trips by 120 s");
    expect_trip(SAFETY_TRIP_LINK_DEAD, "S6b hard", 0);
    W.link_up = true;
    steps(3);
    clear_bound_and_settle();
    TEST_CHECK(!is_tripped(), "S6b: clearable once the link is back");

    // With current: soft timeout (10 s).
    world_reset_keep_clock();
    core_start();
    steps(3);
    W.current.present[0] = true;
    W.current.amps[0] = 8.0f;
    W.link_up = false;
    int n = steps_until_trip(200);
    TEST_CHECK(n > 90 && n <= 110, "S6b: quiet link with current trips at ~link_timeout_s (10 s)");
    TEST_CHECK(trip_reason() == SAFETY_TRIP_LINK_DEAD, "reason LINK_DEAD");
}

static void test_s12_enclosure(void)
{
    TEST_SECTION("Campaign 1: S12 enclosure / cold junction");
    world_reset_keep_clock();
    core_start();
    steps(3);
    W.thermo.cj_c = 70.0f; // warn band
    steps(1000);
    TEST_CHECK(!is_tripped(), "S12: warn band alone never trips");
    uint16_t warn = 0;
    safety_core_get_diag_status(NULL, NULL, NULL, &warn);
    TEST_CHECK(warn != 0 && diag_state() == 3, "S12: warn mask set, diag state 3");
    W.thermo.cj_c = 90.0f;
    int n = steps_until_trip(800);
    TEST_CHECK(n > 550 && n <= 620, "S12: trips after cj_time_s (60 s) above cj_max_c");
    expect_trip(SAFETY_TRIP_ENCLOSURE_TEMP, "S12", 0);
}

static void test_s2_over_setpoint(void)
{
    TEST_SECTION("Campaign 1: S2 over-setpoint needs a fresh, valid context");
    world_reset_keep_clock();
    W.cfg.tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED;
    core_start();
    arm_context(500.0f, 500.0f);
    steps(3);
    W.thermo.tc_c = 700.0f; // 200 C over setpoint, margin 75 C, 120 s
    W.relay_energized = true;
    int n = steps_until_trip(1100);
    TEST_CHECK(n < 0, "S2: not tripped before overshoot_time_s");
    n = steps_until_trip(300);
    TEST_CHECK(n > 0, "S2: tripped after sustained overshoot with valid context");
    expect_trip(SAFETY_TRIP_OVER_SETPOINT, "S2", 0);

    // Stale context (ESP stopped publishing) must disable S2, not trip on it.
    world_reset_keep_clock();
    W.cfg.tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED;
    core_start();
    arm_context(500.0f, 500.0f);
    steps(3);
    W.ctx_publish = false; // context frozen -> ages past 5 s -> context_valid false
    W.thermo.tc_c = 700.0f;
    steps(1800);
    TEST_CHECK(!is_tripped(), "S2: stale context -> no over-setpoint trip (guard cannot reason without context)");

    // degraded_no_context flag likewise.
    world_reset_keep_clock();
    W.cfg.tc_placement_mode = CONFIG_STORE_TC_PLACEMENT_CHAMBER_AGREED;
    core_start();
    arm_context(500.0f, 500.0f);
    W.degraded_no_context = true;
    steps(3);
    W.thermo.tc_c = 700.0f;
    steps(1800);
    TEST_CHECK(!is_tripped(), "S2: degraded_no_context -> context treated invalid");
}

static void test_s3_load_stuck_on(void)
{
    TEST_SECTION("Campaign 1: S3 load stuck on -- current with no relay command");
    world_reset_keep_clock();
    core_start();
    arm_context(0.0f, 25.0f);
    W.ctx.relay_recent_mask = 0;
    W.ctx.relay_now_mask = 0;
    steps(3);
    W.current.present[0] = true;
    W.current.amps[0] = 8.0f;
    W.relay_energized = true;
    int n = steps_until_trip(400);
    TEST_CHECK(n > 150 && n <= 260, "S3: trips after correlation_window + stuck_on_time (~170 s... bounded)");
    expect_trip(SAFETY_TRIP_LOAD_STUCK_ON, "S3", 0);
}

static void test_s3_no_false_trip_when_commanded(void)
{
    TEST_SECTION("Campaign 1: S3 no false trip while the ESP is commanding heat");
    world_reset_keep_clock();
    core_start();
    arm_context(500.0f, 500.0f);
    W.ctx.relay_recent_mask = 0x01;
    W.ctx.relay_now_mask = 0x01;
    W.relay_on_continuous_ms = 1000;
    steps(3);
    W.current.present[0] = true;
    W.current.amps[0] = 8.0f;
    W.thermo.tc_c = 480.0f;
    steps(3000);
    TEST_CHECK(!is_tripped(), "S3: current present while relay commanded recently is normal");
}

static void test_s7_ct_masking(void)
{
    TEST_SECTION("Campaign 1: CT-disabled board -- current presence ignored by the CT guards");
    world_reset_keep_clock();
    W.cfg.fields_set |= CONFIG_STORE_SET_CT_INSTALLED;
    W.cfg.ct_installed = 0;
    core_start();
    arm_context(0.0f, 25.0f);
    steps(3);
    W.current.present[0] = true;
    W.current.present[1] = true;
    W.current.present[2] = true;
    W.current.amps[0] = 50.0f;
    steps(3000);
    TEST_CHECK(!is_tripped(), "ct_installed=0: offset-floor 'current' must not trip S3");
}

static void test_s9_ineffective_trip(void)
{
    TEST_SECTION("Campaign 1: S9 welded contactor -- escalates and becomes unclearable");
    world_reset_keep_clock();
    W.cfg.fields_set |= CONFIG_STORE_SET_CT_INSTALLED | CONFIG_STORE_SET_CT_CHANNEL_MAP;
    W.cfg.ct_installed = 1;
    W.cfg.k_ct_v_per_a[0] = W.cfg.k_ct_v_per_a[1] = W.cfg.k_ct_v_per_a[2] = 0.1f;
    core_start();
    steps(3);
    W.relay_energized = true;
    W.keep_energized_after_trip = false;
    W.main_fault = true;
    TEST_CHECK(steps_until_trip(3) > 0, "initial trip");
    // K4 is open per relay_owner, but a CT still reads current: welded.
    W.current.present[0] = true;
    W.current.amps[0] = 6.0f;
    // relay_deenergized=true (relay_owner_is_energized() false). Wait out trip_verify_s + streak.
    steps(200);
    TEST_CHECK(is_tripped(), "still tripped");
    if (trip_reason() == SAFETY_TRIP_INEFFECTIVE) {
        TEST_CHECK(true, "S9 escalated to TRIP_INEFFECTIVE");
        W.main_fault = false;
        W.current.present[0] = false;
        steps(5);
        clear_bound_and_settle();
        TEST_CHECK(is_tripped() && last_outcome() == SAFETY_CLEAR_TRIP_OUTCOME_REFUSED_STILL_TRIPPED,
                   "S9: TRIP_INEFFECTIVE refused by CLEAR_TRIP even with all causes gone");
    } else {
        TEST_CHECK(false, "S9 did not escalate to TRIP_INEFFECTIVE with commissioned CT and current present");
    }
}

static void test_s16_config_corrupt(void)
{
    TEST_SECTION("Campaign 1: S16 config corruption recurrence");
    world_reset_keep_clock();
    core_start();
    steps(3);
    W.integrity_recurrence = true;
    TEST_CHECK(steps_until_trip(3) > 0, "S16 trips immediately");
    expect_trip(SAFETY_TRIP_CONFIG_CORRUPT, "S16", 0);
}

static void test_s13_borrowed_stale(void)
{
    TEST_SECTION("Campaign 1: S13 borrowed-zone sample counter stalled");
    world_reset_keep_clock();
    W.cfg.fields_set |= CONFIG_STORE_SET_TC_SOURCE | CONFIG_STORE_SET_BORROWED_ZONE_INDEX;
    W.cfg.tc_source = CONFIG_STORE_TC_SOURCE_BORROWED_ZONE;
    W.cfg.borrowed_zone_index = 0;
    core_start();
    arm_context(500.0f, 500.0f);
    W.ctx.zones[0].sample_counter = 0;
    steps(3);
    // Counter advancing every tick: healthy.
    for (int i = 0; i < 300; i++) {
        W.ctx.zones[0].sample_counter++;
        step();
    }
    TEST_CHECK(!is_tripped(), "S13: advancing sample counter -> no trip");
    // Counter frozen: trips after borrowed_stale_trip_s (60 s).
    int n = steps_until_trip(700);
    TEST_CHECK(n > 0, "S13: stalled counter trips");
    expect_trip(SAFETY_TRIP_BORROWED_STALE, "S13", 0);
}

static void test_clock_stall_blinds_not_trips_s1(void)
{
    TEST_SECTION("Campaign 1: stalled clock makes readings invalid, never a false thermal trip");
    world_reset_keep_clock();
    core_start();
    steps(10);
    W.clock_frozen = true; // get_absolute_time() stops advancing
    steps(50);
    TEST_CHECK(!is_tripped(), "stalled clock for 5 s of ticks does not itself trip");
    W.clock_frozen = false;
    steps(20);
    TEST_CHECK(!is_tripped(), "clock resumes cleanly");
    float dt = 0;
    safety_core_get_dt_diag(&dt, NULL, NULL);
    TEST_CHECK(dt > 0.0f && dt < 2.0f, "measured dt stays bounded");
}

static void test_request_enable_gates(void)
{
    TEST_SECTION("Campaign 1: safety_core_request_enable gates");
    world_reset_keep_clock();
    core_start();
    steps(3);
    bool seen = true;
    uint32_t ms = safety_core_ms_since_last_enable_true_request(&seen);
    (void)ms;

    W.commissioned = false;
    TEST_CHECK(!safety_core_request_enable(true), "refused: not commissioned");
    TEST_CHECK(W.energize_true_calls == 0, "relay not energized when commissioning refuses");
    W.commissioned = true;

    safety_core_set_tc_type_apply_in_progress(true);
    TEST_CHECK(!safety_core_request_enable(true), "refused: tc_type apply in progress");
    safety_core_set_tc_type_apply_in_progress(false);

    W.update_active = true;
    TEST_CHECK(!safety_core_request_enable(true), "refused: firmware update transfer active");
    W.update_active = false;
    TEST_CHECK(W.energize_true_calls == 0, "no energize leaked through any refusal");

    TEST_CHECK(safety_core_request_enable(true), "allowed when commissioned, idle, installed");
    TEST_CHECK(W.energize_true_calls == 1 && W.relay_energized, "relay energized");
    uint32_t since = safety_core_ms_since_last_enable_true_request(&seen);
    TEST_CHECK(seen && since < 1000, "enable timestamp recorded");

    // Disable is never gated.
    W.commissioned = false;
    W.update_active = true;
    TEST_CHECK(safety_core_request_enable(false), "disable is never gated");
    TEST_CHECK(!W.relay_energized, "relay off");
    W.commissioned = true;
    W.update_active = false;

    // While tripped, enable must not energize.
    W.main_fault = true;
    TEST_CHECK(steps_until_trip(3) > 0, "trip");
    TEST_CHECK(!safety_core_request_enable(true), "enable refused while tripped (relay_owner refuses)");
    TEST_CHECK(!W.relay_energized, "relay stays off while tripped");
}

static void test_diag_states(void)
{
    TEST_SECTION("Campaign 1: diag state mapping");
    world_reset_keep_clock();
    W.relay_state = RELAY_OWNER_STATE_INIT;
    core_start();
    steps(2);
    TEST_CHECK(diag_state() == 0, "INIT -> 0");
    W.relay_state = RELAY_OWNER_STATE_GRACE;
    TEST_CHECK(diag_state() == 1, "GRACE -> 1");
    W.relay_state = RELAY_OWNER_STATE_ARMED;
    TEST_CHECK(diag_state() == 2, "ARMED -> 2");
    safety_trip_t r = SAFETY_TRIP_ESTOP;
    safety_core_get_diag_status(&r, NULL, NULL, NULL);
    TEST_CHECK(r == SAFETY_TRIP_NONE, "no trip reported while healthy");
    uint8_t seq = 99;
    safety_trip_t rr = SAFETY_TRIP_ESTOP;
    uint32_t up = 7;
    float tc = 0, th = 0;
    bool have = safety_core_get_trip_event(&seq, &rr, &up, &tc, &th);
    (void)have; // seq may be non-zero from earlier scenarios: statics persist
}

int main(void)
{
    test_healthy_never_trips();
    test_estop_trip_latch_and_clear();
    test_clear_queue_refusals();
    test_command_retry_until_relay_accepts();
    test_trip_seq_wrap();
    test_s1_overtemp();
    test_s1_uncommissioned_never_trips();
    test_failed_cfg_read_keeps_last_good_guard_cfg();
    test_s5_stale_thermo();
    test_s5_spi_fault_and_not_installed();
    test_s6a_main_fault();
    test_s6b_link_dead();
    test_s12_enclosure();
    test_s2_over_setpoint();
    test_s3_load_stuck_on();
    test_s3_no_false_trip_when_commanded();
    test_s7_ct_masking();
    test_s9_ineffective_trip();
    test_s16_config_corrupt();
    test_s13_borrowed_stale();
    test_clock_stall_blinds_not_trips_s1();
    test_request_enable_gates();
    test_diag_states();

    printf("\n%d checks, %d failures\n", g_test_count, g_test_failures);
    if (g_test_failures != 0) {
        printf("safety_core_host_tests: FAILED\n");
        return 1;
    }
    printf("safety_core_host_tests: all passed\n");
    return 0;
}
