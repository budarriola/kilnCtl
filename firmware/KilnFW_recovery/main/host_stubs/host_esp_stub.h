// host_esp_stub.h -- minimal host (MSVC) stand-ins for the ESP-IDF APIs that
// recovery_upload.c calls, so its streaming loop can be tested with no board.
// Used ONLY by check_recovery_upload.ps1 (-I host_stubs); never built into the
// firmware (CMakeLists.txt does not list this directory). The behaviour is
// scripted by test_recovery_upload.c through the g_* knobs declared here.
#ifndef HOST_ESP_STUB_H
#define HOST_ESP_STUB_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL (-1)
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

// ---- esp_http_server ----
typedef struct {
    size_t content_len;
} httpd_req_t;
#define HTTPD_SOCK_ERR_TIMEOUT (-3)
#define HTTPD_RESP_USE_STRLEN (-1)
int httpd_req_recv(httpd_req_t *r, char *buf, size_t len);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, int len);

// ---- esp_partition / esp_ota_ops ----
typedef struct {
    int id;
    uint32_t address;
    int subtype;
    char label[17];
} esp_partition_t;
typedef int esp_ota_handle_t;
#define OTA_WITH_SEQUENTIAL_WRITES 0xFFFFFFFFu
esp_err_t esp_ota_begin(const esp_partition_t *p, size_t image_size, esp_ota_handle_t *h);
esp_err_t esp_ota_write(esp_ota_handle_t h, const void *data, size_t size);
esp_err_t esp_ota_end(esp_ota_handle_t h);
esp_err_t esp_ota_abort(esp_ota_handle_t h);
esp_err_t esp_ota_set_boot_partition(const esp_partition_t *p);
const esp_partition_t *esp_ota_get_boot_partition(void);

// ---- esp_heap_caps ----
#define MALLOC_CAP_SPIRAM 1u
#define MALLOC_CAP_INTERNAL 2u
#define MALLOC_CAP_8BIT 4u
void *heap_caps_malloc(size_t size, uint32_t caps);
size_t heap_caps_get_free_size(uint32_t caps);

// ---- esp_log / esp_task_wdt ----
#define ESP_LOGW(tag, ...) ((void)(tag))
#define ESP_LOGE(tag, ...) ((void)(tag))
esp_err_t esp_task_wdt_reset(void);

// ---- scripted behaviour (defined in test_recovery_upload.c) ----
typedef struct {
    int kind;           // 0 = deliver data, 1 = timeout, 2 = hard error / closed
    size_t max_bytes;   // for kind 0: cap per call (0 = as many as asked)
} host_recv_step_t;
extern const host_recv_step_t *g_recv_script; // NULL => always deliver
extern size_t g_recv_script_len;              // after the script ends: closed (-1)
extern const uint8_t *g_body;                 // the request body bytes
extern size_t g_body_len;
extern size_t g_body_pos;

extern bool g_spiram_fail;
extern size_t g_internal_free;

extern int g_ota_begin_calls, g_ota_write_calls, g_ota_end_calls, g_ota_abort_calls;
extern int g_ota_set_boot_calls;
extern int g_ota_write_fail_at; // 1-based write call that fails (0 = never)
extern bool g_ota_begin_fail, g_ota_end_fail;
extern size_t g_ota_bytes_written;

extern int g_status_set_calls;
extern char g_last_status[64];
extern char g_last_hdr_field[32], g_last_hdr_value[32];
extern int g_send_calls;

#endif // HOST_ESP_STUB_H
