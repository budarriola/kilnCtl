// Campaign 8 (docs/audits/HOST_TEST_COVERAGE_GAPS_2026-10-09.md #8): handler-level
// matrix for POST /api/aux_outputs and POST /api/aux_outputs/manual.
//
// Own executable. #includes the REAL aux_outputs_cfg.c store (over fake_kv + a real
// cfg_fs scratch dir), the REAL aux_outputs_http_core.c and the REAL aux_outputs_http.c
// (body read, reply mapping, route registration) and the real system_mode_gate.c. The
// handlers are fetched through aux_outputs_http_start()'s registration, exactly as
// production reaches them. Only the relay board (dashboard_set_relay), the heat-run flags
// (relay_authority_heat_run_active), zones accessors and the httpd transport are fakes.
//
// Every assertion on a refusal is against the REAL store (entries, enabled mask, rev) and
// the fake relay board's call log, not the reply text. Finding IDs (K8-xx) are in
// docs/audits/HOST_TEST_CAMPAIGN_FINDINGS_2026-10-09.md.
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <direct.h>
#define TAO_MKDIR(p) _mkdir(p)
#define TAO_RMDIR(p) _rmdir(p)
#else
#include <sys/stat.h>
#include <unistd.h>
#define TAO_MKDIR(p) mkdir((p), 0755)
#define TAO_RMDIR(p) rmdir(p)
#endif

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include "esp_err.h"
#include "esp_http_server.h"
#include "fake_kv.h"
#include "cfg_fs.h"

#define TAG TAG_cfg
#include "../drivers/persist/aux_outputs_cfg.c"
#undef TAG
#define TAG TAG_core
#include "../drivers/http/aux_outputs_http_core.c"
#undef TAG
#include "../drivers/http/aux_outputs_http.c"
#include "../drivers/http/route_tier_table.h"

/* ---- fakes for aux_outputs_http.c's collaborators ---- */
static bool f_profile_running, f_autotune_running;
void relay_authority_heat_run_active(bool *profile, bool *autotune)
{
    *profile = f_profile_running;
    *autotune = f_autotune_running;
}

static uint8_t f_zone_relay_mask[MAX31856_CHANNEL_COUNT];
bool zones_config_get_relay_mask(uint8_t zi, uint8_t *out)
{
    if (zi >= MAX31856_CHANNEL_COUNT) {
        return false;
    }
    *out = f_zone_relay_mask[zi];
    return true;
}

static int f_relay_calls;
static uint8_t f_relay_last;
static bool f_relay_last_on;
static bool f_relay_state[AUX_OUTPUTS_COUNT + 1];
static dashboard_relay_result_t f_relay_result;
dashboard_relay_result_t dashboard_set_relay(uint8_t relay_index, bool on, uint32_t *out_safety_sources)
{
    (void)out_safety_sources;
    f_relay_calls++;
    f_relay_last = relay_index;
    f_relay_last_on = on;
    if (f_relay_result == DASHBOARD_RELAY_OK && relay_index >= 1 && relay_index <= AUX_OUTPUTS_COUNT) {
        f_relay_state[relay_index] = on;
    }
    return f_relay_result;
}

static int f_claim_busy; /* 1 = somebody else holds the commissioning claim */
static int f_claims, f_releases;
bool safety_cfg_writer_try_claim(safety_cfg_writer_t who)
{
    (void)who;
    if (f_claim_busy) {
        return false;
    }
    f_claims++;
    return true;
}
bool safety_cfg_writer_release(safety_cfg_writer_t who)
{
    (void)who;
    f_releases++;
    return true;
}

bool relay_authority_reset_in_flight(void) { return false; }
bool relay_authority_reset_refuses_writer(void) { return false; }
httpd_handle_t wifi_provision_http_get_server(void) { return (httpd_handle_t)0x1; }
void zone_aux_convert_http_start(void) {}
void zones_config_json_set_aux_enabled_provider(uint8_t (*fn)(void)) { (void)fn; }

typedef struct {
    char uri[48];
    int method;
    esp_err_t (*handler)(httpd_req_t *);
} reg_t;
static reg_t s_reg[8];
static int s_reg_n;
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *u)
{
    (void)server;
    snprintf(s_reg[s_reg_n].uri, sizeof(s_reg[s_reg_n].uri), "%s", u->uri);
    s_reg[s_reg_n].method = (int)u->method;
    s_reg[s_reg_n].handler = u->handler;
    s_reg_n++;
    return ESP_OK;
}

