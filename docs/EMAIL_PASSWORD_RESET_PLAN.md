# Email password-reset plan

Owner request 2026-09-24: a "forgot my password" flow that emails a reset
code to an administrator address, with SMTP settings configurable on the
existing user configuration page (`settings_page.html`,
`firmware/KilnFW/App/drivers/http/settings_page.html` — the web twin of
`ui_page_config.c`, per that file's own header comment). Not implemented
yet. This doc is pending work only — recommended design, data model, HTTP
surface, UI, security review, and a 3-4-worktree breakdown.

## 1. Feasibility and recommended design

ESP-IDF **v6.0.2** (`firmware/KilnFW/sdkconfig.defaults:1`,
`dependencies.lock`), target `esp32s3` (`sdkconfig:650`). `esp-tls`/mbedtls
are already in the build (used by the existing HTTPS/OTA paths), so SMTP
submission over TLS is a library-availability non-issue — this is a new
client role, not a new dependency.

**Recommended: SMTPS on port 465 (implicit TLS), AUTH LOGIN, one-shot
connect-send-quit, no persistent connection.** Rationale:
- STARTTLS (587) needs a plaintext-then-upgrade state machine; implicit TLS
  (465) is a single `esp_tls_conn_new_sync()` call plus a five-line SMTP
  dialogue (`EHLO`, `AUTH LOGIN`, `MAIL FROM`, `RCPT TO`, `DATA`, `QUIT`).
  Less state, less to get wrong on a board with no interactive debugging at
  the bench.
- AUTH LOGIN (base64 user/pass) over AUTH PLAIN: functionally identical,
  marginally wider provider support; either is fine, LOGIN is what the
  ESP-IDF `smtp_client`-style examples use as reference for the wire
  framing (there is no `smtp_client` component in this tree today — this is
  new code modeled on that example's dialogue, not a vendored component).
- Certificate handling: use **`esp_crt_bundle`** (already linked for the
  OTA/HTTPS client paths — check `firmware/KilnFW/App` for existing
  `esp_crt_bundle_attach` call sites before adding a second one), not a
  pinned CA. The admin's SMTP host is user-configured and unknown at build
  time; a bundle covers Gmail, Outlook, and self-hosted-with-public-CA
  cases without per-deployment cert management. A pinned CA would need a
  new NVS blob and a UI to upload it — not worth it for a bench-scale
  feature.
- **Gmail/most providers require an app password**, not the account
  password — document this on the settings page itself (a one-line hint
  next to the password field) so it isn't a support surprise. Do not build
  OAuth2; that's a different, much larger auth flow for a feature this
  narrow.

**Cost, checked against this codebase's own budget notes:**
- A TLS session is ~40 kB heap (stated in the task; consistent with the
  `s_routes` DRAM note and PSRAM-stack gate in `docs/MCP_SERVERS.md`/
  CLAUDE.md — a task doing TLS must not be PSRAM-stacked and must not write
  NVS from inside a PSRAM-stacked context). **The email-send task must run
  from a normal internal-DRAM-stacked task, one-shot (create, connect, send,
  tear down, delete), never a long-lived task holding a TLS session.** This
  matches the existing pattern for OTA HTTP client calls (short-lived task,
  not the httpd worker itself — see the "httpd stack blob class" note:
  never build the SMTP buffer on the 8 KB httpd worker stack either).
  Confirm actual headroom with `get_heap_status()` before and during WT-A's
  first bench test — do not assume 40 kB is free without measuring, per
  `firmware/KilnFW/docs/PROJECT_STATUS.md`'s heap headroom section.
- Do not add a new persistent task or stack-margin registration unless the
  send path becomes a queued background worker (it should not — see
  route design in section 3, which sends synchronously off the OPEN route's
  own request-handling context, one-shot).

## 2. Data model

NVS namespace: reuse an existing settings namespace or add
`email_cfg` (7 chars, well under the 15-char limit — see
`nvs_key_check.h` and the `relay_names_cfg`/`zone_normals_cfg` history:
`zone_normals_cfg` was 16 chars and silently never persisted; keep every
new key here to **12 chars or fewer** as a margin, not 15, given that
history). Fields, one NVS entry (blob or single struct) is cleaner than one
key per field so the same `nvs_key_check.h` compile-time table doesn't grow
by six entries:

| Field | Type | Notes |
|---|---|---|
| `smtp_host` | string, ≤64 | |
| `smtp_port` | u16 | default 465 |
| `security_mode` | enum | `IMPLICIT_TLS` (default) / `STARTTLS` — build STARTTLS only if WT-A finds implicit-TLS-only insufficient for a real provider during bench test |
| `smtp_user` | string, ≤64 | |
| `smtp_password` | string, ≤64, **write-only** | never round-tripped in a GET; GET returns `password_set: bool` only, same pattern as the web admin credential (`security_backend_web_auth.c`) |
| `from_addr` | string, ≤64 | |
| `admin_email` | string, ≤64 | reset codes go here only |

Store as one versioned struct blob (`email_cfg` key, binary), following the
`zones_config_json.h` versioning precedent (a `EMAIL_CFG_VERSION` constant)
so a future field addition doesn't need a new key.

**Backup/restore:** exclude `smtp_password` and `admin_email` from the
backup body, same treatment as the Wi-Fi password (the one documented
irreducible exclusion — grep `docs/` for the backup round-trip doc before
touching `BACKUP_BODY_MAX` or the backup/restore file list; per memory,
the round-trip project already treats one secret as irreducible and this
adds a second class of it). Non-secret fields (`smtp_host`, `smtp_port`,
`security_mode`, `smtp_user`, `from_addr`) may be included since they are
not credentials; `admin_email` is arguably not a secret but is PII — default
to excluding it too and let WT-B's review confirm before flipping that.

**`cfg` LittleFS dual-write:** only if WT-A confirms zones/profiles/other
preferences generically dual-write through one shared helper (e.g. a
`persist_dual_write()`-style call site) — grep `zones_config_store.c` and
sibling persist files for the dual-write call before adding a new one by
hand. If the mechanism is per-store bespoke code, do not add a bespoke
`cfg` writer for this feature in the first pass; NVS alone is sufficient
and avoids growing the `cfg` partition's file count for a secondary
credential store.

## 3. HTTP surface

Current count: **160 of 170** `httpd_uri_t` routes
(`config.max_uri_handlers = 170`,
`firmware/KilnFW/App/drivers/http/wifi_provision_http.c:1141`). This plan
adds **4 routes** (`GET/POST /api/email_settings` count as 2 handlers,
`POST /api/email_settings/test`, `POST /api/auth/forgot`,
`POST /api/auth/reset` — 5 total), which would land at 165/170. **Bump
`config.max_uri_handlers` to 180 in the same change** (10-slot headroom
convention already used at the 160→170 bump) and confirm
`check_uri_handler_cap.ps1` passes with the new count, not the old one.

| Route | Method | Tier | Notes |
|---|---|---|---|
| `/api/email_settings` | GET | ADMIN | returns config minus password, plus `password_set: bool` |
| `/api/email_settings` | POST | ADMIN | writes config; empty/absent password field means "keep existing" |
| `/api/email_settings/test` | POST | ADMIN | sends a test mail to `admin_email` using saved (or request-body override) settings; reports the SMTP-level failure reason to the admin caller only (see section 5) |
| `/api/auth/forgot` | POST | OPEN | generates a 6-digit code, 10-minute expiry, single-use; emails it if `admin_email` is configured; **always returns 202** regardless of whether an email was actually sent, to avoid an oracle for "is email configured" |
| `/api/auth/reset` | POST | OPEN | code + new password; on match, sets the web password via the existing `security_backend_web_auth.c` `set_web_password` path and invalidates all sessions (same session-wipe behavior a normal password change already triggers) |

Route tiers follow `route_tier_table.h`'s existing convention: `/api/auth/*`
OPEN routes already exist there (`/api/auth/login`, `/api/auth/session`) —
add the two new ones beside them, not in a new tier.

**Rate limiting — reuse, do not reinvent:** both OPEN routes go through the
same `login_ip_scope.c` LOCAL/REMOTE classification and the same pooled
REMOTE-scope backoff slot (`s_remote_login_slot`) the login ladder already
uses (`docs/WEB_AUTH_PLAN.md` lines 391-409) — do not build a second
per-IP table. Additionally cap total outbound email sends board-wide (e.g.
3/hour, a simple rolling counter in RAM, reset on reboot — no NVS write
needed) so a REMOTE-pool attacker who churns through the ladder slowly
still can't run the board's mail relay hot or spam the admin inbox. The
`project_login_lockout_saturation_accepted` owner decision (~1 request per
19 s to any new off-subnet address) is **accepted, not to be re-proposed**:
these two new routes inherit that same ceiling by reusing the pool, not a
looser one.

## 4. Web UI

Email settings block goes on **`settings_page.html`** (confirmed as "the
user configuration page" — nav hub twin of `ui_page_config.c`; the file
already has AP-password-style input styling to copy, see its
2026-09-18 AP-password field comment). Fields: host, port, security mode
(dropdown), username, password (masked, blank-keeps-existing, with the
Gmail-app-password hint text), from address, admin email, a **Test** button
(POSTs `/api/email_settings/test`, shows the raw failure text inline — this
is the ADMIN-authenticated path so detail leakage is fine here, unlike the
OPEN `/api/auth/forgot` path).

**Login modal — "Forgot password?" link:** the login pop-up is being built
concurrently in worktree `lazylogin`; this plan references it by name only
and does not touch its markup. WT-B should coordinate with that worktree's
owner (or land after it merges) to add a "Forgot password?" link inside
that modal, opening a two-step inline form: (1) confirm/no input needed —
just a "send code" trigger calling `/api/auth/forgot` with no target field
in the request body (admin email is server-side only, never entered by the
caller — this is the anti-oracle property from section 3); (2) code +
new-password + confirm-password fields calling `/api/auth/reset`. Both
steps show a generic "if configured, a code was sent" / "check the code and
try again" message — never "no admin email configured" or "wrong code" as
distinguishable strings.

## 5. Security review points

- **Code entropy / brute-force budget:** 6 digits = 1e6 space, 10-minute
  expiry, single-use, and the reused REMOTE-pool ladder caps guesses to
  roughly one per ~19s per the accepted owner ceiling — well under 1e6
  attempts possible in a 10-minute window from any single pooled attacker.
  LOCAL-scope callers get the existing per-address 16-slot ladder, same as
  login.
- **No code in logs:** the device log ring is append-only and already
  proven to drop lines silently (`project_device_log_ring_drops_lines`) —
  never log the generated code, the SMTP password, or the full email body
  at any level, including debug. Log only "reset code sent" / "reset code
  requested" as a fact with no payload, same discipline as the login path's
  429 (which the memory note says logs nothing either — match that, don't
  regress it by adding a payload-bearing log line here).
- **No credential in any GET:** `/api/email_settings` GET must never echo
  `smtp_password`; enforce with a host test in WT-D asserting the response
  JSON has no such key ever, not just under happy-path testing.
- **`factory_reset(scope=wifi)` interplay:** email settings are not
  Wi-Fi-scoped; confirm in WT-A that a `scope=wifi` factory reset does not
  touch the `email_cfg` NVS namespace (it shouldn't, since it's a separate
  namespace/key — verify by reading `factory_reset`'s scope-to-namespace
  map rather than assuming).
- **No network / TLS failure surfaced safely:** `/api/auth/forgot` always
  returns 202 even if the SMTP send throws (DNS failure, TLS handshake
  failure, auth rejected by provider) — the OPEN-tier caller never learns
  which. The ADMIN-tier `/api/email_settings/test` route is the only place
  the real error string surfaces, and only to an already-authenticated
  admin.
- **PC-side tooling:** add `email_settings_get` (read-only, ADMIN session,
  mirrors the `password_set: bool` shape) and
  `email_settings_set(confirm=True)` to `tools/PcTools/src/kilnctrl/`
  following the existing `http_auth.urlopen()` ADMIN-session seam
  (`tools/PcTools/src/kilnctrl/http_auth.py`) other admin-tier tools already
  use, then register both through the MCP facade (`kiln_find`/`kiln_call`)
  the way every other admin write tool in this repo's CLAUDE.md tool-history
  section does (fetch state first, refuse without `confirm=True`, report
  presence as `[bool]`, never echo the value).

## 6. Work breakdown

**WT-A — firmware SMTP client + NVS settings + routes.**
Files: new `firmware/KilnFW/App/drivers/net/smtp_client.c/.h` (one-shot
TLS SMTP send), new `firmware/KilnFW/App/drivers/persist/email_config*`
(NVS read/write, versioned struct, write-only password semantics), edits to
`firmware/KilnFW/App/drivers/http/route_tier_table.h` (5 new entries),
`wifi_provision_http.c` (`max_uri_handlers` 170→180), a new
`email_settings_http.c`/`auth_forgot_reset_http.c` for the 4-5 handlers,
edit to `security_backend_web_auth.c` if `set_web_password` needs a
non-session-authenticated entry point for the reset flow. Reuse
`login_ip_scope.c` for rate limiting — do not add a parallel table.
Checks: `-Only "check_uri_handler_cap|check_00_kilnfw_target_build|check_nvs_key"`
plus a full `run_all_checks.ps1` before commit. Acceptance: cap check
passes at the new count, target build green, a bench test (once WT-A is on
real hardware, gated by the usual board-touching rules) sends a real email
to a real inbox and the code round-trips through `/api/auth/reset`.

**WT-B — web UI.**
Files: `settings_page.html` (email settings block + Test button), the
`lazylogin`-worktree login modal (coordinate merge order — do not fork that
modal's markup independently). Checks:
`-Only "check_ui_responsive_sweep|check_lint_pages"`. Acceptance: fields
render at 320-390px width without scrolling (per the LCD/web
no-scroll convention), Test button surfaces the real error text, forgot-
password flow shows only the generic messages from section 4.

**WT-C — PcTools MCP wrappers + tests.**
Files: `tools/PcTools/src/kilnctrl/email_settings_http_client.py` (new,
modeled on `kiln_configs_apply_http_client.py`'s confirm/refuse/read-back
shape), MCP registration for `email_settings_get`/`email_settings_set`,
unit tests under `tools/PcTools/tests/`. Checks:
`run_pctools_tests` (via the pytest runner, per
`project_pctools_tests_need_pytest_runner`). Acceptance: `set` refuses
without `confirm=True`, `get` never surfaces a password value even if the
board's response somehow included one (defensive strip on the client side
too), both callable via `kiln_find`/`kiln_call` after a server restart.

**WT-D — host tests for the code lifecycle.**
HTTP handlers are target-build only (8 of 50 link into host tests per
`project_http_handlers_are_target_build_only`) — do not attempt to host-test
the routes themselves. Host-testable pieces: the 6-digit code generator
(entropy source, format), the 10-minute-expiry/single-use state machine,
the NVS struct versioning/round-trip, and the write-only-password GET
serializer (assert no password key in the JSON output). Files: new
`firmware/KilnFW/App/test/test_email_reset_code.c` (or similar, following
the existing `test_*` naming under that directory),
`test_email_config_persist.c`. Checks: `-Only "check_00_kilnfw_host_tests"`.
Acceptance: a negative test that corrupts the expiry check and confirms the
host-test suite catches it (per the negative-test-every-check rule), then a
forced rebuild before restoring, not a hand-restore.
