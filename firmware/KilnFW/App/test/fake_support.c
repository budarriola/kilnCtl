// Fakes for test_update_fetch.c: everything update_fetch.c / update_http.c reach that is not the code under
// test (see stubs_update_fetch/fake_support.h). The FreeRTOS pieces are real Windows threads, semaphores and
// spin locks so the TLS-task / flash-writer-task interplay (abandon, wedge) runs for real.
#include <windows.h>
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fake_support.h"

#include "esp_crt_bundle.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/portmacro.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "psa/crypto.h"

#include "http_auth_http.h"
#include "ota_http.h"
#include "ota_http_internal.h"
#include "ota_state.h"
#include "relay_authority.h"
#include "stack_margin.h"
#include "system_mode_gate_http.h"
#include "time_sync.h"
#include "update_settings.h"
#include "wifi_provision_http.h"

// ---------------------------------------------------------------------------------------------------------
// state
size_t g_fr_route_n;
fr_route_t g_fr_routes[FR_MAX_ROUTES];
int g_fr_open_count;
char g_fr_last_url[512];

uint8_t g_fr_flash[FR_STAGE_SIZE];
bool g_fr_flash_write_fail;
bool g_fr_flash_write_block;
volatile long g_fr_flash_blocked_n;
int g_fr_flash_erase_n, g_fr_flash_write_n;
static HANDLE s_flash_ev;

bool g_fr_claim_deny;
bool g_fr_claim_held;
int g_fr_claim_begin_n, g_fr_claim_end_n;
int g_fr_interlock_result;
bool g_fr_heat_profile, g_fr_heat_autotune;
bool g_fr_clock_synced;
char g_fr_repo[96];
size_t g_fake_heap_free_internal, g_fake_heap_largest_internal;
bool g_fr_internal_alloc_fail;

const char *g_fr_fail_task;
int g_fr_time_div = 1;
volatile long g_fr_live_tasks;
int g_fr_psa_setup_fail;

#define FR_MAX_HANDLERS 16
static httpd_uri_t s_handlers[FR_MAX_HANDLERS];
static int s_handler_n;

void fr_flash_release(void)
{
    g_fr_flash_write_block = false;
    if (s_flash_ev != NULL) {
        SetEvent(s_flash_ev);
    }
}

void fr_flash_erase_all(void)
{
    memset(g_fr_flash, 0xFF, sizeof(g_fr_flash));
}

void fr_reset(void)
{
    g_fr_route_n = 0;
    memset(g_fr_routes, 0, sizeof(g_fr_routes));
    g_fr_open_count = 0;
    g_fr_last_url[0] = 0;
    fr_flash_erase_all();
    g_fr_flash_write_fail = false;
    g_fr_flash_write_block = false;
    g_fr_flash_erase_n = g_fr_flash_write_n = 0;
    if (s_flash_ev == NULL) {
        s_flash_ev = CreateEvent(NULL, TRUE, FALSE, NULL);
    }
    ResetEvent(s_flash_ev);
    g_fr_claim_deny = false;
    g_fr_claim_held = false;
    g_fr_claim_begin_n = g_fr_claim_end_n = 0;
    g_fr_interlock_result = 0; // OTA_INTERLOCK_OK
    g_fr_heat_profile = g_fr_heat_autotune = false;
    g_fr_clock_synced = true;
    strcpy(g_fr_repo, "budarriola/kilnCtl");
    g_fake_heap_free_internal = 200000;
    g_fake_heap_largest_internal = 100000;
    g_fr_internal_alloc_fail = false;
    g_fr_fail_task = NULL;
    g_fr_time_div = 1;
    g_fr_psa_setup_fail = 0;
}

