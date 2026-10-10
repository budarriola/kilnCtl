// Host test: the REAL update_fetch.c (GitHub release check/download job, its flash-writer task and the four
// routes) and update_http.c (manual stage upload/clear routes), over update_stage.c on an in-memory stage
// partition. Faked (fake_support.c): the HTTP client (scripted routes), psa sha256, FreeRTOS tasks/semaphores
// (Windows threads, so the TLS task and the writer task really interleave), the update claim, the interlock,
// the heat-run probe, the clock and httpd. docs/audits/HOST_TEST_GAP_AUDIT_2026-10-09.md gap 1.
//
// Order matters: the writer-wedge case is LAST because a wedged writer is permanent until reboot.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>

#include "fake_support.h"

#include "kilnlink/kilnlink_version.h"
#include "uart_task_ids.h"
#include "update_http.h"
#include "update_http_internal.h"
#include "update_policy.h"
#include "update_stage.h"
#include "zones_config_json.h"
#include "psa/crypto.h"

static int g_fail = 0, g_checks = 0;
#define CHECK(cond, msg)                                                                                         \
    do {                                                                                                         \
        g_checks++;                                                                                              \
        if (!(cond)) {                                                                                           \
            g_fail++;                                                                                            \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                                 \
        }                                                                                                        \
    } while (0)

#define REPO "budarriola/kilnCtl"
#define BASE "https://github.com/budarriola/kilnCtl/releases/download/"
#define API_URL "https://api.github.com/repos/" REPO "/releases/latest"
#define MAN_URL BASE "v1.2.3/release.json"
#define APP_URL BASE "v1.2.3/KilnCtrl-v1.2.3.bin"
#define COMMIT "0123456789abcdef0123456789abcdef01234567"
#define IMG_LEN 20000u

static uint8_t g_img[IMG_LEN];
static char g_api[2048], g_man[2048];
static char g_sha_hex[65];

static void sha_hex_of(const uint8_t *d, size_t n, char *out)
{
    psa_hash_operation_t op = psa_hash_operation_init();
    uint8_t dg[32];
    size_t dl = 0;
    psa_hash_setup(&op, PSA_ALG_SHA_256);
    psa_hash_update(&op, d, n);
    psa_hash_finish(&op, dg, sizeof(dg), &dl);
    for (int i = 0; i < 32; i++) {
        sprintf(out + 2 * i, "%02x", dg[i]);
    }
}

static void make_image(void)
{
    uint32_t x = 0x12345678u;
    for (size_t i = 0; i < IMG_LEN; i++) {
        x = x * 1664525u + 1013904223u;
        g_img[i] = (uint8_t)(x >> 24);
    }
    g_img[0] = 0xE9;
    g_img[12] = 9;
    g_img[13] = 0;
    uint32_t m = 0xABCD5432u;
    memcpy(g_img + 32, &m, 4);
    memset(g_img + 48, 0, 32);
    memcpy(g_img + 48, "v1.2.3", 6);
    memset(g_img + 80, 0, 32);
    memcpy(g_img + 80, "KilnCtrl", 8);
    update_image_id_t id;
    update_image_id_make(&id, ZONES_CFG_VERSION, KILNLINK_PROTOCOL_VERSION, UART_PROTOCOL_VERSION, "0123456");
    memcpy(g_img + UPDATE_STAGE_IMAGE_ID_FROM, &id, sizeof(id));
}

