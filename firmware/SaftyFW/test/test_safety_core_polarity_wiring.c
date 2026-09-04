// test_safety_core_polarity_wiring.c -- ROADMAP.md M15 A4 (virtual_dut
// above-the-polarity-layer audit). safety_core.c is not host-compilable
// (FreeRTOS/pico-sdk), so like test_safety_core_s8_wiring.c and
// test_safety_core_stack_budget.c before it, this pins the ACTUAL source
// text of safety_core_build_input() rather than a paraphrase.
//
// WHY THIS FILE EXISTS: the audit found that every synthesized input in
// test_safety_guards.c (estop_pressed, main_fault_asserted, tc_valid/tc_c,
// amps[], link_up, ...) is already documented, BY DESIGN, as a pre-reduced
// scalar -- safety_guard_input_t's own header comment calls this out, and
// each raw-to-scalar reduction now has its own pure, host-tested module
// (discrete_pin_policy.c, max31856_*_policy.c, current_presence_policy.c,
// ct_amps_cal.c, snapshots.c, the kilnlink_*_decode() codecs). That closed
// the exact shape of the shipped S7 bug -- see discrete_task.c's own comment
// on "virtual_dut synthesizes estop_pressed directly and never exercises a
// GPIO read" -- for every input that has a raw layer to push the stub below.
//
// What was still open: the one-line ASSIGNMENTS inside
// safety_core_build_input() that carry a bare polarity inversion (or the
// deliberate ABSENCE of one) with nothing host-testable checking they stay
// that way, because the function itself cannot be compiled off-target. A
// silent edit flipping any of these would still pass every existing host
// test (test_safety_guards.c feeds safety_guard_input_t directly, so it
// cannot see how that struct got built) and would only be caught on real
// hardware -- exactly the "invisible to 378/378 green checks" shape the S7
// bug had. This file closes that gap the same way S8's wiring gap was
// closed: a bounded source-text scan of the real, compiled function.
//
// Three lines audited here, chosen because each is a single '!' (or its
// deliberate absence) away from silently disabling a guard:
//   .estop_pressed        = discrete_task_estop_pressed()   -- S7, no '!'
//   .main_fault_asserted  = discrete_task_main_fault()      -- S6a, no '!'
//   .relay_deenergized    = !relay_owner_is_energized()     -- S9, one '!'
// (estop_pressed/main_fault_asserted must NOT be negated here: discrete_task
// already returns the asserted-sense boolean, per its own and safety_core.c's
// comments. relay_deenergized MUST be negated: relay_owner publishes the
// affirmative "energized" fact, safety_guard_input_t wants the negative.)
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

// Mirrors test_safety_core_s8_wiring.c's read_file_any() exactly.
static char *read_file_any(const char *const *candidates, size_t count)
{
    for (size_t i = 0; i < count; i++) {
        FILE *f = fopen(candidates[i], "rb");
        if (!f) {
            continue;
        }
        if (fseek(f, 0, SEEK_END) != 0) {
            fclose(f);
            continue;
        }
        long len = ftell(f);
        if (len < 0) {
            fclose(f);
            continue;
        }
        rewind(f);
        char *buf = (char *)malloc((size_t)len + 1);
        if (!buf) {
            fclose(f);
            return NULL;
        }
        size_t got = fread(buf, 1, (size_t)len, f);
        fclose(f);
        buf[got] = '\0';
        return buf;
    }
    return NULL;
}

// Extracts safety_core_build_input()'s body, same "signature to the first
// column-0 closing brace" convention as find_load_guard_cfg_body() in
// test_safety_core_s8_wiring.c.
static const char *find_build_input_body(const char *text, size_t *out_len)
{
    const char *sig = strstr(text, "safety_core_build_input(");
    if (!sig) {
        return NULL;
    }
    // Skip to that function's OWN definition, not just any call site: find
    // the next "{" and confirm it is not immediately followed by ");" (a
    // forward declaration/call). The definition in this file is
    // `static safety_guard_input_t safety_core_build_input(void)\n{`.
    const char *scan = sig;
    for (;;) {
        scan = strstr(scan, "safety_core_build_input(");
        if (!scan) {
            return NULL;
        }
        const char *paren_close = strchr(scan, ')');
        if (!paren_close) {
            return NULL;
        }
        // Walk past whitespace/newlines to the next non-space char.
        const char *p = paren_close + 1;
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
            p++;
        }
        if (*p == '{') {
            const char *close = strstr(p, "\n}");
            if (!close) {
                return NULL;
            }
            *out_len = (size_t)(close - p);
            return p;
        }
        scan = paren_close;
    }
}

static void run_polarity_wiring_scan(void)
{
    TEST_SECTION("safety_core_build_input(): estop_pressed/main_fault_asserted/"
                 "relay_deenergized keep the exact polarity SAFETY_MODEL.md requires "
                 "(source-text scan -- safety_core.c is not host-compilable, same "
                 "precedent as test_safety_core_s8_wiring.c)");

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
    const char *body = find_build_input_body(text, &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find safety_core_build_input()'s function body in "
                           "src/tasks/safety_core.c -- update this test if it was renamed or "
                           "restructured");
        free(text);
        return;
    }

    char *fn = (char *)malloc(body_len + 1);
    TEST_CHECK(fn != NULL, "malloc for the extracted function body succeeded");
    if (!fn) {
        free(text);
        return;
    }
    memcpy(fn, body, body_len);
    fn[body_len] = '\0';

    // S7: NOT negated. discrete_task_estop_pressed() already returns the
    // asserted-sense boolean (GPIO9 active HIGH, decoded by
    // discrete_pin_policy_estop_asserted() and debounced in discrete_task.c);
    // a stray '!' inserted here would silently disable S7 again, in the
    // opposite direction from the original bug, invisible to every existing
    // host test because none of them exercise this call site.
    TEST_CHECK(strstr(fn, ".estop_pressed = discrete_task_estop_pressed()") != NULL,
               "S7: .estop_pressed is assigned discrete_task_estop_pressed() with no "
               "negation. If this fails, either the call was removed/renamed or a '!' was "
               "added -- both silently disable the E-stop guard on real hardware while "
               "every host test (which feeds safety_guard_input_t directly) stays green.");

    // S6a: NOT negated, same reasoning -- discrete_task_main_fault() already
    // returns the asserted sense (GPIO10 active LOW, inverted once inside
    // discrete_pin_policy.c).
    TEST_CHECK(strstr(fn, ".main_fault_asserted = discrete_task_main_fault()") != NULL,
               "S6a: .main_fault_asserted is assigned discrete_task_main_fault() with no "
               "negation. If this fails, a '!' was added or the call changed -- either "
               "silently disables the mainFault guard on real hardware.");

    // S9: MUST be negated -- relay_owner_is_energized() reports the
    // affirmative, safety_guard_input_t::relay_deenergized wants the
    // negative (see that field's own header comment and the cross-check
    // against out_relay_energized this file's comment block describes).
    // Losing the '!' would make S9's "did K4 actually open" check read a
    // welded-closed contactor as successfully de-energized.
    TEST_CHECK(strstr(fn, ".relay_deenergized = !relay_owner_is_energized()") != NULL,
               "S9: .relay_deenergized is assigned the NEGATION of "
               "relay_owner_is_energized(). If this fails, the '!' was dropped or doubled "
               "-- either makes S9's trip-verify check trust a contactor that never opened.");

    free(fn);
    free(text);
}

void run_test_safety_core_polarity_wiring(void)
{
    run_polarity_wiring_scan();
}
