// Host tests for App/drivers/net/pico_auto_update_state.h/.c -- the shared
// blocking/warning verdict readiness_gate.c and readiness_http.c both read.
//
// Review finding F2 (owner decision, option c, 2026-09-20): exhausting the
// Pico auto-update attempt budget must surface as a non-blocking
// /readiness WARNING, never as a permanent firing refusal. The decision
// function itself (pico_auto_update.h, test_pico_auto_update_decision.c)
// is unchanged -- it still reports ABANDONED_BUDGET_SPENT as one of its
// unrecoverable causes. What changed is how pico_auto_update_boot.c's
// switch CONSUMES that outcome: it now calls
// pico_auto_update_state_set_warning() instead of set_blocking(true), and
// leaves is_blocking() at its false default. That boot-time switch itself
// lives in pico_auto_update_boot.c, which pulls in FreeRTOS/ESP-IDF task
// plumbing and is not part of this host-test build (same class as the
// HTTP-handler files -- see this repo's "HTTP handlers are target-build
// only" note); this file instead proves the CONTRACT the state module
// offers that switch: blocking and warning are independent flags, a
// warning never implies blocking, and the module's own zero-initialized
// default (nothing decided yet) reads as neither blocking nor warning --
// so a boot that reaches ABANDONED_BUDGET_SPENT and calls only
// set_warning() leaves is_blocking() false, exactly as readiness_gate.h
// requires.
#include <string.h>

#include "test_common.h"

#include "../drivers/net/pico_auto_update_state.h"
#include "../drivers/net/pico_auto_update_state.c"

static void test_default_state_is_neither_blocking_nor_warning(void)
{
    TEST_CHECK(!pico_auto_update_state_is_blocking(), "default: not blocking");
    TEST_CHECK(!pico_auto_update_state_is_warning(), "default: no warning");
    TEST_CHECK(strcmp(pico_auto_update_state_reason(), "") == 0, "default: empty blocking reason");
    TEST_CHECK(strcmp(pico_auto_update_state_warning_reason(), "") == 0, "default: empty warning reason");
}

static void test_budget_spent_sets_warning_without_blocking(void)
{
    // The exact sequence pico_auto_update_boot.c's ABANDONED_BUDGET_SPENT
    // case now performs (F2): a warning naming the attempt count/commit,
    // and blocking explicitly left/forced false -- never set_blocking(true).
    pico_auto_update_state_set_blocking(false, NULL);
    pico_auto_update_state_set_warning("Pico update budget spent (3/3 attempts) against abc123");

    TEST_CHECK(!pico_auto_update_state_is_blocking(),
              "F2: a spent update budget must NEVER block firing -- readiness_gate.h's interlock "
              "reads only is_blocking(), so this is what actually keeps the board fireable");
    TEST_CHECK(pico_auto_update_state_is_warning(), "F2: the spent budget IS surfaced, as a warning");
    TEST_CHECK(strstr(pico_auto_update_state_warning_reason(), "3/3 attempts") != NULL,
              "the warning names the attempt count");
    TEST_CHECK(strstr(pico_auto_update_state_warning_reason(), "abc123") != NULL,
              "the warning names the commit that could not be matched");

    pico_auto_update_state_set_warning(NULL); // restore
}

static void test_other_unrecoverable_causes_still_block(void)
{
    // The three OTHER ABANDONED_* causes (NO_IMAGE, CHAIN_GAP, PRIOR_FAILED)
    // keep the pre-F2 behavior: set_blocking(true) with no warning. F2's
    // scope is BUDGET_SPENT alone, per its own finding text.
    pico_auto_update_state_set_blocking(true, "no usable embedded/staged image");
    TEST_CHECK(pico_auto_update_state_is_blocking(), "an image/chain-gap/prior-failed cause still blocks");
    TEST_CHECK(!pico_auto_update_state_is_warning(),
              "blocking for one of the other causes does not, by itself, raise the separate warning flag");
    TEST_CHECK(strcmp(pico_auto_update_state_reason(), "no usable embedded/staged image") == 0,
              "the blocking reason is recorded verbatim");

    pico_auto_update_state_set_blocking(false, NULL); // restore
}

static void test_set_blocking_false_clears_reason(void)
{
    pico_auto_update_state_set_blocking(true, "temporary");
    pico_auto_update_state_set_blocking(false, NULL);
    TEST_CHECK(!pico_auto_update_state_is_blocking(), "cleared: not blocking");
    TEST_CHECK(strcmp(pico_auto_update_state_reason(), "") == 0, "cleared: reason reset to empty");
}

void run_test_pico_auto_update_state(void)
{
    test_default_state_is_neither_blocking_nor_warning();
    test_budget_spent_sets_warning_without_blocking();
    test_other_unrecoverable_causes_still_block();
    test_set_blocking_false_clears_reason();
}