// API/manifest JSON for the image above; `sha` is what the manifest claims the asset hashes to.
static void make_docs(const char *sha, uint32_t api_size)
{
    snprintf(g_api, sizeof(g_api),
             "{\"tag_name\":\"v1.2.3\",\"draft\":false,\"prerelease\":false,\"body\":\"n\",\"assets\":["
             "{\"name\":\"KilnCtrl-v1.2.3.bin\",\"size\":%u,\"browser_download_url\":\"" APP_URL "\"},"
             "{\"name\":\"release.json\",\"size\":900,\"browser_download_url\":\"" MAN_URL "\"}]}",
             (unsigned)api_size);
    snprintf(g_man, sizeof(g_man),
             "{\"schema\":1,\"tag\":\"v1.2.3\",\"channel\":\"stable\",\"published\":\"2026-10-05T00:00:00Z\","
             "\"repo\":\"" REPO "\",\"commit\":\"" COMMIT "\",\"dirty\":false,\"build_date\":\"x\","
             "\"compat\":{\"zones_cfg_version\":%d,\"kilnlink_version\":%d,\"uart_version\":%d,"
             "\"partitions_sha256\":\"%s\"},"
             "\"images\":[{\"name\":\"app\",\"file\":\"KilnCtrl-v1.2.3.bin\",\"size\":%u,\"sha256\":\"%s\","
             "\"includes\":[\"pico_slotA\"]},"
             "{\"name\":\"recovery\",\"file\":\"KilnRecovery-v1.2.3.bin\",\"size\":99,\"sha256\":\"%s\","
             "\"apply\":\"jtag_only\"}],"
             "\"notes_url\":\"https://github.com/budarriola/kilnCtl/releases/tag/v1.2.3\"}",
             (int)ZONES_CFG_VERSION, (int)KILNLINK_PROTOCOL_VERSION, (int)UART_PROTOCOL_VERSION,
             "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff", (unsigned)api_size, sha, sha);
}

static fr_route_t *add_route(const char *url, const void *body, size_t len)
{
    fr_route_t *r = &g_fr_routes[g_fr_route_n++];
    memset(r, 0, sizeof(*r));
    r->url = url;
    r->status = 200;
    r->body = body;
    r->body_len = len;
    r->advertised = -2;
    r->deliver = FR_ALL;
    return r;
}

// Fresh boot-equivalent state plus the three routes of a good v1.2.3 release.
static fr_route_t *g_app_route;
static void fixture(const char *sha, uint32_t api_size)
{
    fr_reset();
    make_docs(sha, api_size);
    add_route(API_URL, g_api, strlen(g_api));
    add_route(MAN_URL, g_man, strlen(g_man));
    g_app_route = add_route(APP_URL, g_img, IMG_LEN);
}

static fake_req_t g_rq;
static int do_req(const char *uri, int method, const char *query)
{
    fr_req_init(&g_rq, method);
    if (query != NULL) {
        snprintf(g_rq.query, sizeof(g_rq.query), "%s", query);
    }
    httpd_uri_t h = fr_find_handler(uri, method);
    CHECK(h.handler != NULL, "route registered");
    if (h.handler == NULL) {
        return -1;
    }
    h.handler(&g_rq.base);
    return fr_status_code(&g_rq);
}
#define POST(uri, q) do_req(uri, HTTP_POST, q)

static DWORD g_t0;
#define WAIT_FOR(cond) for (g_t0 = GetTickCount(); !(cond) && GetTickCount() - g_t0 < 5000;) Sleep(2)

static bool resp_has(const char *s) { return strstr(g_rq.resp, s) != NULL; }

static void wait_idle(void)
{
    CHECK(fr_wait_tasks_idle(15000), "fetch/writer tasks finished");
}

// state/error of the last job, via the real GET /api/update/fetch route.
static char g_state[24], g_error[48];
static void job_result(void)
{
    do_req("/api/update/fetch", HTTP_GET, NULL);
    g_state[0] = g_error[0] = 0;
    const char *s = strstr(g_rq.resp, "\"state\":\"");
    if (s) sscanf(s + 9, "%23[^\"]", g_state);
    const char *e = strstr(g_rq.resp, "\"error\":\"");
    if (e) sscanf(e + 9, "%47[^\"]", g_error);
}

static update_stage_info_t stage_info(void)
{
    static uint8_t scratch[8192];
    update_stage_info_t info;
    memset(&info, 0, sizeof(info));
    (void)update_stage_get_status(update_http_stage(), scratch, sizeof(scratch), &info);
    return info;
}
static bool stage_staged(void) { return stage_info().staged; }
static update_stage_phase_t stage_phase(void) { return stage_info().phase; }

static void expect_claim_balanced(const char *what)
{
    char m[96];
    snprintf(m, sizeof(m), "%s: update claim released", what);
    CHECK(!g_fr_claim_held, m);
    snprintf(m, sizeof(m), "%s: every claim begun was ended", what);
    CHECK(g_fr_claim_end_n == g_fr_claim_begin_n, m);
}

