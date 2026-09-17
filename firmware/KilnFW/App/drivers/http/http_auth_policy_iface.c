// http_auth_policy_iface.c -- real implementation of the
// http_auth_policy_web_enabled() seam (http_auth_policy_iface.h), now that
// section 2/3/11's credential/policy storage has landed
// (persist/web_auth_store.h, commit c3008eb8). Replaces
// http_auth_policy_iface_stub.c, which always returned false; deleted by
// this same change per that stub's own header comment ("delete this file,
// and only this file").
//
// Deliberately calls web_auth_policy_effective_enabled() rather than
// re-deriving the ABSENT/OK/UNREADABLE collapse here -- a second copy of
// that decision is exactly the reset-one-side-of-a-pair shape CLAUDE.md
// warns about, and the header comment on this seam already spells out why
// UNREADABLE must read as enabled (fail-closed), not disabled.
#include "http_auth_policy_iface.h"

#include "web_auth_store.h"

bool http_auth_policy_web_enabled(void) {
    web_auth_policy_t policy;
    web_auth_load_status_t status = web_auth_store_load_policy(&policy);
    bool stored_enabled = (status == WEB_AUTH_LOAD_OK) ? policy.web_enabled : false;
    return web_auth_policy_effective_enabled(status, stored_enabled);
}
