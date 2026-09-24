// totp_http_core.h -- pure, host-testable logic behind auth_totp_http.c.
// docs/TOTP_PASSWORD_RESET_PLAN.md WT-A part 2.
//
// PURE, NO I/O, NO ESP-IDF DEPENDENCY, same split as net/totp.h /
// persist/totp_config.h -- CLAUDE.md's "HTTP handlers are target-build
// only" note means auth_totp_http.c's httpd glue itself cannot link into
// the host-test build, so the three things that plan's acceptance criteria
// need tested (the single-use/expiring reset-token table, the RAM-only
// pending-enrollment-secret state machine, and the "503 before anything
// else" clock-sync ordering) live here instead, taking already-produced
// random bytes/hex strings and an injected `now_ms` rather than generating
// either themselves.
#ifndef TOTP_HTTP_CORE_H
#define TOTP_HTTP_CORE_H

#include <stdbool.h>
#include <stdint.h>

#include "net/totp.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- 503-before-anything-else ordering (plan section 6a) -------------------

// Both /api/auth/forgot and /api/auth/reset must refuse with 503 the moment
// the board's clock has never been SNTP-synced, before any lockout check,
// body parse, or TOTP verification runs -- an unsynced clock makes
// totp_verify()'s time-step math meaningless, so nothing downstream of this
// check may run first. Trivial by itself; named and tested so the ordering
// itself (not just the boolean) is pinned down.
static inline bool totp_http_clock_ready(bool sntp_ever_synced)
{
    return sntp_ever_synced;
}

// --- RAM-only pending-enrollment-secret state machine -----------------------
//
// Enrollment begin() generates a candidate secret and holds it here, in RAM
// only, until confirm() proves one valid code against it -- the secret is
// never persisted to NVS before that proof (plan section 6a's enrollment
// note). Zeroed on disable, on a fresh begin() overwriting a still-pending
// one, and lazily on the next touch past TOTP_PENDING_TTL_MS ("timeout"):
// there is no background timer, so a pending secret past its TTL is only
// actually wiped the next time is_valid()/clear() is called on it, which
// every route that touches pending state does before trusting it.
#define TOTP_PENDING_TTL_MS 300000u // 5 minutes to scan the QR code and confirm

typedef struct {
    bool active;
    uint8_t secret[TOTP_SECRET_LEN];
    uint32_t started_at_ms;
} totp_pending_enrollment_t;

// Resets to the empty/inactive state, zeroing any secret bytes present.
void totp_pending_clear(totp_pending_enrollment_t *p);

// Starts (or restarts) a pending enrollment with `secret`. Overwrites and
// zeroes any previous pending secret first -- at most one pending secret
// exists at a time, matching this board's single-administrator scope.
void totp_pending_begin(totp_pending_enrollment_t *p, const uint8_t secret[TOTP_SECRET_LEN],
                         uint32_t now_ms);

// True iff a pending secret is present AND has not exceeded
// TOTP_PENDING_TTL_MS as of `now_ms`. A stale pending secret is zeroed as a
// side effect of this call (the lazy-timeout point described above) and this
// then returns false.
bool totp_pending_is_valid(totp_pending_enrollment_t *p, uint32_t now_ms);

// --- Reset-token table (plan section 6a) ------------------------------------
//
// A short-lived, single-use, single-administrator-scoped cache binding a
// verified-TOTP-code event to a bearer token /api/auth/reset later consumes.
// The token's own 128 bits of entropy carry the actual security weight (see
// auth_totp_http.c, which is the only caller that ever generates one) -- this
// table only tracks which of a small fixed set of outstanding tokens are
// still active, unused, and unexpired.
#define TOTP_RESET_TOKEN_HEX_LEN 32u // 16 raw bytes, hex-encoded
#define TOTP_RESET_TOKEN_SLOTS 4u
#define TOTP_RESET_TOKEN_TTL_MS 120000u // 2 minutes, plan section 6a
#define TOTP_RESET_USERNAME_MAX 32u

typedef struct {
    bool active;
    bool used;
    char token_hex[TOTP_RESET_TOKEN_HEX_LEN + 1];
    char username[TOTP_RESET_USERNAME_MAX + 1];
    uint32_t expires_at_ms;
} totp_reset_token_slot_t;

typedef struct {
    totp_reset_token_slot_t slots[TOTP_RESET_TOKEN_SLOTS];
} totp_reset_token_table_t;

void totp_reset_token_table_init(totp_reset_token_table_t *t);

// Stores a new active, unused token bound to `username`, expiring
// TOTP_RESET_TOKEN_TTL_MS from `now_ms`. If every slot is occupied by a
// still-active, unexpired, unused token, evicts the one closest to
// expiring -- this table is a short-lived cache, not an audit log, and an
// evicted caller can simply call /api/auth/forgot again.
void totp_reset_token_store(totp_reset_token_table_t *t, const char *token_hex,
                             const char *username, uint32_t now_ms);

typedef enum {
    TOTP_RESET_TOKEN_OK,           // found, unused, unexpired, username matched -- now marked used
    TOTP_RESET_TOKEN_NOT_FOUND,    // no active slot matches this (token, username) pair
    TOTP_RESET_TOKEN_EXPIRED,      // matched but past its TTL
    TOTP_RESET_TOKEN_ALREADY_USED, // matched but already consumed once
} totp_reset_token_result_t;

// Looks up `token_hex` bound to `username`; on TOTP_RESET_TOKEN_OK marks the
// slot used atomically with the lookup (single-use, no verify-then-consume
// window for a caller to race), same "verify persists before returning OK"
// discipline as totp_config_verify_and_consume(). Every other result leaves
// the table unchanged.
totp_reset_token_result_t totp_reset_token_consume(totp_reset_token_table_t *t, const char *token_hex,
                                                    const char *username, uint32_t now_ms);

#ifdef __cplusplus
}
#endif

#endif // TOTP_HTTP_CORE_H
