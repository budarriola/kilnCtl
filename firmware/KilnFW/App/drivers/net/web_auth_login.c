// web_auth_login.c -- see web_auth_login.h for the full design rationale.
// Pure logic, no ESP-IDF/hal_kv dependency -- host-testable, same
// discipline as ota_auth.c/thermal_guard.c (App/test/build_host_tests.ps1).
#include "web_auth_login.h"

#include <string.h>

web_auth_session_role_t web_auth_login_role_for_username(const char *submitted_username,
                                                            const char *admin_username_or_null)
{
    if (!submitted_username || submitted_username[0] == '\0') {
        return WEB_AUTH_SESSION_ROLE_USER;
    }
    if (!admin_username_or_null || admin_username_or_null[0] == '\0') {
        return WEB_AUTH_SESSION_ROLE_USER;
    }
    if (strcmp(submitted_username, admin_username_or_null) == 0) {
        return WEB_AUTH_SESSION_ROLE_ADMIN;
    }
    return WEB_AUTH_SESSION_ROLE_USER;
}
