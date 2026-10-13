// recovery_upload.c -- see recovery_upload.h.
#include "recovery_upload.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "esp_timer.h"

static const char *TAG = "recovery_upload";

// Consecutive recv timeouts tolerated (httpd recv_wait_timeout each) before
// the upload is treated as abandoned.
#define RECV_TIMEOUT_RETRIES 3

// PSRAM-first; internal fallback only if it leaves RECOVERY_INTERNAL_FLOOR_BYTES free.
static uint8_t *alloc_chunk_buffer(void)
{
    uint8_t *p = heap_caps_malloc(RECOVERY_UPLOAD_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (p) {
        return p;
    }
    if (heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) <
        RECOVERY_UPLOAD_CHUNK + RECOVERY_INTERNAL_FLOOR_BYTES) {
        return NULL;
    }
    return heap_caps_malloc(RECOVERY_UPLOAD_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

// Fills buf[0..want) from the request body. Returns bytes read (== want on
// success, less on a dead/timed-out connection).
static size_t read_exact(httpd_req_t *req, uint8_t *buf, size_t want, int64_t deadline_us, bool *too_slow)
{
    size_t got = 0;
    int timeouts = 0;
    while (got < want) {
        if (esp_timer_get_time() > deadline_us) {
            *too_slow = true; // distinct from a lost connection: the client was alive but too slow
            break; // overall upload deadline: a slow-drip client must not hold httpd forever
        }
        esp_task_wdt_reset(); // harmless ESP_ERR_NOT_FOUND if this task is not subscribed
        int n = httpd_req_recv(req, (char *)buf + got, want - got);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > RECV_TIMEOUT_RETRIES) {
                break;
            }
            continue;
        }
        if (n <= 0) {
            break;
        }
        timeouts = 0;
        got += (size_t)n;
    }
    return got;
}

recovery_upload_result_t recovery_upload_stream(httpd_req_t *req, const recovery_upload_cfg_t *cfg,
                                                const recovery_sink_t *sink, int *http_status,
                                                const char **msg)
{
    size_t total = req->content_len;
    recovery_upload_result_t result = RECOVERY_UPLOAD_OK;
    uint8_t *buf = NULL;

    // Length gate first: no allocation, no read, nothing written.
    if (total == 0 || total > cfg->max_len) {
        ric_result_t r = cfg->validate(NULL, 0, total, cfg->max_len, cfg->vctx);
        *http_status = ric_http_status(r);
        *msg = ric_message(r);
        return RECOVERY_UPLOAD_REJECTED;
    }

    const int64_t deadline_us = esp_timer_get_time() + recovery_upload_budget_us(total);
    buf = alloc_chunk_buffer();
    if (!buf) {
        *http_status = 503;
        *msg = "not enough free memory for the upload buffer";
        return RECOVERY_UPLOAD_NO_MEMORY;
    }

    size_t first = total < RECOVERY_UPLOAD_CHUNK ? total : RECOVERY_UPLOAD_CHUNK;
    bool too_slow = false;
    size_t got = read_exact(req, buf, first, deadline_us, &too_slow);
    if (got < first) {
        *http_status = too_slow ? 504 : 400;
        *msg = too_slow ? "upload too slow: overall deadline passed before the first chunk completed"
                        : "connection lost before the first chunk completed";
        result = too_slow ? RECOVERY_UPLOAD_TOO_SLOW : RECOVERY_UPLOAD_READ_ERROR;
        goto done;
    }

    ric_result_t vr = cfg->validate(buf, got, total, cfg->max_len, cfg->vctx);
    if (vr != RIC_OK) {
        *http_status = ric_http_status(vr);
        *msg = ric_message(vr);
        result = RECOVERY_UPLOAD_REJECTED;
        goto done;
    }

    if (!sink->begin(sink->ctx, total)) {
        *http_status = 500;
        *msg = "could not start the flash write";
        result = RECOVERY_UPLOAD_SINK_ERROR;
        goto done;
    }
    size_t written = 0;
    for (;;) {
        if (!sink->write(sink->ctx, buf, got)) {
            sink->abort(sink->ctx);
            *http_status = 500;
            *msg = "flash write failed";
            result = RECOVERY_UPLOAD_SINK_ERROR;
            goto done;
        }
        written += got;
        if (written >= total) {
            break;
        }
        size_t want = total - written;
        if (want > RECOVERY_UPLOAD_CHUNK) {
            want = RECOVERY_UPLOAD_CHUNK;
        }
        got = read_exact(req, buf, want, deadline_us, &too_slow);
        if (got < want) {
            sink->abort(sink->ctx);
            *http_status = too_slow ? 504 : 400;
            *msg = too_slow ? "upload too slow: overall deadline passed mid-image" : "connection lost mid-image";
            result = too_slow ? RECOVERY_UPLOAD_TOO_SLOW : RECOVERY_UPLOAD_READ_ERROR;
            goto done;
        }
    }
    if (!sink->finish(sink->ctx)) {
        // finish() has already released the sink's own handle.
        *http_status = 422;
        *msg = "image failed verification";
        result = RECOVERY_UPLOAD_VERIFY_ERROR;
    }

done:
    free(buf);
    if (result != RECOVERY_UPLOAD_OK) {
        ESP_LOGW(TAG, "upload refused/failed: %d %s", *http_status, *msg);
    }
    return result;
}

