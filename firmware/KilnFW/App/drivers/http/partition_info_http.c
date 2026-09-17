#include "partition_info_http.h"

#include <stdbool.h>
#include <stdio.h>

#include "esp_http_server.h"
#include "http_auth_http.h" // kiln_http_register() -- WEB_AUTH_PLAN.md section 5
#include "esp_log.h"
#include "esp_partition.h"

#include "hal_sysinfo.h" /* hal_sysinfo_get_running_partition() -- the "running" field below */
#include "wifi_provision_http.h"

static const char *TAG = "partition_info_http";

/* One partition entry's JSON, plus room for the ",[" / running-slot framing
 * around it. A real label is at most 16 bytes (esp_partition_t::label, per
 * ESP-IDF); this buffer is generous for one entry either way. Sending the
 * response as a series of httpd_resp_send_chunk() calls -- one chunk for
 * the opening brace + running-slot label, one per partition entry, one for
 * the closing bracket -- means there is no fixed-size whole-response
 * buffer to overrun regardless of how many partitions the table holds
 * (unlike thermo_faults_get_handler()'s single 1536 B buffer in
 * diagnostics_http.c, which silently truncates past that many channels).
 * The flash sector reserved for the table (0x1000 B / 32 B per entry) caps
 * it at 128 possible entries, but this handler does not need to know that
 * number or size a buffer to it. */
#define ENTRY_BUF_SIZE 192

static esp_err_t send_chunk(httpd_req_t *req, const char *buf, size_t len)
{
    esp_err_t err = httpd_resp_send_chunk(req, buf, len);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "httpd_resp_send_chunk failed: %s", esp_err_to_name(err));
    }
    return err;
}

/* Appends every partition of one type (APP or DATA) to the in-progress
 * chunked response. Returns ESP_OK once the whole type has been walked
 * (an empty type -- e.g. no APP partitions found -- is not an error, same
 * as esp_partition_find() itself never treating "no match" as a failure),
 * or the first httpd_resp_send_chunk() failure. *first tracks whether a
 * leading comma is needed, shared across both type passes so the emitted
 * array has no trailing/leading stray comma regardless of which type is
 * walked first or is empty. */
static esp_err_t emit_partitions_of_type(httpd_req_t *req, esp_partition_type_t type, bool *first)
{
    esp_partition_iterator_t it = esp_partition_find(type, ESP_PARTITION_SUBTYPE_ANY, NULL);
    esp_err_t err = ESP_OK;

    while (it != NULL) {
        const esp_partition_t *p = esp_partition_get(it);
        char buf[ENTRY_BUF_SIZE];
        int n = snprintf(buf, sizeof(buf),
                          "%s{\"label\":\"%s\",\"type\":%u,\"subtype\":%u,\"offset\":%lu,\"size\":%lu,\"encrypted\":%s}",
                          *first ? "" : ",", p->label, (unsigned)p->type, (unsigned)p->subtype,
                          (unsigned long)p->address, (unsigned long)p->size, p->encrypted ? "true" : "false");
        *first = false;

        if (n < 0 || (size_t)n >= sizeof(buf)) {
            /* A label longer than ESP-IDF's own 16-byte cap would need
             * this -- treat it as a real failure rather than silently
             * truncating a partition entry into invalid JSON. */
            ESP_LOGE(TAG, "partition entry '%s' did not fit ENTRY_BUF_SIZE=%d", p->label, ENTRY_BUF_SIZE);
            err = ESP_ERR_INVALID_SIZE;
            break;
        }

        err = send_chunk(req, buf, (size_t)n);
        if (err != ESP_OK) {
            break;
        }

        it = esp_partition_next(it);
    }

    /* esp_partition_iterator_release(NULL) is documented safe in ESP-IDF
     * (a no-op) -- both the "walked to the end" (it == NULL already) and
     * "bailed out early on an error" (it still non-NULL) paths call it
     * exactly once here, so no path can leak the iterator's internal
     * linked-list allocation. */
    esp_partition_iterator_release(it);
    return err;
}

/* GET /api/partitions -- the live partition table esp_partition already
 * parsed for this running app, plus which OTA slot is running. See this
 * file's header comment for why this replaces a JTAG flash read of
 * 0x8000 rather than fixing that read. */
static esp_err_t api_partitions_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");

    hal_sysinfo_partition_info_t running;
    bool have_running = (hal_sysinfo_get_running_partition(&running) == HAL_OK);
    char head[ENTRY_BUF_SIZE];
    int n = snprintf(head, sizeof(head), "{\"running\":\"%s\",\"partitions\":[",
                      have_running ? running.label : "");
    if (n < 0 || (size_t)n >= sizeof(head)) {
        ESP_LOGE(TAG, "running-partition label did not fit head buffer");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "partition label too long");
        return ESP_FAIL;
    }
    if (send_chunk(req, head, (size_t)n) != ESP_OK) {
        return ESP_FAIL;
    }

    bool first = true;
    if (emit_partitions_of_type(req, ESP_PARTITION_TYPE_APP, &first) != ESP_OK) {
        /* Best-effort: still try to close the response cleanly rather than
         * leaving the client hanging on a half-sent chunked body. */
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_FAIL;
    }
    if (emit_partitions_of_type(req, ESP_PARTITION_TYPE_DATA, &first) != ESP_OK) {
        httpd_resp_send_chunk(req, NULL, 0);
        return ESP_FAIL;
    }

    if (send_chunk(req, "]}", 2) != ESP_OK) {
        return ESP_FAIL;
    }
    /* Terminates the chunked response (a zero-length chunk), same
     * convention backup_http.c's backup_stream_flush() callers use. */
    return httpd_resp_send_chunk(req, NULL, 0);
}

esp_err_t partition_info_http_start(void)
{
    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t api_uri = {
        .uri = "/api/partitions", .method = HTTP_GET, .handler = api_partitions_get_handler,
    };
    esp_err_t err = kiln_http_register(server, &api_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/partitions) failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}
