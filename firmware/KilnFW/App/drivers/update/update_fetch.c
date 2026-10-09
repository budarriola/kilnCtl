// update_fetch.c -- see update_fetch.h. ESP wiring only: TLS/HTTP client, two tasks, four routes.
// Every decision that can be made without the board (URL and host rules, release JSON, asset
// pinning, version policy) lives in update_url.c / update_release.c / update_policy.c and is
// host-tested; this file just drives them.
//
// Memory: the one place this feature could hurt the board is internal RAM (TLS handshake, task
// stacks). Nothing large is static: the status block and the job's work area (buffers, parsed
// release, hash context) are allocated from PSRAM. The TLS task's stack is PSRAM too; mbedTLS is
// configured for external allocation (sdkconfig.defaults). The only internal allocations this file
// adds are the 4 KB stack of the lazily created flash-writer task and two binary semaphores.
#include "update_fetch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "build_info.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "kilnlink/kilnlink_version.h"
#include "http_auth_http.h" /* kiln_http_register() */
#include "ota_http.h"
#include "ota_http_internal.h" /* ota_http_get_client_ip() */
#include "relay_authority.h" /* relay_authority_heat_run_active() */
#include "stack_margin.h"
#include "dram_watch.h"
#include "time_sync.h"
#include "uart_task_ids.h"
#include "update_http_internal.h"
#include "update_policy.h"
#include "update_fetch_heap.h"
#include "update_release.h"
#include "update_settings.h"
#include "update_stage.h"
#include "update_wr_arb.h"
#include "update_url.h"
#include "zones_config_json.h"

// The TLS memory plan this file's heap budget (and the WP7 spike) rests on is option D: mbedTLS
// buffers outside internal RAM and freed after the handshake. An sdkconfig regenerated from an
// older sdkconfig.defaults silently keeps option B (internal allocation), which does not fit.
#if !CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC || !CONFIG_MBEDTLS_DYNAMIC_BUFFER
#error "WP8 needs option D (CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC and CONFIG_MBEDTLS_DYNAMIC_BUFFER); regenerate sdkconfig"
#endif

static const char *TAG = "update_fetch";

// ---- limits ---------------------------------------------------------------------------------
#define FETCH_API_BODY_CAP (256u * 1024u)    // releases/latest or releases?per_page=N JSON; PSRAM
#define FETCH_LIST_PER_PAGE 5u               // releases list size when pre-releases are allowed
#define FETCH_MANIFEST_CAP 16384u            // release.json; matches update_release.c's size cap
#define FETCH_CHUNK_LEN 4096u                // PSRAM read chunk
#define FETCH_SCRATCH_LEN FETCH_HEAP_SCRATCH_BYTES // stager scratch; INTERNAL RAM (MED-1), see update_fetch_heap.h
#define FETCH_WR_TIMEOUT_MS 30000u           // bounded wait for one flash-writer op (64 KiB erase+write is well under 1 s)
// The request buffer: a signed release-assets URL carries a JWT query, so the GET line alone can be
// well over 1 KB. esp_http_client mallocs it, and a malloc under CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL
// (8192 B) is INTERNAL heap, so it is charged to the precheck below, not to PSRAM.
#define FETCH_TX_BUF_BYTES 2048u
// Admission and mid-body abort thresholds (FETCH_HEAP_PRECHECK_MIN 28 KB on current free internal
// heap, FETCH_LARGEST_BLOCK_MIN on the largest internal block, FETCH_HEAP_ABORT_BELOW 12288 B) live
// in update_fetch_heap.h with their derivation and are host-tested. Every hop start uses the full
// admission rule; the read loop uses only the abort floor.
// The tx buffer stays 2048 B (a JWT-signed release-assets GET line can exceed 1 KB, so 1 KB would
// risk a truncated request); that costs 1 KB over the spike and is inside the worst-case draw.
#define FETCH_JOB_DEADLINE_MS (20u * 60u * 1000u)
// Per-socket-operation timeout. Cancel latency is bounded by it: the loop checks the cancel flag
// between reads, and a read may block this long, with at most FETCH_EAGAIN_RETRIES retries after a
// timed-out read, so a cancel takes effect within (1 + retries) * timeout = 10 s.
#define FETCH_HTTP_TIMEOUT_MS 5000
#define FETCH_EAGAIN_RETRIES 1u
// Stack sizes are still the WP7 spike's numbers. OWED ON THE BENCH: measure the production
// high-water marks (stack_margin reports "update_fetch" and "update_fetch_wr") over a full
// download that includes the redirect hop with a JWT-sized URL, and adjust. Not measured here.
#define FETCH_TLS_STACK_BYTES 12288          // WP7 spike high-water 7952 B (spike, not production)
#define FETCH_WR_STACK_BYTES 4096
#define FETCH_TASK_PRIO 2
#define FETCH_TASK_CORE 1                    // never core 0: spike gate (b), IDLE0 starvation

#define FETCH_JSON_REASON_MAX 96

// ---- shared state (heap, one block) ----------------------------------------------------------
typedef enum { FS_IDLE = 0, FS_CHECKING, FS_DOWNLOADING, FS_DONE, FS_FAILED } fetch_state_t;
typedef enum { KIND_CHECK = 0, KIND_DOWNLOAD } fetch_kind_t;

typedef struct {
    fetch_kind_t kind;
    bool allow_downgrade;
    bool allow_prerelease;
    bool force;
    bool claimed; // the OTA claim is held for this job and released when it ends
    char confirm[UPDATE_TAG_MAX];
} job_params_t;

