// Host tests for App/drivers/rules_eval.c -- the pure rule-evaluator logic
// TODO.md section 6 (via rules_http.c's disclaimer) said did not exist yet.
// No ESP-IDF dependency, mirrors test_heat_interlock.c's own pattern for the
// same pure/impure split.
#include <string.h>

#include "test_common.h"

#include "../drivers/rules_eval.h"

static rules_eval_inputs_t make_inputs(void)
{
    rules_eval_inputs_t in;
    memset(&in, 0, sizeof(in));
    for (uint8_t i = 0; i < RULES_EVAL_ZONE_COUNT; i++) {
        in.zone_temp_valid[i] = true;
        in.zone_temp_c[i] = 20.0f;
    }
    return in;
}

static relay_rules_cfg_t make_cfg(void)
{
    relay_rules_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    return cfg;
}

/* ---- rules_eval_condition -- TEMP ----------------------------------------- */

static void test_temp_ge_fires_above_and_at_threshold(void)
{
    TEST_SECTION("rules_eval_condition -- TEMP GE fires at/above threshold, not below");

    rules_eval_inputs_t in = make_inputs();
    in.zone_temp_c[0] = 500.0f;
    rule_condition_t cond = { .type = COND_TEMP, .zone_index = 0, .cmp = CMP_GE, .threshold_c = 500.0f };

    TEST_CHECK(rules_eval_condition(&cond, &in) == true, "500.0 >= 500.0 fires");
    in.zone_temp_c[0] = 500.1f;
    TEST_CHECK(rules_eval_condition(&cond, &in) == true, "500.1 >= 500.0 fires");
    in.zone_temp_c[0] = 499.9f;
    TEST_CHECK(rules_eval_condition(&cond, &in) == false, "499.9 >= 500.0 does not fire");
}

static void test_temp_le_fires_below_and_at_threshold(void)
{
    TEST_SECTION("rules_eval_condition -- TEMP LE fires at/below threshold, not above");

    rules_eval_inputs_t in = make_inputs();
    rule_condition_t cond = { .type = COND_TEMP, .zone_index = 1, .cmp = CMP_LE, .threshold_c = 100.0f };

    in.zone_temp_c[1] = 100.0f;
    TEST_CHECK(rules_eval_condition(&cond, &in) == true, "100.0 <= 100.0 fires");
    in.zone_temp_c[1] = 99.9f;
    TEST_CHECK(rules_eval_condition(&cond, &in) == true, "99.9 <= 100.0 fires");
    in.zone_temp_c[1] = 100.1f;
    TEST_CHECK(rules_eval_condition(&cond, &in) == false, "100.1 <= 100.0 does not fire");
}

static void test_temp_invalid_zone_never_fires_either_comparator(void)
{
    TEST_SECTION("rules_eval_condition -- TEMP fails safe (false) on an invalid zone reading, both comparators");

    rules_eval_inputs_t in = make_inputs();
    in.zone_temp_valid[0] = false;
    in.zone_temp_c[0] = 9999.0f; /* would satisfy GE trivially if validity were ignored */

    rule_condition_t ge = { .type = COND_TEMP, .zone_index = 0, .cmp = CMP_GE, .threshold_c = 0.0f };
    rule_condition_t le = { .type = COND_TEMP, .zone_index = 0, .cmp = CMP_LE, .threshold_c = 9999.0f };

    TEST_CHECK(rules_eval_condition(&ge, &in) == false,
               "invalid zone reading does not satisfy GE even against a trivial threshold");
    TEST_CHECK(rules_eval_condition(&le, &in) == false,
               "invalid zone reading does not satisfy LE even against a trivial threshold");
}

static void test_temp_out_of_range_zone_index_never_fires(void)
{
    TEST_SECTION("rules_eval_condition -- TEMP with out-of-range zone_index fails safe");

    rules_eval_inputs_t in = make_inputs();
    rule_condition_t cond = { .type = COND_TEMP, .zone_index = RULES_EVAL_ZONE_COUNT, .cmp = CMP_GE,
                              .threshold_c = -1000.0f };
    TEST_CHECK(rules_eval_condition(&cond, &in) == false, "out-of-range zone_index never fires");
}

