// web_auth_store.h -- credential storage foundation for docs/WEB_AUTH_PLAN.md
// sections 2 (credential storage), 3 (strength rules) and 11 (auth
// disabled/first boot/field upgrade).
//
// SCOPE: this module owns exactly the credential RECORD -- hashed/salted
// web passwords and LCD PINs for the `user` and `administrator` roles, the
// auth-enabled policy flags, strength validation, and the verify/set entry
// points other slices call. It does NOT implement: sessions (plan item 4),
// the HTTP enforcement pre-handler and route tier table (item 5), the
// password page (item 6), LCD PIN entry/the keypad (item 7), the inactivity
// lock (item 8), or the physical credential-reset gesture (item 10). Those
// are other slices; this header is what they call into.
//
// WHERE THE RECORD LIVES -- load-bearing, see the plan's item 2. Credentials
// live in NVS namespace `kiln_auth` on the DEFAULT `nvs` partition
// (NVS_DEFAULT_PART_NAME, pass partition=NULL to hal_kv_open), deliberately
// OUTSIDE every config partition (`wifi_nvs`, `kiln_nvs`, `profiles_nvs`).
// No factory-reset scope names `nvs`, and no config operation (kiln config
// slot save/apply/clone/delete/rename, package import/export, whole-board
// backup/restore, zones/prefs/profiles writes, cfg LittleFS dual-write) may
// read or write these keys -- that is the "one property, not several rules"
// item 12b describes. Do not add a credential read/write anywhere outside
// this file and web_auth_store.c.
//
// PURE VS I/O SPLIT (App/test/build_host_tests.ps1 discipline, same as
// ota_auth.h/.c): the strength-check and policy-collapse functions below
// take no hal_kv dependency and are pure. The record load/verify/set
// functions do real NVS I/O via hal_kv.h and are exercised in host tests
// through fake_kv.h's RAM-backed fake, same convention as boot_guard.c/
// crash_report.c.
//
// HASHING: PBKDF2-style iterated HMAC-SHA256 (WEB_AUTH_ITERATIONS rounds),
// 16-byte random salt, 32-byte output -- see web_auth_store.c's
// web_auth_hash_compute() doc comment for the exact construction and why it
// is the plan's documented fallback rather than a PSA PBKDF2 algorithm ID.
// Runs on the caller's stack; callers must not run it while holding a
// safety/relay-owner lock and must not call it from a PSRAM-backed stack
// (this module makes no attempt to detect that -- callers already know which
// task they run on, per hal_kv.h's own write-context contract).
//
// SALT/RANDOMNESS: this module does NOT generate randomness itself (no
// esp_fill_random() call anywhere in this file), the same design choice
// ota_auth_nonce_issue() already made for the identical reason: a pure,
// host-testable module cannot own an entropy source, and every caller
// already has one. web_auth_store_set_password()/_set_pin() take the salt as
// a caller-supplied `const uint8_t[16]` -- callers fill it via
// esp_fill_random() (ESP-IDF) immediately before calling.
#ifndef KILNCTL_WEB_AUTH_STORE_H
#define KILNCTL_WEB_AUTH_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Sizing --------------------------------------------------------------

#define WEB_AUTH_SALT_LEN        16u
#define WEB_AUTH_HASH_LEN        32u
#define WEB_AUTH_USERNAME_MAX_LEN 32u /* +1 for NUL in the record */
// 2026-09-21 owner decision: fast logon over the plan's original "20,000
// iterations" -- measured on hardware at 20000 rounds: 4158 ms/attempt
// (docs/BENCH_TEST_LOG.md, 2026-09-21). Scaling linearly (time is
// proportional to iteration count -- see web_auth_hash_compute()'s single
// hot loop, no other per-call cost that varies with `iterations`):
// 20000 * (0.4 / 4.158) ~= 1924, rounded to 2000 for a clean number, giving
// an estimated 4158 * (2000 / 20000) ~= 416 ms/attempt -- comfortably under
// 1 s on this hardware. A record's OWN `iterations` field (not this
// constant) is what verify actually uses (web_auth_store_verify_password()),
// so an existing credential hashed at the old 20000 keeps verifying
// correctly at that cost; it is only re-hashed at the new, lower count the
// next time its password/PIN is SET (web_auth_store_set_password()/
// _set_pin() both stamp rec->iterations = WEB_AUTH_ITERATIONS at set time).
#define WEB_AUTH_ITERATIONS      2000u

