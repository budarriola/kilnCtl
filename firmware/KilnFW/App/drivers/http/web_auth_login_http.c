// web_auth_login_http.c -- see web_auth_login_http.h for scope. Docs/
// WEB_AUTH_PLAN.md section 6, the last missing piece of the browser-side
// auth flow: GET /login (page) and POST /api/auth/login (credential check
// + session mint).
//
// SESSION TABLE / TOKEN HASH OWNERSHIP: this file deliberately does NOT own
// a web_auth_table_t or a token-hashing implementation of its own -- it
// calls http_session_table() and http_session_hash_token()
// (http_session_iface.h), the same table and the same hash
// http_auth_session_resolve() looks sessions up against. Creating a second,
// independently-owned table or hash here would be exactly the
// "reset-one-side-of-a-pair" bug class CLAUDE.md warns about: a session
// minted here would then never resolve there. See http_session_iface.c's
// own header comment, which names this file as the intended adopter.
//
// LOCKOUT: web login gets its OWN escalating per-IP backoff ladder,
// separate from the OTA-esp/OTA-pico/LCD-PIN lockouts -- WEB_AUTH_PLAN.md
// section 7's explicit design note that each authentication surface must
// not share lockout state with any other. (Owner decision, 2026-09-21,
// replacing the earlier 3-failures-per-tier ota_auth_lockout_state_t
// scheme this route used to reuse: "Auth should allow a fast logon. Then
// if a wrong password comes from an ip wait an increasing amount of time
// before the next is accepted. 1st retry 5 sec, second 10, then 30, 60,
// then 5 min before the cycle resets." See LOGIN_BACKOFF_LADDER_MS below.)
//
// REMOTE-VS-LOCAL SCOPE (owner decision, 2026-09-20, verbatim): "All remote
// ip addresses ie. not on the same subnet should be treated as the same ip
// as far as login timeouts go" -- a single remote attacker can rotate
// through addresses to spread failures across many per-IP slots and defeat
// a per-IP ladder entirely, while a genuine LAN client cannot spoof being
// on the LAN. Every client classified off-subnet (login_ip_scope.h) shares
// ONE reserved backoff slot (s_remote_login_slot below), never a slot in
// the ordinary per-IP table -- see login_backoff_slot_for().
//
// Finding 4 fix (2026-09-17 review): a SINGLE global instance meant one
// remote IP failing three logins locked out every other client on the LAN
// too, including the legitimate operator -- a trivial denial-of-service
// against a single-operator device. Lockout state is now tracked per
// source IP, in a small FIXED-SIZE table (LOGIN_LOCKOUT_MAX_IPS below) so
// an attacker sending attempts from many spoofed/rotating source addresses
// cannot grow this table without bound -- once full, the least-recently-
// active IP's slot is evicted for the new one, the same bounded LRU
// discipline web_auth_table_create_session() already uses for the (also
// fixed-size) session table.
//
// Finding 2 fix (2026-09-17 adversarial review of the Finding 4 fix above):
// that first cut of eviction considered every in_use slot interchangeable,
// which let an attacker controlling >= LOGIN_LOCKOUT_MAX_IPS addresses evict
// his own locked slot to keep guessing from a fresh one -- more permitted
// guesses than the global lockout this replaced, not fewer. Eviction now
// skips any slot that is CURRENTLY LOCKED (login_lockout_slot_for()'s own
// header comment has the full reasoning and the accepted DoS tradeoff for
// the case where every slot ends up locked at once).
//
// STACK: the request body (LOGIN_BODY_MAX, 512 bytes) lives on the heap and is
// wiped and freed before the KDF runs; the largest remaining local is the
// 129-byte password, well under the ~256-byte budget
// WEB_AUTH_PLAN.md section 4 documents for the KDF's own locals, and no
// buffer here is enlarged beyond that.
#include "web_auth_login_http.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "hal_sysinfo.h"    // hal_sysinfo_fill_random() -- HAL boundary, no raw esp_random.h
#include "hal_time.h"       // hal_time_now_ms()
#include "http_auth_http.h" // kiln_http_register()
#include "http_form.h"      // http_form_find_field()
#include "http_session_iface.h" // http_session_table(), http_session_hash_token()
#include "../net/login_backoff.h" // LOGIN_BACKOFF_LADDER_MS + login_backoff_* -- shared with the
                                    // LCD PIN keypad (lcd_auth_state.h), 2026-09-28