typedef struct {
    fetch_state_t state;
    fetch_kind_t kind;
    const char *stage;   // static
    const char *error;   // static; "" when none
    int http_status;
    uint32_t done;
    uint32_t total;
    char repo[UPDATE_REPO_MAX];
    char tag[UPDATE_TAG_MAX];
    bool prerelease;
    uint32_t app_size;
    char running[UPDATE_VERSION_STR_MAX];
    char commit[UPDATE_COMMIT_HEX_LEN + 1];
    char sha256[UPDATE_SHA256_HEX_LEN + 1];
    const char *verdict; // static
    const char *reason;  // static
    bool allowed;
    bool needs_typed_confirm;
    bool zones_cfg_lower;
    bool have_decision;
} job_status_t;

typedef enum { WR_BEGIN = 1, WR_WRITE, WR_FINISH, WR_ABORT, WR_EXIT } wr_cmd_id_t;

typedef struct {
    wr_cmd_id_t cmd;
    const uint8_t *data;
    size_t len;
    uint32_t total;
    const char *semver;
    const char *commit;
    const update_identity_t *want; // WR_BEGIN: manifest identity for update_stage_manifest_gate
    update_stage_err_t res;
} wr_cmd_t;

typedef struct {
    volatile int busy;
    volatile bool cancel;
    SemaphoreHandle_t mx;     // guards status + params
    job_params_t params;
    job_status_t st;
    TaskHandle_t tls_task;    // stack-margin slot
    TaskHandle_t wr_task;     // stack-margin slot
    SemaphoreHandle_t wr_req;
    SemaphoreHandle_t wr_done;
    volatile update_wr_arb_t wr_arb; // guarded by s_wr_mux; see update_wr_arb.h
    volatile bool wr_wedged;  // a writer op timed out: its task/buffers are abandoned until reboot
    wr_cmd_t wr;
} fetch_ctx_t;

static fetch_ctx_t *s_c;

// ---- per-job work area (PSRAM) ---------------------------------------------------------------
typedef struct work work_t;
typedef struct {
    const char *(*begin)(work_t *w, int64_t clen); // clen < 0: unknown (chunked)
    const char *(*data)(work_t *w, const uint8_t *d, size_t n);
} sink_t;

struct work {
    job_params_t p;
    char repo[UPDATE_REPO_MAX];
    char url[UPDATE_URL_MAX];
    char loc[UPDATE_URL_MAX]; // response Location of the current hop (PSRAM, bounded)
    update_loc_capture_t loc_cap;
    update_release_info_t info;
    update_manifest_t man;
    uint8_t *body;
    size_t body_len;
    size_t mem_cap;
    TickType_t deadline_tick;
    uint32_t recvd;
    psa_hash_operation_t sha;
    bool sha_active;
    bool stage_begun;
    bool stage_done;
    uint8_t chunk[FETCH_CHUNK_LEN];
    uint8_t *scratch;                 // FETCH_SCRATCH_LEN bytes, MALLOC_CAP_INTERNAL, freed with the job
};

