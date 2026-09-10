// test_safety_core_ct_calibration_gate.c -- opus review (2026-09-10) finding
// 1: cs_counts_to_amps() (current_sense.c) returns a plausible-looking 0.0f
// for any channel whose k_ct_v_per_a has never been commissioned -- not an
// error, just a reading that happens to be zero. Before this fix,
// safety_core_build_input()'s amps_valid_for_ct[ch] went true off freshness
// alone (current_fresh && !cts_disabled), with no check that the channel's
// own CT scale was ever commissioned. That let S14/S15 (safety_guards.c)
// arm against a fabricated "always 0.00 A, always below expected" reading
// the moment the ESP side (zones_current_sweep_task.c) recorded a nonzero
// i_normal_a for the same channel -- a guaranteed permanent false WARN, kept
// latent today only by both sides of the link happening to read 0.
//
// Like test_safety_core_polarity_wiring.c and test_safety_core_s8_wiring.c
// before it, this pins the ACTUAL source text of safety_core_build_input():
// safety_core.c is not host-compilable (FreeRTOS/pico-sdk), so the function
// cannot be linked and driven directly off-target.
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

static char *read_file_any(const char *const *candidates, size_t count)
{
    return test_read_source_anchored(__FILE__, candidates[0], candidates, count);
}

// Mirrors test_safety_core_polarity_wiring.c's find_build_input_body()
// exactly -- same "signature to the next column-0 closing brace" scan.
static const char *find_build_input_body(const char *text, size_t *out_len)
{
    const char *sig = strstr(text, "safety_core_build_input(");
    if (!sig) {
        return NULL;
    }
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

static void run_ct_calibration_gate_scan(void)
{
    TEST_SECTION("safety_core_build_input(): amps_valid_for_ct[ch] must be gated on "
                 "cfg_rec.k_ct_v_per_a[ch] being commissioned, not on freshness alone "
                 "(source-text scan -- safety_core.c is not host-compilable, same "
                 "precedent as test_safety_core_polarity_wiring.c)");

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

    // The per-channel commissioning gate must exist at all...
    const char *gate = strstr(fn, "if (!(cfg_rec.k_ct_v_per_a[ch] > 0.0f)) {");
    TEST_CHECK(gate != NULL,
               "amps_valid_for_ct[ch]'s loop must refuse an uncommissioned channel "
               "(cfg_rec.k_ct_v_per_a[ch] <= 0.0f). If this fails, the gate was removed or "
               "reworded -- S14/S15 can then arm off a channel current_sense.c can only "
               "report as a fabricated 0.0 A, exactly the false-WARN this test guards "
               "against.");

    // ...and it must come BEFORE `amps_valid_for_ct[ch] = true;` in source
    // order, i.e. it must actually gate the assignment rather than sit
    // somewhere else (dead, or after) in the function body.
    const char *set_valid = gate ? strstr(gate, "amps_valid_for_ct[ch] = true;") : NULL;
    TEST_CHECK(set_valid != NULL,
               "the k_ct_v_per_a commissioning gate must appear BEFORE "
               "`amps_valid_for_ct[ch] = true;` -- a gate that exists but does not actually "
               "precede (and therefore `continue;` past) the assignment provides no "
               "protection at all.");

    free(fn);
    free(text);
}

void run_test_safety_core_ct_calibration_gate(void)
{
    run_ct_calibration_gate_scan();
}