#include "login_ip_scope.h"   // login_ip_scope_classify() -- remote-vs-local backoff scope
#include "ota_http_util.h"   // ota_http_hex_encode()
#include "security_http_core.h" // SECURITY_HTTP_USERNAME_MAX/PASSWORD_MAX
#include "web_auth_login.h"  // web_auth_login_role_for_username()
#include "web_auth_login_ip_gate.h" // web_auth_login_may_mint_session()
#include "web_auth_session.h"
#include "web_auth_store.h"
#include "web_encoding.h"
#include "wifi_prov.h"           // wifi_prov_get_sta_ip_netmask() -- remote-vs-local backoff scope
#include "wifi_provision_http.h" // wifi_provision_http_get_server()

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "web_auth_login_http";

// Checked variant (ota_http.c, 2026-09-18 follow-up to d2c51f55) -- same
// lookup, but also reports whether a real address was resolved (true) or
// the caller is looking at the shared "unknown" collision sentinel (false).
// login_post_handler() below is the ONE call site in this codebase that
// MINTS a session rather than merely comparing against one that already
// exists, so it is also the one place that distinction matters -- see
// ota_http.c's own comment on this function and this file's
// web_auth_login_ip_gate.h include for why.
bool ota_http_get_client_ip_checked(httpd_req_t *req, char *out, size_t out_len);

/* Embedded via EMBED_TXTFILES, pre-gzipped at configure time by
 * App/drivers/CMakeLists.txt -- same convention as every other *_page.html
 * in this component. */
extern const uint8_t login_page_html_gz_start[] asm("_binary_login_page_html_gz_start");
extern const uint8_t login_page_html_gz_end[] asm("_binary_login_page_html_gz_end");

// This route's own PER-IP lockout table -- separate from every OTA/LCD-PIN
// instance, per WEB_AUTH_PLAN.md section 7. Guarded by s_login_lock, same
// "short critical section around plain state, not around I/O" shape
// ota_http.c's s_ota_lock uses around ota_auth_nonce_issue(). Fixed size
// (Finding 4 fix, see this file's header comment) -- bounded LRU eviction,
// never grows.
#define LOGIN_LOCKOUT_MAX_IPS 16u

// --- Escalating backoff ladder (owner decision 2026-09-21, see this file's
// header comment) -------------------------------------------------------
//
// Extracted to ../net/login_backoff.h 2026-09-28 (owner decision: "apply the
// web password policies to the LCD PIN too") so the LCD PIN keypad
// (lcd_auth_state.c) can mirror this exact ladder and stepping logic by
// calling the same functions against its OWN, separate login_backoff_state_t
// instance, rather than a second hand-copied definition that could drift
// from this one. This file's own per-IP table below (s_login_lockouts,
// s_remote_login_slot) is unaffected and stays exactly as separate from the
// LCD's instance as WEB_AUTH_PLAN.md section 7 requires -- only the ladder
// constant and the four pure functions are now shared, never the state.

typedef struct {
    bool in_use;
    char ip[46];
    uint32_t last_activity_ms;
    login_backoff_state_t backoff;
} login_lockout_slot_t;
static login_lockout_slot_t s_login_lockouts[LOGIN_LOCKOUT_MAX_IPS];

// Reserved shared slot for every client classified LOGIN_IP_SCOPE_REMOTE
// (login_ip_scope.h) -- deliberately NOT one of s_login_lockouts[] above,
// so ordinary per-IP table saturation by LOCAL clients can never evict it,
// and it can never itself be evicted to make room for a LOCAL client
// either. See this file's header comment for the owner requirement this
// implements.
static login_lockout_slot_t s_remote_login_slot;
static SemaphoreHandle_t s_login_lock;

