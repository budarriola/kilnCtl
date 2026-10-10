// Host tests for firmware/KilnFW/App/drivers/bridge/link_watchdog_decide.h --
// the PC-link watchdog's pure decision core (uart_bridge.c's
// link_watchdog_task refactored 2026-09-07 to call these functions instead
// of inlining the logic, so it could be tested off-target).
//
// TODO.md: "The PC-link watchdog drops all relays every 5 s of host
// silence, and an idle-but-connected MCP session is enough to trigger it
// repeatedly... Check what this does to a running firing before a real one
// is attempted." The watchdog's own header comment (uart_bridge.h) and the
// 2026-08-27 fix already establish the policy this file proves:
//   - an idle host must NOT touch a relay claimed by an on-board owner
//     (profile_executor.c's PROFILE, autotune_engine.c's AUTOTUNE) -- the
//     executor is the controller and its own watchdogs (guard 9, the
//     safety-link silence abort) are the ones that apply to it;
//   - an idle host DOES lose authority over a relay it (or a manual UI
//     command) left NONE/MANUAL-owned -- that grant must expire on silence;
//   - traffic (a frame or an ACK) resets the timer.
//
// These call the REAL relay_authority.c (compiled into this executable,
// same "link the real .c" standard test_kiln_io_owner.c and
// test_safety_link_compile.c use) via relay_authority_claim_mask()/
// relay_authority_release_mask() -- not a hand-set struct standing in for
// ownership -- so what's proven is the actual ownership lookup the board
// runs, per this repo's "idealized test input" and "binding a mirror to C"
// standing cautions.
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "link_watchdog_decide.h"
#include "relay_authority.h"

// relay_authority.c also defines relay_authority_on_blocked(), which this
// test never calls (link_watchdog_decide_unowned_mask() only reaches
// relay_authority_manual_blocked_by_owner()/relay_authority_get_owner()) but
// which still needs safety_link_get_fault_sources() to link -- MSVC pulls in
// a whole .obj's external references even for a function this binary never
// invokes. A stub body is enough: same pattern test_kiln_io_owner.c's header
// comment describes for its own SX1509/danger_mode/ota_http fakes.
uint32_t safety_link_get_fault_sources(SafetyLinkClass *link)
{
    (void)link;
    return 0;
}

// relay_authority.c's ownership state is a plain static array with no
// init/reset function (matches its "no create/destroy lifecycle" doc
// comment) -- release every relay before each test so one test's claim
// cannot leak into the next.
static void reset_all_relay_ownership(void)
{
    relay_authority_release_mask(0x0Fu); // relays 1..4, bit0..bit3
}

static void test_all_unowned_relays_are_in_the_mask(void)
{
    TEST_SECTION("link_watchdog_decide_unowned_mask -- nothing claimed -> every relay is unowned");
    reset_all_relay_ownership();

    uint8_t mask = link_watchdog_decide_unowned_mask(false);

    TEST_CHECK(mask == 0x0Fu, "all four relays (bits 0-3) are unowned when nothing has claimed any");
}

static void test_profile_owned_relay_is_excluded(void)
{
    TEST_SECTION("link_watchdog_decide_unowned_mask -- a PROFILE-owned relay is NOT in the mask "
                 "(an idle host must not touch a relay a running firing commands)");
    reset_all_relay_ownership();

    // Relay 1 (bit 0) claimed by a running firing, same call
    // profile_executor.c makes to hold its zone-heat relay.
    relay_authority_claim_mask(0x01u, RELAY_OWNER_PROFILE);

    uint8_t mask = link_watchdog_decide_unowned_mask(false);

    TEST_CHECK((mask & 0x01u) == 0, "relay 1 (PROFILE-owned) is excluded from the watchdog's mask");
    TEST_CHECK((mask & 0x0Eu) == 0x0Eu, "relays 2-4 (still unowned) remain in the mask");
}

