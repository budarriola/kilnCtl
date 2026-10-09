// Host tests for App/drivers/http/dashboard_http.h's dashboard_safety_ready() --
// the pure predicate factored out of dashboard_get_status() and
// dashboard_http_get_hw_ready() (dashboard_http.c) after an owner-reported
// bench bug: "Safty link says linked even though the uart between them is
// disconnected." With the UART physically unplugged, the ESP web UI and the
// LCD both kept reporting the safety link as up.
//
// Root cause: both call sites computed "safety_ready" as
// `s_dash.safety != NULL` -- true from the moment the SafetyLinkClass driver
// object is constructed and forever after, regardless of whether the Pico
// has ever answered or has since gone silent. safety_link_get_status()'s
// link_up field is the real answer (safety_link.c's safety_link_up_locked(),
// gated at SAFETY_LINK_STALE_MS = 1500 ms of silence -- see
// test_safety_link.c for that half of the contract), but nothing read it for
// this bit.
//
// dashboard_safety_ready(have_safety_link, status_err, link_up) is the fixed
// logic, pulled out to a pure function so this "must equal link_up, never
// merely non-NULL" contract is host-testable without standing up the whole
// httpd/kiln_io/MAX31856/WiFi machinery dashboard_http.c otherwise needs.
//
// 2026-09-19 addition: dashboard_get_status()'s OWN safety_relay_known
// assignment (dashboard_http.c, the K4 field) had this exact bug independently
// -- it read `out->safety_relay_known = true` whenever safety_link_get_status()
// returned ESP_OK, which it does for ANY initialized link object regardless of
// sl.link_up, so a Pico that had never answered (or had answered once and then
// gone silent) still reported a confident "known" relay state instead of
// "unknown". dashboard_http.c itself is not host-compilable from this
// executable (file-scope #include "lvgl_port.h" pulls in GCC-only attributes
// MSVC's host toolchain rejects -- same reason test_dashboard_status_http.c
// has to #include the narrower dashboard_status_http.c instead), so the
// section below follows test_display_power_wiring.c's established fallback:
// a source-text scan of the real .c file, function body extracted by name so
// a match can only land inside dashboard_get_status() itself, comments
// stripped first so an explanatory comment reusing these tokens cannot
// satisfy the check.
#include <stdlib.h>
#include <string.h>

#include "test_common.h"
#include "../drivers/http/dashboard_http.h"

static void test_no_driver_is_never_ready(void)
{
    TEST_SECTION("dashboard_safety_ready -- no driver object at all reads not-ready");

    TEST_CHECK(dashboard_safety_ready(false, ESP_OK, true) == false,
               "have_safety_link=false must read not-ready regardless of link_up "
               "(mirrors the pre-safety_link_start() / never-initialized case)");
}

static void test_driver_exists_but_link_down_reads_not_ready(void)
{
    TEST_SECTION("dashboard_safety_ready -- THE BUG: driver constructed, UART unplugged");

    // This is the exact bench scenario the owner reported: the SafetyLinkClass
    // object exists (safety_link_start() ran at boot), a status fetch still
    // succeeds (it only fails on a NULL pointer / uninitialized state -- see
    // safety_link_get_status()'s own contract), but link_up is false because
    // no frame has arrived within SAFETY_LINK_STALE_MS (safety_link_up_locked()
    // already gates this correctly on its own). The old code never looked at
    // link_up here at all -- it would have reported ready=true for this exact
    // input. This is the check that catches that regression coming back.
    TEST_CHECK(dashboard_safety_ready(true, ESP_OK, false) == false,
               "driver present + link_up=false (UART unplugged/stale) MUST read not-ready -- "
               "this is the owner-reported false positive");
}

static void test_driver_exists_and_link_up_reads_ready(void)
{
    TEST_SECTION("dashboard_safety_ready -- driver constructed, link genuinely alive");

    TEST_CHECK(dashboard_safety_ready(true, ESP_OK, true) == true,
               "driver present + link_up=true reads ready -- the only true-positive case");
}