// Must be called with s_login_lock already held. Finds the slot for `ip`,
// touching its last_activity_ms; if none exists, claims a free slot or,
// failing that, evicts the least-recently-active slot AMONG THOSE NOT
// CURRENTLY LOCKED (bounded table, see header comment) and starts that IP
// with fresh (unlocked) lockout state.
//
// Finding 2 fix (2026-09-17 review): the eviction used to consider every
// in_use slot regardless of lock state, so a slot currently serving a lock
// (i.e. an attacker mid-lockout) was just as evictable as any other. An
// attacker controlling >= LOGIN_LOCKOUT_MAX_IPS source addresses (trivial on
// a LAN, or via NAT churn) could rotate to a fresh address once every
// address had been used, evicting his own oldest locked slot to make room --
// three guesses per address, unlimited addresses, which is MORE permitted
// guesses than the single global lockout ca7a7d31 replaced, not fewer. A
// slot that is currently locked (login_backoff_is_locked() true at `now`)
// is now never a candidate for eviction.
//
// Returns NULL when every slot is in_use AND currently locked -- there is no
// safe slot left to reuse for a new IP without evicting an active lock.
// Callers MUST treat NULL as "refuse this login attempt" (see
// login_post_handler()), not as "retry with a fresh slot" -- see this file's
// header comment for the tradeoff this accepts: while the table is fully
// saturated with simultaneously-locked attacker IPs, even a legitimate
// operator's fresh address gets refused rather than bumping one of them.
// That state requires an attacker to actively maintain LOGIN_LOCKOUT_MAX_IPS
// concurrent locked-out addresses (each requiring OTA_AUTH_LOCKOUT_THRESHOLD
// failed attempts to arm, and lock durations that only grow with repeated
// failures), which is a far narrower and more expensive window than the
// eviction-bypass this fix closes, and preserves the per-IP design's actual
// intent -- a real lockout must mean something for its whole duration.
//
// ACCEPTED TRADEOFF (owner decision, 2026-09-17, not a defect awaiting a
// fix -- do not "helpfully" undo this): with LOGIN_LOCKOUT_MAX_IPS == 16,
// one failure is already enough to arm a slot's lock (the ladder's first
// step, see LOGIN_BACKOFF_LADDER_MS), so an attacker who fails once from
// each of 16 distinct LOCAL-subnet addresses and keeps re-failing each one
// just as its wait expires can hold the whole table saturated
// indefinitely at low request volume. While saturated, this function
// returns NULL for any IP without an existing slot, and login_post_handler()
// answers that with a bare 429 *before* credentials are examined --
// including the legitimate operator's own laptop after a DHCP lease
// change, or a phone on a different address. There is no admin override
// and no prune of expired-but-still-in-use slots; recovery is either
// waiting out the attacker or rebooting the board (this table is static
// RAM, cleared by a reset). See login_post_handler()'s 429 site for the
// matching oracle-safety note. (This is the LOCAL-subnet table only --
// every REMOTE-subnet client shares one reserved slot instead, see this
// file's header comment and login_backoff_slot_for() below, so this
// specific saturation cost only applies to an attacker who can reach the
// board from LAN-local addresses.)
static login_lockout_slot_t *login_lockout_slot_for(const char *ip, uint32_t now)
{
    int free_idx = -1;
    int lru_idx = -1;
    uint32_t lru_time = UINT32_MAX;
    for (unsigned i = 0; i < LOGIN_LOCKOUT_MAX_IPS; i++) {
        login_lockout_slot_t *s = &s_login_lockouts[i];
        login_backoff_cycle_reset_if_due(&s->backoff, now);
        if (s->in_use && strcmp(s->ip, ip) == 0) {
            s->last_activity_ms = now;
            return s;
        }
        if (!s->in_use && free_idx < 0) {
            free_idx = (int)i;
        }
        if (s->in_use && !login_backoff_is_locked(&s->backoff, now) && s->last_activity_ms < lru_time) {
            lru_time = s->last_activity_ms;
            lru_idx = (int)i;
        }
    }
    int idx = (free_idx >= 0) ? free_idx : lru_idx;
    if (idx < 0) {
        // Every slot is in_use and currently locked -- see this function's
        // header comment. Deliberately not falling back to "evict the
        // globally-oldest slot regardless of lock state": that is exactly
        // the bypass this fix closes.
        return NULL;
    }
    login_lockout_slot_t *s = &s_login_lockouts[idx];
    memset(s, 0, sizeof(*s));
    s->in_use = true;
    strncpy(s->ip, ip, sizeof(s->ip) - 1);
    s->last_activity_ms = now;
    return s;
}

