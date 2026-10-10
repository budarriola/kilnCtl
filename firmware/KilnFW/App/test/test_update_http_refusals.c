// Host test, campaign 9c: gate-refusal and failure paths of the update_*_http handlers
// (docs/audits/HOST_TEST_COVERAGE_GAPS_2026-10-09.md item 9). Builds on test_update_fetch.c (#included, so the
// REAL update_fetch.c / update_http.c / update_stage.c run over its fakes) and adds the REAL
// update_settings_http.c. The new cases run through extra_tests(), called by the included main() BEFORE
// its permanent writer-wedge case.
//
// Asserted for every refusal: status line, body, that nothing was staged or written to flash, and that the
// update claim / fetch busy flag were released (a leak would make the next request fail 409).
// Auth tier is not a handler property (route_tier_table.h + the dispatcher gate it; guarded by
// check_route_tier*), and the recovery image has its own, separate handler set: noted, not tested here.
#define main uf_main
#define UF_EXTRA_TESTS 1
static void extra_tests(void);
#include "test_update_fetch.c"
#undef main

#include "update_settings.h"
#include "update_settings_http.h"

// ---- stubs the real update_settings_http.c needs --------------------------------------------------------
static bool g_cfg_mounted = true;
static bool g_cfg_recovery = false;
bool cfg_fs_is_available(void) { return g_cfg_mounted; }
bool cfg_fs_skipped_for_recovery(void) { return g_cfg_recovery; }

esp_err_t httpd_resp_send_500(httpd_req_t *r)
{
    return httpd_resp_set_status(r, "500 Internal Server Error");
}

static int g_set_calls;
static char g_set_last[128];
static esp_err_t g_set_result;
esp_err_t update_settings_set(const char *repo)
{
    g_set_calls++;
    snprintf(g_set_last, sizeof(g_set_last), "%s", repo);
    if (g_set_result == ESP_OK) {
        snprintf(g_fr_repo, sizeof(g_fr_repo), "%s", repo);
    }
    return g_set_result;
}

static int post_settings(const char *body, size_t len_override)
{
    fr_req_init(&g_rq, HTTP_POST);
    fr_req_body(&g_rq, (const uint8_t *)body, strlen(body));
    if (len_override != 0) {
        g_rq.base.content_len = (long long)len_override;
    }
    httpd_uri_t h = fr_find_handler("/api/update/settings", HTTP_POST);
    CHECK(h.handler != NULL, "settings POST route registered");
    if (h.handler == NULL) {
        return -1;
    }
    h.handler(&g_rq.base);
    return fr_status_code(&g_rq);
}

static int get_json(const char *uri)
{
    return do_req(uri, HTTP_GET, NULL);
}