// ---------------------------------------------------------------------------------------------------------
// heap
volatile long g_fr_heap_live;
void *heap_caps_malloc(size_t size, uint32_t caps)
{
    if ((caps & MALLOC_CAP_INTERNAL) && g_fr_internal_alloc_fail) {
        return NULL;
    }
    void *p = malloc(size);
    if (p != NULL) {
        InterlockedIncrement(&g_fr_heap_live);
    }
    return p;
}
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps)
{
    if ((caps & MALLOC_CAP_INTERNAL) && g_fr_internal_alloc_fail) {
        return NULL;
    }
    void *p = calloc(n, size);
    if (p != NULL) {
        InterlockedIncrement(&g_fr_heap_live);
    }
    return p;
}
void heap_caps_free(void *p)
{
    if (p != NULL) {
        InterlockedDecrement(&g_fr_heap_live);
    }
    free(p);
}
size_t heap_caps_get_free_size(uint32_t caps) { return (caps & MALLOC_CAP_INTERNAL) ? g_fake_heap_free_internal : 4000000u; }
size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    return (caps & MALLOC_CAP_INTERNAL) ? g_fake_heap_largest_internal : 2000000u;
}
bool esp_ptr_external_ram(const void *p)
{
    (void)p;
    return false;
}
size_t strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size > 0) {
        size_t c = n >= size ? size - 1 : n;
        memcpy(dst, src, c);
        dst[c] = 0;
    }
    return n;
}

// ---------------------------------------------------------------------------------------------------------
// FreeRTOS on Windows
typedef struct {
    TaskFunction_t fn;
    void *arg;
} thr_t;

static unsigned __stdcall thr_main(void *p)
{
    thr_t t = *(thr_t *)p;
    free(p);
    t.fn(t.arg);
    InterlockedDecrement(&g_fr_live_tasks);
    return 0;
}

static BaseType_t task_create(TaskFunction_t fn, const char *name, void *arg, TaskHandle_t *out)
{
    if (out) {
        *out = NULL;
    }
    if (g_fr_fail_task != NULL && name != NULL && strcmp(g_fr_fail_task, name) == 0) {
        return pdFAIL;
    }
    thr_t *t = malloc(sizeof(*t));
    t->fn = fn;
    t->arg = arg;
    InterlockedIncrement(&g_fr_live_tasks);
    uintptr_t h = _beginthreadex(NULL, 0, thr_main, t, 0, NULL);
    if (h == 0) {
        InterlockedDecrement(&g_fr_live_tasks);
        free(t);
        return pdFAIL;
    }
    CloseHandle((HANDLE)h);
    if (out) {
        *out = (TaskHandle_t)1; // non-NULL: stack_margin never reads it in a host build
    }
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t task, const char *name, unsigned long stack, void *arg,
                                   UBaseType_t prio, TaskHandle_t *out, BaseType_t core)
{
    (void)stack; (void)prio; (void)core;
    return task_create(task, name, arg, out);
}
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t task, const char *name, unsigned long stack, void *arg,
                                           UBaseType_t prio, TaskHandle_t *out, BaseType_t core, uint32_t caps)
{
    (void)stack; (void)prio; (void)core; (void)caps;
    return task_create(task, name, arg, out);
}
void vTaskDelete(TaskHandle_t t)
{
    if (t == NULL) {
        InterlockedDecrement(&g_fr_live_tasks);
        _endthreadex(0);
    }
}
void vTaskDeleteWithCaps(TaskHandle_t t) { vTaskDelete(t); }
TickType_t xTaskGetTickCount(void) { return (TickType_t)GetTickCount(); }
void vTaskDelay(TickType_t ticks) { Sleep(ticks); }
TaskHandle_t xTaskGetCurrentTaskHandle(void) { return (TaskHandle_t)1; }
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t t)
{
    (void)t;
    return 0;
}

bool fr_wait_tasks_idle(int ms)
{
    for (int i = 0; i < ms; i++) {
        if (g_fr_live_tasks == 0) {
            return true;
        }
        Sleep(1);
    }
    return g_fr_live_tasks == 0;
}

