// Host-test stub -- see stubs/esp_err.h for why these exist. Added
// 2026-08-21 for wifi_prov.c's host tests (reached via the real
// wifi_provision_http.h, which wifi_prov.c #includes). Nothing here was
// exercised by those tests.
//
// Extended same day for backup_http.c's host test (test_backup_import.c):
// that file includes backup_http.c directly to reach backup_import_apply(),
// a static function with no public seam, and backup_http.c's OTHER handlers
// (the page GET, the export GET, the import POST wrapper) are compiled
// alongside it in the same translation unit even though the test never
// calls them -- so this stub must supply enough of esp_http_server's real
// surface for the whole file to COMPILE and LINK, not just the one function
// under test. None of httpd_register_uri_handler/httpd_resp_*/httpd_req_recv
// below is ever invoked by test_backup_import.c; their bodies (in that test
// file) only need to exist for the linker.
#ifndef TEST_STUB_ESP_HTTP_SERVER_H
#define TEST_STUB_ESP_HTTP_SERVER_H

#include <stddef.h>
#include <stdlib.h>

#include "esp_err.h"

typedef struct httpd_handle_obj *httpd_handle_t;
typedef struct httpd_req {
    int dummy;
    int method; /* real httpd_req_t::method (HTTP_GET == 0): the request line's method */
    long long content_len; /* matches real esp_http_server's httpd_req_t::content_len (size_t) closely
                             * enough for req->content_len comparisons/casts in backup_http.c -- signed
                             * so a test can also express "no body" as <= 0 the same way real code checks. */
    // Matches real esp_http_server's httpd_req_t::user_ctx -- added for
    // kiln_http_register()'s pre-handler (http_auth_http.c), which reads and
    // overwrites this field to smuggle its own wrapper context through
    // httpd_register_uri_handler() and restore the real handler's original
    // user_ctx before dispatching to it.
    void *user_ctx;
    // Matches real esp_http_server's httpd_req_t::handle -- added 2026-09-28
    // for http_async_job.c's A4 review follow-up C fix
    // (httpd_sess_trigger_close() needs the owning handle, not just a
    // sockfd).
    httpd_handle_t handle;
} httpd_req_t;

#ifndef ESP_ERR_HTTPD_RESULT_TRUNC
#define ESP_ERR_HTTPD_RESULT_TRUNC 0xB003 /* value is irrelevant to the host tests */
#endif

typedef enum {
    HTTP_GET = 0,
    HTTP_POST = 1,
    HTTP_HEAD = 2,
} httpd_method_t;

typedef enum {
    HTTPD_400_BAD_REQUEST = 400,
    HTTPD_401_UNAUTHORIZED = 401, /* added for web_auth_login_http.c's host tests (Findings 4/5) */
    HTTPD_408_REQ_TIMEOUT = 408, /* added for the overall upload deadline (ota_http_esp/pico) */
    HTTPD_403_FORBIDDEN = 403, /* added 2026-08-27 for ota_http.c's host tests */
    HTTPD_404_NOT_FOUND = 404, /* added 2026-09-27 for kiln_cfg_http.c's host tests (test_kiln_cfg_http.c) */
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
} httpd_err_code_t;

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#endif

/* Real esp_http_server.h's sentinel meaning "buf is a NUL-terminated C
 * string, compute its length with strlen()" -- added 2026-08-27 for
 * ota_http.c's host tests (test_ota_http.c), which link ota_http.c for real
 * and so need this macro to resolve even though the call sites that use it
 * are never reached by these tests. */
#define HTTPD_RESP_USE_STRLEN ((long long)-1)

typedef esp_err_t (*httpd_uri_handler_t)(httpd_req_t *req);

typedef struct {
    const char *uri;
    httpd_method_t method;
    httpd_uri_handler_t handler;
    void *user_ctx;
} httpd_uri_t;

/* Declared here (matching real esp_http_server.h) so backup_http.c's other
 * handlers compile; DEFINED in test_backup_import.c, since this stub header
 * is included by more than one test file and must not multiply-define. */
esp_err_t httpd_register_uri_handler(httpd_handle_t handle, const httpd_uri_t *uri_handler);
esp_err_t httpd_resp_set_type(httpd_req_t *r, const char *type);
esp_err_t httpd_resp_set_hdr(httpd_req_t *r, const char *field, const char *value);
esp_err_t httpd_resp_send(httpd_req_t *r, const char *buf, long long buf_len);
esp_err_t httpd_resp_send_chunk(httpd_req_t *r, const char *buf, size_t buf_len);
esp_err_t httpd_resp_send_err(httpd_req_t *r, httpd_err_code_t error, const char *msg);
esp_err_t httpd_resp_set_status(httpd_req_t *r, const char *status);
int httpd_req_recv(httpd_req_t *r, char *buf, size_t buf_len);
/* Added 2026-08-21 for zones_http.c's host test (test_zones_http.c) -- that
 * file's zones_post_handler() calls this on success. Declared here only;
 * definition lives in whichever test .c file first needs it to link (same
 * "declared once, defined per-executable" split as the rest of this header --
 * see this header's own comment). */
