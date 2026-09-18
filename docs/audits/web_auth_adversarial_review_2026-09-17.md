# Adversarial security review of the web-authentication feature — 2026-09-17

Scope: the browser-side authentication work now on `origin/main`, measured in a
clean worktree detached at `8714ee31` (verified `origin/main` at review time).
Every line number below was read at that tip, not carried over from a commit
message or a plan status line.

Commits in scope:

- `7d00e531` — login page, `POST /api/auth/login`, the first production caller
  of `web_auth_table_create_session()`, the shared `HTTP_SESSION_COOKIE_NAME`
  macro and the new `extract_named_cookie()` parser.
- `1d3ac303` — **not the retiering change.** This hash is a merge commit
  (`Merge remote-tracking branch 'origin/main' into HEAD`). The three
  heat-driving abort routes were actually retiered to `ROUTE_TIER_SAFETY_REDUCE`
  by `26f9d896` and `b91b82f0`. Anything attributing the retiering to
  `1d3ac303` is citing the merge, not the change.
- `9c2b1c1b` — OTA status route payload trimmed.
- `e0ef5bf0` — `/settings/security` page plus `GET /api/auth/config` and
  `POST /api/auth/security`.
- `8714ee31` — item 12b "Clear login credentials" ADMIN action.

Each finding says whether it was **confirmed by reading the code** or is only
**suspected**. Findings that turned out to be false alarms are kept as
checked-and-cleared entries rather than deleted.

**No defect below was fixed in this pass.** This document is the report only.

---

## Part 1 — Re-verification of the five prior defects

Each was treated as a claim to re-verify, not a closed fact.

### Prior defect 1 — a consumer with no producer at feature scale

**GENUINELY CLOSED. Confirmed by reading the code.**

`web_auth_table_create_session()` (`firmware/KilnFW/App/drivers/net/web_auth_session.c:35`)
now has exactly one production call site outside its own translation unit and
its tests:
`firmware/KilnFW/App/drivers/http/web_auth_login_http.c:188`.

That call mints into `http_session_table()` — the same single static table
`http_auth_session_resolve()` reads — and hashes the token with
`http_session_hash_token()` (`web_auth_login_http.c:184`), the same function the
resolver hashes lookups with. Producer and consumer are the same table and the
same hash, so the lockout scenario (auth enabled, ~120 of 137 routes
permanently unreachable because no session could ever exist) is no longer
reachable.

### Prior defect 2 — a transition rule with no caller

**GENUINELY CLOSED. Confirmed by reading the code.**

`web_auth_policy_check_transition()`
(`firmware/KilnFW/App/drivers/persist/web_auth_store.c:541`) has exactly one
production call site outside its own translation unit and its tests:
`firmware/KilnFW/App/drivers/http/security_backend_web_auth.c:182`.

Its outputs are genuinely consumed, not merely received:
`security_backend_web_auth.c:203` calls
`web_auth_table_destroy_all(http_session_table())` on the web-clear flag and
`:206` calls `ui_lcd_lock_force_lock()` on the LCD-clear flag.

### Prior defect 3 — a fail-open sentinel inside a fail-closed design

**GENUINELY CLOSED. Confirmed by reading the code.**

`firmware/KilnFW/App/drivers/http/http_session_iface.c:86-115` replaces the bare
`uint32_t` return with

```c
typedef struct { uint32_t timeout_s; bool unreadable; } resolved_timeout_t;
```

and sets `.unreadable = true` for `WEB_AUTH_LOAD_UNREADABLE` instead of
returning the `WEB_AUTH_TIMEOUT_NEVER_S` (0) sentinel that
`web_auth_session_is_valid()` treats as always-valid
(`web_auth_session.c:147-149`). All three callers deny on that flag:
`http_auth_session_resolve` (lines 149-157), `http_auth_session_status`
(186-188), `http_auth_session_touch` (235-237).

An unreadable policy blob no longer produces an immortal session.

### Prior defect 4 — reset one side of a pair, twice

**BOTH HALVES GENUINELY CLOSED. Confirmed by reading the code.**