SemaphoreHandle_t xSemaphoreCreateBinary(void) { return CreateSemaphore(NULL, 0, 1, NULL); }
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return CreateSemaphore(NULL, 1, 1, NULL); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t ticks)
{
    DWORD ms = ticks == portMAX_DELAY ? INFINITE : (DWORD)(ticks / (TickType_t)g_fr_time_div);
    return WaitForSingleObject((HANDLE)s, ms) == WAIT_OBJECT_0 ? pdTRUE : pdFALSE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { return ReleaseSemaphore((HANDLE)s, 1, NULL) ? pdTRUE : pdFALSE; }
void vSemaphoreDelete(SemaphoreHandle_t s) { CloseHandle((HANDLE)s); }

void fr_crit_enter(portMUX_TYPE *m)
{
    while (InterlockedCompareExchange(m, 1, 0) != 0) {
        Sleep(0);
    }
}
void fr_crit_exit(portMUX_TYPE *m) { InterlockedExchange(m, 0); }

// ---------------------------------------------------------------------------------------------------------
// SHA-256 (FIPS 180-4) behind the psa hash API
static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98,
    0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8,
    0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819,
    0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
    0xc67178f2,
};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha_block(psa_hash_operation_t *s, const uint8_t *p)
{
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) | ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR(w[i - 15], 7) ^ ROR(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR(w[i - 2], 17) ^ ROR(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4], f = s->h[5], g = s->h[6], h = s->h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = h + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}

