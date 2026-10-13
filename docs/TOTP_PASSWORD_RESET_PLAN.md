# TOTP password-reset plan

**2026-09-24 owner change, superseding the email design below in full:** the
"forgot my password" flow must NOT use email. Owner direction: use
authenticator-app TOTP (RFC 6238) instead, "support all the common ones" —
Google Authenticator, Microsoft Authenticator, Authy, 1Password, Bitwarden,
Aegis, FreeOTP, Apple Passwords. This doc replaces
`docs/EMAIL_PASSWORD_RESET_PLAN.md` (renamed via `git mv`, history follows)
and reuses everything from that plan that still applies: the "Forgot
password?" link in the lazy login modal (worktree `lazylogin`), the open-tier
reset routes rate-limited through `login_ip_scope.c`'s existing ladder, the
always-202/generic-failure response shape, the settings page as the
credential-enrollment UI, and the WT-A/B/C/D work-tranche structure. WT-A..D
are implemented; only live-board verification remains. This doc is pending
work only.

**The existing LCD password-reset touch sequence is untouched by this plan
and stays exactly as it is.** That is `firmware/KilnFW/App/drivers/net/
auth_reset_gesture.c`/`.h` (state machine) plus `auth_reset_gesture_wiring.c`
(the LVGL/UI wiring) — docs/WEB_AUTH_PLAN.md item 10: with E-stop asserted
and no firing/heat active, tap each of the four LCD corners once, in order
(top-left, top-right, bottom-left, bottom-right) within a 10 s window to arm,
then an explicit confirm dialog within 30 s clears the administrator
credential via `web_auth_store_clear_for_physical_reset()`
(`firmware/KilnFW/App/drivers/persist/web_auth_store.h`). This plan's TOTP
reset is a **second, independent** reset path — it exists because the LCD
gesture requires physical access to the board, and the web forgot-password
flow is for someone who does not have that. Nothing in this plan changes
`auth_reset_gesture.*`, its corner order, its timing windows, or its wiring.
Section 6 below does add one narrow, additive touch to that module: on a
successful LCD gesture confirm, also disenroll TOTP (see section 6) — that is
a new call at the confirm site, not a change to the gesture state machine
itself.

## 1. Compatibility: fixed RFC 6238 defaults, not configurable

**HMAC-SHA1, 6 digits, 30 s period, 160-bit (20-byte) base32-encoded secret.**
Standard enrollment URI:

```
otpauth://totp/kilnCtl:<username>?secret=<base32>&issuer=kilnCtl&algorithm=SHA1&digits=6&period=30
```

These four parameters are **fixed, not configurable**, because several of
the "common ones" hardcode them and ignore (or mishandle) anything else in
the URI — building a settings UI for algorithm/digits/period would offer a
choice that silently breaks enrollment on at least one of the eight target
apps. Verified against each app's own documented behavior (WebSearch,
2026-09-24):

| App | RFC 6238 default (SHA1/6-digit/30s) | Non-default params (SHA256, 8-digit, other periods) |
|---|---|---|
| Google Authenticator | Yes | Hardcodes SHA-1, 30 s, and the digit count from its own UI regardless of URI hints — `algorithm=SHA256`/`period=60` in the otpauth URI is silently ignored, not honored. |
| Microsoft Authenticator | Yes | Same class of restriction — built and documented around the RFC 6238 defaults; do not rely on non-default fields being read. |
| Authy | Yes | Defaults to SHA-1; SHA-256 "breaks compatibility" per its own ecosystem's documented behavior — treat non-default params as unsupported. |
| 1Password | Yes | Runs the standard RFC 6238 algorithm at the standard parameters; broader parameter support is not something to depend on for interop. |
| Bitwarden | Yes | Its own integrated authenticator generates six-digit SHA-1 codes on a 30 s rotation by default; it can *parse* an otpauth URI carrying other parameters, but that is Bitwarden reading someone else's non-default issuer, not evidence this board should emit one. |
| Aegis | Yes | An open-source app built for general otpauth import; accepts the standard URI shape without special-casing. |
| FreeOTP | Yes | Same — a straightforward RFC 6238 client; the standard URI enrolls cleanly. |
| Apple Passwords | Yes | Handles TOTP codes from a standard otpauth URI/QR as part of its passkey/password manager; no evidence of, or dependency needed on, non-default parameter support. |