/* ---- rules_eval_condition -- TIME ------------------------------------------ */

static void test_time_ge_and_le(void)
{
    TEST_SECTION("rules_eval_condition -- TIME GE/LE against profile_elapsed_s");

    rules_eval_inputs_t in = make_inputs();
    in.profile_running = true;
    in.profile_elapsed_s = 3600;

    rule_condition_t ge = { .type = COND_TIME, .cmp = CMP_GE, .seconds = 3600 };
    rule_condition_t ge_over = { .type = COND_TIME, .cmp = CMP_GE, .seconds = 3601 };
    rule_condition_t le = { .type = COND_TIME, .cmp = CMP_LE, .seconds = 3600 };
    rule_condition_t le_under = { .type = COND_TIME, .cmp = CMP_LE, .seconds = 3599 };

    TEST_CHECK(rules_eval_condition(&ge, &in) == true, "elapsed 3600 >= 3600 fires");
    TEST_CHECK(rules_eval_condition(&ge_over, &in) == false, "elapsed 3600 >= 3601 does not fire");
    TEST_CHECK(rules_eval_condition(&le, &in) == true, "elapsed 3600 <= 3600 fires");
    TEST_CHECK(rules_eval_condition(&le_under, &in) == false, "elapsed 3600 <= 3599 does not fire");
}

static void test_time_no_profile_running_never_fires(void)
{
    TEST_SECTION("rules_eval_condition -- TIME fails safe (false) with no profile running, "
                 "including TIME LE 0 which would otherwise be a false positive against elapsed==0");

    rules_eval_inputs_t in = make_inputs();
    in.profile_running = false;
    in.profile_elapsed_s = 0; /* zero-initialized default, not a real running profile */

    rule_condition_t le_zero = { .type = COND_TIME, .cmp = CMP_LE, .seconds = 0 };
    rule_condition_t ge_zero = { .type = COND_TIME, .cmp = CMP_GE, .seconds = 0 };

    TEST_CHECK(rules_eval_condition(&le_zero, &in) == false,
               "TIME LE 0 does not fire with no profile running, despite elapsed_s == 0");
    TEST_CHECK(rules_eval_condition(&ge_zero, &in) == false,
               "TIME GE 0 does not fire with no profile running either");
}

/* ---- rules_eval_condition -- RELAY ----------------------------------------- */

static void test_relay_condition_matches_commanded_state(void)
{
    TEST_SECTION("rules_eval_condition -- RELAY matches another relay's commanded state");

    rules_eval_inputs_t in = make_inputs();
    in.relay_commanded_on[0] = true; /* relay 1 */

    rule_condition_t want_on = { .type = COND_RELAY, .other_relay = 1, .other_relay_state = true };
    rule_condition_t want_off = { .type = COND_RELAY, .other_relay = 1, .other_relay_state = false };

    TEST_CHECK(rules_eval_condition(&want_on, &in) == true, "relay 1 commanded ON matches 'ON' condition");
    TEST_CHECK(rules_eval_condition(&want_off, &in) == false, "relay 1 commanded ON does not match 'OFF' condition");
}

static void test_relay_condition_out_of_range_never_fires(void)
{
    TEST_SECTION("rules_eval_condition -- RELAY with out-of-range other_relay fails safe");

    rules_eval_inputs_t in = make_inputs();
    rule_condition_t bad_zero = { .type = COND_RELAY, .other_relay = 0, .other_relay_state = false };
    rule_condition_t bad_high = { .type = COND_RELAY, .other_relay = RULES_EVAL_RELAY_COUNT + 1,
                                  .other_relay_state = false };
    TEST_CHECK(rules_eval_condition(&bad_zero, &in) == false, "other_relay 0 never fires");
    TEST_CHECK(rules_eval_condition(&bad_high, &in) == false, "other_relay past the count never fires");
}

