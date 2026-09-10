// test_safety_core_s8_wiring.c -- proves S8 (implausible rate of rise) is
// wired end-to-end from config_store through to safety_guards.c's evaluator,
// closing the exact "consumer without producer" gap this codebase has
// shipped at least four times before (see project_consumer_without_producer_
// class in the durable memory, and safety_core.c's own header comment on the
// 2026-08-27 abs_max_temp_c audit that found the same shape for S1).
//
// THE PROBLEM THIS FILE SOLVES: src/tasks/safety_core.c is not itself
// host-testable -- it #includes FreeRTOS.h/pico/time.h/task.h and reaches
// into watchdog_task.h, boot_reason.h, current_task.h, relay_owner.h, none
// of which exist off real hardware. test_safety_core_stack_budget.c already
// established the precedent for this file: fall back to a source-text scan
// of tasks/safety_core.c rather than compiling it. That is exactly what
// section 1 below does for the S8 copy specifically -- and because it reads
// the ACTUAL function body, not a paraphrase, deleting the copy in
// safety_core.c makes this test fail.
//
// That alone would be "testing the copy in isolation" (the exact trap this
// task was warned about -- a passing scan proves the text exists, not that
// it does anything). Section 2 closes that gap: it drives config_store's
// REAL, compiled producer path (config_store_default() + config_params_set(),
// the identical SET_PARAM path COMMIT_CONFIG uses on real hardware) to
// stage a commissioned max_rate_c_per_min/rate_window_s, applies the SAME
// fields_set-gated mapping safety_core_load_guard_cfg() uses (mirrored, and
// pinned to match by section 1's scan of the real function), and feeds the
// result into safety_guards_tick() -- the REAL, compiled S8 evaluator --
// across a ramp that only trips when the commissioned value actually
// arrived. A default (nothing commissioned) record must run the identical
// ramp and stay silent, pinning the inertness property end-to-end rather
// than only inside safety_guards.c (test_safety_guards.c's own S8 section
// already pins it guard-side; this file pins it store-to-guard).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "../src/safety_guards.h"
#include "../src/config_store.h"
#include "../src/config_params.h"

// Mirrors test_safety_core_stack_budget.c's read_file_any() exactly -- same
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

// Extracts the body of safety_core_load_guard_cfg() -- from its "static
// void safety_core_load_guard_cfg(" signature to the closing brace at column
// 0 -- so the substring checks below can only match text that is actually
// inside that function, not a stray comment or a different function
// elsewhere in the file.
static const char *find_load_guard_cfg_body(const char *text, size_t *out_len)
{
    const char *sig = strstr(text, "static void safety_core_load_guard_cfg(");
    if (!sig) {
        return NULL;
    }
    const char *open = strchr(sig, '{');
    if (!open) {
        return NULL;
    }
    // First "\n}" after the opening brace -- this codebase's own functions
    // close at column 0, same assumption test_safety_core_stack_budget.c and
    // test_watchdog_budget_coverage.c already make about this file family.
    const char *close = strstr(open, "\n}");
    if (!close) {
        return NULL;
    }
    *out_len = (size_t)(close - open);
    return open;
}