/* ---- httpd transport fakes ---- */
static int s_status; /* last status the handler produced (200 unless set) */
static char s_reply[256];
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t) { (void)r; (void)t; return ESP_OK; }
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s) { (void)r; s_status = atoi(s); return ESP_OK; }
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s)
{
    (void)r;
    snprintf(s_reply, sizeof(s_reply), "%s", s ? s : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *m)
{
    (void)r;
    s_status = (int)e;
    snprintf(s_reply, sizeof(s_reply), "%s", m ? m : "");
    return ESP_OK;
}
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *b, size_t n) { (void)r; (void)b; (void)n; return ESP_OK; }

static const char *s_body;
static size_t s_body_len, s_off, s_chunk;
static size_t s_fail_after;
static int s_fail_ret;
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len)
{
    (void)r;
    if (s_off >= s_fail_after) {
        return s_fail_ret;
    }
    if (!s_body || s_off >= s_body_len) {
        return 0;
    }
    size_t rem = s_body_len - s_off;
    if (s_fail_after != (size_t)-1 && s_off + rem > s_fail_after) {
        rem = s_fail_after - s_off;
    }
    size_t n = rem < buf_len ? rem : buf_len;
    if (n > s_chunk) {
        n = s_chunk;
    }
    memcpy(buf, s_body + s_off, n);
    s_off += n;
    return (int)n;
}

/* post: declared_len lets a test lie about Content-Length (truncated body). */
static int post(esp_err_t (*h)(httpd_req_t *), const char *body, long long declared_len, size_t fail_after, int fail_ret)
{
    httpd_req_t req;
    memset(&req, 0, sizeof(req));
    req.content_len = declared_len;
    s_body = body;
    s_body_len = body ? strlen(body) : 0;
    s_off = 0;
    s_chunk = 3; /* force the recv loop to iterate */
    s_fail_after = fail_after;
    s_fail_ret = fail_ret;
    s_status = 200;
    s_reply[0] = '\0';
    (void)h(&req);
    return s_status;
}
static int post_ok(esp_err_t (*h)(httpd_req_t *), const char *body)
{
    return post(h, body, (long long)strlen(body), (size_t)-1, 0);
}

/* ---- real-store fixture ---- */
static const char *SCRATCH = "cfg_fs_test_aux_http_handlers";
static void scratch_reset(void)
{
    char p[600];
    snprintf(p, sizeof(p), "%s/.tmp/%s", SCRATCH, AUX_OUTPUTS_FILE_PATH);
    remove(p);
    snprintf(p, sizeof(p), "%s/%s", SCRATCH, AUX_OUTPUTS_FILE_PATH);
    remove(p);
    snprintf(p, sizeof(p), "%s/.tmp", SCRATCH);
    TAO_RMDIR(p);
    TAO_RMDIR(SCRATCH);
    TAO_MKDIR(SCRATCH);
    cfg_fs_deinit();
    pref_cfg_fs_reset_write_fn_for_test();
}

static esp_err_t (*h_cfg)(httpd_req_t *);
static esp_err_t (*h_manual)(httpd_req_t *);

static void fresh(void)
{
    scratch_reset();
    fake_kv_reset_all();
    hal_kv_init_partition(KILN_NVS_PARTITION);
    TEST_CHECK(cfg_fs_init(SCRATCH, NULL) == ESP_OK, "cfg_fs mounts scratch dir");
    memset(f_zone_relay_mask, 0, sizeof(f_zone_relay_mask));
    memset(f_relay_state, 0, sizeof(f_relay_state));
    f_profile_running = f_autotune_running = false;
    f_relay_calls = 0;
    f_relay_result = DASHBOARD_RELAY_OK;
    f_claim_busy = 0;
    f_claims = f_releases = 0;
    TEST_CHECK(aux_outputs_cfg_start(0) == ESP_OK, "store starts");
}

/* snapshot of everything a refusal must not change */
typedef struct {
    uint8_t enabled, conflict;
    uint32_t rev;
    aux_output_t e[AUX_OUTPUTS_COUNT + 1];
    int relay_calls;
    bool relay_state[AUX_OUTPUTS_COUNT + 1];
} snap_t;
static void take(snap_t *s)
{
    memset(s, 0, sizeof(*s));
    s->enabled = aux_outputs_cfg_enabled_mask();
    s->conflict = aux_outputs_cfg_conflict_mask();
    s->rev = s_rev;
    for (uint8_t r = 1; r <= AUX_OUTPUTS_COUNT; r++) {
        aux_outputs_cfg_get(r, &s->e[r]);
        s->relay_state[r] = f_relay_state[r];
    }
    s->relay_calls = f_relay_calls;
}
static bool same(const snap_t *a)
{
    snap_t b;
    take(&b);
    return a->enabled == b.enabled && a->conflict == b.conflict && a->rev == b.rev &&
           a->relay_calls == b.relay_calls && memcmp(a->relay_state, b.relay_state, sizeof(a->relay_state)) == 0 &&
           memcmp(a->e, b.e, sizeof(a->e)) == 0;
}