// Claims/refreshes the shared remote slot -- factored out since both the
// REMOTE and UNKNOWN cases below (finding 3) route here.
static login_lockout_slot_t *claim_remote_slot(uint32_t now)
{
    login_backoff_cycle_reset_if_due(&s_remote_login_slot.backoff, now);
    s_remote_login_slot.in_use = true;
    strncpy(s_remote_login_slot.ip, "*remote*", sizeof(s_remote_login_slot.ip) - 1);
    s_remote_login_slot.ip[sizeof(s_remote_login_slot.ip) - 1] = '\0';
    s_remote_login_slot.last_activity_ms = now;
    return &s_remote_login_slot;
}

// Scope-aware entry point -- decides LOCAL (per-IP table slot, above) vs
// REMOTE (the one shared s_remote_login_slot) before handing off to
// login_lockout_slot_for(). The unresolvable-address sentinel ("unknown",
// ip_known == false) deliberately bypasses classification altogether and
// goes through the ordinary per-string table path unchanged -- it stays
// fail-closed and separate from both the LOCAL and REMOTE buckets, per this
// file's header comment.
static login_lockout_slot_t *login_backoff_slot_for(const char *ip, bool ip_known, uint32_t now)
{
    if (ip_known) {
        char sta_ip[16] = { 0 };
        char sta_netmask[16] = { 0 };
        // Review fix (2026-09-21, finding 4): this used to call
        // wifi_prov_get_sta_ip_netmask(), which round-trips through the
        // Wi-Fi owner-task queue and can block for up to WIFI_OWNER_WAIT_MS
        // (12s) -- called here while s_login_lock is held (both call sites
        // below), which would stall every other concurrent login attempt,
        // including 429 refusals, behind one slow owner-task round trip.
        // wifi_prov_get_cached_sta_ip_netmask() is the non-blocking
        // replacement: a plain spinlocked read of a cache kept fresh by the
        // GOT_IP event handler (wifi_prov_link.c's do_ev_got_ip()), never a
        // queue round trip. Best-effort in the same sense as before: an
        // unpopulated/stale cache reads as empty strings, and
        // login_ip_scope_classify() falls back to the fixed AP subnet check
        // only -- never treated as an error requiring a refusal.
        (void)wifi_prov_get_cached_sta_ip_netmask(sta_ip, sizeof(sta_ip), sta_netmask, sizeof(sta_netmask));
        login_ip_scope_t scope = login_ip_scope_classify(ip, sta_ip, sta_netmask);
        // Review fix (2026-09-21, finding 3): LOGIN_IP_SCOPE_UNKNOWN (a
        // syntactically address-shaped string login_ip_scope_classify()
        // could not parse as IPv4 -- e.g. a raw IPv6 literal that slipped
        // past ota_http_get_client_ip_checked()'s own normalization) used to
        // fall through to the ordinary per-IP table below, keyed on that
        // unparsed string. That gave an attacker who can present many
        // distinct non-IPv4-parsing address strings a fresh ladder per
        // string -- the exact per-IP-table-saturation attack the REMOTE
        // bucket exists to close for ordinary IPv4 clients. Route UNKNOWN
        // into the same shared remote slot. This is NOT the separate
        // ip_known == false sentinel path (ota_http_get_client_ip_checked()
        // returning false, e.g. getpeername() itself failing) -- that stays
        // outside both buckets, below, unchanged.
        if (scope == LOGIN_IP_SCOPE_REMOTE || scope == LOGIN_IP_SCOPE_UNKNOWN) {
            return claim_remote_slot(now);
        }
    }
    return login_lockout_slot_for(ip, now);
}

static uint32_t now_ms(void)
{
    return (uint32_t)hal_time_now_ms();
}

// Human-readable scope label for the 429 log line only, derived from the
// already-resolved slot -- never re-classifies or re-reads the STA ip/netmask
// cache (Finding 4's "a refused attempt makes at most one non-blocking cache
// read" invariant, enforced by test_refused_login_makes_no_extra_sta_ip_netmask_calls(),
// must hold here too). `slot_ip` is NULL when login_backoff_slot_for()
// returned NULL (the LOCAL table saturated with other IPs' locks -- see
// login_lockout_slot_for()'s header comment); REMOTE and UNKNOWN scope share
// one slot keyed "*remote*" and are not distinguished here, which is fine
// since this line is diagnostic only, never security-relevant (the response
// itself stays scope-blind, see login_post_handler()'s 429 site for why).
static const char *login_scope_label_for_log(const char *ip, bool ip_known, const char *slot_ip)
{
    if (!ip_known) {
        return "UNKNOWN";
    }
    if (slot_ip == NULL) {
        return "LOCAL(saturated)";
    }
    if (strcmp(slot_ip, ip) == 0) {
        return "LOCAL";
    }
    return "REMOTE";
}