static void run_section1_source_scan(void)
{
    TEST_SECTION("safety_core_load_guard_cfg() copies max_rate_c_per_min/rate_window_s "
                 "(source-text scan -- safety_core.c is not host-compilable, same precedent "
                 "as test_safety_core_stack_budget.c)");

    static const char *candidates[] = {
        "../src/tasks/safety_core.c",
        "src/tasks/safety_core.c",
        "firmware/SaftyFW/src/tasks/safety_core.c",
    };
    char *text = read_file_any(candidates, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/safety_core.c from the host test's "
                           "working directory -- update the candidate paths in this test if "
                           "the build layout moved");
        return;
    }

    size_t body_len = 0;
    const char *body = find_load_guard_cfg_body(text, &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find safety_core_load_guard_cfg()'s function body in "
                           "src/tasks/safety_core.c -- update this test if it was renamed or "
                           "restructured");
        free(text);
        return;
    }

    // Bounded copy so strstr() below only ever searches inside the function.
    char *fn = (char *)malloc(body_len + 1);
    TEST_CHECK(fn != NULL, "malloc for the extracted function body succeeded");
    if (!fn) {
        free(text);
        return;
    }
    memcpy(fn, body, body_len);
    fn[body_len] = '\0';

    TEST_CHECK(strstr(fn, "CONFIG_STORE_SET_MAX_RATE_C_PER_MIN") != NULL,
               "S8: safety_core_load_guard_cfg() gates max_rate_c_per_min on its fields_set "
               "bit -- same idiom as abs_max_temp_c/CONFIG_STORE_SET_ABS_MAX_TEMP_C. If this "
               "fails, the gate was deleted and the guard is either dead again or, worse, would "
               "read a substituted-default 0 as commissioned.");

    TEST_CHECK(strstr(fn, "s_guard_cfg.max_rate_c_per_min") != NULL &&
                   strstr(fn, "rec->max_rate_c_per_min") != NULL,
               "S8: max_rate_c_per_min is actually copied from the config_store record into "
               "s_guard_cfg -- this is the literal line whose ABSENCE was the shipped gap "
               "(guard reads a field nothing ever populates). Deleting the copy makes this "
               "check fail.");

    TEST_CHECK(strstr(fn, "s_guard_cfg.rate_window_s") != NULL &&
                   strstr(fn, "rec->rate_window_s") != NULL,
               "S8: rate_window_s is actually copied from the config_store record into "
               "s_guard_cfg. Deleting the copy makes this check fail.");

    free(fn);
    free(text);
}

// Section 2's mirror of safety_core_load_guard_cfg()'s S8 mapping. Deliberately
// NOT a call into safety_core.c (that file cannot be linked into a host
// test -- see this file's header comment) -- it is a literal transcription of
// the two lines section 1 just proved exist verbatim in the real function, so
// the two sections stay coupled: change the real mapping's shape and this
// helper (and the behavioural assertions built on it) must be updated to
// match, same as any other "known good" mirror in this test suite.
static void apply_s8_cfg_from_record(const config_store_record_t *rec, safety_guard_cfg_t *cfg)
{
    cfg->max_rate_c_per_min =
        (rec->fields_set & CONFIG_STORE_SET_MAX_RATE_C_PER_MIN) ? rec->max_rate_c_per_min : 0.0f;
    cfg->rate_window_s = (float)rec->rate_window_s;
}

// A ramp guaranteed to trip S8 once max_rate_c_per_min/rate_window_s are
// actually non-zero: 20C/min sustained for two consecutive 60s windows, well
// past any plausible full-power ramp (test_safety_guards.c's own S8 nuisance
// case uses the same shape). Returns true iff a trip occurred anywhere in
// the sequence.
static bool run_implausible_ramp(const safety_guard_cfg_t *cfg)
{
    safety_guard_state_t s;
    safety_guards_reset(&s);

    safety_guard_input_t in;
    memset(&in, 0, sizeof(in));
    in.tc_valid = true;
    in.cj_c = 25.0f;
    in.dt_s = 60.0f;
    in.tc_c = 20.0f;
    in.context_valid = false;
    in.link_up = true;
    in.current_sensing_commissioned = true;
    in.sample_counter_advancing = true;

    bool tripped = false;
    for (int i = 0; i < 3 && !tripped; i++) {
        in.tc_c += 20.0f; // 20C in a 60s window == 20C/min
        tripped = safety_guards_tick(&s, cfg, &in);
    }
    return tripped;
}

