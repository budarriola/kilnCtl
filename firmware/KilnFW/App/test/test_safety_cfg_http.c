// Host test for App/drivers/safety_cfg_http.c's pure/static helpers --
// parse_set_param_body() (the id=/value= form tokenizer), parse_value_for_
// type(), build_commissioning_json() and apply_pairs(). Own SEPARATE
// executable (build_host_tests.ps1's third build+run step), same reason
// test_zones_http.c is: safety_cfg_http.c's static functions have no other
// seam, so this file #includes it directly, which means defining its OWN
// fake bodies for safety_cfg_store_get_by_index()/safety_link_get_status()/
// etc -- and the main executable already links the REAL safety_cfg_store_*
// definitions via test_safety_cfg_store.c's #include of safety_cfg_store.c.
// Linking both into one binary would multiply-define every safety_cfg_
// store_* symbol.
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"

#include "kilnlink/kilnlink_commit_config_rejected.h"

// safety_cfg_http.c gained the /safety/commissioning PAGE handler after this
// test was first written, and that handler declares the embedded gzip blob
// with GCC's `asm("_binary_...")` label syntax -- which MSVC cannot parse at
// all (error C2061: syntax error: identifier 'asm'). #define it away to
// nothing, exactly as test_zones_http.c already does for the identical
// reason; see that file's own note. The symbols themselves are then supplied
// as ordinary arrays below, since nothing on the host links the real
// EMBED_TXTFILES blobs.
#define asm(x)

const uint8_t safety_commissioning_page_html_gz_start[] = { 0x1f, 0x8b, 0x00 };
const uint8_t safety_commissioning_page_html_gz_end[] = { 0x00 };

// web_encoding.h -- reached only from the page handler, which these tests do
// not exercise (they cover the JSON builder, the tokenizer and apply_pairs).
// Same stub bodies test_zones_http.c uses for the identical reason.
esp_err_t web_send_gzip_not_acceptable(httpd_req_t *req, const char *tag, const char *page_name)
{
    (void)req;
    (void)tag;
    (void)page_name;
    return ESP_OK;
}
bool web_client_accepts_gzip(httpd_req_t *req)
{
    (void)req;
    return true;
}

#include "../drivers/safety_cfg_http.c"

// ---------------------------------------------------------------------------
// esp_http_server.h stub bodies -- never invoked by these tests (only the
// static helper functions are called directly), but every symbol safety_cfg_
// http.c references anywhere in the file must resolve at link time.
// ---------------------------------------------------------------------------
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler)
{
    (void)handle;
    (void)uri_handler;
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type) { (void)r; (void)type; return ESP_OK; }
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value)
{
    (void)r; (void)field; (void)value; return ESP_OK;
}

/* web_encoding.c is not part of the host build (it pulls in the real httpd),
   but zones_http.c/safety_cfg_http.c now call this from their page handlers.
   Same local-stub convention as httpd_resp_set_hdr() just above. */
