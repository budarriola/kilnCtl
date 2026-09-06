// Host tests for App/drivers/net/ota_interlock.c -- TODO.md 9.4's "both update
// paths refused unless..." precondition list. No ESP-IDF dependency.
#include <string.h>

#include "test_common.h"

#include "../drivers/net/ota_interlock.h"

// A snapshot that passes every check -- each test below mutates one field
// (or one zone) away from this baseline so a failure is attributable to
// exactly one precondition, not a combination.
static ota_interlock_snapshot_t good_snapshot(void)
{
    ota_interlock_snapshot_t s;
    memset(&s, 0, sizeof(s));
    s.profile_state = OTA_INTERLOCK_PROFILE_IDLE;
    s.autotune_active = false;
    s.safety_link_up = true;
    s.run_state_interrupted = false;
    s.other_update_in_progress = false;
    s.temp_ceiling_c = OTA_INTERLOCK_TEMP_CEILING_C;
    return s;
}

static ota_interlock_zone_snapshot_t good_zone(void)
{
    ota_interlock_zone_snapshot_t z;
    memset(&z, 0, sizeof(z));
    z.active = true;
    z.actual_valid = true;
    z.actual_c = 20.0f;
    z.heater_commanded = false;
    return z;
}

static void test_all_clear(void)
{
    TEST_SECTION("ota_interlock_check -- everything clear is OK");

    ota_interlock_snapshot_t snap = good_snapshot();
    ota_interlock_zone_snapshot_t zones[3] = { good_zone(), good_zone(), good_zone() };
    char reason[OTA_INTERLOCK_REASON_MAX];

    TEST_CHECK(ota_interlock_check(&snap, zones, 3, reason, sizeof(reason)) == OTA_INTERLOCK_OK,
               "a fully idle, cool, linked kiln with no other update passes");

    // No zones at all (thermo_count == 0, e.g. a board with zero configured
    // zones) must also pass -- an empty zone list has nothing to refuse on.
    TEST_CHECK(ota_interlock_check(&snap, NULL, 0, reason, sizeof(reason)) == OTA_INTERLOCK_OK,
               "zero configured zones is OK, not a refusal");

    // NULL/0 reason_out/reason_cap must not crash and must still evaluate
    // correctly -- callers that only want the pass/fail result.
    TEST_CHECK(ota_interlock_check(&snap, zones, 3, NULL, 0) == OTA_INTERLOCK_OK,
               "NULL reason_out is accepted, result unaffected");
}

static void test_update_mutex_checked_first(void)
{
    TEST_SECTION("ota_interlock_check -- other_update_in_progress refuses first");

    ota_interlock_snapshot_t snap = good_snapshot();
    snap.other_update_in_progress = true;
    // Also break several other preconditions -- the mutex check must still
    // be what's reported, since it's documented to run first.
    snap.safety_link_up = false;
    snap.profile_state = OTA_INTERLOCK_PROFILE_RUNNING;

    ota_interlock_zone_snapshot_t zones[1] = { good_zone() };
    char reason[OTA_INTERLOCK_REASON_MAX] = { 0 };

    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "an update already in progress refuses");
    TEST_CHECK(strstr(reason, "already in progress") != NULL,
               "reason names the update-in-progress mutex, not the other broken preconditions");
}

static void test_safety_link_down(void)
{
    TEST_SECTION("ota_interlock_check -- safety link down refuses");

    ota_interlock_snapshot_t snap = good_snapshot();
    snap.safety_link_up = false;

    ota_interlock_zone_snapshot_t zones[1] = { good_zone() };
    char reason[OTA_INTERLOCK_REASON_MAX] = { 0 };

    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason))
                   == OTA_INTERLOCK_REFUSED_NEEDS_ACK,
               "a down safety link refuses, and reports itself as the acknowledgeable refusal");
    TEST_CHECK(strstr(reason, "safety link") != NULL, "reason names the safety link specifically");
    TEST_CHECK(OTA_INTERLOCK_REFUSED_NEEDS_ACK != OTA_INTERLOCK_OK,
               "and it is still a refusal to any caller testing `!= OTA_INTERLOCK_OK`");
}