// ---------------------------------------------------------------------------------------------------------
static void test_check_happy(void)
{
    fixture(g_sha_hex, IMG_LEN);
    CHECK(POST("/api/update/check", NULL) == 202, "check accepted");
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "done") == 0, "check job done");
    CHECK(g_fr_claim_begin_n == 0, "a check never takes the update claim");
    CHECK(g_fr_flash_erase_n == 0 && g_fr_flash_write_n == 0, "a check touches no flash");
    CHECK(!stage_staged(), "check stages nothing");
}

static void test_download_happy(void)
{
    fixture(g_sha_hex, IMG_LEN);
    CHECK(POST("/api/update/download", NULL) == 202, "download accepted");
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "done") == 0, "download job done");
    CHECK(stage_staged(), "image staged and verified");
    CHECK(g_fr_claim_begin_n == 1, "download took the claim once");
    expect_claim_balanced("happy download");
}

// Gap 1: a body that ends before the advertised Content-Length must abort the stage, not finish it.
static void test_short_body_http_layer(void)
{
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->deliver = IMG_LEN / 2; // connection ends half way; advertised stays IMG_LEN
    CHECK(POST("/api/update/download", NULL) == 202, "download accepted");
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "short body fails the job");
    CHECK(strcmp(g_error, "short_body") == 0, "error is short_body (HTTP layer)");
    CHECK(!stage_staged(), "short body leaves nothing staged");
    CHECK(stage_phase() == UPDATE_STAGE_IDLE, "short body aborted the upload (stage idle)");
    expect_claim_balanced("short_body");
}

// Same error name from run_job's own byte count: a chunked response has no Content-Length to compare.
static void test_short_body_chunked(void)
{
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->advertised = -1;
    g_app_route->body_len = IMG_LEN - 100; // complete chunked body, fewer bytes than the API's asset size
    CHECK(POST("/api/update/download", NULL) == 202, "download accepted");
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "chunked short body fails the job");
    CHECK(strcmp(g_error, "short_body") == 0, "error is short_body (byte count)");
    CHECK(!stage_staged(), "nothing staged");
    CHECK(stage_phase() == UPDATE_STAGE_IDLE, "stage aborted");
    expect_claim_balanced("short_body chunked");
}

static void test_sha_mismatch(void)
{
    char wrong[65];
    memset(wrong, 'a', 64);
    wrong[64] = 0;
    fixture(wrong, IMG_LEN);
    CHECK(POST("/api/update/download", NULL) == 202, "download accepted");
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "hash mismatch fails the job");
    CHECK(strcmp(g_error, "sha256_mismatch") == 0, "error is sha256_mismatch");
    CHECK(!stage_staged(), "a hash mismatch never stages");
    CHECK(stage_phase() == UPDATE_STAGE_IDLE, "stage aborted before WR_FINISH");
    expect_claim_balanced("sha256_mismatch");
    // the next, good job still works: the failure left nothing wedged
    fixture(g_sha_hex, IMG_LEN);
    CHECK(POST("/api/update/download", NULL) == 202, "good download after a failed one");
    wait_idle();
    CHECK(stage_staged(), "good download after failure stages");
}

static void test_claim_released_on_failures(void)
{
    fixture(g_sha_hex, IMG_LEN);
    g_fr_fail_task = "update_fetch";
    CHECK(POST("/api/update/download", NULL) == 500, "task create failure answers 500");
    CHECK(resp_has("task_create_failed"), "task_create_failed body");
    expect_claim_balanced("task_create_failed");
    g_fr_fail_task = NULL;
    CHECK(POST("/api/update/download", NULL) == 202, "busy flag was released: next download starts");
    wait_idle();

    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 404;
    POST("/api/update/download", NULL);
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "404 API reply fails the job");
    expect_claim_balanced("http_status");

    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[2].open_fail = true;
    POST("/api/update/download", NULL);
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "asset connect failure fails the job");
    CHECK(stage_phase() == UPDATE_STAGE_IDLE, "stage idle after connect failure");
    expect_claim_balanced("connect_failed");

    fixture(g_sha_hex, IMG_LEN);
    g_fake_heap_free_internal = 1000;
    POST("/api/update/download", NULL);
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "low heap fails the job");
    expect_claim_balanced("low_heap");

    fixture(g_sha_hex, IMG_LEN);
    g_fr_repo[0] = 0;
    POST("/api/update/download", NULL);
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "bad repo fails the job");
    expect_claim_balanced("bad_repo");

    fixture(g_sha_hex, IMG_LEN);
    g_fr_flash_write_fail = true;
    POST("/api/update/download", NULL);
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "flash failure fails the job");
    CHECK(!stage_staged(), "nothing staged after a flash failure");
    expect_claim_balanced("flash failure");
    g_fr_flash_write_fail = false;
}

