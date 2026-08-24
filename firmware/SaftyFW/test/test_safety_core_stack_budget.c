// test_safety_core_stack_budget.c -- guards against SAFETY_CORE_STACK_WORDS
// (src/tasks/safety_core.c) ever shrinking back toward either value that
// has already caused a live reboot on the bench.
//
// 2026-08-23, the CLEAR_TRIP-reboots-the-Pico investigation, round 1:
// SAFETY_CORE_STACK_WORDS was configMINIMAL_STACK_SIZE (256 words,
// unmultiplied) -- not enough headroom for safety_guards.c's trip() to
// vsnprintf() two %.1f floats into its detail[96] buffer on top of
// safety_core_task()'s own locals. Bumped to *4; fixed the PERIODIC trip
// path (proven on hardware: S5 latches and stays latched).
//
// Round 2: *4 was not enough for the CLEAR_TRIP path specifically -- a
// DEEPER re-entry into the identical trip()/vsnprintf() call (one extra
// stack frame via safety_guards_try_clear()) plus an entirely separate
// log_task_log() call the periodic path never even reaches (its own
// ~100-byte log_entry_t and second snprintf). See safety_core.c's own
// comment on the #define for the full reasoning. Bumped to *6, matching
// link_task.c's own already-established multiplier for this exact class of
// problem.
//
// vApplicationStackOverflowHook() (main.c) halts with interrupts disabled
// rather than resetting, so both failures were silent: the unfed 1s
// hardware watchdog rebooted the board about a second later, indistinguish-
// able from any other reboot from the ESP's side. The exact right number
// has still not been measured on hardware (a per-checkin
// uxTaskGetStackHighWaterMark() census was tried and removed the same day
// for distorting watchdog checkin timing, watchdog_task.c's own comment).
// This test is the floor that stands in for that measurement: it does not
// prove *6 is sufficient, only that nobody can silently walk the multiplier
// back down toward *1 or *4 (both already proven insufficient on real
// hardware) without this test failing first.
//
// Same discipline as test_watchdog_budget_coverage.c: a source-text scan,
// not a compiled/linked check, because safety_core.c needs real pico-sdk/
// FreeRTOS headers and is not otherwise host-testable.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "test_common.h"

// Mirrors test_watchdog_budget_coverage.c's read_file_any() exactly -- same
// "different build layouts have different working directories" reasoning.
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

// Finds "#define SAFETY_CORE_STACK_WORDS   (configMINIMAL_STACK_SIZE * N)"
// (whitespace-tolerant, parens optional) and returns N, or -1 if the
// #define or the "configMINIMAL_STACK_SIZE * <digits>" shape inside it is
// not found. Deliberately narrow -- this only ever has to parse this
// codebase's own one #define, not general C, and a value that changes SHAPE
// entirely (e.g. back to a bare "configMINIMAL_STACK_SIZE" with no
// multiplier at all, exactly the historical bug) is supposed to make this
// return -1 and fail loudly, not silently pass.
static long find_stack_multiplier(const char *text)
{
    const char *define = strstr(text, "#define SAFETY_CORE_STACK_WORDS");
    if (!define) {
        return -1;
    }
    const char *mul = strstr(define, "configMINIMAL_STACK_SIZE");
    if (!mul) {
        return -1;
    }
    // Must be on the SAME line as the #define -- a match further down the
    // file (e.g. a comment mentioning the same identifier) must not count.
    const char *line_end = strchr(define, '\n');
    if (line_end && mul > line_end) {
        return -1;
    }
    const char *p = mul + strlen("configMINIMAL_STACK_SIZE");
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (*p != '*') {
        // Bare "configMINIMAL_STACK_SIZE" with no multiplier at all -- the
        // exact historical bug (1x, unmultiplied). Report as multiplier 1
        // rather than "not found", so the check below fails with an honest
        // "1 < floor" rather than a confusing "could not parse".
        return 1;
    }
    p++;
    while (*p == ' ' || *p == '\t') {
        p++;
    }
    if (!isdigit((unsigned char)*p)) {
        return -1;
    }
    return strtol(p, NULL, 10);
}

void run_test_safety_core_stack_budget(void)
{
    TEST_SECTION("safety_core stack budget (SAFETY_CORE_STACK_WORDS must stay comfortably "
                  "above the value that caused the 2026-08-23 reboot loop)");

    static const char *safety_core_candidates[] = {
        "../src/tasks/safety_core.c",
        "src/tasks/safety_core.c",
        "firmware/SaftyFW/src/tasks/safety_core.c",
    };

    char *text = read_file_any(safety_core_candidates, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/safety_core.c from the host test's "
                           "working directory -- update the candidate paths in "
                           "test_safety_core_stack_budget.c if the build layout moved");
        return;
    }

    long multiplier = find_stack_multiplier(text);
    TEST_CHECK(multiplier >= 0,
               "could not find SAFETY_CORE_STACK_WORDS's #define (or its "
               "\"configMINIMAL_STACK_SIZE * N\" shape) in src/tasks/safety_core.c -- update "
               "this test if the constant was renamed or restructured");

    if (multiplier >= 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "SAFETY_CORE_STACK_WORDS is configMINIMAL_STACK_SIZE * %ld -- must be at "
                 "least * 5. * 1 (unmultiplied) overflowed on the periodic trip path "
                 "(safety_guards.c's trip() vsnprintf-ing two %%.1f floats) and put the board "
                 "into an indefinite reboot loop; * 4 fixed that path but still overflowed on "
                 "the CLEAR_TRIP path specifically (one extra stack frame re-entering the same "
                 "vsnprintf via safety_guards_try_clear(), plus a log_task_log() call the "
                 "periodic path never reaches). vApplicationStackOverflowHook() (main.c) halts "
                 "silently rather than reporting, so neither failure could be caught by any "
                 "runtime assertion on this hardware -- this source-text floor is what stands "
                 "in its place",
                 multiplier);
        TEST_CHECK(multiplier >= 5, msg);
    }

    free(text);
}