*Half A — physical reset clearing only the web record.* Closed.
`web_auth_store_clear_for_physical_reset()` (`web_auth_store.c:601`) now clears
both blobs: the web administrator record (`WEB_AUTH_KEY_WEB`, with
`must_change = true`, `configured = false`) **and** the LCD administrator PIN
record (`WEB_AUTH_KEY_LCD`, `configured = false`), each followed by an
independent read-back verification that returns `false` on mismatch rather than
trusting the write's own return code. The operator who forgets the LCD PIN and
performs the four-corner gesture now actually recovers the LCD side.

This path is also **not vacuous**: the seam is wired at
`firmware/KilnFW/App/drivers/net/auth_reset_gesture_wiring.c`, which assigns
`s_singleton.clear_credentials_fn = web_auth_store_clear_for_physical_reset`,
and the singleton is driven from real UI code —
`firmware/KilnFW/App/drivers/ui/ui_page_home_actions.c:318-320` calls
`auth_reset_gesture_confirm()` on the live gesture state.

*Half B — `ui_lcd_lock.c` tick returning early.* Closed.
`firmware/KilnFW/App/drivers/ui/ui_lcd_lock.c:117-141`: the `!policy.enabled`
branch now force-locks (and closes the prompt and any open keypad) before
returning, instead of returning with `granted_role` untouched. Disabling then
re-enabling auth no longer leaves the panel usable at ADMIN tier with no fresh
PIN.

### Prior defect 5 — documented IP binding never enforced

**STILL OPEN. Confirmed by reading the code.** See Finding 1 below for the
failure scenario; this is the highest-severity item in this review.

The contract in `firmware/KilnFW/App/drivers/http/http_session_iface.h:60-68`
states the resolver MUST return `HTTP_AUTH_ROLE_NONE` for, among other cases,
"a slot whose IP binding does not match `client_ip`". The implementation at
`firmware/KilnFW/App/drivers/http/http_session_iface.c:135-136` opens with:

```c
http_auth_role_t http_auth_session_resolve(const char *token, const char *client_ip) {
    (void)client_ip; // the session table's client_ip binding is section 6's
```

`client_ip` is discarded. The binding is captured at login
(`web_auth_login_http.c:188` passes `ip` into the slot, and
`web_auth_session.c:56-63` stores it) but never compared on any subsequent
request.

---

## Part 2 — New findings, ranked by severity

### Finding 1 — HIGH — session cookie is not bound to a client address, contradicting its own stated contract

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/http/http_session_iface.c:135-136` versus
`firmware/KilnFW/App/drivers/http/http_session_iface.h:60-68`.

The slot's `client_ip` field is written on every login and read by nothing. The
header's normative MUST is unimplemented, and the code comment explaining the
`(void)client_ip;` argues the resolver only looks a token up by hash — which is
precisely the behaviour the contract forbids.

Failure scenario: an administrator signs in from a workstation on the LAN. The
`kiln_sid` cookie value is captured — by any of the ordinary ways a bearer
token leaks on a flat LAN where this board speaks plain HTTP: a proxy log, a
browser extension, shoulder-surfed devtools, or passive capture, since nothing
in this stack is TLS-protected. A second host on the same LAN replays that
cookie value against `POST /api/auth/security` and is served as ADMIN, because
the resolver never compares the requesting peer against the address recorded in
the slot. The stored binding that would have refused the replay exists in
memory and is simply never consulted. From there the attacker can set both
passwords, set both LCD PINs, or clear every credential (Finding 5).

Note this is an unimplemented *defence in depth* layer, not the only control —
the token itself is 32 bytes from `esp_fill_random()` (see cleared entry C4) and
is not guessable. The defect is that the documented second factor is absent
while the header asserts it is present, which is the more dangerous shape: a
future reviewer reading the header will believe replay from a foreign address
is already refused.

### Finding 2 — HIGH — the cookie parser aborts on the first `kiln_sid` match, so one attacker-planted cookie permanently locks out a signed-in administrator

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/http/http_auth_http.c:70-72`.

```c
if (val_len == 0 || val_len >= out_len) {
    return false;
}
```

On encountering a cookie named `kiln_sid` whose value is empty or too long, the
function returns `false` immediately. It does **not** continue scanning the rest
of the header for another `kiln_sid` with a usable value. The loop's
continue-path (lines 78-82) is reached only when the name does not match at all.