static void test_status_fetch_error_reads_not_ready(void)
{
    TEST_SECTION("dashboard_safety_ready -- a failed status fetch is never ready, even if "
                  "the caller (wrongly) still had link_up=true lying around");

    // safety_link_get_status() itself fails only when the driver was never
    // initialized (ESP_ERR_INVALID_STATE) or the lock couldn't be taken
    // (ESP_FAIL) -- in either case there is no fresh `sl` to trust, so this
    // must not read ready no matter what stale/leftover link_up value a
    // caller happened to pass through.
    TEST_CHECK(dashboard_safety_ready(true, ESP_ERR_INVALID_STATE, true) == false,
               "status fetch error (ESP_ERR_INVALID_STATE) reads not-ready even with link_up=true");
    TEST_CHECK(dashboard_safety_ready(true, ESP_FAIL, true) == false,
               "status fetch error (ESP_FAIL) reads not-ready even with link_up=true");
}

// ---------------------------------------------------------------------------
// Source-text scan of dashboard_http.c's dashboard_get_status() -- see the
// 2026-09-19 header comment above for why this file can't just link the real
// dashboard_http.c and call it. Same helpers/approach as test_display_power_
// wiring.c's find_function_body()/strip_c_comments(), duplicated here (both
// are `static` in that file, so there is no header seam to share them from)
// rather than introducing a new shared test-utility header for one caller.
// ---------------------------------------------------------------------------

static const char *find_function_body(const char *text, const char *sig, size_t *out_len)
{
    const char *s = text;
    for (;;) {
        s = strstr(s, sig);
        if (!s) {
            return NULL;
        }
        const char *p = s + strlen(sig);
        int depth = 1; // sig already consumed the opening '('
        while (*p && depth > 0) {
            if (*p == '(') depth++;
            else if (*p == ')') depth--;
            p++;
        }
        if (depth != 0) {
            return NULL; // unbalanced parens -- malformed input, fail closed
        }
        while (*p == ' ' || *p == '\n' || *p == '\t' || *p == '\r') p++;
        if (*p == ';') {
            s = p + 1; // a prototype, not a definition -- keep looking
            continue;
        }
        if (*p != '{') {
            return NULL;
        }
        const char *open = p;
        const char *close = strstr(open, "\n}");
        if (!close) {
            return NULL;
        }
        *out_len = (size_t)(close - open);
        return open;
    }
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

static char *strip_c_comments(const char *src)
{
    size_t n = strlen(src);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n;) {
        if (src[i] == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) i++;
            i = (i + 1 < n) ? i + 2 : n;
            out[o++] = ' ';
        } else if (src[i] == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') i++;
            out[o++] = ' ';
        } else {
            out[o++] = src[i++];
        }
    }
    out[o] = '\0';
    return out;
}

// Strips ALL whitespace (not just comments) so the strstr needles below
// can't be defeated by reformatting alone -- e.g. clang-format changing
// `out->safety_relay_known = true;` to `out->safety_relay_known=true;` or
// spreading it across a line wrap. C identifiers and operators (=, ;, (, ))
// are never whitespace themselves, so collapsing whitespace to nothing
// cannot accidentally merge two distinct tokens into a needle that wasn't
// really there.
static char *strip_all_ws(const char *src)
{
    size_t n = strlen(src);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (src[i] == ' ' || src[i] == '\t' || src[i] == '\n' || src[i] == '\r') {
            continue;
        }
        out[o++] = src[i];
    }
    out[o] = '\0';
    return out;
}

static const char *DASHBOARD_HTTP_C_CANDIDATES[] = {
    "../drivers/http/dashboard_http.c",
    "App/drivers/http/dashboard_http.c",
    "firmware/KilnFW/App/drivers/http/dashboard_http.c",
};