static void test_settings_http(void)
{
    CHECK(update_settings_http_start() == ESP_OK, "settings routes register");
    CHECK(fr_find_handler("/api/update/settings", HTTP_GET).handler != NULL, "settings GET route");
    CHECK(fr_find_handler("/api/update/settings", HTTP_POST).handler != NULL, "settings POST route");
    fr_reset();
    g_cfg_mounted = true;
    g_cfg_recovery = false;
    g_set_calls = 0;
    g_set_result = ESP_OK;

    // GET
    CHECK(get_json("/api/update/settings") == 200, "settings GET -> 200");
    CHECK(resp_has("\"ok\":true") && resp_has("\"repo\":\"budarriola/kilnCtl\"") && resp_has("\"is_default\":true"),
          "settings GET body names the default repo");
    strcpy(g_fr_repo, "alice/proj");
    get_json("/api/update/settings");
    CHECK(resp_has("\"repo\":\"alice/proj\"") && resp_has("\"is_default\":false"), "settings GET reports non-default");
    g_fr_repo[0] = 0; // update_settings_repo_copy() fails
    CHECK(get_json("/api/update/settings") == 500, "settings GET: repo unreadable -> 500, never a fabricated repo");
    CHECK(!resp_has("\"ok\":true"), "unreadable repo is not reported as ok");
    strcpy(g_fr_repo, "budarriola/kilnCtl");

    // cfg unmounted: refused before anything is parsed or applied
    g_cfg_mounted = false;
    CHECK(post_settings("repo=a%2Fb", 0) == 503, "settings POST: cfg unmounted -> 503");
    CHECK(resp_has("\"ok\":false"), "unmounted body is a failure");
    CHECK(g_set_calls == 0, "unmounted: update_settings_set never called");
    g_cfg_recovery = true;
    CHECK(post_settings("repo=a%2Fb", 0) == 503, "settings POST: recovery-skipped cfg -> 503");
    CHECK(g_set_calls == 0, "recovery-skipped cfg: update_settings_set never called");
    g_cfg_recovery = false;
    g_cfg_mounted = true;

    // body length gates
    CHECK(post_settings("", 0) == 400, "settings POST: empty body -> 400");
    CHECK(strstr(g_rq.status, "400") != NULL && resp_has("body missing or too large"), "empty body message");
    static char big[400];
    memset(big, 'a', sizeof(big) - 1);
    big[sizeof(big) - 1] = 0;
    CHECK(post_settings(big, 0) == 400 && resp_has("body missing or too large"), "settings POST: oversize body -> 400");
    CHECK(g_set_calls == 0, "length refusals never reach update_settings_set");
    // short read: content_len promises more than the socket delivers
    fr_req_init(&g_rq, HTTP_POST);
    fr_req_body(&g_rq, (const uint8_t *)"repo=a", 6);
    g_rq.base.content_len = 20;
    fr_find_handler("/api/update/settings", HTTP_POST).handler(&g_rq.base);
    CHECK(fr_status_code(&g_rq) == 400 && resp_has("body read failed"), "settings POST: short body -> 400 body read failed");
    CHECK(g_set_calls == 0, "short read never reaches update_settings_set");

    // mode gate (409) after the body drains; shadows a malformed field
    g_fr_heat_profile = true;
    CHECK(post_settings("repo=a%2Fb", 0) == 409, "settings POST: firing -> 409");
    CHECK(post_settings("nonsense=1", 0) == 409, "settings POST: firing shadows the missing-field 400");
    g_fr_heat_profile = false;
    g_fr_heat_autotune = true;
    CHECK(post_settings("repo=a%2Fb", 0) == 409, "settings POST: autotune -> 409");
    g_fr_heat_autotune = false;
    CHECK(g_set_calls == 0, "mode-gate refusals never reach update_settings_set");
    CHECK(strcmp(g_fr_repo, "budarriola/kilnCtl") == 0, "refusals left the repo unchanged");

    // field gates
    CHECK(post_settings("other=1", 0) == 400 && resp_has("missing \"repo\" field"), "settings POST: no repo field -> 400");
    char longrepo[128];
    memset(longrepo, 'a', sizeof(longrepo));
    memcpy(longrepo, "repo=", 5);
    longrepo[100] = 0;
    CHECK(post_settings(longrepo, 0) == 400 && resp_has("invalid repo"), "settings POST: over-long repo -> 400 invalid repo");
    CHECK(post_settings("repo=ab%00c%2Fd", 0) == 400 && resp_has("invalid repo"), "settings POST: embedded NUL -> 400");
    CHECK(g_set_calls == 0, "field refusals never reach update_settings_set");
    g_set_result = ESP_ERR_INVALID_ARG;
    CHECK(post_settings("repo=notarepo", 0) == 400 && resp_has("invalid repo"), "settings POST: validator rejects -> 400");
    CHECK(g_set_calls == 1, "validator refusal came from update_settings_set");
    CHECK(strcmp(g_fr_repo, "budarriola/kilnCtl") == 0, "rejected repo not applied");

    // persist failure is never reported as success
    g_set_result = ESP_FAIL;
    CHECK(post_settings("repo=alice%2Fproj", 0) == 500, "settings POST: persist failure -> 500");
    CHECK(resp_has("could not be saved to flash") && !resp_has("\"ok\":true"), "persist failure body is a failure");

    // success echoes the new state
    g_set_result = ESP_OK;
    g_set_calls = 0;
    CHECK(post_settings("repo=alice%2Fproj", 0) == 200, "settings POST: valid repo -> 200");
    CHECK(strcmp(g_set_last, "alice/proj") == 0 && g_set_calls == 1, "decoded repo passed to update_settings_set once");
    CHECK(resp_has("\"repo\":\"alice/proj\"") && resp_has("\"is_default\":false"), "success body is the new setting");
    strcpy(g_fr_repo, "budarriola/kilnCtl");
}

