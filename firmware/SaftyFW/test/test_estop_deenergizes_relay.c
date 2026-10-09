// test_estop_deenergizes_relay.c -- the E-stop secondary-protection contract,
// end to end, through real production functions.
//
// OWNER INTENT (2026-09-08), and why this file exists:
//   "the estop input on the safety processor should be wired as part of a
//    double pole switch that also disables power to the large safety relay.
//    it's main purpose is to tell you that power has been cut. but you should
//    also depower the safety relay."
//   "if the user wired it correctly then it should have lost power already
//    but dont take chances."
//
// So there are THREE layers, and this file pins the third:
//   1. Pole 1 of the double-pole E-stop sits IN SERIES with the safety
//      relay's coil circuit. Opening the switch drops the relay by physics.
//      No firmware involvement, and nothing here can test it.
//   2. Pole 2 feeds GPIO9 so the safety processor KNOWS power was cut.
//      That path is `discrete_pin_policy_estop_asserted()` ->
//      `discrete_task_estop_pressed()` -> `safety_guard_input_t::estop_pressed`.
//   3. The firmware ALSO commands the safety relay de-energized, as an
//      independent action that assumes nothing about layers 1 and 2 having
//      worked. That is what this file tests.
//
// Layer 3 is not new code -- safety_core_task()'s trip path has commanded
// relay_owner into TRIPPED since the Phase 5 relay-authority work, and the
// 2026-08-27 audit added the owed/retry latch so a dropped queue send is not
// a silent one-shot. What did NOT exist was a single test asserting the whole
// chain as one property, so nothing would fail if a future edit removed the
// de-energize from relay_owner's TRIP case and left only the report. This
// file is that test.
//
// SPLIT OF TECHNIQUE. safety_core.c and relay_owner.c are not host-compilable
// as executable units (FreeRTOS scheduler, pico-sdk), and relay_owner_task()
// is a static for(;;) that can never be called from host. So:
//   - the pure decisions run for real (safety_guards_tick(),
//     discrete_pin_policy_estop_asserted(), relay_trip_command_still_owed(),
//     relay_trip_transition());
//   - the two links that only exist inside non-compilable task bodies are
//     pinned by a bounded source-text scan of the real .c files, exactly the
//     precedent test_safety_core_s8_wiring.c and
//     test_safety_core_polarity_wiring.c set for this same problem.
// The scan is deliberately narrowed to relay_owner_task()'s
// RELAY_OWNER_CMD_TRIP case rather than the whole file, so a
// hal_gpio_set(SAFTYFW_PIN_RELAY, false) surviving somewhere else (the
// GRACE/TRIPPED refusal branch, or relay_owner_start()'s fail-safe latch)
// cannot make a deleted de-energize look present.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

#include "../src/discrete_pin_policy.h"
#include "../src/safety_guards.h"
#include "../src/tasks/relay_grace.h"
#include "../src/tasks/relay_owner.h"

static safety_guard_input_t estop_base_input(void)
{
    safety_guard_input_t in;
    memset(&in, 0, sizeof(in));
    in.tc_valid = true;
    in.tc_c = 20.0f;
    in.cj_c = 25.0f;
    in.current_sensing_commissioned = true;
    in.sample_counter_advancing = true;
    in.link_up = true;
    in.dt_s = 0.1f;
    return in;
}

static safety_guard_cfg_t estop_base_cfg(void)
{
    safety_guard_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.tc_placement_valid = true;
    cfg.tc_placement_mode = SAFETY_TC_EXTERNAL_OVERHEAT;
    cfg.abs_max_temp_c = 1300.0f;
    cfg.tc_source = SAFETY_TC_SOURCE_OWN_J7;
    return cfg;
}