// The acknowledgement's whole point, and its whole limit. Written as
// negative tests first: if operator_ack_no_safety_processor were wired to
// short-circuit the WHOLE check rather than just precondition 2, every
// assertion below about the other preconditions would fail.
static void test_no_safety_ack_overrides_only_the_link(void)
{
    TEST_SECTION("ota_interlock_check -- the no-safety-processor acknowledgement");

    ota_interlock_zone_snapshot_t zones[1] = { good_zone() };
    char reason[OTA_INTERLOCK_REASON_MAX] = { 0 };

    // 1. Acknowledged, link down, nothing else wrong -> proceeds.
    ota_interlock_snapshot_t snap = good_snapshot();
    snap.safety_link_up = false;
    snap.operator_ack_no_safety_processor = true;
    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_OK,
               "an acknowledged down link proceeds when every other precondition holds");

    // 2. The ack must NOT rescue any other precondition. Each of these has
    // the ack set AND the link down -- exactly the state a real overriding
    // operator is in -- so a bug that returned early on the ack would let
    // every one of them through.
    snap = good_snapshot();
    snap.safety_link_up = false;
    snap.operator_ack_no_safety_processor = true;
    snap.profile_state = OTA_INTERLOCK_PROFILE_RUNNING;
    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "the acknowledgement does NOT let a running profile through");
    TEST_CHECK(strstr(reason, "running") != NULL, "and the reason is the profile, not the link");

    snap = good_snapshot();
    snap.safety_link_up = false;
    snap.operator_ack_no_safety_processor = true;
    snap.autotune_active = true;
    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "the acknowledgement does NOT let a running autotune through");

    snap = good_snapshot();
    snap.safety_link_up = false;
    snap.operator_ack_no_safety_processor = true;
    snap.other_update_in_progress = true;
    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "the acknowledgement does NOT let a second concurrent update through");

    snap = good_snapshot();
    snap.safety_link_up = false;
    snap.operator_ack_no_safety_processor = true;
    snap.run_state_interrupted = true;
    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "the acknowledgement does NOT let an unacknowledged interrupted firing through");

    {
        ota_interlock_snapshot_t s2 = good_snapshot();
        s2.safety_link_up = false;
        s2.operator_ack_no_safety_processor = true;
        ota_interlock_zone_snapshot_t hot[1] = { good_zone() };
        hot[0].actual_c = 500.0f;
        TEST_CHECK(ota_interlock_check(&s2, hot, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
                   "the acknowledgement does NOT let a hot zone through");
        TEST_CHECK(strstr(reason, "500") != NULL, "and the reason names the temperature");

        ota_interlock_zone_snapshot_t on[1] = { good_zone() };
        on[0].heater_commanded = true;
        TEST_CHECK(ota_interlock_check(&s2, on, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
                   "the acknowledgement does NOT let a commanded heater through");
    }

    // 3. The ack is inert when the link is UP -- it must never be a way to
    // weaken anything on a healthy board.
    snap = good_snapshot();
    snap.operator_ack_no_safety_processor = true;
    snap.profile_state = OTA_INTERLOCK_PROFILE_PAUSED;
    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "with the link up the acknowledgement changes nothing");
}

static void test_autotune_active(void)
{
    TEST_SECTION("ota_interlock_check -- autotune running refuses");

    ota_interlock_snapshot_t snap = good_snapshot();
    snap.autotune_active = true;

    ota_interlock_zone_snapshot_t zones[1] = { good_zone() };
    char reason[OTA_INTERLOCK_REASON_MAX] = { 0 };

    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "autotune active refuses");
    TEST_CHECK(strstr(reason, "autotune") != NULL, "reason names autotune specifically");
}

