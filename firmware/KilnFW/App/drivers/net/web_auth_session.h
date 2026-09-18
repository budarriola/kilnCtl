// web_auth_session.h -- the pure, host-testable session mechanism for web
// (and LCD) authentication: docs/WEB_AUTH_PLAN.md section 4 (session table)
// and the web half of section 8 (inactivity lock + 10 s stay-unlocked
// prompt).
//
// Scope, stated explicitly because four other passes are touching this same
// feature concurrently:
//   - Owned here: the session record and its lifecycle (create, look up,
//     touch/expire, explicit teardown), the fixed-size slot table with LRU
//     eviction, the web inactivity timeout and its 10 s prompt window, and
//     the auth-disabled inert path. All pure logic, no ESP-IDF dependency,
//     so this file and web_auth_session.c build and test on the host exactly
//     like ota_auth.h/.c do.
//   - NOT owned here: credential storage/hashing (section 2/3 -- another
//     pass owns `kiln_auth`/PBKDF2 and will hand this module a verification
//     function), route tier classification and the mechanical tier check
//     (section 1), the httpd enforcement pre-handler itself (section 5), the
//     password page (section 6), LCD PIN entry/keypad (section 7 and the LCD
//     half of section 8), and the physical credential-reset gesture
//     (section 10). This module does not send HTTP responses and does not
//     touch NVS.
//
// Credential verification seam (see the header comment on
// web_auth_credential_verify_fn below): callers hand this module a function
// pointer rather than this module calling into a credential-storage header
// directly, so wiring up the real `kiln_auth`-backed verifier once section 2
// lands is a one-line change at the call site, not a rewrite here.
#ifndef KILNCTL_WEB_AUTH_SESSION_H
#define KILNCTL_WEB_AUTH_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// web_auth_store.h (landed c3008eb8) owns credential-role identity
// (web_auth_role_t: WEB_AUTH_ROLE_USER/_ADMINISTRATOR) for its
// PBKDF2-backed password/PIN records. This module's role concept is a
// distinct, session-tier notion that also needs a NONE (no session) value
// the credential-role enum has no room for -- so it gets its own type,
// web_auth_session_role_t, rather than colliding on the name
// `web_auth_role_t` (and its `WEB_AUTH_ROLE_USER` enumerator) when both
// headers are included together, as the real login/enforcement code will
// need to. Included here (a pure header, no I/O) purely for the credential
// verification seam's function-pointer types below.
#include "../persist/web_auth_store.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Roles -----------------------------------------------------------------

typedef enum {
    WEB_AUTH_SESSION_ROLE_NONE = 0, // no session / unauthenticated
    WEB_AUTH_SESSION_ROLE_USER,
    WEB_AUTH_SESSION_ROLE_ADMIN,
} web_auth_session_role_t;

// --- Session slots (WEB_AUTH_PLAN.md section 4) -----------------------------
//
// Originally "a slot is { token_hash[32], client_ip[16], role, issued_ms,
// last_seen_ms } -- 56 bytes plus padding" (WEB_AUTH_PLAN.md section 4).
// client_ip grew from 16 to 46 bytes in the 2026-09-17 review's Finding 1
// fix (see WEB_AUTH_CLIENT_IP_LEN below) -- +30 bytes/slot, +240 bytes total
// across the 8-slot table, still RAM-only, no heap. 8 web slots, LRU-evicted.
// The LCD gets exactly one session (a single struct of the same shape), not
// a slot in this table -- see web_auth_lcd_session_t below.
#define WEB_AUTH_TOKEN_HASH_LEN 32u
// 46 == INET6_ADDRSTRLEN, the same size as the producer's own buffer
// (ota_http_get_client_ip()'s caller-side `char ip[46]` in http_auth_http.c/
// web_auth_login_http.c). 2026-09-17 review, Finding 1: this used to be 16u
// ("enough for a dotted-quad IPv4 string"), which silently truncated every
// IPv6 address to 15 characters -- on a dual-stack (IPv4+IPv6) listener
// (ESP-IDF's default CONFIG_LWIP_IPV6=y), an IPv6 client could log in but
// then fail web_auth_effective_role()'s IP-binding compare on every
// subsequent request (the login handler stored the truncated 15-char form,
// but the comparison side always received the full untruncated address), a
// silent, total lockout for every IPv6 client. Worse, two distinct IPv6
// clients sharing their first 15 characters (e.g. two `fe80::` link-local
// peers) collapsed onto the same stored string -- a cross-client
// session-binding collision, not merely a lockout. This module still never
// parses/formats the IP itself; it only needs to hold whatever the producer
// hands it without truncating it.
#define WEB_AUTH_CLIENT_IP_LEN  46u
#define WEB_AUTH_WEB_SLOT_COUNT 8u