// --- Layer 2 + the trip decision, with the ESP link DOWN --------------------
// An E-stop is exactly the moment a wider fault may have taken the link with
// it. The safety processor must reach its decision with no help from the ESP.
static void test_estop_trips_with_link_down(void)
{
    TEST_SECTION("E-stop: GPIO9 open -> S7 trip, reached with the ESP link DOWN "
                 "(the safety processor never needs the ESP to act)");

    // Layer 2, for real: GPIO9 high (contact open / wire cut / nothing
    // fitted) is the asserted sense.
    TEST_CHECK(discrete_pin_policy_estop_asserted(true) == true,
               "discrete_pin_policy_estop_asserted(gpio9_high=true) -> pressed");
    TEST_CHECK(discrete_pin_policy_estop_asserted(false) == false,
               "discrete_pin_policy_estop_asserted(gpio9_high=false) -> released");

    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg = estop_base_cfg();

    safety_guard_input_t in = estop_base_input();
    in.link_up = false;              // ESP is gone
    in.any_current_present = false;  // and S6b's soft path needs current, so
                                     // S7 is unambiguously the guard firing
    in.estop_pressed = discrete_pin_policy_estop_asserted(true);

    TEST_CHECK(safety_guards_tick(&s, &cfg, &in) == true,
               "E-stop trips on the very first tick with the link down");
    TEST_CHECK(s.is_tripped, "guard state is latched tripped");
    TEST_CHECK(s.reason == SAFETY_TRIP_ESTOP,
               "reason is SAFETY_TRIP_ESTOP, not a link guard");
}

// --- Idempotence -----------------------------------------------------------
// E-stop is a LEVEL, not an edge: it may be held for hours. The de-energize
// must not thrash the relay or spam the queue once it has landed.
static void test_estop_command_is_idempotent(void)
{
    TEST_SECTION("E-stop: held indefinitely -> exactly one owed relay command, "
                 "not one per tick (no relay thrash, no queue spam)");

    safety_guard_state_t s;
    safety_guards_reset(&s);
    safety_guard_cfg_t cfg = estop_base_cfg();
    safety_guard_input_t in = estop_base_input();
    in.estop_pressed = true;

    // Tick one decides the trip. safety_core_task() latches
    // s_trip_command_owed on exactly this return value.
    bool newly_tripped = safety_guards_tick(&s, &cfg, &in);
    TEST_CHECK(newly_tripped, "tick 1 reports newly-tripped");

    // The send lands: nothing further is owed. This is the real
    // classification function safety_core_task() assigns from.
    bool owed = relay_trip_command_still_owed(s.is_tripped, /*send_succeeded=*/true);
    TEST_CHECK(!owed, "a successful send clears the owed flag -- no re-command");

    // 600 more ticks with the button still held (a minute at 100ms) must not
    // produce a second command.
    int extra_commands = 0;
    for (int i = 0; i < 600; i++) {
        if (safety_guards_tick(&s, &cfg, &in)) {
            extra_commands++;
        }
        if (!s.is_tripped || s.reason != SAFETY_TRIP_ESTOP) {
            extra_commands = -1000; // forces the assertion below to fail loudly
            break;
        }
    }
    TEST_CHECK(extra_commands == 0,
               "60s of held E-stop issues no further relay commands (the trip latch "
               "makes the de-energize idempotent)");
    TEST_CHECK(s.reason == SAFETY_TRIP_ESTOP,
               "and the reason is still SAFETY_TRIP_ESTOP after 600 ticks");
}

// --- Fail-safe when the relay command itself fails --------------------------
static void test_failed_relay_command_is_retried_not_reported_done(void)
{
    TEST_SECTION("E-stop: a DROPPED relay command stays owed and is retried -- "
                 "never reported as a success it did not achieve");

    // relay_owner's command queue is 4 deep and never blocks, so a send can
    // be dropped. The contract: while the guard is tripped, a failed send
    // leaves the command owed, forever, until one actually lands.
    TEST_CHECK(relay_trip_command_still_owed(/*is_tripped=*/true, /*send_succeeded=*/false) == true,
               "tripped + dropped send -> still owed (retried next tick)");
    TEST_CHECK(relay_trip_command_still_owed(/*is_tripped=*/true, /*send_succeeded=*/true) == false,
               "tripped + send landed -> no longer owed");
    // And a clear that arrived while the send was stuck must not leave a
    // stale trip command to re-fire later.
    TEST_CHECK(relay_trip_command_still_owed(/*is_tripped=*/false, /*send_succeeded=*/false) == false,
               "no longer tripped -> nothing owed, a stale trip is not re-issued");
}

