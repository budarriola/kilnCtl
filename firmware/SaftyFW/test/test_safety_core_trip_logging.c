// test_safety_core_trip_logging.c -- proves the ORIGINAL trip event (step 4
// of SAFETY_MODEL.md section 6's 4-step trip order) is actually logged via
// log_task, closing the gap safety_core.c's own comment used to name
// explicitly: "log_task now exists (see the CLEAR_TRIP drain right below,
// which does use it), but wiring the ORIGINAL trip event to it is separate,
// later work, not this pass's."
//
// THE PROBLEM THIS FILE SOLVES: src/tasks/safety_core.c is not itself
// host-testable -- it #includes FreeRTOS.h/pico/time.h/task.h and reaches
// into watchdog_task.h, boot_reason.h, current_task.h, relay_owner.h, none
// of which exist off real hardware. Same precedent test_safety_core_stack_
// budget.c and test_safety_core_s8_wiring.c already established: fall back
// to a source-text scan of the real, compiled file rather than pretending
// a stub copy is production code (see project_negative_test_on_a_mirror_
// is_vacuous in durable memory for why a test-local copy would be vacuous).
//
// Section 1 extracts the body of the `if (newly_tripped) { ... }` block
// inside safety_core_task() and checks that it actually calls
// log_task_log() with the trip tag, INSIDE that block (so a call added
// somewhere unrelated in the file cannot satisfy this test), and AFTER the
// K4-command/latch lines (s_trip_command_owed = true;,
// boot_reason_latch_trip(...)) so the audit-trail write can never be
// positioned to precede or gate the actual trip action -- this repo's
// working rule that logging must never extend or gate a safety decision.
//
// Section 2 pins the log level: a trip is unconditionally an ERROR-level
// event (mirroring the CLEAR_TRIP drain's own choice of WARN/INFO by
// outcome), never silently downgraded to INFO/DEBUG.
#include <stdbool.h>

#include "test_common.h"

static char *read_file_any(const char *const *candidates, size_t count)
{
    return test_read_source_anchored(__FILE__, candidates[0], candidates, count);
}

// Extracts the body of the `if (newly_tripped) {` block inside
// safety_core_task() -- from that exact condition to its closing brace at
// the same indentation (8 spaces, matching this file's existing style, then
// a lone "}" line). Bounded to the file's own safety_core_task() function so
// a same-named block elsewhere could never be matched instead.
static const char *find_newly_tripped_block(const char *text, size_t *out_len)
{
    const char *task_sig = strstr(text, "static void safety_core_task(");
    if (!task_sig) {
        return NULL;
    }
    const char *cond = strstr(task_sig, "if (newly_tripped) {");
    if (!cond) {
        return NULL;
    }
    const char *open = strchr(cond, '{');
    if (!open) {
        return NULL;
    }
    // This codebase's blocks at this nesting close with "\n        }" (8
    // spaces then the brace) -- same convention the enclosing function
    // itself closes with one level out. Take the FIRST such line after the
    // opening brace.
    const char *close = strstr(open, "\n        }");
    if (!close) {
        return NULL;
    }
    *out_len = (size_t)(close - open);
    return open;
}

static void run_section1_source_scan(void)
{
    TEST_SECTION("safety_core_task()'s newly_tripped block logs the original trip event via "
                 "log_task_log() (source-text scan -- safety_core.c is not host-compilable, "
                 "same precedent as test_safety_core_stack_budget.c / "
                 "test_safety_core_s8_wiring.c)");

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
    const char *body = find_newly_tripped_block(text, &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find the `if (newly_tripped) { ... }` block in "
                           "safety_core_task() -- update this test if it was renamed or "
                           "restructured");
        free(text);
        return;
    }

    char *fn = (char *)malloc(body_len + 1);
    TEST_CHECK(fn != NULL, "malloc for the extracted block succeeded");
    if (!fn) {
        free(text);
        return;
    }
    memcpy(fn, body, body_len);
    fn[body_len] = '\0';

    // Matched on the literal CALL form, not the bare "log_task_log(" token --
    // this file's own surrounding comments are free to mention log_task_log
    // in prose (e.g. "the log_task call below"), and a substring match on
    // just the token would then find the comment instead of the real call
    // and pass vacuously. Tying the match to "log_task_log(LOG_LEVEL_ERROR,"
    // is exactly the real call's opening -- see the LOG_LEVEL_ERROR pin in
    // section 2 below for why that argument is itself asserted separately.
    const char *log_call = strstr(fn, "log_task_log(LOG_LEVEL_ERROR,");
    TEST_CHECK(log_call != NULL,
               "the newly_tripped block calls log_task_log(LOG_LEVEL_ERROR, ...) -- this is the "
               "literal call whose ABSENCE was the shipped gap (the original trip event was "
               "silent; only the later CLEAR_TRIP was ever logged). Deleting/removing this call "
               "makes this check fail.");

    TEST_CHECK(log_call != NULL && strstr(log_call, "\"trip\"") != NULL &&
                   strstr(log_call, "\"trip\"") < log_call + 64,
               "the log call tags the entry \"trip\" (matching \"clear_trip\"'s own tag "
               "convention on the CLEAR_TRIP drain a few lines below) so the two are "
               "distinguishable in the log stream.");

    // Ordering: the log call must appear strictly AFTER the K4-command latch
    // (s_trip_command_owed = true;) and the trip-reason latch
    // (boot_reason_latch_trip(...)) -- never positioned to precede or gate
    // the actual trip action. Both must be present too, or this ordering
    // check is vacuous.
    const char *owed = strstr(fn, "s_trip_command_owed = true;");
    const char *latch = strstr(fn, "boot_reason_latch_trip(");
    TEST_CHECK(owed != NULL, "s_trip_command_owed is still latched in this block (sanity check "
                              "for the ordering assertion below)");
    TEST_CHECK(latch != NULL, "boot_reason_latch_trip() is still called in this block (sanity "
                               "check for the ordering assertion below)");
    if (owed && latch && log_call) {
        TEST_CHECK(log_call > owed && log_call > latch,
                   "log_task_log() for the trip event runs AFTER the K4-command latch and the "
                   "trip-reason latch -- the audit-trail write must never precede or gate the "
                   "trip action itself");
    }

    free(fn);
    free(text);
}

static void run_section2_log_level(void)
{
    TEST_SECTION("the original trip event is logged at ERROR level, not silently downgraded");

    static const char *candidates[] = {
        "../src/tasks/safety_core.c",
        "src/tasks/safety_core.c",
        "firmware/SaftyFW/src/tasks/safety_core.c",
    };
    char *text = read_file_any(candidates, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/safety_core.c");
        return;
    }

    size_t body_len = 0;
    const char *body = find_newly_tripped_block(text, &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find the newly_tripped block");
        free(text);
        return;
    }

    char *fn = (char *)malloc(body_len + 1);
    TEST_CHECK(fn != NULL, "malloc succeeded");
    if (!fn) {
        free(text);
        return;
    }
    memcpy(fn, body, body_len);
    fn[body_len] = '\0';

    const char *log_call = strstr(fn, "log_task_log(LOG_LEVEL_ERROR,");
    TEST_CHECK(log_call != NULL,
               "log_task_log() is called with LOG_LEVEL_ERROR as its first argument -- a trip "
               "is unconditionally an error-level event, not INFO/DEBUG/WARN");

    free(fn);
    free(text);
}

void run_test_safety_core_trip_logging(void)
{
    run_section1_source_scan();
    run_section2_log_level();
}