#define WEB_AUTH_STORE_VERSION 1u /* own schema version -- see header comment;
                                    * ZONES_CFG_VERSION (26) is untouched by
                                    * this module and must stay untouched. */

// --- Roles -----------------------------------------------------------------

typedef enum {
    WEB_AUTH_ROLE_USER = 0,
    WEB_AUTH_ROLE_ADMINISTRATOR = 1,
    WEB_AUTH_ROLE_COUNT = 2,
} web_auth_role_t;

// --- Strength rules (plan item 3) ------------------------------------------

typedef enum {
    WEB_AUTH_PW_OK = 0,
    WEB_AUTH_PW_TOO_SHORT,        // < 10 characters
    WEB_AUTH_PW_TOO_LONG,         // > 64 characters
    WEB_AUTH_PW_ALL_LOWERCASE,    // no character outside a-z
    WEB_AUTH_PW_REJECTED_COMMON,  // on the rejection list (see below)
} web_auth_pw_check_t;

// Minimum 10 characters, at most 64, at least one character that is not a
// lowercase letter, and not on the rejection list: the literal strings
// "password" and "kiln" (case-insensitive), the username being set, the AP
// SSID, and the AP password. `username`/`ap_ssid`/`ap_password` may each be
// NULL or empty (nothing to compare against); comparisons are
// case-insensitive for the fixed words, case-SENSITIVE for username/SSID/AP
// password (those are exact secrets, not English words). No maximum below
// 64, no forced rotation, no composition beyond this -- plan item 3's
// reasoning against a character-class matrix. Pure function, no I/O.
web_auth_pw_check_t web_auth_password_check(const char *password, const char *username,
                                             const char *ap_ssid, const char *ap_password);

// The PIN length bounds this store actually enforces (web_auth_pin_check(),
// web_auth_store_set_pin()) -- named so security_http_core.h's
// SECURITY_HTTP_PIN_MAX (the request buffer's sizing limit, one file over)
// has something to be checked against instead of two independently-chosen
// literal 8s. See security_backend_web_auth.c's static assert.
#define WEB_AUTH_PIN_MIN_LEN 4u
#define WEB_AUTH_PIN_MAX_LEN 8u

typedef enum {
    WEB_AUTH_PIN_OK = 0,
    WEB_AUTH_PIN_TOO_SHORT,   // < WEB_AUTH_PIN_MIN_LEN digits
    WEB_AUTH_PIN_TOO_LONG,    // > WEB_AUTH_PIN_MAX_LEN digits
    WEB_AUTH_PIN_NOT_DIGITS,  // contains a non-digit character
    WEB_AUTH_PIN_SAME_AS_OTHER, // equals the other role's PIN (plan item 3:
                                  // "the two PINs must also differ from
                                  // each other")
} web_auth_pin_check_t;

// `pin` is the candidate PIN as a NUL-terminated decimal digit string (4-8
// digits). `other_pin_or_null` is the OTHER role's current PIN in the same
// form, or NULL/empty if the other role has no PIN set yet (nothing to
// collide with). Pure function, no I/O.
web_auth_pin_check_t web_auth_pin_check(const char *pin, const char *other_pin_or_null);

// --- Hashing (item 2) -------------------------------------------------------

// Iterated-HMAC-SHA256 KDF: out = H_n(secret, salt) where H_1 = HMAC(secret,
// salt) and H_i = HMAC(secret, H_{i-1}) for i > 1, `iterations` = n. This is
// the plan's explicitly-sanctioned fallback ("an iterated HMAC-SHA256 loop
// over the already-proven psa_mac_compute() path") rather than a PSA PBKDF2
// algorithm object, chosen because only psa/crypto.h (not a raw mbedtls
// header) is included anywhere in this tree today -- see ota_http.c's
// hmac_sha256() for the same PSA import/compute/destroy shape this reuses.
// `secret`/`secret_len` is the password or PIN's raw bytes -- never logged,
// never stored, discarded by the caller immediately after this call.
// `iterations` must be >= 1; the record stores it, not a compile-time
// constant, so a future round-count bump costs nothing on records that
// already exist. `out` receives exactly WEB_AUTH_HASH_LEN bytes.
void web_auth_hash_compute(const uint8_t *secret, size_t secret_len,
                            const uint8_t salt[WEB_AUTH_SALT_LEN], uint32_t iterations,
                            uint8_t out[WEB_AUTH_HASH_LEN]);