// --- Layer 3, the actual de-energize ---------------------------------------
// The two links that live inside non-host-compilable task bodies.
static char *read_src(const char *rel)
{
    const char *candidates[3];
    char alt1[256];
    char alt2[256];
    // candidates[0] is resolved relative to THIS file; the other two are
    // working-directory fallbacks, same shape as test_safety_core_*_wiring.c.
    snprintf(alt1, sizeof(alt1), "firmware/SaftyFW/%s", rel + 3); // strip "../"
    snprintf(alt2, sizeof(alt2), "%s", rel + 3);
    candidates[0] = rel;
    candidates[1] = alt1;
    candidates[2] = alt2;
    return test_read_source_anchored(__FILE__, candidates[0], candidates, 3);
}

// Extracts relay_owner_task()'s RELAY_OWNER_CMD_TRIP case only: from the
// `case RELAY_OWNER_CMD_TRIP:` label up to its `break;`. Narrow on purpose --
// see the file header.
static char *extract_trip_case(const char *text)
{
    const char *start = strstr(text, "case RELAY_OWNER_CMD_TRIP:");
    if (!start) {
        return NULL;
    }
    const char *end = strstr(start, "break;");
    if (!end) {
        return NULL;
    }
    size_t len = (size_t)(end - start);
    char *out = (char *)malloc(len + 1);
    if (!out) {
        return NULL;
    }
    memcpy(out, start, len);
    out[len] = '\0';
    return out;
}

static void test_relay_owner_trip_case_deenergizes(void)
{
    TEST_SECTION("E-stop layer 3: relay_owner's TRIP case actually DE-ENERGIZES "
                 "the safety relay -- it does not merely latch and report");

    char *text = read_src("../src/tasks/relay_owner.c");
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/relay_owner.c -- update the "
                          "candidate paths in this test if the layout moved");
        return;
    }
    char *trip_case = extract_trip_case(text);
    if (!trip_case) {
        TEST_CHECK(false, "could not find relay_owner_task()'s `case RELAY_OWNER_CMD_TRIP:` "
                          "block -- update this test if the command dispatch was restructured");
        free(text);
        return;
    }

    // THE line this whole file exists to protect. If it is deleted, a trip
    // still latches TRIPPED and still refuses future energize commands --
    // which is why every other test in this suite stays green -- but the
    // relay that is energized RIGHT NOW is left energized. On correctly
    // wired hardware pole 1 has already dropped it; this is the layer that
    // covers the hardware being wrong.
    TEST_CHECK(strstr(trip_case, "hal_gpio_set(SAFTYFW_PIN_RELAY, false)") != NULL,
               "safety relay is left ENERGIZED on E-stop: relay_owner's TRIP case no "
               "longer calls hal_gpio_set(SAFTYFW_PIN_RELAY, false). A trip that only "
               "latches state is a report, not a protection.");
    TEST_CHECK(strstr(trip_case, "s_energized = false") != NULL,
               "and the s_energized mirror is cleared in the same case, so S9's "
               "relay_deenergized input agrees with the pin");
    TEST_CHECK(strstr(trip_case, "hal_gpio_set(SAFTYFW_PIN_RELAY, true)") == NULL,
               "and nothing in the TRIP case ever drives the relay high");

    free(trip_case);
    free(text);
}