esp_err_t httpd_resp_sendstr(httpd_req_t *r, const char *s);

/* Added 2026-08-27 for ota_http.c's host tests (test_ota_http.c) -- that
 * file's get_client_ip()/ota_http_authenticate_request()/ota_esp_post_handler()
 * etc. read request headers and the socket fd (the X-Ota-Mac header these
 * used to read for the AP-password HMAC scheme is gone -- retired
 * 2026-09-29, WEB_AUTH_PLAN.md item 2b). Same "declared once here, defined
 * once per test file" split as the rest of this header. */
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size);
int httpd_req_to_sockfd(httpd_req_t *r);

/* Added 2026-09-27 for kiln_cfg_http.c's host tests (test_kiln_cfg_http.c) --
 * list_get_handler()'s ?id= query-string parsing. Declared once here,
 * defined per test file, same split as the group above. */
esp_err_t httpd_req_get_url_query_str(httpd_req_t *r, char *buf, size_t buf_len);
esp_err_t httpd_query_key_value(const char *qry, const char *key, char *val, size_t val_size);

/* Added 2026-09-25 for http_async_job.c's host tests
 * (test_http_async_job.c) -- the first driver code to call the real
 * ESP-IDF async-handoff pair (esp_http_server.h:856/873). Defined here as
 * static inline (not "declared once, defined per test file" like the
 * group above) because their behavior is fixed and shared rather than
 * per-test-customized: begin() heap-allocates a copy of *r (a dummy
 * identity is enough -- host tests are single-threaded and nothing here
 * distinguishes one httpd_req_t's contents from another beyond what the
 * caller put in user_ctx/content_len, which the copy preserves), unless a
 * test has set g_test_stub_async_begin_should_fail, matching how a real
 * board can run out of the fixed session/async slot pool. complete() frees
 * the copy and counts itself in g_test_stub_async_complete_calls so a test
 * can assert begin/complete stay paired on every code path, including a
 * dropped complete() (the required negative test). selectany: this header
 * is included by more than one .c file in the same test executable, same
 * reasoning as freertos/semphr.h's g_test_stub_lock_depth. */
__declspec(selectany) int g_test_stub_async_begin_should_fail = 0;
__declspec(selectany) int g_test_stub_async_complete_calls = 0;

static inline esp_err_t httpd_req_async_handler_begin(httpd_req_t *r, httpd_req_t **out)
{
    if (g_test_stub_async_begin_should_fail) {
        if (out) {
            *out = NULL;
        }
        return ESP_FAIL;
    }
    httpd_req_t *copy = (httpd_req_t *)calloc(1, sizeof(httpd_req_t));
    if (!copy) {
        if (out) {
            *out = NULL;
        }
        return ESP_ERR_NO_MEM;
    }
    if (r) {
        *copy = *r;
    }
    if (out) {
        *out = copy;
    }
    return ESP_OK;
}

static inline esp_err_t httpd_req_async_handler_complete(httpd_req_t *r)
{
    g_test_stub_async_complete_calls++;
    free(r);
    return ESP_OK;
}

/* Added 2026-09-28 for http_async_job.c's A4 review follow-up C fix -- the
 * xTaskCreate()-failure path force-closes the session so an unread request
 * body can never be mistaken for the start of the next keep-alive request.
 * Same "static inline, fixed/shared behavior, selectany counter" convention
 * as the async begin/complete pair just above: records (handle, sockfd) of
 * its last call so a test can assert it fired, with no real socket to touch
 * on the host. */
__declspec(selectany) int g_test_stub_sess_trigger_close_calls = 0;
__declspec(selectany) httpd_handle_t g_test_stub_sess_trigger_close_last_handle = NULL;
__declspec(selectany) int g_test_stub_sess_trigger_close_last_sockfd = -1;

static inline esp_err_t httpd_sess_trigger_close(httpd_handle_t handle, int sockfd)
{
    g_test_stub_sess_trigger_close_calls++;
    g_test_stub_sess_trigger_close_last_handle = handle;
    g_test_stub_sess_trigger_close_last_sockfd = sockfd;
    return ESP_OK;
}

#endif // TEST_STUB_ESP_HTTP_SERVER_H
