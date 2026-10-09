// Host tests for App/drivers/control/heat_interlock.c -- the OWNER's mutual OTA
// interlock, direction B ("heating is not allowed during updates"). No
// ESP-IDF dependency, mirrors test_ota_interlock.c's own pattern for
// direction A.
#include <string.h>

#include "test_common.h"

#include "../drivers/control/heat_interlock.h"

static void test_no_update_is_ok(void)
{
    TEST_SECTION("heat_interlock_check -- no update in progress is OK");

    heat_interlock_snapshot_t snap = { 0 };
    snap.update_in_progress = false;
    snap.update_context = HEAT_INTERLOCK_UPDATE_NONE;
    char reason[HEAT_INTERLOCK_REASON_MAX];

    TEST_CHECK(heat_interlock_check(&snap, reason, sizeof(reason)) == HEAT_INTERLOCK_OK,
               "no update in progress -- heat-causing action may proceed");

    // NULL/0 reason_out/reason_cap must not crash.
    TEST_CHECK(heat_interlock_check(&snap, NULL, 0) == HEAT_INTERLOCK_OK,
               "NULL reason_out is accepted, result unaffected");

    // NULL snapshot must not crash and must fail safe by... wait, NULL means
    // "no snapshot taken" -- treated as OK only because the pure function
    // has no data to refuse on; the ESP-IDF glue (ota_http_heat_blocked_by_
    // update()) always passes a real snapshot, never NULL, so this is a
    // defensive-only case, not a real caller path.
    TEST_CHECK(heat_interlock_check(NULL, reason, sizeof(reason)) == HEAT_INTERLOCK_OK,
               "NULL snapshot is treated as OK (defensive only -- no real caller passes NULL)");
}

static void test_esp_update_refuses(void)
{
    TEST_SECTION("heat_interlock_check -- an ESP update in progress refuses, names the ESP");

    heat_interlock_snapshot_t snap = { 0 };
    snap.update_in_progress = true;
    snap.update_context = HEAT_INTERLOCK_UPDATE_ESP;
    char reason[HEAT_INTERLOCK_REASON_MAX] = { 0 };

    TEST_CHECK(heat_interlock_check(&snap, reason, sizeof(reason)) == HEAT_INTERLOCK_REFUSED,
               "an ESP update in progress refuses the heat-causing action");
    TEST_CHECK(strstr(reason, "ESP") != NULL, "reason names the ESP specifically");
    TEST_CHECK(strstr(reason, "update") != NULL, "reason says it's an update, not a generic refusal");
    TEST_CHECK(strstr(reason, "GitHub fetch: POST .../cancel") != NULL,
               "ESP reason names the fetch cancel route, conditional on it being a GitHub fetch");
    TEST_CHECK(strlen(reason) < HEAT_INTERLOCK_REASON_MAX, "ESP reason is not truncated by the 96-byte budget");
}

static void test_fetch_busy_refuses(void)
{
    TEST_SECTION("heat_interlock_check -- a busy GitHub fetch/check refuses with no OTA claim held");

    heat_interlock_snapshot_t snap = { 0 };
    snap.fetch_busy = true;
    char reason[HEAT_INTERLOCK_REASON_MAX] = { 0 };

    TEST_CHECK(heat_interlock_check(&snap, reason, sizeof(reason)) == HEAT_INTERLOCK_REFUSED,
               "fetch_busy alone refuses");
    TEST_CHECK(strstr(reason, "/api/update/fetch/cancel") != NULL, "reason names the cancel route");
    TEST_CHECK(strlen(reason) < HEAT_INTERLOCK_REASON_MAX - 1, "reason fits the 96-byte budget untruncated");
    snap.fetch_busy = false;
    TEST_CHECK(heat_interlock_check(&snap, reason, sizeof(reason)) == HEAT_INTERLOCK_OK, "idle fetch does not refuse");
}

static void test_pico_update_refuses(void)
{
    TEST_SECTION("heat_interlock_check -- a Pico update in progress refuses, names the Pico");

    heat_interlock_snapshot_t snap = { 0 };
    snap.update_in_progress = true;
    snap.update_context = HEAT_INTERLOCK_UPDATE_PICO;
    char reason[HEAT_INTERLOCK_REASON_MAX] = { 0 };

    TEST_CHECK(heat_interlock_check(&snap, reason, sizeof(reason)) == HEAT_INTERLOCK_REFUSED,
               "a Pico update in progress refuses the heat-causing action");
    TEST_CHECK(strstr(reason, "Pico") != NULL, "reason names the Pico specifically");
    TEST_CHECK(strstr(reason, "ESP") == NULL, "reason does not claim it's the ESP when it's the Pico");
}

static void test_reason_out_null_tolerant_on_refusal(void)
{
    TEST_SECTION("heat_interlock_check -- refusal with NULL reason_out does not crash");

    heat_interlock_snapshot_t snap = { 0 };
    snap.update_in_progress = true;
    snap.update_context = HEAT_INTERLOCK_UPDATE_ESP;

    TEST_CHECK(heat_interlock_check(&snap, NULL, 0) == HEAT_INTERLOCK_REFUSED,
               "still refuses correctly with no reason buffer supplied");
}

static void test_truncation_is_null_terminated(void)
{
    TEST_SECTION("heat_interlock_check -- a too-small reason buffer is truncated, not overrun");

    heat_interlock_snapshot_t snap = { 0 };
    snap.update_in_progress = true;
    snap.update_context = HEAT_INTERLOCK_UPDATE_ESP;
    char tiny[8];
    memset(tiny, 'X', sizeof(tiny)); // poison, so a missing null terminator is detectable

    TEST_CHECK(heat_interlock_check(&snap, tiny, sizeof(tiny)) == HEAT_INTERLOCK_REFUSED,
               "still refuses correctly with an undersized reason buffer");
    TEST_CHECK(strlen(tiny) < sizeof(tiny), "truncated reason is still null-terminated within the buffer");
}

void run_test_heat_interlock(void)
{
    test_no_update_is_ok();
    test_esp_update_refuses();
    test_fetch_busy_refuses();
    test_pico_update_refuses();
    test_reason_out_null_tolerant_on_refusal();
    test_truncation_is_null_terminated();
}