// ---- small helpers ---------------------------------------------------------------------------
static uint32_t free_internal(void)
{
    return (uint32_t)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static uint32_t largest_internal_block(void)
{
    return (uint32_t)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Full start/hop admission; logs the numbers on refusal. NULL when admitted, else the error name.
static const char *heap_admit(void)
{
    uint32_t f = free_internal();
    uint32_t l = largest_internal_block();
    if (update_fetch_heap_admit(f, l) == FETCH_HEAP_OK) {
        return NULL;
    }
    ESP_LOGW(TAG, "refused: internal free %u (need %u), largest block %u (need %u)", (unsigned)f,
             (unsigned)FETCH_HEAP_PRECHECK_MIN, (unsigned)l, (unsigned)FETCH_LARGEST_BLOCK_MIN);
    return "low_heap";
}

// The start-time mode gate and the heat-side fetch_busy refusal read each other's state without a
// shared lock, so a firing or autotune start that interleaves with a check/download start can let
// both through. Re-check from the job itself and give up, so the loser of that race is the fetch.
static bool heat_run_active(void)
{
    bool profile = false;
    bool autotune = false;
    relay_authority_heat_run_active(&profile, &autotune);
    return profile || autotune;
}

// SNTP has landed at least once this boot. Without a wall clock the certificate validity dates
// cannot mean anything (plan item 11), so check and download refuse until it has.
static bool clock_synced(void)
{
    time_sync_status_t ts;
    memset(&ts, 0, sizeof(ts));
    time_sync_get_status(&ts);
    return ts.ever_synced;
}

static bool deadline_passed(const work_t *w)
{
    return (int32_t)(xTaskGetTickCount() - w->deadline_tick) > 0;
}

static void st_lock(void)
{
    xSemaphoreTake(s_c->mx, portMAX_DELAY);
}
static void st_unlock(void)
{
    xSemaphoreGive(s_c->mx);
}

static void st_set_stage(const char *stage)
{
    st_lock();
    s_c->st.stage = stage;
    st_unlock();
}

static void st_set_progress(uint32_t done, uint32_t total)
{
    st_lock();
    s_c->st.done = done;
    s_c->st.total = total;
    st_unlock();
}

static void hex_lower(const uint8_t *in, size_t n, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = '\0';
}

// Copies src into dst replacing anything that would need JSON escaping.
static void json_safe_copy(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (cap == 0) {
        return;
    }
    for (; src != NULL && src[i] != '\0' && i + 1 < cap; i++) {
        unsigned char c = (unsigned char)src[i];
        dst[i] = (c < 0x20 || c >= 0x7F || c == '"' || c == '\\') ? ' ' : (char)c;
    }
    dst[i] = '\0';
}

// ---- embedded identity record ----------------------------------------------------------------
// The linker places .rodata_custom_desc directly after esp_app_desc_t (first DROM segment), so a hand
// upload's stager reads the schema versions out of the image head (update_image_id_find) instead of
// trusting headers. Needs `used`: nothing references it.
// v1 record (20 bytes, no commit) FIRST at offset 288, then the v2 record at 308 (ends 344 =
// UPDATE_STAGE_HEAD_LEN): boards running the v1-only gate (735875b6..dcd67f54) scan for the v1 magic in
// their 320-byte head and would refuse every image without one (review 7 L4).
typedef struct {
    uint32_t v1_magic, v1_zones, v1_kilnlink, v1_uart, v1_check;
    update_image_id_t v2;
} image_id_pair_t;
_Static_assert(sizeof(image_id_pair_t) == UPDATE_IMAGE_ID_V1_SIZE + UPDATE_IMAGE_ID_SIZE, "no padding");

__attribute__((section(".rodata_custom_desc"), used, aligned(4)))
const image_id_pair_t g_update_image_id = {
    .v1_magic = UPDATE_IMAGE_ID_MAGIC_V1,
    .v1_zones = ZONES_CFG_VERSION,
    .v1_kilnlink = KILNLINK_PROTOCOL_VERSION,
    .v1_uart = UART_PROTOCOL_VERSION,
    .v1_check = UPDATE_IMAGE_ID_MAGIC_V1 ^ ZONES_CFG_VERSION ^ KILNLINK_PROTOCOL_VERSION ^ UART_PROTOCOL_VERSION ^ 0xA5A5A5A5u,
    .v2 = {
        .magic = UPDATE_IMAGE_ID_MAGIC,
        .zones_cfg_version = ZONES_CFG_VERSION,
        .kilnlink_version = KILNLINK_PROTOCOL_VERSION,
        .uart_version = UART_PROTOCOL_VERSION,
        .commit = FW_GIT_COMMIT,
        .check = UPDATE_IMAGE_ID_MAGIC ^ ZONES_CFG_VERSION ^ KILNLINK_PROTOCOL_VERSION ^ UART_PROTOCOL_VERSION ^ 0xA5A5A5A5u,
    },
};

// ---- running identity ------------------------------------------------------------------------
#ifndef FW_RELEASE_VERSION
#define FW_RELEASE_VERSION ""
#endif
#ifndef FW_PARTITIONS_SHA256
#define FW_PARTITIONS_SHA256 ""
#endif

static void running_identity(update_identity_t *r, const update_identity_t *cand)
{
    memset(r, 0, sizeof(*r));
    strlcpy(r->version, FW_RELEASE_VERSION, sizeof(r->version));
    strlcpy(r->partitions_sha256, FW_PARTITIONS_SHA256, sizeof(r->partitions_sha256));
    r->zones_cfg_version = ZONES_CFG_VERSION;
    r->kilnlink_version = KILNLINK_PROTOCOL_VERSION;
    r->uart_version = UART_PROTOCOL_VERSION;
    r->dirty = FW_GIT_DIRTY != 0;
    // The build knows only a short commit; the policy compares full ids. Expand it against the
    // candidate when the candidate starts with it, else leave it unknown (a different commit).
    static const char shortc[] = FW_GIT_COMMIT;
    size_t sl = sizeof(shortc) - 1;
    if (sl >= 7 && sl <= UPDATE_COMMIT_HEX_LEN && strncasecmp(cand->commit, shortc, sl) == 0) {
        strlcpy(r->commit, cand->commit, sizeof(r->commit));
    }
}

void update_fetch_running_identity(update_identity_t *r, const char *cand_commit)
{
    update_identity_t c;
    memset(&c, 0, sizeof(c));
    if (cand_commit != NULL) {
        strlcpy(c.commit, cand_commit, sizeof(c.commit));
    }
    running_identity(r, &c);
}

// ---- flash writer task -----------------------------------------------------------------------
static portMUX_TYPE s_wr_mux = portMUX_INITIALIZER_UNLOCKED;

static void wr_task(void *arg)
{
    (void)arg;
    update_stage_t *st = update_http_stage();
    for (;;) {
        xSemaphoreTake(s_c->wr_req, portMAX_DELAY);
        wr_cmd_t *c = &s_c->wr;
        c->res = UPDATE_STAGE_OK;
        switch (c->cmd) {
        case WR_BEGIN:
            c->res = update_stage_upload_begin(st, (uint8_t *)c->data, c->len, c->total, c->semver, c->commit,
                                               STAGE_SOURCE_GITHUB);
            if (c->res == UPDATE_STAGE_OK) {
                // Cross-check the image against release.json (plan section 5): schema record and
                // descriptor version must equal the manifest's, or the stage stays blank.
                update_stage_set_gate(st, update_stage_manifest_gate, (void *)c->want);
            }
            break;
        case WR_WRITE: c->res = update_stage_upload_write(st, c->data, c->len); break;
        case WR_FINISH: c->res = update_stage_upload_finish(st); break;
        case WR_ABORT: update_stage_upload_abort(st); break;
        case WR_EXIT:
            s_c->wr_task = NULL;
            xSemaphoreGive(s_c->wr_done);
            vTaskDelete(NULL);
            return;
        }
        // Review 5 L1: one critical section decides whether the caller already timed out, so a finish in
        // the same tick as the timeout is either collected by the caller or undone here, never both
        // skipped.
        portENTER_CRITICAL(&s_wr_mux);
        const bool abandoned = update_wr_arb_writer_done(&s_c->wr_arb);
        portEXIT_CRITICAL(&s_wr_mux);
        if (abandoned) {
            // Review 3 LOW-1: this op was abandoned (the job already reported FAILED). Never let it leave
            // a valid stage behind: undo a late finish (clear) or begin/write (abort). The abort is scoped
            // to the fetch's own upload (review 5 L3) so it cannot kill a newer hand upload.
            if (c->cmd == WR_FINISH && c->res == UPDATE_STAGE_OK) {
                int clr = (int)update_stage_clear(st);
                if (clr != (int)UPDATE_STAGE_OK) {
                    ESP_LOGW(TAG, "abandoned fetch: late-finish stage clear failed (%d) -- a valid staged image remains", clr);
                }
            } else if (c->cmd == WR_BEGIN || c->cmd == WR_WRITE) {
                update_stage_upload_abort_owned(st, STAGE_SOURCE_GITHUB);
            }
        }
        xSemaphoreGive(s_c->wr_done);
    }
}

bool update_fetch_writer_wedged(void)
{
    return s_c != NULL && s_c->wr_wedged;
}

static bool wr_start(void)
{
    if (s_c->wr_wedged) {
        return false; // a previous writer op never returned; reboot to recover
    }
    s_c->wr_req = xSemaphoreCreateBinary();
    s_c->wr_done = xSemaphoreCreateBinary();
    if (s_c->wr_req == NULL || s_c->wr_done == NULL) {
        goto fail;
    }
    dram_watch_log_task("update_fetch_wr", "before-create");
    if (dram_watch_task_after("update_fetch_wr",
                              xTaskCreatePinnedToCore(wr_task, "update_fetch_wr", FETCH_WR_STACK_BYTES, NULL,
                                                      FETCH_TASK_PRIO, &s_c->wr_task, FETCH_TASK_CORE)) != pdPASS) {
        s_c->wr_task = NULL;
        goto fail;
    }
    return true;
fail:
    if (s_c->wr_req) {
        vSemaphoreDelete(s_c->wr_req);
        s_c->wr_req = NULL;
    }
    if (s_c->wr_done) {
        vSemaphoreDelete(s_c->wr_done);
        s_c->wr_done = NULL;
    }
    return false;
}

static update_stage_err_t wr_call(wr_cmd_id_t cmd, const uint8_t *data, size_t len, uint32_t total,
                                  const char *semver, const char *commit)
{
    if (s_c->wr_wedged) {
        // Before touching the command block: an abandoned writer op still owns it and reads wr.cmd after it
        // returns to pick its undo (abort vs clear). Overwriting it here (the job's final WR_ABORT) made the
        // late op skip its cleanup and leave the stage UPLOADING.
        return UPDATE_STAGE_ERR_FLASH;
    }
    s_c->wr.cmd = cmd;
    s_c->wr.data = data;
    s_c->wr.len = len;
    s_c->wr.total = total;
    s_c->wr.semver = semver;
    s_c->wr.commit = commit;
    portENTER_CRITICAL(&s_wr_mux);
    update_wr_arb_issue(&s_c->wr_arb);
    portEXIT_CRITICAL(&s_wr_mux);
    xSemaphoreGive(s_c->wr_req);
    if (xSemaphoreTake(s_c->wr_done, pdMS_TO_TICKS(FETCH_WR_TIMEOUT_MS)) != pdTRUE) {
        portENTER_CRITICAL(&s_wr_mux);
        const bool wedged = update_wr_arb_caller_timeout(&s_c->wr_arb);
        portEXIT_CRITICAL(&s_wr_mux);
        if (wedged) {
            // Wedged flash op. Never race the writer: abandon its task, semaphores and the buffers it may
            // still touch (see fetch_task), fail the job and let the caller release the update claim.
            s_c->wr_wedged = true;
            ESP_LOGE(TAG, "flash writer op %d timed out after %u ms", (int)cmd, (unsigned)FETCH_WR_TIMEOUT_MS);
            return UPDATE_STAGE_ERR_FLASH;
        }
        // Review 5 L1: the writer finished in the same instant; its give is (about to be) posted.
        (void)xSemaphoreTake(s_c->wr_done, pdMS_TO_TICKS(1000));
    }
    return s_c->wr.res;
}

static void wr_stop(void)
{
    if (s_c->wr_req == NULL || s_c->wr_wedged) {
        return;
    }
    (void)wr_call(WR_EXIT, NULL, 0, 0, NULL, NULL);
    vSemaphoreDelete(s_c->wr_req);
    vSemaphoreDelete(s_c->wr_done);
    s_c->wr_req = NULL;
    s_c->wr_done = NULL;
}

// ---- HTTP GET with allowlisted, bounded manual redirects --------------------------------------
static bool is_redirect(int s)
{
    return s == 301 || s == 302 || s == 303 || s == 307 || s == 308;
}

// esp_http_client_get_header() reads the REQUEST headers (IDF v6.0.2), so the response Location can
// only come from the header events. The capture logic itself is host-tested: update_loc_capture_*.
static esp_err_t http_event(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_HEADER && e->user_data != NULL) {
        update_loc_capture_feed((update_loc_capture_t *)e->user_data, e->header_key, e->header_value);
    }
    return ESP_OK;
}

static const char *http_get(work_t *w, const char *url, const char *accept, const sink_t *sink)
{
    update_url_err_t ue = update_url_check(url, NULL, 0);
    if (ue != UPDATE_URL_OK) {
        return update_url_err_name(ue);
    }
    if (url != w->url) {
        strlcpy(w->url, url, sizeof(w->url));
    }
    unsigned hops = 0;
    for (;;) {
        if (s_c->cancel) {
            return "cancelled";
        }
        if (deadline_passed(w)) {
            return "timeout";
        }
        if (heat_run_active()) {
            return "heat_run_active";
        }
        // Every hop opens a fresh TLS session: a handshake costs about 8-11 KB of internal heap, so
        // gating on the mid-body abort floor alone could dip under the owner's 8192 B internal
        // floor. Gate each esp_http_client_init on the full start threshold instead.
        const char *adm = heap_admit();
        if (adm != NULL) {
            return adm;
        }
        update_loc_capture_init(&w->loc_cap, w->loc, sizeof(w->loc));
        esp_http_client_config_t cfg = {
            .url = w->url,
            .timeout_ms = FETCH_HTTP_TIMEOUT_MS,
            .event_handler = http_event,
            .user_data = &w->loc_cap,
            .buffer_size = 2048,
            .buffer_size_tx = FETCH_TX_BUF_BYTES,
            .disable_auto_redirect = true,
            .crt_bundle_attach = esp_crt_bundle_attach,
        };
        esp_http_client_handle_t c = esp_http_client_init(&cfg);
        if (c == NULL) {
            return "no_memory";
        }
        esp_http_client_set_header(c, "User-Agent", "kilnCtl-update");
        esp_http_client_set_header(c, "Accept", accept);
        esp_err_t err = esp_http_client_open(c, 0);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(c);
            return "connect_failed";
        }
        int64_t clen = esp_http_client_fetch_headers(c);
        int status = esp_http_client_get_status_code(c);
        st_lock();
        s_c->st.http_status = status;
        st_unlock();
        const char *fail = NULL;
        bool again = false;
        if (clen < 0) {
            fail = "bad_response";
        } else if (is_redirect(status)) {
            bool loc_refused = false;
            const char *loc = update_loc_capture_get(&w->loc_cap, &loc_refused);
            if (loc_refused) {
                fail = "redirect_location_refused"; // too long for the buffer, or ambiguous
            } else if (loc == NULL) {
                fail = "redirect_no_location";
            } else {
                ue = update_redirect_check(hops, loc);
                if (ue != UPDATE_URL_OK) {
                    fail = update_url_err_name(ue);
                } else {
                    strlcpy(w->url, loc, sizeof(w->url));
                    hops++;
                    again = true;
                }
            }
        } else if (status != 200) {
            fail = "http_status";
        } else {
            const bool known = !esp_http_client_is_chunked_response(c);
            fail = sink->begin(w, known ? clen : -1);
            uint32_t chunks = 0;
            unsigned eagain = 0;
            int64_t total = 0;
            while (fail == NULL) {
                if (s_c->cancel) {
                    fail = "cancelled";
                    break;
                }
                if (deadline_passed(w)) {
                    fail = "timeout";
                    break;
                }
                if (update_fetch_heap_abort(free_internal())) {
                    fail = "low_heap";
                    break;
                }
                int n = esp_http_client_read(c, (char *)w->chunk, FETCH_CHUNK_LEN);
                if (n > 0) {
                    eagain = 0;
                    total += n;
                    if (known && total > clen) {
                        fail = "overrun";
                        break;
                    }
                    fail = sink->data(w, w->chunk, (size_t)n);
                    if ((++chunks & 7u) == 0) {
                        if (fail == NULL && heat_run_active()) {
                            fail = "heat_run_active";
                            break;
                        }
                        vTaskDelay(1);
                    }
                } else if (n == 0) {
                    if (!esp_http_client_is_complete_data_received(c)) {
                        fail = "short_body";
                    }
                    break;
                } else if (n == -ESP_ERR_HTTP_EAGAIN && ++eagain <= FETCH_EAGAIN_RETRIES) {
                    continue;
                } else {
                    fail = "read_failed";
                    break;
                }
            }
            if (fail == NULL && known && total != clen) {
                fail = "short_body";
            }
        }
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        if (fail != NULL) {
            return fail;
        }
        if (!again) {
            return NULL;
        }
    }
}

