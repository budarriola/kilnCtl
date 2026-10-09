// test_watchdog_gate.c -- host tests for the per-task check-in deadline gate
// (src/tasks/watchdog_gate.c/.h), the fix for the watchdog feed gate that
// used to require every registered task to land in the SAME
// SAFTYFW_PERIOD_WATCHDOG_TASK_MS (250 ms) window -- structurally impossible
// once thermo_task's own real cadence (500 ms whenever the MAX31856 is
// unconfigured) exceeds that window. See watchdog_gate.h's header comment
// and watchdog_task.c's s_checkin_deadline_ms table for the full story.
//
// watchdog_gate.c has no FreeRTOS/pico-sdk dependency, so unlike
// watchdog_task.c itself it is directly host-testable here rather than only
// reachable via a source-text scan like test_watchdog_budget_coverage.c.
#include "test_common.h"
#include "../src/tasks/watchdog_gate.h"

// A slow task (e.g. thermo_task, unconfigured: 500ms real cadence, 700ms
// deadline per watchdog_task.c) checking in less often than the watchdog's
// own 250ms evaluation period must still feed, as long as it is within ITS
// OWN deadline. This is the exact case the old one-window gate got wrong.
static void test_slow_task_within_own_deadline_feeds(void)
{
    watchdog_gate_entry_t entries[] = {
        { .elapsed_ms = 480, .deadline_ms = 700 }, // thermo_task-like: > 250ms window, < own deadline
        { .elapsed_ms = 20,  .deadline_ms = 30  }, // discrete_task-like: fast, healthy
        { .elapsed_ms = 290, .deadline_ms = 300 }, // safety_core-like: right up against its deadline, still ok
    };

    uint32_t ok_mask = 0;
    bool all_ok = watchdog_gate_all_within_deadline(entries, 3, &ok_mask);

    TEST_CHECK(all_ok, "a task silent for 480ms with a 700ms deadline (slower than the 250ms "
                        "watchdog window, but within its own allowance) must not block the feed");
    TEST_CHECK(ok_mask == 0x7u, "all three entries were within their own deadlines -- ok_mask "
                                 "should have all three bits set");
}

// A task genuinely silent past its own deadline must block the feed, even
// while every other task is healthy.
static void test_genuinely_silent_task_blocks_feed(void)
{
    watchdog_gate_entry_t entries[] = {
        { .elapsed_ms = 701, .deadline_ms = 700 }, // thermo_task-like: 1ms over its own deadline
        { .elapsed_ms = 20,  .deadline_ms = 30  }, // discrete_task-like: healthy
        { .elapsed_ms = 100, .deadline_ms = 300 }, // safety_core-like: healthy
    };

    uint32_t ok_mask = 0;
    bool all_ok = watchdog_gate_all_within_deadline(entries, 3, &ok_mask);

    TEST_CHECK(!all_ok, "a task 1ms past its own deadline must block the feed, regardless of "
                         "how healthy every other task is");
    TEST_CHECK(ok_mask == 0x6u, "the stalled entry (index 0) must be the one clear in ok_mask; "
                                 "the two healthy entries must still read as ok");
}

// A fast task (e.g. discrete_task, 10ms real period, 30ms deadline) going
// silent must be caught promptly -- it must NOT be given a slow task's
// (e.g. 700ms) grace just because a slower task exists elsewhere in the
// same evaluation. Each entry's own deadline is what matters, never the
// loosest deadline in the set.
static void test_fast_task_silence_caught_promptly_not_given_slow_grace(void)
{
    watchdog_gate_entry_t entries[] = {
        { .elapsed_ms = 40,  .deadline_ms = 30  }, // discrete_task-like: 10ms over ITS deadline
        { .elapsed_ms = 40,  .deadline_ms = 700 }, // thermo_task-like: comfortably within ITS deadline
    };

    uint32_t ok_mask = 0;
    bool all_ok = watchdog_gate_all_within_deadline(entries, 2, &ok_mask);

    TEST_CHECK(!all_ok, "discrete_task-like entry is 40ms silent against a 30ms deadline -- "
                         "must block the feed at 40ms, not be excused because another entry "
                         "with a 700ms deadline is also at 40ms and still fine");
    TEST_CHECK(ok_mask == 0x2u, "only the fast/stalled entry (index 0) should be clear; the "
                                 "slow/healthy entry (index 1) must still read as ok, proving "
                                 "the two deadlines are evaluated independently");
}

// Boundary: elapsed_ms exactly equal to deadline_ms counts as still within
// the deadline (a task landing exactly on its own deadline has not yet been
// silent LONGER than its own allowance).
static void test_elapsed_equal_to_deadline_still_ok(void)
{
    watchdog_gate_entry_t entries[] = {
        { .elapsed_ms = 700, .deadline_ms = 700 },
    };

    uint32_t ok_mask = 0;
    bool all_ok = watchdog_gate_all_within_deadline(entries, 1, &ok_mask);

    TEST_CHECK(all_ok, "elapsed_ms == deadline_ms must still count as within the deadline");
    TEST_CHECK(ok_mask == 0x1u, "the single entry should read as ok at exactly its deadline");
}

// --- watchdog_gate_feed_if_all_within_deadline() ----------------------------
//
// The gate-and-feed helper shared by watchdog_task_fn() and
// update_task_erase_slot() (2026-09-18,
// docs/audits/pico_ota_erase_watchdog_reset_2026-09-18.md). The whole point of
// it existing is that the erase loop must be able to feed the hardware
// watchdog WITHOUT acquiring the right to feed it unconditionally: a feed is
// still allowed only when every registered task is within its own deadline.
// These tests pin exactly that, since an unconditional feed would leave a
// genuinely wedged safety processor unable to reboot itself.

