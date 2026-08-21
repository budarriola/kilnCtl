# UI_PLAN.md — LCD + web usability/cleanup plan (open work only)

Finished work has been moved out of this file per the 2026-08-21 cleanup
rule: done items either became reference material in
`docs/ARCHITECTURE_DECISIONS.md` / `docs/BRINGUP_HAZARDS.md`, or were
deleted outright as narrative with no lasting value. This file now tracks
**only work not yet done**. See those two docs for the no-scroll budget
rule, the current page/route map, the LVGL touch-hit-testing gotchas, and
what shipped in the 2026-08-19/20/21 passes (icon top bar, denser config
hub, paged diagnostics, profile-creation flow, web nav.js/app.js, etc.).

## Where the completed sections went

Code comments across `App/` still cite this file's pre-prune section
numbers (e.g. "UI_PLAN.md section 3 item 5"). Those sections no longer
exist here; this table says where each one's substance landed so such a
citation still resolves to something. Do not use this table to renumber
or restore anything below — it is a lookup, not a table of contents.

| Old section | Was about | Now |
|---|---|---|
| 1. LCD audit | Per-page ~264px no-scroll budget audit, shrunk touch targets | `ARCHITECTURE_DECISIONS.md` ("LVGL / LCD rendering"), `BRINGUP_HAZARDS.md` ("LVGL status-bar icon touch clipping") |
| 2. Web audit | Table overflow, button min-height, fixed-width elements | Completed, removed |
| 3, LCD items 1-6 | Status-card overrun fix, relay-row height cap, touch-target borders, touch-cal/test budget check, config-hub grid scrolling, touch-cal Cancel button | Completed, removed (touch-hit-testing gotchas kept in `BRINGUP_HAZARDS.md`) |
| 3, Web items 1-6 | Table `.table-scroll` wrapper, button min-height, AP-mode QR viewport, coupling-matrix zone count, fixed-width canvas audit, DHCP/static IP toggle | Completed, removed |
| LCD: profile creation page (planned) | New profile-builder page (graph view, point stepper) | Built as `ui_page_profile_builder_{zones,segment,review}.c` — see `ARCHITECTURE_DECISIONS.md` ("Page organization"); note the shipped flow uses stepper cards, not the originally-planned graph/drag view |
| Web: settings/profile import-export (planned) | Same feature, pre-prune draft | Settings half partially built as `backup_http.c` 2026-08-21; profile half still open — see "Open: settings import/export (partial) and profile import/export" above |
| Web: page structure rework (planned) | Route map, dashboard reorg | Built — `safety_page.html`, `diagnostics_page.html`, `manual_page.html`, `settings_page.html` trimmed to the danger zone, `main_page.html` reordered/trimmed, `nav.js`/`app.js` — see `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| Web: global-chrome rework (planned 2026-08-21) | Drop-down nav, Home button, dashboard reorder, settings trim | Built same day — see `ARCHITECTURE_DECISIONS.md` ("Page organization", "Global chrome rework") |
| 4 item 1 | Shared `nav.js` header + bottom nav bar | Built, then the bottom nav was deleted entirely in the chrome rework — `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| 4 item 2 | Sticky Stop control on every page | Built — `app.js`, `ARCHITECTURE_DECISIONS.md` |
| 4 item 3 | Shared `/app.js` poller (single `/api/status` fetch, visibility-aware) | Built — `app.js`, `ARCHITECTURE_DECISIONS.md` |
| 4 item 4 | Connection-lost banner, `.kc-stale`/`.kc-live-value` | Built and adopted on every page — `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| 4 item 5 | Confirm step for destructive actions | Built, and **corrected 2026-08-20**: Start and Stop both confirm now, not just destructive actions — see `ARCHITECTURE_DECISIONS.md` ("Page organization") and `App/drivers/ui_confirm.c` |
| 4 item 6 | Auth / session-token layer, scope decision | Still open, explicitly deferred by the owner — see "Open" section above |
| 4 item 7 | Unit parity (°F/°C) | Built — shared device-backed `unit_pref.c/.h`, see `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| 4 item 8 | `main_page.html` trim as it splits | Built — see `ARCHITECTURE_DECISIONS.md` ("Page organization") |
| 5. Data already on the wire | `/api/status` field inventory for new web pages | Completed, removed (fields now just exist in the API; nothing to track) |
| 6. TLS for web UI and OTA | TLS plan | Still open, explicitly deferred by the owner — see "Open: TLS" section above |
| 7. Not yet done / 2026-08-20 decisions | Recap of decided-but-unbuilt scope | Superseded by the "Open" sections above (this file now tracks only open work) |
| LCD navigation (Back-button audit) | `ui_page_temperature.c`'s wrong Back target | Completed, removed |