typedef struct {
    bool     in_use;
    uint8_t  token_hash[WEB_AUTH_TOKEN_HASH_LEN]; // never the raw token -- compared
                                                    // constant-time, see
                                                    // web_auth_session_constant_time_equal()
    char     client_ip[WEB_AUTH_CLIENT_IP_LEN];
    web_auth_session_role_t role;
    uint32_t issued_ms;
    uint32_t last_seen_ms;
    bool     prompted; // true once the 10 s stay-unlocked prompt has been surfaced
                        // for this slot since its last touch -- lets a caller show
                        // the prompt exactly once per idle approach to expiry
} web_auth_slot_t;

typedef struct {
    web_auth_slot_t slots[WEB_AUTH_WEB_SLOT_COUNT];
} web_auth_table_t;

// Zero-initializes `t`. Equivalent to memset(t, 0, sizeof(*t)) but named so
// callers don't have to know that a zeroed table is a valid empty one (it
// is: in_use == false for every slot).
void web_auth_table_init(web_auth_table_t *t);

// Constant-time comparison, same discipline and same reasoning as
// ota_auth_constant_time_equal() (see ota_auth.h): a token hash comparison
// is exactly the kind of network-facing secret compare that must not leak
// timing information proportional to the first mismatched byte. Named
// web_auth_session_* (not the shorter web_auth_constant_time_equal) because
// web_auth_store.c/.h already defines its own externally-linked
// web_auth_constant_time_equal() for its hash compares -- check_duplicate_
// symbols.ps1 caught the collision.
bool web_auth_session_constant_time_equal(const uint8_t *a, const uint8_t *b, size_t len);

// Creates a new session: finds a free slot, or if the table is full, evicts
// the slot with the smallest last_seen_ms (least recently seen) -- "a stale
// session can never lock out a real operator" (WEB_AUTH_PLAN.md section 4).
// Stores `token_hash` (already hashed by the caller -- this module never
// sees or stores a raw token) and `client_ip` (copied; caller is responsible
// for NUL-terminating a shorter string) into the chosen slot with `role`,
// issued_ms == now_ms, last_seen_ms == now_ms, prompted == false.
// `client_ip` is expected to always fit WEB_AUTH_CLIENT_IP_LEN (46, same as
// the producer's own buffer) -- but 2026-09-17 review, Finding 1, found this
// module silently truncating an oversized address instead, which is how a
// 16-byte buffer holding IPv6 addresses caused a cross-client collision. If
// `client_ip` still does not fit (a producer bug, since none of today's
// callers can hand this a string this long), this function refuses to guess
// at a truncated form: it records an empty client_ip for the slot instead,
// so the slot can never spuriously match a real address later (an empty
// stored string only matches a caller-supplied client_ip that is itself an
// empty string, which web_auth_effective_role() already treats as
// impossible to present legitimately) -- fails closed instead of silently
// colliding. Returns the index of the slot used. Two distinct calls
// with distinct `token_hash` values always occupy distinct slots (this
// module performs no dedup); the caller is responsible for minting a fresh
// random token per login, same as UI_PLAN.md's original design.
size_t web_auth_table_create_session(web_auth_table_t *t, const uint8_t token_hash[WEB_AUTH_TOKEN_HASH_LEN],
                                      const char *client_ip, web_auth_session_role_t role, uint32_t now_ms);

