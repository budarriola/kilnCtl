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
#include "update_fetch_heap.h"
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
#define API_URL "https://api.github.com/repos/" REPO "/releases?per_page=15"
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
    {   // AUP-02: both channels read the releases LIST, so the fixture body is a one-element array
        const size_t n = strlen(g_api);
        memmove(g_api + 1, g_api, n);
        g_api[0] = '[';
        g_api[n + 1] = ']';
        g_api[n + 2] = '\0';
    }
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

// bounded condition wait with a caller-chosen ceiling; exits as soon as cond holds
#define WAIT_FOR_MS(cond, ms) for (g_t0 = GetTickCount(); !(cond) && GetTickCount() - g_t0 < (DWORD)(ms);) Sleep(2)

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
    CHECK(g_fr_live_clients == 0, "no http client leaked by a failed open (REVIEW_UPDFX3 LOW-4)");
    expect_claim_balanced("connect_failed");

    fixture(g_sha_hex, IMG_LEN);
    g_fake_heap_free_internal = 1000;
    POST("/api/update/download", NULL);
    wait_idle();
    job_result();
    CHECK(strcmp(g_state, "failed") == 0, "low heap fails the job");
    CHECK(strcmp(g_error, "low_heap_free") == 0, "low free heap error is low_heap_free");
    expect_claim_balanced("low_heap_free");

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

#define BENCH_IDLE_FREE 47191u  /* idle internal free on the current firmware (bench 2026-10-13); any value >= the admission need works */
#define T_RTC 7788u  /* RTC FAST heap free on the bench: internal free minus DMA-capable free */
#define T_DMA_FLOOR (FETCH_HEAP_ABORT_BELOW - T_RTC)  /* 1428 B */
#define T_DMA_HARD (FETCH_HEAP_FLOOR_BYTES - T_RTC)    /* 404 B */
/* Internal free and DMA free move together on the real board; keep the pair consistent (rtc = 7788). */
static void heap_idle_pair(uint32_t internal_free)
{
    g_fake_heap_free_internal = internal_free;
    g_fake_heap_dma_free_internal = internal_free - T_RTC;
}
#define HOP_URL "https://objects.githubusercontent.com/KilnCtrl-v1.2.3.bin"
#define HOP2_URL "https://objects.githubusercontent.com/second/KilnCtrl-v1.2.3.bin"
static void hop_dma_one_under(void)
{
    g_fake_heap_free_internal = 47191u;  /* internal fine, DMA one byte under the derived hop need */
    g_fake_heap_dma_free_internal = update_fetch_heap_dma_need(true, true, T_RTC) - 1u;
}
static void hop_dma_at_need(void)
{
    g_fake_heap_free_internal = 47191u;
    g_fake_heap_dma_free_internal = update_fetch_heap_dma_need(true, true, T_RTC);
}
static void drop_heap_low(void) { g_fake_heap_free_internal = 8000; }
static void drop_heap_one_under_abort(void) { g_fake_heap_free_internal = FETCH_HEAP_ABORT_BELOW - 1; }
static void set_heap_hop_ok(void) { heap_idle_pair(FETCH_HOP_DOWNLOAD_HEAP_MIN); g_fake_heap_largest_internal = 7680; }
static void set_heap_hop_low(void) { heap_idle_pair(27855); }

static void run_job_and_result(const char *uri)
{
    POST(uri, NULL);
    wait_idle();
    job_result();
}

static void test_heap_admission_updfx2(void)
{
    // MED-1: 22910 B free admits neither kind (a check start needs the download need).
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(22910);
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "low_heap_free") == 0, "22910 B refuses a check");
    expect_claim_balanced("check low free");

    // check admitted with a 4096 B block; download refused LOW_BLOCK
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fake_heap_largest_internal = 4096;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_state, "failed") != 0 || strcmp(g_error, "low_heap_block") != 0, "check admitted with 4096 B block");
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fake_heap_largest_internal = 4096;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_block") == 0, "download refused on 4096 B block");
    expect_claim_balanced("download low block");

    // bench idle reading admits a full download
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fake_heap_largest_internal = 10752;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0 || strcmp(g_state, "staged") == 0 || stage_staged(), "bench idle reading downloads");

    // LOW-1/mid-body: drop during the first read aborts with low_heap_midrun
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->on_read = drop_heap_low;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_midrun") == 0, "mid-body drop aborts low_heap_midrun");
    expect_claim_balanced("midrun");

    // One byte under the abort level aborts; the admission bottom (exactly the abort level) does not (see below).
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->on_read = drop_heap_one_under_abort;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_midrun") == 0, "one byte under the abort level aborts low_heap_midrun");
    expect_claim_balanced("midrun edge");

    // UPDFX7 LOW-2: the DMA need is derived, not a literal: hop need minus the RTC heap measured at the start
    // (30192 - 7788 = 22404 B). Judged at the hop, where DMA can move independently of the start reading.
    // (At the first admission both readings are one sample, so the DMA gate cannot disagree with the internal one.)
    const uint32_t dma_hop_need = update_fetch_heap_dma_need(true, true, T_RTC);
    CHECK(dma_hop_need == 22404u, "derived DMA hop need is 22404 B on the bench");
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_app_route->status = 302;
    g_app_route->location = HOP_URL;
    add_route(HOP_URL, g_img, IMG_LEN);
    g_app_route->on_open = hop_dma_one_under;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_dma") == 0, "hop refused one byte under the derived DMA need");
    CHECK(strstr(g_rq.resp, "\"hop\":\"refused\"") != NULL, "a hop refused on DMA is reported refused, not admitted (UPDFX9 LOW-2)");
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_app_route->status = 302;
    g_app_route->location = HOP_URL;
    add_route(HOP_URL, g_img, IMG_LEN);
    g_app_route->on_open = hop_dma_at_need;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_dma") != 0, "hop with DMA exactly at the derived need admitted");

    // UPDFX6 MED-1: internal admission edge, judged on the live (scratch-held) reading.
    fixture(g_sha_hex, IMG_LEN);
    g_fake_heap_free_internal = FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - 1;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_free") == 0, "download one byte under the admission need refused");
    expect_claim_balanced("admission edge refused");

    // per-hop admission: asset URL redirects to a second URL; heap low at the hop refuses it,
    // the hop need (30192 B) admits it, 27855 B does not.
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->status = 302;
    g_app_route->location = "https://objects.githubusercontent.com/KilnCtrl-v1.2.3.bin";
    fr_route_t *tgt = add_route("https://objects.githubusercontent.com/KilnCtrl-v1.2.3.bin", g_img, IMG_LEN);
    (void)tgt;
    g_app_route->on_open = set_heap_hop_low;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "failed") == 0 && strstr(g_error, "low_heap") != NULL, "low heap at redirect hop refused");

    fixture(g_sha_hex, IMG_LEN);
    g_app_route->status = 302;
    g_app_route->location = "https://objects.githubusercontent.com/KilnCtrl-v1.2.3.bin";
    add_route("https://objects.githubusercontent.com/KilnCtrl-v1.2.3.bin", g_img, IMG_LEN);
    g_app_route->on_open = set_heap_hop_ok;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_free") != 0 && strcmp(g_error, "low_heap_dma") != 0,
          "hop at exactly the UPDFX7 hop need (30192 B) admitted");
}