Failure scenario: the operator's browser holds a valid session cookie for the
board. Any other content the browser loads that can set a cookie on the board's
hostname — a second service on that host, a captive-portal style injection on
the plain-HTTP LAN, or any page able to write a host-scoped cookie — sets
`kiln_sid=` (empty value) with a broader `Path` or an earlier-sorting path, so
the browser serializes it first. Every subsequent request from that operator
carries `Cookie: kiln_sid=; kiln_sid=<real token>`. The parser matches the first
`kiln_sid`, sees `val_len == 0`, returns `false`, and the pre-handler resolves
`HTTP_AUTH_ROLE_NONE`. The administrator is served 401 on ~120 routes and cannot
sign back in usefully, because a fresh login sets yet another `kiln_sid` that
still sorts after the planted one. Recovery requires manually clearing site data
in the browser — a step an operator has no reason to guess at, since the login
page will appear to accept the password and then still deny every page.

Safety-relevant consequence: the abort routes are `ROUTE_TIER_SAFETY_REDUCE` and
the dashboard is OPEN, so stopping a firing still works. The lockout is a
denial of administration, not a denial of safety. That containment is real and
is why this is HIGH rather than critical.

The same bug shape yields a lower-effort variant: the value is also rejected
when `val_len >= out_len`, so a planted `kiln_sid` with a long junk value
produces the identical permanent lockout.

