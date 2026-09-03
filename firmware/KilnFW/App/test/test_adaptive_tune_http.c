// Host test for App/drivers/adaptive_tune_http.c's status_get_handler() --
// GET /api/adaptive_tune. Added for the live bug: the zones page's
// "Continuous Tuning (learn from firings)" panel was stuck on "Loading..."
// because that endpoint returned exactly 1023 bytes of TRUNCATED JSON,
// ending mid-key ("coupled_cells_changed":0,"coupled_refusal" with no
// closing) -- json.load() on the client failed with "Expecting ':'
// delimiter: line 1 column 1024".
//
// Root cause: ADAPTIVE_TUNE_STATUS_BUF_BYTES was `320 * MAX31856_CHANNEL_
// COUNT + 64` (1024 bytes at 3 zones), a guessed per-zone budget never
// measured against the real per-zone snprintf() below it -- the real
// worst-case per-zone object is 778 bytes, nearly 2.5x the guess -- and the
// old code CLAMPED on overflow (`off = ADAPTIVE_TUNE_STATUS_BUF_BYTES - 1`)
// rather than failing, so it silently served the truncated body with a 200
// OK instead of an error, which is exactly what made this look like a hung
// fetch instead of a visible failure.
//
// adaptive_tune_http.c cannot be #included directly the way test_dashboard_
// json.c includes dashboard_json.c: its status_get_handler() is `static`,
// and pulling in the whole file would need stub bodies for wifi_provision_
// http_get_server()/httpd_register_uri_handler()/adaptive_tune_get_status()/
// adaptive_tune_set_enabled()/adaptive_tune_revert() just so the OTHER
// handlers in that translation unit link, none of which this test exercises
// -- the exact "wider stub surface than the function under test needs"
// situation test_zones_http.c's own header comment describes. Instead this
// file takes dashboard_http.c's/test_dashboard_json.c's OTHER documented
// path (that file's own header comment: dashboard_http.c #includes
// lvgl_port.h at file scope and cannot compile on this host toolchain at
// all): a hand-written MIRROR of status_get_handler()'s exact snprintf()
// format string, kept byte-for-byte in step with adaptive_tune_http.c by
// comment cross-reference in both files. This is the same trust boundary
// test_dashboard_json.c's render_worst_case_status_json() already accepts
// for dashboard_http.c's /api/status handler.
//
// adaptive_tune.h is included directly (not adaptive_tune.c) purely for the
// TYPE -- adaptive_tune_zone_status_t and its char[96] reason-string fields
// -- so the reason-string worst case (95 chars, from a 96-byte buffer) is
// read from the real struct via sizeof(), not re-typed as a magic number
// that could drift out of step with adaptive_tune.h.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

// Same relative-include trick test_dashboard_json.c uses (compiled with no
// driversDir on the include path -- the compiler still finds sibling
// headers relative to the including file's own directory).
#include "../drivers/adaptive_tune.h"

// Mirrors ADAPTIVE_TUNE_STATUS_BUF_BYTES in adaptive_tune_http.c EXACTLY --
// if that constant changes, this one must change with it (see this file's
// own header comment on the mirror-testing trust boundary).
#define ADAPTIVE_TUNE_STATUS_BUF_BYTES (900 * MAX31856_CHANNEL_COUNT + 64)

#define AT_APPEND(...)                                                                              \
    do {                                                                                             \
        int n_ = snprintf(json + o, cap - o, __VA_ARGS__);                                           \
        if (n_ < 0 || (size_t)n_ >= cap - o) { return false; }                                        \
        o += (size_t)n_;                                                                              \
    } while (0)

// Fills st with the WORST-CASE value every %-spec in status_get_handler()'s
// per-zone snprintf() can produce: every reason string at its full 95-char
// capacity (96-byte buffer, adaptive_tune.h), every %u field at its type's
// max, every %.4f/%.2f float at a wide negative magnitude (these are
// physical quantities -- gain, percent, dwell counts -- so a plausible
// worst case, not FLT_MAX; same convention test_dashboard_json.c's own
// fill_worst_case_zone() uses for prior_k_dc's analogues there).
static void fill_worst_case_zone_status(adaptive_tune_zone_status_t *st, char fill_char)
{
    memset(st, 0, sizeof(*st));
    st->enabled = false;                    // "false" (5) > "true" (4)
    st->ring_count = 0xFFFFFFFFu;
    st->observations_lifetime = 0xFFFFFFFFu;
    st->has_applied = false;
    st->prior_k_dc = -12345.6789f;
    st->applied_k_dc = -12345.6789f;
    st->last_delta_pct = -1234.56f;
    st->last_applied_profile_id = 255u;
    st->last_applied_unix_s = 0xFFFFFFFFu;
    memset(st->last_refusal_reason, fill_char, sizeof(st->last_refusal_reason) - 1);
    st->last_refusal_reason[sizeof(st->last_refusal_reason) - 1] = '\0';

    st->joint_observations = 0xFFFFFFFFu;
    st->coupled_attempted = false;
    st->coupled_applied = false;
    st->coupled_cells_changed = 255u;
    memset(st->coupled_refusal_reason, fill_char, sizeof(st->coupled_refusal_reason) - 1);
    st->coupled_refusal_reason[sizeof(st->coupled_refusal_reason) - 1] = '\0';

    st->ki_verdict = 255u;
    st->ki_correction_pct = -1234.56f;
    st->ki_applied = false;
    memset(st->ki_refusal_reason, fill_char, sizeof(st->ki_refusal_reason) - 1);
    st->ki_refusal_reason[sizeof(st->ki_refusal_reason) - 1] = '\0';

    st->revert_available = false;
}