static void set_dma_low(void) { g_fake_heap_dma_free_internal = T_DMA_HARD - 1; }
static void set_dma_ok(void) { g_fake_heap_dma_free_internal = 100000; }
static void set_heap_hop_edge(void) { heap_idle_pair(FETCH_HOP_DOWNLOAD_HEAP_MIN - 1); }
static void set_heap_25000(void) { heap_idle_pair(25000); }
static void set_heap_idle(void) { heap_idle_pair(BENCH_IDLE_FREE); }
// Arms the scripted per-read DMA sequence (fake_support) when the asset body starts.
static void dip_heap_bottom(void) { g_fake_heap_free_internal = FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH - FETCH_HEAP_MODEL_DRAW_BYTES; }
static void arm_dma_seq(void) { g_fr_dma_seq_armed = true; }
static void set_dma_low_and_arm(void)
{
    g_fake_heap_dma_free_internal = T_DMA_FLOOR - 1;
    g_fr_dma_seq_armed = true;
}
static void cancel_on_second_delay(int n)
{
    if (n == 2) {
        POST("/api/update/fetch/cancel", NULL);
    }
}
static void deadline_on_first_delay(int n)
{
    if (n == 1) {
        g_fr_tick_skew_ms += 25LL * 60LL * 1000LL;  /* past the 20 min job deadline */
    }
}
static void heat_on_second_delay(int n)
{
    if (n == 2) {
        g_fr_heat_profile = true;
    }
}
static void cancel_on_third_delay(int n)
{
    if (n == 3) {
        POST("/api/update/fetch/cancel", NULL);
    }
}
static void test_heap_admission_updfx3(void)
{
    // UPDFX7: a download hop needs the start need less the measured hop dip (30192 B); one byte under it
    // is refused after the settle wait.
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->status = 302;
    g_app_route->location = HOP_URL;
    add_route(HOP_URL, g_img, IMG_LEN);
    g_app_route->on_open = set_heap_hop_edge;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_free") == 0, "download hop one byte under the need refused");
    expect_claim_balanced("hop edge");

    // hop rule differs from start rule for a check: start needs 31728 B, a check hop needs 21284 B
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_routes[0].status = 302;
    g_fr_routes[0].location = HOP_URL;
    add_route(HOP_URL, g_api, strlen(g_api))->on_open = set_heap_idle;
    g_fr_routes[0].on_open = set_heap_25000;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "low_heap_free") != 0, "check hop at 25000 B admitted by the hop rule (21284 B)");

    // LOW-1/MED-1: a residual that clears while the settle loop waits admits the start
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(27855);
    g_fake_heap_recover_to = BENCH_IDLE_FREE;
    g_fake_heap_recover_reads = 4;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "low_heap_free") != 0, "start admitted once the previous call's residual clears");
    CHECK(g_fake_heap_recover_reads == 0, "the settle loop re-sampled the heap");

    // LOW-5 M8: a check allocates no internal scratch; a download allocates one.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_internal_mallocs = 0;
    run_job_and_result("/api/update/check");
    CHECK(g_fr_internal_mallocs == 0, "a check job allocates no internal scratch");
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_internal_mallocs = 0;
    run_job_and_result("/api/update/download");
    CHECK(g_fr_internal_mallocs >= 1, "a download job allocates the internal stager scratch");

    // LOW-5 M12: DMA low at the redirect hop refuses
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->status = 302;
    g_app_route->location = HOP_URL;
    add_route(HOP_URL, g_img, IMG_LEN)->on_open = set_dma_ok;  // restored after the hop admission, so only that gate sees it low
    heap_idle_pair(BENCH_IDLE_FREE);
    g_app_route->on_open = set_dma_low;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_dma") == 0, "low DMA at a redirect hop refused");

    // LOW-3 / REVIEW_UPDFX3 LOW-2: a sustained DMA shortage mid-body aborts, and the job state says so.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_app_route->on_read = set_dma_low;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_dma") == 0, "DMA drop mid-body aborts low_heap_dma");
    CHECK(strcmp(g_state, "failed") == 0, "mid-body DMA abort leaves the job failed");
    CHECK(stage_phase() == UPDATE_STAGE_IDLE && !stage_staged(), "mid-body DMA abort stages nothing, stage idle");
    CHECK(g_fr_live_clients == 0, "mid-body DMA abort cleans the client up");
    expect_claim_balanced("mid-body dma");

    // UPDFX6: steady between-read samples exactly at the derived floor (1428 B)
    // do not abort.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    for (int i = 0; i < 8; i++) {
        g_fr_dma_seq[i] = T_DMA_FLOOR;
    }
    g_fr_dma_seq_n = 8;
    g_app_route->on_read = arm_dma_seq;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0 && stage_staged(), "steady DMA exactly at the derived floor (1428 B) completes");
    expect_claim_balanced("steady dma above floor");

    // A reading under the 1428 B floor (1000 B): if it happens mid-body it aborts (3 consecutive low samples).
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    for (int i = 0; i < 8; i++) {
        g_fr_dma_seq[i] = 1000;
    }
    g_fr_dma_seq_n = 8;
    g_app_route->on_read = arm_dma_seq;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_dma") == 0, "sustained DMA at 1000 B (under the 1428 B floor) aborts low_heap_dma");
    expect_claim_balanced("dma under floor");

    // UPDFX7: admitted at exactly the admission need (live 31728 B, scratch held) and the
    // internal free dips to 9216 B (admission minus the 22512 B model draw) mid-body: completes, no abort.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH);
    g_app_route->on_read = dip_heap_bottom;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0 && stage_staged(), "fetch admitted at the threshold dipping to 9216 B completes without an abort");
    expect_claim_balanced("admission-edge dip");

    // A transient dip under the floor (two between-read samples) that recovers does not abort.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_dma_seq[0] = T_DMA_FLOOR - 1;
    g_fr_dma_seq[1] = T_DMA_FLOOR - 1;
    g_fr_dma_seq_n = 2;
    g_app_route->on_read = arm_dma_seq;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0 && stage_staged(), "two low DMA reads then recovery: download completes");
    CHECK(g_fr_dma_seq_i >= 2, "the scripted dip was consumed by body reads");
    expect_claim_balanced("transient dma dip");

    // LOW-1: low,low,ok repeating aborts (the consecutive rule alone never would).
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    for (int i = 0; i < 12; i++) {
        g_fr_dma_seq[i] = (i % 3 == 2) ? 100000 : T_DMA_FLOOR - 1;
    }
    g_fr_dma_seq_n = 12;
    g_app_route->on_read = arm_dma_seq;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_dma") == 0, "low,low,ok repeating DMA aborts low_heap_dma");
    expect_claim_balanced("dma low-low-ok");

    // MED-2: stalled (EAGAIN) slices are not reads. DMA stays low across 5 stalled slices, then the first
    // data read samples healthy: the download completes (3 stalled slices used to abort).
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_dma_seq[0] = 100000;
    g_fr_dma_seq_n = 1;
    g_app_route->read_stall_ms = 1;
    g_app_route->stall_reads = 5;
    g_app_route->on_read = set_dma_low_and_arm;  /* low from the first read on, healthy only after the 5 stalls */
    g_fr_eagain_n = 0;
    run_job_and_result("/api/update/download");
    CHECK(g_fr_eagain_n >= 5, "the route stalled five slices");
    CHECK(strcmp(g_state, "done") == 0 && stage_staged(), "low DMA across stalled slices does not abort");
    expect_claim_balanced("dma eagain");

    // LOW-5: the low-sample history is per response. Job A ends on two low samples; job B starts with one
    // low sample: B must complete (a carried-over history would abort it).
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    /* IMG_LEN is 5 body reads of 4096 B: job A ends on its last two samples low. */
    for (int i = 0; i < 3; i++) {
        g_fr_dma_seq[i] = 100000;
    }
    g_fr_dma_seq[3] = T_DMA_FLOOR - 1;
    g_fr_dma_seq[4] = T_DMA_FLOOR - 1;
    g_fr_dma_seq_n = 5;
    g_app_route->on_read = arm_dma_seq;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0, "job A (two low samples) completes");
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_dma_seq[0] = T_DMA_FLOOR - 1;
    g_fr_dma_seq_n = 1;
    g_app_route->on_read = arm_dma_seq;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0, "job B (one low sample) is not aborted by job A's history");

    // UPDFX4 MED-3: real exhaustion (under the hard minimum) aborts on the very first sample.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_dma_seq[0] = 100;
    g_fr_dma_seq_n = 1;
    g_app_route->on_read = arm_dma_seq;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_dma") == 0 && strcmp(g_state, "failed") == 0, "DMA under the hard minimum aborts");
    CHECK(g_fr_dma_seq_i <= 2, "aborted on the first low sample, not after a run of three");
    CHECK(!stage_staged(), "hard-min abort stages nothing");
    expect_claim_balanced("hard-min dma");

    // REVIEW_UPDFX3 LOW-3: the settle wait is FETCH_HEAP_SETTLE_TRIES x FETCH_HEAP_SETTLE_MS for a heap that stays low.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(20000);
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "low_heap_free") == 0, "persistently low heap refuses low_heap_free");
    CHECK(g_fr_delay_calls == (int)FETCH_HEAP_SETTLE_TRIES, "settle wait re-samples exactly FETCH_HEAP_SETTLE_TRIES times");
    CHECK(g_fr_delay_total_ticks == 3000ul /* 12 x 250 ms, pinned literally (UPDFX3 LOW-3) */,
          "each settle delay is FETCH_HEAP_SETTLE_MS");
    expect_claim_balanced("settle persistent low");

    // REVIEW_UPDFX3 LOW-1/LOW-3: a cancel during the settle wait reports cancelled, breaks the loop at once.
    fixture(g_sha_hex, IMG_LEN);
    g_fake_heap_free_internal = 20000;
    g_fr_on_delay = cancel_on_third_delay;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "cancelled") == 0, "cancel during the settle wait reports cancelled, not low_heap_free");
    CHECK(strcmp(g_state, "failed") == 0, "cancelled settle wait fails the job");
    CHECK(g_fr_delay_calls == 3, "the settle loop stops at the cancel (no further delays)");
    expect_claim_balanced("settle cancel");

    // UPDFX5 LOW-2: a cancel during the wait is "cancelled" even when the sample after it reads LOW_BLOCK.
    fixture(g_sha_hex, IMG_LEN);
    g_fake_heap_free_internal = 20000;
    g_fake_heap_largest_internal = 100;
    g_fake_heap_recover_to = BENCH_IDLE_FREE;
    g_fake_heap_recover_reads = 3;
    g_fr_on_delay = cancel_on_second_delay;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "cancelled") == 0, "cancel during the wait reports cancelled, not low_heap_block");
    CHECK(g_fr_delay_calls == 2, "the wait stops at the cancelling tick");
    expect_claim_balanced("settle cancel block");

    // UPDFX5 LOW-2: the job deadline is re-checked each tick at a redirect hop (armed by then).
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->status = 302;
    g_app_route->location = HOP_URL;
    add_route(HOP_URL, g_img, IMG_LEN);
    g_app_route->on_open = set_heap_hop_edge;
    g_fr_on_delay = deadline_on_first_delay;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "timeout") == 0, "deadline passing during the hop's settle wait reports timeout");
    CHECK(g_fr_delay_calls == 1, "the wait stops at the tick that passed the deadline");
    expect_claim_balanced("settle deadline");

    // UPDFX5 LOW-2: a firing that starts during the wait stops the fetch at once.
    fixture(g_sha_hex, IMG_LEN);
    g_fake_heap_free_internal = 20000;
    g_fr_on_delay = heat_on_second_delay;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "heat_run_active") == 0, "firing start during the settle wait reports heat_run_active");
    CHECK(g_fr_delay_calls == 2, "the wait stops at the tick that saw the firing");
    expect_claim_balanced("settle heat");

    // UPDFX4 LOW-4: a block-starved heap (free fine, largest block short) is refused low_heap_block with no
    // settle wait at all: only LOW_FREE settles.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fake_heap_largest_internal = 100;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "low_heap_block") == 0, "block-starved heap refused low_heap_block");
    CHECK(g_fr_delay_calls == 0, "no settle wait on a LOW_BLOCK verdict");
    expect_claim_balanced("block starved");

    // AUP-02: a 404 on the list endpoint is a plain http_status on either channel; /releases/latest is never read.
    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 404;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "http_status") == 0, "404 on the list endpoint (pre-releases off) -> http_status");
    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 404;
    POST("/api/update/check", "allow_prerelease=1");
    wait_idle();
    job_result();
    CHECK(strcmp(g_error, "http_status") == 0, "404 with pre-releases allowed stays http_status");
}