/* ---- rules_eval_relay_wants_on ---------------------------------------------- */

static void test_relay_wants_on_requires_rule_driven(void)
{
    TEST_SECTION("rules_eval_relay_wants_on -- rule_driven=false never fires, even with a satisfied rule");

    relay_rules_cfg_t cfg = make_cfg();
    cfg.rule_driven = false;
    cfg.rules[0].condition_count = 1;
    cfg.rules[0].conditions[0] = (rule_condition_t){ .type = COND_TEMP, .zone_index = 0, .cmp = CMP_GE,
                                                      .threshold_c = -1000.0f }; /* trivially true */
    rules_eval_inputs_t in = make_inputs();

    TEST_CHECK(rules_eval_relay_wants_on(&cfg, &in) == false,
               "rule_driven=false suppresses an otherwise-satisfied rule");
}

static void test_relay_wants_on_and_across_conditions(void)
{
    TEST_SECTION("rules_eval_relay_wants_on -- a rule's conditions are AND'd (all must hold)");

    relay_rules_cfg_t cfg = make_cfg();
    cfg.rule_driven = true;
    cfg.rules[0].condition_count = 2;
    cfg.rules[0].conditions[0] = (rule_condition_t){ .type = COND_TEMP, .zone_index = 0, .cmp = CMP_GE,
                                                      .threshold_c = 500.0f };
    cfg.rules[0].conditions[1] = (rule_condition_t){ .type = COND_TEMP, .zone_index = 1, .cmp = CMP_GE,
                                                      .threshold_c = 500.0f };

    rules_eval_inputs_t in = make_inputs();
    in.zone_temp_c[0] = 600.0f; /* satisfies condition 0 */
    in.zone_temp_c[1] = 10.0f;  /* does NOT satisfy condition 1 */
    TEST_CHECK(rules_eval_relay_wants_on(&cfg, &in) == false,
               "one satisfied condition out of two AND'd conditions does not fire the rule");

    in.zone_temp_c[1] = 600.0f; /* now both satisfied */
    TEST_CHECK(rules_eval_relay_wants_on(&cfg, &in) == true,
               "both AND'd conditions satisfied fires the rule");
}

static void test_relay_wants_on_or_across_rules(void)
{
    TEST_SECTION("rules_eval_relay_wants_on -- a relay's rules are OR'd (any one rule firing is enough)");

    relay_rules_cfg_t cfg = make_cfg();
    cfg.rule_driven = true;
    /* Rule 0: zone 0 >= 500 (will be false). Rule 1: zone 1 <= 10 (will be true). */
    cfg.rules[0].condition_count = 1;
    cfg.rules[0].conditions[0] = (rule_condition_t){ .type = COND_TEMP, .zone_index = 0, .cmp = CMP_GE,
                                                      .threshold_c = 500.0f };
    cfg.rules[1].condition_count = 1;
    cfg.rules[1].conditions[0] = (rule_condition_t){ .type = COND_TEMP, .zone_index = 1, .cmp = CMP_LE,
                                                      .threshold_c = 10.0f };

    rules_eval_inputs_t in = make_inputs();
    in.zone_temp_c[0] = 20.0f; /* rule 0 false */
    in.zone_temp_c[1] = 5.0f;  /* rule 1 true */

    TEST_CHECK(rules_eval_relay_wants_on(&cfg, &in) == true,
               "rule 1 firing alone is enough even though rule 0 does not fire (OR across rules)");
}

static void test_relay_wants_on_empty_rule_slot_never_fires(void)
{
    TEST_SECTION("rules_eval_relay_wants_on -- an unused rule slot (condition_count==0) never fires "
                 "(no vacuous-AND-true default-on)");

    relay_rules_cfg_t cfg = make_cfg();
    cfg.rule_driven = true;
    /* All rules[].condition_count are 0 from make_cfg()'s zero-init. */
    rules_eval_inputs_t in = make_inputs();

    TEST_CHECK(rules_eval_relay_wants_on(&cfg, &in) == false,
               "rule_driven with zero configured conditions does not turn the relay on");
}