Driven by the standing user requirement: "the user interface is in need of
usability and cleanup fixes... the webpage should be optimized for a phone
or tablet. and the lcd should not require scrolling." That budget rule and
its arithmetic convention are recorded once in `ARCHITECTURE_DECISIONS.md`
("LVGL / LCD rendering" section) rather than re-derived per page here.

## Open: settings import/export (partial) and profile import/export

Two separate import/export features, kept independent (different data,
different failure modes if merged into one blob).

1. **Settings import/export** — partial (`backup_http.c`, see
   `docs/ARCHITECTURE_DECISIONS.md` "Backup / restore"). Open items tracked
   in `firmware/KilnFW/TODO.md` section 0.5.
2. **Profile import/export.** Covers kiln firing profiles only
   (`profiles_save`/`profiles_get`/`profiles_list` imply existing schema) —
   export a single profile or all profiles as JSON from `profiles_page.html`,
   import via file-picker + upload. Should reuse whatever point/segment
   schema the LCD profile-creation flow (`ui_page_profile_builder_*.c`)
   writes, so a profile authored on the LCD round-trips through web
   export/import unchanged.
3. **Shared mechanics, not shared data:** both likely want the same
   file-picker + `POST` upload + JSON-parse-and-validate pattern on the
   ESP32 HTTP server side, so implementation can share a helper, but the two
   export files/endpoints stay separate (`/api/settings/export`,
   `/api/profiles/export` style) — a settings file should never accidentally
   double as a profile file or vice versa.
4. Needs confirming actual field lists in firmware source before
   implementation starts.

## Open, explicitly deferred by the owner: auth / session layer

**Do not implement.** Scope was decided 2026-08-20 (writes gated, reads
open, server-side session tokens, multi-user) but building it is not
authorized yet.

- **Open, no auth:** every page and every read endpoint (`GET /api/status`,
  `/api/profile_exec`, `/api/readiness`, `/api/history.csv`, `/api/zones`,
  `/api/rules`, `/api/profiles`, `/status`, `/scan`).
- **Gated once built:** every state-changing POST — `/api/relay`,
  `/api/profile_exec/{start,stop,pause,resume,ack_last_run}`,
  `/api/profile`, `/api/profile/delete`, `/api/safety/clear_trip`,
  `/api/zones`, `/api/rules`, `/api/control`, `/api/autotune/*`,
  `/provision`, `/forget`, `/ip_config`, and the danger zone. OTA keeps its
  own existing per-request HMAC challenge (`App/drivers/ota_auth.{c,h}`)
  regardless — this session layer sits alongside it, never replaces it.

**Mechanism — reuse `ota_auth.{c,h}`, don't invent a second scheme.** That
module already has the primitives: `ota_auth_nonce_issue()`/`_check()` (16
random bytes, single use, 30 s expiry, IP-bound), `hmac_sha256()` over the
nonce keyed by the AP password (so the password itself is never sent),
`ota_auth_constant_time_equal()`, and `ota_auth_lockout_*()` for brute-force
backoff. Login flow mirrors OTA's challenge/verify: `GET /api/auth/challenge`
→ client HMACs the nonce with the AP password → `POST /api/auth/login`.

**Session tokens — server-side table, not a stateless signed cookie.**

- **Token**: 32 bytes from `esp_fill_random()`, hex-encoded, random (not
  derived) — nothing recoverable from it.
- **Storage**: fixed-size RAM table (start at 8 slots) of `{token_hash,
  client_ip, issued_ms, last_seen_ms}`, guarded the same way `ota_http.c`'s
  `s_ota_lock` is. Store the SHA-256 of the token, not the token itself.
  Compare with `ota_auth_constant_time_equal()`.
