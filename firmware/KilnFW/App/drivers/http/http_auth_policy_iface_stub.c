// http_auth_policy_iface_stub.c -- placeholder for http_auth_policy_web_enabled()
// (http_auth_policy_iface.h) until section 2/11's real auth_policy NVS
// record lands. Delete only this file once that module supplies the same
// symbol; the header is the seam and stays.
#include "http_auth_policy_iface.h"

bool http_auth_policy_web_enabled(void) {
    return false; // shipped default: web auth off, section 11
}