void web_set_asset_cache_headers(httpd_req_t *r);
void web_set_asset_cache_headers(httpd_req_t *r) { (void)r; }
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len)
{
    (void)r; (void)buf; (void)buf_len; return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len)
{
    (void)r; (void)buf; (void)buf_len; return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg)
{
    (void)r; (void)error; (void)msg; return ESP_OK;
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status) { (void)r; (void)status; return ESP_OK; }
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len) { (void)r; (void)buf; (void)buf_len; return 0; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { (void)r; (void)s; return ESP_OK; }

httpd_handle_t wifi_provision_http_get_server(void) { return NULL; }

// ---------------------------------------------------------------------------
// safety_cfg_store.h stub bodies -- controllable fakes, same convention
// test_kiln_cfg_store.c/test_backup_import.c use for zones_http.h.
// ---------------------------------------------------------------------------
static safety_cfg_param_t s_stub_params[SAFETY_CFG_PARAM_COUNT];
static bool s_stub_lookup_result = true;
static uint8_t s_stub_lookup_type = KILNLINK_PARAM_TYPE_U16;
static const char *s_stub_lookup_name = "stub_field";

size_t safety_cfg_store_param_count(void) { return SAFETY_CFG_PARAM_COUNT; }

bool safety_cfg_store_get_by_index(size_t index, safety_cfg_param_t *out)
{
    if (!out || index >= SAFETY_CFG_PARAM_COUNT) {
        return false;
    }
    *out = s_stub_params[index];
    return true;
}

bool safety_cfg_store_lookup(uint16_t param_id, uint8_t *out_type, const char **out_name)
{
    (void)param_id;
    if (!s_stub_lookup_result) {
        return false;
    }
    if (out_type) *out_type = s_stub_lookup_type;
    if (out_name) *out_name = s_stub_lookup_name;
    return true;
}

uint16_t safety_cfg_store_cached_crc(void) { return 0; }
uint32_t safety_cfg_store_fetched_ms_ago(void) { return UINT32_MAX; }
bool safety_cfg_store_refetch(SafetyLinkClass *link, uint16_t crc) { (void)link; (void)crc; return true; }
bool safety_cfg_store_maybe_refetch(SafetyLinkClass *link, uint16_t crc) { (void)link; (void)crc; return false; }
esp_err_t safety_cfg_store_init(void) { return ESP_OK; }

// ---------------------------------------------------------------------------
// safety_link.h stub bodies -- controllable fakes.
// ---------------------------------------------------------------------------
static esp_err_t s_stub_set_param_result = ESP_OK;
static esp_err_t s_stub_commit_result = ESP_OK;
static int s_stub_set_param_calls = 0;
static int s_stub_commit_calls = 0;
static bool s_stub_commit_rejected = false;
static uint16_t s_stub_commit_reject_param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
static uint8_t s_stub_commit_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_RANGE;

esp_err_t safety_link_send_set_param(SafetyLinkClass *link, uint16_t param_id, uint8_t type,
                                      kilnlink_param_value_t value)
{
    (void)link; (void)param_id; (void)type; (void)value;
    s_stub_set_param_calls++;
    return s_stub_set_param_result;
}

esp_err_t safety_link_send_commit_config(SafetyLinkClass *link, uint16_t *out_param_id, uint8_t *out_reason,
                                          bool *out_rejected)
{
    (void)link;
    s_stub_commit_calls++;
    if (out_rejected) *out_rejected = s_stub_commit_rejected;
    if (out_param_id) *out_param_id = s_stub_commit_reject_param_id;
    if (out_reason) *out_reason = s_stub_commit_reject_reason;
    return s_stub_commit_result;
}

esp_err_t safety_link_get_status(SafetyLinkClass *link, safety_link_status_t *out)
{
    (void)link;
    if (out) memset(out, 0, sizeof(*out));
    return ESP_OK;
}

esp_err_t safety_link_get_peer_build_status(SafetyLinkClass *link, bool *out_known, bool *out_dirty,
                                             uint8_t *commit_buf, uint8_t *out_commit_len,
                                             uint8_t *datetime_buf, uint8_t *out_datetime_len,
                                             uint8_t *out_config_version, uint16_t *out_config_crc)
{
    (void)link; (void)out_dirty; (void)commit_buf; (void)out_commit_len; (void)datetime_buf;
    (void)out_datetime_len; (void)out_config_version;
    if (out_known) *out_known = false;
    if (out_config_crc) *out_config_crc = 0;
    return ESP_OK;
}

// ---------------------------------------------------------------------------
// Tests
// ---------------------------------------------------------------------------

static void reset_all(void)
{
    memset(s_stub_params, 0, sizeof(s_stub_params));
    s_stub_lookup_result = true;
    s_stub_lookup_type = KILNLINK_PARAM_TYPE_U16;
    s_stub_lookup_name = "stub_field";
    s_stub_set_param_result = ESP_OK;
    s_stub_commit_result = ESP_OK;
    s_stub_set_param_calls = 0;
    s_stub_commit_calls = 0;
    s_stub_commit_rejected = false;
    s_stub_commit_reject_param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
    s_stub_commit_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_RANGE;
}

static void test_parse_single_pair_no_commit(void)
{
    TEST_SECTION("parse_set_param_body -- a single id=/value= pair, no commit");
    safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = true; // must be reset to false by the parser
    int n = parse_set_param_body("id=257&value=1", pairs, SAFETY_CFG_POST_MAX_PAIRS, &commit);
    TEST_CHECK(n == 1, "one pair parsed");
    TEST_CHECK(pairs[0].param_id == 257, "id decoded as 257 (0x0101, tc_source)");
    TEST_CHECK(strcmp(pairs[0].value_text, "1") == 0, "value text is \"1\"");
    TEST_CHECK(commit == false, "commit was not requested -- defaults false, not left uninitialized");
}

static void test_parse_multiple_pairs_plus_commit(void)
{
    TEST_SECTION("parse_set_param_body -- repeated id=/value= pairs plus a trailing commit=1");
    safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = false;
    int n = parse_set_param_body("id=513&value=100.5&id=514&value=60&commit=1", pairs,
                                  SAFETY_CFG_POST_MAX_PAIRS, &commit);
    TEST_CHECK(n == 2, "two pairs parsed");
    TEST_CHECK(pairs[0].param_id == 513 && strcmp(pairs[0].value_text, "100.5") == 0,
               "first pair (0x0201 firing_margin_c) intact");
    TEST_CHECK(pairs[1].param_id == 514 && strcmp(pairs[1].value_text, "60") == 0,
               "second pair (0x0202 overshoot_margin_c) intact");
    TEST_CHECK(commit == true, "commit=1 was recognised");
}

static void test_parse_rejects_value_without_id(void)
{
    TEST_SECTION("parse_set_param_body -- a bare \"value=\" with no preceding \"id=\" is rejected");
    safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = false;
    int n = parse_set_param_body("value=5", pairs, SAFETY_CFG_POST_MAX_PAIRS, &commit);
    TEST_CHECK(n == -1, "malformed body (orphan value) is refused, not silently dropped");
}

static void test_parse_rejects_dangling_id(void)
{
    TEST_SECTION("parse_set_param_body -- a trailing \"id=\" with no following \"value=\" is rejected");
    safety_cfg_post_pair_t pairs[SAFETY_CFG_POST_MAX_PAIRS];
    bool commit = false;
    int n = parse_set_param_body("id=257&commit=1", pairs, SAFETY_CFG_POST_MAX_PAIRS, &commit);
    TEST_CHECK(n == -1, "an id with no paired value is refused");
}

static void test_parse_value_for_type_bounds(void)
{
    TEST_SECTION("parse_value_for_type -- accepts in-range, rejects out-of-range/garbage per wire type");
    kilnlink_param_value_t v;
    TEST_CHECK(parse_value_for_type("1", KILNLINK_PARAM_TYPE_BOOL, &v) && v.bool_val == 1,
               "bool '1' parses");
    TEST_CHECK(parse_value_for_type("2", KILNLINK_PARAM_TYPE_BOOL, &v) == false,
               "bool '2' is out of range (0/1 only)");
    TEST_CHECK(parse_value_for_type("255", KILNLINK_PARAM_TYPE_U8, &v) && v.u8_val == 255,
               "u8 accepts its max value");
    TEST_CHECK(parse_value_for_type("256", KILNLINK_PARAM_TYPE_U8, &v) == false, "u8 rejects 256");
    TEST_CHECK(parse_value_for_type("65535", KILNLINK_PARAM_TYPE_U16, &v) && v.u16_val == 65535,
               "u16 accepts its max value");
    TEST_CHECK(parse_value_for_type("70000", KILNLINK_PARAM_TYPE_U16, &v) == false, "u16 rejects 70000");
    TEST_CHECK(parse_value_for_type("123.5", KILNLINK_PARAM_TYPE_F32, &v) && v.f32_val > 123.0f,
               "f32 parses a decimal");
    TEST_CHECK(parse_value_for_type("not_a_number", KILNLINK_PARAM_TYPE_F32, &v) == false,
               "garbage text is rejected, not parsed as 0");
    TEST_CHECK(parse_value_for_type("12abc", KILNLINK_PARAM_TYPE_U16, &v) == false,
               "trailing garbage after a valid-looking prefix is rejected, not truncated");
}

static void test_build_json_unset_param_omits_value(void)
{
    TEST_SECTION("build_commissioning_json -- an unset parameter carries \"set\":false and OMITS \"value\"");
    reset_all();
    s_stub_params[0].param_id = 0x0101;
    s_stub_params[0].name = "tc_source";
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_U8;
    s_stub_params[0].set = false;
    s_stub_params[0].value.u8_val = 0; // a real, plausible-looking value that must NOT be printed

    safety_cfg_http_snapshot_t snap = {0};
    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"id\":257") != NULL, "the unset param still appears in the list");
    TEST_CHECK(strstr(json, "\"set\":false") != NULL, "it is marked set:false");
    TEST_CHECK(strstr(json, "\"value\"") == NULL,
               "\"value\" is OMITTED entirely -- never a printed 0 a page could mistake for real");
}