// Constant-time comparison of two WEB_AUTH_HASH_LEN buffers -- same
// reasoning as ota_auth_constant_time_equal(): a network- or panel-facing
// comparison of a secret-derived value must not leak timing information
// proportional to the first mismatched byte.
bool web_auth_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len);

// --- Records -----------------------------------------------------------------

typedef struct {
    char     username[WEB_AUTH_USERNAME_MAX_LEN + 1];
    uint8_t  salt[WEB_AUTH_SALT_LEN];
    uint8_t  hash[WEB_AUTH_HASH_LEN];
    uint32_t iterations;
    bool     must_change; // item 10: a physical reset sets this true
    bool     configured;  // false = this role has no password set yet
} web_auth_password_record_t;

typedef struct {
    uint8_t  salt[WEB_AUTH_SALT_LEN];
    uint8_t  hash[WEB_AUTH_HASH_LEN];
    uint32_t iterations;
    uint8_t  digits;      // number of decimal digits this PIN has (4-8)
    bool     configured;
} web_auth_pin_record_t;

typedef struct {
    bool    web_enabled;
    bool    lcd_enabled;
    int32_t web_timeout_s; // -1 == "never" (plan item 8)
    int32_t lcd_timeout_s; // -1 == "never"
} web_auth_policy_t;

// Result of loading a versioned record from NVS. Distinguishes "never
// written" (ABSENT -- the shipped default / an un-upgraded board, item 11)
// from "written, but this build cannot trust it" (UNREADABLE -- wrong size,
// bad CRC, or a version newer than this build knows, item 12b's OTA-rollback
// case). The two must NEVER collapse to the same caller-visible behaviour
// for a credential or policy record: ABSENT means "auth off, fully
// functional", UNREADABLE must fail closed. See
// web_auth_policy_effective_enabled() below, which is the one function that
// turns this distinction into a yes/no answer for the auth-off collapse.
typedef enum {
    WEB_AUTH_LOAD_OK = 0,
    WEB_AUTH_LOAD_ABSENT,
    WEB_AUTH_LOAD_UNREADABLE,
} web_auth_load_status_t;

// --- Load / verify / set: web passwords -------------------------------------

// Loads role's password record. WEB_AUTH_LOAD_ABSENT (never written) leaves
// *out zeroed with configured=false; WEB_AUTH_LOAD_UNREADABLE leaves *out
// zeroed too -- a caller must check the return status, not *out->configured,
// to tell "no credential" from "unreadable, do not trust this".
web_auth_load_status_t web_auth_store_load_password(web_auth_role_t role,
                                                      web_auth_password_record_t *out);

// True iff `password` matches role's stored record. Always false if the
// record is ABSENT or UNREADABLE, or if `password`/`role` is invalid -- a
// caller never needs to check web_auth_store_load_password() first just to
// decide whether to call this. Runs the KDF (WEB_AUTH_ITERATIONS rounds) on
// the caller's own stack -- see this header's hashing note above for the
// stack-context caveat.
bool web_auth_store_verify_password(web_auth_role_t role, const char *password);

// True iff role has a password configured (LOAD_OK and configured==true).
// Used by item 11's "enabling auth is refused unless a credential exists"
// gate and by nothing else -- it is not itself an auth check.
bool web_auth_store_password_configured(web_auth_role_t role);

// Sets role's password. Caller supplies `salt` (WEB_AUTH_SALT_LEN
// caller-generated random bytes -- see this header's randomness note) and
// `username` (NUL-terminated, truncated to WEB_AUTH_USERNAME_MAX_LEN if
// longer -- callers should validate length themselves via
// web_auth_password_check() first). Does NOT itself validate password
// strength -- that is the caller's job via web_auth_password_check(), kept
// separate so a caller can show a strength error before ever reaching a
// storage call. Hashes, writes the whole `web_auth` blob (both roles -- see
// web_auth_store.c for why it is one blob, not one key per role), and
// verifies the write by reading it back before returning HAL_OK -- never
// trust a bare NVS write return code (see boot_guard_mark_healthy()'s
// history). Returns HAL_IO if the read-back does not match what was
// written. Must be called from a context where hal_kv_write_safe_here() is
// true (this function does not check that itself, matching every other
// persist module's convention).
hal_status_t web_auth_store_set_password(web_auth_role_t role, const char *username,
                                          const char *password,
                                          const uint8_t salt[WEB_AUTH_SALT_LEN],
                                          bool must_change);