static HANDLE g_hold_ev;
static void hold_job(void) { WaitForSingleObject(g_hold_ev, 20000); }

static void test_handler_refusals(void)
{
    fixture(g_sha_hex, IMG_LEN);
    char longq[256];
    memset(longq, 'a', sizeof(longq) - 1);
    longq[sizeof(longq) - 1] = 0;
    CHECK(POST("/api/update/check", longq) == 400 && resp_has("bad_query"), "check: oversized query -> 400");
    CHECK(POST("/api/update/download", longq) == 400 && resp_has("bad_query"), "download: oversized query -> 400");
    CHECK(POST("/api/update/download", "confirm_downgrade=bad%20tag") == 400 && resp_has("bad_confirm"),
          "download: invalid confirm tag -> 400 bad_confirm");
    CHECK(g_fr_claim_begin_n == 0, "400s never reach the claim");
    CHECK(g_fr_open_count == 0, "400s start no job");

    g_fr_clock_synced = false;
    CHECK(POST("/api/update/check", NULL) == 409 && resp_has("clock_not_synced"), "check: clock not synced -> 409");
    CHECK(POST("/api/update/download", NULL) == 409 && resp_has("clock_not_synced"), "download: clock -> 409");
    CHECK(g_fr_claim_begin_n == 0, "clock refusal is before the claim");
    g_fr_clock_synced = true;
    CHECK(POST("/api/update/check", NULL) == 202, "busy flag was released by the clock refusal");
    wait_idle();

    g_fr_heat_autotune = true;
    CHECK(POST("/api/update/check", NULL) == 409, "check: autotune running -> 409");
    CHECK(POST("/api/update/download", NULL) == 409, "download: autotune running -> 409");
    g_fr_heat_autotune = false;
    CHECK(POST("/api/update/check", NULL) == 202, "busy flag released by the mode-gate refusal");
    wait_idle();

    g_fr_interlock_result = 1;
    CHECK(POST("/api/update/download", NULL) == 409, "download: OTA interlock refused -> 409");
    g_fr_interlock_result = 0;
    CHECK(!g_fr_claim_held, "interlock refusal leaves no claim");
    g_fr_claim_deny = true;
    CHECK(POST("/api/update/download", NULL) == 409, "download: claim taken -> 409");
    g_fr_claim_deny = false;
    CHECK(!g_fr_claim_held, "claim refusal leaves no claim held");
    CHECK(POST("/api/update/download", NULL) == 202, "busy flag released by interlock/claim refusals");
    wait_idle();

    fixture(g_sha_hex, IMG_LEN);
    g_hold_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    g_fr_routes[0].on_read = hold_job;
    CHECK(POST("/api/update/download", NULL) == 202, "job started and parked in its first read");
    CHECK(POST("/api/update/check", NULL) == 409 && resp_has("fetch_busy"), "check while busy -> 409 fetch_busy");
    CHECK(POST("/api/update/download", NULL) == 409 && resp_has("fetch_busy"), "download while busy -> 409");
    CHECK(POST("/api/update/fetch/cancel", NULL) == 200 && resp_has("\"cancelling\":true"), "cancel while busy");
    SetEvent(g_hold_ev);
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "cancelled job failed");
    CHECK(!stage_staged(), "cancelled job staged nothing");
    expect_claim_balanced("cancelled job");
    CHECK(POST("/api/update/fetch/cancel", NULL) == 200 && resp_has("\"cancelling\":false"), "cancel when idle");
    CloseHandle(g_hold_ev);
}