static void test_build_json_set_param_includes_value(void)
{
    TEST_SECTION("build_commissioning_json -- a set parameter DOES include its value");
    reset_all();
    s_stub_params[0].param_id = 0x0104;
    s_stub_params[0].name = "abs_max_temp_c";
    s_stub_params[0].type = KILNLINK_PARAM_TYPE_F32;
    s_stub_params[0].set = true;
    s_stub_params[0].value.f32_val = 1300.0f;

    safety_cfg_http_snapshot_t snap = {0};
    static char json[SAFETY_CFG_JSON_MAX];
    size_t len = build_commissioning_json(&snap, json, sizeof(json));
    TEST_CHECK(len > 0, "JSON built successfully");
    TEST_CHECK(strstr(json, "\"set\":true") != NULL, "marked set:true");
    TEST_CHECK(strstr(json, "\"value\":1300") != NULL, "the real value is present");
}

static void test_stale_flag_reflects_crc_mismatch(void)
{
    TEST_SECTION("snapshot_is_stale -- true on any mismatch or unknown-live-crc, false only when they agree");
    safety_cfg_http_snapshot_t snap = {0};
    snap.live_crc_known = false;
    TEST_CHECK(snapshot_is_stale(&snap) == true, "an unknown live CRC can never be vouched for as current");

    snap.live_crc_known = true;
    snap.live_crc = 100;
    snap.cached_crc = 200;
    TEST_CHECK(snapshot_is_stale(&snap) == true, "a real mismatch is stale");

    snap.cached_crc = 100;
    TEST_CHECK(snapshot_is_stale(&snap) == false, "matching CRCs -- not stale");
}

