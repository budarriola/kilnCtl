// security_backend_web_auth -- the REAL security_backend_vtable_t
// implementation for WEB_AUTH_PLAN.md item 6, wired to the landed
// credential-storage module (items 2/3/11, firmware/KilnFW/App/drivers/
// persist/web_auth_store.h).
//
// ESP-only (esp_fill_random() for per-write salt generation, plus whatever
// web_auth_store.c itself needs) -- NOT part of the host test build. The
// pure logic this replaces the placeholder for (security_http_core.c's
// dispatch, security_pin_is_valid(), security_timeout_minutes_is_valid())
// stays host-tested and untouched; only the vtable functions that actually
// touch storage/entropy live here.
//
// Session invalidation (item 4/5, web_auth_session.h) has not landed on
// main as of this writing -- invalidate_sessions_for_role() is a
// documented no-op below, exactly like the placeholder it replaces, until
// that module exists. Replacing that one function body is then a one-line
// change, same seam discipline as the rest of this file.
#ifndef SECURITY_BACKEND_WEB_AUTH_H
#define SECURITY_BACKEND_WEB_AUTH_H

#ifdef __cplusplus
extern "C" {
#endif

// Installs this real backend via security_backend_set_vtable(). Call once
// at startup (main_network_http.c bringup), after NVS is up. Safe to call
// more than once (idempotent); not thread-safe, same contract as
// security_backend_set_vtable() itself.
void security_backend_web_auth_install(void);

#ifdef __cplusplus
}
#endif

#endif // SECURITY_BACKEND_WEB_AUTH_H
