// login_backoff.h -- the escalating per-account/per-source backoff ladder
// (owner decision 2026-09-21, firmware/KilnFW/App/drivers/http/web_auth_login_http.c's
// header comment): "Auth should allow a fast logon. Then if a wrong password
// comes from an ip wait an increasing amount of time before the next is
// accepted. 1st retry 5 sec, second 10, then 30, 60, then 5 min before the
// cycle resets."
//
// Extracted 2026-09-28 (owner decision: "apply the web password policies to
// the LCD PIN too") from web_auth_login_http.c, where this ladder and its
// four stepping functions used to be file-static, so the LCD PIN keypad
// (lcd_auth_state.c) can mirror the web login's ACTUAL CURRENT lockout
// behavior by calling the same functions against its own, separate
// login_backoff_state_t instance -- not by copying the numbers into a second
// definition that could quietly drift from this one. Per
// docs/WEB_AUTH_PLAN.md section 7, each authentication surface keeps its own
// lockout STATE (the LCD's instance and the web login's per-IP table entries
// are never the same memory, never compared, never merged); only the POLICY
// -- this ladder and the pure functions that walk it -- is now shared.
//
// Before this extraction, the LCD PIN reused ota_auth_lockout_state_t (the
// OTA surface's 3-failures/60s-doubling-to-900s-ceiling scheme) instead --
// that was already a drift bug in its own right, since the web login itself
// migrated OFF that same shared scheme on 2026-09-21 and onto this ladder,
// and nothing updated the LCD to follow. See lcd_auth_state.h's lockout
// field for the fix.
//
// Pure, no ESP-IDF/mbedTLS/LVGL dependency -- host-testable exactly like
// ota_auth.h (see App/test/build_host_tests.ps1 and test_login_backoff.c).
#ifndef KILNCTL_LOGIN_BACKOFF_H
#define KILNCTL_LOGIN_BACKOFF_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// One 1-based step per consecutive failure: 1st failure -> 5s, 2nd -> 10s,
// 3rd -> 30s, 4th -> 60s, 5th and beyond -> 300s (clamped to the last entry).
extern const uint32_t LOGIN_BACKOFF_LADDER_MS[5];
#define LOGIN_BACKOFF_LADDER_LEN 5u

typedef struct {
    uint32_t failure_count;   // consecutive failures since the last success or cycle reset,
                                // 0..LOGIN_BACKOFF_LADDER_LEN
    uint32_t locked_until_ms; // 0 = not currently locked
} login_backoff_state_t;

// True iff `s` is currently locked (now_ms has not yet reached locked_until_ms).
bool login_backoff_is_locked(const login_backoff_state_t *s, uint32_t now_ms);

// Must be called before consulting is_locked()/failure_count on any read
// path -- once the ladder's last step has both been reached AND its lock has
// expired, this is the point where "the cycle resets": the next failure
// starts over at the 5s step rather than staying pinned at the 300s tier
// forever.
void login_backoff_cycle_reset_if_due(login_backoff_state_t *s, uint32_t now_ms);

// Records one failed attempt: advances (and clamps) the consecutive-failure
// count and imposes the matching ladder step's wait, starting now. Callers
// must never call this for an attempt that was itself refused by
// is_locked() first.
void login_backoff_record_failure(login_backoff_state_t *s, uint32_t now_ms);

// A successful attempt clears the ladder entirely.
void login_backoff_record_success(login_backoff_state_t *s);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_LOGIN_BACKOFF_H
