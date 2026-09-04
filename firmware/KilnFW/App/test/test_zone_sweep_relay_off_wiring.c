// test_zone_sweep_relay_off_wiring.c -- proves zone_sweep_force_relays_off()
// (zones_current_sweep_engine.c) actually CHECKS kiln_io_owner_command_all_
// relays_off()'s return value and logs a named failure, rather than the
// fire-and-forget shape it shipped with.
//
// THE PROBLEM THIS FILE SOLVES: this is the exact sibling the danger_mode.c
// seed bug (commit 2bcdc2d) named but did not have scope to fix --
// zones_current_sweep_engine.c:386 discarded kiln_io_owner_command_all_
// relays_off()'s result outright (not even cast to (void) with a comment --
// simply not captured). An owner-queue timeout (ESP_ERR_TIMEOUT, the same
// real, reachable post_and_wait() failure danger_mode.c's fix names) would
// leave a relay closed with nothing in the log to say so, while every one of
// this function's callers -- the normal per-zone measurement end, and the
// sweep's own final choke point -- moves on as though the coil had opened.
//
// zones_current_sweep_engine.c is not host-compilable on its own in this
// suite's current source list (it pulls in the full sweep engine and its
// FreeRTOS task plumbing) -- source-text scan is the established precedent
// (test_safety_core_s8_wiring.c, test_display_power_wiring.c): extract the
// exact function body so a match can only land inside it, not a stray
// comment or a different function, and FAIL CLOSED if the file or the
// function cannot be located at all.
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "test_common.h"

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

// Same convention test_display_power_wiring.c uses: this codebase's own
// functions close at column 0, so the first "\n}" after the opening brace
// is the real end of the function.
static const char *find_function_body(const char *text, const char *sig, size_t *out_len)
{
    const char *s = strstr(text, sig);
    if (!s) {
        return NULL;
    }
    const char *open = strchr(s, '{');
    if (!open) {
        return NULL;
    }
    const char *close = strstr(open, "\n}");
    if (!close) {
        return NULL;
    }
    *out_len = (size_t)(close - open);
    return open;
}

static char *dup_range(const char *start, size_t len)
{
    char *buf = (char *)malloc(len + 1);
    if (!buf) {
        return NULL;
    }
    memcpy(buf, start, len);
    buf[len] = '\0';
    return buf;
}

static const char *SWEEP_ENGINE_C_CANDIDATES[] = {
    "../drivers/zones_current_sweep_engine.c",
    "App/drivers/zones_current_sweep_engine.c",
    "firmware/KilnFW/App/drivers/zones_current_sweep_engine.c",
};

static void run_section1_result_checked_and_logged(void)
{
    TEST_SECTION("zone_sweep_force_relays_off() checks kiln_io_owner_command_all_relays_off()'s "
                 "result and logs a named failure -- source-text scan, zones_current_sweep_"
                 "engine.c is not in this suite's host-compiled source list");

    char *text = read_file_any(SWEEP_ENGINE_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/zones_current_sweep_engine.c from the host "
                           "test's working directory -- update the candidate paths in this test "
                           "if the build layout moved");
        return;
    }

    size_t body_len = 0;
    const char *body = find_function_body(text, "void zone_sweep_force_relays_off(void)", &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find zone_sweep_force_relays_off()'s function body in "
                           "zones_current_sweep_engine.c -- update this test if it was renamed/"
                           "restructured. If this function is simply gone, find whatever now "
                           "performs the sweep's relay-off choke point and point this test at it.");
        free(text);
        return;
    }
    char *fn = dup_range(body, body_len);
    TEST_CHECK(fn != NULL, "malloc for the extracted function body succeeded");
    if (!fn) {
        free(text);
        return;
    }

    const char *call = strstr(fn, "kiln_io_owner_command_all_relays_off()");
    TEST_CHECK(call != NULL,
               "zone_sweep_force_relays_off() must still call kiln_io_owner_command_all_relays_"
               "off() -- if this fails, the sweep's relay-off choke point was rewired to "
               "something else entirely.");

    // The bug this test exists to catch: the call's result used to not even
    // be captured. Require it assigned to a named esp_err_t.
    const char *assign = strstr(fn, "esp_err_t err = kiln_io_owner_command_all_relays_off()");
    TEST_CHECK(assign != NULL,
               "the call's return value must be captured into a named esp_err_t -- if this "
               "fails, the result went back to being discarded outright (the original shape of "
               "this bug: not even a (void) cast, just an unused return value).");

    // And the captured result must actually be inspected and logged on
    // failure -- capturing it into a variable nobody reads would be no
    // improvement at all.
    const char *check = strstr(fn, "if (err != ESP_OK)");
    TEST_CHECK(check != NULL,
               "the captured result must be checked (if (err != ESP_OK)) -- if this fails, err "
               "is assigned but never inspected, which is the same silent-drop bug wearing a "
               "variable name.");
    TEST_CHECK(assign != NULL && check != NULL && assign < check,
               "the check must come after the assignment in source order -- otherwise err is "
               "being tested before this call ever ran.");

    const char *log_call = strstr(fn, "ESP_LOGE(TAG,");
    TEST_CHECK(log_call != NULL && check != NULL && check < log_call,
               "a failure must be logged with ESP_LOGE, after the check -- matching danger_mode."
               "c's fix for the identical seed bug (commit 2bcdc2d): naming the failure instead "
               "of staying silent or asserting an outcome that was never confirmed.");
    TEST_CHECK(log_call != NULL && strstr(log_call, "esp_err_to_name(err)") != NULL,
               "the ESP_LOGE call must name the actual esp_err_t (esp_err_to_name(err)), not a "
               "generic message that would look identical for every possible failure.");

    free(fn);
    free(text);
}

void run_test_zone_sweep_relay_off_wiring(void)
{
    run_section1_result_checked_and_logged();
}
