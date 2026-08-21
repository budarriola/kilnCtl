// test_boot_checkin_coverage.c -- guards against exactly the class of bug
// that caused the 2026-08-21 reboot loop: update_task registered a
// WATCHDOG_CHECKIN_UPDATE_TASK bit in watchdog_task.h but src/main.c never
// called update_task_start(), so s_checkin_mask could never equal
// WATCHDOG_CHECKIN_ALL_MASK and the 1 s hardware watchdog fired forever.
//
// That bug was a start function with no caller -- a link-time-legal, boot-
// time-silent gap between "this task's bit is part of the mask" and "this
// task's start function is actually invoked from main()". Nothing in the
// type system catches it: xTaskCreate doesn't know about
// WATCHDOG_CHECKIN_ALL_MASK, and the watchdog gate doesn't know about
// main.c's call list. The only mechanical check left is textual: for every
// WATCHDOG_CHECKIN_* id in watchdog_task.h, does main.c's real (non-comment)
// source text contain a call to that task's <name>_start(...)?
//
// This is deliberately a source-text scan, not a compiled/linked check --
// host-testing src/main.c itself would require stubbing all of pico-sdk/
// FreeRTOS, which is exactly the effort TODO.md Phase 4 says these host
// tests avoid. A comment-stripped substring search is cheap, has no false
// negatives for this codebase's own "(void)xxx_start();" call convention
// (see main.c step 7), and is exactly precise enough to have caught the
// real bug: main.c's comment block about update_task (added when the bug
// was fixed) *mentions* "update_task_start()" in prose, which is why
// comments are stripped before searching -- otherwise the post-mortem
// comment describing the bug would itself satisfy a naive search and the
// guard would never have been able to fail.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "test_common.h"

// Reads a whole file into a NUL-terminated malloc'd buffer, or NULL if it
// can't be found. Tries a few relative-path candidates because
// build_host_tests.ps1 and a plain `cl` invocation from an IDE can each
// have a different working directory; this is a host test convenience, not
// something that ships.
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

// Strips // line comments and /* */ block comments in place, leaving
// everything else (including string/char literals -- main.c and
// watchdog_task.h have none in the relevant lines, so this doesn't need to
// be literal-aware to be correct here). Newlines inside comments are kept
// as newlines so line numbers in any future error reporting would still
// line up; that isn't used today but costs nothing.
static void strip_comments(char *s)
{
    char *out = s;
    while (*s) {
        if (s[0] == '/' && s[1] == '/') {
            while (*s && *s != '\n') {
                s++;
            }
        } else if (s[0] == '/' && s[1] == '*') {
            s += 2;
            while (*s && !(s[0] == '*' && s[1] == '/')) {
                if (*s == '\n') {
                    *out++ = '\n';
                }
                s++;
            }
            if (*s) {
                s += 2;
            }
        } else {
            *out++ = *s++;
        }
    }
    *out = '\0';
}

typedef struct {
    const char *enum_name;  // e.g. "WATCHDOG_CHECKIN_RELAY_OWNER"
    const char *start_call; // e.g. "relay_owner_start"
} checkin_binding_t;

// Mirrors watchdog_task.h's watchdog_checkin_id_t enum and this project's
// <module>_start() naming convention. Deliberately hand-maintained rather
// than derived from the enum text: the whole point of this test is to
// catch a *missing* call, so the expected-call list must not be generated
// from anything that itself could silently drop an entry. Adding a new
// WATCHDOG_CHECKIN_* id means adding a line here too -- see the assertion
// below that the two lists are the same length as a tripwire for that.
static const checkin_binding_t k_bindings[] = {
    {"WATCHDOG_CHECKIN_RELAY_OWNER", "relay_owner_start"},
    {"WATCHDOG_CHECKIN_SAFETY_CORE", "safety_core_start"},
    {"WATCHDOG_CHECKIN_DISCRETE_TASK", "discrete_task_start"},
    {"WATCHDOG_CHECKIN_THERMO_TASK", "thermo_task_start"},
    {"WATCHDOG_CHECKIN_CURRENT_TASK", "current_task_start"},
    {"WATCHDOG_CHECKIN_LINK_TASK", "link_task_start"},
    {"WATCHDOG_CHECKIN_LOG_TASK", "log_task_start"},
    {"WATCHDOG_CHECKIN_UPDATE_TASK", "update_task_start"},
};
#define K_BINDINGS_COUNT (sizeof(k_bindings) / sizeof(k_bindings[0]))