// UPDFX7: the hop's settle result and the job's low-waters are in the status JSON.
static void hop_edge_then_recover(void)
{
    g_fake_heap_free_internal = FETCH_HOP_DOWNLOAD_HEAP_MIN - 1;
    g_fake_heap_dma_free_internal = BENCH_IDLE_FREE - T_RTC;
    g_fake_heap_recover_to = BENCH_IDLE_FREE;
    g_fake_heap_recover_reads = 3;
}

// UPDFX8: hop 1 settles up to the hop edge + 500 B; the heap handed to the NEXT admission is set by the route that
// redirects (an on_open runs after that route's own admission).
static void hop_edge_plus_500_then_recover(void)
{
    g_fake_heap_free_internal = FETCH_HOP_DOWNLOAD_HEAP_MIN - 1;
    g_fake_heap_dma_free_internal = BENCH_IDLE_FREE - T_RTC;
    g_fake_heap_recover_to = FETCH_HOP_DOWNLOAD_HEAP_MIN + 500;
    g_fake_heap_recover_reads = 3;
}
static void hop_idle_clean(void) { heap_idle_pair(BENCH_IDLE_FREE); }

static long json_heap_num(const char *key)
{
    const char *h = strstr(g_rq.resp, "\"heap\":{");
    if (!h) {
        return -1;
    }
    char pat[48];
    snprintf(pat, sizeof(pat), "\"%s\":", key);
    const char *k = strstr(h, pat);
    return k ? strtol(k + strlen(pat), NULL, 10) : -2;
}