// Looks up a session by token hash (compared constant-time against every
// in-use slot -- there is no faster indexed lookup here, and 8 slots makes
// a linear scan free). Returns the slot index, or -1 if no in_use slot's
// token_hash matches. Does NOT check expiry and does NOT touch last_seen_ms
// -- see web_auth_table_touch() for that, kept as a separate call so a
// caller can distinguish "look up, then decide whether this counts as
// activity" (WEB_AUTH_PLAN.md section 8: "a dashboard polling /api/status
// ... does NOT keep an admin session alive").
int web_auth_table_find_by_token(const web_auth_table_t *t, const uint8_t token_hash[WEB_AUTH_TOKEN_HASH_LEN]);

// Updates slot[idx].last_seen_ms to now_ms and clears its `prompted` flag
// (a real activity touch dismisses any pending stay-unlocked prompt for that
// slot, since the session is no longer near its timeout). A no-op if idx is
// out of range or the slot is not in_use.
void web_auth_table_touch(web_auth_table_t *t, size_t idx, uint32_t now_ms);

// Explicit teardown of one slot (logout). A no-op if idx is out of range.
void web_auth_table_destroy_session(web_auth_table_t *t, size_t idx);

// Explicit teardown of every slot holding `role` (WEB_AUTH_PLAN.md section 6:
// "changing a password invalidates every session for that role"). Slots of
// any other role are left untouched.
void web_auth_table_destroy_role(web_auth_table_t *t, web_auth_session_role_t role);

// Explicit teardown of every slot, regardless of role (WEB_AUTH_PLAN.md
// section 11: "Enabling auth: every existing session is cleared").
void web_auth_table_destroy_all(web_auth_table_t *t);

// --- The LCD's single session ------------------------------------------------
//
// "The LCD has exactly one session, a single struct, not a slot in the web
// table. The panel has one operator." Same fields, no table/eviction logic
// needed since there is only ever one.
typedef struct {
    bool     active;
    web_auth_session_role_t role; // which PIN was entered decides the tier -- section 7
    uint32_t issued_ms;
    uint32_t last_seen_ms;
    bool     prompted;
} web_auth_lcd_session_t;

void web_auth_lcd_session_init(web_auth_lcd_session_t *s);
void web_auth_lcd_session_create(web_auth_lcd_session_t *s, web_auth_session_role_t role, uint32_t now_ms);
void web_auth_lcd_session_touch(web_auth_lcd_session_t *s, uint32_t now_ms);
void web_auth_lcd_session_destroy(web_auth_lcd_session_t *s);

// --- Timeout / inactivity lock (WEB_AUTH_PLAN.md section 8, web half) ------
//
// A `never` timeout is spelled as 0 -- there is no negative or "unset"
// sentinel needed because 0 s is not a usable timeout value in its own
// right (the strength/range rule lives in the password page, section 6,
// which restricts stored values to 1-60 minutes or "never"; this module
// just has to interpret whatever policy value it is handed). This module
// never reads or writes `auth_policy` itself -- the caller passes the
// timeout in on every call, exactly like every other pure module in this
// tree takes its inputs as parameters instead of reaching into a global
// config store.
#define WEB_AUTH_TIMEOUT_NEVER_S    0u
#define WEB_AUTH_PROMPT_WINDOW_MS   10000u // "the 10-second stay-unlocked prompt"

// True iff a session last seen at `last_seen_ms`, under a timeout of
// `timeout_s` seconds, is still valid at `now_ms`. `timeout_s ==
// WEB_AUTH_TIMEOUT_NEVER_S` is always valid (owner decision: "never" must
// work, so an operator is not forced to disable auth to get a permanently
// unlocked interface). The boundary is inclusive: exactly at the timeout
// the session is still valid, one ms past it is not (same convention as
// ota_auth's OTA_AUTH_NONCE_EXPIRY_MS boundary).
bool web_auth_session_is_valid(uint32_t last_seen_ms, uint32_t timeout_s, uint32_t now_ms);

