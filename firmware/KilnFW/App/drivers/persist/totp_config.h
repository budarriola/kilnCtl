// totp_config.h -- NVS-backed persistence for the administrator TOTP secret
// and its replay-guard counter. docs/TOTP_PASSWORD_RESET_PLAN.md section 3,
// work tranche WT-A.
//
// Same namespace (kiln_auth) and tri-state load-status discipline as
// web_auth_store.h -- TOTP enrollment is part of the same administrator-
// credential concept and gets the SAME backup/restore exclusion treatment
// (see docs/CONFIG_FILESYSTEM.md / backup_json.c: totp_secret is excluded
// from whole-board backup/restore and config-package export/import, exactly
// like the Wi-Fi password and smtp_password).
//
// This module is the I/O glue around hal_kv.h; the pure TOTP math lives in
// net/totp.h and has no dependency on this file or on NVS.
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
// tri-state above; `out` is left unmodified unless the return is
// TOTP_CONFIG_LOAD_OK.
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
bool totp_config_clear(void);

// --- Replay-guard counter --------------------------------------------------

// Reads the persisted last-accepted TOTP counter (0 if never set/absent --
// see totp_verify()'s doc comment on why 0 is a safe "never accepted"
// sentinel). This is the NVS-backed slow path; totp_config_ram_last_counter()
// below is the fast path a verifying HTTP handler should actually call.
uint32_t totp_config_load_last_counter(void);

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
uint32_t totp_config_ram_last_counter(void);

// Clears the RAM cache back to "not yet loaded" (used by totp_config_clear()
// and by host tests needing a clean-slate re-load).
void totp_config_ram_reset(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_TOTP_CONFIG_H