// ---- update_fetch.c routes: status, cancel, flags -------------------------------------------------------
static void test_fetch_routes_extra(void)
{
    fixture(g_sha_hex, IMG_LEN);
    // status GET: allocation failure answers 500, never a truncated body
    g_fr_alloc_fail_all = true;
    CHECK(get_json("/api/update/fetch") == 500 && resp_has("out of memory"), "fetch status: OOM -> 500");
    g_fr_alloc_fail_all = false;
    CHECK(get_json("/api/update/fetch") == 200 && resp_has("\"ok\":true") && resp_has("\"busy\":false"),
          "fetch status (idle job) -> 200");
    const long live = g_fr_heap_live;
    get_json("/api/update/fetch");
    CHECK(g_fr_heap_live == live, "fetch status frees its buffer on the success path");

    // download refusals shadow each other in the documented order; none leaks the busy flag or the claim
    CHECK(POST("/api/update/download", "allow_downgrade=1&confirm_downgrade=") == 400 &&
              resp_has("bad_confirm"),
          "download: empty confirm_downgrade tag -> 400 bad_confirm");
    CHECK(g_fr_claim_begin_n == 0 && g_fr_open_count == 0, "bad_confirm: no claim, no job");
    g_fr_clock_synced = false;
    g_fr_heat_profile = true;
    CHECK(POST("/api/update/download", NULL) == 409 && resp_has("clock_not_synced"),
          "download: clock gate precedes the mode gate");
    g_fr_clock_synced = true;
    CHECK(POST("/api/update/download", NULL) == 409 && !resp_has("clock_not_synced"), "download: firing -> 409 mode gate");
    CHECK(g_fr_claim_begin_n == 0, "mode-gate refusal precedes the claim");
    g_fr_heat_profile = false;
    g_fr_interlock_result = 1;
    g_fr_heat_autotune = true;
    CHECK(POST("/api/update/download", NULL) == 409, "download: autotune and interlock together -> 409");
    CHECK(g_fr_claim_begin_n == 0, "mode gate shadows interlock: still no claim");
    g_fr_heat_autotune = false;
    g_fr_interlock_result = 0;
    g_fr_claim_deny = true;
    CHECK(POST("/api/update/download", NULL) == 409, "download: claim busy -> 409");
    CHECK(g_fr_open_count == 0, "claim refusal starts no network job");
    g_fr_claim_deny = false;
    CHECK(get_json("/api/update/fetch") == 200 && resp_has("\"busy\":false"), "no refusal left the fetch job busy");
    CHECK(!g_fr_claim_held && g_fr_claim_end_n == g_fr_claim_begin_n, "claims balanced after refusals");

    // a check never touches the claim even when the OTA interlock / claim would refuse a download
    g_fr_interlock_result = 1;
    g_fr_claim_deny = true;
    CHECK(POST("/api/update/check", NULL) == 202, "check ignores interlock and a held claim");
    wait_idle();
    g_fr_interlock_result = 0;
    g_fr_claim_deny = false;
    CHECK(g_fr_claim_begin_n == 0, "check never takes the claim");

    // flags are read from the query; an unknown query key is not an error and starts a normal job
    CHECK(POST("/api/update/check", "allow_prerelease=1&bogus=2") == 202, "check with extra query key -> 202");
    wait_idle();
}