// True iff `now_ms` falls inside the 10 s stay-unlocked prompt window for a
// session last seen at `last_seen_ms` under `timeout_s` -- i.e. the session
// is still valid (per web_auth_session_is_valid()) but within
// WEB_AUTH_PROMPT_WINDOW_MS of expiring. Always false when timeout_s is
// WEB_AUTH_TIMEOUT_NEVER_S, since a session that never expires never
// approaches expiry.
bool web_auth_session_in_prompt_window(uint32_t last_seen_ms, uint32_t timeout_s, uint32_t now_ms);

// --- Auth-disabled inert path (WEB_AUTH_PLAN.md section 11) -----------------
//
// "With auth off, every tier collapses to full access... the enforcement
// pre-handler's first check is web_enabled, and if it is false it returns
// ALLOW immediately, before any session lookup." This module doesn't own the
// enforcement pre-handler (section 5), but it owns the primitive that lets
// that pre-handler short-circuit correctly and cheaply: when auth is
// disabled, none of the session machinery above should be consulted at all,
// so there is no added latency and no new failure mode on a board that has
// never turned auth on. web_auth_effective_role() is the one call the
// enforcement point needs: it captures exactly that short-circuit so the
// "auth off" branch lives in one place, tested once, rather than being
// re-derived at every call site.
//
// Returns WEB_AUTH_SESSION_ROLE_ADMIN immediately, with none of the other
// parameters inspected, when `web_enabled` is false -- collapsing every
// tier to full access, as section 11 requires. When `web_enabled` is true,
// looks the token up in `t` (NULL token_hash means "no session presented",
// e.g. a request with no cookie) and returns its role if the session is
// both found and still valid under `timeout_s` at `now_ms`; returns
// WEB_AUTH_SESSION_ROLE_NONE otherwise (unknown token, or a token that IS in
// the table but has expired -- section 8's lock: "The session drops to the
// unauthenticated tier").
//
// `web_enabled` is a plain, caller-resolved boolean -- this function has
// never read any stored policy flag itself. The section 5 enforcement point
// (the caller) is expected to derive it via
// web_auth_policy_effective_enabled(status, stored_enabled) (from the now-
// landed web_auth_store.h) before calling here, so the UNREADABLE-record
// fail-closed behaviour lives in that one collapse function rather than
// being re-derived at this call site -- this signature does not need to
// change to support that; it already only ever takes an already-resolved
// bool.
//
// `client_ip` (2026-09-17 review, Finding 1 / prior defect 5): the caller's
// resolved peer address, NUL-terminated, or NULL when no peer address could
// be determined. Ignored entirely while `web_enabled` is false (the
// short-circuit above never touches the table). Once a live session is
// found and not expired, this is compared -- exact string match, not a
// prefix -- against the `client_ip` recorded in the slot at login
// (web_auth_table_create_session()). A mismatch, including `client_ip ==
// NULL` (a session token can never legitimately be presented "from no
// address"), resolves to WEB_AUTH_SESSION_ROLE_NONE exactly like an expired
// or unknown token -- see http_session_iface.h's MUST list, which names this
// case explicitly. Exact match, not a /24 or similar prefix, is a
// deliberate choice: this binding exists to stop a captured cookie being
// replayed from a DIFFERENT host, and a prefix broad enough to tolerate
// typical NAT/DHCP churn would also cover most of the same flat LAN the
// threat model in docs/audits/web_auth_adversarial_review_2026-09-17.md's
// Finding 1 is defending against. The operational cost is real and
// intentional: a client whose apparent address changes mid-session (a
// Wi-Fi/cellular handoff, a NAT re-binding) is logged out and must sign in
// again -- the header's stated MUST does not carve out an exception for
// that case, and this implementation does not invent one.
web_auth_session_role_t web_auth_effective_role(const web_auth_table_t *t, bool web_enabled,
                                                 const uint8_t *token_hash, const char *client_ip,
                                                 uint32_t timeout_s, uint32_t now_ms);

