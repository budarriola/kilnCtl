// test_update_task_relay_wiring.c -- ROADMAP.md's "Link-loss heating block
// not bypassed during a Pico update" item (2026-09-04). Pins, by source-text
// scan, the half of that safety property that lives entirely on this
// processor and cannot be host-compiled: src/tasks/update_task.c pulls in
// FreeRTOS.h/pico/flash.h/hardware/flash.h/hardware/watchdog.h, none of
// which exist off real hardware, so like test_safety_core_s8_wiring.c and
// test_safety_core_polarity_wiring.c before it, this scans the ACTUAL
// compiled source text rather than restating its logic somewhere host-
// friendly.
//
// THE PROPERTY: while a Pico firmware update is in progress, nothing in
// update_task.c may command a relay ON or otherwise grant heat authority.
// The one function on this processor that can actually energize the relay
// is relay_owner_command_energize() (src/tasks/relay_owner.c: the only
// caller of hal_gpio_set(SAFTYFW_PIN_RELAY, ...) outside relay_owner_task()'s
// own internal state machine -- see relay_owner.c's own header comment,
// "every hal_gpio_set(SAFTYFW_PIN_RELAY, ...) call ... so the two can never").
// HAL Phase 1b (docs/HW_ABSTRACTION_PLAN.md) moved relay_owner.c from a raw
// gpio_put(SAFTYFW_PIN_RELAY, ...) write to hal_gpio_set(SAFTYFW_PIN_RELAY,
// ...) -- the scan target below was updated to match; see
// test_relay_owner_gpio_init.c for the new hal_gpio-backed init-order test
// this same phase added.
// update_task.c today only READS output/thermo status
// (thermo_task_get_snapshot()/current_task_get_snapshot()) to decide whether
// to gate UPDATE_BEGIN -- ROADMAP.md's own words for the property this file
// pins. A future edit that adds a relay_owner_command_energize() call, or a
// direct gpio_put(SAFTYFW_PIN_RELAY, ...), into update_task.c would silently
// give an in-progress update a path to command heat; nothing else in this
// suite would notice, since relay_owner.c's own host tests
// (test_relay_grace.c) never see update_task.c at all.
//
// FAILS CLOSED: a missing source file, a renamed relay-energize function
// that this scan can no longer find defined where expected, or a scan that
// cannot even run is a TEST FAILURE, never a skip -- a scan that silently
// matches nothing (because the file it meant to check moved) would be worse
// than no test at all.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

// Mirrors test_safety_core_s8_wiring.c's read_file_any() exactly -- same
// "different build layouts have different working directories" reasoning.
// candidates[0] is always this test's target written relative to this
// test file's OWN directory (e.g. "../src/tasks/safety_core.c") --
// test_read_source_anchored() (test_common.h) uses it to resolve an
// absolute path anchored to __FILE__ first, which works from ANY working
// directory the test binary is launched from, then falls back to the
// literal candidates[] entries as a second layer.
static char *read_file_any(const char *const *candidates, size_t count)
{
    return test_read_source_anchored(__FILE__, candidates[0], candidates, count);
}

static const char *UPDATE_TASK_CANDIDATES[] = {
    "../src/tasks/update_task.c",
    "src/tasks/update_task.c",
    "firmware/SaftyFW/src/tasks/update_task.c",
};

static const char *RELAY_OWNER_CANDIDATES[] = {
    "../src/tasks/relay_owner.c",
    "src/tasks/relay_owner.c",
    "firmware/SaftyFW/src/tasks/relay_owner.c",
};