// ---------------------------------------------------------------------------------------------------------
// update_http.c: manual stage routes
static void upload(const uint8_t *body, size_t len)
{
    fr_req_init(&g_rq, HTTP_POST);
    fr_req_body(&g_rq, body, len);
}
static int run_upload(void)
{
    httpd_uri_t h = fr_find_handler("/api/update/stage", HTTP_POST);
    CHECK(h.handler != NULL, "stage upload route registered");
    h.handler(&g_rq.base);
    return fr_status_code(&g_rq);
}

static void test_stage_upload_and_clear(void)
{
    fixture(g_sha_hex, IMG_LEN);
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 200, "upload of a good image -> 200");
    CHECK(stage_staged(), "stage holds the upload");
    expect_claim_balanced("good upload");

    g_fr_flash_write_fail = true;
    CHECK(POST("/api/update/stage/clear", NULL) >= 500, "clear: flash failure -> 5xx");
    expect_claim_balanced("failed clear");
    g_fr_flash_write_fail = false;
    CHECK(POST("/api/update/stage/clear", NULL) == 200, "clear -> 200");
    CHECK(!stage_staged(), "clear emptied the stage");
    expect_claim_balanced("clear");

    int b = g_fr_claim_begin_n;
    g_fr_heat_profile = true;
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 409, "upload: firing -> 409");
    CHECK(POST("/api/update/stage/clear", NULL) == 409, "clear: firing -> 409");
    g_fr_heat_profile = false;
    CHECK(g_fr_claim_begin_n == b, "mode-gate refusal is before the claim");
    g_fr_interlock_result = 1;
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 409, "upload: interlock -> 409");
    CHECK(POST("/api/update/stage/clear", NULL) == 409, "clear: interlock -> 409");
    g_fr_interlock_result = 0;
    g_fr_claim_deny = true;
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 409, "upload: claim taken -> 409");
    CHECK(POST("/api/update/stage/clear", NULL) == 409, "clear: claim taken -> 409");
    g_fr_claim_deny = false;
    CHECK(!g_fr_claim_held, "no claim left held");
    CHECK(!stage_staged(), "refused uploads staged nothing");

    upload(g_img, IMG_LEN);
    fr_req_header(&g_rq, "X-Stage-Version", "1234567890123456789012345678901234567890");
    CHECK(run_upload() == 400 && resp_has("bad_version"), "over-long X-Stage-Version -> 400 bad_version");
    expect_claim_balanced("bad_version");
    upload(g_img, IMG_LEN);
    fr_req_header(&g_rq, "X-Stage-Commit", "0123456789012345678901234567890123456789012345678901234567890");
    CHECK(run_upload() == 400 && resp_has("bad_commit"), "over-long X-Stage-Commit -> 400 bad_commit");
    expect_claim_balanced("bad_commit");

    upload(g_img, IMG_LEN);
    g_rq.recv_fail_at = 8000;
    run_upload();
    CHECK(stage_phase() == UPDATE_STAGE_IDLE, "recv failure aborted the upload");
    CHECK(!stage_staged(), "recv failure staged nothing");
    expect_claim_balanced("recv failure");

    static uint8_t bad[IMG_LEN];
    memcpy(bad, g_img, IMG_LEN);
    bad[0] = 0x00;
    upload(bad, IMG_LEN);
    int code = run_upload();
    CHECK(code >= 400 && code < 500, "bad image -> 4xx");
    CHECK(!stage_staged() && stage_phase() == UPDATE_STAGE_IDLE, "bad image staged nothing, stage idle");
    expect_claim_balanced("bad image");
}