static void test_safety_core_commands_the_relay_on_trip(void)
{
    TEST_SECTION("E-stop layer 3: safety_core routes the trip through relay_owner "
                 "(the sanctioned owner module), never a direct relay write");

    char *text = read_src("../src/tasks/safety_core.c");
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/safety_core.c -- update the "
                          "candidate paths in this test if the layout moved");
        return;
    }

    TEST_CHECK(strstr(text, "s_trip_command_owed = true;") != NULL,
               "safety_core latches 'a trip command is owed' the tick a trip is decided, "
               "unconditionally -- so a dropped send is retried rather than lost");
    TEST_CHECK(strstr(text, "relay_owner_command_trip(s_guard_state.reason)") != NULL,
               "safety_core issues relay_owner_command_trip() with the decided reason");
    TEST_CHECK(strstr(text, "relay_trip_command_still_owed(s_guard_state.is_tripped") != NULL,
               "and re-derives 'still owed' from the send's ACTUAL result, not from an "
               "assumed success (the unchecked-result success-log bug class)");
    // check_relay_authority_paths enforces this repo-wide; asserting it here
    // too keeps the E-stop story self-contained.
    TEST_CHECK(strstr(text, "hal_gpio_set(SAFTYFW_PIN_RELAY") == NULL,
               "safety_core never writes the relay pin directly -- relay_owner is the "
               "only writer");

    free(text);
}

// --- The latch, for real ---------------------------------------------------
static void test_trip_transition_latches_tripped(void)
{
    TEST_SECTION("E-stop: relay_trip_transition() latches TRIPPED from every state, "
                 "so a held E-stop cannot be re-energized around");

    TEST_CHECK(relay_trip_transition(RELAY_OWNER_STATE_ARMED) == RELAY_OWNER_STATE_TRIPPED,
               "ARMED -> TRIPPED");
    TEST_CHECK(relay_trip_transition(RELAY_OWNER_STATE_GRACE) == RELAY_OWNER_STATE_TRIPPED,
               "GRACE -> TRIPPED");
    TEST_CHECK(relay_trip_transition(RELAY_OWNER_STATE_TRIPPED) == RELAY_OWNER_STATE_TRIPPED,
               "TRIPPED -> TRIPPED (idempotent: a repeated E-stop trip is a no-op)");
}

// --- Configurable polarity, and what "lost signal" means for each setting --
// Owner decision 2026-09-08: polarity configurable, but the state this bench
// is in RIGHT NOW (GPIO9 measured LOW over SWD, 2026-09-08) must be the
// DEFAULT and must read as healthy / safe to fire.
static void test_estop_polarity_default_matches_the_bench(void)
{
    TEST_SECTION("E-stop polarity: the DEFAULT is what this bench is wired for "
                 "(GPIO9 LOW = healthy, safe to fire) and is the fail-safe choice");

    // The compiled default. 0 in every direction: struct default, legacy
    // record's 0x00, erased flash's 0xFF -- all decode here.
    TEST_CHECK(DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH == 0u,
               "ACTIVE_HIGH is the zero value, so every unset/legacy/erased byte "
               "lands on it");

    // Bench state: GPIO9 LOW must be NOT a fault at the default polarity.
    TEST_CHECK(discrete_pin_policy_estop_asserted_ex(
                   false, DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH) == false,
               "default polarity + GPIO9 LOW (this bench, now) -> E-stop NOT asserted, "
               "safe to fire");
    TEST_CHECK(discrete_pin_policy_estop_asserted_ex(
                   true, DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH) == true,
               "default polarity + GPIO9 HIGH -> asserted");

    // A lost signal is a trip IN ITS OWN RIGHT, at the default polarity.
    // R10's 1k pull-up floats a broken line HIGH, so a cut cable, a pulled
    // connector and an unfitted switch are all electrically identical to a
    // pressed button -- and all of them stop.
    TEST_CHECK(discrete_pin_policy_estop_asserted_ex(
                   /*gpio9_high (line broken, pulled up)=*/true,
                   DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH) == true,
               "default polarity: a BROKEN E-stop line reads as STOP -- loss of the "
               "signal is a trip, not a neutral state");

    // Any unrecognised byte must fall through to the fail-safe polarity,
    // never to the one that cannot see a broken wire.
    TEST_CHECK(discrete_pin_policy_estop_asserted_ex(true, 0xFFu) == true,
               "erased-flash 0xFF falls through to ACTIVE_HIGH (broken line = STOP)");
    TEST_CHECK(discrete_pin_policy_estop_asserted_ex(true, 42u) == true,
               "an unrecognised polarity byte falls through to ACTIVE_HIGH, never to "
               "the polarity that cannot detect a lost signal");
}

