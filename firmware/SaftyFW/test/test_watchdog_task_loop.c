// Host tests for the REAL watchdog_task.c (docs/audits/
// HOST_TEST_COVERAGE_GAPS_2026-10-09.md, campaign 4).
//
// watchdog_gate.c's pure comparison already has host tests. What had none is
// the task around it: the per-task deadline table, the tick-to-millisecond
// elapsed computation (including the unsigned tick wrap), the decision to
// withhold the hardware feed, the overdue-forensics latch, the LED, and the
// end-to-end consequence: a task that stops checking in makes the 1000 ms
// hardware watchdog (fake_wdt) fire. watchdog_task_feed_if_all_within_
// deadline() is called directly for boundary timing; watchdog_task_fn() is
// driven through the task harness (vTaskDelayUntil hook) for the loop.
#include <stdio.h>
#include <string.h>

#include "test_common.h"
#include "task_harness.h"

#include "board_pins.h"
#include "fake_gpio.h"
#include "fake_scratch.h"
#include "fake_wdt.h"
#include "hal_wdt.h"

#include "task_priorities.h"
#include "tasks/watchdog_task.h"
#include "watchdog_overdue_diag.h"
#include "watchdog_overdue_diag_codec.h"

#define WDT_TIMEOUT_MS 1000u
#define NLOOPS_MAX 64

// Mirrors watchdog_task.c's deadline table (id order). A change to the
// table's numbers must change this mirror on purpose.
static const uint32_t k_deadline_ms[WATCHDOG_CHECKIN_COUNT] = {
    600u, 300u, 30u, 700u, 150u, 30u, 700u, 300u,
};
static const char *const k_name[WATCHDOG_CHECKIN_COUNT] = {
    "relay_owner", "safety_core", "discrete_task", "thermo_task",
    "current_task", "link_task", "log_task", "update_task",
};

static void fresh(uint32_t boot_tick)
{
    th_reset();
    fake_wdt_reset_all();
    fake_gpio_reset();
    fake_scratch_reset_all();
    (void)hal_wdt_init(WDT_TIMEOUT_MS, false, false);
    th_set_tick(boot_tick);
    (void)watchdog_task_start();
}

// Check in every task except up to two skipped ids (-1 = none).
static void checkin_all_except(int skip_a, int skip_b)
{
    for (int i = 0; i < WATCHDOG_CHECKIN_COUNT; i++) {
        if (i != skip_a && i != skip_b) {
            watchdog_task_checkin((watchdog_checkin_id_t)i);
        }
    }
}

// ---------------------------------------------------------------------------

static void test_start_state(void)
{
    TEST_SECTION("watchdog_task_start() -- initial state");
    fresh(5000u);
    TEST_CHECK(th_captured_task_fn() != NULL, "start creates the watchdog task");
    TEST_CHECK(!watchdog_task_all_checked_in_since_boot(), "nothing has checked in yet at boot");
    TEST_CHECK(!fake_gpio_current_level(SAFTYFW_PIN_HEARTBEAT_LED), "heartbeat LED starts off");
    TEST_CHECK(fake_gpio_current_dir(SAFTYFW_PIN_HEARTBEAT_LED) == HAL_GPIO_DIR_OUT, "heartbeat LED pin is an output");

    // Boot grace: every task's last check-in is the boot tick, so a feed
    // immediately after start is allowed (no task is overdue yet).
    uint32_t feeds = fake_wdt_get_feed_count();
    TEST_CHECK(watchdog_task_feed_if_all_within_deadline(), "right after boot nothing is overdue");
    TEST_CHECK(fake_wdt_get_feed_count() == feeds + 1u, "and the hardware watchdog is fed");
    TEST_CHECK(!watchdog_task_all_checked_in_since_boot(), "feeding does not count as a check-in");
}

static void test_all_checked_in_mask(void)
{
    TEST_SECTION("watchdog_task_all_checked_in_since_boot() -- needs every task");
    fresh(100u);
    for (int i = 0; i < WATCHDOG_CHECKIN_COUNT - 1; i++) {
        watchdog_task_checkin((watchdog_checkin_id_t)i);
    }
    TEST_CHECK(!watchdog_task_all_checked_in_since_boot(), "7 of 8 tasks is not all");
    watchdog_task_checkin((watchdog_checkin_id_t)(WATCHDOG_CHECKIN_COUNT - 1));
    TEST_CHECK(watchdog_task_all_checked_in_since_boot(), "all 8 tasks checked in");

    fresh(100u);
    watchdog_task_checkin((watchdog_checkin_id_t)WATCHDOG_CHECKIN_COUNT);
    watchdog_task_checkin((watchdog_checkin_id_t)200);
    checkin_all_except(WATCHDOG_CHECKIN_LOG_TASK, -1);
    TEST_CHECK(!watchdog_task_all_checked_in_since_boot(),
               "out-of-range ids are ignored: they cannot stand in for a missing task");
}