// ---- update_http.c stage routes -------------------------------------------------------------------------
static void test_stage_routes_extra(void)
{
    fixture(g_sha_hex, IMG_LEN);
    CHECK(get_json("/api/update/stage") == 200 && resp_has("\"staged\":false"), "stage status empty -> 200 staged:false");

    // refusals: body names the cause; nothing erased or written; claim released
    int er = g_fr_flash_erase_n, wr = g_fr_flash_write_n;
    g_fr_claim_deny = true;
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 409 && resp_has("update_in_progress"), "upload: claim busy -> 409 update_in_progress");
    CHECK(POST("/api/update/stage/clear", NULL) == 409 && resp_has("update_in_progress"),
          "clear: claim busy -> 409 update_in_progress");
    g_fr_claim_deny = false;
    g_fr_interlock_result = 1;
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 409 && resp_has("fake interlock"), "upload: interlock reason in body");
    g_fr_interlock_result = 0;
    g_fr_heat_autotune = true;
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 409, "upload: autotune -> 409");
    CHECK(POST("/api/update/stage/clear", NULL) == 409, "clear: autotune -> 409");
    g_fr_heat_autotune = false;
    CHECK(g_fr_flash_erase_n == er && g_fr_flash_write_n == wr, "refusals touched no flash");
    CHECK(g_fr_claim_begin_n == g_fr_claim_end_n && !g_fr_claim_held, "refusals leave the claim balanced");

    // a refused upload while a fetch job is busy: the fetch busy probe, not the claim, guards heat; the
    // upload itself is still refused by the claim the job holds
    fixture(g_sha_hex, IMG_LEN);
    g_hold_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_fr_routes[0].on_read = hold_job;
    CHECK(POST("/api/update/download", NULL) == 202, "download parked holding the claim");
    CHECK(g_fr_claim_held, "the running download holds the update claim");
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 409 && resp_has("update_in_progress"), "upload while a download holds the claim -> 409");
    CHECK(POST("/api/update/stage/clear", NULL) == 409, "clear while a download holds the claim -> 409");
    CHECK(g_fr_claim_held, "refused upload/clear did not release the DOWNLOAD's claim");
    CHECK(POST("/api/update/fetch/cancel", NULL) == 200, "cancel the parked download");
    SetEvent(g_hold_ev);
    wait_idle();
    expect_claim_balanced("download parked then cancelled");
    CloseHandle(g_hold_ev);
    CHECK(!stage_staged(), "nothing staged by the cancelled download");

    // content_len gates
    fixture(g_sha_hex, IMG_LEN);
    upload(g_img, 0);
    int code = run_upload();
    CHECK(code >= 400 && code < 500, "upload: zero-length body -> 4xx");
    CHECK(!stage_staged() && stage_phase() == UPDATE_STAGE_IDLE, "zero-length upload staged nothing");
    expect_claim_balanced("zero-length upload");
    upload(g_img, IMG_LEN);
    g_rq.base.content_len = 100 * 1024 * 1024;
    CHECK(run_upload() == 413, "upload: oversize content_len -> 413");
    CHECK(!stage_staged() && stage_phase() == UPDATE_STAGE_IDLE, "oversize upload staged nothing");
    expect_claim_balanced("oversize upload");

    // malformed advisory schema headers: 400, before any image byte is written
    const char *hdrs[] = { "X-Stage-Zones-Cfg", "X-Stage-Kilnlink", "X-Stage-Uart" };
    for (size_t i = 0; i < 3; i++) {
        upload(g_img, IMG_LEN);
        fr_req_header(&g_rq, hdrs[i], "12abc");
        int w = g_fr_flash_write_n;
        char m[96];
        code = run_upload();
        snprintf(m, sizeof(m), "%s malformed -> 400 bad_schema_header", hdrs[i]);
        CHECK(code == 400 && resp_has("bad_schema_header"), m);
        CHECK(!stage_staged() && stage_phase() == UPDATE_STAGE_IDLE, "malformed schema header staged nothing");
        CHECK(g_fr_flash_write_n == w, "malformed schema header wrote no image bytes");
        expect_claim_balanced(hdrs[i]);
    }

    // downgrade: the candidate's own version (app descriptor) older than the running 1.0.0
    static uint8_t old_img[IMG_LEN];
    memcpy(old_img, g_img, IMG_LEN);
    memset(old_img + 48, 0, 32);
    memcpy(old_img + 48, "0.5.0", 5);
    int e0 = g_fr_flash_erase_n;
    upload(old_img, IMG_LEN);
    code = run_upload();
    CHECK(code == 409 && resp_has("\"ok\":false") && resp_has("candidate_version"), "downgrade upload -> 409 policy body");
    CHECK(!stage_staged() && stage_phase() == UPDATE_STAGE_IDLE, "refused downgrade staged nothing");
    (void)e0;
    expect_claim_balanced("downgrade upload");

    // status GET reports a staged image, and clear removes it
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 200, "good upload still stages after the refusals");
    CHECK(get_json("/api/update/stage") == 200 && resp_has("\"staged\":true"), "stage status after upload staged:true");
    CHECK(POST("/api/update/stage/clear", NULL) == 200 && resp_has("\"staged\":false"), "clear -> 200");
    CHECK(get_json("/api/update/stage") == 200 && resp_has("\"staged\":false"), "stage status empty again");
    expect_claim_balanced("upload then clear");

    // clear failure keeps the stage and reports the flash error
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 200, "restage");
    g_fr_flash_write_fail = true;
    code = POST("/api/update/stage/clear", NULL);
    CHECK(code >= 500 && resp_has("\"ok\":false"), "clear flash failure -> 5xx failure body");
    g_fr_flash_write_fail = false;
    expect_claim_balanced("clear failure");
    CHECK(POST("/api/update/stage/clear", NULL) == 200, "clear retry succeeds (claim was released)");
}

static void extra_tests(void)
{
    test_settings_http();
    test_fetch_routes_extra();
    test_stage_routes_extra();
}

int main(void)
{
    return uf_main();
}