static void test_heap_status_json_updfx7(void)
{
    // A REFUSED first admission still reports the refusing reading as heap.start_free (it used to read 0).
    fixture(g_sha_hex, IMG_LEN);
    g_fake_heap_free_internal = 1000;
    POST("/api/update/download", NULL);
    wait_idle();
    job_result();
    CHECK(strcmp(g_error, "low_heap_free") == 0, "refused first admission is low_heap_free");
    CHECK(json_heap_num("start_free") == 1000, "refused admission reports the refusing reading as heap.start_free, not 0");
    // A completed download: sampled, rtc, start values and the low-waters are reported.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    run_job_and_result("/api/update/download");
    CHECK(strstr(g_rq.resp, "\"heap\":{\"sampled\":true") != NULL, "status JSON carries the heap object");
    CHECK(json_heap_num("rtc_free") == (long)T_RTC, "heap.rtc_free is internal minus DMA free at admission");
    CHECK(json_heap_num("start_free") == (long)BENCH_IDLE_FREE, "heap.start_free is the admission reading");
    CHECK(json_heap_num("start_dma") == (long)(BENCH_IDLE_FREE - T_RTC), "heap.start_dma is the DMA reading");
    CHECK(strstr(g_rq.resp, "\"hop\":\"none\"") != NULL, "no redirect reports hop none");
    CHECK(json_heap_num("min_free") > 0 && json_heap_num("min_free") <= (long)BENCH_IDLE_FREE,
          "heap.min_free is a real low-water not above the start reading");
    CHECK(json_heap_num("min_dma") > 0 && json_heap_num("min_dma") <= (long)(BENCH_IDLE_FREE - T_RTC),
          "heap.min_dma is a real low-water");
    expect_claim_balanced("heap json plain");

    // The low-waters track the read loop, not just admission: a mid-body dip to the abort level and a DMA
    // reading above the DMA floor are both reported exactly.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH);
    g_app_route->on_read = dip_heap_bottom;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0, "dip to the abort level completes");
    CHECK(json_heap_num("min_free") == (long)FETCH_HEAP_ABORT_BELOW, "heap.min_free tracks the read-loop low-water");
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_dma_seq[0] = T_DMA_FLOOR + 500;
    g_fr_dma_seq_n = 1;
    g_app_route->on_read = arm_dma_seq;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0, "one DMA reading above the floor completes");
    CHECK(json_heap_num("min_dma") == (long)(T_DMA_FLOOR + 500), "heap.min_dma tracks the body-read low-water");

    // A hop that recovers while the job settles: hop admitted, tries > 0, free at the admitting sample.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_app_route->status = 302;
    g_app_route->location = HOP_URL;
    add_route(HOP_URL, g_img, IMG_LEN);
    g_app_route->on_open = hop_edge_then_recover;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0, "hop that recovers in the settle wait completes");
    CHECK(strstr(g_rq.resp, "\"hop\":\"admitted\"") != NULL, "hop settle result admitted is reported");
    CHECK(json_heap_num("hop_settle_tries") > 0, "hop_settle_tries counts the settle re-samples");
    CHECK(json_heap_num("hop_free") >= (long)FETCH_HOP_DOWNLOAD_HEAP_MIN, "hop_free is the admitting reading");
    expect_claim_balanced("heap json hop");

    // A refused hop reports refused.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_app_route->status = 302;
    g_app_route->location = HOP_URL;
    add_route(HOP_URL, g_img, IMG_LEN);
    g_app_route->on_open = set_heap_hop_edge;
    run_job_and_result("/api/update/download");
    CHECK(strstr(g_rq.resp, "\"hop\":\"refused\"") != NULL, "refused hop reported");
    CHECK(json_heap_num("hop_settle_tries") == (long)FETCH_HEAP_SETTLE_TRIES, "refused hop used every settle try");
    expect_claim_balanced("heap json hop refused");

    // UPDFX8 LOW-2: every hop is recorded. Hop 1 recovers in the settle wait (tries > 0), hop 2 is clean: the
    // job reports the most settle re-samples of any hop and the lowest hop reading, not the last hop's.
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_app_route->status = 302;
    g_app_route->location = HOP_URL;
    g_app_route->on_open = hop_edge_plus_500_then_recover;
    fr_route_t *h1 = add_route(HOP_URL, g_img, IMG_LEN);
    h1->status = 302;
    h1->location = HOP2_URL;
    h1->on_open = hop_idle_clean;
    add_route(HOP2_URL, g_img, IMG_LEN);
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0, "two-hop download completes");
    CHECK(strstr(g_rq.resp, "\"hop\":\"admitted\"") != NULL, "two admitted hops report admitted");
    CHECK(json_heap_num("hop_settle_tries") > 0, "hop_settle_tries keeps the most re-samples of any hop");
    CHECK(json_heap_num("hop_free") == (long)(FETCH_HOP_DOWNLOAD_HEAP_MIN + 500),
          "hop_free keeps the lowest hop reading (hop 1), not the clean second hop's");
    expect_claim_balanced("heap json two hops");

    // UPDFX8 LOW-2: hop 1 admitted, hop 2 refused: the refusal is reported (a first-hop-only record says admitted).
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_app_route->status = 302;
    g_app_route->location = HOP_URL;
    h1 = add_route(HOP_URL, g_img, IMG_LEN);
    h1->status = 302;
    h1->location = HOP2_URL;
    h1->on_open = set_heap_hop_edge;
    add_route(HOP2_URL, g_img, IMG_LEN);
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_free") == 0, "second hop one byte under the need refused");
    CHECK(strstr(g_rq.resp, "\"hop\":\"refused\"") != NULL, "a refused second hop is reported refused");
    CHECK(json_heap_num("hop_settle_tries") == (long)FETCH_HEAP_SETTLE_TRIES, "second hop's settle tries reported");
    expect_claim_balanced("heap json second hop refused");
}