esp_err_t recovery_upload_send_error(httpd_req_t *req, int http_status, const char *msg)
{
    const char *status = "500 Internal Server Error";
    switch (http_status) {
    case 400: status = "400 Bad Request"; break;
    case 413: status = "413 Payload Too Large"; break;
    case 422: status = "422 Unprocessable Entity"; break;
    case 503: status = "503 Service Unavailable"; break;
    case 504: status = "504 Gateway Timeout"; break; // upload too slow (not 408: browsers auto-resend)
    default: break;
    }
    httpd_resp_set_status(req, status);
    // The body is usually only partly consumed; do not leave the socket open
    // for a keep-alive request that would parse leftover image bytes.
    httpd_resp_set_hdr(req, "Connection", "close");
    httpd_resp_send(req, msg, HTTPD_RESP_USE_STRLEN);
    // ESP_FAIL makes esp_http_server close the socket instead of draining
    // the (possibly huge) unread request body.
    return ESP_FAIL;
}

// --- ESP application sink -------------------------------------------------

static bool esp_begin(void *ctx, size_t total_len)
{
    recovery_esp_sink_state_t *st = ctx;
    // Sequential-writes mode: the erase is spread across esp_ota_write calls
    // (one sector at a time) instead of one long erase of the whole partition
    // inside esp_ota_begin, which would stall the httpd task and the upload.
    (void)total_len;
    if (esp_ota_begin(st->part, OTA_WITH_SEQUENTIAL_WRITES, &st->handle) != ESP_OK) {
        return false;
    }
    st->begun = true;
    return true;
}

static bool esp_write(void *ctx, const uint8_t *data, size_t len)
{
    recovery_esp_sink_state_t *st = ctx;
    return esp_ota_write(st->handle, data, len) == ESP_OK;
}

static bool esp_finish(void *ctx)
{
    recovery_esp_sink_state_t *st = ctx;
    st->begun = false; // esp_ota_end releases the handle whether or not it succeeds
    return esp_ota_end(st->handle) == ESP_OK;
}

static void esp_abort(void *ctx)
{
    recovery_esp_sink_state_t *st = ctx;
    if (st->begun) {
        esp_ota_abort(st->handle);
        st->begun = false;
    }
}

void recovery_upload_esp_sink_init(recovery_sink_t *sink, recovery_esp_sink_state_t *st,
                                   const esp_partition_t *part)
{
    memset(st, 0, sizeof(*st));
    st->part = part;
    sink->begin = esp_begin;
    sink->write = esp_write;
    sink->finish = esp_finish;
    sink->abort = esp_abort;
    sink->ctx = st;
}

ric_result_t recovery_upload_validate_esp(const uint8_t *first, size_t len, size_t content_len,
                                          size_t max_len, void *vctx)
{
    (void)vctx;
    return ric_validate_first_chunk(first, len, content_len, max_len, RIC_EXPECTED_PROJECT);
}