static void test_apply_pairs_all_succeed_with_commit(void)
{
    TEST_SECTION("apply_pairs -- every pair staged, commit ACKed -- success, empty reason");
    reset_all();
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[2] = {
        { .param_id = 513, .value_text = "100" },
        { .param_id = 514, .value_text = "75" },
    };
    char reason[160];
    bool ok = apply_pairs(&fake_link, pairs, 2, true, reason, sizeof(reason));
    TEST_CHECK(ok == true, "all staged and committed successfully");
    TEST_CHECK(reason[0] == '\0', "no reason text on success");
    TEST_CHECK(s_stub_set_param_calls == 2, "both pairs were staged");
    TEST_CHECK(s_stub_commit_calls == 1, "commit was sent exactly once");
}

static void test_apply_pairs_unknown_id_is_refused_and_named(void)
{
    TEST_SECTION("apply_pairs -- an unknown param id is refused and named in the reason");
    reset_all();
    s_stub_lookup_result = false; // simulates an id this build's table does not recognise
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x9999, .value_text = "1" } };
    char reason[160];
    bool ok = apply_pairs(&fake_link, pairs, 1, false, reason, sizeof(reason));
    TEST_CHECK(ok == false, "an unknown id is refused");
    TEST_CHECK(strstr(reason, "39321") != NULL || strstr(reason, "unknown") != NULL,
               "the reason names the offending id (COMMISSIONING.md sec 2's per-id refusal)");
    TEST_CHECK(s_stub_set_param_calls == 0, "nothing was sent to the wire for an unrecognised id");
}