static void test_autotune_owned_relay_is_excluded(void)
{
    TEST_SECTION("link_watchdog_decide_unowned_mask -- an AUTOTUNE-owned relay is also excluded");
    reset_all_relay_ownership();

    relay_authority_claim_mask(0x04u, RELAY_OWNER_AUTOTUNE); // relay 3

    uint8_t mask = link_watchdog_decide_unowned_mask(false);

    TEST_CHECK((mask & 0x04u) == 0, "relay 3 (AUTOTUNE-owned) is excluded");
    TEST_CHECK((mask & 0x0Bu) == 0x0Bu, "the other three, unowned, remain in the mask");
}

static void test_manual_owned_relay_is_still_in_the_mask(void)
{
    TEST_SECTION("link_watchdog_decide_unowned_mask -- a MANUAL-owned relay IS in the mask "
                 "(a PC-commanded grant must expire on silence)");
    reset_all_relay_ownership();

    relay_authority_claim_mask(0x02u, RELAY_OWNER_MANUAL); // relay 2

    uint8_t mask = link_watchdog_decide_unowned_mask(false);

    TEST_CHECK((mask & 0x02u) != 0,
               "a MANUAL-owned relay is still subject to the link watchdog -- the PC's own grant, "
               "not an on-board owner's, so it must expire when the PC goes silent");
}

static void test_danger_mode_suppresses_the_whole_mask(void)
{
    TEST_SECTION("link_watchdog_decide_unowned_mask -- danger mode zeroes the mask even for "
                 "otherwise-unowned relays");
    reset_all_relay_ownership();

    uint8_t mask = link_watchdog_decide_unowned_mask(true);

    TEST_CHECK(mask == 0, "danger mode active -> the watchdog touches nothing, even NONE-owned relays");
}

static void test_link_up_within_timeout(void)
{
    TEST_SECTION("link_watchdog_decide_link_up -- traffic within the window keeps the link up");

    bool up = link_watchdog_decide_link_up(/*now=*/1000, /*last_activity=*/900, /*ever_seen=*/true,
                                            /*timeout_ticks=*/200);

    TEST_CHECK(up, "100 ticks elapsed, 200-tick timeout -- still up");
}

static void test_link_down_after_timeout(void)
{
    TEST_SECTION("link_watchdog_decide_link_up -- silence past the window is down");

    bool up = link_watchdog_decide_link_up(/*now=*/1300, /*last_activity=*/900, /*ever_seen=*/true,
                                            /*timeout_ticks=*/200);

    TEST_CHECK(!up, "400 ticks elapsed, 200-tick timeout -- link counts as lost");
}

static void test_never_seen_counts_as_down(void)
{
    TEST_SECTION("link_watchdog_decide_link_up -- before the host has ever spoken, the link is down");

    // A board that just booted: last_activity/now are whatever the tick
    // counter happens to be, but ever_seen is false -- uart_bridge.h's "before
    // the host has ever spoken the link counts as lost" contract.
    bool up = link_watchdog_decide_link_up(/*now=*/50, /*last_activity=*/0, /*ever_seen=*/false,
                                            /*timeout_ticks=*/200);

    TEST_CHECK(!up, "ever_seen=false is down regardless of the tick math");
}

static void test_traffic_resets_the_timer(void)
{
    TEST_SECTION("link_watchdog_decide_link_up -- a fresh last_activity (traffic) resets the timer");

    const uint32_t timeout_ticks = 200;

    // Silence long enough to have expired against the OLD last_activity...
    bool would_be_down = link_watchdog_decide_link_up(/*now=*/1300, /*last_activity=*/900,
                                                       /*ever_seen=*/true, timeout_ticks);
    TEST_CHECK(!would_be_down, "sanity: without new traffic this window would already be down");

    // ...but a frame/ACK moved last_activity up to just before `now`, exactly
    // what bridge_note_link_activity() does on every accepted frame or ACK.
    bool up_after_traffic = link_watchdog_decide_link_up(/*now=*/1300, /*last_activity=*/1290,
                                                         /*ever_seen=*/true, timeout_ticks);
    TEST_CHECK(up_after_traffic, "traffic 10 ticks ago resets the window -- link reads up again");
}