/* ---- rules_eval_decide -- the fail-safe gates ------------------------------ */

static void test_decide_off_when_rules_dont_want_on_regardless_of_gates(void)
{
    TEST_SECTION("rules_eval_decide -- OFF is never gated: rules not wanting on stays off "
                 "even with both gates healthy");

    relay_rules_cfg_t cfg = make_cfg();
    cfg.rule_driven = true; /* no rules configured -- wants_on is false */
    rules_eval_inputs_t in = make_inputs();

    TEST_CHECK(rules_eval_decide(&cfg, &in, /*safety_link_ok=*/true, /*heat_interlock_ok=*/true) == false,
               "no rule fires -> off, independent of gate state");
}

static void test_decide_blocks_on_when_safety_link_down(void)
{
    TEST_SECTION("rules_eval_decide -- fail-safe: a satisfied rule is still refused when "
                 "safety_link_ok is false (safety link down / a fault is asserted)");

    relay_rules_cfg_t cfg = make_cfg();
    cfg.rule_driven = true;
    cfg.rules[0].condition_count = 1;
    cfg.rules[0].conditions[0] = (rule_condition_t){ .type = COND_TEMP, .zone_index = 0, .cmp = CMP_GE,
                                                      .threshold_c = -1000.0f }; /* trivially satisfied */
    rules_eval_inputs_t in = make_inputs();

    TEST_CHECK(rules_eval_relay_wants_on(&cfg, &in) == true, "sanity: the rule itself is satisfied");
    TEST_CHECK(rules_eval_decide(&cfg, &in, /*safety_link_ok=*/false, /*heat_interlock_ok=*/true) == false,
               "a satisfied rule is refused ON when the safety link/relay-authority gate is not clear");
    TEST_CHECK(rules_eval_decide(&cfg, &in, /*safety_link_ok=*/true, /*heat_interlock_ok=*/true) == true,
               "the same rule IS allowed on once the safety gate is clear (control case, proves the "
               "refusal above is the gate, not a bug in the rule itself)");
}

static void test_decide_blocks_on_when_heat_interlock_open(void)
{
    TEST_SECTION("rules_eval_decide -- fail-safe: a satisfied rule is still refused when "
                 "heat_interlock_ok is false (an ESP/Pico OTA update is in progress)");

    relay_rules_cfg_t cfg = make_cfg();
    cfg.rule_driven = true;
    cfg.rules[0].condition_count = 1;
    cfg.rules[0].conditions[0] = (rule_condition_t){ .type = COND_TEMP, .zone_index = 0, .cmp = CMP_GE,
                                                      .threshold_c = -1000.0f };
    rules_eval_inputs_t in = make_inputs();

    TEST_CHECK(rules_eval_decide(&cfg, &in, /*safety_link_ok=*/true, /*heat_interlock_ok=*/false) == false,
               "a satisfied rule is refused ON while an OTA update holds the heat interlock");
    TEST_CHECK(rules_eval_decide(&cfg, &in, /*safety_link_ok=*/true, /*heat_interlock_ok=*/true) == true,
               "the same rule IS allowed on once the interlock clears (control case)");
}

void run_test_rules_eval(void)
{
    test_temp_ge_fires_above_and_at_threshold();
    test_temp_le_fires_below_and_at_threshold();
    test_temp_invalid_zone_never_fires_either_comparator();
    test_temp_out_of_range_zone_index_never_fires();
    test_time_ge_and_le();
    test_time_no_profile_running_never_fires();
    test_relay_condition_matches_commanded_state();
    test_relay_condition_out_of_range_never_fires();
    test_relay_wants_on_requires_rule_driven();
    test_relay_wants_on_and_across_conditions();
    test_relay_wants_on_or_across_rules();
    test_relay_wants_on_empty_rule_slot_never_fires();
    test_decide_off_when_rules_dont_want_on_regardless_of_gates();
    test_decide_blocks_on_when_safety_link_down();
    test_decide_blocks_on_when_heat_interlock_open();
}