### Finding 3 — MEDIUM — any Cookie header at or above 128 bytes is discarded whole, silently ending the session

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/http/http_auth_http.c:119-121`.

```c
size_t cookie_hdr_len = httpd_req_get_hdr_value_len(req, "Cookie");
if (cookie_hdr_len > 0 && cookie_hdr_len < sizeof(cookie)) {
```

with `char cookie[128]`. A header of 128 bytes or more is not parsed at all —
the guard simply skips the block, leaving the token empty, which resolves to
`HTTP_AUTH_ROLE_NONE`.

The board's own session cookie is already substantial: `kiln_sid=` (9 bytes)
plus a 64-character hex token is 73 bytes. That leaves under 55 bytes of
headroom for every other cookie the browser will send to that host. Two or three
unrelated cookies — an analytics cookie, a framework session, a preference
cookie set by any other service sharing the hostname (cookies are not
port-scoped, so a different service on the same host on another port is enough)
— push the header past 128 and the operator is logged out on every request with
no diagnostic anywhere.

Failure scenario: the operator runs any second web service on the same machine
name as the kiln board, or the board is reached through a hostname that some
other tool has already set cookies on. Their kiln session works one day and
returns 401 on every ADMIN page the next, with a valid cookie sitting in the
browser, and nothing logged to explain it.

This is deliberate fail-closed behaviour and the comment says so. It is reported
because the threshold is set too close to the feature's own normal traffic to be
a safe margin, not because failing closed is wrong.

### Finding 4 — MEDIUM — login lockout is one global counter, so any LAN host can lock the administrator out

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/http/web_auth_login_http.c:72`.

```c
static ota_auth_lockout_state_t s_login_lockout;
```

One instance for the whole route, not per-IP. The file's header comment
(lines 16-22) states this deliberately, reasoning that the board is a
single-operator LAN device.

Failure scenario: any other host on the LAN — a compromised IoT device, a guest
laptop, a mis-pointed script — sends enough failed `POST /api/auth/login`
attempts to trip the lockout. The legitimate administrator, at a different
address, is then served `429 Too Many Requests` at `web_auth_login_http.c:111`
and cannot sign in for the lockout window, no matter how correct their password
is. Repeating the attempts holds the lockout open indefinitely.

The single-operator assumption is defensible for *lockout accounting* but it is
the wrong assumption for *attribution*: the design note reasons about how many
legitimate operators exist, when the relevant question is how many hosts can
reach the port. Heat-safety routes are unaffected (SAFETY_REDUCE/OPEN), so
again this is denial of administration.

### Finding 5 — MEDIUM — clearing credentials leaves the board in an unauthenticated bootstrap window reachable from the network

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/persist/web_auth_store.c:714` onward
(`web_auth_store_clear_all_credentials()`), reached via
`security_backend_web_auth.c:289` and `security_http_core.c:191-214`.

The clear is thorough — both roles in the web blob and both roles in the LCD
blob, each read-back verified — and it deliberately leaves the policy record
untouched, which the code comments argue for at length (silently disabling auth
would be a larger hole). That reasoning is sound.

The consequence is still worth naming: after the clear, the policy still reads
`web_enabled == true` while no administrator credential is configured. That is
exactly the state `web_auth_admin_bootstrap_needed()`
(`web_auth_session.c:202-205`) reports true for, which opens the
`ROUTE_TIER_ADMIN_BOOTSTRAP` route — `auth_bootstrap_password_post_handler()` at
`security_backend_web_auth.c:336` — to an unauthenticated caller.

Failure scenario: an administrator clicks "Clear login credentials" intending to
start fresh, then walks away or is interrupted before setting a new password.
During that window, the first host on the LAN to POST to the bootstrap route
sets the administrator password and owns the board. Unlike the physical reset
gesture — which requires E-stop asserted, no firing, no heat, and four corner
taps in order within 10 s (`auth_reset_gesture.c`, and its preconditions are
tested) — this clear is triggered remotely over HTTP with no physical presence
requirement, so the resulting bootstrap window is reachable by whoever is
already on the network rather than by whoever is standing at the kiln.

Suspected, not confirmed: whether the bootstrap handler applies any additional
rate limit or first-writer-wins guard of its own. That handler was read only at
its entry point.

### Finding 6 — MEDIUM — the `9c2b1c1b` build-identity redaction is inert in the board's default configuration

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/http/ota_http_esp.c:548-559` and
`firmware/KilnFW/App/drivers/http/http_auth_http.c:168-170`.

The redaction is gated on `http_auth_caller_is_admin(req)`:

```c
bool is_admin = http_auth_caller_is_admin(req);
```

and `http_auth_caller_is_admin()` opens with

```c
if (!http_auth_policy_web_enabled()) {
    return true;
}
```

`http_auth_policy_web_enabled()`
(`firmware/KilnFW/App/drivers/http/http_auth_policy_iface.c`) returns `false`
when the policy record is `WEB_AUTH_LOAD_ABSENT` — the state of a board that has
never had authentication configured, which is the shipping default.

Failure scenario: on a board with authentication not yet enabled (the default),
any unauthenticated caller — `/api/ota/esp/status` is an OPEN route — receives
the full `commit`, `build_date` and `dirty` fields, because `is_admin` short-
circuits to `true` for everyone. The leak `9c2b1c1b` set out to close is closed
only once an administrator has separately turned authentication on.

This follows directly from plan section 11's "a board with auth off is exactly
as open as the board is today", so it may well be intended. It is reported
because the commit is recorded as having closed a build-identity leak, and in
the default configuration it does not.

### Finding 7 — LOW — `/status` appears twice in the route tier table, and the firmware and the coverage checker read opposite rows

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/http/route_tier_table.h:141` and `:322`.

Both are literal array entries:

```c
ROUTE_TIER("/status", HTTP_GET, ROUTE_TIER_OPEN),   /* line 141 */
ROUTE_TIER("/status", HTTP_GET, ROUTE_TIER_OPEN),   /* line 322 */
```

The comment attached to the line-322 row asserts the opposite of what the file
contains — "kept as a single entry, not duplicated, since a table keyed on
(uri, method) can only hold one tier per key by construction". The table is a
flat C array, not a keyed map; nothing prevents the duplicate, and the duplicate
is there.

The two readers disagree about which row wins:

- Firmware: `http_auth_lookup_tier()`
  (`firmware/KilnFW/App/drivers/http/http_auth_enforce.c:12-21`) scans forward
  and returns on first match — **line 141 wins**.
- Coverage checker: `Get-TieredKeys`
  (`tools/check_route_tier_coverage.ps1:111-115`) builds a hashtable with
  `$keys[$key] = tier`, so a repeated key silently overwrites — **line 322
  wins**.

Both rows are `ROUTE_TIER_OPEN` today, so behaviour and check agree by
coincidence and the defect is currently inert.

Failure scenario: a future change tightens `/status` by editing the row it finds
first in the file — line 141 — to `ROUTE_TIER_USER`, intending to close the
Wi-Fi status page to anonymous callers. The firmware honours it. The coverage
checker, reading line 322, still sees OPEN and reports the route correctly
tiered. The reverse edit is worse: tightening only line 322 leaves the firmware
serving `/status` OPEN while the checker reports it as protected — a route that
every mechanical guard says is closed and that is in fact open. The
single-source-of-truth property the table exists to provide is not actually
enforced against duplicate keys by either reader.

### Finding 8 — LOW — unlooped body read on the login route turns a split TCP segment into a failed login that also counts toward lockout

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/http/web_auth_login_http.c:122`.

```c
int received = httpd_req_recv(req, body, req->content_len);
```

A single call, with no loop. `httpd_req_recv()` may legitimately return fewer
bytes than requested. The neighbouring newer handler gets this right —
`security_http.c:151` reads in a proper loop
(`httpd_req_recv(req, body + received, req->content_len - received)`), so the
correct pattern is already established in this feature and was not applied here.

Failure scenario: the POST body is split across TCP segments (a long password, a
slow or lossy Wi-Fi link, an interposed proxy). The handler reads only the first
segment, NUL-terminates it at `body[received]`, and `http_form_find_field()`
either fails to find `password=` or finds a truncated value. The login fails
with 401 despite a correct password — and, worse, the failure is recorded:
`ota_auth_lockout_record_failure()` at line 163 runs on that path. A few
consecutive split reads trip the global lockout of Finding 4, so a network
condition becomes an administrative lockout with no indication that the password
was ever correct.

### Finding 9 — LOW — `web_auth_store_verify_password()` returns before the KDF when a record is unconfigured, exposing a timing oracle for which roles exist

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/persist/web_auth_store.c:337-353`.

```c
if (!rec.configured) {
    return false;
}
uint8_t computed[WEB_AUTH_HASH_LEN];
web_auth_hash_compute(...);
```

The early return skips PBKDF2 entirely. The comparison itself is constant-time
(`web_auth_constant_time_equal`), and the per-record salt is handled correctly —
this is not a hash-comparison flaw.

Failure scenario: an unauthenticated caller posts to `/api/auth/login` with an
arbitrary username and measures response latency. A submitted username matching
the configured administrator (`web_auth_login_role_for_username()` routes it to
the administrator record) runs the full KDF and answers slowly; any other
username routes to the `user` record, and if that record is unconfigured the
answer returns in microseconds. The attacker learns the administrator's username
and which roles have credentials set, without a single correct guess. The
administrator username is explicitly documented in this codebase as not being a
secret, which bounds the impact — the durable leak is the configured/unconfigured
map, which usefully tells an attacker whether the bootstrap route of Finding 5
is live.

### Finding 10 — LOW — `clear_all_credentials` is called through the vtable with no per-pointer NULL check

**Confirmed by reading the code.**
`firmware/KilnFW/App/drivers/http/security_http_core.c:195`.

```c
security_err_t err = vt->clear_all_credentials();
```

`security_http_dispatch()` checks `caller_role` (line 99) and `!vt` (line 104),
but never checks the individual function pointer before calling it. This matches
the existing style for `set_web_password`, `set_lcd_pin` and `set_policy`, so it
is a consistent pattern rather than a new lapse — but `8714ee31` added a new
required slot to `security_backend_vtable_t`, and a partially-initialised vtable
is exactly how that slot ends up NULL.

All three in-tree initializers were checked and all three set
`.clear_all_credentials`: the real backend, the placeholder
(`security_backend_placeholder.c:92`), and the test fake. So the crash is not
reachable at this tip.

Failure scenario (latent): a future backend — a variant build, a bring-up stub,
a partial refactor — omits the new slot. The struct's remaining fields
zero-initialise, the `!vt` guard passes, and the first ADMIN who clicks "Clear
login credentials" takes the board down with a null-pointer dereference on the
httpd task rather than receiving the `SECURITY_ERR_NOT_IMPLEMENTED` that the
placeholder path was built to return.

---

## Part 3 — Checked and cleared

These were investigated as suspected defects and found to be sound on closer
reading. Kept per the review brief.

**C1 — Does the credential clear actually clear everything, or only some
records?** Cleared. `web_auth_store_clear_all_credentials()`
(`web_auth_store.c:714` onward) clears both roles in the web blob (administrator
gets `must_change = true`) and both roles in the LCD PIN blob, each with its own
read-back verification, returning `false` on any failure. The session tables are
handled separately and correctly: `security_http_core.c:208-209` invalidates
ADMIN then USER, and `security_backend_web_auth.c` force-locks the single LCD
session. Web sessions, LCD session, web passwords and LCD PINs are all covered.

**C2 — Does the placeholder backend still refuse everything it should?**
Cleared. `security_backend_placeholder.c`: every mutator returns
`SECURITY_ERR_NOT_IMPLEMENTED`, including the newly added
`placeholder_clear_all_credentials()` at line 79, which also logs loudly rather
than failing silently. `get_config` reports the safe all-off state.

**C3 — Are the new routes' tiers correct against what their handlers actually
reach?** Cleared. All five new rows are present and correctly tiered:
`/login` GET OPEN (`route_tier_table.h:117`), `/api/auth/login` POST OPEN (118),
`/settings/security` GET ADMIN (337), `/api/auth/config` GET ADMIN (338),
`/api/auth/security` POST ADMIN (339). The two OPEN rows must be OPEN — an
unauthenticated caller has to reach the login route to obtain a session at all.
`/api/auth/session` is OPEN (127) and was confirmed never to touch
`last_seen_ms`, so polling it cannot extend a session; `/api/auth/session/extend`
is USER (161), which is the correct floor for an action that does extend one.
`POST /api/auth/security` additionally re-checks ADMIN inside
`security_http_dispatch()` (`security_http_core.c:99`) independently of the
table — correct belt-and-suspenders on the one route that can destroy every
credential.

**C4 — Is the session token guessable?** Cleared. `web_auth_login_http.c:179`
draws 32 bytes from `hal_sysinfo_fill_random()`, which is
`esp_fill_random()` on target (`firmware/hwAbstraction/esp/sysinfo/hal_sysinfo_esp.c:202-207`)
— the hardware RNG, routed through the HAL boundary rather than called directly.
The raw token is hex-encoded for the cookie and only its SHA-256 is stored in
the slot, so a memory disclosure of the session table does not yield usable
cookies.

**C5 — Can credential material reach a log line, a response body, or a persisted
artifact?** Cleared, with one caveat recorded below.
- Logs: the login handler logs only the client IP and the numeric role
  (`web_auth_login_http.c:169`, `:200`). No password, username, token or hash.
  `security_backend_web_auth.c:279` onward logs at warn/error level with no
  credential material.
- Response bodies: `set_result()` (`security_http_core.c:34-48`) copies only
  static literals and never caller-controlled input — verified against all
  call sites in that switch. `GET /api/auth/config` emits booleans, timeouts and
  the escaped administrator username only; never a hash, salt or PIN.
- Stack hygiene: the plaintext password is wiped with
  `memset(password, 0, sizeof(password))` at `web_auth_login_http.c:157`,
  immediately after the verify call.
- Storage separation: `WEB_AUTH_NAMESPACE "kiln_auth"` is defined at
  `web_auth_store.c:19`, and a repo-wide grep confirms that string appears in
  exactly one production translation unit — `web_auth_store.c` itself. No config
  or backup code opens that namespace.

*Caveat, suspected not confirmed:* the "no config operation touches `kiln_auth`"
property appears to be enforced only by construction and asserted only by host
tests (`test_backup_import.c`, `test_kiln_cfg_store.c`,
`test_kiln_cfg_swap.c`). No production guard was found in `kiln_cfg_store.c`,
`backup_export.c` or `backup_import.c` that would refuse a namespace argument of
`kiln_auth`. The property holds at this tip; nothing mechanical prevents a future
config feature from regressing it.

**C6 — Redundant double session invalidation.** Cleared as harmless.
`security_http.c:243` invalidates sessions for `result.invalidated_role` after
`security_http_dispatch()` (line 240) has already invalidated them internally.
Invalidation is idempotent — `web_auth_table_destroy_role()` memsets matching
slots — so the second call is wasted work, not a defect.

**C7 — Does an empty password field bypass verification?** Cleared.
`http_form_find_field()` (`firmware/KilnFW/App/drivers/common/http_form.h`)
returns 0, not -1, for a present-but-empty `password=` field, so the empty
string reaches `web_auth_store_verify_password()`. That function runs the full
PBKDF2 over the empty string and compares against the stored hash, which fails.
No bypass. (The `-2` "too long to decode" return is also correctly treated as a
rejection at `web_auth_login_http.c:133`, refusing rather than silently
truncating a password.)

**C8 — Is the route tier table still the single source of truth?** Cleared as to
completeness; see Finding 7 as to uniqueness. `tools/check_route_tier_coverage.ps1`
scans every registered route under the drivers directory against the table, has
blindness floors that throw rather than pass vacuously if its patterns stop
matching (lines 140-147), and treats an unparsable method as an offender rather
than skipping it. `http_auth_effective_tier()`
(`http_auth_enforce.c:24-28`) fails closed to `ROUTE_TIER_ADMIN` on a lookup
miss.

---

## Part 4 — Vacuity sweep

The standing test applied: for each new capability, count production call sites
of the writer **outside its own translation unit and its tests**. A capability
whose writer has zero such call sites is not shipped, however well tested.

| Writer | Production call sites outside its TU and tests | Where |
| --- | --- | --- |
| `web_auth_table_create_session()` | 1 | `web_auth_login_http.c:188` |
| `web_auth_policy_check_transition()` | 1 | `security_backend_web_auth.c:182` |
| `web_auth_store_clear_all_credentials()` | 1 | `security_backend_web_auth.c:289` |
| `web_auth_store_clear_for_physical_reset()` | 1 (as a function-pointer assignment) | `auth_reset_gesture_wiring.c`, driven from `ui_page_home_actions.c:318-320` |
| `web_auth_table_destroy_all()` | 1 | `security_backend_web_auth.c:203` |
| `ui_lcd_lock_force_lock()` | 2 | `security_backend_web_auth.c:206`, `:273` |
| `http_session_hash_token()` | 1 | `web_auth_login_http.c:184` |
| `http_auth_extract_session_token()` | 2 | `http_auth_http.c:132`, `web_auth_session_status_http.c:50` |
| `web_auth_store_verify_password()` | 1 | `web_auth_login_http.c:151` |

**No writer in this feature has zero production call sites.** The specific
vacuity that made the earlier review's defect 1 severe — a session creator
reachable only from tests while the enable switch shipped — is not present
anywhere in the current surface.

One note on how the physical-reset row was counted: a plain grep for
`web_auth_store_clear_for_physical_reset(` finds no call expression outside
`web_auth_store.c` and the tests, because the production use is an assignment to
a function pointer rather than a call. Counting call expressions alone would
have wrongly scored this capability as vacuous. The seam and its driver were
read to confirm the path is live.

---

## Summary

| # | Severity | Finding | Status |
| --- | --- | --- | --- |
| 1 | HIGH | Session not bound to client address; header contract unimplemented | Confirmed — prior defect 5, still open |
| 2 | HIGH | Cookie parser aborts on first `kiln_sid` match; planted cookie locks out admin | Confirmed |
| 3 | MEDIUM | Cookie header at or above 128 bytes discarded whole | Confirmed |
| 4 | MEDIUM | Global, non-per-IP login lockout | Confirmed |
| 5 | MEDIUM | Credential clear opens a network-reachable bootstrap window | Confirmed (bootstrap handler's own guards suspected only) |
| 6 | MEDIUM | Build-identity redaction inert while auth is off | Confirmed |
| 7 | LOW | Duplicate `/status` row; firmware and checker read opposite rows | Confirmed |
| 8 | LOW | Unlooped login body read; split segment counts as a failed attempt | Confirmed |
| 9 | LOW | Timing oracle on unconfigured records | Confirmed |
| 10 | LOW | `clear_all_credentials` called with no per-pointer NULL check | Confirmed latent; not reachable at this tip |

Prior defects 1, 2, 3 and 4 (both halves) are genuinely closed with the evidence
cited in Part 1. Prior defect 5 is still open and is Finding 1.
