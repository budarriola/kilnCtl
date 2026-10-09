// http_auth_disclosure_gate.c -- see http_auth_disclosure_gate.h's header
// comment for why this exists as its own translation unit and why the
// expression below must never be reshaped.
#include "http_auth_disclosure_gate.h"

#include "http_auth_http.h"
#include "http_auth_policy_iface.h"

bool http_auth_may_disclose(httpd_req_t *req)
{
    return !http_auth_policy_web_enabled() || http_auth_caller_is_admin(req);
}