// Not a watchdog test -- this is simply the one host executable that links the
// REAL relay_authority.c, so the per-zone profile/autotune claim added in the
// review of 933a7eec (relay_authority_zone_claim_begin()/_end()) is proven
// here against the actual test-and-set, not against the call-recording fakes
// test_profile_executor_prestart.c/test_autotune_engine_prestart.c use for
// their wiring checks.
static void test_zone_claim_arbitrates_profile_vs_autotune_per_zone(void)
{
    uint8_t conflict = 0xFFu;

    TEST_CHECK(relay_authority_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_PROFILE, 0x03u, &conflict),
               "profile claims zones 0+1 on a clean slate");
    TEST_CHECK(conflict == 0, "a successful claim reports no conflict");

    conflict = 0;
    TEST_CHECK(!relay_authority_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE, 0x02u, &conflict),
               "autotune is refused on zone 1 while a profile holds it");
    TEST_CHECK(conflict == 0x02u, "the refusal names exactly the contended zone bit");

    TEST_CHECK(relay_authority_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE, 0x04u, NULL),
               "autotune on a DIFFERENT zone (2) is still allowed alongside the profile");

    conflict = 0;
    TEST_CHECK(!relay_authority_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_PROFILE, 0x04u, &conflict),
               "the other direction is arbitrated too: profile refused on autotune's zone 2");
    TEST_CHECK(conflict == 0x04u, "reverse refusal names zone 2");

    // A refused claim must not have taken anything: releasing autotune's
    // zone 2 alone must leave zone 2 free for a profile.
    relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE, 0x04u);
    TEST_CHECK(relay_authority_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_PROFILE, 0x04u, NULL),
               "zone 2 is free again once autotune released it");

    // _end() only ever clears the caller's OWN side: an autotune release of
    // zone 1 must not free a zone the profile still holds.
    relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE, 0x02u);
    TEST_CHECK(!relay_authority_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE, 0x02u, NULL),
               "an AUTOTUNE release never clears a PROFILE-held bit");

    relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_PROFILE, 0x07u);
    TEST_CHECK(relay_authority_zone_claim_begin(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE, 0x02u, NULL),
               "after the profile's release, autotune may claim zone 1");
    relay_authority_zone_claim_end(RELAY_HEAT_ZONE_CLAIM_AUTOTUNE, 0x02u);
}

// HTTP audit L37 follow-up (LOW-5): the factory-reset in-flight mark and the sweep claim accessor,
// against the REAL relay_authority.c counter (the factory_reset/sweep/starter tests use fakes).
static void test_reset_refuses_writer_exempts_reset_job_task(void)
{
    TEST_CHECK(!relay_authority_reset_refuses_writer(), "no mark: nothing refused");
    relay_authority_reset_in_flight_begin();
    TEST_CHECK(relay_authority_reset_refuses_writer(), "mark set, no job registered: refused");
    relay_authority_reset_job_enter();
    TEST_CHECK(!relay_authority_reset_refuses_writer(), "the registered reset-job task itself is exempt");
    relay_authority_reset_job_exit();
    TEST_CHECK(relay_authority_reset_refuses_writer(), "after the job exits the exemption is gone");
    relay_authority_reset_in_flight_end();
    TEST_CHECK(!relay_authority_reset_refuses_writer(), "mark cleared: nothing refused");
}

