#include "system_mode_gate_http.h"

#include <string.h>

esp_err_t system_mode_gate_http_send_refusal(httpd_req_t *req, const char *reason)
{
    httpd_resp_set_status(req, "409 Conflict");
    httpd_resp_set_type(req, "text/plain");
    const char *body = (reason != NULL) ? reason : "refused -- system mode does not permit this action right now";
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}
