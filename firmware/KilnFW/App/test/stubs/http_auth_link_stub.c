// Shared link-only stub bodies for esp_http_server.h symbols that
// http_auth_http.c's resolve_role_for_request() calls (httpd_req_get_hdr_value_len/
// _str, ota_http_get_client_ip) but which host-test executables that link
// http_auth_http.c WITHOUT also linking the real ota_http.c (only
// test_ota_http.c does that, via a direct #include, and supplies its own
// test-controllable versions of these same three symbols) have no other
// source for. Not test-controllable on purpose -- these executables
// (main/zones_http/safety_cfg_http/profiles_http/partition_info_http/
// profile_export_import/safety_stack_margin_http) don't exercise
// resolve_role_for_request()'s header-parsing path directly; they only need
// http_auth_http.c/http_auth_enforce.c to link. If a future test in one of
// them needs to control these headers, give that executable its own
// test-local override instead of complicating this shared one (same
// precedent as test_ota_http.c not using this file).
#include "esp_http_server.h"
#include <string.h>

size_t httpd_req_get_hdr_value_len(httpd_req_t *r, const char *field)
{
    (void)r;
    (void)field;
    return 0;
}

/* Optional per-test header hook (NULL = no headers present, the old behaviour). */
esp_err_t (*g_test_get_hdr_hook)(const char *field, char *val, size_t val_size) = NULL;

esp_err_t httpd_req_get_hdr_value_str(httpd_req_t *r, const char *field, char *val, size_t val_size)
{
    (void)r;
    if (g_test_get_hdr_hook) {
        return g_test_get_hdr_hook(field, val, val_size);
    }
    if (val && val_size > 0) {
        val[0] = '\0';
    }
    return ESP_FAIL;
}

void ota_http_get_client_ip(httpd_req_t *req, char *out, size_t out_len)
{
    (void)req;
    if (out && out_len > 0) {
        strncpy(out, "0.0.0.0", out_len - 1);
        out[out_len - 1] = '\0';
    }
}