// ---- sinks ------------------------------------------------------------------------------------
static const char *mem_begin(work_t *w, int64_t clen)
{
    if (clen > (int64_t)w->mem_cap) {
        return "too_large";
    }
    w->body_len = 0;
    return NULL;
}

static const char *mem_data(work_t *w, const uint8_t *d, size_t n)
{
    if (w->body_len + n > w->mem_cap) {
        return "too_large";
    }
    memcpy(w->body + w->body_len, d, n);
    w->body_len += n;
    return NULL;
}

static const char *stage_begin(work_t *w, int64_t clen)
{
    if (clen >= 0 && clen != (int64_t)w->info.app_size) {
        return "size_mismatch";
    }
    if (!wr_start()) {
        if (s_c->wr_wedged) {
            return "writer_wedged_reboot_required";
        }
        return "no_memory";
    }
    w->sha = psa_hash_operation_init();
    if (psa_hash_setup(&w->sha, PSA_ALG_SHA_256) != PSA_SUCCESS) {
        return "hash_failed";
    }
    w->sha_active = true;
    s_c->wr.want = &w->man.identity;
    update_stage_err_t e = wr_call(WR_BEGIN, w->scratch, FETCH_SCRATCH_LEN, w->info.app_size,
                                   w->man.identity.version, w->man.identity.commit);
    if (e != UPDATE_STAGE_OK) {
        return update_stage_err_name(e);
    }
    w->stage_begun = true;
    w->recvd = 0;
    st_set_progress(0, w->info.app_size);
    return NULL;
}

