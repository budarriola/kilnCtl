// Control API of the update_fetch host test's fakes (fake_support.c): a scripted HTTP client, an in-memory
// stage flash, Windows-thread FreeRTOS task/semaphore stand-ins, a counting update claim, and a httpd request
// recorder. The code under test (update_fetch.c, update_http.c, update_stage.c, ...) is the real thing.
#ifndef UPDATE_FETCH_FAKE_SUPPORT_H
#define UPDATE_FETCH_FAKE_SUPPORT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"

// ---- scripted HTTP routes (exact URL match) ----
#define FR_ALL ((size_t)-1)
typedef struct {
    const char *url;
    int status;               // HTTP status served
    const uint8_t *body;
    size_t body_len;
    int64_t advertised;       // Content-Length: >=0 as given; -1 = chunked; -2 = body_len
    size_t deliver;           // bytes actually handed out before the connection "ends" (FR_ALL = body_len)
    const char *location;     // Location header (redirects)
    bool open_fail;           // esp_http_client_open() fails
    void (*on_read)(void);    // called once, at the first read of this response
} fr_route_t;

#define FR_MAX_ROUTES 16
extern size_t g_fr_route_n;
extern fr_route_t g_fr_routes[FR_MAX_ROUTES];
extern int g_fr_open_count;                // successful or failed opens, any URL
extern char g_fr_last_url[512];

// ---- stage flash (partition "stage") ----
#define FR_STAGE_SIZE (4096u + 262144u)
extern uint8_t g_fr_flash[FR_STAGE_SIZE];
extern bool g_fr_flash_write_fail;
extern bool g_fr_flash_write_block;        // writes block until fr_flash_release()
extern volatile long g_fr_flash_blocked_n; // writers currently parked
extern int g_fr_flash_erase_n, g_fr_flash_write_n;
void fr_flash_release(void);

// ---- update claim / interlock / mode / clock / heap ----
extern bool g_fr_claim_deny;
extern bool g_fr_claim_held;
extern int g_fr_claim_begin_n, g_fr_claim_end_n;
extern int g_fr_interlock_result;          // ota_interlock_result_t value
extern bool g_fr_heat_profile, g_fr_heat_autotune;
extern bool g_fr_clock_synced;
extern char g_fr_repo[96];
extern size_t g_fake_heap_free_internal, g_fake_heap_largest_internal;
extern bool g_fr_internal_alloc_fail;

// ---- tasks ----
extern const char *g_fr_fail_task;         // xTaskCreate*() with this name fails
extern int g_fr_time_div;                  // semaphore waits are divided by this (30 s wedge timeout -> ms)
extern volatile long g_fr_live_tasks;
bool fr_wait_tasks_idle(int ms);           // true once every created task has returned or deleted itself

// ---- httpd request/response recorder ----
typedef struct {
    httpd_req_t base;
    const char *hdr_k[4];
    const char *hdr_v[4];
    int hdr_n;
    char query[256];
    const uint8_t *body;
    size_t body_len, body_pos;
    int recv_fail_at;                      // recv returns -1 once this many bytes were handed out (-1 = never)
    char status[64];
    char ctype[64];
    char resp[16384];
    size_t resp_len;
} fake_req_t;

void fr_req_init(fake_req_t *r, int method);
void fr_req_header(fake_req_t *r, const char *k, const char *v);
void fr_req_body(fake_req_t *r, const uint8_t *body, size_t len);
httpd_uri_t fr_find_handler(const char *uri, int method);   // .handler NULL when not registered
int fr_status_code(const fake_req_t *r);                    // 200 when no status was set

void fr_reset(void);                       // everything back to a clean boot (does not undo update_fetch_start())
void fr_flash_erase_all(void);

#endif