// UPDFX8 LOW-1: rtc_free is the smallest offset of the back-to-back first-admission pairs. The first DMA reading
// is taken just after a concurrent 1500 B allocation (offset 9288); reading only that pair would lower the DMA floor.
static void test_rtc_offset_min_of_pairs_updfx8(void)
{
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_dma_call_seq[0] = BENCH_IDLE_FREE - T_RTC - 1500u;
    g_fr_dma_call_seq[1] = BENCH_IDLE_FREE - T_RTC;
    g_fr_dma_call_seq[2] = BENCH_IDLE_FREE - T_RTC - 1500u;
    g_fr_dma_call_seq_n = 3;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0, "download with a perturbed first DMA reading completes");
    CHECK(g_fr_dma_call_seq_i >= 3, "the first admission took three DMA readings");
    CHECK(json_heap_num("rtc_free") == (long)T_RTC, "rtc_free is the smallest-offset pair (7788), not the inflated first pair");
    CHECK(json_heap_num("start_dma") == (long)(BENCH_IDLE_FREE - T_RTC), "start_dma is the picked pair's DMA reading");
    expect_claim_balanced("rtc min of pairs");
}

// UPDFX9 LOW-1: a persistent allocation lands between pair 0's f and DMA reads (offset 7788+1500). It persists, so
// pairs 1 and 2 read internal free 1500 lower too and their offsets are the true 7788. Reusing pair 0's f for the
// later pairs, or deriving from pair 0's f, would inflate rtc_free.
static void test_rtc_offset_persistent_alloc_updfx9(void)
{
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    const uint32_t low = BENCH_IDLE_FREE - 1500u;
    g_fr_dma_call_seq[0] = low - T_RTC;
    g_fr_dma_call_seq[1] = low - T_RTC;
    g_fr_dma_call_seq[2] = low - T_RTC;
    g_fr_dma_call_seq_n = 3;
    g_fr_int_call_after = 1;  /* pair 0's f was read before its DMA reading */
    g_fr_int_call_seq[0] = low;
    g_fr_int_call_seq[1] = low;
    g_fr_int_call_seq_n = 2;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0, "download with a persistent allocation inside pair 0 completes");
    CHECK(g_fr_int_call_seq_i == 2, "pairs 1 and 2 each re-read internal free");
    CHECK(json_heap_num("rtc_free") == (long)T_RTC, "rtc_free comes from the re-read pairs (7788), not pair 0's stale f");
    CHECK(json_heap_num("start_free") == (long)BENCH_IDLE_FREE, "start_free reports pair 0's internal reading (the verdict's), not the picked pair's (UPDFX9 INFO-5)");
    CHECK(json_heap_num("min_dma") == (long)(low - T_RTC), "min_dma carries the lowest scripted DMA reading");
    expect_claim_balanced("rtc persistent alloc");
}