static const char *stage_data(work_t *w, const uint8_t *d, size_t n)
{
    if (w->recvd + n > w->info.app_size) {
        return "overrun";
    }
    if (psa_hash_update(&w->sha, d, n) != PSA_SUCCESS) {
        return "hash_failed";
    }
    update_stage_err_t e = wr_call(WR_WRITE, d, n, 0, NULL, NULL);
    if (e != UPDATE_STAGE_OK) {
        return update_stage_err_name(e);
    }
    w->recvd += (uint32_t)n;
    st_set_progress(w->recvd, w->info.app_size);
    return NULL;
}

static const sink_t MEM_SINK = { mem_begin, mem_data };
static const sink_t STAGE_SINK = { stage_begin, stage_data };

// ---- the job ----------------------------------------------------------------------------------
static const char *run_job(work_t *w)
{
    const char *e;
    st_set_stage("precheck");
    if (!clock_synced()) {
        return "clock_not_synced";
    }
    const char *adm = heap_admit();
    if (adm != NULL) {
        return adm;
    }
    // WP9 publishes the repo under a writer mutex; copy it, never hold the pointer.
    char repo[UPDATE_SETTINGS_REPO_MAX_LEN + 1];
    if (!update_settings_repo_copy(repo, sizeof(repo)) || !update_repo_valid(repo)) {
        return "bad_repo";
    }
    strlcpy(w->repo, repo, sizeof(w->repo));
    st_lock();
    strlcpy(s_c->st.repo, w->repo, sizeof(s_c->st.repo));
    st_unlock();
    // /releases/latest never returns a pre-release or a draft, so when pre-releases are allowed the
    // list endpoint is read instead and the highest-semver usable release is picked from it.
    const bool use_list = w->p.allow_prerelease;
    if (!(use_list ? update_url_build_list(w->repo, FETCH_LIST_PER_PAGE, w->url, sizeof(w->url))
                   : update_url_build_latest(w->repo, w->url, sizeof(w->url)))) {
        return "bad_repo";
    }
    w->deadline_tick = xTaskGetTickCount() + pdMS_TO_TICKS(FETCH_JOB_DEADLINE_MS);

    st_set_stage("release");
    w->mem_cap = FETCH_API_BODY_CAP;
    e = http_get(w, w->url, "application/vnd.github+json", &MEM_SINK);
    if (e != NULL) {
        return e;
    }
    const uint32_t max_app = update_stage_capacity(update_http_stage());
    update_rel_err_t re = use_list ? update_release_pick_from_list((const char *)w->body, w->body_len, w->repo,
                                                                    max_app, true, &w->info)
                                   : update_release_parse_api((const char *)w->body, w->body_len, w->repo, max_app,
                                                              &w->info);
    if (re != UPDATE_REL_OK) {
        ESP_LOGW(TAG, "release parse: %s", update_rel_err_name(re));
        return update_rel_err_name(re);
    }
    st_lock();
    strlcpy(s_c->st.tag, w->info.tag, sizeof(s_c->st.tag));
    s_c->st.prerelease = w->info.prerelease;
    s_c->st.app_size = w->info.app_size;
    st_unlock();

    st_set_stage("manifest");
    w->mem_cap = FETCH_MANIFEST_CAP;
    e = http_get(w, w->info.manifest_url, "application/octet-stream", &MEM_SINK);
    if (e != NULL) {
        return e;
    }
    re = update_release_parse_manifest((const char *)w->body, w->body_len, w->repo, w->info.tag, w->info.app_size,
                                       &w->man);
    if (re != UPDATE_REL_OK) {
        ESP_LOGW(TAG, "manifest parse: %s", update_rel_err_name(re));
        return update_rel_err_name(re);
    }

    // Policy. The typed confirm is folded in here, where the tag is known: allow_downgrade only
    // counts when confirm_downgrade equals the release tag exactly. When the running version is
    // unknown (a dev build) force=1 needs the same typed confirm, because the policy cannot tell
    // whether the release is a downgrade (update_policy_decide_typed).
    update_identity_t run;
    running_identity(&run, &w->man.identity);
    const bool typed_ok = update_policy_typed_confirm_ok(w->p.confirm, w->info.tag);
    update_policy_flags_t flags = {
        .allow_prerelease = w->p.allow_prerelease,
        .allow_downgrade = w->p.allow_downgrade && typed_ok,
        .force = w->p.force,
    };
    update_decision_t d = update_policy_decide_typed(&run, &w->man.identity, &flags, typed_ok);
    st_lock();
    strlcpy(s_c->st.running, run.version, sizeof(s_c->st.running));
    strlcpy(s_c->st.commit, w->man.identity.commit, sizeof(s_c->st.commit));
    strlcpy(s_c->st.sha256, w->man.app_sha256, sizeof(s_c->st.sha256));
    s_c->st.verdict = update_verdict_name(d.verdict);
    s_c->st.reason = d.reason;
    s_c->st.allowed = d.allowed;
    s_c->st.needs_typed_confirm = d.needs_typed_confirm;
    s_c->st.zones_cfg_lower = d.zones_cfg_lower;
    s_c->st.have_decision = true;
    st_unlock();
    ESP_LOGW(TAG, "release %s: verdict %s (%s)", w->info.tag, update_verdict_name(d.verdict),
             d.reason ? d.reason : "");

    if (w->p.kind == KIND_CHECK) {
        return NULL;
    }
    if (!d.allowed) {
        return update_verdict_name(d.verdict);
    }

    st_set_stage("asset");
    e = http_get(w, w->info.app_url, "application/octet-stream", &STAGE_SINK);
    if (e != NULL) {
        return e;
    }
    if (w->recvd != w->info.app_size) {
        return "short_body";
    }
    uint8_t digest[PSA_HASH_LENGTH(PSA_ALG_SHA_256)];
    size_t dl = 0;
    psa_status_t ps = psa_hash_finish(&w->sha, digest, sizeof(digest), &dl);
    w->sha_active = false;
    if (ps != PSA_SUCCESS || dl != sizeof(digest)) {
        return "hash_failed";
    }
    char hex[2 * sizeof(digest) + 1];
    hex_lower(digest, sizeof(digest), hex);
    if (strcmp(hex, w->man.app_sha256) != 0) {
        ESP_LOGW(TAG, "asset sha256 differs from the manifest");
        return "sha256_mismatch";
    }
    st_set_stage("verify");
    update_stage_err_t se = wr_call(WR_FINISH, NULL, 0, 0, NULL, NULL);
    if (se != UPDATE_STAGE_OK) {
        return update_stage_err_name(se);
    }
    w->stage_done = true;
    return NULL;
}