// --- Load / verify / set: LCD PINs -------------------------------------------

web_auth_load_status_t web_auth_store_load_pin(web_auth_role_t role, web_auth_pin_record_t *out);

// `pin` is the entered PIN as a NUL-terminated decimal digit string.
bool web_auth_store_verify_pin(web_auth_role_t role, const char *pin);

bool web_auth_store_pin_configured(web_auth_role_t role);

// Same read-back-verified write discipline as web_auth_store_set_password().
// Does not itself enforce web_auth_pin_check() -- caller's job, same split.
hal_status_t web_auth_store_set_pin(web_auth_role_t role, const char *pin,
                                     const uint8_t salt[WEB_AUTH_SALT_LEN]);

// --- Load / verify / set: policy ---------------------------------------------

web_auth_load_status_t web_auth_store_load_policy(web_auth_policy_t *out);

// Same read-back-verified write discipline. Does not itself refuse enabling
// auth without a credential -- that check belongs at the call site (the
// password page, item 6), using web_auth_store_password_configured()/
// web_auth_store_pin_configured() above; this setter just persists whatever
// it is given.
hal_status_t web_auth_store_set_policy(const web_auth_policy_t *policy);

// --- Auth-off / first-boot / field-upgrade collapse (plan item 11) ---------

// Turns a load status + a possibly-stale stored flag into the one answer
// every enforcement point needs: "is this interface's auth actually in
// force right now". ABSENT -> false (shipped default, field-upgrade
// no-regression). OK -> `stored_enabled` verbatim. UNREADABLE -> true
// (fail closed -- item 12b's OTA-rollback-past-a-schema-bump case: a record
// this build cannot parse must never be read as "no credentials set", so it
// is treated as enabled, and since the credential record underneath it is
// then also UNREADABLE, web_auth_store_verify_password()/_verify_pin() both
// report false unconditionally -- the net effect is "login refused, use the
// physical reset gesture (item 10)", exactly what the plan specifies).
// Pure function, no I/O -- callers pass in the status/flag they already
// loaded via web_auth_store_load_policy().
bool web_auth_policy_effective_enabled(web_auth_load_status_t status, bool stored_enabled);

// --- Enable-gate / session-clear decision (plan item 11, "the only place
// the two interact") ---------------------------------------------------------
//
// "No credential set" is not an error state -- a board with auth off and no
// credential is fully functional. But "enabling auth is refused unless a
// credential for that interface exists" (item 11) is a real invariant this
// module must protect, because nothing else stands between an operator and
// locking themselves out: web_auth_store_set_policy() above deliberately does
// NOT enforce it (any caller can persist any policy value -- see its own doc
// comment), so the one place this check happens is here, called by whichever
// slice actually flips a switch (today, the item 6 password page). A second,
// re-derived copy of this rule anywhere else would be exactly the kind of
// silent duplicate CLAUDE.md's "reset one side of a pair" note warns about.
//
// `requested` is the whole policy record the caller wants to persist next;
// `admin_password_configured`/`admin_pin_configured` are
// web_auth_store_password_configured(WEB_AUTH_ROLE_ADMINISTRATOR) /
// web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR) -- passed in
// rather than read here so this stays a pure function like the rest of this
// header's decision logic (no I/O, host-testable with a fake_kv-free test).
// The check is on `requested`, not on the current->requested edge: an
// enabled-but-now-credential-less state is never valid to persist, not just
// newly-invalid to enter, so a caller cannot "grandfather" a stale enabled
// flag back in by resaving it unchanged.
//
// `out_clear_web_sessions`/`out_clear_lcd_session` are only meaningful when
// the return value is WEB_AUTH_POLICY_TRANSITION_OK; on any REFUSED result
// they are set false and the caller MUST NOT call web_auth_store_set_policy()
// with `requested` at all. When OK, `*out_clear_web_sessions` is true iff
// this transition turns web_enabled on (false -> true in `current`), never
// merely because it is already true (a re-save of an unchanged "on" policy,
// e.g. from changing only a timeout, must not silently log everyone out) --
// same edge-triggered reasoning for `*out_clear_lcd_session` against
// lcd_enabled. Disabling never clears (item 11: "sessions become irrelevant
// but are kept").
typedef enum {
    WEB_AUTH_POLICY_TRANSITION_OK = 0,
    WEB_AUTH_POLICY_TRANSITION_REFUSED_NO_WEB_CREDENTIAL,
    WEB_AUTH_POLICY_TRANSITION_REFUSED_NO_LCD_CREDENTIAL,
} web_auth_policy_transition_t;