// UPDFX9 INFO-5: a START (not a hop) can be refused on DMA: pair 0's internal reading admits, a persistent allocation
// then lowers the later pairs, whose offset is the picked one, so the DMA check (which reduces to pf[best] >= need)
// refuses. The hop record is only for hops: it must stay "none".
static void test_start_dma_refusal_leaves_hop_none_updfx9(void)
{
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    const uint32_t dma_low = update_fetch_heap_dma_need(true, false, T_RTC) - 1u;
    g_fr_dma_call_seq[0] = BENCH_IDLE_FREE - T_RTC - 1500u; /* pair 0 offset 9288, never picked */
    g_fr_dma_call_seq[1] = dma_low;
    g_fr_dma_call_seq[2] = dma_low;
    g_fr_dma_call_seq_n = 3;
    g_fr_int_call_after = 1;
    g_fr_int_call_seq[0] = dma_low + T_RTC;
    g_fr_int_call_seq[1] = dma_low + T_RTC;
    g_fr_int_call_seq_n = 2;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "low_heap_dma") == 0, "a start refused on DMA reports low_heap_dma");
    CHECK(strstr(g_rq.resp, "\"hop\":\"none\"") != NULL, "a START DMA refusal is not recorded as a hop refusal");
    expect_claim_balanced("start dma refused");
}

// UPDFX9 LOW-1: only the THIRD pair is clean (offsets 9288, 9788, 7788), and the lowest DMA reading is pair 1's.
// A pair count of two, or extra pairs that never reach the low-waters, is visible.
static void test_rtc_offset_third_pair_clean_updfx9(void)
{
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(BENCH_IDLE_FREE);
    g_fr_dma_call_seq[0] = BENCH_IDLE_FREE - T_RTC - 1500u;
    g_fr_dma_call_seq[1] = BENCH_IDLE_FREE - T_RTC - 2000u;
    g_fr_dma_call_seq[2] = BENCH_IDLE_FREE - T_RTC;
    g_fr_dma_call_seq_n = 3;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0, "download with only the third pair clean completes");
    CHECK(json_heap_num("rtc_free") == (long)T_RTC, "rtc_free is the third (clean) pair's offset");
    CHECK(json_heap_num("start_dma") == (long)(BENCH_IDLE_FREE - T_RTC), "start_dma is the third pair's DMA reading");
    CHECK(json_heap_num("min_dma") == (long)(BENCH_IDLE_FREE - T_RTC - 2000u), "min_dma is pair 1's reading (extra pairs are noted)");
    expect_claim_balanced("rtc third pair clean");
}

// UPDFX7 LOW-4: a check starting with internal free between the hop need and the start need is refused as a
// START (an assertion that fails if a start were judged by the hop rule).
static void test_check_start_between_needs_updfx7(void)
{
    const uint32_t between = (FETCH_HOP_CHECK_HEAP_MIN + FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH) / 2;
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(between);
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "low_heap_free") == 0, "check start between the hop and start needs is refused");
    CHECK(g_fr_delay_calls == (int)FETCH_HEAP_SETTLE_TRIES, "the refused start waited the full settle");
    CHECK(json_heap_num("min_dma") == 0, "a job refused before any DMA reading reports min_dma 0, not UINT32_MAX");
    CHECK(json_heap_num("min_free") == (long)between, "min_free is the refused reading");
    expect_claim_balanced("check between needs");
    fixture(g_sha_hex, IMG_LEN);
    heap_idle_pair(FETCH_DOWNLOAD_HEAP_MIN_AFTER_SCRATCH);
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "low_heap_free") != 0, "check start exactly at the start need admitted");
}

// UPDAUDIT Q1: a redirect hop closes and frees the first client (its TLS session) before the second
// client is created and opened, so the two TLS sessions never coexist in internal heap.
static void test_redirect_hop_frees_first_client(void)
{
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->status = 302;
    g_app_route->location = "https://objects.githubusercontent.com/KilnCtrl-v1.2.3.bin";
    add_route("https://objects.githubusercontent.com/KilnCtrl-v1.2.3.bin", g_img, IMG_LEN);
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0 && stage_staged(), "redirected download staged");
    CHECK(strcmp(g_fr_last_url, "https://objects.githubusercontent.com/KilnCtrl-v1.2.3.bin") == 0,
          "second hop opened the Location URL");
    CHECK(g_fr_max_live_clients == 1, "never two http clients live at once across the redirect hop");
    CHECK(g_fr_open_while_live == 0, "no open while a previous client was still live");
    CHECK(g_fr_live_clients == 0, "every client cleaned up after the job");
    expect_claim_balanced("redirect hop");
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
// UPDATE_PATH_ROBUSTNESS_AUDIT LOW-1/2/4/8: rate-limit and server-error reasons with a backoff, header/read
// timeouts labelled timeout, cancel inside a stalled read, old stage kept when a new download fails early.
static unsigned status_retry_after(void)
{
    const char *p = strstr(g_rq.resp, "\"retry_after_s\":");
    return p ? (unsigned)strtoul(p + 16, NULL, 10) : 9999u;
}
static void outwait_backoff(void) { g_fr_tick_skew_ms += 4000000LL; }

