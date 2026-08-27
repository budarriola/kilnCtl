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

#include "esp_err.h"

typedef struct httpd_handle_obj *httpd_handle_t;
typedef struct httpd_req {
    int dummy;
    long long content_len; /* matches real esp_http_server's httpd_req_t::content_len (size_t) closely
                             * enough for req->content_len comparisons/casts in backup_http.c -- signed
                             * so a test can also express "no body" as <= 0 the same way real code checks. */
} httpd_req_t;

typedef enum {
    HTTP_GET = 0,
    HTTP_POST = 1,
} httpd_method_t;

typedef enum {
    HTTPD_400_BAD_REQUEST = 400,
    HTTPD_403_FORBIDDEN = 403, /* added 2026-08-27 for ota_http.c's host tests */
    HTTPD_500_INTERNAL_SERVER_ERROR = 500,
} httpd_err_code_t;

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
 * etc. read the X-Ota-Mac header and the socket fd. Same "declared once
 * here, defined once per test file" split as the rest of this header. */
size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size);
int httpd_req_to_sockfd(httpd_req_t *r);

#endif // TEST_STUB_ESP_HTTP_SERVER_H