static unsigned s_feed_calls;

static void counting_feed(void)
{
    s_feed_calls++;
}

// The refusal case, and the reason this helper is not just "call feed()".
static void test_feed_helper_refuses_when_a_task_is_past_deadline(void)
{
    watchdog_gate_entry_t entries[] = {
        { .elapsed_ms = 701, .deadline_ms = 700 }, // 1ms over its own deadline
        { .elapsed_ms = 20,  .deadline_ms = 30  }, // healthy
        { .elapsed_ms = 100, .deadline_ms = 300 }, // healthy
    };

    s_feed_calls = 0;
    uint32_t ok_mask = 0;
    bool all_ok = watchdog_gate_feed_if_all_within_deadline(entries, 3, &ok_mask, counting_feed);

    TEST_CHECK(!all_ok, "one task past its own deadline must make the shared helper refuse");
    TEST_CHECK(s_feed_calls == 0, "the hardware feed must NOT be issued while any task is past "
                                   "its own deadline -- feeding here would prevent a wedged "
                                   "safety processor from rebooting, which is the exact "
                                   "failure mode the watchdog exists to catch");
    TEST_CHECK(ok_mask == 0x6u, "the refusal must still report WHICH entry was overdue, so the "
                                 "overdue diagnostic latch works identically for both callers");
}

// The permit case: a healthy system must actually get its feed, or the erase
// loop would starve the watchdog just as surely as feeding never having been
// wired up at all.
static void test_feed_helper_feeds_when_all_within_deadline(void)
{
    watchdog_gate_entry_t entries[] = {
        { .elapsed_ms = 480, .deadline_ms = 700 },
        { .elapsed_ms = 20,  .deadline_ms = 30  },
        { .elapsed_ms = 290, .deadline_ms = 300 },
    };

    s_feed_calls = 0;
    uint32_t ok_mask = 0;
    bool all_ok = watchdog_gate_feed_if_all_within_deadline(entries, 3, &ok_mask, counting_feed);

    TEST_CHECK(all_ok, "every task within its own deadline must permit the feed");
    TEST_CHECK(s_feed_calls == 1, "the feed must be issued exactly once when the gate permits it");
    TEST_CHECK(ok_mask == 0x7u, "all three entries should read as ok");
}

// The helper's decision must be identical to the plain gate's for the same
// input -- it is supposed to BE that gate plus an action, not a second,
// slightly different rule that can drift away from it.
static void test_feed_helper_decision_matches_plain_gate(void)
{
    const watchdog_gate_entry_t cases[][2] = {
        { { .elapsed_ms = 20,  .deadline_ms = 30 },  { .elapsed_ms = 100, .deadline_ms = 700 } },
        { { .elapsed_ms = 31,  .deadline_ms = 30 },  { .elapsed_ms = 100, .deadline_ms = 700 } },
        { { .elapsed_ms = 30,  .deadline_ms = 30 },  { .elapsed_ms = 700, .deadline_ms = 700 } },
        { { .elapsed_ms = 0,   .deadline_ms = 0  },  { .elapsed_ms = 1,   .deadline_ms = 0   } },
    };

    for (unsigned i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t plain_mask = 0;
        bool plain = watchdog_gate_all_within_deadline(cases[i], 2, &plain_mask);

        s_feed_calls = 0;
        uint32_t helper_mask = 0;
        bool helper = watchdog_gate_feed_if_all_within_deadline(cases[i], 2, &helper_mask,
                                                                counting_feed);

        TEST_CHECK(plain == helper, "the gate-and-feed helper must return the same decision the "
                                     "plain gate does for identical input -- two different rules "
                                     "is the drift this shared helper exists to prevent");
        TEST_CHECK(plain_mask == helper_mask, "the helper must report the same ok_mask the plain "
                                               "gate does for identical input");
        TEST_CHECK(s_feed_calls == (plain ? 1u : 0u), "the feed must happen if and only if the "
                                                       "plain gate would have permitted it");
    }
}

// A NULL feed must evaluate the gate and simply skip the call, never crash --
// this is the shape a caller that only wants the decision would use.
static void test_feed_helper_tolerates_null_feed(void)
{
    watchdog_gate_entry_t entries[] = {
        { .elapsed_ms = 10, .deadline_ms = 30 },
    };

    uint32_t ok_mask = 0;
    bool all_ok = watchdog_gate_feed_if_all_within_deadline(entries, 1, &ok_mask, NULL);

    TEST_CHECK(all_ok, "a NULL feed must still evaluate the gate and report its decision");
    TEST_CHECK(ok_mask == 0x1u, "a NULL feed must still report ok_mask");
}

void run_test_watchdog_gate(void)
{
    TEST_SECTION("watchdog_gate: per-task check-in deadlines (the fix for the "
                  "one-window-for-everyone gate)");
    test_slow_task_within_own_deadline_feeds();
    test_genuinely_silent_task_blocks_feed();
    test_fast_task_silence_caught_promptly_not_given_slow_grace();
    test_elapsed_equal_to_deadline_still_ok();

    TEST_SECTION("watchdog_gate: shared gate-and-feed helper (update_task's erase "
                  "loop borrows the feed, never the policy)");
    test_feed_helper_refuses_when_a_task_is_past_deadline();
    test_feed_helper_feeds_when_all_within_deadline();
    test_feed_helper_decision_matches_plain_gate();
    test_feed_helper_tolerates_null_feed();
}