static void test_relay_owner_command_energize_exists_where_expected(void)
{
    TEST_SECTION("sanity: relay_owner_command_energize() is actually defined in "
                 "relay_owner.c -- if this fails, the scan below is chasing a renamed "
                 "or moved function and its silence would prove nothing");

    char *text = read_file_any(RELAY_OWNER_CANDIDATES,
                                sizeof(RELAY_OWNER_CANDIDATES) / sizeof(RELAY_OWNER_CANDIDATES[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/relay_owner.c from the host test's "
                           "working directory -- update the candidate paths in this test if "
                           "the build layout moved");
        return;
    }

    TEST_CHECK(strstr(text, "bool relay_owner_command_energize(bool energize)") != NULL,
               "relay_owner_command_energize(bool) is defined in relay_owner.c with its "
               "expected signature -- this is the function update_task.c must never call");

    TEST_CHECK(strstr(text, "hal_gpio_set(SAFTYFW_PIN_RELAY") != NULL,
               "relay_owner.c itself still contains the real hal_gpio_set(SAFTYFW_PIN_RELAY, ...) "
               "write -- confirms this scan's target string actually means \"energize the relay\" "
               "on this hardware, not a stale symbol name");

    free(text);
}

static void test_update_task_never_calls_relay_owner_command_energize(void)
{
    TEST_SECTION("update_task.c never calls relay_owner_command_energize() -- the link-loss "
                 "heating block must not be bypassable by an in-progress Pico update "
                 "(ROADMAP.md 2026-09-04)");

    char *text = read_file_any(UPDATE_TASK_CANDIDATES,
                                sizeof(UPDATE_TASK_CANDIDATES) / sizeof(UPDATE_TASK_CANDIDATES[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/update_task.c from the host test's "
                           "working directory -- update the candidate paths in this test if "
                           "the build layout moved");
        return;
    }

    TEST_CHECK(strstr(text, "relay_owner_command_energize") == NULL,
               "update_task.c does not call relay_owner_command_energize() anywhere -- if this "
               "fails, an update-path caller now has a route to command heat, which is exactly "
               "the bypass ROADMAP.md's link-loss-heating-block item requires never exist");

    free(text);
}

static void test_update_task_never_writes_the_relay_gpio_directly(void)
{
    TEST_SECTION("update_task.c never writes SAFTYFW_PIN_RELAY directly (bypassing "
                 "relay_owner.c's single chokepoint entirely) -- a stricter net than the "
                 "function-name check above, catching a raw gpio_put() bypass too");

    char *text = read_file_any(UPDATE_TASK_CANDIDATES,
                                sizeof(UPDATE_TASK_CANDIDATES) / sizeof(UPDATE_TASK_CANDIDATES[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/update_task.c from the host test's "
                           "working directory -- update the candidate paths in this test if "
                           "the build layout moved");
        return;
    }

    TEST_CHECK(strstr(text, "SAFTYFW_PIN_RELAY") == NULL,
               "update_task.c contains no reference to SAFTYFW_PIN_RELAY at all -- if this "
               "fails, something in the update path now touches the relay GPIO directly, "
               "outside relay_owner.c's single chokepoint");

    TEST_CHECK(strstr(text, "gpio_put(") == NULL,
               "update_task.c calls gpio_put() nowhere -- it has no business driving ANY GPIO "
               "output, relay or otherwise; a violation here is the exact shape of the bypass "
               "this file exists to catch");

    free(text);
}

static void test_update_task_only_reads_status_never_relay_owner_writes(void)
{
    TEST_SECTION("update_task.c's only touchpoints with relay/output state are READS "
                 "(thermo_task_get_snapshot/current_task_get_snapshot to gate UPDATE_BEGIN's "
                 "precondition check) -- confirms it reads status without ever reaching for "
                 "relay_owner.c's other mutating entry points either");

    char *text = read_file_any(UPDATE_TASK_CANDIDATES,
                                sizeof(UPDATE_TASK_CANDIDATES) / sizeof(UPDATE_TASK_CANDIDATES[0]));
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/update_task.c from the host test's "
                           "working directory -- update the candidate paths in this test if "
                           "the build layout moved");
        return;
    }

    // The precondition gate this file's own header comment describes must
    // still be reading status -- if this disappears, update_task.c's gate
    // was gutted, a different (but related) regression.
    TEST_CHECK(strstr(text, "thermo_task_get_snapshot") != NULL,
               "update_task.c still reads thermo_task_get_snapshot() to gate UPDATE_BEGIN");

    // No other relay_owner_* mutating entry point (trip/clear_trip) is
    // reachable from the update path either -- only command_energize is the
    // heat-authority one, but a trip/clear_trip call from here would still
    // be update_task.c reaching into relay_owner.c's state machine, which
    // ROADMAP.md's "never writes relay/GPIO state" phrase rules out too.
    TEST_CHECK(strstr(text, "relay_owner_command_trip") == NULL,
               "update_task.c does not call relay_owner_command_trip() either");
    TEST_CHECK(strstr(text, "relay_owner_clear_trip") == NULL,
               "update_task.c does not call relay_owner_clear_trip() either");
    TEST_CHECK(strstr(text, "relay_owner_start") == NULL,
               "update_task.c does not call relay_owner_start() either");

    free(text);
}

void run_test_update_task_relay_wiring(void)
{
    test_relay_owner_command_energize_exists_where_expected();
    test_update_task_never_calls_relay_owner_command_energize();
    test_update_task_never_writes_the_relay_gpio_directly();
    test_update_task_only_reads_status_never_relay_owner_writes();
}
