// recovery_upload.h -- shared streaming-upload helper for the recovery
// routes (unauthenticated since 2026-10-02: the LCD-passphrase SoftAP is the
// only access control). A route handler hands the request to
// recovery_upload_stream() with a validator and a sink:
//   - POST /api/ota/esp  -> validator = recovery_upload_validate_esp, sink =
//     the `app` OTA partition (recovery_upload_esp_sink_init()).
//   - a later Pico route -> its own validator and sink, same loop.
//
// Contract: nothing is written (sink.begin is not even called) until the first
// chunk passed the validator; any later error calls sink.abort and returns a
// failure code, never touching the boot partition. The body is streamed in
// RECOVERY_UPLOAD_CHUNK-byte pieces from a heap buffer (PSRAM-first, internal
// fallback guarded by an internal-RAM floor) -- never a stack buffer.
#ifndef RECOVERY_UPLOAD_H
#define RECOVERY_UPLOAD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "recovery_image_check.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RECOVERY_UPLOAD_CHUNK 4096u
// Internal RAM that must remain free after allocating a non-PSRAM buffer.
#define RECOVERY_INTERNAL_FLOOR_BYTES 8192u

// Overall upload deadline (HTTP audit LOW, group C 7): the per-recv timeout
// alone lets a slow-drip client hold the single httpd task for as long as it
// keeps a byte arriving inside each window. Budget = 60 s + content_len at a
// 2 KB/s minimum rate, in microseconds.
#define RECOVERY_UPLOAD_BASE_US 60000000LL
#define RECOVERY_UPLOAD_MIN_RATE_BPS 2048LL
static inline int64_t recovery_upload_budget_us(size_t content_len)
{
    return RECOVERY_UPLOAD_BASE_US + ((int64_t)content_len * 1000000LL) / RECOVERY_UPLOAD_MIN_RATE_BPS;
}

typedef struct {
    bool (*begin)(void *ctx, size_t total_len);
    bool (*write)(void *ctx, const uint8_t *data, size_t len);
    bool (*finish)(void *ctx);  // final verification; false => image rejected
    void (*abort)(void *ctx);   // safe to call after begin() succeeded; idempotent
    void *ctx;
} recovery_sink_t;

typedef struct {
    size_t max_len;  // content length limit (e.g. partition size)
    // Pure first-chunk validator. Return RIC_OK to proceed. Also called with
    // first == NULL, len == 0 for a content-length-only rejection.
    ric_result_t (*validate)(const uint8_t *first, size_t len, size_t content_len, size_t max_len,
                             void *vctx);
    void *vctx;
} recovery_upload_cfg_t;

typedef enum {
    RECOVERY_UPLOAD_OK = 0,
    RECOVERY_UPLOAD_REJECTED,     // validator said no; nothing written
    RECOVERY_UPLOAD_NO_MEMORY,    // 503
    RECOVERY_UPLOAD_READ_ERROR,   // 400, client went away / timed out; sink aborted
    RECOVERY_UPLOAD_SINK_ERROR,   // 500, sink begin/write failed; sink aborted
    RECOVERY_UPLOAD_VERIFY_ERROR, // 422, finish() rejected the image
} recovery_upload_result_t;

recovery_upload_result_t recovery_upload_stream(httpd_req_t *req, const recovery_upload_cfg_t *cfg,
                                                const recovery_sink_t *sink, int *http_status,
                                                const char **msg);

// Sends the error response for a non-OK result (status line + plain text
// body, "Connection: close"). Always returns ESP_FAIL (after the response was
// sent) so httpd closes the socket rather than draining an unread body.
esp_err_t recovery_upload_send_error(httpd_req_t *req, int http_status, const char *msg);

// ESP application sink over an OTA partition (esp_ota_begin with the known
// length / esp_ota_write / esp_ota_end; abort => esp_ota_abort). `st` must
// outlive the upload; the caller sets the boot partition itself afterwards.
typedef struct {
    const esp_partition_t *part;
    esp_ota_handle_t handle;
    bool begun;
} recovery_esp_sink_state_t;

void recovery_upload_esp_sink_init(recovery_sink_t *sink, recovery_esp_sink_state_t *st,
                                   const esp_partition_t *part);

// First-chunk validator for an ESP application image (wraps
// ric_validate_first_chunk with RIC_EXPECTED_PROJECT); vctx unused.
ric_result_t recovery_upload_validate_esp(const uint8_t *first, size_t len, size_t content_len,
                                          size_t max_len, void *vctx);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_UPLOAD_H