static void fetch_task(void *arg)
{
    (void)arg;
    const job_params_t p = s_c->params;
    work_t *w = heap_caps_calloc(1, sizeof(*w), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    uint8_t *body = heap_caps_malloc(FETCH_API_BODY_CAP, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    // The stager scratch is internal on purpose (MED-1); one 2 KiB block per job, in the admission budget.
    uint8_t *scratch = heap_caps_malloc(FETCH_SCRATCH_LEN, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    const char *err = NULL;
    if (w == NULL || body == NULL || scratch == NULL) {
        err = "no_memory";
    } else {
        w->p = p;
        w->body = body;
        w->scratch = scratch;
        err = run_job(w);
        if (w->sha_active) {
            psa_hash_abort(&w->sha);
        }
        if (w->stage_begun && !w->stage_done) {
            (void)wr_call(WR_ABORT, NULL, 0, 0, NULL, NULL);
        }
        wr_stop();
    }
    if (s_c->wr_wedged) {
        // The writer may still be reading these; abandon them (a reboot recovers) rather than race it.
        ESP_LOGE(TAG, "flash writer wedged: leaking job buffers, update claim released");
        err = "writer_wedged_reboot_required";
    } else {
        heap_caps_free(scratch);
        heap_caps_free(body);
        heap_caps_free(w);
    }
    st_lock();
    s_c->st.error = err != NULL ? err : "";
    s_c->st.state = err != NULL ? FS_FAILED : FS_DONE;
    s_c->st.stage = "";
    st_unlock();
    if (err != NULL) {
        ESP_LOGW(TAG, "job failed: %s", err);
    } else {
        ESP_LOGW(TAG, "job done");
    }
    if (p.claimed) {
        ota_http_update_end();
    }
    s_c->tls_task = NULL;
    s_c->busy = 0;
    vTaskDeleteWithCaps(NULL);
}

// ---- routes -----------------------------------------------------------------------------------
static esp_err_t send_error_json(httpd_req_t *req, const char *status, const char *name)
{
    char json[96];
    snprintf(json, sizeof(json), "{\"ok\":false,\"error\":\"%s\"}", name);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, json);
}

static bool job_try_begin(void)
{
    return __atomic_exchange_n(&s_c->busy, 1, __ATOMIC_ACQ_REL) == 0;
}

// Fills the status for a new job, creates the TLS task, answers 202. busy is already claimed by
// the caller; on any failure everything is released again.
static esp_err_t start_job(httpd_req_t *req, const job_params_t *p)
{
    st_lock();
    s_c->params = *p;
    memset(&s_c->st, 0, sizeof(s_c->st));
    s_c->st.state = p->kind == KIND_CHECK ? FS_CHECKING : FS_DOWNLOADING;
    s_c->st.kind = p->kind;
    s_c->st.stage = "starting";
    s_c->st.error = "";
    s_c->st.verdict = "";
    s_c->st.reason = "";
    st_unlock();
    s_c->cancel = false;
    if (xTaskCreatePinnedToCoreWithCaps(fetch_task, "update_fetch", FETCH_TLS_STACK_BYTES, NULL, FETCH_TASK_PRIO,
                                        &s_c->tls_task, FETCH_TASK_CORE,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        s_c->tls_task = NULL;
        st_lock();
        s_c->st.state = FS_FAILED;
        s_c->st.error = "task_create_failed";
        st_unlock();
        if (p->claimed) {
            ota_http_update_end();
        }
        s_c->busy = 0;
        return send_error_json(req, "500 Internal Server Error", "task_create_failed");
    }
    httpd_resp_set_status(req, "202 Accepted");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true,\"started\":true}");
}

static bool query_flag(const char *q, const char *key);

static esp_err_t check_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip)); // logging only; ADMIN tier is the gate
    job_params_t p;
    memset(&p, 0, sizeof(p));
    p.kind = KIND_CHECK;
    // Same buffer and refusal as the download: a query that does not fit is a 400, never read as
    // "no flag" (that would silently fall back to /releases/latest and hide the pre-release).
    char q[160];
    q[0] = '\0';
    esp_err_t qe = httpd_req_get_url_query_str(req, q, sizeof(q));
    if (qe == ESP_OK) {
        p.allow_prerelease = query_flag(q, "allow_prerelease");
    } else if (qe != ESP_ERR_NOT_FOUND) {
        return send_error_json(req, "400 Bad Request", "bad_query");
    }
    if (!job_try_begin()) {
        return send_error_json(req, "409 Conflict", "fetch_busy");
    }
    // Same mode gate as the download (409 while a firing or autotune runs): a check opens a TLS
    // session and takes internal heap a run must not lose. The OTA claim is NOT taken.
    if (update_http_mode_gate_refuses(req, "update check", ip)) {
        s_c->busy = 0;
        return ESP_OK;
    }
    if (!clock_synced()) {
        s_c->busy = 0;
        return send_error_json(req, "409 Conflict", "clock_not_synced");
    }
    return start_job(req, &p);
}