static void test_updlow(void)
{
    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 429;
    g_fr_routes[0].retry_after = "120";
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "rate_limited") == 0, "429 -> rate_limited");
    CHECK(status_retry_after() >= 100 && status_retry_after() <= 120, "Retry-After 120 reported in status");
    CHECK(POST("/api/update/check", NULL) == 429 && resp_has("\"error\":\"backoff\""), "check refused during backoff");
    CHECK(POST("/api/update/download", NULL) == 429 && resp_has("\"error\":\"backoff\""), "download refused during backoff");
    expect_claim_balanced("backoff refusal");
    outwait_backoff();
    g_fr_routes[0].status = 200;
    g_fr_routes[0].retry_after = NULL;
    CHECK(POST("/api/update/check", NULL) == 202, "check accepted once the backoff passed");
    wait_idle();

    fixture(g_sha_hex, IMG_LEN);
    char reset[24];
    snprintf(reset, sizeof(reset), "%lld", g_fr_now_epoch + 300);
    g_fr_routes[0].status = 403;
    g_fr_routes[0].ratelimit_reset = reset;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "access_denied") == 0, "403 with only a reset header is an access error, not rate_limited");
    CHECK(status_retry_after() >= 50 && status_retry_after() <= 60, "non-rate 403 gets the normal backoff, not the reset window");
    outwait_backoff();

    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 403;
    g_fr_routes[0].ratelimit_reset = reset;
    g_fr_routes[0].ratelimit_remaining = "0";
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "rate_limited") == 0, "403 with Remaining 0 -> rate_limited");
    CHECK(status_retry_after() >= 250 && status_retry_after() <= 300, "X-RateLimit-Reset sets the backoff when Remaining is 0");
    outwait_backoff();

    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 403;
    g_fr_routes[0].ratelimit_reset = reset;
    g_fr_routes[0].ratelimit_remaining = "7";
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "access_denied") == 0 && status_retry_after() <= 60, "403 with Remaining 7 ignores the reset window");
    outwait_backoff();

    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 503;
    g_fr_routes[0].ratelimit_reset = reset;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "server_error") == 0 && status_retry_after() <= 60, "5xx never inherits a reset-window backoff");
    outwait_backoff();

    // LOW-3: tick-difference wrap. Backoff set, then the tick counter jumps past 2^31 ticks: expired, not active.
    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 429;
    g_fr_routes[0].retry_after = "300";
    run_job_and_result("/api/update/check");
    CHECK(status_retry_after() > 0, "wrap test: backoff active");
    g_fr_tick_skew_ms += 2147483648LL + 1000000LL;
    job_result();
    CHECK(status_retry_after() == 0, "backoff expired across a >2^31 tick jump (unsigned elapsed)");

    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 503;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "server_error") == 0, "503 -> server_error");
    CHECK(status_retry_after() >= 50 && status_retry_after() <= 60, "5xx default backoff 60 s");
    outwait_backoff();

    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].status = 418;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "http_status") == 0, "other status stays http_status");
    CHECK(status_retry_after() == 0, "no backoff for other statuses");
    CHECK(POST("/api/update/check", NULL) == 202, "no refusal after a plain http_status");
    wait_idle();

    // LOW-4: a header timeout is a timeout, not bad_response
    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].advertised = -5;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "timeout") == 0, "header timeout labelled timeout");
    CHECK(g_fr_hdr_eagain_n == 10, "header stall patience is exactly 10 slices");
    expect_claim_balanced("header timeout");

    // LOW-4/8: a read that never delivers fails as timeout after the stall budget
    fixture(g_sha_hex, IMG_LEN);
    g_fr_routes[0].read_stall_ms = 200;
    g_fr_routes[0].stall_reads = 0;
    run_job_and_result("/api/update/check");
    CHECK(strcmp(g_error, "timeout") == 0, "stalled read labelled timeout");
    CHECK(g_fr_eagain_n == 10, "body stall patience is exactly 10 slices");

    // LOW-4/M10: progress resets the stall count -- 9 stalled reads before every chunk still completes
    fixture(g_sha_hex, IMG_LEN);
    g_app_route->stall_per_chunk = 9;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_error, "timeout") != 0, "trickling body with progress is not a stall");
    CHECK(g_fr_eagain_n >= 18, "trickle test really stalled across several chunks");

    // LOW-8: cancel lands within a slice while a read is stalled
    for (int hdr = 0; hdr < 2; hdr++) {
        fixture(g_sha_hex, IMG_LEN);
        if (hdr) {
            g_fr_routes[0].advertised = -6; // header read blocks one full slice per call
        } else {
            g_fr_routes[0].read_stall_ms = -1; // body read blocks one full slice per call
            g_fr_routes[0].stall_reads = 0;
        }
        CHECK(POST("/api/update/check", NULL) == 202, "check started");
        Sleep(100);
        DWORD tc = GetTickCount();
        CHECK(POST("/api/update/fetch/cancel", NULL) == 200, "cancel accepted");
        wait_idle();
        CHECK(GetTickCount() - tc < 1250, hdr ? "cancel inside a stalled header read takes effect within about 1 s"
                                              : "cancel inside a stalled body read takes effect within about 1 s");
        job_result();
        CHECK(strcmp(g_state, "failed") == 0 && strcmp(g_error, "timeout") != 0, hdr ? "cancelled stalled header job failed as cancelled" : "cancelled stalled body job failed as cancelled");
        expect_claim_balanced("cancel in stalled read");
    }

    // LOW-1: a download that fails before the first flash write leaves the old verified stage alone
    fixture(g_sha_hex, IMG_LEN);
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "done") == 0 && stage_staged(), "first download staged");
    g_app_route->status = 404;
    run_job_and_result("/api/update/download");
    CHECK(strcmp(g_state, "failed") == 0, "second download failed");
    CHECK(stage_staged(), "old verified stage kept when the new download fails before writing");
    expect_claim_balanced("old stage kept");
}