static void enable_aux(uint8_t relay)
{
    char b[64];
    snprintf(b, sizeof(b), "relay=%u&enabled=1", relay);
    TEST_CHECK(post_ok(h_cfg, b) == 200, "setup: enable aux");
}

static void test_registration_and_tier(void)
{
    TEST_SECTION("route registration + auth tier");
    s_reg_n = 0;
    TEST_CHECK(aux_outputs_http_start() == ESP_OK, "aux_outputs_http_start registers");
    TEST_CHECK(s_reg_n == 3, "three routes registered (GET, POST, POST manual)");
    for (int i = 0; i < s_reg_n; i++) {
        if (s_reg[i].method == HTTP_POST && strcmp(s_reg[i].uri, "/api/aux_outputs") == 0) {
            h_cfg = s_reg[i].handler;
        }
        if (s_reg[i].method == HTTP_POST && strcmp(s_reg[i].uri, "/api/aux_outputs/manual") == 0) {
            h_manual = s_reg[i].handler;
        }
    }
    TEST_CHECK(h_cfg && h_manual, "both POST handlers captured");
    for (int i = 0; i < s_reg_n; i++) {
        route_tier_t found = ROUTE_TIER_OPEN;
        bool have = false;
        for (size_t k = 0; k < sizeof(kRouteTierTable) / sizeof(kRouteTierTable[0]); k++) {
            if (strcmp(kRouteTierTable[k].uri, s_reg[i].uri) == 0 && (int)kRouteTierTable[k].method == s_reg[i].method) {
                found = kRouteTierTable[k].tier;
                have = true;
            }
        }
        TEST_CHECK(have && found == ROUTE_TIER_ADMIN, "every aux route is ADMIN tier in the table");
    }
}

static void test_mode_gate(void)
{
    TEST_SECTION("system-mode gate: 409 while a firing or autotune runs, nothing changes");
    fresh();
    enable_aux(2);
    snap_t s;
    take(&s);
    const char *cfg = "relay=3&enabled=1&hyst_c=2.0";
    const char *man = "relay=2&on=1";
    for (int mode = 0; mode < 2; mode++) {
        f_profile_running = mode == 0;
        f_autotune_running = mode == 1;
        TEST_CHECK(post_ok(h_cfg, cfg) == 409, "config write refused 409 mid-run");
        TEST_CHECK(post_ok(h_manual, man) == 409, "manual write refused 409 mid-run");
        TEST_CHECK(same(&s), "mid-run refusals: store, rev and relay board untouched");
        TEST_CHECK(f_claims == f_releases, "claim/release balanced");
    }
    f_profile_running = f_autotune_running = false;
    TEST_CHECK(post_ok(h_manual, man) == 200 && f_relay_state[2], "control: idle manual on succeeds");
}