// Counts every WATCHDOG_CHECKIN_* identifier in watchdog_task.h's enum body
// (between "typedef enum {" and the matching "}"), excluding the sentinel
// WATCHDOG_CHECKIN_COUNT. This is what makes the test fail loudly if a new
// bit is added to the mask without a matching line added to k_bindings
// above, rather than the new bit silently passing an incomplete check.
static int count_checkin_ids_in_header(const char *text)
{
    const char *enum_start = strstr(text, "typedef enum {");
    if (!enum_start) {
        return -1;
    }
    const char *body_end = strchr(enum_start, '}');
    if (!body_end) {
        return -1;
    }
    int count = 0;
    const char *p = enum_start;
    while ((p = strstr(p, "WATCHDOG_CHECKIN_")) != NULL && p < body_end) {
        if (strncmp(p, "WATCHDOG_CHECKIN_COUNT", strlen("WATCHDOG_CHECKIN_COUNT")) != 0) {
            count++;
        }
        p += strlen("WATCHDOG_CHECKIN_");
    }
    return count;
}

void run_test_boot_checkin_coverage(void)
{
    TEST_SECTION("boot checkin coverage (every WATCHDOG_CHECKIN_* bit has a caller in main.c)");

    static const char *header_candidates[] = {
        "../src/tasks/watchdog_task.h",
        "src/tasks/watchdog_task.h",
        "firmware/SaftyFW/src/tasks/watchdog_task.h",
    };
    static const char *main_candidates[] = {
        "../src/main.c",
        "src/main.c",
        "firmware/SaftyFW/src/main.c",
    };

    char *header_text = read_file_any(header_candidates, 3);
    char *main_text = read_file_any(main_candidates, 3);

    if (!header_text || !main_text) {
        TEST_CHECK(false, "could not locate watchdog_task.h and/or main.c from the host test's "
                           "working directory -- update the candidate paths in "
                           "test_boot_checkin_coverage.c if the build layout moved");
        free(header_text);
        free(main_text);
        return;
    }

    // Comments stripped from main.c before searching -- see this file's
    // header comment for why: the very comment documenting the 2026-08-21
    // bug fix mentions "update_task_start()" in prose, and a search that
    // matched comments could never fail again on a re-introduced regression
    // as long as that historical comment stays in the file.
    strip_comments(main_text);

    int header_id_count = count_checkin_ids_in_header(header_text);
    TEST_CHECK(header_id_count == (int)K_BINDINGS_COUNT,
               "watchdog_task.h's enum has a different number of WATCHDOG_CHECKIN_* ids than "
               "this test's k_bindings[] table -- a checkin id was added (or removed) without "
               "updating test_boot_checkin_coverage.c's k_bindings[] to match, which would "
               "silently exempt the new bit from this guard");

    for (size_t i = 0; i < K_BINDINGS_COUNT; i++) {
        int in_header = strstr(header_text, k_bindings[i].enum_name) != NULL;
        TEST_CHECK(in_header, k_bindings[i].enum_name);

        int called = strstr(main_text, k_bindings[i].start_call) != NULL;
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "%s is registered in WATCHDOG_CHECKIN_ALL_MASK but main.c never calls %s() -- "
                 "this bit can never be set, s_checkin_mask can never equal "
                 "WATCHDOG_CHECKIN_ALL_MASK, and the hardware watchdog will fire forever "
                 "(the exact 2026-08-21 bug)",
                 k_bindings[i].enum_name, k_bindings[i].start_call);
        TEST_CHECK(called, msg);
    }

    free(header_text);
    free(main_text);
}