Conclusion: emit exactly the fixed-parameter URI above. Every one of the
eight apps enrolls correctly from it; at least three (Google Authenticator,
Microsoft Authenticator, Authy) are documented to ignore or mishandle a
non-default algorithm/digit-count/period, so there is no configuration this
board could offer that would be "more compatible" — only ones that would be
less.

## 2. Enrollment (settings page, ADMIN session)

Reuses `settings_page.html`'s role as the enrollment surface, same
placement idea as the email plan's SMTP block.

- **Secret generation:** 160 bits (20 bytes) from `esp_random()` — ESP32-S3's
  hardware TRNG, already used elsewhere in this tree for OTA/session tokens;
  no new entropy source needed. Base32-encode (RFC 4648, no padding) for the
  manual-entry key and for the otpauth URI's `secret=` field.
- **QR code:** **superseded by section 6b (WT-B, 2026-09-24): the QR is
  rendered client-side** by a self-written byte-mode encoder embedded in
  `security_page.html` (no CDN, no third-party library; canonical copy
  `firmware/KilnFW/App/test/qrcode_encoder_src.js`, drift-checked by
  `test_qrcode_encoder.js`). Firmware only returns the `otpauth_uri`
  string; WT-A needs no C QR encoder or SVG wrapper. Original proposal,
  kept as history: render server-side as inline SVG (no third-party image
  library, no new binary dependency, no client-side JS library to vet
  against `docs/`'s page-bundle/`lint_pages.js` rules). A QR encoder is
  ~200-400 lines of pure C with no external dependencies (well-known
  public-domain QR encoders exist at this size, e.g. Nayuki's
  `qrcodegen` — single .c/.h pair, no floating point, no dynamic
  allocation required for version <=10 which fully covers a ~90-character
  otpauth URI); emit the QR matrix from firmware into the HTTP response as
  an inline `<svg>` of `<rect>` cells, same discipline as this codebase's
  other server-rendered pages (no CDN script, no bundle-size growth against
  `settings_page.html`'s existing size, since a QR-matrix SVG is generated
  data, not a library). Recommended over a client-side JS QR library
  specifically to avoid adding a new script to the page-bundle allowlist
  `lint_pages.js` enforces. Show the manual base32 key beside the QR
  unconditionally — every one of the eight apps supports manual key entry,
  and it is the only path when the camera can't reach the LCD/board or the
  phone is used to view the settings page itself.
- **Commit gate:** the secret is generated and shown but **not** written to
  NVS until the admin enters one valid 6-digit code from their app against
  it (standard "verify before commit" TOTP enrollment pattern — prevents a
  typo'd QR scan or a wrong app locking enrollment in on an unusable
  secret). NVS key `totp_secret` (11 chars, well under the 15-char limit —
  see `nvs_key_check.h` and the `zone_normals_cfg`/16-char history this repo
  already tracks).
- **Disable path:** requires a current valid code (not just an ADMIN
  session) before clearing `totp_secret` — same "prove you still hold the
  factor before removing it" principle as changing a password normally
  requires the old one.
- **Backup/restore:** **exclude** `totp_secret`, same treatment as the Wi-Fi
  password (the one documented irreducible exclusion) and as the email
  plan's `smtp_password`/`admin_email`. A restored secret without the
  matching phone app enrolled would be a silent lockout waiting to happen,
  and unlike a password there is no "the operator remembers it" fallback —
  the corresponding QR/key was likely never written down. Re-enrollment
  after a restore is the safe default.

## 3. Verification (ESP-side)

- **HMAC-SHA1** — implemented in-tree in `drivers/net/totp.c` (hand-rolled
  SHA-1/HMAC, RFC 3174/2104). The target does have SHA-1 available
  (`CONFIG_MBEDTLS_SHA1_C=y`, reachable through PSA), but the classic
  `mbedtls_md_hmac()` API is not the path used here and the host-test PSA
  stub is non-cryptographic, so the hand-rolled code is kept so that the
  exact target code is what the host tests validate against RFC 2202 and
  hashlib vectors.
- **Window:** accept the current 30 s step and the one immediately before
  and after it (±1 step, i.e. 3 candidate counters), the standard
  RFC 6238 clock-skew allowance.
- **Replay protection:** store the last-accepted counter value both in RAM
  (immediate re-use rejection) and in NVS (survives a reboot between
  attempts) — a counter value already accepted is rejected even if it falls
  inside the ±1 window on a later call. Persisting this is a small,
  15-char-safe key (e.g. `totp_last_ctr`, 13 chars).
- **Constant-time compare:** compare the computed and submitted digit
  strings with a fixed-time comparison (same discipline this codebase
  already applies to password/session-token comparisons — do not
  `strcmp`/`memcmp` short-circuit on a secret-derived value).
- **Time source requirement:** TOTP is meaningless without synced wall
  clock. Require `esp_sntp`/SNTP sync status to read synced before
  accepting *or verifying* any code; if unsynced, both enrollment
  verification and the reset flow refuse with a clear, specific message
  ("board clock not synced yet") rather than a generic failure. The
  enrollment page additionally shows the board's current UTC time
  next to the QR/key so the admin can visually confirm it matches their
  phone's clock before relying on it — skew is the single most common
  real-world TOTP failure mode and is worth surfacing, not hiding.

## 4. Reset flow: two routes, not one

Recommend **two routes**, matching the email plan's shape
(`/api/auth/forgot` then `/api/auth/reset`), rather than collapsing to one:
the code-verification step and the password-set step have different failure
modes worth distinguishing operationally (code wrong vs. new password
policy rejected), and collapsing them would mean re-deriving/re-checking the
code inside a single handler that also has to validate the new password —
no meaningful reduction in complexity, and the URI cap has headroom for two
(see below), so "fewer routes" does not need to mean "fewer than the email
plan's own count."

| Route | Method | Tier | Notes |
|---|---|---|---|
| `POST /api/auth/forgot` | POST | OPEN | body: `{"username","code"}`. Verifies the TOTP code (section 3) against that user's enrolled secret. On success, issues a short-lived (2-minute), single-use reset token (random, `esp_random()`-derived, held in RAM only — no NVS write for a token this short-lived). **Always returns 202** whether or not the user/secret exists or the code matched, to avoid an oracle for "is TOTP enrolled" — same anti-oracle property the email plan already established for "is email configured." |
| `POST /api/auth/reset` | POST | OPEN | body: `{"username","reset_token","new_password"}`. Consumes the token (single-use, expires at 2 minutes), sets the password via the existing `security_backend_web_auth.c` `set_web_password` path, invalidates all sessions. Generic 400 `{"ok":false}` on a bad/expired/reused token. A weak password answers 400 `{"ok":false,"reason":"weak_password"}` and leaves the token unspent; strength is checked before and independently of the token, so that reply never reveals token validity. |

Net **2 new routes** against the current **160 of 170** (10 spare,
reconfirmed 2026-09-24 with `check_uri_handler_cap.ps1`'s own method:
`URI handler cap check: 160 route(s) registered across 30 file(s) ...,
cap = 170`, `10 spare slot(s)`). Two routes fit inside the existing 10-slot
headroom **without** a cap bump — unlike the email plan's 5-route/180 cap
proposal. Land at 162/170, 8 spare — re-verify with a fresh
`check_uri_handler_cap.ps1` run in the same commit that adds the routes,
since the count drifts as other work lands first.

**Rate limiting — reuse, do not reinvent:** both OPEN routes go through the
same `login_ip_scope.c` LOCAL/REMOTE classification and pooled REMOTE-scope
backoff slot the login ladder already uses, same as the email plan
specified. **Brute-force budget, computed for TOTP specifically (this is a
different arithmetic than the email plan's 6-digit/10-minute-window
numbers):** a 6-digit code has 1,000,000 values; accepting a ±1-step window
means 3 valid codes are live at any instant, so a single blind guess has
odds 3 in 1,000,000 ≈ **1 in 333,333**. The accepted owner ceiling
(`project_login_lockout_saturation_accepted`, ~1 request per 19 s to any one
off-subnet address) limits an attacker to roughly 3 guesses per 90-second
TOTP validity window (30 s step × 3-step window) — far too slow to matter
against 1-in-333,333 odds; reusing the existing ladder is sufficient and no
new per-attempt throttle is needed for the REMOTE pool. **Additionally**, a
hard **per-boot attempt cap is still worth adding** as defense in depth
against a LOCAL-scope caller (the existing LOCAL ladder is a 16-slot
lockout keyed per source address, which is looser than the REMOTE pool) —
recommend capping `/api/auth/forgot` at a small fixed number of failed
attempts per boot (e.g. 20) *board-wide*, separate from and in addition to
the per-address lockout, specifically because 1-in-333,333 is low enough
odds that even the LOCAL ladder's 16-per-address slots, multiplied across
many source addresses on a LAN, would eventually matter over a long enough
uptime — a board-wide counter closes that regardless of how many addresses
an attacker rotates through. Reset the counter only on reboot (RAM only, no
NVS write, same treatment as the email plan's board-wide send cap).

## 5. TOTP as optional login 2FA — one paragraph, not designed here

Whether to also offer the same enrolled TOTP secret as an optional
second factor at ordinary login (not just for password reset) is a
**separate decision the coordinator should put to the owner as an
option**, not something this plan designs. It is attractive because the
enrollment/verification machinery in sections 2-3 would be fully reused
with no new secret or NVS key; the cost is real, though — every login
becomes two round trips when 2FA is on, the lazy-login modal (built
concurrently in worktree `lazylogin`) would need a second step, and a
lost/unenrolled-phone recovery story becomes urgent for *login* rather
than merely for password reset (today, forgetting the password is rare;
requiring a phone for every login is a much larger behavioral and
availability commitment for a bench appliance with one administrator).
Recommend treating it as a follow-on plan, not folded into this one, if
the owner wants it at all.

## 6. Recovery when the phone is lost

The **LCD touch-sequence reset remains the fallback path**, unchanged (see
the notice at the top of this document) — a lost or wiped phone with no
enrolled TOTP app is exactly the scenario the physical gesture exists for,
since it requires no network, no email, and no second factor at all beyond
physical presence and E-stop.

**Recommend: yes, disenrolling TOTP is part of the LCD reset.** The LCD
gesture already clears the administrator web credential
(`web_auth_store_clear_for_physical_reset()`); if it does not also clear
`totp_secret`, a factory-style credential reset leaves an orphaned TOTP
secret enrolled against a *new* password the operator is about to set, and
the very next login (or the very next attempt to use the web forgot-password
flow, if 2FA-at-login is ever added per section 5) is gated on a factor tied
to a phone that may no longer exist or may not have been the reason for the
reset in the first place. That is a lockout, not a recovery — the class of
bug this repo already tracks in its "reset one side of a pair" bug class
(CLAUDE.md): the web credential and the TOTP secret are two pieces of state
joined by an implicit contract ("both describe how this administrator
proves who they are"), and clearing one without the other breaks that
contract silently. Implementation is additive only: at the point
`auth_reset_gesture_confirm()`'s `clear_credentials_fn` succeeds, also clear
`totp_secret` (and `totp_last_ctr`) via a second, equally read-back-verified
NVS clear — no change to `auth_reset_gesture.c`'s state machine, corner
order, or timing constants.

**Note (second review pass, 2026-09-24): an UNREADABLE stored secret is a
dead end short of the LCD gesture.** `totp_config_enrolled()`'s tri-state
(section 2/7, `test_totp_config_persist.c`) treats a corrupt-CRC secret as
UNREADABLE, distinct from ABSENT — and `totp_enroll_begin`/`totp_enroll_confirm`
refuse unless the state is exactly ABSENT (6b above), while `totp_disable`
verifies a *code against the stored secret* before it will clear anything, so
it cannot clear a secret it cannot read either. Neither route can dig the
board out of an UNREADABLE state, so the only recovery is the LCD
four-corner gesture — which, per this section, also wipes the web
credentials, not just the TOTP secret. This is a known, accepted gap, not a
firmware defect to fix here: no change to `totp_disable`/`clear_credentials`
behavior is being made for it.

## 6a. WT-A/WT-C wire contract (decided by WT-C, 2026-09-24)

WT-C (`tools/PcTools/src/kilnctrl/totp_http_client.py`) landed before WT-A
(the firmware routes themselves), so it had to choose the
fields section 4 above left unspecified rather than guess at call sites.
WT-A implements exactly this shape; the fuller reasoning lives in
`totp_http_client.py`'s own module docstring:

- `POST /api/auth/forgot` body: form-urlencoded `{"username","code"}`.
  Response is **always** HTTP 202 with JSON `{"reset_token": "<opaque
  string>"}` — a token-shaped value is present whether or not the code
  actually matched (a wrong/unenrolled code gets a token that simply will
  not later verify), so the response shape itself carries no oracle.
- `POST /api/auth/reset` body: form-urlencoded
  `{"username","reset_token","new_password"}`. Success: HTTP 200, JSON
  `{"ok": true}`. Any failure (expired/wrong/reused token, password policy
  rejection): HTTP 400, JSON `{"ok": false}` — deliberately generic, never
  distinguishing which of the two failed.
- Both OPEN routes: a rate-limit refusal from the shared `login_ip_scope.c`
  ladder is HTTP 429 with the login ladder's existing plain-text body,
  reused as-is rather than remapped. An unsynced board clock (section 3's
  "board clock not synced yet" refusal) is HTTP 503 on either route — the
  one deliberate exception to `/forgot`'s always-202, and not an oracle,
  since clock sync is board-wide rather than per-user. No non-2xx body has
  to be JSON; the PC client branches on status code alone.
- `GET /api/auth/totp_status` (ROUTE_TIER_ADMIN, unlike the two OPEN routes
  above): JSON `{"enrolled": bool}`, plus WT-C's PC-side tool additionally
  reads and reports `board_time_utc`/`sntp_synced` fields **if present** —
  WT-A does not have to add them for WT-C's tool to keep working, but
  adding them lets `totp_enroll_status()` warn on an unsynced clock per
  section 3's requirement; their absence is tolerated, not required.

## 6b. WT-B's chosen enrollment/disable field names (2026-09-24)

Same situation as 6a: WT-B (`security_page.html`'s new "Two-factor reset"
card) landed before WT-A, so it had to pick the enrollment/disable field
names section 2/7 left open. **WT-A implements exactly this shape.**
Chosen design: reuse the *existing* `POST /api/auth/security` `cmd=`
dispatch (already used for `set_web_password`/`set_lcd_pin`/`set_policy`/
`clear_credentials`) rather than three new routes — this repo's
`check_uri_handler_cap.ps1` was, as of the same day, down to single-digit
spare `httpd_uri_t` slots, so spending zero new routes on a feature that can
ride an existing one was a deliberate constraint, not just a style choice.

- `cmd=totp_enroll_begin` (no other fields): generates a new pending
  secret (NOT committed to NVS — see below) and returns JSON
  `{"ok": true, "secret_base32", "otpauth_uri", "board_time_utc",
  "sntp_synced"}`. A second call before confirming replaces the
  still-pending secret (no accumulation of abandoned attempts). Both
  `totp_enroll_begin` and `totp_enroll_confirm` answer `{"ok": false}`
  unless NO secret is stored (an enrolled or unreadable one refuses) --
  replacing an enrolled secret must go through `totp_disable` first, or
  an admin session alone could swap the owner's factor for its own. The page
  renders `otpauth_uri` as a QR code itself (client-side encoder, see
  section 2's superseded QR bullet) -- firmware returns only the string.
- `cmd=totp_enroll_confirm&code=NNNNNN`: validates `code` against the
  pending secret from the most recent `totp_enroll_begin` and, only on a
  match, commits it as the enrolled secret (NVS `totp_secret`/
  `totp_last_ctr`, per section 7's WT-A file list). Returns
  `{"ok": bool}`; no other field — the page treats any `ok:false` as
  "incorrect code, try again" without distinguishing "no pending secret" or
  "wrong code" (same generic-failure principle as section 4's reset route).
- `cmd=totp_disable&code=NNNNNN`: requires a currently-valid TOTP code (not
  merely the admin session already required by this route's
  `ROUTE_TIER_ADMIN` tier) — a hijacked web session alone must not be able
  to silently remove a locked-out owner's only non-admin-session recovery
  path. Returns `{"ok": bool}`.

Wire rules for all three (review of WT-B, 2026-09-24):

- **Transport status is always HTTP 200**, the existing convention of this
  route (`security_http.c` reports every dispatch outcome inside the JSON
  body; the page checks `ok`, never the status). Only the httpd layer's own
  401/403 (session/role) differs, which the page's `app.js` wrapper already
  handles.
- A bad code, no pending enrollment, and any other refusal all read
  `{"ok": false}` (an `error` string may follow, as for the route's other
  commands; the page does not show it), so the page says only "incorrect
  code" / "could not start enrollment" -- the same no-distinction principle
  as sections 4/6a (this surface is ADMIN-gated, so the concern is a
  stolen-session attacker brute-forcing disable, not the anonymous-caller
  oracle of `/forgot`).
- **The one exception is an unsynced board clock**, which section 3
  requires be reported specifically: `{"ok": false, "clock_unsynced":
  true}` on any of the three (optionally with `board_time_utc`). It is
  board-wide, not per-user, so it is no oracle -- same reasoning as the 503
  on the OPEN routes in 6a. The page shows "board clock is not synced yet"
  for it.
- `totp_enroll_confirm`/`totp_disable` must go through
  `totp_config_verify_and_consume()` like every other code check (section
  7), with the SNTP check first.
- **Buffer sizes (DONE):** the begin response uses its own local `body[320]`
  (plus `uri[192]`/`base32[64]` locals feeding it), never the shared
  `resp[SECURITY_HTTP_MESSAGE_MAX + 32]` (192 B) that the other cmd=
  outcomes use -- still a fixed stack buffer, not `httpd_resp_send_chunk()`,
  but comfortably under the 8 KB httpd stack (see CLAUDE.md's "httpd stack
  blob class" note) and sized with margin above the worst case (32-char
  base32 + up to a 33-char username baked into the otpauth URI + JSON
  overhead, ~280 B against a 320 B buffer); a `snprintf()` truncation check
  fails loud (500) rather than silently truncating if that margin is ever
  exceeded. `cmd_val[24]` already fits the longest new command
  (`totp_enroll_confirm`, 19 chars).

## 7. Pending: live-board verification (only open item)

WT-A (firmware routes + NVS), WT-B (web UI), WT-C (PcTools MCP wrappers), WT-D
(host tests, RFC 6238 Appendix B) and the 2026-09-25 owner decisions (credential
wipe disenrolls TOTP via `totp_wipe_disenroll()`; enrollment refused while web
auth is off) are all built, host-tested and negative-tested. Host coverage:
`test_totp.c`, `test_totp_config_persist.c`, `test_totp_http_core.c`. The
`security_backend_web_auth.c` vtable wiring is ESP-only (target build + inspection).

Steps, on the bench board with web auth on and an authenticator app:
1. Settings page: enroll (`totp_enroll_begin`, scan QR, `totp_enroll_confirm`);
   `totp_enroll_status` must report `enrolled: true` and no secret.
2. Set `KILNCTL_TOTP_CODE` from the app and `KILNCTL_WEB_PASSWORD_NEW`; call
   `totp_reset_password(confirm=True)`; it must report success and the
   follow-up login with the new password must work.
3. Replay the same code: must be refused (single use / replay counter).
4. Credential wipe (or LCD four-corner gesture): `totp_enroll_status` must go
   back to `enrolled: false`.
5. With web auth off, enrollment must be refused.