static bool query_flag(const char *q, const char *key)
{
    char v[8];
    if (httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK) {
        return false;
    }
    return strcmp(v, "1") == 0 || strcmp(v, "true") == 0;
}

static esp_err_t download_post_handler(httpd_req_t *req)
{
    char ip[46];
    ota_http_get_client_ip(req, ip, sizeof(ip)); // logging only; ADMIN tier is the gate

    job_params_t p;
    memset(&p, 0, sizeof(p));
    p.kind = KIND_DOWNLOAD;
    char q[160];
    q[0] = '\0';
    esp_err_t qe = httpd_req_get_url_query_str(req, q, sizeof(q));
    if (qe == ESP_OK) {
        p.allow_downgrade = query_flag(q, "allow_downgrade");
        p.allow_prerelease = query_flag(q, "allow_prerelease");
        p.force = query_flag(q, "force");
        char cv[UPDATE_TAG_MAX + 1];
        esp_err_t ce = httpd_query_key_value(q, "confirm_downgrade", cv, sizeof(cv));
        if (ce == ESP_OK) {
            if (!update_tag_valid(cv)) {
                return send_error_json(req, "400 Bad Request", "bad_confirm");
            }
            strlcpy(p.confirm, cv, sizeof(p.confirm));
        } else if (ce != ESP_ERR_NOT_FOUND) {
            return send_error_json(req, "400 Bad Request", "bad_confirm");
        }
    } else if (qe != ESP_ERR_NOT_FOUND) {
        return send_error_json(req, "400 Bad Request", "bad_query");
    }

    if (!job_try_begin()) {
        return send_error_json(req, "409 Conflict", "fetch_busy");
    }
    if (!clock_synced()) {
        s_c->busy = 0;
        return send_error_json(req, "409 Conflict", "clock_not_synced");
    }
    // Same refusals, same order, as the manual upload: mode gate, OTA interlock, update claim.
    if (update_http_gate_refuses(req, "update download", ip)) {
        s_c->busy = 0;
        return ESP_OK;
    }
    p.claimed = true;
    ESP_LOGW(TAG, "download requested from %s (downgrade=%d prerelease=%d force=%d)", ip, p.allow_downgrade,
             p.allow_prerelease, p.force);
    return start_job(req, &p);
}