static void test_indices_and_fields(void)
{
    TEST_SECTION("relay index range, invalid / duplicate fields");
    fresh();
    enable_aux(1);
    snap_t s;
    take(&s);
    const char *bad_cfg[] = {
        "relay=0&enabled=1", "relay=5&enabled=1", "relay=-1&enabled=1", "relay=255&enabled=1", "relay=256&enabled=1",
        "relay=abc&enabled=1", "relay=&enabled=1", "relay=1.5&enabled=1", "relay=1x&enabled=1",
        "relay=99999999999999999999&enabled=1", "enabled=1", "relay=2", "relay=2&enabled=2", "relay=2&enabled=-1",
        "relay=2&enabled=", "relay=2&enabled=1&tc_zone=9", "relay=2&enabled=1&tc_zone=-2",
        "relay=2&enabled=1&hyst_c=nan", "relay=2&enabled=1&hyst_c=inf", "relay=2&enabled=1&hyst_c=1e99",
        "relay=2&enabled=1&hyst_c=-1", "relay=2&enabled=1&min_on_s=-1", "relay=2&enabled=1&min_on_s=999999",
        "relay=2&enabled=1&min_off_s=x", "relay=%00&enabled=1", "relay=2%00&enabled=1",
    };
    for (size_t i = 0; i < sizeof(bad_cfg) / sizeof(bad_cfg[0]); i++) {
        int st = post_ok(h_cfg, bad_cfg[i]);
        char m[160];
        snprintf(m, sizeof(m), "cfg [%s] refused 4xx and nothing changed", bad_cfg[i]);
        TEST_CHECK(st >= 400 && st < 500 && same(&s), m);
    }
    const char *bad_man[] = {
        "relay=0&on=1", "relay=5&on=1", "relay=-1&on=1", "on=1", "relay=1", "relay=1&on=2", "relay=1&on=", "relay=1&on=x",
        "relay=1.0&on=1", "relay=3&on=1" /* not an enabled aux relay */, "relay=4&on=0",
    };
    for (size_t i = 0; i < sizeof(bad_man) / sizeof(bad_man[0]); i++) {
        int st = post_ok(h_manual, bad_man[i]);
        char m[160];
        snprintf(m, sizeof(m), "manual [%s] refused 4xx, no relay write, store untouched", bad_man[i]);
        TEST_CHECK(st >= 400 && st < 500 && same(&s), m);
    }
    /* duplicate keys: the first occurrence is the one honoured; the second must not leak through. */
    take(&s);
    TEST_CHECK(post_ok(h_manual, "relay=1&on=0&relay=3&on=1") == 200, "duplicate relay: first occurrence used");
    TEST_CHECK(f_relay_calls == s.relay_calls + 1 && f_relay_last == 1 && !f_relay_last_on,
               "duplicate keys: exactly one write, to the FIRST relay with the FIRST on value");
    TEST_CHECK(!f_relay_state[3], "duplicate keys: the second relay was never driven");
    take(&s);
    TEST_CHECK(post_ok(h_cfg, "relay=3&enabled=0&relay=4&enabled=1") == 200, "duplicate cfg keys: first used");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x01, "duplicate cfg keys: relay 4 NOT enabled, relay 3 stays disabled");
}