psa_status_t psa_hash_setup(psa_hash_operation_t *op, psa_algorithm_t alg)
{
    static const uint32_t iv[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    if (alg != PSA_ALG_SHA_256 || g_fr_psa_setup_fail) {
        g_fr_psa_setup_fail = 0;
        return PSA_ERROR_GENERIC_ERROR;
    }
    memset(op, 0, sizeof(*op));
    memcpy(op->h, iv, sizeof(iv));
    op->active = 1;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_update(psa_hash_operation_t *op, const uint8_t *in, size_t len)
{
    if (!op->active) {
        return PSA_ERROR_GENERIC_ERROR;
    }
    op->total += len;
    while (len > 0) {
        size_t n = 64 - op->buflen;
        if (n > len) {
            n = len;
        }
        memcpy(op->buf + op->buflen, in, n);
        op->buflen += (uint32_t)n;
        in += n;
        len -= n;
        if (op->buflen == 64) {
            sha_block(op, op->buf);
            op->buflen = 0;
        }
    }
    return PSA_SUCCESS;
}
psa_status_t psa_hash_finish(psa_hash_operation_t *op, uint8_t *hash, size_t hash_size, size_t *hash_length)
{
    if (!op->active || hash_size < 32) {
        return PSA_ERROR_GENERIC_ERROR;
    }
    uint64_t bits = op->total * 8;
    uint8_t pad = 0x80;
    psa_hash_update(op, &pad, 1);
    pad = 0;
    while (op->buflen != 56) {
        psa_hash_update(op, &pad, 1);
    }
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) {
        lenb[i] = (uint8_t)(bits >> (56 - 8 * i));
    }
    psa_hash_update(op, lenb, 8);
    for (int i = 0; i < 8; i++) {
        hash[4 * i] = (uint8_t)(op->h[i] >> 24);
        hash[4 * i + 1] = (uint8_t)(op->h[i] >> 16);
        hash[4 * i + 2] = (uint8_t)(op->h[i] >> 8);
        hash[4 * i + 3] = (uint8_t)op->h[i];
    }
    if (hash_length) {
        *hash_length = 32;
    }
    op->active = 0;
    return PSA_SUCCESS;
}
psa_status_t psa_hash_abort(psa_hash_operation_t *op)
{
    op->active = 0;
    return PSA_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------
// scripted esp_http_client
struct esp_http_client {
    esp_http_client_config_t cfg;
    const fr_route_t *route;
    size_t pos;
    bool read_once;
};

static const fr_route_t *route_for(const char *url)
{
    for (size_t i = 0; i < g_fr_route_n; i++) {
        if (strcmp(g_fr_routes[i].url, url) == 0) {
            return &g_fr_routes[i];
        }
    }
    return NULL;
}

esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config)
{
    struct esp_http_client *c = calloc(1, sizeof(*c));
    c->cfg = *config;
    return c;
}
esp_err_t esp_http_client_set_header(esp_http_client_handle_t c, const char *k, const char *v)
{
    (void)c; (void)k; (void)v;
    return ESP_OK;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len)
{
    (void)write_len;
    g_fr_open_count++;
    snprintf(g_fr_last_url, sizeof(g_fr_last_url), "%s", c->cfg.url);
    c->route = route_for(c->cfg.url);
    if (c->route == NULL || c->route->open_fail) {
        return ESP_FAIL;
    }
    return ESP_OK;
}
int64_t esp_http_client_fetch_headers(esp_http_client_handle_t c)
{
    const fr_route_t *r = c->route;
    if (r->location != NULL && c->cfg.event_handler != NULL) {
        esp_http_client_event_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.event_id = HTTP_EVENT_ON_HEADER;
        ev.header_key = (char *)"Location";
        ev.header_value = (char *)r->location;
        ev.user_data = c->cfg.user_data;
        c->cfg.event_handler(&ev);
    }
    if (r->advertised == -4) {
        return -1;
    }
    if (r->advertised == -1) {
        return 0; // chunked
    }
    return r->advertised == -2 ? (int64_t)r->body_len : r->advertised;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) { return c->route->status; }
bool esp_http_client_is_chunked_response(esp_http_client_handle_t c) { return c->route->advertised == -1; }
static size_t deliverable(const fr_route_t *r)
{
    return r->deliver == FR_ALL || r->deliver > r->body_len ? r->body_len : r->deliver;
}
int esp_http_client_read(esp_http_client_handle_t c, char *buf, int len)
{
    if (!c->read_once) {
        c->read_once = true;
        if (c->route->on_read != NULL) {
            c->route->on_read();
        }
    }
    size_t avail = deliverable(c->route) - c->pos;
    size_t n = avail < (size_t)len ? avail : (size_t)len;
    if (n > 0) {
        memcpy(buf, c->route->body + c->pos, n);
        c->pos += n;
    }
    return (int)n;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) { return c->pos >= c->route->body_len; }
esp_err_t esp_http_client_close(esp_http_client_handle_t c)
{
    (void)c;
    return ESP_OK;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c)
{
    free(c);
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------------------------
// stage partition
static const esp_partition_t s_stage_part = { .address = 0, .size = FR_STAGE_SIZE, .label = "stage",
                                              .type = ESP_PARTITION_TYPE_DATA, .subtype = 0x06 };
static const esp_partition_t s_app_part = { .address = 0x10000, .size = 0x400000, .label = "app",
                                            .type = ESP_PARTITION_TYPE_APP, .subtype = ESP_PARTITION_SUBTYPE_APP_OTA_0 };

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char *label)
{
    (void)type; (void)subtype;
    return label != NULL && strcmp(label, "stage") == 0 ? &s_stage_part : NULL;
}
const esp_partition_t *esp_ota_get_running_partition(void) { return &s_app_part; }
const esp_app_desc_t *esp_app_get_description(void) { return NULL; }

static void maybe_block(void)
{
    if (g_fr_flash_write_block) {
        InterlockedIncrement(&g_fr_flash_blocked_n);
        WaitForSingleObject(s_flash_ev, INFINITE);
        InterlockedDecrement(&g_fr_flash_blocked_n);
    }
}
esp_err_t esp_partition_erase_range(const esp_partition_t *p, size_t off, size_t len)
{
    (void)p;
    g_fr_flash_erase_n++;
    if (g_fr_flash_write_fail || off + len > FR_STAGE_SIZE) {
        return ESP_FAIL;
    }
    memset(g_fr_flash + off, 0xFF, len);
    return ESP_OK;
}
esp_err_t esp_partition_write(const esp_partition_t *p, size_t off, const void *src, size_t len)
{
    (void)p;
    maybe_block();
    g_fr_flash_write_n++;
    if (g_fr_flash_write_fail || off + len > FR_STAGE_SIZE) {
        return ESP_FAIL;
    }
    const uint8_t *s = src;
    for (size_t i = 0; i < len; i++) {
        g_fr_flash[off + i] &= s[i]; // NOR: writes only clear bits
    }
    return ESP_OK;
}
esp_err_t esp_partition_read(const esp_partition_t *p, size_t off, void *dst, size_t len)
{
    if (p == &s_app_part) {
        memset(dst, 0, len);
        return ESP_OK;
    }
    if (off + len > FR_STAGE_SIZE) {
        return ESP_FAIL;
    }
    memcpy(dst, g_fr_flash + off, len);
    return ESP_OK;
}

// ---------------------------------------------------------------------------------------------------------
// update claim, interlocks, mode, clock, misc firmware services
bool ota_http_update_try_begin(ota_http_context_t ctx)
{
    (void)ctx;
    if (g_fr_claim_deny || g_fr_claim_held) {
        return false;
    }
    g_fr_claim_begin_n++;
    g_fr_claim_held = true;
    return true;
}
void ota_http_update_end(void)
{
    g_fr_claim_end_n++;
    g_fr_claim_held = false;
}
bool ota_http_update_in_progress(ota_http_context_t *out)
{
    (void)out;
    return g_fr_claim_held;
}
ota_interlock_result_t ota_http_check_interlocks(bool ack, char *reason, size_t cap)
{
    (void)ack;
    if (g_fr_interlock_result != 0 && reason != NULL && cap > 0) {
        snprintf(reason, cap, "fake interlock");
    }
    return (ota_interlock_result_t)g_fr_interlock_result;
}
bool ota_http_req_ack_no_safety(httpd_req_t *req)
{
    (void)req;
    return false;
}
static bool (*s_busy_probe)(void);
void ota_http_set_fetch_busy_probe(bool (*probe)(void)) { s_busy_probe = probe; }
void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t n)
{
    (void)req;
    snprintf(out, n, "127.0.0.1");
}
void relay_authority_heat_run_active(bool *profile, bool *autotune)
{
    *profile = g_fr_heat_profile;
    *autotune = g_fr_heat_autotune;
}
void time_sync_get_status(time_sync_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->ever_synced = g_fr_clock_synced;
}
bool stack_margin_register(const char *name, void *slot, uint32_t bytes)
{
    (void)name; (void)slot; (void)bytes;
    return true;
}
bool update_settings_repo_copy(char *out, size_t cap)
{
    if (g_fr_repo[0] == 0) {
        return false;
    }
    snprintf(out, cap, "%s", g_fr_repo);
    return true;
}
httpd_handle_t wifi_provision_http_get_server(void)
{
    static int dummy;
    return (httpd_handle_t)&dummy;
}
esp_err_t kiln_http_register(httpd_handle_t server, const httpd_uri_t *u)
{
    (void)server;
    if (s_handler_n >= FR_MAX_HANDLERS) {
        return ESP_FAIL;
    }
    s_handlers[s_handler_n++] = *u;
    return ESP_OK;
}
httpd_uri_t fr_find_handler(const char *uri, int method)
{
    for (int i = 0; i < s_handler_n; i++) {
        if (strcmp(s_handlers[i].uri, uri) == 0 && (int)s_handlers[i].method == method) {
            return s_handlers[i];
        }
    }
    httpd_uri_t none;
    memset(&none, 0, sizeof(none));
    return none;
}