// ---------------------------------------------------------------------------------------------------------
// LAST: a flash-writer op that never returns. Permanent until reboot, so nothing may follow.
static void test_writer_wedge(void)
{
    fixture(g_sha_hex, IMG_LEN);
    CHECK(!update_fetch_writer_wedged(), "writer not wedged before the case");
    g_fr_time_div = 1000; // the writer wait becomes milliseconds
    g_fr_flash_write_block = true;
    CHECK(POST("/api/update/download", NULL) == 202, "download accepted");
    WAIT_FOR(update_fetch_writer_wedged());
    CHECK(update_fetch_writer_wedged(), "a timed-out writer op marks the writer wedged");
    WAIT_FOR((job_result(), strcmp(g_state, "failed") == 0));
    CHECK(strcmp(g_state, "failed") == 0, "wedged job reported failed");
    CHECK(strcmp(g_error, "writer_wedged_reboot_required") == 0, "wedged job names writer_wedged_reboot_required");
    WAIT_FOR(!g_fr_claim_held);
    expect_claim_balanced("wedged job");
    CHECK(g_fr_flash_blocked_n >= 1, "the abandoned writer is still parked in flash");

    // a NEW job is refused: no second writer next to the parked one
    const long heap_before = g_fr_heap_live;
    int r = POST("/api/update/download", NULL);
    if (r == 202) {
        WAIT_FOR((job_result(), strcmp(g_state, "failed") == 0 && strstr(g_rq.resp, "\"busy\":false")));
    }
    CHECK(r == 202 || r == 409, "second download answered");
    job_result();
    CHECK(strcmp(g_error, "writer_wedged_reboot_required") == 0 || resp_has("writer_wedged_reboot_required"),
          "new download refused: writer_wedged_reboot_required");
    expect_claim_balanced("second wedged job");
    WAIT_FOR(g_fr_live_tasks <= 1); // only the parked writer remains
    CHECK(g_fr_heap_live == heap_before, "a job started after the wedge frees its buffers (never handed to the writer)");
    CHECK(POST("/api/update/check", NULL) == 202, "check after the wedge accepted");
    WAIT_FOR((job_result(), strcmp(g_state, "failed") == 0 || strcmp(g_state, "done") == 0));
    WAIT_FOR(g_fr_live_tasks <= 1);
    CHECK(g_fr_heap_live == heap_before, "a check after the wedge frees its buffers");

    CHECK(stage_phase() == UPDATE_STAGE_UPLOADING, "wedged writer holds the stage in UPLOADING");
    upload(g_img, IMG_LEN);
    CHECK(run_upload() == 409 && resp_has("writer_wedged_reboot_required"),
          "stage upload reports writer_wedged_reboot_required");
    CHECK(stage_phase() == UPDATE_STAGE_UPLOADING, "a refused manual upload does not abort the wedged owner's upload");
    expect_claim_balanced("upload while wedged");
    CHECK(POST("/api/update/stage/clear", NULL) == 409 && resp_has("writer_wedged_reboot_required"),
          "stage clear reports writer_wedged_reboot_required");
    expect_claim_balanced("clear while wedged");

    // release the parked writer: its abandoned op must undo itself, never leave a stage behind
    fr_flash_release();
    WAIT_FOR(g_fr_flash_blocked_n == 0);
    CHECK(g_fr_flash_blocked_n == 0, "parked writer resumed");
    WAIT_FOR(stage_phase() == UPDATE_STAGE_IDLE);
    CHECK(stage_phase() == UPDATE_STAGE_IDLE, "abandoned op aborted its own upload (stage back to idle)");
    CHECK(!stage_staged(), "abandoned op left no valid stage");
    CHECK(update_fetch_writer_wedged(), "wedge itself is permanent until reboot");
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    fr_reset();
    make_image();
    sha_hex_of(g_img, IMG_LEN, g_sha_hex);
    esp_err_t e = update_http_start();
    CHECK(e == 0, "update_http_start registers everything");
    CHECK(fr_find_handler("/api/update/stage", HTTP_POST).handler != NULL, "stage POST route");
    CHECK(fr_find_handler("/api/update/stage/clear", HTTP_POST).handler != NULL, "stage clear route");
    CHECK(fr_find_handler("/api/update/check", HTTP_POST).handler != NULL, "check route");
    CHECK(fr_find_handler("/api/update/download", HTTP_POST).handler != NULL, "download route");
    CHECK(fr_find_handler("/api/update/fetch", HTTP_GET).handler != NULL, "status route");
    CHECK(fr_find_handler("/api/update/fetch/cancel", HTTP_POST).handler != NULL, "cancel route");

    test_check_happy();
    test_download_happy();
    test_short_body_http_layer();
    test_short_body_chunked();
    test_sha_mismatch();
    test_claim_released_on_failures();
    test_handler_refusals();
    test_stage_upload_and_clear();
    test_writer_wedge();

    printf("test_update_fetch: %d checks, %d failed\n", g_checks, g_fail);
    if (g_fail != 0) {
        return 1;
    }
    printf("test_update_fetch: PASS\n");
    return 0;
}