static esp_err_t cancel_post_handler(httpd_req_t *req)
{
    const bool was_busy = s_c->busy != 0;
    if (was_busy) {
        s_c->cancel = true;
    }
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, was_busy ? "{\"ok\":true,\"cancelling\":true}" : "{\"ok\":true,\"cancelling\":false}");
}

static const char *state_name(fetch_state_t s)
{
    switch (s) {
    case FS_IDLE: return "idle";
    case FS_CHECKING: return "checking";
    case FS_DOWNLOADING: return "downloading";
    case FS_DONE: return "done";
    case FS_FAILED: return "failed";
    }
    return "idle";
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char a[320];
    char b[768];
    char rp[UPDATE_REPO_MAX + 48];
    char repo_now[UPDATE_REPO_MAX];
    char running[UPDATE_VERSION_STR_MAX];
    char reason[FETCH_JSON_REASON_MAX];
    st_lock();
    const job_status_t *s = &s_c->st;
    snprintf(a, sizeof(a),
             "{\"ok\":true,\"state\":\"%s\",\"kind\":\"%s\",\"stage\":\"%s\",\"error\":\"%s\","
             "\"http_status\":%d,\"bytes_done\":%u,\"bytes_total\":%u,\"busy\":%s,",
             state_name(s->state), s->kind == KIND_CHECK ? "check" : "download", s->stage ? s->stage : "",
             s->error ? s->error : "", s->http_status, (unsigned)s->done, (unsigned)s->total,
             s_c->busy ? "true" : "false");
    json_safe_copy(repo_now, sizeof(repo_now), s->repo);
    json_safe_copy(running, sizeof(running), s->running);
    json_safe_copy(reason, sizeof(reason), s->reason);
    snprintf(b, sizeof(b),
             "\"tag\":\"%s\",\"prerelease\":%s,\"app_size\":%u,\"running\":\"%s\",\"commit\":\"%s\","
             "\"sha256\":\"%s\",\"verdict\":\"%s\",\"reason\":\"%s\",\"allowed\":%s,"
             "\"needs_typed_confirm\":%s,\"zones_cfg_lower\":%s}",
             s->tag, s->prerelease ? "true" : "false", (unsigned)s->app_size, running, s->commit, s->sha256,
             s->verdict ? s->verdict : "", reason, s->allowed ? "true" : "false",
             s->needs_typed_confirm ? "true" : "false", s->zones_cfg_lower ? "true" : "false");
    st_unlock();
    // Before any job has run the repo is the configured setting.
    if (repo_now[0] == '\0') {
        char cur[UPDATE_SETTINGS_REPO_MAX_LEN + 1];
        if (update_settings_repo_copy(cur, sizeof(cur))) {
            json_safe_copy(repo_now, sizeof(repo_now), cur);
        }
    }
    snprintf(rp, sizeof(rp), "\"repo\":\"%s\",", repo_now);
    httpd_resp_set_type(req, "application/json");
    if (httpd_resp_send_chunk(req, a, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, rp, HTTPD_RESP_USE_STRLEN) != ESP_OK ||
        httpd_resp_send_chunk(req, b, HTTPD_RESP_USE_STRLEN) != ESP_OK) {
        return ESP_FAIL;
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

static bool fetch_busy_probe(void)
{
    return s_c != NULL && s_c->busy != 0;
}

esp_err_t update_fetch_start(httpd_handle_t server)
{
    s_c = heap_caps_calloc(1, sizeof(*s_c), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_c == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_c->mx = xSemaphoreCreateMutex();
    if (s_c->mx == NULL) {
        heap_caps_free(s_c);
        s_c = NULL;
        return ESP_ERR_NO_MEM;
    }
    s_c->st.state = FS_IDLE;
    s_c->st.stage = "";
    s_c->st.error = "";
    s_c->st.verdict = "";
    s_c->st.reason = "";
    ota_http_set_fetch_busy_probe(fetch_busy_probe);
    (void)stack_margin_register("update_fetch", &s_c->tls_task, FETCH_TLS_STACK_BYTES);
    (void)stack_margin_register("update_fetch_wr", &s_c->wr_task, FETCH_WR_STACK_BYTES);

    static const httpd_uri_t check_uri = {
        .uri = "/api/update/check", .method = HTTP_POST, .handler = check_post_handler,
    };
    static const httpd_uri_t download_uri = {
        .uri = "/api/update/download", .method = HTTP_POST, .handler = download_post_handler,
    };
    static const httpd_uri_t status_uri = {
        .uri = "/api/update/fetch", .method = HTTP_GET, .handler = status_get_handler,
    };
    static const httpd_uri_t cancel_uri = {
        .uri = "/api/update/fetch/cancel", .method = HTTP_POST, .handler = cancel_post_handler,
    };
    const httpd_uri_t *routes[] = { &check_uri, &download_uri, &status_uri, &cancel_uri };
    for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        esp_err_t err = kiln_http_register(server, routes[i]);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register %s failed: %s", routes[i]->uri, esp_err_to_name(err));
            return err;
        }
    }
    return ESP_OK;
}