- **Multi-user**: the table is why this beats a stateless HMAC cookie — N
  independent sessions, each revocable (logout, password change, "sign out
  all"); a stateless token cannot be revoked before it expires. Full table
  evicts the least-recently-seen slot — a stale session must never lock out
  a real operator.
- **Expiry**: sliding idle timeout (~30 min) plus a hard absolute cap
  (~12 h). Both cleared on reboot (RAM-only table) — deliberate.
- **Transport**: `Set-Cookie: kiln_sid=…; HttpOnly; SameSite=Strict; Path=/`.
- **IP binding**: bind the session to the login IP, same as OTA's nonce
  (`httpd_req_to_sockfd()` + `getpeername()`) — kills stolen-cookie replay
  from another LAN device. Cost: switching Wi-Fi network requires re-login.

**Honest limitation to state in the UI, not paper over:** plain HTTP means
the session token travels in clear text on every gated request and can be
sniffed on the LAN, even though the *password* never is. IP binding and the
idle timeout narrow that window, not close it. TLS (below) closes it; this
layer is designed to be correct without TLS first so a handshake bug and an
auth bug stay distinguishable during bring-up.

**Open sub-question, not blocking:** whether the AP password is the right
long-term credential, or whether a separate "web password" should be
settable. Reusing the AP password ships first — already provisioned,
already the OTA credential, no new stored secret.

## Open, explicitly deferred by the owner: TLS for web UI and OTA

Requested explicitly: *"i also want tls for both ota and this. for now plan
only."* **Plan only — not authorized to build.** Closes the auth layer's
"sniffable on the LAN" gap once it lands, and protects OTA's firmware image
transfer (today plain HTTP).

**Server**: swap `httpd_start()` for `httpd_ssl_start()` (`esp_https_server`,
already in ESP-IDF). Every module reaches the server through
`wifi_provision_http_get_server()`, so no other `.c` file's registration
code changes. `httpd_ssl_config_t` wraps the existing `httpd_config_t`, so
the deliberate deviations already in `docs/WEB_UI.md` (`lru_purge_enable`,
`max_uri_handlers`, `stack_size`) carry over unchanged.

**Certificate — self-signed, generated on-device at first boot.** No CA
will issue for a LAN device with no public name; a cert baked into the
firmware image would be identical on every board and extractable from the
binary — worse than self-signed.

- **ECDSA P-256, not RSA-2048** — an order of magnitude faster handshake on
  an ESP32-S3, smaller key/cert/mbedTLS footprint. RSA's only edge is
  ancient-client compatibility a phone browser doesn't need.
- Generated once, persisted in NVS in its own namespace (see
  `docs/PROJECT_STATUS.md`'s one-partition-per-concern rule and its
  boot-time compatibility check, which the cert namespace needs to
  participate in).
- CN/SAN covering the mDNS name (`kilnctl.local`) and the current IP — IP
  SANs go stale on a DHCP lease change; plan for regeneration on IP change
  or accept name-only access and make mDNS the supported path. Decide
  during implementation.
- **Show the certificate fingerprint on the LCD** (`ui_page_diagnostics.c`,
  which already exists and has room). This is what makes self-signed
  defensible: the user compares the browser's "untrusted certificate"
  fingerprint against the physical panel and confirms it's *their* kiln,
  not something else answering on that IP.

**Port and redirect layout.** HTTPS on 443; a plain-HTTP listener stays on
80 doing nothing but a 301 to HTTPS, so a bookmarked/typed
`http://kilnctl.local` still lands. **AP-mode provisioning stays plain
HTTP** — phone captive-portal detection breaks on TLS, and a fresh board's
AP has no name/trusted cert anyway. `wifi_provision_page.html` served over
the fallback AP is the one deliberate exception; every STA-mode route gets
TLS.

**Cost.** PSRAM is on (`CONFIG_SPIRAM=y`, octal, 8 MB, `_USE_MALLOC=y`), so
mbedTLS's session/record buffers can land there — but the handshake still
burns **internal** SRAM for stack, and this firmware has hit internal-SRAM
exhaustion once already (see `BRINGUP_HAZARDS.md`). So when implementing:

- Size the HTTPS server task's own stack deliberately (don't take the
  default), same reasoning that already forced `stack_size = 8192` on the
  plain-HTTP server for `zones_post_handler`'s body buffer.
- Reduce `MBEDTLS_SSL_IN_CONTENT_LEN`/`OUT_CONTENT_LEN` from the 16 KB
  default — this UI's largest body is ~3.2 KB, so 4 KB is generous.
- Enable TLS session resumption/tickets — the shared poller reconnects
  regularly; a full ECDHE handshake every couple seconds per client is the
  one workload that would actually hurt.
- Measure `esp_get_minimum_free_heap_size()` and PSRAM free, before/after,
  with two or three phones connected — `ui_page_diagnostics.c` already
  displays both.

**OTA over TLS.** `ota_http.c`'s routes ride the same server, so they
inherit TLS with no per-route change; the HMAC challenge scheme stays as
defense in depth (proves knowledge of the AP password without sending it).
TLS adds confidentiality/integrity of the image transfer; it does **not**
replace image signing — secure boot / signed images guard against a
malicious image delivered over a perfectly good TLS connection, and signing
is not in this plan.

**Sequencing.** TLS lands *after* the auth/session layer above works over
plain HTTP, not before — otherwise a handshake bug and an auth bug are
indistinguishable during bring-up.