static void test_body_transport(void)
{
    TEST_SECTION("body missing/oversize/truncated, recv errors");
    fresh();
    enable_aux(1);
    snap_t s;
    take(&s);
    esp_err_t (*hs[2])(httpd_req_t *) = { h_cfg, h_manual };
    const char *goods[2] = { "relay=2&enabled=1", "relay=1&on=1" };
    for (int i = 0; i < 2; i++) {
        char big[300];
        memset(big, 'a', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        TEST_CHECK(post(hs[i], goods[i], 0, (size_t)-1, 0) == 400, "content_len 0 -> 400");
        TEST_CHECK(post(hs[i], goods[i], -1, (size_t)-1, 0) == 400, "content_len -1 -> 400");
        TEST_CHECK(post(hs[i], big, (long long)strlen(big), (size_t)-1, 0) == 400, "299-byte body -> 400");
        TEST_CHECK(post(hs[i], goods[i], 128, (size_t)-1, 0) == 400, "content_len == cap (128) -> 400");
        TEST_CHECK(post(hs[i], goods[i], 2147483648LL, (size_t)-1, 0) == 400, "content_len > INT_MAX -> 400");
        TEST_CHECK(post(hs[i], goods[i], (long long)strlen(goods[i]) + 10, (size_t)-1, 0) == 400,
                   "declared length longer than delivered (truncated) -> 400");
        TEST_CHECK(post(hs[i], goods[i], (long long)strlen(goods[i]), 4, 0) == 400, "peer closes mid-body (recv 0) -> 400");
        TEST_CHECK(post(hs[i], goods[i], (long long)strlen(goods[i]), 4, -1) == 400, "recv error (-1) -> 400");
        TEST_CHECK(post(hs[i], goods[i], (long long)strlen(goods[i]), 0, -3) == 400, "recv error at offset 0 -> 400");
        TEST_CHECK(same(&s), "no transport refusal changed the store or drove a relay");
    }
    /* a body cut BEFORE its last byte must not be applied half-way. */
    TEST_CHECK(post(h_cfg, "relay=2&enabled=1", (long long)strlen("relay=2&enabled="), (size_t)-1, 0) >= 400 && same(&s),
               "short declared length drops the trailing byte: refused, nothing stored");
    TEST_CHECK(f_claims == f_releases, "claim/release balanced after transport refusals");
}

static void test_claim_busy_and_quarantine(void)
{
    TEST_SECTION("commissioning claim busy; zone-claimed relay; quarantined store");
    fresh();
    enable_aux(1);
    snap_t s;
    take(&s);
    f_claim_busy = 1;
    TEST_CHECK(post_ok(h_cfg, "relay=2&enabled=1") == 409, "cfg: claim busy -> 409");
    TEST_CHECK(post_ok(h_manual, "relay=1&on=1") == 409, "manual: claim busy -> 409");
    TEST_CHECK(same(&s), "claim-busy refusals change nothing");
    f_claim_busy = 0;
    f_zone_relay_mask[0] = 0x04; /* zone 0 owns relay 3 */
    TEST_CHECK(post_ok(h_cfg, "relay=3&enabled=1") == 409, "aux on a zone-claimed relay -> 409");
    TEST_CHECK(same(&s), "zone-conflict refusal changes nothing");
    TEST_CHECK(post_ok(h_manual, "relay=3&on=1") == 409 && same(&s), "manual on a zone relay -> 409, board untouched");
    s_quarantined = true;
    TEST_CHECK(post_ok(h_cfg, "relay=2&enabled=1") == 409, "quarantined store -> 409");
    TEST_CHECK(s_rev == s.rev, "quarantined store: rev not bumped");
    s_quarantined = false;
}

static void test_success_readback(void)
{
    TEST_SECTION("success path reads back from the real store");
    fresh();
    TEST_CHECK(post_ok(h_cfg, "relay=2&enabled=1&tc_zone=1&hyst_c=3.5&min_on_s=30&min_off_s=45") == 200, "cfg write 200");
    aux_output_t o;
    TEST_CHECK(aux_outputs_cfg_get(2, &o) && o.enabled && o.tc_zone == 1 && o.hyst_c > 3.49f && o.hyst_c < 3.51f &&
                   o.min_on_s == 30 && o.min_off_s == 45,
               "store holds the written fields");
    TEST_CHECK(aux_outputs_cfg_enabled_mask() == 0x02, "enabled mask has exactly relay 2");
    TEST_CHECK(post_ok(h_cfg, "relay=2&enabled=1") == 200, "re-enable without optional fields");
    TEST_CHECK(aux_outputs_cfg_get(2, &o) && o.tc_zone == 1 && o.min_on_s == 30, "omitted fields kept");
    uint8_t persisted_mask = aux_outputs_cfg_enabled_mask();
    memset(s_entries, 0x5A, sizeof(s_entries));
    TEST_CHECK(aux_outputs_cfg_start(0) == ESP_OK && aux_outputs_cfg_enabled_mask() == persisted_mask,
               "reboot: persisted store reproduces the enabled mask");
    int before = f_relay_calls;
    TEST_CHECK(post_ok(h_manual, "relay=2&on=1") == 200 && f_relay_state[2] && f_relay_calls == before + 1,
               "manual on drives relay 2");
    TEST_CHECK(post_ok(h_cfg, "relay=2&enabled=0") == 200, "disable ok");
    TEST_CHECK(!f_relay_state[2], "disabling an aux output drove its relay OFF");
    TEST_CHECK(f_claims == f_releases, "claim/release balanced");
}

static void test_relay_errors_map(void)
{
    TEST_SECTION("relay board errors map to refusals without store change");
    fresh();
    enable_aux(1);
    snap_t s;
    take(&s);
    struct {
        dashboard_relay_result_t r;
        int want;
    } t[] = {
        { DASHBOARD_RELAY_ERR_NO_BOARD, 503 }, { DASHBOARD_RELAY_ERR_RANGE, 400 }, { DASHBOARD_RELAY_ERR_RUNNING, 409 },
        { DASHBOARD_RELAY_ERR_OWNED, 409 },    { DASHBOARD_RELAY_ERR_SAFETY, 403 }, { DASHBOARD_RELAY_ERR_UPDATING, 409 },
        { DASHBOARD_RELAY_ERR_CRASH_UNACK, 409 },
    };
    for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        f_relay_result = t[i].r;
        char m[96];
        snprintf(m, sizeof(m), "relay result %d maps to %d", (int)t[i].r, t[i].want);
        TEST_CHECK(post_ok(h_manual, "relay=1&on=1") == t[i].want, m);
        TEST_CHECK(!f_relay_state[1] && aux_outputs_cfg_enabled_mask() == s.enabled, "refused relay write: state unchanged");
    }
}

int main(void)
{
    g_test_stub_semaphore_take_default = 1;
    test_registration_and_tier();
    test_mode_gate();
    test_indices_and_fields();
    test_body_transport();
    test_claim_busy_and_quarantine();
    test_success_readback();
    test_relay_errors_map();
    printf("\n%d/%d checks passed\n", g_test_count - g_test_failures, g_test_count);
    if (g_test_failures > 0) {
        printf("%d FAILURE(S)\n", g_test_failures);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