web_auth_policy_transition_t web_auth_policy_check_transition(const web_auth_policy_t *current,
                                                                const web_auth_policy_t *requested,
                                                                bool admin_password_configured,
                                                                bool admin_pin_configured,
                                                                bool *out_clear_web_sessions,
                                                                bool *out_clear_lcd_session);

// --- Physical credential reset (plan item 10) -------------------------------

// The one entry point item 10's confirmed gesture (E-stop asserted, all four
// LCD corners tapped in order, then an explicit timed confirm) calls once
// the confirm step fires. Does exactly two things, and nothing else:
//
//  1. Clears the ADMINISTRATOR's web password record only -- the USER
//     record within the same shared blob is left byte-for-byte untouched.
//     "Cleared" means configured=false, must_change=true, hash/salt zeroed:
//     no default password value is invented or stored (this module holds no
//     entropy source per its randomness note, and a fixed literal password
//     baked into source is exactly the class of secret this codebase's
//     "never write a real credential into the repo" rule exists to forbid).
//  2. Leaves the policy record (WEB_AUTH_KEY_POLICY), the LCD PIN record
//     (WEB_AUTH_KEY_LCD, both roles) and every config namespace/partition
//     completely untouched. Plan section 10 is explicit that this reset
//     "does not disable authentication ... and does not clear any config":
//     the failure being recovered from is a forgotten password, and
//     silently taking policy back to WEB_AUTH_LOAD_ABSENT (auth-off, via
//     web_auth_policy_effective_enabled()'s ABSENT collapse) would be a
//     bigger hole than the one being closed. An earlier version of this
//     function did exactly that and was corrected.
//
// Net effect: if web auth was enabled before the reset, it stays enabled,
// with the administrator credential now unconfigured. That is a new
// reachable state -- auth enabled, no administrator credential configured,
// must_change=true -- and it is section 11's login-path work (auth
// disabled/first-boot/field-upgrade handling, owned separately) that must
// treat it as a forced set-a-new-password flow, not as a lockout or a
// silent bypass. This function only creates the state; it does not handle
// it.
//
// Same read-back-verified write discipline as every other setter in this
// file. Returns true only once the administrator-record write is confirmed
// by read-back; on false the caller must treat the reset as not having
// happened.
//
// Signature is exactly auth_reset_gesture_clear_fn (bool (*)(void)) so it
// can be assigned directly to auth_reset_gesture_state_t.clear_credentials_fn
// with no adapter -- see auth_reset_gesture.h.
bool web_auth_store_clear_for_physical_reset(void);

// WEB_AUTH_PLAN.md item 12b, "the counter-expectation, acknowledged rather
// than dismissed": an already-authenticated administrator's explicit
// "Clear login credentials" action (POST /api/auth/security,
// cmd=clear_credentials -- security_http_core.c), reachable from
// /settings/security, distinct from BOTH the physical four-corner reset
// gesture (item 10, E-stop-gated, recovers a forgotten credential with
// nobody able to log in) and from `POST /api/factory_reset` (which item
// 12b states explicitly must NOT touch this namespace, in any of its four
// scopes). This route is not a weakening: it requires a session that
// already holds full administrator access, which could change every
// password individually anyway -- this is a convenience for the "I
// expected factory reset to also clear login" expectation, answered
// through a legitimate route instead of by widening factory_reset's own
// blast radius.
//
// Clears BOTH roles, web password and LCD PIN alike: the administrator
// web record (configured=false, must_change=true -- same as the physical
// reset, so a subsequent login is forced through the set-a-new-password
// flow / POST /api/auth/bootstrap_password if web auth is enabled), the
// `user` web record (configured=false, no must_change field to set), and
// both LCD PIN records (configured=false). Deliberately does NOT touch
// `auth_policy` -- exactly like web_auth_store_clear_for_physical_reset(),
// enabling/disabling auth is a separate decision this function has no
// opinion on, and silently flipping it back to auth-off would be a bigger
// change than "clear the credentials" implies.
//
// Same read-back-verified write discipline as every other setter in this
// file. Returns true only once every one of the four records (web
// admin/user, LCD admin/user) is confirmed cleared by read-back; on false
// the caller must treat the clear as not having (fully) happened.
bool web_auth_store_clear_all_credentials(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_WEB_AUTH_STORE_H