// For one task: deadline-1 and exact deadline feed, deadline+1 withholds.
// Every other task is kicked at the evaluation tick, so only `id` can be overdue.
static void check_boundary(int id, uint32_t base)
{
    char msg[200];
    uint32_t d = k_deadline_ms[id];

    fresh(base);
    watchdog_task_checkin((watchdog_checkin_id_t)id); // last kick at `base`

    th_set_tick(base + d - 1u);
    checkin_all_except(id, -1);
    uint32_t feeds = fake_wdt_get_feed_count();
    snprintf(msg, sizeof(msg), "%s base=%u: elapsed = deadline-1 (%u ms) feeds", k_name[id], (unsigned)base, (unsigned)(d - 1u));
    TEST_CHECK(watchdog_task_feed_if_all_within_deadline() && fake_wdt_get_feed_count() == feeds + 1u, msg);

    th_set_tick(base + d);
    checkin_all_except(id, -1);
    feeds = fake_wdt_get_feed_count();
    snprintf(msg, sizeof(msg), "%s base=%u: elapsed = deadline (%u ms) still feeds (no false trip at the boundary)", k_name[id], (unsigned)base, (unsigned)d);
    TEST_CHECK(watchdog_task_feed_if_all_within_deadline() && fake_wdt_get_feed_count() == feeds + 1u, msg);

    th_set_tick(base + d + 1u);
    checkin_all_except(id, -1);
    feeds = fake_wdt_get_feed_count();
    bool ok = watchdog_task_feed_if_all_within_deadline();
    snprintf(msg, sizeof(msg), "%s base=%u: elapsed = deadline+1 withholds the feed", k_name[id], (unsigned)base);
    TEST_CHECK(!ok && fake_wdt_get_feed_count() == feeds, msg);

    watchdog_overdue_diag_t diag = watchdog_overdue_diag_read();
    snprintf(msg, sizeof(msg), "%s base=%u: forensic latch names exactly this task with overage 1 ms", k_name[id], (unsigned)base);
    TEST_CHECK(diag.magic_ok && diag.overdue_mask == (1u << id) && diag.worst_task_id == id &&
                   diag.worst_overage_ms == 1u,
               msg);
}

static void test_deadline_boundaries(void)
{
    TEST_SECTION("watchdog_task -- per-task deadline boundary (-1 feeds, exact feeds, +1 withholds)");
    for (int id = 0; id < WATCHDOG_CHECKIN_COUNT; id++) {
        check_boundary(id, 1000u);
    }
    TEST_SECTION("watchdog_task -- same boundaries across the 32-bit tick wrap");
    for (int id = 0; id < WATCHDOG_CHECKIN_COUNT; id++) {
        check_boundary(id, 0xFFFFFF00u);
    }
}

static void test_never_started_task(void)
{
    TEST_SECTION("watchdog_task -- a task that never checks in after boot");
    fresh(2000u);
    th_set_tick(2000u + 300u);
    checkin_all_except(WATCHDOG_CHECKIN_UPDATE_TASK, -1);
    TEST_CHECK(watchdog_task_feed_if_all_within_deadline(), "boot + 300 ms: a silent task is still inside its 300 ms boot deadline");
    th_set_tick(2000u + 301u);
    checkin_all_except(WATCHDOG_CHECKIN_UPDATE_TASK, -1);
    TEST_CHECK(!watchdog_task_feed_if_all_within_deadline(), "boot + 301 ms: a task that never started withholds the feed");
    TEST_CHECK(!watchdog_task_all_checked_in_since_boot(), "and the boot check-in mask still reports it missing");
}

