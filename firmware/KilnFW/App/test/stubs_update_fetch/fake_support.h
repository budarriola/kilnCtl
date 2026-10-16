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
    const char *retry_after;  // Retry-After header value
    const char *ratelimit_reset; // X-RateLimit-Reset header value
    const char *ratelimit_remaining; // X-RateLimit-Remaining header value
    int stall_per_chunk;      // >0: this many EAGAIN reads (each read_stall_ms, default 5 ms) before every delivered chunk
    long read_stall_ms;       // nonzero: each read sleeps this long (<0: the client slice) then reports EAGAIN until stall_reads exhausted
    int stall_reads;          // number of stalled reads (0 with read_stall_ms = stall forever)
    bool open_fail;           // esp_http_client_open() fails
    void (*on_read)(void);    // called once, at the first read of this response
    void (*on_open)(void);    // called at every esp_http_client_open() of this URL
} fr_route_t;

#define FR_MAX_ROUTES 16
extern size_t g_fr_route_n;
extern fr_route_t g_fr_routes[FR_MAX_ROUTES];
extern int g_fr_open_count;                // successful or failed opens, any URL
extern char g_fr_last_url[512];
extern int g_fr_live_clients;             // esp_http_client_init() handles not yet cleaned up
extern int g_fr_max_live_clients;         // high-water mark of g_fr_live_clients since fr_reset()
extern int g_fr_open_while_live;          // opens issued while another client handle was still live

// ---- stage flash (partition "stage") ----
#define FR_STAGE_SIZE (4096u + 262144u)
extern uint8_t g_fr_flash[FR_STAGE_SIZE];
extern bool g_fr_flash_write_fail;
extern bool g_fr_flash_write_block;        // writes block until fr_flash_release()
extern volatile long g_fr_flash_blocked_n; // writers currently parked
extern volatile long g_fr_sem_idle_waiters; // tasks currently blocked in an unbounded (portMAX_DELAY) take
extern volatile long g_fr_sem_bad_n; // take/give on a NULL or deleted semaphore
extern volatile long g_fr_wr_wait_after_wedge; // writer-op waits begun after the wedge latched
extern bool g_fr_wr_missed_give;            // the writer's next completion give is dropped (LOW-1 case)
extern volatile long g_fr_wr_give_dropped;  // 1 once that give was dropped
extern int g_fr_flash_erase_n, g_fr_flash_write_n;
void fr_flash_release(void);

// ---- update claim / interlock / mode / clock / heap ----
extern bool g_fr_claim_deny;
extern bool g_fr_claim_held;
extern int g_fr_claim_begin_n, g_fr_claim_end_n;
extern int g_fr_interlock_result;          // ota_interlock_result_t value
extern bool g_fr_heat_profile, g_fr_heat_autotune;
extern bool g_fr_clock_synced;
extern long long g_fr_now_epoch;
extern long long g_fr_tick_skew_ms;       // added to the fake tick count (lets a test outwait a backoff)
extern volatile long g_fr_hdr_eagain_n; // EAGAIN results handed out by header reads
extern volatile long g_fr_eagain_n;  // EAGAIN results handed out
extern char g_fr_repo[96];
extern size_t g_fake_heap_free_internal, g_fake_heap_largest_internal;
extern size_t g_fake_heap_dma_free_internal;
extern int g_fake_heap_recover_reads;
// vTaskDelay observers (REVIEW_UPDFX3 LOW-3): calls and total ticks since fr_reset(); g_fr_on_delay, when set,
// runs inside every delay with the 1-based call number (a test cancels the job from it).
extern int g_fr_delay_calls;
extern unsigned long g_fr_delay_total_ticks;
extern void (*g_fr_on_delay)(int call_no);
// Scripted DMA-free per body read (UPDFX4 MED-3): once armed (a route's on_read sets g_fr_dma_seq_armed), the
// Nth read sets the DMA-capable internal free to g_fr_dma_seq[N]; after the script it returns to 100000.
#define FR_DMA_SEQ_MAX 16
extern size_t g_fr_dma_seq[FR_DMA_SEQ_MAX];
extern int g_fr_dma_seq_n, g_fr_dma_seq_i;
extern bool g_fr_dma_seq_armed;
// Scripted DMA-free per heap_caps_get_free_size(DMA|INTERNAL) CALL (UPDFX8): the Nth call returns [N]; afterwards the static value.
extern size_t g_fr_dma_call_seq[FR_DMA_SEQ_MAX];
extern int g_fr_dma_call_seq_n, g_fr_dma_call_seq_i;
// Scripted internal-free per heap_caps_get_free_size(INTERNAL) CALL (UPDFX9): once g_fr_dma_call_seq_i has reached
// g_fr_int_call_after (i.e. that many DMA readings were taken), the next calls return [0], [1], ...; then the static value.
extern size_t g_fr_int_call_seq[FR_DMA_SEQ_MAX];
extern int g_fr_int_call_seq_n, g_fr_int_call_seq_i, g_fr_int_call_after;
extern size_t g_fake_heap_recover_to;
extern volatile long g_fr_internal_mallocs;
extern bool g_fr_internal_alloc_fail;
extern bool g_fr_alloc_fail_all;           // every heap_caps_malloc fails (reset by fr_reset only if the test clears it)
extern volatile long g_fr_heap_live;        // heap_caps_* blocks currently allocated (leak checks)

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