static void test_kiln_nvs_fence_only_when_scope_erases_it(void)
{
    relay_authority_reset_set_erases_kiln_nvs(false);
    relay_authority_reset_in_flight_begin();
    TEST_CHECK(relay_authority_reset_refuses_writer(), "generic fence still set for a non-kiln scope");
    TEST_CHECK(!relay_authority_reset_refuses_kiln_nvs_writer(), "wifi/profiles scope: kiln_nvs writes are NOT refused");
    relay_authority_reset_in_flight_end();
    relay_authority_reset_set_erases_kiln_nvs(true);
    relay_authority_reset_in_flight_begin();
    TEST_CHECK(relay_authority_reset_refuses_kiln_nvs_writer(), "kiln/all scope: other-task kiln_nvs write refused");
    relay_authority_reset_job_enter();
    TEST_CHECK(!relay_authority_reset_refuses_kiln_nvs_writer(), "the reset job's own task is exempt");
    relay_authority_reset_job_exit();
    relay_authority_reset_in_flight_end();
    TEST_CHECK(!relay_authority_reset_refuses_kiln_nvs_writer(), "mark cleared: nothing refused, flag cleared");
    relay_authority_reset_in_flight_begin();
    TEST_CHECK(!relay_authority_reset_refuses_kiln_nvs_writer(), "the erase flag does not leak into the next reset");
    relay_authority_reset_in_flight_end();
}

static void test_reset_in_flight_counter_is_a_depth_count(void)
{
    TEST_CHECK(!relay_authority_reset_in_flight(), "clean slate: no reset in flight");

    relay_authority_reset_in_flight_begin();
    TEST_CHECK(relay_authority_reset_in_flight(), "begin sets the mark");
    relay_authority_reset_in_flight_end();
    TEST_CHECK(!relay_authority_reset_in_flight(), "a matching end clears it");

    // Two resets (HTTP and UART) at once: the first to finish must not clear the other's mark.
    relay_authority_reset_in_flight_begin();
    relay_authority_reset_in_flight_begin();
    relay_authority_reset_in_flight_end();
    TEST_CHECK(relay_authority_reset_in_flight(), "depth 2: one end leaves the second reset's mark set");
    relay_authority_reset_in_flight_end();
    TEST_CHECK(!relay_authority_reset_in_flight(), "depth 2: the second end clears it");

    // An end with nothing in flight must not wrap the counter (uint8_t) into "in flight".
    relay_authority_reset_in_flight_end();
    TEST_CHECK(!relay_authority_reset_in_flight(), "end on zero stays clear (no underflow)");
    relay_authority_reset_in_flight_begin();
    relay_authority_reset_in_flight_end();
    TEST_CHECK(!relay_authority_reset_in_flight(), "after an end on zero, begin/end still pair exactly");
}

static void test_heat_sweep_active_follows_the_sweep_claim(void)
{
    TEST_CHECK(!relay_authority_heat_sweep_active(), "no sweep claim on a clean slate");
    TEST_CHECK(relay_authority_heat_sweep_claim_begin() == RELAY_HEAT_SWEEP_CLAIM_OK, "sweep claim taken");
    TEST_CHECK(relay_authority_heat_sweep_active(), "the accessor reports the held sweep claim");
    relay_authority_heat_sweep_claim_end();
    TEST_CHECK(!relay_authority_heat_sweep_active(), "released claim reads inactive");
}

int main(void)
{
    test_all_unowned_relays_are_in_the_mask();
    test_profile_owned_relay_is_excluded();
    test_autotune_owned_relay_is_excluded();
    test_manual_owned_relay_is_still_in_the_mask();
    test_danger_mode_suppresses_the_whole_mask();
    test_link_up_within_timeout();
    test_link_down_after_timeout();
    test_never_seen_counts_as_down();
    test_traffic_resets_the_timer();
    test_zone_claim_arbitrates_profile_vs_autotune_per_zone();
    test_kiln_nvs_fence_only_when_scope_erases_it();
    test_reset_refuses_writer_exempts_reset_job_task();
    test_reset_in_flight_counter_is_a_depth_count();
    test_heat_sweep_active_follows_the_sweep_claim();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures > 0 ? 1 : 0;
}