static void test_writer_wedge(void)
{
    fixture(g_sha_hex, IMG_LEN);
    CHECK(!update_fetch_writer_wedged(), "writer not wedged before the case");
    const long heap_pre_wedge = g_fr_heap_live;
    g_fr_time_div = 1000; // the writer wait becomes milliseconds
    g_fr_flash_write_block = true;
    CHECK(POST("/api/update/download", NULL) == 202, "download accepted");
    WAIT_FOR_MS(update_fetch_writer_wedged(), 30000); // writer start-up is starvation-sensitive
    CHECK(update_fetch_writer_wedged(), "a timed-out writer op marks the writer wedged");
    WAIT_FOR_MS((job_result(), strcmp(g_state, "failed") == 0), 30000);
    CHECK(strcmp(g_state, "failed") == 0, "wedged job reported failed");
    CHECK(strcmp(g_error, "writer_wedged_reboot_required") == 0, "wedged job names writer_wedged_reboot_required");
    CHECK(strstr(g_rq.resp, "\"bytes_total\":20000") != NULL && IMG_LEN == 20000u,
          "wedge hit after WR_BEGIN, so the post-wedge WR_ABORT path is exercised");
    WAIT_FOR(!g_fr_claim_held);
    expect_claim_balanced("wedged job");
    CHECK(g_fr_flash_blocked_n >= 1, "the abandoned writer is still parked in flash");
    CHECK(g_fr_heap_live > heap_pre_wedge, "wedged job buffers stay held while the writer is parked");

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
    CHECK(g_fr_live_tasks <= 1, "no second writer task started after the wedge (wr_start guard)");
    CHECK(g_fr_heap_live == heap_before, "a job started after the wedge frees its buffers (never handed to the writer)");
    CHECK(POST("/api/update/check", NULL) == 202, "check after the wedge accepted");
    WAIT_FOR((job_result(), strcmp(g_state, "failed") == 0 || strcmp(g_state, "done") == 0));
    WAIT_FOR(g_fr_live_tasks <= 1);
    CHECK(g_fr_live_tasks <= 1, "a check after the wedge starts no second writer task");
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

    // wr_call()'s wedge guard: once wedged, no further writer op may be issued (the job's final WR_ABORT would
    // overwrite wr.cmd under the parked op). A guardless wr_call starts another bounded wait here.
    CHECK(g_fr_wr_wait_after_wedge == 0, "no writer op issued after the wedge latched (wr_call guard)");

    // release the parked writer: its abandoned op must undo itself, never leave a stage behind
    fr_flash_release();
    WAIT_FOR(g_fr_flash_blocked_n == 0);
    CHECK(g_fr_flash_blocked_n == 0, "parked writer resumed");
    WAIT_FOR(stage_phase() == UPDATE_STAGE_IDLE);
    CHECK(stage_phase() == UPDATE_STAGE_IDLE, "abandoned op aborted its own upload (stage back to idle)");
    CHECK(!stage_staged(), "abandoned op left no valid stage");
    CHECK(update_fetch_writer_wedged(), "wedge itself is permanent until reboot");
    // handshake: the released writer finished its abandoned op (final give included) and is back parked in its
    // idle command wait (unbounded take), so nothing it does later can race the check below
    WAIT_FOR_MS(g_fr_sem_idle_waiters >= 1, 30000);
    CHECK(g_fr_sem_idle_waiters >= 1, "released writer is back at its idle wait");
    CHECK(g_fr_sem_bad_n == 0, "no semaphore take/give on a NULL or deleted handle (wr_stop kept the parked writer's semaphores)");
    // LOW-5: the wedged job's buffers are reclaimed by the writer when its late op returns
    WAIT_FOR(g_fr_heap_live == heap_pre_wedge);
    CHECK(g_fr_heap_live == heap_pre_wedge, "late-exiting writer freed the wedged job buffers");
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
    test_heap_admission_updfx2();
    test_rtc_offset_min_of_pairs_updfx8();
    test_rtc_offset_persistent_alloc_updfx9();
    test_start_dma_refusal_leaves_hop_none_updfx9();
    test_rtc_offset_third_pair_clean_updfx9();
    test_redirect_hop_frees_first_client();
    test_updlow();
    test_heap_admission_updfx3();
    test_heap_status_json_updfx7();
    test_check_start_between_needs_updfx7();
    test_handler_refusals();
    test_stage_upload_and_clear();
#ifdef UF_EXTRA_TESTS
    extra_tests(); // test_update_http_refusals.c: runs before the permanent writer wedge
    // A wedge is permanent per process, so this exe wedges the other way (writer finished, completion give
    // missed); test_writer_wedge runs in kilnctl_host_tests_update_fetch.exe.
    extra_final_wedge_test();
    (void)test_writer_wedge;
#else
    test_writer_wedge();
#endif

    printf("test_update_fetch: %d checks, %d failed\n", g_checks, g_fail);
    if (g_fail != 0) {
        return 1;
    }
    printf("test_update_fetch: PASS\n");
    return 0;
}