static void test_safety_relay_known_gated_on_link_up_in_source(void)
{
    TEST_SECTION("dashboard_get_status() -- source-text scan: safety_relay_known must be "
                 "gated on link_up, not merely on safety_link_get_status() returning ESP_OK "
                 "(dashboard_http.c is not host-compilable here -- see this file's own header "
                 "comment for why)");

    char *text = test_read_source_anchored(__FILE__, DASHBOARD_HTTP_C_CANDIDATES[0],
                                            DASHBOARD_HTTP_C_CANDIDATES, 3);
    if (!text) {
        TEST_CHECK(false, "could not locate drivers/http/dashboard_http.c from the host "
                           "test's working directory -- update the candidate paths in this "
                           "test if the build layout moved");
        return;
    }

    char *stripped = strip_c_comments(text);
    free(text);
    TEST_CHECK(stripped != NULL, "comment-stripping dashboard_http.c succeeded");
    if (!stripped) {
        return;
    }

    size_t body_len = 0;
    // find_function_body() walks balanced parens itself starting right after
    // sig's own opening '(' -- sig must end there, not at the closing ')'.
    const char *body = find_function_body(stripped, "void dashboard_get_status(", &body_len);
    if (!body) {
        TEST_CHECK(false, "could not find dashboard_get_status()'s function body in "
                           "dashboard_http.c -- update this test if it was renamed/"
                           "restructured/split into another file");
        free(stripped);
        return;
    }
    char *fn = dup_range(body, body_len);
    free(stripped);
    TEST_CHECK(fn != NULL, "malloc for the extracted function body succeeded");
    if (!fn) {
        return;
    }

    // Whitespace-stripped copy: the needles below only contain identifiers
    // and operators, none of which is itself whitespace, so stripping all
    // whitespace first makes the match immune to spacing/line-wrap choices
    // (e.g. `= true;` vs `=true;` vs a wrapped `=\n    true;`) that a
    // reformat could otherwise introduce to dodge a literal, spacing-exact
    // needle -- see strip_all_ws()'s own comment.
    char *dense = strip_all_ws(fn);
    free(fn);
    TEST_CHECK(dense != NULL, "malloc for the whitespace-stripped function body succeeded");
    if (!dense) {
        return;
    }

    // The old, buggy line was a bare `out->safety_relay_known = true;` with
    // nothing else on the statement -- reject that shape outright...
    TEST_CHECK(strstr(dense, "safety_relay_known=true;") == NULL,
               "safety_relay_known must NOT be unconditionally assigned true -- this is "
               "the exact regression this test exists to catch (a Pico that never "
               "answered, or answered once and went silent, must not read as a known "
               "relay state). If this fails, the old bug is back.");

    // ...and require the fixed assignment to actually be derived from
    // safety_link_up (the local this function already captures from
    // sl.link_up a few lines above) via the same dashboard_safety_ready()
    // predicate safety_ready itself uses, so the two fields can never
    // silently diverge again.
    TEST_CHECK(strstr(dense, "safety_relay_known=dashboard_safety_ready(") != NULL,
               "safety_relay_known must be computed via dashboard_safety_ready(), the same "
               "predicate safety_ready uses, so the two can never drift apart -- if this "
               "fails, the assignment was changed to some other (possibly still-buggy) "
               "expression");
    TEST_CHECK(strstr(dense, "safety_link_up") != NULL,
               "dashboard_get_status() must read safety_link_up (sl.link_up) somewhere in "
               "its body -- if this fails, the link_up bit this whole fix depends on was "
               "removed from this function entirely");

    free(dense);
}

void run_test_dashboard_safety_ready(void)
{
    test_no_driver_is_never_ready();
    test_driver_exists_but_link_down_reads_not_ready();
    test_driver_exists_and_link_up_reads_ready();
    test_status_fetch_error_reads_not_ready();
    test_safety_relay_known_gated_on_link_up_in_source();
}
