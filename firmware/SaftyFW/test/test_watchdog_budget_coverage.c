// test_watchdog_budget_coverage.c -- guards against any core-1 task's
// bounded wait being allowed to exceed the 1 s hardware watchdog timeout.
//
// spi_owner.c's SPI_OWNER_LOCK_TIMEOUT_MS used to be 2000 ms -- "bounded",
// but bounded to MORE than the 1 s hardware deadline it lives inside. That is
// not a hang (spi_owner has exactly one caller today, thermo_task, so the
// mutex is never actually contended), but it is a live defect: thermo_task
// calls spi_owner_transfer() synchronously, before its own
// watchdog_task_checkin(WATCHDOG_CHECKIN_THERMO_TASK), on every pass through
// its loop (thermo_task.c). Any future second caller, or any other reason
// the mutex is held across a call, could then block thermo_task -- and with
// it the watchdog checkin the whole hardware watchdog depends on -- for up
// to the old 2000 ms, comfortably more than the 1000 ms
// SAFTYFW_WATCHDOG_TIMEOUT_MS the watchdog is armed with in main.c. A wait
// timeout that is merely "bounded" is not the same property as "bounded
// under the watchdog's own deadline", and nothing else in this codebase
// checked that relationship before this test existed.
//
// Same discipline as test_boot_checkin_coverage.c: a source-text scan, not a
// compiled/linked check, because spi_owner.c needs real pico-sdk SPI
// hardware (hardware/spi.h, hardware/gpio.h) and is not otherwise
// host-testable. Pulls both constants out of their defining files by name so
// this test breaks (rather than silently drifting stale) if either value's
// #define is edited without updating the other.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>

#include "test_common.h"

// Mirrors test_boot_checkin_coverage.c's read_file_any() exactly -- same
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

// Finds "#define <name> <digits>" (any whitespace between the name and the
// digits, an optional trailing 'u'/'U' suffix) and returns the parsed value,
// or -1 if the name/pattern is not found. Deliberately simple -- this only
// ever has to parse this codebase's own two single-line, unsuffixed-except-
// for-'u' #define constants, not general C.
static long find_define_value(const char *text, const char *name)
{
    const char *p = text;
    size_t name_len = strlen(name);
    while ((p = strstr(p, name)) != NULL) {
        // Require a #define ... immediately before this occurrence on the
        // same line, and a non-identifier character right after the name
        // (so e.g. searching "FOO" does not match "FOO_BAR").
        const char *after_name = p + name_len;
        if (isalnum((unsigned char)*after_name) || *after_name == '_') {
            p = after_name;
            continue;
        }

        // Walk backwards on this line to confirm "#define" precedes it.
        const char *line_start = p;
        while (line_start > text && line_start[-1] != '\n') {
            line_start--;
        }
        if (strstr(line_start, "#define") == NULL || strstr(line_start, "#define") >= p) {
            p = after_name;
            continue;
        }

        const char *q = after_name;
        while (*q == ' ' || *q == '\t') {
            q++;
        }
        if (!isdigit((unsigned char)*q)) {
            p = after_name;
            continue;
        }
        return strtol(q, NULL, 10);
    }
    return -1;
}

void run_test_watchdog_budget_coverage(void)
{
    TEST_SECTION("watchdog budget coverage (no core-1 bounded wait may exceed the 1 s hardware watchdog timeout)");

    static const char *spi_owner_candidates[] = {
        "../src/spi_owner.c",
        "src/spi_owner.c",
        "firmware/SaftyFW/src/spi_owner.c",
    };
    static const char *main_candidates[] = {
        "../src/main.c",
        "src/main.c",
        "firmware/SaftyFW/src/main.c",
    };

    char *spi_owner_text = read_file_any(spi_owner_candidates, 3);
    char *main_text = read_file_any(main_candidates, 3);

    if (!spi_owner_text || !main_text) {
        TEST_CHECK(false, "could not locate src/spi_owner.c and/or src/main.c from the host "
                           "test's working directory -- update the candidate paths in "
                           "test_watchdog_budget_coverage.c if the build layout moved");
        free(spi_owner_text);
        free(main_text);
        return;
    }

    long spi_lock_timeout_ms = find_define_value(spi_owner_text, "SPI_OWNER_LOCK_TIMEOUT_MS");
    long watchdog_timeout_ms = find_define_value(main_text, "SAFTYFW_WATCHDOG_TIMEOUT_MS");

    TEST_CHECK(spi_lock_timeout_ms >= 0,
               "could not find SPI_OWNER_LOCK_TIMEOUT_MS's #define in src/spi_owner.c -- "
               "update this test if the constant was renamed");
    TEST_CHECK(watchdog_timeout_ms >= 0,
               "could not find SAFTYFW_WATCHDOG_TIMEOUT_MS's #define in src/main.c -- "
               "update this test if the constant was renamed");

    if (spi_lock_timeout_ms >= 0 && watchdog_timeout_ms >= 0) {
        char msg[320];
        snprintf(msg, sizeof(msg),
                 "SPI_OWNER_LOCK_TIMEOUT_MS (%ld ms) must be strictly less than "
                 "SAFTYFW_WATCHDOG_TIMEOUT_MS (%ld ms) -- thermo_task calls "
                 "spi_owner_transfer() synchronously, before its own watchdog checkin, so a "
                 "wait bounded at or above the hardware watchdog's own deadline can starve "
                 "that checkin all by itself, watchdog reason TIMER, exactly the class of bug "
                 "this test exists to catch",
                 spi_lock_timeout_ms, watchdog_timeout_ms);
        TEST_CHECK(spi_lock_timeout_ms < watchdog_timeout_ms, msg);
    }

    free(spi_owner_text);
    free(main_text);
}
