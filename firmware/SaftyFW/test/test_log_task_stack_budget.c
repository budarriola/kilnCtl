// test_log_task_stack_budget.c -- guards against LOG_TASK_STACK_WORDS
// (src/tasks/log_task.c) ever shrinking back toward the value that just
// caused a live reboot on the bench, confirmed by the reset-surviving
// vApplicationStackOverflowHook() latch (main.c, watchdog_hw->scratch[5],
// magic byte 0xE3) naming log_task by its first two name bytes ("lo").
//
// 2026-08-23, the CLEAR_TRIP-reboots-the-Pico investigation's final finding
// (after safety_core_task's own stack was ruled out by the same hook
// latch): LOG_TASK_STACK_WORDS was configMINIMAL_STACK_SIZE (256 words,
// unmultiplied) -- not enough for log_task_fn()'s own log_entry_t/payload
// locals (~205 bytes) plus link_task_send_broadcast_to()'s
// raw[KILNLINK_FRAME_RAW_MAX]/stuffed[KILNLINK_FRAME_STUFFED_MAX] pair
// (263 + 528 = 791 bytes) reachable through link_task_send_log() -- ~1032
// bytes of known, exact-by-construction locals alone, already over the old
// 1024-byte budget before counting a single byte of call-frame overhead.
// See log_task.c's own comment on the #define for the full arithmetic.
// Bumped to *2 (2048 bytes), a stated ~1.7-1.8x margin over the ~1130-1180
// byte estimated peak -- deliberately smaller than link_task.c's own *6,
// which covers a genuinely deeper nested-buffer chain, not copied here
// without re-deriving why.
//
// Same discipline as test_safety_core_stack_budget.c: a source-text scan,
// not a compiled/linked check, because log_task.c needs real pico-sdk/
// FreeRTOS headers and is not otherwise host-testable.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "test_common.h"

// Mirrors test_safety_core_stack_budget.c's read_file_any() exactly -- same
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

// Finds "#define LOG_TASK_STACK_WORDS   (configMINIMAL_STACK_SIZE * N)"
// (whitespace-tolerant, parens optional) and returns N, or -1 if the
// #define or the "configMINIMAL_STACK_SIZE * <digits>" shape inside it is
// not found -- same parser shape as
// test_safety_core_stack_budget.c's find_stack_multiplier(), duplicated
// rather than shared (each test file is self-contained in this codebase's
// established pattern, and the two constants being checked are unrelated
// beyond sharing a naming convention).
static long find_stack_multiplier(const char *text)
{
    const char *define = strstr(text, "#define LOG_TASK_STACK_WORDS");
    if (!define) {
        return -1;
    }
    const char *mul = strstr(define, "configMINIMAL_STACK_SIZE");
    if (!mul) {
        return -1;
    }
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

void run_test_log_task_stack_budget(void)
{
    TEST_SECTION("log_task stack budget (LOG_TASK_STACK_WORDS must stay comfortably above the "
                  "value that caused the 2026-08-23 reboot loop)");

    static const char *log_task_candidates[] = {
        "../src/tasks/log_task.c",
        "src/tasks/log_task.c",
        "firmware/SaftyFW/src/tasks/log_task.c",
    };

    char *text = read_file_any(log_task_candidates, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate src/tasks/log_task.c from the host test's working "
                           "directory -- update the candidate paths in "
                           "test_log_task_stack_budget.c if the build layout moved");
        return;
    }

    long multiplier = find_stack_multiplier(text);
    TEST_CHECK(multiplier >= 0,
               "could not find LOG_TASK_STACK_WORDS's #define (or its "
               "\"configMINIMAL_STACK_SIZE * N\" shape) in src/tasks/log_task.c -- update this "
               "test if the constant was renamed or restructured");

    if (multiplier >= 0) {
        char msg[512];
        snprintf(msg, sizeof(msg),
                 "LOG_TASK_STACK_WORDS is configMINIMAL_STACK_SIZE * %ld -- must be at least "
                 "* 2. * 1 (unmultiplied) is the exact value that overflowed on the bench "
                 "(log_task_fn()'s own locals plus link_task_send_broadcast_to()'s "
                 "raw[263]/stuffed[528] buffer pair, ~1032 known bytes against a 1024-byte "
                 "budget) and put the board into a watchdog reset loop, confirmed by name via "
                 "vApplicationStackOverflowHook()'s reset-surviving latch (main.c) rather than "
                 "inferred -- this source-text floor is what stands in for a runtime assertion "
                 "this hardware cannot make on its own",
                 multiplier);
        TEST_CHECK(multiplier >= 2, msg);
    }

    free(text);
}