static void test_apply_pairs_refused_commit_surfaces_reason(void)
{
    TEST_SECTION("apply_pairs -- a commit the safety processor never ACKs surfaces a reason, not silence");
    reset_all();
    s_stub_commit_result = ESP_ERR_TIMEOUT;
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 513, .value_text = "100" } };
    char reason[160];
    bool ok = apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason));
    TEST_CHECK(ok == false, "an un-ACKed commit is reported as a failure");
    TEST_CHECK(reason[0] != '\0', "a non-empty reason is always produced on failure");
    TEST_CHECK(strstr(reason, "commit") != NULL || strstr(reason, "acknowledge") != NULL,
               "the reason talks about the commit outcome, not a generic error");
    TEST_CHECK(s_stub_set_param_calls == 1, "the field WAS staged before the commit was attempted");
}

static void test_apply_pairs_rejected_commit_names_field_and_reason(void)
{
    TEST_SECTION("apply_pairs -- a REJECTED commit (0x20) names the offending field and reason, "
                 "not \"awaiting confirmation\"");
    reset_all();
    s_stub_commit_rejected = true;
    s_stub_commit_reject_param_id = 0x0104u; // abs_max_temp_c
    s_stub_commit_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_RANGE;
    s_stub_lookup_name = "abs_max_temp_c"; // safety_cfg_store_lookup() stub returns this for any id
    SafetyLinkClass fake_link;
    memset(&fake_link, 0, sizeof(fake_link));
    // value_text just has to parse for the stub's default wire type
    // (KILNLINK_PARAM_TYPE_U16) -- the actual rejection this test exercises
    // comes from the stubbed safety_link_send_commit_config() outcome, not
    // from parse_value_for_type(), so this must be an in-range U16.
    safety_cfg_post_pair_t pairs[1] = { { .param_id = 0x0104u, .value_text = "500" } };
    char reason[160];
    bool ok = apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason));
    TEST_CHECK(ok == false, "a rejected commit is reported as a failure, not success");
    TEST_CHECK(strstr(reason, "abs_max_temp_c") != NULL,
               "the offending field is named (COMMISSIONING.md sec 3.1: \"name the offending field\")");
    TEST_CHECK(strstr(reason, "range") != NULL, "the reason (\"value out of range\") is named");
    TEST_CHECK(strstr(reason, "awaiting confirmation") == NULL,
               "this is no longer the old ambiguous \"sent, awaiting confirmation\" wording");
    TEST_CHECK(s_stub_commit_calls == 1, "commit was sent exactly once");

    // Same rejection, but a not-field-specific one (ARMED) -- the
    // NO_PARAM_ID sentinel must not be looked up as if it were a real id.
    reset_all();
    s_stub_commit_rejected = true;
    s_stub_commit_reject_param_id = KILNLINK_COMMIT_CONFIG_REJECTED_NO_PARAM_ID;
    s_stub_commit_reject_reason = KILNLINK_COMMIT_CONFIG_REJECT_ARMED;
    reason[0] = '\0';
    ok = apply_pairs(&fake_link, pairs, 1, true, reason, sizeof(reason));
    TEST_CHECK(ok == false, "an ARMED rejection is reported as a failure");
    TEST_CHECK(strstr(reason, "ARMED") != NULL, "the ARMED reason is named");
    TEST_CHECK(strstr(reason, "0x0104") == NULL && strstr(reason, "abs_max_temp_c") == NULL,
               "a not-field-specific rejection does not fabricate a field name");
}

int main(void)
{
    test_parse_single_pair_no_commit();
    test_parse_multiple_pairs_plus_commit();
    test_parse_rejects_value_without_id();
    test_parse_rejects_dangling_id();
    test_parse_value_for_type_bounds();
    test_build_json_unset_param_omits_value();
    test_build_json_set_param_includes_value();
    test_stale_flag_reflects_crc_mismatch();
    test_apply_pairs_all_succeed_with_commit();
    test_apply_pairs_unknown_id_is_refused_and_named();
    test_apply_pairs_refused_commit_surfaces_reason();
    test_apply_pairs_rejected_commit_names_field_and_reason();

    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