// Byte-for-byte mirror of status_get_handler()'s snprintf() sequence
// (adaptive_tune_http.c) -- see this file's header comment for why this is
// a hand-kept mirror rather than a call into the real (static) handler.
// Returns false the instant any one field would not fit, exactly like the
// real handler's post-fix loud-failure behavior -- never returns true over
// a partial/invalid document.
static bool render_worst_case_adaptive_tune_json(char *json, size_t cap, size_t channel_count, size_t *out_len)
{
    size_t o = 0;

    AT_APPEND("{\"zones\":[");
    for (size_t zi = 0; zi < channel_count; zi++) {
        adaptive_tune_zone_status_t st;
        fill_worst_case_zone_status(&st, (char)('A' + (int)(zi % 26)));

        AT_APPEND(
            "%s{\"zone\":%u,\"enabled\":%s,\"observation_count\":%u,\"observations_lifetime\":%u,"
            "\"has_applied\":%s,\"prior_k_dc\":%.4f,\"applied_k_dc\":%.4f,\"delta_pct\":%.2f,"
            "\"last_profile_id\":%u,\"last_applied_unix_s\":%u,\"refusal\":\"%s\","
            "\"joint_observations\":%u,\"coupled_attempted\":%s,\"coupled_applied\":%s,"
            "\"coupled_cells_changed\":%u,\"coupled_refusal\":\"%s\","
            "\"ki_verdict\":%u,\"ki_correction_pct\":%.2f,\"ki_applied\":%s,\"ki_refusal\":\"%s\","
            "\"revert_available\":%s}",
            zi == 0 ? "" : ",", (unsigned)zi, st.enabled ? "true" : "false", (unsigned)st.ring_count,
            (unsigned)st.observations_lifetime, st.has_applied ? "true" : "false", (double)st.prior_k_dc,
            (double)st.applied_k_dc, (double)st.last_delta_pct, (unsigned)st.last_applied_profile_id,
            (unsigned)st.last_applied_unix_s, st.last_refusal_reason,
            (unsigned)st.joint_observations, st.coupled_attempted ? "true" : "false",
            st.coupled_applied ? "true" : "false", (unsigned)st.coupled_cells_changed, st.coupled_refusal_reason,
            (unsigned)st.ki_verdict, (double)st.ki_correction_pct, st.ki_applied ? "true" : "false",
            st.ki_refusal_reason, st.revert_available ? "true" : "false");
    }
    AT_APPEND("]}");

    if (out_len) *out_len = o;
    return true;
}
#undef AT_APPEND

/* True iff `json` is syntactically complete, balanced JSON -- same cheap
 * check test_dashboard_json.c's json_looks_complete() uses: starts with
 * '{', ends with '}', every '[' has a matching ']' before the final '}'. */
static bool json_looks_complete(const char *json)
{
    size_t len = strlen(json);
    if (len < 2 || json[0] != '{' || json[len - 1] != '}') { return false; }
    int depth = 0;
    for (size_t i = 0; i < len; i++) {
        if (json[i] == '[') depth++;
        else if (json[i] == ']') depth--;
    }
    return depth == 0;
}

