// totp_config.h -- NVS-backed persistence for the administrator TOTP secret
// and its replay-guard counter. docs/TOTP_PASSWORD_RESET_PLAN.md section 3,
// work tranche WT-A.
//
// Same namespace (kiln_auth) and tri-state load-status discipline as
// web_auth_store.h -- TOTP enrollment is part of the same administrator-
// credential concept and gets the SAME backup/restore exclusion treatment
// as web_auth_store's records (docs/TOTP_PASSWORD_RESET_PLAN.md section 3):
// excluded BY OMISSION -- backup_json.c/backup_export.c never open the
// kiln_auth namespace, so nothing here needs an explicit exclusion entry;
// a future backup change that starts walking kiln_auth must skip both keys.
//
// This module is the I/O glue around hal_kv.h; the pure TOTP math lives in
// net/totp.h and has no dependency on this file or on NVS.
//
// WRITE CONTEXT: totp_config_set_secret(), totp_config_set_last_counter()
// and totp_config_clear() write NVS, so they inherit hal_kv.h's write-
// context contract -- never call them from a task on a PSRAM-backed stack
// (panics on this target). The replay-counter write happens on EVERY
// accepted code, so the verifying route must run it on an internal-DRAM
// stack (the httpd worker, as web_auth_store's writers already do) or
// dispatch it to the flash worker. Reads carry no such restriction.
//
// CONCURRENCY: no internal lock. The RAM counter cache and the
// verify-then-set_last_counter sequence assume one caller at a time (the
// single httpd worker plus the LCD reset-gesture's clear). A caller that
// can race another verifier must serialise verify + set itself, or two
// requests could both accept the same code before either persists it.
#ifndef KILNCTL_TOTP_CONFIG_H
#define KILNCTL_TOTP_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "net/totp.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TOTP_CONFIG_LOAD_OK,        // secret present and read back successfully
    TOTP_CONFIG_LOAD_ABSENT,    // never enrolled -- safe default, not an error
    TOTP_CONFIG_LOAD_UNREADABLE // present but corrupt/short/CRC-mismatched --
                                 // MUST be treated as "not usable", never
                                 // collapsed to ABSENT (fail closed)
} totp_config_load_status_t;

// Loads the enrolled secret into `out[TOTP_SECRET_LEN]`. Returns the
// tri-state above: ABSENT only when the key (or the namespace) does not
// exist; any other read error, a wrong-size blob, an unknown version or a
// CRC mismatch is UNREADABLE. `out` is left unmodified unless the return is
// TOTP_CONFIG_LOAD_OK. The caller should totp_secure_zero() `out` when done.
totp_config_load_status_t totp_config_load_secret(uint8_t out[TOTP_SECRET_LEN]);

// True only on TOTP_CONFIG_LOAD_OK -- the convenience check enrollment/reset
// HTTP glue uses to decide whether TOTP is active at all.
bool totp_config_enrolled(void);

// Persists `secret[TOTP_SECRET_LEN]` (versioned + CRC32, same shape as
// web_auth_store.c's blob records) and read-back-verifies before returning
// true. Also resets the persisted replay counter to 0 (a freshly-enrolled
// secret has never accepted a code yet) -- this is the ONLY setter that
// touches both keys, so a caller enrolling a new secret can never leave a
// stale counter from a previous enrollment behind.
bool totp_config_set_secret(const uint8_t secret[TOTP_SECRET_LEN]);

// Erases BOTH totp_secret and totp_last_ctr and read-back-verifies the
// erasure. Used by: (a) the enrollment "disable" action (proves the current
// code first, at the HTTP layer, before calling this), and (b) the LCD
// four-corner physical reset gesture's confirm site, additively alongside
// web_auth_store_clear_for_physical_reset() -- see auth_reset_gesture_wiring.c.
// Also clears the RAM-cached replay counter (totp_config_ram_reset()).
// Safe to call when nothing is enrolled (idempotent, still returns true).
// The secret is erased first; if that fails the counter is left in place
// (never a surviving secret with a reset replay counter).
bool totp_config_clear(void);

// --- Replay-guard counter --------------------------------------------------

// Reads the persisted last-accepted TOTP counter into *out. ABSENT (key
// never written) sets *out = 0 -- see totp_verify()'s doc comment on why 0
// is a safe "never accepted" sentinel. UNREADABLE (read error or wrong
// size) leaves *out untouched and the caller MUST refuse to verify: a
// counter that cannot be read must never collapse to 0, or every recently
// used code becomes replayable. This is the NVS-backed slow path;
// totp_config_ram_last_counter() below is the fast path a verifying HTTP
// handler should actually call.
totp_config_load_status_t totp_config_load_last_counter(uint32_t *out);

// Persists `counter` as the new last-accepted counter, read-back-verified.
// Also updates the RAM cache so a subsequent totp_config_ram_last_counter()
// call in the same boot sees it immediately without a redundant NVS read.
bool totp_config_set_last_counter(uint32_t counter);

// RAM-cache mirror of the replay counter: lazily loads from NVS on first
// call each boot, then serves from RAM afterward. This is the fast path
// totp_verify() callers use for the "was this counter already used" check
// without an NVS read on every single verification attempt -- NVS is only
// touched by totp_config_set_last_counter() when a code is actually
// accepted, not on every verify attempt (including failed ones).
// Returns false (and caches nothing, so the next call retries NVS) when
// the persisted counter is UNREADABLE; the caller must then refuse the
// code. On true, *out holds the counter (0 if never set).
bool totp_config_ram_last_counter(uint32_t *out);

// Clears the RAM cache back to "not yet loaded" (used by totp_config_clear()
// and by host tests needing a clean-slate re-load).
void totp_config_ram_reset(void);

// --- Verify-and-consume (the one call a verifying route should make) -----

typedef enum {
    TOTP_CONSUME_OK,           // code valid AND its counter persisted as used
    TOTP_CONSUME_REJECTED,     // malformed, wrong, outside the window, or replayed
    TOTP_CONSUME_NOT_ENROLLED, // no secret stored (ABSENT)
    TOTP_CONSUME_UNAVAILABLE   // secret or counter UNREADABLE, or persisting the
                               // matched counter failed -- refuse, fail closed
} totp_consume_result_t;

// Loads the replay counter (RAM fast path) and the secret, runs
// totp_verify(), and on a match persists the matched counter via
// totp_config_set_last_counter() BEFORE returning TOTP_CONSUME_OK. The
// counter is therefore already recorded as used by the time any caller
// sees success, so a second call with the same code (or an earlier step's
// code) is refused -- there is no verify-then-persist window for a caller
// to get wrong. Anything short of a verified persist is not OK. Callers
// must still check SNTP sync before calling (plan section 3) and must not
// log `code`. Writes NVS: same write-context rule as the setters above
// (never from a PSRAM-stacked task). Single caller at a time (see the
// CONCURRENCY note at the top of this file).
totp_consume_result_t totp_config_verify_and_consume(const char *code, uint64_t unix_time_s);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_TOTP_CONFIG_H