static esp_err_t login_page_get_handler(httpd_req_t *req)
{
    if (!web_client_accepts_gzip(req)) {
        return web_send_gzip_not_acceptable(req, TAG, "login_page.html");
    }
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    web_set_asset_cache_headers(req);
    return httpd_resp_send(req, (const char *)login_page_html_gz_start,
                            (size_t)(login_page_html_gz_end - login_page_html_gz_start));
}

// "username"/"password" form fields. Sized for the worst legal body: "username="
// (9) + a 32-char username and a 128-char password both fully percent-encoded
// (3x each = 480) + "&password=" (10) = 499, rounded up. Too large for the
// 8 KB httpd stack, so the body buffer lives on the heap (see the handler).
#define LOGIN_BODY_MAX 512

// Explicit wipe the optimizer cannot drop (volatile stores).
static void login_wipe(void *buf, size_t len)
{
    volatile uint8_t *p = (volatile uint8_t *)buf;
    while (len--) {
        *p++ = 0;
    }
}

static esp_err_t login_post_handler(httpd_req_t *req)
{
    char ip[46];
    bool ip_known = ota_http_get_client_ip_checked(req, ip, sizeof(ip));

    bool locked = false;
    bool have_slot_ip = false;
    char slot_ip_snapshot[46] = { 0 };
    uint32_t retry_after_ms = 0;
    if (xSemaphoreTake(s_login_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        uint32_t now = now_ms();
        login_lockout_slot_t *slot = login_backoff_slot_for(ip, ip_known, now);
        if (slot != NULL) {
            have_slot_ip = true;
            strncpy(slot_ip_snapshot, slot->ip, sizeof(slot_ip_snapshot) - 1);
        }
        // Finding 2 fix (kept under the new ladder): NULL means every LOCAL
        // slot is in_use and currently locked (the table is saturated with
        // active attacker lockouts) -- refuse this attempt outright rather
        // than dereferencing a slot that does not exist. See
        // login_lockout_slot_for()'s header comment for the tradeoff. A
        // REMOTE-scope client never sees NULL here (its reserved slot is
        // never subject to that saturation).
        locked = (slot == NULL) || login_backoff_is_locked(&slot->backoff, now);
        if (locked) {
            // Review fix (2026-09-21, finding 1): slot == NULL (the LOCAL
            // table saturated with OTHER IPs' active locks, see
            // login_lockout_slot_for()'s header comment) used to fall
            // through with retry_after_ms left at its 0 initializer, which
            // the code below then reported as a bare "1" second -- visibly
            // different from a genuine lock's 5/10/30/60/300, and exactly
            // the side channel the comment below already says this path
            // must not be: an attacker probing for a "1" vs a ladder value
            // could infer table saturation itself, not merely "is this one
            // IP locked". Report the ladder's LAST (worst-case) step here
            // instead, matching what a maximally-escalated genuine lock
            // would also show, so saturation is indistinguishable from an
            // ordinary long lock.
            retry_after_ms = (slot != NULL) ? (slot->backoff.locked_until_ms - now)
                                             : LOGIN_BACKOFF_LADDER_MS[LOGIN_BACKOFF_LADDER_LEN - 1];
        }
        xSemaphoreGive(s_login_lock);
    } else {
        ESP_LOGW(TAG, "login from %s: internal lock timeout, refused", ip);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_OK;
    }
    if (locked) {
        // Deliberately identical status line and body whether `slot` was a
        // genuine per-IP lockout or NULL (the LOCAL table saturated with
        // OTHER IPs' locks -- see login_lockout_slot_for()'s header comment
        // for the tradeoff). If the saturation case answered any
        // differently, the response itself would be an oracle telling an
        // attacker whether the table is full, which is exactly the kind of
        // side channel this backoff exists to deny -- so a legitimate
        // operator refused here sees the same "too many failed attempts"
        // message a genuinely locked-out IP does. Retry-After is safe to
        // disclose either way: it names how long THIS response is refused
        // for, not anything about other clients' state. Rounded up so a
        // client that honors it never retries a fraction of a second early.
        uint32_t retry_after_s = (retry_after_ms == 0) ? 1u : (retry_after_ms + 999u) / 1000u;
        // Log-only (never in the response, which stays deliberately scope-
        // blind per the comment above): so a locked-out client leaves a
        // trace in the device log. Never logs username/password -- neither
        // is available yet at this point in the handler.
        ESP_LOGW(TAG, "login blocked (429) from %s scope=%s retry_after=%us", ip,
                 login_scope_label_for_log(ip, ip_known, have_slot_ip ? slot_ip_snapshot : NULL),
                 (unsigned)retry_after_s);
        char retry_after_hdr[16];
        snprintf(retry_after_hdr, sizeof(retry_after_hdr), "%u", (unsigned)retry_after_s);
        httpd_resp_set_hdr(req, "Retry-After", retry_after_hdr);
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_send(req, "too many failed attempts, try again later", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    if (req->content_len <= 0 || req->content_len >= LOGIN_BODY_MAX) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "body missing or too large");
        return ESP_OK;
    }

    // Finding 5 fix (2026-09-17 review): httpd_req_recv() is not guaranteed
    // to fill the whole request in one call -- ESP-IDF's own docs note it
    // may return fewer bytes than asked even before EOF/error. A single
    // unlooped call silently truncated the body on any connection that
    // split it across TCP segments, corrupting username/password field
    // parsing rather than failing loud. Loop until content_len bytes are
    // read, same shape as security_http.c's security_post_handler().
    char *body = (char *)malloc(LOGIN_BODY_MAX);
    if (body == NULL) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "out of memory");
        return ESP_OK;
    }
    size_t received = 0;
    while (received < (size_t)req->content_len) {
        int ret = httpd_req_recv(req, body + received, (size_t)req->content_len - received);
        if (ret <= 0) {
            login_wipe(body, LOGIN_BODY_MAX);
            free(body);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "failed to read body");
            return ESP_OK;
        }
        received += (size_t)ret;
    }
    body[received] = '\0';

    char username[SECURITY_HTTP_USERNAME_MAX + 1];
    char password[SECURITY_HTTP_PASSWORD_MAX + 1];
    int username_len = http_form_find_field(body, "username", username, sizeof(username));
    int password_len = http_form_find_field(body, "password", password, sizeof(password));
    // The plaintext body is no longer needed once the fields are parsed.
    login_wipe(body, LOGIN_BODY_MAX);
    free(body);
    body = NULL;
    if (username_len < 0 || password_len < 0) {
        login_wipe(password, sizeof(password));
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "username and password are required");
        return ESP_OK;
    }

    // Resolve which role's record to check against WITHOUT running the KDF
    // twice -- see web_auth_login.h's header comment for the full
    // rationale (the administrator username is not a secret).
    web_auth_password_record_t admin_record;
    const char *admin_username = NULL;
    if (web_auth_store_load_password(WEB_AUTH_ROLE_ADMINISTRATOR, &admin_record) == WEB_AUTH_LOAD_OK &&
        admin_record.configured) {
        admin_username = admin_record.username;
    }
    web_auth_session_role_t candidate_role = web_auth_login_role_for_username(username, admin_username);
    web_auth_role_t store_role =
        (candidate_role == WEB_AUTH_SESSION_ROLE_ADMIN) ? WEB_AUTH_ROLE_ADMINISTRATOR : WEB_AUTH_ROLE_USER;

    bool ok = web_auth_store_verify_password(store_role, password);

    // Wipe the plaintext password from this stack frame the moment it is no
    // longer needed -- same discipline web_auth_store.h documents for its
    // own callers ("never logged, never stored, discarded ... immediately
    // after this call").
    login_wipe(password, sizeof(password));

    if (xSemaphoreTake(s_login_lock, pdMS_TO_TICKS(50)) != pdTRUE) {
        // Fail closed: an attempt whose outcome cannot be recorded must not
        // be answered (a success would mint nothing, a failure would go
        // uncounted -- unlimited free guesses under contention). Same
        // response for ok and !ok so it is not an oracle.
        ESP_LOGW(TAG, "login from %s: lock timeout recording attempt, refused", ip);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "busy");
        return ESP_OK;
    }
    {
        uint32_t now = now_ms();
        login_lockout_slot_t *slot = login_backoff_slot_for(ip, ip_known, now);
        // Finding 2 fix (kept under the new ladder): the pre-check above
        // already refused this request when the table is saturated with
        // locked IPs, so NULL here would mean the saturation state changed
        // between the two calls under the same held lock -- not expected,
        // but handled rather than dereferencing NULL. This attempt was NOT
        // refused (we only reach here past the `locked` check above), so
        // recording a failure/success here is exactly one ladder step, per
        // this file's header comment -- a refused attempt never reaches
        // this line at all, and therefore never counts or extends the wait.
        if (slot != NULL) {
            if (ok) {
                login_backoff_record_success(&slot->backoff);
            } else {
                login_backoff_record_failure(&slot->backoff, now);
            }
        }
        xSemaphoreGive(s_login_lock);
    }

    if (!ok) {
        ESP_LOGW(TAG, "login failed from %s", ip);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid username or password");
        return ESP_OK;
    }

    // Fail-closed follow-up to d2c51f55 (2026-09-18): credentials verified,
    // but if this request's own address could not be determined, `ip` is
    // the shared "unknown" collision sentinel (web_auth_session.h) -- every
    // OTHER client whose lookup also fails presents the identical string.
    // Minting a session bound to it would let any of THOSE clients present
    // "unknown" too and be accepted as this one -- a cross-client binding
    // hole, not merely a lockout. Refuse instead, via
    // web_auth_login_may_mint_session() (web_auth_login_ip_gate.h) rather
    // than re-deriving this from a strcmp() against the sentinel text,
    // which would just be a second, fragile copy of the same contract.
    //
    // The response is deliberately BYTE-IDENTICAL to the "wrong password"
    // path just above (same status, same body) so this refusal is not
    // itself an oracle telling a caller its credentials were actually
    // correct -- same non-disclosure standard this file's per-IP lockout
    // (Finding 4/2 above) already holds itself to. The only place this
    // outcome is visible is the log line below, which is not attacker-
    // reachable.
    if (!web_auth_login_may_mint_session(ip_known)) {
        ESP_LOGW(TAG, "login credentials valid but client address could not be determined "
                      "(ip=%s); refusing to mint a session bound to the shared sentinel",
                 ip);
        httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "invalid username or password");
        return ESP_OK;
    }

    // Mint a fresh 32-byte random token, hash it with the SAME function the
    // resolver (http_session_iface.c) hashes lookups with, and create the
    // session in the SAME table that resolver reads -- see this file's
    // header comment.
    uint8_t token_raw[32];
    hal_sysinfo_fill_random(token_raw, sizeof(token_raw));
    char token_hex[sizeof(token_raw) * 2 + 1];
    ota_http_hex_encode(token_raw, sizeof(token_raw), token_hex);

    uint8_t token_hash[32];
    http_session_hash_token(token_hex, strlen(token_hex), token_hash);

    web_auth_session_role_t session_role =
        (store_role == WEB_AUTH_ROLE_ADMINISTRATOR) ? WEB_AUTH_SESSION_ROLE_ADMIN : WEB_AUTH_SESSION_ROLE_USER;
    size_t session_idx =
        web_auth_table_create_session(http_session_table(), token_hash, ip, session_role, now_ms());
    // 2026-09-29 owner decision: tag the session's initial AP-origin state at
    // login time. Necessary here specifically because POST /api/auth/login
    // is ROUTE_TIER_OPEN (http_auth_http.c's activity-touch path never runs
    // for it), so login is the only chance to record whether THIS session
    // started life over the SoftAP interface before any ordinary request
    // touch has a chance to (re-)tag it -- see web_auth_session.h's via_ap
    // field comment for why later touches can still flip it either way.
    web_auth_table_set_via_ap(http_session_table(), session_idx,
                               wifi_prov_request_arrived_on_ap(httpd_req_to_sockfd(req)));

    // 128 bytes: "kiln_sid=" (9) + 64 hex chars + "; HttpOnly; SameSite=Strict; Path=/"
    // (35) + NUL = 109 -- Finding 5 fix (2026-09-17 review): this comment's
    // arithmetic previously read "36 + NUL = 110", one byte over the actual
    // 108-byte-plus-NUL total; the buffer itself was already correctly sized
    // (128 is still comfortably above 109), only the comment's sum was wrong.
    // The previous 96-byte buffer was too small to hold a real cookie and
    // made every successful login fail with a spurious 500 (found by this
    // pass's host tests, not itself one of the seven audit findings, but
    // blocking their test coverage). Still well under the stack budget,
    // this file's stated httpd-stack budget.
    char cookie[128];
    int cookie_len = snprintf(cookie, sizeof(cookie), HTTP_SESSION_COOKIE_NAME "=%s; HttpOnly; SameSite=Strict; Path=/",
                               token_hex);
    if (cookie_len < 0 || (size_t)cookie_len >= sizeof(cookie)) {
        ESP_LOGE(TAG, "Set-Cookie formatting failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "internal error");
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);

    ESP_LOGI(TAG, "login succeeded from %s (role=%d)", ip, (int)session_role);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

// POST /api/auth/logout -- ROUTE_TIER_USER (route_tier_table.h), so an
// unauthenticated caller never reaches this handler body at all (the shared
// pre-handler answers 401 first). Revokes the caller's OWN session (never
// takes a target token/id from the request body -- there is no "log out
// someone else" surface) via http_auth_session_logout() (http_session_iface.h),
// which acts against the SAME table http_auth_session_resolve() reads, then
// clears the cookie with the SAME name/path/flags login_post_handler() sets
// it with above, plus Max-Age=0 so the browser discards it immediately
// rather than waiting for it to merely stop matching a live session.
// Idempotent within what the tier admits: any caller that reaches this
// body at all gets a clean 204, whether or not the cookie still matched a
// live slot. It is NOT true that "a caller with no cookie or an expired
// session still gets a 204" -- ROUTE_TIER_USER means the pre-handler's
// http_auth_session_resolve() (expiry AND exact client_ip binding) has
// already answered 401 for both of those, so the cookie-clearing Set-Cookie
// below is never sent to them. That is deliberate and harmless: a session
// the server has already expired is unusable anyway, and app.js's own
// 401 handling navigates such a caller to /login regardless.
static esp_err_t logout_post_handler(httpd_req_t *req)
{
    char token[128] = { 0 };
    (void)http_auth_extract_session_token(req, token, sizeof(token));
    if (token[0] != '\0') {
        http_auth_session_logout(token);
    }

    char cookie[128];
    int cookie_len = snprintf(cookie, sizeof(cookie),
                               HTTP_SESSION_COOKIE_NAME "=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
    if (cookie_len < 0 || (size_t)cookie_len >= sizeof(cookie)) {
        ESP_LOGE(TAG, "logout Set-Cookie formatting failed");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "internal error");
        return ESP_OK;
    }
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    httpd_resp_set_status(req, "204 No Content");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t web_auth_login_http_start(void)
{
    if (s_login_lock == NULL) {
        s_login_lock = xSemaphoreCreateMutex();
        if (s_login_lock == NULL) {
            ESP_LOGE(TAG, "xSemaphoreCreateMutex failed");
            return ESP_ERR_NO_MEM;
        }
    }

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t login_page_uri = {
        .uri = "/login",
        .method = HTTP_GET,
        .handler = login_page_get_handler,
    };
    esp_err_t err = kiln_http_register(server, &login_page_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/login) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t login_post_uri = {
        .uri = "/api/auth/login",
        .method = HTTP_POST,
        .handler = login_post_handler,
    };
    err = kiln_http_register(server, &login_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/auth/login) failed: %s", esp_err_to_name(err));
        return err;
    }

    static const httpd_uri_t logout_post_uri = {
        .uri = "/api/auth/logout",
        .method = HTTP_POST,
        .handler = logout_post_handler,
    };
    err = kiln_http_register(server, &logout_post_uri);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_register_uri_handler(/api/auth/logout) failed: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "web login routes up: GET /login, POST /api/auth/login, POST /api/auth/logout");
    return ESP_OK;
}