// --- Admin-credential bootstrap state (plan items 10 & 11) ------------------
//
// Three paths can leave an interface "enabled" while its administrator
// credential is unconfigured -- and they must all resolve to exactly the
// same caller-visible behaviour, or this is the reset-one-side-of-a-pair bug
// class CLAUDE.md names (four confirmed silent instances already):
//   - First boot: unreachable by construction. An absent auth_policy record
//     collapses to effective_enabled==false via web_auth_policy_effective_
//     enabled(), so this predicate is never even true here -- there is
//     nothing to bootstrap because auth is off.
//   - Field upgrade: same as first boot for the same reason -- an upgraded
//     board has no auth_policy record either, so effective_enabled==false.
//   - The item 10 physical credential reset: reachable, and deliberate. It
//     clears the administrator password/PIN record (configured=false,
//     must_change=true) but leaves the policy record's enabled flags
//     untouched -- "does not disable authentication" (plan item 10) -- so a
//     board can be left with effective auth ON and no administrator
//     credential to log in with.
//
// This is the ONE predicate that names that state, so it is decided once
// here rather than re-derived at every call site that could reach it (today,
// only the item 10 reset reaches it -- but a future path must be checked
// against this same function, not a fresh copy of "enabled && !configured").
// It answers "is this interface waiting on a fresh administrator credential
// with no prior login" -- true iff `effective_enabled` (already collapsed
// via web_auth_policy_effective_enabled(), same convention as
// web_auth_effective_role() above) is true AND
// `admin_credential_configured` (web_auth_store_password_configured(
// WEB_AUTH_ROLE_ADMINISTRATOR) for the web interface, or
// web_auth_store_pin_configured(WEB_AUTH_ROLE_ADMINISTRATOR) for the LCD --
// same formula, either interface) is false.
//
// This must be neither a lockout nor a silent bypass: ordinary login can
// never succeed in this state on its own (web_auth_store_verify_password()/
// _verify_pin() both report false unconditionally against an unconfigured
// record -- ordinary login stays fail-closed, not a silent bypass), so a
// caller that sees this predicate true must route to a credential-bootstrap
// flow that requires no prior session (none can exist) and writes a new
// administrator credential through the ordinary, strength-checked
// web_auth_store_set_password()/_set_pin() -- never a shortcut that skips
// strength validation or invents a session. This predicate only identifies
// the state; it grants nothing by itself. Pure function, no I/O.
bool web_auth_admin_bootstrap_needed(bool effective_enabled, bool admin_credential_configured);

// --- Credential verification seam --------------------------------------------
//
// Section 2/3's credential storage has landed (web_auth_store.h,
// c3008eb8): web_auth_store_verify_password(role, password) and
// web_auth_store_verify_pin(role, pin). These two typedefs are shaped to
// match those functions' signatures exactly, so wiring the real verifiers
// in is a direct assignment at the call site --
//     web_auth_verify_password_fn verify_pw = web_auth_store_verify_password;
//     web_auth_verify_pin_fn      verify_pin = web_auth_store_verify_pin;
// -- not an adapter/shim. `role` here is web_auth_store.h's credential-role
// type (WEB_AUTH_ROLE_USER / WEB_AUTH_ROLE_ADMINISTRATOR), distinct from
// this file's own web_auth_session_role_t (see the top-of-file comment on
// that collision).
typedef bool (*web_auth_verify_password_fn)(web_auth_role_t role, const char *password);
typedef bool (*web_auth_verify_pin_fn)(web_auth_role_t role, const char *pin);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_WEB_AUTH_SESSION_H