static void test_diag_worst_clamp_recovery(void)
{
    TEST_SECTION("watchdog_task -- overdue forensics: worst task, tie, clamp, recovery clears it");
    watchdog_overdue_diag_t d;
    fresh(10u);

    // discrete and link never check in; both overage 970 ms; tie -> lower id.
    th_set_tick(10u + 1000u);
    checkin_all_except(WATCHDOG_CHECKIN_DISCRETE_TASK, WATCHDOG_CHECKIN_LINK_TASK);
    TEST_CHECK(!watchdog_task_feed_if_all_within_deadline(), "two stale tasks withhold the feed");
    d = watchdog_overdue_diag_read();
    TEST_CHECK(d.magic_ok && d.overdue_mask == ((1u << WATCHDOG_CHECKIN_DISCRETE_TASK) | (1u << WATCHDOG_CHECKIN_LINK_TASK)),
               "both stale tasks appear in the overdue mask");
    TEST_CHECK(d.worst_overage_ms == 970u && d.worst_task_id == WATCHDOG_CHECKIN_DISCRETE_TASK,
               "worst overage 970 ms; a tie names the lower task id");

    // discrete recovers, link stays stale: now link alone is named.
    th_set_tick(10u + 1100u);
    checkin_all_except(WATCHDOG_CHECKIN_LINK_TASK, -1);
    TEST_CHECK(!watchdog_task_feed_if_all_within_deadline(), "link alone still withholds the feed");
    d = watchdog_overdue_diag_read();
    TEST_CHECK(d.worst_task_id == WATCHDOG_CHECKIN_LINK_TASK && d.worst_overage_ms == 1100u - 30u &&
                   d.overdue_mask == (1u << WATCHDOG_CHECKIN_LINK_TASK),
               "link (1100 ms elapsed vs 30 ms deadline) is the only one named");

    // Clamp at 16 bits.
    th_set_tick(10u + 1100u + 200000u);
    checkin_all_except(WATCHDOG_CHECKIN_UPDATE_TASK, -1);
    (void)watchdog_task_feed_if_all_within_deadline();
    d = watchdog_overdue_diag_read();
    TEST_CHECK(d.worst_task_id == WATCHDOG_CHECKIN_UPDATE_TASK && d.worst_overage_ms == WATCHDOG_OVERDUE_DIAG_OVERAGE_MAX_MS,
               "a huge overage saturates at the latch format maximum (8191 ms), not wrapped");

    // Recovery: everything kicks, evaluation feeds and clears the latch.
    checkin_all_except(-1, -1);
    TEST_CHECK(watchdog_task_feed_if_all_within_deadline(), "all tasks back inside their deadlines feeds again");
    d = watchdog_overdue_diag_read();
    TEST_CHECK(!d.magic_ok, "a transient overdue that recovered leaves no stale latch for the next reset");
}

// --- loop-driven scenarios ----------------------------------------------------

typedef struct {
    int      stop_id;         // task that stops checking in (or -1)
    unsigned stop_after_loop; // first period (1-based) in which it no longer kicks
    unsigned nloops;
    bool     fired_at[NLOOPS_MAX];
    unsigned feed_count_at[NLOOPS_MAX];
    bool     led_at[NLOOPS_MAX];
} loop_scn_t;

static loop_scn_t s_scn;
static unsigned   s_loop;

// Runs after vTaskDelayUntil advanced the tick, before the task evaluates.
static void loop_delay_hook(void)
{
    if (s_loop > 0) { // outcome of the previous evaluation
        s_scn.feed_count_at[s_loop - 1] = fake_wdt_get_feed_count();
        s_scn.led_at[s_loop - 1] = fake_gpio_current_level(SAFTYFW_PIN_HEARTBEAT_LED);
    }
    if (s_loop >= s_scn.nloops) {
        th_abort();
    }
    fake_wdt_advance_ms(SAFTYFW_PERIOD_WATCHDOG_TASK_MS); // wall time passes for the chip watchdog too
    s_scn.fired_at[s_loop] = fake_wdt_fired();
    s_loop++;
    for (int i = 0; i < WATCHDOG_CHECKIN_COUNT; i++) {
        if (i == s_scn.stop_id && s_loop >= s_scn.stop_after_loop) {
            continue;
        }
        watchdog_task_checkin((watchdog_checkin_id_t)i);
    }
}

static void run_loop(int stop_id, unsigned stop_after_loop, unsigned nloops)
{
    memset(&s_scn, 0, sizeof(s_scn));
    s_scn.stop_id = stop_id;
    s_scn.stop_after_loop = stop_after_loop;
    s_scn.nloops = nloops;
    s_loop = 0;
    th_set_delay_hook(loop_delay_hook);
    th_run_captured_task();
}

static void test_healthy_loop(void)
{
    TEST_SECTION("watchdog_task_fn -- healthy loop feeds every period and blinks the LED");
    fresh(100u);
    run_loop(-1, 0, 12);
    TEST_CHECK(s_scn.feed_count_at[11] == 12u, "one hardware feed per 250 ms period");
    bool never_fired = true;
    for (unsigned i = 0; i < 12; i++) {
        never_fired = never_fired && !s_scn.fired_at[i];
    }
    TEST_CHECK(never_fired, "the 1000 ms hardware watchdog never fires while every task keeps checking in");
    bool toggles = true;
    for (unsigned i = 1; i < 12; i++) {
        toggles = toggles && (s_scn.led_at[i] != s_scn.led_at[i - 1]);
    }
    TEST_CHECK(toggles && s_scn.led_at[0], "heartbeat LED toggles every healthy period (first period turns it on)");
    TEST_CHECK(watchdog_task_all_checked_in_since_boot(), "every task checked in during the run");
}