static void test_worst_case_render_fits_documented_buffer(void)
{
    TEST_SECTION("render_worst_case_adaptive_tune_json() -- GET /api/adaptive_tune's actual "
                 "worst-case render must fit ADAPTIVE_TUNE_STATUS_BUF_BYTES, with real, measured "
                 "headroom reported here (this is the check that would have caught the live "
                 "1023-byte-truncated-JSON bug -- the old 320*N+64 constant was never measured "
                 "against this render at all)");

    char big[8192];
    size_t worst_len = 0;
    bool ok = render_worst_case_adaptive_tune_json(big, sizeof(big), MAX31856_CHANNEL_COUNT, &worst_len);
    TEST_CHECK(ok, "the worst-case render must fit comfortably in an 8192-byte scratch buffer, or "
              "this test's own measurement buffer is too small (raise `big`, not a real bug)");
    TEST_CHECK(worst_len == strlen(big), "the returned length must match the rendered string");

    printf("  /api/adaptive_tune worst-case render: %zu bytes (strlen) at MAX31856_CHANNEL_COUNT=%d, "
          "against ADAPTIVE_TUNE_STATUS_BUF_BYTES=%d -- measured headroom = %ld bytes\n",
          worst_len, (int)MAX31856_CHANNEL_COUNT, (int)ADAPTIVE_TUNE_STATUS_BUF_BYTES,
          (long)ADAPTIVE_TUNE_STATUS_BUF_BYTES - (long)worst_len);

    TEST_CHECK(worst_len < ADAPTIVE_TUNE_STATUS_BUF_BYTES, "the real worst-case render must fit "
              "ADAPTIVE_TUNE_STATUS_BUF_BYTES with room for the NUL terminator -- a failure here "
              "means the shipped buffer is genuinely too small, not a test artifact -- THIS is the "
              "exact assertion the live bug (1023B truncated body) would have failed under the old "
              "320*N+64 constant");
    /* Minimum real margin, not just ">0" -- same 50-byte rule of thumb
     * test_dashboard_json.c applies to its own analogous buffer. */
    TEST_CHECK((long)ADAPTIVE_TUNE_STATUS_BUF_BYTES - (long)worst_len >= 50,
              "headroom has shrunk below this file's own 50-byte minimum margin -- raise "
              "ADAPTIVE_TUNE_STATUS_BUF_BYTES in adaptive_tune_http.c");

    TEST_CHECK(json_looks_complete(big), "the worst-case render must itself be complete, balanced "
              "JSON -- a bug in this mirror, not the production handler, would show up here");
}

static void test_mutation_old_buffer_size_goes_red(void)
{
    TEST_SECTION("MUTATION 1/2 -- the OLD (buggy) 320*N+64 buffer size, against today's actual "
                 "worst-case render, must fail: proves this test really would have caught the "
                 "live truncation bug, not just that the new constant happens to be bigger");

    char big[8192];
    size_t worst_len = 0;
    bool ok0 = render_worst_case_adaptive_tune_json(big, sizeof(big), MAX31856_CHANNEL_COUNT, &worst_len);
    TEST_CHECK(ok0, "setup: the unshrunk render must succeed, or this mutation test proves nothing");

    size_t old_buggy_size = (size_t)(320 * MAX31856_CHANNEL_COUNT + 64);
    printf("  old (buggy) buffer size = %zu bytes; today's real worst case = %zu bytes\n",
          old_buggy_size, worst_len);

    char *tight = malloc(old_buggy_size > 0 ? old_buggy_size : 1);
    TEST_CHECK(tight != NULL, "malloc must succeed on a host with plenty of heap");
    if (tight != NULL) {
        size_t got_len = 0;
        bool ok = render_worst_case_adaptive_tune_json(tight, old_buggy_size, MAX31856_CHANNEL_COUNT, &got_len);
        printf("  RED (expected): render into the old %zu-byte buffer returned %s\n", old_buggy_size,
              ok ? "true (BUG -- old size was not actually too small)" : "false");
        TEST_CHECK(!ok, "the old 320*N+64 buffer size must be reported as too small against the real "
                  "worst-case render -- this is the exact regression this whole file exists to catch");
        free(tight);
    }
}

static void test_mutation_shrink_new_buffer_goes_red(void)
{
    TEST_SECTION("MUTATION 2/2 -- shrinking the buffer to just under the measured worst case must "
                 "make the worst-case render fail (prove the check can actually fail, not just "
                 "that it currently passes)");

    char big[8192];
    size_t worst_len = 0;
    bool ok0 = render_worst_case_adaptive_tune_json(big, sizeof(big), MAX31856_CHANNEL_COUNT, &worst_len);
    TEST_CHECK(ok0, "setup: the unshrunk render must succeed, or this mutation test proves nothing");

    char *tight = malloc(worst_len);
    TEST_CHECK(tight != NULL, "malloc must succeed on a host with plenty of heap");
    if (tight != NULL) {
        size_t got_len = 0;
        bool ok = render_worst_case_adaptive_tune_json(tight, worst_len, MAX31856_CHANNEL_COUNT, &got_len);
        printf("  RED (expected): render into a %zu-byte buffer (worst case is %zu bytes) returned %s\n",
              worst_len, worst_len, ok ? "true (BUG)" : "false");
        TEST_CHECK(!ok, "a buffer exactly at (not over) the worst-case length must be reported as too "
                  "small -- this is the exact class of bug a stale/reverted "
                  "ADAPTIVE_TUNE_STATUS_BUF_BYTES would reintroduce");
        free(tight);
    }
}

int main(void)
{
    test_worst_case_render_fits_documented_buffer();
    test_mutation_old_buffer_size_goes_red();
    test_mutation_shrink_new_buffer_goes_red();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    return g_test_failures == 0 ? 0 : 1;
}