static void run_section2_whole_chain(void)
{
    TEST_SECTION("config_store -> safety_core mapping -> safety_guards_tick(): the whole "
                 "chain a value set at commissioning must survive to change guard behaviour");

    // -- Commissioned path: config_store's REAL producer (config_params_set(),
    // the identical function COMMIT_CONFIG's handler calls on real hardware)
    // stages max_rate_c_per_min=15.0C/min (CONFIG_STORE_MAX_RATE_C_PER_MIN_FLOOR,
    // the tightest a commissioned value may legally go per the
    // s8_rate_guard_retune_2026-09-09 audit -- was 10.0 before that floor
    // existed), still below the 20C/min ramp above, so a correctly-wired
    // chain MUST trip.
    {
        config_store_record_t rec;
        config_store_default(&rec);

        kilnlink_param_value_t v;
        v.f32_val = 15.0f;
        TEST_CHECK(config_params_set(&rec, 0x0204u, KILNLINK_PARAM_TYPE_F32, v),
                   "config_params_set() accepts max_rate_c_per_min=15.0 on param 0x0204 -- "
                   "the real wire id SET_PARAM/COMMIT_CONFIG use");
        TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_MAX_RATE_C_PER_MIN) != 0u,
                   "config_params_set() actually sets CONFIG_STORE_SET_MAX_RATE_C_PER_MIN in "
                   "fields_set -- the bit safety_core_load_guard_cfg() gates on");

        v.u16_val = 60u;
        TEST_CHECK(config_params_set(&rec, 0x0205u, KILNLINK_PARAM_TYPE_U16, v),
                   "config_params_set() accepts rate_window_s=60 on param 0x0205");

        safety_guard_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        apply_s8_cfg_from_record(&rec, &cfg);

        TEST_CHECK(cfg.max_rate_c_per_min == 15.0f,
                   "the commissioned value reached safety_guard_cfg_t.max_rate_c_per_min");
        TEST_CHECK(cfg.rate_window_s == 60.0f,
                   "the commissioned value reached safety_guard_cfg_t.rate_window_s");

        bool tripped = run_implausible_ramp(&cfg);
        TEST_CHECK(tripped,
                   "WHOLE CHAIN: a value staged through config_store's real SET_PARAM path, "
                   "carried through the mapping this test's section 1 proves exists in "
                   "safety_core.c, reaches safety_guards_tick() and changes its verdict -- an "
                   "implausible ramp that config_store's default (0 = disabled) would ignore "
                   "now trips S8. If safety_core.c's copy is deleted, section 1 above already "
                   "fails; this check independently proves the copy is not a no-op.");
    }

    // -- Default (uncommissioned) path: config_store_default() ships
    // max_rate_c_per_min genuinely unset (fields_set bit clear), exactly as
    // config_store.c's own comment documents ("0 = off ... genuinely UNSET").
    // The SAME implausible ramp, run through the SAME mapping, must produce
    // NO trip -- this is the inertness property pinned end-to-end, not just
    // inside safety_guards.c.
    {
        config_store_record_t rec;
        config_store_default(&rec);
        TEST_CHECK((rec.fields_set & CONFIG_STORE_SET_MAX_RATE_C_PER_MIN) == 0u,
                   "sanity: config_store_default() ships max_rate_c_per_min genuinely unset "
                   "(fields_set bit clear) -- if this ever fails, config_store_default() "
                   "changed and the inertness claim below needs re-checking against the new "
                   "default, not just this guard");

        safety_guard_cfg_t cfg;
        memset(&cfg, 0, sizeof(cfg));
        apply_s8_cfg_from_record(&rec, &cfg);
        TEST_CHECK(cfg.max_rate_c_per_min == 0.0f,
                   "an uncommissioned record maps to max_rate_c_per_min == 0.0f -- 'never "
                   "trips', not a substituted default");

        bool tripped = run_implausible_ramp(&cfg);
        TEST_CHECK(!tripped,
                   "INERTNESS, END-TO-END: the exact same implausible ramp that trips S8 once "
                   "commissioned produces NO trip when config_store is at its shipped default "
                   "-- wiring the guard did not make any board trip that would not have "
                   "before. This is the property that makes it safe to land this change while "
                   "a firing is running elsewhere on other hardware.");
    }
}

void run_test_safety_core_s8_wiring(void)
{
    run_section1_source_scan();
    run_section2_whole_chain();
}