static void test_estop_polarity_active_low_is_selectable_and_blind(void)
{
    TEST_SECTION("E-stop polarity: ACTIVE_LOW is selectable for a differently-wired "
                 "installation -- and CANNOT detect a broken line. Stated, not hidden.");

    TEST_CHECK(discrete_pin_policy_estop_asserted_ex(
                   false, DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW) == true,
               "ACTIVE_LOW + GPIO9 LOW -> asserted (the inverted installation)");
    TEST_CHECK(discrete_pin_policy_estop_asserted_ex(
                   true, DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW) == false,
               "ACTIVE_LOW + GPIO9 HIGH -> healthy");

    // THE ASYMMETRY, asserted here so it is impossible to ship this knob
    // while believing both settings are equally safe. With R10's pull-up
    // fitted, a broken line floats HIGH; under ACTIVE_LOW that is the
    // HEALTHY reading. So a cut cable is indistinguishable from a working,
    // un-pressed E-stop, and no firmware change can recover it -- only a
    // pull-DOWN on the board would move which failure is detectable.
    bool broken_line_reads_high = true; // R10 1k pull-up to 3.3v_Safty
    TEST_CHECK(discrete_pin_policy_estop_asserted_ex(
                   broken_line_reads_high, DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW) == false,
               "ACTIVE_LOW: a broken line reads HEALTHY. This is the documented, "
               "unavoidable cost of the non-default polarity with a pull-up fitted "
               "(HARDWARE.md 5.1) -- if this ever starts passing as 'asserted', the "
               "pull-up assumption changed and 5.1 needs rewriting.");

    // And the two polarities really are opposites -- no setting collapses
    // both readings onto one answer.
    for (int i = 0; i < 2; i++) {
        bool high = (i == 1);
        TEST_CHECK(discrete_pin_policy_estop_asserted_ex(high, DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH) !=
                       discrete_pin_policy_estop_asserted_ex(high, DISCRETE_PIN_POLICY_ESTOP_ACTIVE_LOW),
                   "the two polarities disagree at every pin level (neither is a no-op)");
    }

    // The default-polarity function and the _ex form must agree, so the
    // legacy call site and the configurable one can never drift.
    TEST_CHECK(discrete_pin_policy_estop_asserted(true) ==
                   discrete_pin_policy_estop_asserted_ex(true, DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH),
               "discrete_pin_policy_estop_asserted() == the _ex form at the default");
    TEST_CHECK(discrete_pin_policy_estop_asserted(false) ==
                   discrete_pin_policy_estop_asserted_ex(false, DISCRETE_PIN_POLICY_ESTOP_ACTIVE_HIGH),
               "... at both pin levels");
}

// discrete_task.c is not host-compilable (hal_gpio/FreeRTOS), so pin the one
// line that carries the configurable polarity into the real sampler -- same
// precedent as test_safety_core_polarity_wiring.c.
static void test_discrete_task_uses_the_configured_polarity(void)
{
    TEST_SECTION("E-stop polarity: discrete_task actually READS the configured "
                 "polarity, rather than the knob existing but never being consulted");

    char *text = read_src("../src/tasks/discrete_task.c");
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/discrete_task.c");
        return;
    }
    TEST_CHECK(strstr(text, "discrete_pin_policy_estop_asserted_ex(") != NULL,
               "discrete_task calls the configurable-polarity form");
    TEST_CHECK(strstr(text, "config_store_get_estop_active_level()") != NULL,
               "and sources the polarity from the commissioning config store, re-read "
               "each sample so a SET_PARAM applies without a reboot (the "
               "consumer-without-a-producer bug class: a knob nothing reads)");
    free(text);
}

void run_test_estop_deenergizes_relay(void)
{
    test_estop_polarity_default_matches_the_bench();
    test_estop_polarity_active_low_is_selectable_and_blind();
    test_discrete_task_uses_the_configured_polarity();
    test_estop_trips_with_link_down();
    test_estop_command_is_idempotent();
    test_failed_relay_command_is_retried_not_reported_done();
    test_relay_owner_trip_case_deenergizes();
    test_safety_core_commands_the_relay_on_trip();
    test_trip_transition_latches_tripped();
}