// ---------------------------------------------------------------------------------------------------------
// httpd recorder
static fake_req_t *fq(httpd_req_t *r) { return (fake_req_t *)r; }

void fr_req_init(fake_req_t *r, int method)
{
    memset(r, 0, sizeof(*r));
    r->base.method = method;
    r->recv_fail_at = -1;
}
void fr_req_header(fake_req_t *r, const char *k, const char *v)
{
    r->hdr_k[r->hdr_n] = k;
    r->hdr_v[r->hdr_n] = v;
    r->hdr_n++;
}
void fr_req_body(fake_req_t *r, const uint8_t *body, size_t len)
{
    r->body = body;
    r->body_len = len;
    r->base.content_len = (long long)len;
}
int fr_status_code(const fake_req_t *r) { return r->status[0] ? atoi(r->status) : 200; }

static void append(fake_req_t *r, const char *s, size_t n)
{
    if (r->resp_len + n < sizeof(r->resp)) {
        memcpy(r->resp + r->resp_len, s, n);
        r->resp_len += n;
        r->resp[r->resp_len] = 0;
    }
}
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *s)
{
    snprintf(fq(r)->status, sizeof(fq(r)->status), "%s", s);
    return ESP_OK;
}
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *t)
{
    snprintf(fq(r)->ctype, sizeof(fq(r)->ctype), "%s", t);
    return ESP_OK;
}
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *f, const char *v)
{
    (void)r; (void)f; (void)v;
    return ESP_OK;
}
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long n)
{
    append(fq(r), buf, n < 0 ? strlen(buf) : (size_t)n);
    return ESP_OK;
}
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s) { return httpd_resp_send(r, s, -1); }
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t n)
{
    if (buf != NULL) {
        append(fq(r), buf, (n == (size_t)-1 || (long long)n < 0) ? strlen(buf) : n);
    }
    return ESP_OK;
}
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t e, const char *msg)
{
    snprintf(fq(r)->status, sizeof(fq(r)->status), "%d err", (int)e);
    if (msg != NULL) {
        append(fq(r), msg, strlen(msg));
    }
    return ESP_OK;
}
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len)
{
    fake_req_t *q = fq(r);
    size_t limit = q->body_len;
    if (q->recv_fail_at >= 0) {
        if (q->body_pos >= (size_t)q->recv_fail_at) {
            return -1;
        }
        limit = (size_t)q->recv_fail_at;
    }
    size_t n = limit - q->body_pos;
    if (n > len) {
        n = len;
    }
    memcpy(buf, q->body + q->body_pos, n);
    q->body_pos += n;
    return (int)n;
}
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    fake_req_t *q = fq(r);
    for (int i = 0; i < q->hdr_n; i++) {
        if (_stricmp(q->hdr_k[i], field) == 0) {
            return strlen(q->hdr_v[i]);
        }
    }
    return 0;
}
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t cap)
{
    fake_req_t *q = fq(r);
    for (int i = 0; i < q->hdr_n; i++) {
        if (_stricmp(q->hdr_k[i], field) == 0) {
            if (strlen(q->hdr_v[i]) >= cap) {
                return ESP_ERR_HTTPD_RESULT_TRUNC;
            }
            strcpy(val, q->hdr_v[i]);
            return ESP_OK;
        }
    }
    return ESP_ERR_NOT_FOUND;
}
int httpd_req_to_sockfd(httpd_req_t *r)
{
    (void)r;
    return 1;
}
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t cap)
{
    fake_req_t *q = fq(r);
    if (q->query[0] == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    if (strlen(q->query) >= cap) {
        return ESP_ERR_HTTPD_RESULT_TRUNC;
    }
    strcpy(buf, q->query);
    return ESP_OK;
}
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t cap)
{
    size_t kl = strlen(key);
    const char *p = qry;
    while (*p) {
        const char *end = strchr(p, '&');
        size_t seg = end ? (size_t)(end - p) : strlen(p);
        if (seg > kl && strncmp(p, key, kl) == 0 && p[kl] == '=') {
            size_t vl = seg - kl - 1;
            if (vl >= cap) {
                return ESP_ERR_HTTPD_RESULT_TRUNC;
            }
            memcpy(val, p + kl + 1, vl);
            val[vl] = 0;
            return ESP_OK;
        }
        p += seg + (end ? 1 : 0);
    }
    return ESP_ERR_NOT_FOUND;
}
esp_err_t system_mode_gate_http_send_refusal(httpd_req_t *req, const char *reason)
{
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, reason);
}
esp_err_t ota_http_send_interlock_refusal(httpd_req_t *req, ota_interlock_result_t r, const char *reason)
{
    (void)r;
    httpd_resp_set_status(req, "409 Conflict");
    return httpd_resp_sendstr(req, reason ? reason : "interlock");
}
esp_err_t ota_http_send_json_clamped(httpd_req_t *req, const char *buf, int n, size_t cap)
{
    (void)cap;
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, buf, n);
}
esp_err_t ota_http_refusal_drain(httpd_req_t *req, uint8_t *buf, size_t cap)
{
    (void)req; (void)buf; (void)cap;
    return ESP_OK;
}
uint8_t *ota_http_esp_chunk_buf(size_t *cap)
{
    static uint8_t buf[4096];
    *cap = sizeof(buf);
    return buf;
}
void ota_http_hex_encode(const uint8_t *in, size_t len, char *out)
{
    static const char hx[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = hx[in[i] >> 4];
        out[2 * i + 1] = hx[in[i] & 15];
    }
    out[2 * len] = 0;
}