static void test_profile_running_and_paused(void)
{
    TEST_SECTION("ota_interlock_check -- profile RUNNING or PAUSED refuses, DONE/FAULTED/IDLE do not");

    ota_interlock_zone_snapshot_t zones[1] = { good_zone() };
    char reason[OTA_INTERLOCK_REASON_MAX];

    ota_interlock_snapshot_t running = good_snapshot();
    running.profile_state = OTA_INTERLOCK_PROFILE_RUNNING;
    TEST_CHECK(ota_interlock_check(&running, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "RUNNING refuses");
    TEST_CHECK(strstr(reason, "running") != NULL, "reason says the profile is running");

    ota_interlock_snapshot_t paused = good_snapshot();
    paused.profile_state = OTA_INTERLOCK_PROFILE_PAUSED;
    TEST_CHECK(ota_interlock_check(&paused, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "PAUSED refuses too -- a paused firing is not an ended one");
    TEST_CHECK(strstr(reason, "paused") != NULL, "reason says the profile is paused");

    ota_interlock_snapshot_t done = good_snapshot();
    done.profile_state = OTA_INTERLOCK_PROFILE_DONE;
    TEST_CHECK(ota_interlock_check(&done, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_OK,
               "DONE does not refuse -- the run has ended");

    ota_interlock_snapshot_t faulted = good_snapshot();
    faulted.profile_state = OTA_INTERLOCK_PROFILE_FAULTED;
    TEST_CHECK(ota_interlock_check(&faulted, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_OK,
               "FAULTED does not refuse by itself -- relays are off across a faulted run");
}

static void test_run_state_interrupted(void)
{
    TEST_SECTION("ota_interlock_check -- run_state_interrupted refuses");

    ota_interlock_snapshot_t snap = good_snapshot();
    snap.run_state_interrupted = true;

    ota_interlock_zone_snapshot_t zones[1] = { good_zone() };
    char reason[OTA_INTERLOCK_REASON_MAX] = { 0 };

    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "a firing interrupted by a reboot refuses, even though profile_state itself is IDLE");
    TEST_CHECK(strstr(reason, "interrupted") != NULL, "reason names the interrupted firing");
}

static void test_heater_commanded(void)
{
    TEST_SECTION("ota_interlock_check -- a commanded-on zone refuses, by zone index");

    ota_interlock_snapshot_t snap = good_snapshot();
    ota_interlock_zone_snapshot_t zones[3] = { good_zone(), good_zone(), good_zone() };
    zones[2].heater_commanded = true;

    char reason[OTA_INTERLOCK_REASON_MAX] = { 0 };
    TEST_CHECK(ota_interlock_check(&snap, zones, 3, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "zone 2's heater being commanded on refuses");
    TEST_CHECK(strstr(reason, "zone 2") != NULL, "reason names zone 2 specifically");
    TEST_CHECK(strstr(reason, "commanded") != NULL, "reason explains it is heater-commanded, not a generic failure");

    // An INACTIVE zone with a heater-commanded flag set must be ignored --
    // only zones actually in use are checked.
    ota_interlock_zone_snapshot_t zones2[2] = { good_zone(), good_zone() };
    zones2[1].active = false;
    zones2[1].heater_commanded = true;
    TEST_CHECK(ota_interlock_check(&snap, zones2, 2, reason, sizeof(reason)) == OTA_INTERLOCK_OK,
               "an inactive zone's heater_commanded is ignored");
}

static void test_temperature_ceiling(void)
{
    TEST_SECTION("ota_interlock_check -- temperature ceiling, by zone index and value");

    ota_interlock_snapshot_t snap = good_snapshot();
    ota_interlock_zone_snapshot_t zones[3] = { good_zone(), good_zone(), good_zone() };
    zones[1].actual_c = 340.0f; // matches TODO.md 9.4's own worked example

    char reason[OTA_INTERLOCK_REASON_MAX] = { 0 };
    TEST_CHECK(ota_interlock_check(&snap, zones, 3, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "a zone above the ceiling refuses");
    TEST_CHECK(strstr(reason, "zone 1") != NULL, "reason names zone 1 (0-based index) specifically");
    TEST_CHECK(strstr(reason, "340") != NULL, "reason states the actual temperature, not a vague message");

    // Exactly at the ceiling refuses too (>=, not >) -- "below a configured
    // ceiling" per UPDATE_PROTOCOL.md section 1 means strictly below.
    ota_interlock_zone_snapshot_t at_ceiling[1] = { good_zone() };
    at_ceiling[0].actual_c = OTA_INTERLOCK_TEMP_CEILING_C;
    TEST_CHECK(ota_interlock_check(&snap, at_ceiling, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "exactly at the ceiling still refuses (inclusive boundary)");

    // Just under the ceiling passes.
    ota_interlock_zone_snapshot_t under_ceiling[1] = { good_zone() };
    under_ceiling[0].actual_c = OTA_INTERLOCK_TEMP_CEILING_C - 0.1f;
    TEST_CHECK(ota_interlock_check(&snap, under_ceiling, 1, reason, sizeof(reason)) == OTA_INTERLOCK_OK,
               "just under the ceiling is OK");
}

static void test_invalid_reading_refuses(void)
{
    TEST_SECTION("ota_interlock_check -- an active zone with no valid reading refuses (safe default)");

    ota_interlock_snapshot_t snap = good_snapshot();
    ota_interlock_zone_snapshot_t zones[1] = { good_zone() };
    zones[0].actual_valid = false;

    char reason[OTA_INTERLOCK_REASON_MAX] = { 0 };
    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "an active zone with no trustworthy reading refuses rather than being skipped");
    TEST_CHECK(strstr(reason, "zone 0") != NULL, "reason names the zone with the missing reading");
}

static void test_check_order(void)
{
    TEST_SECTION("ota_interlock_check -- documented check order (mutex, link, autotune, profile, "
                 "run_state, heater, temperature)");

    // Break heater-commanded AND temperature-ceiling together on the same
    // zone; heater-commanded must win since it's checked first in the loop
    // order documented in ota_interlock.h.
    ota_interlock_snapshot_t snap = good_snapshot();
    ota_interlock_zone_snapshot_t zones[1] = { good_zone() };
    zones[0].heater_commanded = true;
    zones[0].actual_c = 500.0f;

    char reason[OTA_INTERLOCK_REASON_MAX] = { 0 };
    TEST_CHECK(ota_interlock_check(&snap, zones, 1, reason, sizeof(reason)) == OTA_INTERLOCK_REFUSED,
               "refused (either reason would be valid)");
    TEST_CHECK(strstr(reason, "commanded") != NULL,
               "heater-commanded is checked (and reported) before the temperature ceiling for the "
               "same zone, per the documented per-zone loop order");
}

void run_test_ota_interlock(void)
{
    test_all_clear();
    test_update_mutex_checked_first();
    test_safety_link_down();
    test_no_safety_ack_overrides_only_the_link();
    test_autotune_active();
    test_profile_running_and_paused();
    test_run_state_interrupted();
    test_heater_commanded();
    test_temperature_ceiling();
    test_invalid_reading_refuses();
    test_check_order();
}
