// ota_auth.h -- the pure, host-testable half of OTA challenge-response
// authentication: nonce lifecycle and per-endpoint lockout/backoff.
// firmware/CommonFW/docs/UPDATE_PROTOCOL.md section 2.
//
// Deliberately does NOT implement the HMAC-SHA256 computation itself --
// that calls mbedTLS (already linked into this build, per UPDATE_PROTOCOL.md
// section 2's own note: "mbedTLS is already linked in"), a vetted crypto
// library, from ESP-IDF-coupled code that cannot be host-built the way this
// file can. Hand-rolling a hash/HMAC implementation here just to keep this
// module "pure" would be exactly backwards: the primitive that most needs to
// be a well-vetted library call is the one thing this file stays out of.
// What IS worth writing carefully and testing here is the state machine
// around it -- single-use/expiry tracking and the escalating lockout -- and
// the constant-time comparison, which is a correctness property (not a
// cryptographic primitive) that is easy to get subtly wrong by hand and easy
// to verify by test.
//
// Pure, no ESP-IDF/mbedTLS dependency -- host-testable, same discipline as
// firmware/KilnFW/App/drivers/thermal_guard.c and pid.c (see
// App/test/build_host_tests.ps1).
#ifndef KILNCTL_OTA_AUTH_H
#define KILNCTL_OTA_AUTH_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OTA_AUTH_NONCE_LEN 16u

// --- Nonce lifecycle (UPDATE_PROTOCOL.md section 2 steps 1-4) ------------
//
// One active nonce at a time, "bound to the client's connection" in the doc's
// words -- this codebase's OTA surface is a single operator on a LAN, not a
// multi-tenant service, so one slot is the right amount of state, not a
// simplification that loses anything real. A fresh GET /api/ota/challenge
// always overwrites whatever nonce existed before, which is itself a form of
// single-use enforcement: an old, un-consumed nonce becomes unreachable the
// moment a new one is issued, not just when it is used or expires.
typedef struct {
    uint8_t  nonce[OTA_AUTH_NONCE_LEN];
    bool     valid;         // false until ota_auth_nonce_issue() has been called at least once
    bool     used;          // true once ota_auth_nonce_invalidate() has run for this nonce
    uint32_t issued_at_ms;
} ota_auth_nonce_state_t;

// Stores `rand_bytes` (caller-supplied, from a real RNG -- this module has no
// entropy source of its own and must not invent one) as the new active
// nonce, valid, unused, timestamped `now_ms`. Always succeeds; this is what
// makes issuing a fresh challenge implicitly retire whatever nonce came
// before, per this file's header comment.
void ota_auth_nonce_issue(ota_auth_nonce_state_t *s, const uint8_t rand_bytes[OTA_AUTH_NONCE_LEN],
                           uint32_t now_ms);

typedef enum {
    OTA_AUTH_NONCE_OK = 0,     // usable: issued, unused, unexpired -- caller may now compute
                                // and compare the HMAC against s->nonce
    OTA_AUTH_NONCE_NOT_ISSUED, // ota_auth_nonce_issue() has never been called
    OTA_AUTH_NONCE_EXPIRED,    // now_ms - issued_at_ms > OTA_AUTH_NONCE_EXPIRY_MS
    OTA_AUTH_NONCE_ALREADY_USED,
} ota_auth_nonce_check_t;

#define OTA_AUTH_NONCE_EXPIRY_MS 30000u // UPDATE_PROTOCOL.md section 2: "30 s expiry"

// Checks whether `s` is currently usable -- does NOT itself invalidate the
// nonce (that is a separate call, ota_auth_nonce_invalidate(), so the caller
// can compute/compare the actual HMAC in between and STILL invalidate
// afterward regardless of whether that comparison succeeded --
// UPDATE_PROTOCOL.md section 2 step 4: "invalidates the nonce whether or not
// it matched"). Calling this on an `s` that ota_auth_nonce_issue() has never
// touched (e.g. a fresh, zero-initialised struct) returns
// OTA_AUTH_NONCE_NOT_ISSUED, never a false OK.
ota_auth_nonce_check_t ota_auth_nonce_check(const ota_auth_nonce_state_t *s, uint32_t now_ms);

// Marks the nonce used. Idempotent -- calling this on an already-used or
// never-issued state is a harmless no-op, not an error, since the caller is
// expected to call this unconditionally after every POST attempt regardless
// of what ota_auth_nonce_check() said.
void ota_auth_nonce_invalidate(ota_auth_nonce_state_t *s);

// --- Constant-time comparison ---------------------------------------------
//
// UPDATE_PROTOCOL.md section 2 step 4: "recomputes and compares in constant
// time". An early-exit memcmp()-style comparison leaks timing information
// proportional to how many leading bytes match, which is exactly the kind of
// side channel a network-facing MAC comparison must not have. This routine
// always inspects every byte regardless of where the first mismatch is.
bool ota_auth_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len);

// --- Lockout / backoff (UPDATE_PROTOCOL.md section 2, "Rate limiting") ---
//
// "Three failures locks OTA endpoints for 60 s, doubling to a 15-minute
// ceiling, counted per-endpoint and reset on success." One instance of this
// struct per endpoint (the ESP path and the Pico path have independent
// lockout state -- a caller wires up two of these, one per
// POST /api/ota/{esp,pico}).
typedef struct {
    uint32_t failure_count;   // failures accumulated since the last lock-out or the last success
    uint32_t lockout_tier;    // how many times this endpoint has been locked out in a row without
                                // an intervening success -- 0 means never locked (or reset by success)
    uint32_t locked_until_ms; // 0 = not currently locked; otherwise the tick at which the lock lifts
} ota_auth_lockout_state_t;

#define OTA_AUTH_LOCKOUT_THRESHOLD 3u        // failures before a lock is imposed
#define OTA_AUTH_LOCKOUT_BASE_MS   60000u    // first lockout: 60 s
#define OTA_AUTH_LOCKOUT_MAX_MS    900000u   // ceiling: 15 min

// True iff `s` is currently locked (now_ms has not yet reached
// locked_until_ms). A caller should refuse the attempt entirely (not even
// checking the password) while this is true.
bool ota_auth_lockout_is_locked(const ota_auth_lockout_state_t *s, uint32_t now_ms);

// Records one authentication failure. Once accumulated failures reach
// OTA_AUTH_LOCKOUT_THRESHOLD, imposes a lock starting at `now_ms` for
// OTA_AUTH_LOCKOUT_BASE_MS * 2^lockout_tier, capped at
// OTA_AUTH_LOCKOUT_MAX_MS, then resets the failure count and advances the
// tier for next time -- so a client that keeps failing after each lockout
// expires sees 60s, then 120s, then 240s, ... capping at 15 minutes, exactly
// the doubling schedule the doc describes, rather than staying at 60s
// forever or escalating on every single failure regardless of whether a lock
// was actually imposed.
void ota_auth_lockout_record_failure(ota_auth_lockout_state_t *s, uint32_t now_ms);

// Records a success: clears failure_count, lockout_tier, and any active
// lock. UPDATE_PROTOCOL.md: "counted per-endpoint and reset on success."
void ota_auth_lockout_record_success(ota_auth_lockout_state_t *s);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_OTA_AUTH_H