static void test_stopped_task_resets_chip(void)
{
    TEST_SECTION("watchdog_task_fn -- a task that stops kicking withholds the feed and the hardware watchdog fires");
    enum { NL = 24 };
    for (int id = 0; id < WATCHDOG_CHECKIN_COUNT; id++) {
        char msg[220];
        fresh(100u);
        // Task `id` kicks in periods 1..4, then stops (last kick 1000 ms after start).
        run_loop(id, 5, NL);

        int first_miss = -1;
        for (unsigned i = 1; i < NL; i++) {
            if (s_scn.feed_count_at[i] == s_scn.feed_count_at[i - 1]) {
                first_miss = (int)i;
                break;
            }
        }
        // Evaluation e (0-based) runs 250*(e+1) ms after start; the stopped
        // task's elapsed there is 250*(e+1) - 1000.
        int expect_miss = -1;
        for (int e = 4; e < NL; e++) {
            if (250 * (e + 1) - 1000 > (int)k_deadline_ms[id]) {
                expect_miss = e;
                break;
            }
        }
        snprintf(msg, sizeof(msg), "%s: feed first withheld on the first evaluation past its %u ms deadline (idx %d)", k_name[id], (unsigned)k_deadline_ms[id], expect_miss);
        TEST_CHECK(first_miss == expect_miss && first_miss > 0, msg);

        bool early_fire = false;
        for (int e = 0; e <= first_miss && e < NL; e++) {
            early_fire = early_fire || s_scn.fired_at[e];
        }
        snprintf(msg, sizeof(msg), "%s: no hardware reset before the feed is first withheld", k_name[id]);
        TEST_CHECK(!early_fire, msg);

        int first_fire = -1;
        for (int e = 0; e < NL; e++) {
            if (s_scn.fired_at[e]) {
                first_fire = e;
                break;
            }
        }
        snprintf(msg, sizeof(msg), "%s: hardware watchdog fires 5 periods (>1000 ms) after the last feed (got %d)", k_name[id], first_fire - (first_miss - 1));
        TEST_CHECK(first_fire >= 0 && first_fire - (first_miss - 1) == 5, msg);

        bool frozen = true;
        for (int e = first_miss + 1; e < NL; e++) {
            frozen = frozen && (s_scn.led_at[e] == s_scn.led_at[first_miss]);
        }
        snprintf(msg, sizeof(msg), "%s: heartbeat LED freezes once the feed is withheld", k_name[id]);
        TEST_CHECK(frozen, msg);

        watchdog_overdue_diag_t d = watchdog_overdue_diag_read();
        snprintf(msg, sizeof(msg), "%s: the latch that survives the reset names this task", k_name[id]);
        TEST_CHECK(d.magic_ok && d.overdue_mask == (1u << id) && d.worst_task_id == id, msg);
    }
}

static void test_task_recovers_before_reset(void)
{
    TEST_SECTION("watchdog_task_fn -- a stall that recovers inside the hardware timeout does not reset the chip");
    fresh(100u);
    run_loop(WATCHDOG_CHECKIN_THERMO_TASK, 3, 6); // thermo (700 ms) silent from period 3
    uint32_t feeds_after_stall = fake_wdt_get_feed_count();
    watchdog_overdue_diag_t d = watchdog_overdue_diag_read();
    TEST_CHECK(d.magic_ok && d.overdue_mask == (1u << WATCHDOG_CHECKIN_THERMO_TASK), "stall is latched while it lasts");
    TEST_CHECK(!fake_wdt_fired(), "stalled for under the hardware timeout: no reset yet");

    checkin_all_except(-1, -1); // the abort left the other tasks one period stale; recover them all
    TEST_CHECK(watchdog_task_feed_if_all_within_deadline(), "once the task kicks again the next evaluation feeds");
    TEST_CHECK(fake_wdt_get_feed_count() == feeds_after_stall + 1u, "the hardware watchdog is fed again");
    d = watchdog_overdue_diag_read();
    TEST_CHECK(!d.magic_ok, "and the overdue latch is cleared");
}

void run_test_watchdog_task_loop(void)
{
    test_start_state();
    test_all_checked_in_mask();
    test_deadline_boundaries();
    test_never_started_task();
    test_diag_worst_clamp_recovery();
    test_healthy_loop();
    test_stopped_task_resets_chip();
    test_task_recovers_before_reset();
}
