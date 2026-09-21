# Web + LCD authentication plan

> **Status:** in progress · **Opened:** 2026-09-16. The backend, LCD half,
> physical credential-reset gesture, the web login surface and the admin
> password/settings page (both section 6), and the web-GUI half of the
> inactivity lock (section 8) have all landed and are host-tested. Section 9
> is also fully closed, per its own 2026-09-17 status note below — the
> "still pending" wording that used to sit here was stale against that note.
> What remains is any outstanding items in sections 10-13 — see each
> section's own status line (section 10 is bench-only work, out of scope for
> a host-only pass). No board was flashed and no heating run was performed
> for this refresh; no `.kicad_*` file was touched.

Username/password authentication and roles for the web GUI, two numeric PINs
for the LCD, an inactivity lock on both, and a physical-presence credential
reset.

---

## 0. Read this first: what is done, and what is still pending

**Done and host-tested** (see the commits landing each item for detail —
`git log --oneline -- docs/WEB_AUTH_PLAN.md` and the files named below):

- **Route tiers** (item 1): `firmware/KilnFW/App/drivers/http/route_tier_table.h`
  classifies every registered route as OPEN/USER/ADMIN/ADMIN_BOOTSTRAP, and
  `http_auth_enforce.c`/`http_auth_http.c` gate every real request through it
  via `kiln_http_register()` — a route not converted to that macro cannot
  reach production per `check_route_tier_coverage.ps1`.
- **Credential storage** (items 2, 2b, 3): `web_auth_store.c`/`.h` (namespace
  `kiln_auth` on the default `nvs` partition, PBKDF2-HMAC-SHA256, per-role
  records) and `security_backend_web_auth.c` (the setter, wired into
  `main_network_http.c`'s bringup and into `CMakeLists.txt` for the real
  target, not just host tests). The nine previously OTA-authenticated routes
  are ordinary ADMIN rows in the tier table; the AP-password fallback for
  those nine while auth is disabled is implemented in `ota_http.c`.
- **Session primitives** (item 4): `web_auth_session.c`/`.h`,
  `http_session_iface.c`/`.h`.
- **LCD PIN entry, keypad, and the LCD half of the inactivity lock**
  (items 7, 8's LCD half): `lcd_auth_state.c`, `ui_lcd_keypad.c`,
  `ui_lcd_lock.c`, `lcd_credential_bridge.c`.
- **The physical credential reset** (item 10): `auth_reset_gesture.c`/`.h`,
  `auth_reset_gesture_wiring.c`.
- **Admin-bootstrap wiring** (items 10/11): `ROUTE_TIER_ADMIN_BOOTSTRAP` gates
  `POST /api/auth/bootstrap_password` (`security_backend_web_auth.c`)
  exclusively through `web_auth_admin_bootstrap_needed()`, and a stale ADMIN
  session is denied on ordinary ADMIN routes while bootstrap is outstanding.

- **The section 12 host-test matrix**: `test_http_auth_enforce.c` drives all
  60 tier/role/enabled/bootstrap combinations against the contract stated in
  `http_auth_enforce.h`, and asserts every row of `route_tier_table.h` is
  found by `http_auth_lookup_tier()` with its declared tier.
  `test_web_auth_safety_interaction.c` covers the stop path while bootstrap is
  outstanding, and `test_check_route_tier_coverage.ps1`'s fifth assertion
  proves no tier-table row is orphaned. All of these call the production
  functions rather than a transcribed copy of the logic, and each was
  negative-tested by sabotaging the production source, confirming a
  behavioural RED, hand-restoring, and forcing a full rebuild from a deleted
  build directory before reconfirming GREEN.
- **Section 6, the login page and route:** `GET /login` and
  `POST /api/auth/login` (`web_auth_login_http.c`) exchange a password for a
  session cookie for either role.
- **Section 6, the admin password/settings page:** `GET /settings/security`,
  `GET /api/auth/config`, and `POST /api/auth/security` (`security_http.c`)
  are now registered (ADMIN tier, `route_tier_table.h`) and dispatch through
  the already-host-tested `security_http_core.c`/`security_backend_web_auth.c`
  pair. `net/security_page.html` (the page itself, written earlier) named
  route registration, this HTTP glue, the `CMakeLists.txt` wiring, and the
  nav.js entry as its own remaining DEFERRED items; all four now landed.
  `test_http_auth_enforce.c` asserts all three routes resolve to
  `ROUTE_TIER_ADMIN` against the real table, negative-tested the same way as
  the rest of this list.
- **Section 8, the web-GUI half of the inactivity lock:**
  `GET /api/auth/session` (OPEN tier, a passive status poll that never
  extends a session) and `POST /api/auth/session/extend` (USER tier, the
  explicit "stay unlocked" action) are implemented in
  `web_auth_session_status_http.c`. Any ALLOWed request against USER/ADMIN
  counts as activity and extends the session automatically
  (`http_auth_decision_counts_as_activity()`,
  `kiln_http_prehandler()` in `http_auth_http.c`) — no route has to touch
  the session itself, and the status poll is excluded from that set by
  tier alone rather than a special-cased branch. Server-side enforcement is
  independent of the browser: `http_auth_session_touch()`
  (`http_session_iface.c`) re-validates the session with
  `web_auth_session_is_valid()` before ever calling `web_auth_table_touch()`,
  so an already-expired session can never be revived just because a client
  keeps calling in. `app.js` polls the status route every 5 s and shows a
  small, non-blocking `.kc-lock-prompt` corner widget (`theme.css`) — not a
  full-screen overlay — in the server-reported prompt window, and returns
  to the Dashboard (`/`, not `/login`) once the server reports the session
  gone, per this section's own wording below. `test_http_session_iface.c`
  covers the status/touch contract, negative-tested the same way as the
  rest of this list.

**Still pending:**

- Section 9 is fully closed (see its own "Status, 2026-09-17" note below) —
  the bullet that used to be here claiming a further sweep was stale against
  that note and has been removed.
- Sections 10-13's own status lines name any work still open under each
  (section 10 is bench-only, out of scope for a host-only pass).

### Two facts that shape the design

### Two facts that shape the design

1. **The OTA challenge *mechanism* is kept; the separate OTA *credential* is
   retired.** `GET /api/ota/challenge`, the `X-Ota-Mac` header and the whole
   nonce/lockout/constant-time machinery stay. What goes is the second secret:
   the nine currently-authenticated routes become ordinary ADMIN routes
   verified against the single administrator credential, so an operator never
   sets or remembers an OTA password of their own. See item 2b.
2. **Auth defaults to OFF for both interfaces** (owner decision). The tier
   model must behave correctly in that configuration, not treat it as an error.
   See item 11.

---

## 1. Route classification — three tiers

Tier names: **OPEN** (no credential, ever), **USER** (`user` or
`administrator`), **ADMIN** (`administrator` only).

The 137 registered routes are classified below (comment-stripped count, the
same one `check_uri_handler_cap.ps1` and `check_route_tier_coverage.ps1`
report — an earlier draft said 140, which counted three `.uri = "..."` text
matches inside `wifi_provision_http.c`'s own historical audit-comment block
as if they were real registrations). The rule that decides the ambiguous
cases: **a route is OPEN only if its response cannot be used to
change the kiln's behaviour and reveals nothing an onlooker at the kiln cannot
already see.** Anything that writes is at least USER.

### OPEN — the always-viewable dashboard

The dashboard must be monitorable by anyone, so these are the routes
`app.js` actually fetches to render it, plus the static assets and page shells
needed to load it:

`GET /`, `/app.js`, `/nav.js`, `/theme.css`, `/commissioning_shared.js`,
`GET /api/status`, `GET /api/profile_exec`, `GET /api/readiness`,
`GET /api/ota/esp/status`, `GET /api/history.csv`,
`GET /api/profile_plan`, `GET /api/board_temps`, `GET /api/firing_history`.
(An earlier draft also listed `GET /api/unit_pref` here — no such route
exists. Only `POST /api/unit_pref` is registered, and it belongs in ADMIN
below; the value the dashboard needs is already in `GET /api/status`'s JSON.)

Also OPEN, read-only and needed before anyone can log in at all:
`GET /status`, `GET /scan`, `GET /networks`, `GET /wifi`.

### USER — start and stop a firing, and nothing else

`POST /api/profile_exec/start`, `/api/profile_exec/stop`,
`/api/profile_exec/pause`, `/api/profile_exec/resume`,
`/api/profile_exec/ack_last_run`.
Plus the reads a start needs: `GET /api/profile`, `/api/profiles`,
`/api/profiles/builtin`, `/api/profile/export`, `/api/kiln_configs`,
`/api/profiles/favorites`.

`/api/profile_exec/stop` is reached from the dashboard by `app.js` today. See
item 9 — stop is **never** blocked by a missing session.

### ADMIN — everything else

Every remaining route, without exception. Named explicitly so no reviewer has
to infer the set:

*Config and zones:* `POST /api/zones`, `/api/zones/pid`,
`/api/zones/current_sweep/start`, `/api/zones/current_sweep/abort`,
`/api/settings/tz`, `/api/settings/display_power`, `/api/unit_pref`,
`/api/watchdog_cfg`, `/api/ramp_assist`, `/api/sim`, `/api/setup/progress`;
and the reads `GET /api/zones`, `/api/zones/ct_channel_map`,
`/api/zones_diag`, `/api/zones/current_sweep/status`, `/api/control`,
`/api/settings/display_power`, `/api/watchdog_cfg`, `/api/ramp_assist`,
`/api/sim`, `/api/setup/progress`, `/api/kiln_configs/export`.

*Profiles as data (not execution):* `POST /api/profile`,
`/api/profile/delete`, `/api/profile/import`, `/api/profile/builtin/hide`,
`/api/profile/builtin/restore`, `/api/profile/favorite`,
`POST /api/kiln_configs/{apply,clone,delete,import,rename,save}`.

*Safety and calibration:* `POST /api/safety/clear_trip`,
`/api/safety/commissioning`, `/api/safety/commissioning/{bench_preset,ct_auto_zero,ct_cal,relay_type}`,
`/api/safety/log_level`, `/api/safety/rate_guard/auto`, `/api/estop/verify`,
`/api/relay_cycles/reset`, `/api/relay_cycles/restore`; and the reads
`GET /api/safety/commissioning`, `/api/safety/rate_guard/auto`,
`/api/thermo/faults`.

*Tuning:* `POST /api/autotune/{start,accept,abort}`,
`/api/adaptive_tune/{enable,revert}`; and the reads `GET /api/autotune`,
`/api/autotune/matrix`, `/api/autotune/trace.csv`, `/api/adaptive_tune`,
`/api/tuning_recommendations`, `/api/logs/autotune`, `/api/logs/firing`.

*Danger zone:* `POST /api/diagnostics/danger/{enable,relay,start,stop}`; and
`GET /api/diagnostics/danger`.

*OTA, reset, filesystem:* every `/api/ota/*` route, `POST /api/factory_reset`,
`/api/sw_reset`, `/api/cfgfs/file`, `/api/cfgfs/format_confirm`,
`/api/backup/import`, `/api/crash_report/{ack,clear}`; and the reads
`GET /api/ota/interlock`, `/api/ota/pico/status`,
`/api/ota/pico/rollback/status`, `/api/cfgfs`, `/api/cfgfs/file`,
`/api/cfgfs/format_pending`, `/api/dualwrite_window`, `/api/partitions`,
`/api/backup/export`, `/api/crash_report`, `/api/boot_guard`,
`/api/coredump/info`, `/api/coredump/chunk`, `/api/debug/lwip_stats`,
`/api/diagnostics/timing`, `/api/saftyfw_stack_margin`.
Also `POST /api/dualwrite_window/restore_verified` (mutates the same
persisted dual-write/migration state its GET sibling above reads — an
earlier draft omitted it from every table here).

(`POST /api/kiln_configs/quarantine_clear`, referenced in
`kiln_cfg_store.c`'s comments, is still not an actually-registered route
(confirmed 2026-09-21 — only `kiln_cfg_store_quarantine_clear()` exists, no
`httpd_uri_t` wires it up) and needs no tier entry until it is; deferred by
the URI handler cap, which sits at 155/160 with essentially no headroom left
(see CLAUDE.md's "URI handler cap" note). It is not part of the 137.)

*Network writes:* `POST /provision`, `/forget`, `/ip_config`.

*Page shells other than `/`:* `/status`, `/settings`, `/settings/backup`,
`/settings/display`, `/settings/zones`, `/settings/kiln_configs`,
`/settings/safety`, `/safety`,
`/safety/commissioning`, `/profiles`, `/diagnostics`, `/readiness`, `/setup`,
`/ota`. (`/settings/kiln_configs` split off from the `/` dashboard 2026-09-18;
same ADMIN tier as every other `/settings/*` shell here.) A page shell is
HTML only, but an unauthenticated visitor who can load
the diagnostics shell learns the board's full feature surface, and it is
cheaper to gate the shell than to audit every widget inside it.

`GET /api/ota/challenge` stays OPEN — it issues a nonce and (per item 2b) the
administrator record's salt and iteration count, none of which is usable
without the administrator password itself. Gating it would break the flow it
exists to serve. The nine routes it serves are ADMIN like any other.

**`GET /api/ota/esp/status` — FIXED in `9c2b1c1b`.** The payload was trimmed
to remove the commit-and-dirty-flag fingerprint and the other identity/
recovery-state fields flagged below; the route deliberately stays OPEN tier,
since a dashboard progress read is the intended, non-sensitive use. The
original finding is kept here for the record: the handler
(`ota_esp_status_get_handler()`, `ota_http_esp.c`) used to return more than a
progress percentage — `commit` (the exact firmware git commit hash), `dirty`
(whether that build had local modifications), `build_date`,
`active_slot`/`inactive_slot` with `inactive_version`, `recovery_mode`
(whether the board is boot-looping into recovery), and, once one update had
happened, the last update's `image_sha256_hex`. That fingerprint would have
let a remote, unauthenticated attacker target a known defect in that exact
build, and `recovery_mode` signaled a window where the board's own safety
gating (boot_guard) was in a degraded state — not the same class of
information as "what an onlooker at the kiln can already see" (section 1's
OPEN test).

### Ambiguous routes, with a recommendation

| Route | Why ambiguous | Recommendation |
|---|---|---|
| `GET /api/history.csv`, `/api/firing_history` | Full thermal history is more than a glance at the panel | **OPEN.** The owner's requirement is that a run can be monitored; a run's temperature curve is the monitoring. |
| `GET /api/zones` | Read-only, but exposes PID gains and per-zone calibration | **ADMIN.** It is a config dump, not telemetry. Its temperatures are already in `/api/status`. |
| `GET /api/readiness` | Enumerates every unfinished commissioning step | **OPEN.** The dashboard renders a readiness banner from it, and it reports nothing secret. |
| `POST /api/unit_pref` | Only a POST route exists (no GET) — the earlier draft's "OPEN for GET" half was fictional | **ADMIN.** The GET variant does not exist; the value the dashboard needs is already in `GET /api/status`'s JSON, which is OPEN. |
| `GET /api/board_temps` | Telemetry, but board-internal | **OPEN.** Same class as `/api/status`. |
| `GET /api/coredump/*`, `/api/crash_report` | Diagnostics, no write | **ADMIN.** A coredump contains RAM contents, including the session table. |
| `GET /status` (the Wi-Fi status page) | Publishes the AP password in plain text | **OPEN, unchanged.** It is the board's own AP identity and is already deliberately public. Because it is public, the web credential must not be derived from it — see item 2. |
| `POST /api/estop/verify` | An operator record, not a live control | **ADMIN.** It records that E-stop wiring was bench-verified; a wrong record weakens a safety argument. |

*Acceptance:* every `.uri` literal in the tree appears exactly once in the
tier table generated by item 5's check, and the check's route count matches
`check_uri_handler_cap.ps1`'s count for the same tree.

---

## 2. Credential storage

**Separate from the AP password.** `UI_PLAN.md` left "AP password or a separate
web password" as an open sub-question and shipped AP-password-first. The
owner's new requirements settle it: a password that must be hashed, salted and
unrecoverable cannot be the one `GET /status` prints in plain text.

**Hash: PBKDF2-HMAC-SHA256, 16-byte random salt from `esp_fill_random()`,
32-byte output, 20 000 iterations.** Reasoning:

- Only `psa/crypto.h` is included anywhere in the tree today; no raw
  `mbedtls/*.h` header is. So the first sub-task is to confirm
  `PSA_ALG_PBKDF2_HMAC(PSA_ALG_SHA_256)` is enabled in this build's PSA
  configuration. **If it is not**, the fallback is an explicit iterated
  HMAC-SHA256 loop over the already-proven `psa_mac_compute()` path that
  `ota_http.c` uses — same cost, same security for this threat model, and no
  new Kconfig option. Do not add a new crypto dependency to get a fancier KDF.
- bcrypt/scrypt/Argon2 are not in this build and are not worth adding. The
  threat is an attacker who has extracted the NVS blob from a kiln on a home
  LAN, not an internet-scale credential dump.
- 20 000 iterations of HMAC-SHA256 is roughly 30–60 ms on this ESP32-S3.
  That is unnoticeable on a login and a real cost to an offline guesser. It
  runs **on the httpd task**, so it must use no large stack locals (see
  item 3's stack budget) — PBKDF2's working set is two 32-byte blocks.

> **Status correction, 2026-09-20 — the 30–60 ms estimate above was wrong,
> and it crashed the board.** `POST /api/auth/login` panicked every time it
> was called: crash id bb8ed4204c07 (a crash-report record id, not a git
> commit -- deliberately not backticked, so check_doc_hash_citations.ps1 does
> not read it as a commit citation), symbolized against
> `KilnCtrl-727735eac269.elf` as `login_post_handler` →
> `web_auth_store_verify_password` → `web_auth_hash_compute`. The KDF ran
> long enough on the httpd worker task to starve **IDLE1** past
> `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5`, so the task watchdog fired and
> `CONFIG_ESP_TASK_WDT_PANIC=y` turned that into a reboot. Cause: the loop
> did a full `psa_import_key` → `psa_mac_compute` → `psa_destroy_key` round
> trip **per iteration** — 20 000 key imports, not 20 000 HMACs — so the real
> cost was ≥250 µs per iteration rather than the 1.5–3 µs the estimate above
> assumed.
>
> **Fix** (`web_auth_store.c`, `web_auth_hash_iterate()`): import the HMAC key
> **once** for the whole run and call `psa_mac_compute()` per round against
> that key id, destroying it once at the end. The key bytes never change
> across iterations, only the message does, and `psa_mac_compute()` is a pure
> function of (key, message) — so the derived bytes are unchanged and no
> stored credential needs migrating. `test_hash_compute_pinned_vectors()` in
> `test_web_auth_store.c` pins two digests computed from the pre-fix
> implementation at the real iteration count to hold that.
>
> The loop additionally calls `vTaskDelay(1)` every 1024 iterations so the
> watchdog is fed regardless of how slow the crypto backend turns out to be.
> It is **not** `taskYIELD()`: the httpd worker runs at
> `tskIDLE_PRIORITY + 5`, and a yield only re-picks the highest-priority
> *ready* task — the idle task would still never run. Only blocking gets the
> loop off the ready list.
>
> **Login latency expectation** (none was documented before): one password or
> PIN check costs the 20 000 HMAC-SHA256 rounds plus 19 one-tick sleeps,
> i.e. **roughly 0.2–0.5 s**, and it must stay well under
> `CONFIG_ESP_TASK_WDT_TIMEOUT_S`. Any change that raises
> `WEB_AUTH_ITERATIONS`, lowers `WEB_AUTH_HASH_YIELD_EVERY`, or reintroduces
> a per-iteration key import must be re-measured against that budget on real
> hardware.

**Where the record lives — corrected, and this is load-bearing.** An earlier
draft of this plan put the credentials in namespace `kiln_cfg` on
`KILN_NVS_PARTITION`. **That is wrong**, and reading `factory_reset.c` is what
found it: every reset scope erases whole **partitions** via
`hal_kv_erase_partition()`, and both `kKilnOnly` and `kAll` name `kiln_nvs`.
Credentials placed there would be destroyed by the "kiln" button, which is
advertised as erasing only the kiln configuration.

**Credentials live in their own NVS namespace `kiln_auth` on the default `nvs`
partition** (`NVS_DEFAULT_PART_NAME`, 24 KB at 0x9000). Reasons, in order of
weight:

- **No reset scope can reach it.** The scope table names exactly three
  partitions — `wifi_nvs`, `kiln_nvs`, `profiles_nvs` — and `nvs` is not one of
  them. The blanket `nvs_flash_erase()` that would have reached it was retired
  in 2026-08-12 (TODO.md 8.1) and no call to it remains under `App/`. This
  gives credentials the same "no scope's erase can reach another scope's data"
  property that `factory_reset.c`'s own header comment claims for the rest.
- **It exists on every already-flashed board**, so a field upgrade delivered as
  an OTA — which cannot rewrite the partition table — can still enable
  authentication afterwards. A dedicated new `auth_nvs` partition was considered
  and **rejected for exactly that reason**: there is free flash after `cfg` for
  one, but a board that only ever receives OTAs would never get the partition
  and so could never enable auth.
- It is no longer a config store. Zones, rules and profiles moved out to the
  three dedicated partitions; `nvs` is kept in place only so a rollback to
  pre-split firmware still finds its old data. One added namespace beside that
  is additive and collides with nothing.

Namespace `kiln_auth` is 9 characters. One versioned blob per credential
inside it:

- key `web_auth` — `{ version, flags, per-role records }` where each record is
  `{ username[33], salt[16], hash[32], iterations, must_change }`.
  Two records: one `administrator`, one `user`.
- key `lcd_auth` — `{ version, per-role records }`, each record
  `{ salt[16], hash[32], iterations, digits }`. **Two records: a `user` PIN and
  an `administrator` PIN** (owner decision, item 7). Separate key from
  `web_auth` so a PIN change does not rewrite the web blob.
- key `auth_policy` — `{ version, web_enabled, lcd_enabled, web_timeout_s,
  lcd_timeout_s }`. Two independent timeouts (owner decision, item 8).

**Key lengths, checked rather than assumed.** Four names, all inside NVS's
15-character cap: namespace `kiln_auth` (9), keys `web_auth` (8), `lcd_auth`
(8), `auth_policy` (11). Checked explicitly because an over-length key in this
project once silently never persisted at all. Each name is declared with
`NVS_KEY_LEN_CHECK()` so an over-length name is a compile-time failure, and each
is also caught by `tools/check_nvs_key_length.ps1`'s independent backstop. Any
later rename must keep both properties: a too-long key does not fail at
runtime, it quietly stores nothing — which for a credential record reads as
"no credentials set", the one outcome item 12b forbids.

**New schema version: `WEB_AUTH_STORE_VERSION 1u`**, its own constant in the
new module's header, following `SAFETY_CFG_STORE_VERSION` and
`KILN_CFG_STORE_VERSION`. **`ZONES_CFG_VERSION` stays at 26** — nothing here
touches the zones blob, and a bump would strand a rollback on default PID gains
for no reason.

**`cfg` LittleFS: NVS-only, no dual-write.** Credentials join the eleven
deliberately NVS-only items in `docs/CONFIG_FILESYSTEM.md`'s migration table,
for the same reason `setup_wizard_progress` did: the `cfg` partition is
unformatted and unmounted on the bench board, so a dual-write is inert today,
and a credential is exactly what must stay readable while diagnosing a
filesystem problem. Revisit only after the dual-write window closes.

**Both PINs are hashed identically to a password.** A 4–8 digit PIN has at most
10^8 candidates, so its hash is brute-forceable offline in any case; hashing it
costs nothing and stops a casual NVS dump from reading it directly. The real
protection is that PIN entry is physically local and rate-limited by the
existing lockout — which matters more now that one of the two PINs grants full
administrator access at the panel (item 7).

*Acceptance:* a host test sets a password, reads back the stored blob, and
asserts the plaintext appears nowhere in it; two identical passwords set on
two boards produce different hashes (salt is actually random and actually
stored); verification of the correct password succeeds and of a
one-character variant fails.
*Negative test:* replace the salt with a fixed constant in the production
setter, confirm the "two boards differ" assertion goes RED, restore by hand,
confirm GREEN.

---

## 2b. OTA and the other eight authenticated routes use the administrator credential

Owner requirement: **OTA must not require a password of its own.** The nine
routes that authenticate today — `POST /api/ota/esp`, `/api/ota/esp/rollback`,
`/api/ota/esp/recovery_exit`, `/api/ota/esp/boot_guard_reset`,
`/api/ota/pico`, `/api/ota/pico/rollback`, `/api/factory_reset`,
`/api/cfgfs/format_confirm` and `/api/sw_reset` — become ordinary ADMIN routes.
These nine are a *subset*, not the whole OTA/reset/filesystem route family:
that family (section 1's "OTA, reset, filesystem" list) has 32 entries in
`route_tier_table.h` today — 30 ADMIN, and 2 deliberately OPEN
(`GET /api/ota/challenge`, `GET /api/ota/esp/status`; see below). "Nine"
throughout this item refers only to the routes that carried their own
OTA-password authentication before this plan, matching the code comment at
`route_tier_table.h`'s "OTA, reset, filesystem" block.

**Kept versus retired, stated separately because the objection is only to one
of them:**

- **Kept:** `ota_auth.c` in full — nonce issue/check, single use, 30 s expiry,
  constant-time comparison, and the eight per-context escalating lockouts. Also
  kept: the `X-Ota-Mac` header shape, `ota_http_authenticate_request()`, the
  auth-before-state-check ordering, and the `boot_button_ota_bypass_active()`
  physical-presence bypass. None of that is the problem, and all of it is
  already host-tested.
- **Retired:** the *credential* — the `HMAC-SHA256(ap_password,
  "kilnctl-ota-v1")` key derivation. That is what makes OTA a second secret.

**What the credential is verified against.** One record: the administrator
entry in `web_auth` (item 2). Both paths check that same stored record, and
neither keeps a store of its own:

- **A web session.** An authenticated administrator session satisfies these
  routes with no MAC at all, which is what makes the browser OTA page work
  without a second prompt.
- **The challenge–response, for scripted and command-line clients.**
  `GET /api/ota/challenge` returns the nonce plus the administrator record's
  salt and iteration count. The client computes
  `PBKDF2-HMAC-SHA256(password, salt, iterations)` locally — the same value the
  board has stored — and the MAC key becomes
  `HMAC-SHA256(stored_hash, "<context string>")`, preserving the existing
  "the secret is the key, the context string is the message" structure that
  currently keeps the literal password out of the comparison path. The password
  itself never crosses the wire.

Publishing the salt is deliberate and standard (SCRAM does the same); a salt
without the hash is not a credential, and the challenge endpoint is covered by
the existing per-context lockout. One consequence stated honestly: the stored
hash is OTA-equivalent to the password for anyone who can already read NVS off
the flash. That is **not** a regression — today's OTA secret is the AP
password, stored in plaintext and deliberately published by `GET /status` — but
it is another reason the hash must never appear in an export (item 12b).

**Migration for a board that has an OTA password today.** There is no separate
OTA password to migrate: today's OTA credential *is* the AP password. A board
taking this firmware keeps working unchanged, because auth ships OFF and the
auth-off rule below preserves exactly today's behaviour. **At no point is there
an OTA route that no credential can open.** The AP-password path for these nine
routes is retired only at the moment an administrator enables web auth, which
is also the moment an administrator credential is guaranteed to exist (item 11
refuses to enable auth without one).

**Auth disabled: what gates OTA, and is it a loosening?** This is the one place
where "auth off collapses every tier to full access" must **not** apply, and it
is a deliberate, named exception. If auth-off left these nine routes ungated, a
field-upgraded board would go from OTA-requiring-the-AP-password to
OTA-open-to-the-network — **that would be a real loosening of the only routes
that are authenticated today.** Recommendation, and this plan's position:
**with web auth disabled, the nine routes fall back to today's AP-password
challenge, unchanged.** Auth-off therefore means "no web login required", never
"anyone may reflash the kiln". With web auth enabled, the administrator
credential governs and the AP-password path is refused.

*Acceptance:* a host test asserts that with auth enabled a correct
administrator-derived MAC is accepted on each of the nine contexts and an
AP-password-derived MAC is refused; that with auth disabled the reverse holds;
that an administrator session alone satisfies all nine; and that a `user`
session satisfies none. A bench check performs a real OTA with no OTA-specific
password configured anywhere.
*Negative test:* point the MAC key derivation back at
`wifi_prov_get_ap_password()` while auth is enabled, confirm the
"AP-password MAC is refused" assertion goes RED, restore by hand, confirm
GREEN, and force a full rebuild.

The LCD PINs remain separate secrets from the web password — that is the
owner's explicit design (item 7) and is untouched by this item.

**`GET /api/ota/esp/status` stays OPEN, but its unauthenticated payload was
trimmed (2026-09-17).** This route is one of the two deliberately-OPEN
entries named above, kept OPEN so a scripted OTA client can poll it before
authenticating. An audit found the OPEN tier's own test — "what an onlooker
standing at the kiln can already see" — did not actually cover three of its
fields: `commit` (the exact firmware git commit), `dirty` (whether that build
had uncommitted changes) and `build_date`. Publishing the exact commit lets a
remote, unauthenticated caller narrow its search for a specific known defect
in that exact build, and none of the three is otherwise observable by
standing at the board. The fix is **not** a retier (OTA clients still need
this route pre-auth) and **not** removing the fields from the schema (the
authenticated `/ota` admin page's `renderEspInfo()` still reads them, and
retiring the keys would be a breaking schema change for that page). Instead,
`ota_esp_status_get_handler()` now resolves the caller's role itself — via a
new `http_auth_caller_is_admin()` helper (`http_auth_http.c`/`.h`), since
`kiln_http_prehandler()` never resolves a role for an OPEN-tier route at all
(item 5's own performance carve-out) — and reports `commit`/`dirty`/
`build_date` as their real values only to a caller already holding an
authenticated administrator session (or when web auth is off entirely, per
item 11's "auth off is exactly as open as the board is today" collapse);
every other caller gets JSON `null` for those three keys, keeping the
response shape stable rather than dropping keys.

`recovery_mode` was deliberately left unauthenticated rather than folded into
the same redaction, decided on its own merits: the dashboard's OPEN-tier
`app.js` already polls this exact route to drive a recovery-mode banner on
the main page, a fix shipped for the 2026-09-08 incident where recovery mode
was effectively invisible to whoever was standing at the kiln. That is
precisely the OPEN tier's own test — a board already visibly unhealthy (LCD,
relays, general behaviour) is not narrowed by also reporting one boolean
about *why* — unlike the commit hash, which narrows an attacker's search
rather than describing something already visible. `version`, `active_slot`,
`inactive_slot`, `inactive_version`, `phase`, `percent` and `last_update`
were left unchanged for the same reason: none of them names a specific known
defect the way an exact commit hash does.

*Acceptance:* host tests in `test_ota_http.c` cover all four
web-auth/role combinations against the real
`ota_esp_status_get_handler()`/`http_auth_caller_is_admin()` production
functions: auth off (full payload, matching item 11); auth on with no
session (redacted); auth on with a `user`-role session (still redacted,
identical to no session); and auth on with an `admin`-role session (full
payload). `recovery_mode` and `version` are asserted present and unredacted
in the no-session case.
*Negative test:* forced `ota_esp_status_get_handler()`'s admin check to
`true` unconditionally, rebuilt, confirmed the three redaction assertions
(no-session and user-session cases, six checks total) went RED while the
other 176 of this executable's 182 checks stayed GREEN, restored the source
by hand, confirmed an empty `git diff` and a matching `git hash-object`,
deleted the test build directory, forced a full rebuild, and reconfirmed
182/182 GREEN.

**`GET /api/status` closed the same way, same day (2026-09-17).** The
analysis above was written against `GET /api/ota/esp/status`, but two of its
three narrowing fields — the safety processor's exact commit hash and its
dirty flag — were also reachable one OPEN route away, unauthenticated, on
`route_tier_table.h`'s `ROUTE_TIER("/api/status", HTTP_GET,
ROUTE_TIER_OPEN)`: `dashboard_status_get_handler()`
(`dashboard_status_http.c`) unconditionally emitted `safety_build_commit`,
`safety_build_datetime` and `safety_build_dirty` (the RP2040's build
identity) and `fw_build` (the ESP's own build timestamp) to any caller,
gated by nothing. Fixed with the identical `is_admin =
http_auth_policy_web_enabled() && http_auth_caller_is_admin(req)` gate this
section established, redacting only those four fields to JSON `null` for a
non-admin caller (or when web auth is on but the caller holds no admin
session); `safety_build_known`, `safety_config_version`,
`safety_config_crc`, `fw_version` and `fw_version_known` stay unconditional,
since none of them names a specific known defect and the dashboard needs the
version/known-flags to render regardless of caller. The build-identity
disclosure class named above is now closed across both routes that carried
it.

---

## 3. Password and PIN strength rules

**Both LCD PINs: 4 to 8 digits**, enforced in two places — the keypad refuses
to submit outside the range, and the web setter rejects out-of-range before
hashing. The setter's check is the authoritative one; the keypad's is a
convenience. Both must exist, because a PIN set out of range from a script
would otherwise be unenterable at the panel and lock the LCD permanently. The
two PINs must also differ from each other, or the administrator PIN is
reachable by anyone holding the user PIN.

**Web password: minimum 10 characters, at least one character that is not a
lowercase letter, and a rejection list of the obvious (`password`, `kiln`,
the username, the AP SSID, the AP password).** Reasoning:

- A character-class matrix (upper + lower + digit + symbol) is the rule that
  makes people write the password on the kiln, which is strictly worse than a
  moderate rule on a LAN-local device. 10 characters with one
  non-lowercase admits `firing kiln2` and rejects `kilnkiln`.
- The AP password is on the rejection list because `GET /status` publishes it.
  Reusing it as the web password would make the web credential public.
- No maximum below 64, no forced rotation, no composition beyond the above.

*Acceptance:* a host test table of accept/reject cases covering each boundary
— 9 and 10 characters, all-lowercase at 10, the AP password, the username,
PIN 3/4/8/9 digits — passes, and the same table is exercised against the HTTP
handler's validation path, not a test-local copy of the rule.

---

## 4. Session mechanism

Adopt `UI_PLAN.md`'s design as written, with these amendments:

- **Role in the slot.** Each slot gains a `role` field. A slot is
  `{ token_hash[32], client_ip[16], role, issued_ms, last_seen_ms }` — 56
  bytes plus padding.
- **Slot count: 8 web slots.** Concurrent sessions are capped at 8 with LRU
  eviction of the least recently seen, so a stale session can never lock out a
  real operator. 8 slots is comfortably above the ~6-socket accept-mailbox
  ceiling: more simultaneous sessions than the server can hold sockets for is
  wasted RAM.
- **The LCD has exactly one session**, a single struct, not a slot in the web
  table. The panel has one operator.

**RAM cost, stated explicitly.** 8 web slots at 64 bytes is 512 bytes of
`.bss`, plus one mutex (about 80 bytes), plus the single LCD session (64
bytes), plus the three cached policy fields. **Under 700 bytes of internal
DRAM, fixed at link time, no heap.** Measured headroom is 35,391 B free with a
27,015 B low-water, well above the 11.9 kB exhaustion line — a 700 B static
allocation does not move that materially, but it must be re-measured after the
feature lands, not assumed.

**Stack cost.** Login runs the KDF on the httpd task's shared 8 KB stack. Its
locals are the 32-byte salt+hash pair, a 64-byte username buffer and a
128-byte password buffer — under 256 bytes. **No httpd buffer and no zones JSON
buffer is enlarged by anything in this plan**, and no handler added here
declares a stack local over 256 bytes. This constraint is not negotiable: a
large local on this stack has panicked the board twice.

**Across reboot: all sessions drop.** RAM-only, deliberate. A reboot is an
event an operator can see, and persisting session tokens to flash would put a
bearer credential in NVS and wear it on every keepalive.

**Honest limitation, carried forward from `UI_PLAN.md`:** over plain HTTP the
session cookie travels in clear text and can be sniffed on the LAN, even
though the password itself never crosses the wire (the login is a challenge
HMAC). TLS is sequenced after this layer works over plain HTTP.

*Acceptance:* a host test issues 9 sessions against an 8-slot table and
asserts the 9th evicts the least-recently-seen slot and that the other 8 stay
valid; asserts two distinct logins never produce the same token; asserts a
token is compared by hash, never stored in the clear.

---

## 5. Enforcement point — fail closed, plus a mechanical check

A per-route allowlist consulted inside each handler is the failure mode to
avoid: a route added later with no entry is open, silently. So:

**The tier is declared at registration, not inside the handler.** Wrap
`httpd_register_uri_handler()` in a project-local
`kiln_http_register(server, &uri, tier)` that stores the tier in a parallel
table keyed by `(uri, method)` and installs a shared pre-handler. The
pre-handler resolves the session, compares role against tier, and sends 401
(no session) or 403 (insufficient role) before the handler body runs.

**Fail closed has to be structural, not a default value.** A route whose tier
is absent from the table is treated as ADMIN — the most restrictive tier — and
logs a loud `ROUTE WITH NO TIER` error at registration. That is the runtime
half. It is not sufficient on its own, because an unnoticed ADMIN default on a
route that should be OPEN breaks the dashboard quietly instead. Hence:

**`tools/check_route_tier_coverage.ps1`**, following
`check_uri_handler_cap.ps1`'s shape exactly: recursive `*.c` scan under
`firmware/KilnFW/App/drivers`, a local `Get-CodeOnlyLines` comment stripper, a
`Resolve-DriverFile` that fails loud on a missing or ambiguous path, the same
`\.uri\s*=\s*"` extraction, and the same blindness floor (`throw` if fewer
than 80 routes were found, so a broken regex cannot report a clean pass on
zero routes). It fails the build when any route literal is registered through
a path that does not carry an explicit tier, and names every offender.

**Design deviation from this section's original sketch:** the shipped
`kiln_http_register(server, &uri_handler)` takes no `tier` parameter — the
sketch above shows one. The tier is resolved *internally*, inside
`kiln_http_register()`, via `http_auth_lookup_tier(uri, method)` against
`route_tier_table.h`, rather than being supplied by each call site. Reasoning:
a caller-supplied tier is a second place the classification can drift from
the table that `check_route_tier_coverage.ps1` actually audits — a call site
could pass `ROUTE_TIER_USER` while the table (or a later edit to the table)
says `ADMIN`, and nothing would catch the mismatch until an attacker found
it. Resolving the tier from the single table `route_tier_table.h` already
owns means there is exactly one place classification can be wrong, and the
mechanical check already audits that one place. The fail-closed behaviour
this section calls for (an untiered route defaults to ADMIN and logs
`ROUTE WITH NO TIER`) is unchanged, and lives in `kiln_http_register()`
itself (`http_auth_http.c`) rather than needing a caller to have supplied
anything.

*Acceptance:* `tools/run_all_checks.ps1` discovers the new check by its
`check_*` name and it passes on a clean tree; the check's reported route count
equals `check_uri_handler_cap.ps1`'s.
*Negative test, as a checked-in script beside
`firmware/KilnFW/App/test/test_check_hal_include_boundary.ps1`:* register a
new mutating route with no tier in a scratch copy, confirm the check reports
exactly one finding naming that route; delete the blindness floor and confirm
the check still fails rather than passing vacuously; call the production
detection function with a synthetic route list to prove it can detect a rise;
restore by hand, confirm GREEN, and **force a full rebuild** — an empty
`git diff` proves the source is restored and says nothing about artifacts.

---

## 6. The password page

**Location: `/settings/security`**, a new page in the settings nav beside
`/settings/display` and `/settings/backup`. **ADMIN only** — a `user` cannot
open it and cannot change their own password; the owner described one person
who sets the passwords.

It sets, in one place: the administrator username and password, the `user`
password, **both LCD PINs**, **both lock timeouts** (web and LCD, independently
settable), and the two auth-enabled switches.
**The LCD PINs are set here and only here** — the LCD never sets its own PIN,
so a stolen panel session cannot change the credential that protects it. That
holds even for the LCD administrator PIN: full access at the panel still does
not include changing what the panel's own credential is.

**Changing a password invalidates every session for that role, including the
caller's.** The caller is redirected to a fresh login. Reasoning: the usual
reason to change a password is that the old one is compromised, and leaving
the sessions it authorised alive defeats the change. Changing the LCD PIN
drops the LCD session. Changing the lock timeout does not invalidate anything
— it is a preference, not a credential.

*Acceptance:* a host test asserts that a password change clears exactly the
slots holding that role and leaves other-role slots intact; a `user`-role
session receives 403 from every method on the security endpoints.

**Status, 2026-09-17 — landed.** `security_http.c` registers all three
routes (`GET /settings/security`, `GET /api/auth/config`,
`POST /api/auth/security`) via `kiln_http_register()`, wired into
`main_network_http.c`'s bringup right after `security_backend_web_auth_start()`
installs the real vtable. It is pure glue: request parsing and response
sending only, dispatching every decision into `security_http_core.c`'s
already-host-tested `security_http_dispatch()`. The page itself
(`net/security_page.html`) was already written and defined its own wire
contract (`GET /api/auth/config`'s JSON shape, `POST /api/auth/security`'s
`cmd=set_web_password|set_lcd_pin|set_policy` form contract) before this
glue existed; this landing matches that contract field-for-field rather than
re-deriving it. `CMakeLists.txt` gained the page in `KILNCTL_GZIP_ASSETS`
and `security_http.c` in the target's `SRCS`; `nav.js` gained a
`/settings/security` entry. The 403-for-`user`-role half of this section's
acceptance criterion is exercised by `security_http_core.c`'s existing host
tests (`security_http_dispatch()` unconditionally refuses a non-ADMIN caller
first); the password-change-clears-the-right-slots half is
`security_backend_web_auth.c`'s existing coverage. What is new here is only
the routing (all three ADMIN in `route_tier_table.h`, asserted against the
real table in `test_http_auth_enforce.c`) and the ESP-side parsing/response
glue — neither of which existed on `main` before this pass.

---

## 7. LCD PIN entry and the keypad

**The keypad is a modal overlay, not a page.** Build it with
`lv_msgbox_create(NULL)` on `lv_layer_top()`, exactly as `ui_confirm.c` does,
so it takes no part in any page's flex column and consumes **none** of the
no-scroll content budget on the 480x320 panel. This is the single most
important structural decision here: a keypad added into a page's column would
overflow a panel that already fits its content exactly.

Layout inside the overlay: a prompt line, a masked entry field showing one dot
per digit, a 3x4 `lv_buttonmatrix` of `1`–`9`, `0`, a backspace and an `OK`,
and a `Cancel` in the footer. **No new colours** — reuse the existing
`UI_THEME_*` constants (38 are already defined); the keypad uses the same
accent the confirm dialog's confirm button uses.

**When it appears:** when a touch on the LCD attempts a gated action while the
LCD is locked and `lcd_enabled` is true. The Dashboard itself never triggers
it.

**How it is dismissed:** `Cancel`, a correct PIN, a tap on the backdrop (the
`ui_confirm` backdrop already swallows stray taps and does nothing — here it
cancels), or the lock timeout expiring. Cancelling returns to the Dashboard.

**Entry validation:** `OK` is inert below 4 digits and entry stops accepting
at 8. A wrong PIN feeds the existing `ota_auth` lockout under a new context so
repeated wrong guesses at the panel escalate the same way a wrong OTA MAC does.

**Two PINs, two tiers — settled by the owner.** A `user` PIN grants the USER
tier and an `administrator` PIN grants ADMIN at the panel. Which PIN was
entered decides the tier; there is no username to type, so the PIN itself is
the role selector. Entry is identical for both.

**Accepted tradeoff, recorded rather than dropped.** This was decided against
the recommendation in this plan's first draft, and the cost is real: **a 4–8
digit secret now guards full configuration access at the panel**, while the web
administrator credential requires 10 characters with a non-lowercase (item 3).
A 4-digit PIN is 10^4 candidates — materially weaker, and it reaches zones
config, calibration and the danger zone through the panel. The owner accepts
this; the plan records it as an accepted tradeoff, not a solved problem.

What makes it tolerable is rate limiting, so the PIN path must be wired to it
properly rather than incidentally:

- Every wrong PIN feeds the existing `ota_auth` lockout under **its own new
  context**, separate from the web login's and from each OTA context's, so
  panel guessing cannot lock out the web interface and vice versa.
- The existing escalating backoff applies unchanged: 3 failures arms it, then
  60 s doubling to a 900 s ceiling. At that ceiling an exhaustive 4-digit
  search is on the order of years of continuous presence at the panel, which
  is the property that makes a short secret acceptable *here* and would not
  make it acceptable on a network-reachable route.
- The lockout is keyed to the panel as a whole, not per PIN, so an attacker
  cannot halve the work by alternating between the two PINs.
- A locked-out panel still shows the Dashboard and still stops a firing
  (item 9). Lockout must never be reachable as a denial of a stop.

Because the administrator PIN can authorise a factory reset from the panel, it
is worth noting explicitly that the *credential reset* gesture (item 10) does
**not** accept it — that gesture requires physical E-stop assertion, not a PIN.

*Acceptance:* `UI_TEST_CMD_LIST_TAP_TARGETS` reports all 12 keypad keys plus
`OK` and `Cancel` as named, non-hidden tap targets; a scripted
`kiln_ui_click_by_name()` sequence entering a correct PIN unlocks, and a wrong
one does not; `capture_lcd.ps1` plus numeric pixel sampling confirms the
overlay fits inside the panel with no scrollbar and introduces no colour
outside the theme set.

---

## 8. The inactivity lock and the 10-second prompt

**Two independent timeout values — settled by the owner**, one for the web GUI
and one for the LCD, both on the password page, both stored in `auth_policy` as
`web_timeout_s` and `lcd_timeout_s`. This was decided against the first
draft's recommendation of a single shared value; the practical argument for two
is that the panel is physically at the kiln and the browser may be anywhere, so
the sensible values genuinely differ. Range 1–60 minutes plus `never` for each.
`never` is meaningful and must work: an operator who wants the panel
permanently unlocked while standing at the kiln should not have to disable auth
to get it.

**The 10-second stay-unlocked prompt applies to each interface separately**,
against that interface's own timeout. The two never interact: web activity does
not extend the LCD session, and a tap on the panel does not extend a browser
session.

**What counts as activity.** Web: any request actually ALLOWed against
`ROUTE_TIER_USER`/`ROUTE_TIER_ADMIN`
(`http_auth_decision_counts_as_activity()`) — so a dashboard polling
`/api/status` from an idle browser tab, or the lock prompt's own
`GET /api/auth/session` status poll (kept `ROUTE_TIER_OPEN` for exactly
this reason), does **not** keep an admin session alive, which is the whole
point; only the explicit `POST /api/auth/session/extend` (or any other
ordinary USER/ADMIN action) does. LCD: any touch event delivered to LVGL,
including a touch that only scrolls or is swallowed by a backdrop.

**What locking does.** The session drops to the unauthenticated tier and the
interface returns to the Dashboard. The display does not blank — backlight
behaviour stays entirely `display_power_cfg`'s business, which already has its
own separate idle timeout and keep-on-while-firing setting. **A firing is
never interrupted by a lock**; the executor does not read auth state at all.

**The 10-second prompt.** At `timeout - 10 s`, both interfaces show a "Stay
unlocked" prompt. On the LCD it is a `ui_confirm` dialog with a 10-second
countdown in its body; on the web it is a small non-blocking corner widget
in `app.js` (`.kc-lock-prompt`), deliberately not a modal — see below.
Accepting posts `POST /api/auth/session/extend`, which the shared
activity-touch mechanism extends by the full timeout the same as any other
USER-tier request. Ignoring it locks at expiry (enforced server-side
regardless of whether the prompt was ever shown or clicked) and the widget
hides itself once the next poll reports the session gone.

**The prompt must never block a safety action.** The LCD prompt is a normal
`lv_layer_top()` modal, so the Stop control is behind it for those 10 seconds
— that is unacceptable, so the LCD prompt is shown **only when a gated action
is possible in the first place**, and it is dismissed instantly by any touch
outside it, not only by its own buttons. On the web the widget is a small
fixed-position corner element, not a full-screen overlay: the dashboard
behind it, including Stop, stays fully interactive the whole time. State
plainly: no code path in this feature may take a lock that a stop or trip
path also takes.

**Should locking be suspended during a firing? No — but the Dashboard stays
live, which is what the operator actually wants.** Recommendation and
reasoning: the Dashboard is unauthenticated by requirement, so an operator
watching a firing sees everything they were watching before the lock, and Stop
stays reachable (item 9). Suspending the lock during a firing would mean an
admin session stays live for the multi-hour period when the kiln is at
temperature and an accidental config change does the most harm. So the lock
runs during a firing exactly as it does otherwise.

**Timeout bookkeeping cost.** One `last_seen_ms` per slot, already counted in
item 4's 700 bytes. The sweep is a comparison per slot on an existing periodic
tick — no new task, no new timer, no stack growth.

*Acceptance, landed.* The boundary math (`timeout - 1 ms`/`timeout + 1 ms`,
the prompt window opening at `timeout - 10 s`, and touch extending by the
full timeout) is covered by `test_web_auth.c` against
`web_auth_session.c`/`web_auth_session_in_prompt_window()`. The HTTP-layer
wiring — that any ALLOWed USER/ADMIN request counts as activity, that the
status poll alone never does (it is `ROUTE_TIER_OPEN`, not a special-cased
branch), and that touching an already-expired session can never revive it —
is covered by `test_http_session_iface.c` and
`test_http_auth_enforce.c`'s `test_decision_counts_as_activity()`. Each
assertion was negative-tested: the guarded production line was broken,
confirmed RED, restored by hand with a matching `git hash-object`, and
reconfirmed GREEN after a forced full rebuild.

---

## 9. Safety interaction

**These paths bypass authentication unconditionally, in every auth state —
enabled, disabled, locked, or mid-prompt:**

- `POST /api/profile_exec/stop`. Nominally USER, but a stop is never refused
  for lack of a session. A stop makes the kiln safer; there is no threat model
  in which blocking it is the safe choice, and `app.js` already calls it from
  the unauthenticated dashboard.
- `POST /api/zones/current_sweep/abort`, `POST /api/autotune/abort`, and
  `POST /api/diagnostics/danger/stop`. Nominally ADMIN, but each one aborts an
  operation that is actively driving relays — the current-sweep task, a
  running autotune (via `force_relays_off()`), and the danger-mode relay
  window (via `kiln_io_owner_command_all_relays_off()` plus releasing
  heat-enable), respectively. The same reasoning as the stop route above
  applies without modification: an expired or session-less client must be
  able to end any of these, since `/api/profile_exec/stop` does not own them
  and cannot substitute for aborting them.
- The LCD Stop control. `ui_home_fire_btn_cb()` must never route through the
  keypad. On the LCD the Start/Stop control is one merged widget that reads
  "Stop" only while RUNNING or PAUSED — so the gate is: **that widget demands
  a PIN when it would Start, and never when it would Stop.**
- The physical E-stop, every Pico-side guard, every trip, and the
  `heat_enable` release path. None of these traverse HTTP or the UI at all, so
  they are unaffected by construction — but it is stated here so a future
  reader does not have to re-derive it.

**Nothing in this design can wedge the safety link or the relay owner
module.** The enforcement pre-handler runs entirely inside the httpd task and
touches only the session table's own mutex. It takes no safety lock, no
`heat_enable` claim, and no relay-owner lock, and it never calls into
`safety_link_*` or `thermo_owner`. The KDF's 30–60 ms runs on the httpd task
and cannot delay the safety poll, which is a separate task. The
`abs_max_temp_c` sent to the Pico is untouched by anything here, and no path in
this feature can leave the Pico unarmed — arming is not an authenticated
operation and must not become one.

`POST /api/safety/clear_trip` is ADMIN, deliberately. Clearing a trip is not a
safety action; it re-enables heat after one.

*Acceptance:* a host test drives the enforcement function with every
combination of {auth off, auth on + no session, auth on + locked, auth on +
user, auth on + admin} against `/api/profile_exec/stop` and asserts ALLOW in
all five; a bench check confirms the LCD Stop button works with the LCD locked
and a firing running.

**Status, 2026-09-17 — both LCD and HTTP sides done.**

LCD side: `ui_home_fire_btn_cb()` (`firmware/KilnFW/App/drivers/ui/ui_page_home_actions.c`)
already matches this section exactly — its RUNNING/PAUSED branch calls
`ui_home_show_stop_confirm()` directly with no PIN gate, and only its Start
branch goes through `ui_lcd_lock_run_gated()`. This is now enforced
mechanically: `tools/check_stop_path_never_gated.ps1` fails the build if a
future change adds a gate call to the Stop branch (or removes the Stop call
entirely, or strips the Start gate so the check can no longer tell the two
branches apart), and its negative test
(`firmware/KilnFW/App/test/test_check_stop_path_never_gated.ps1`) proves the
check catches a PIN gate actually moved onto the Stop branch — verified
against the real production file, restored by hand, sha256-confirmed
unchanged.

HTTP side, defect found and closed (section 5's enforcement point): the
first implementation of `http_auth_check()`/`kiln_http_prehandler()`
classified `POST /api/profile_exec/stop` as `ROUTE_TIER_USER` — its
plan-nominal tier — and had no code path that bypassed the session check
for it, so an auth-on, session-less or locked-out client got
`DENY_NO_SESSION` on stop. Only 3 of this section's 5 required combinations
allowed it. Fixed by introducing a fourth tier, `ROUTE_TIER_SAFETY_REDUCE`
(`route_tier_table.h`), assigned to `/api/profile_exec/stop` in place of
`ROUTE_TIER_USER`, and handled in `http_auth_check()`
(`http_auth_enforce.c`) as an unconditional ALLOW — checked before the
no-session/no-role logic, same position as `ROUTE_TIER_OPEN`. Deliberately
not a URI string comparison inside the enforcement function or the
pre-handler: a hardcoded path match would be a second, independently-
maintained record of which routes bypass auth, alongside
`route_tier_table.h` — exactly the reset-one-side-of-a-pair shape
CLAUDE.md documents four prior instances of. Routing it through the tier
table instead means `check_route_tier_coverage.ps1` keeps covering this
route the same as every other one, and there remains exactly one place
(`route_tier_table.h`) that answers "which routes bypass auth entirely."
`firmware/KilnFW/App/test/test_web_auth_safety_interaction.c` and
`test_http_auth_enforce.c` now cover all 5 acceptance-list combinations
against the real route table, driven through `ROUTE_TIER_SAFETY_REDUCE`
rather than a bare literal.

---

## 10. The physical credential reset

**The owner's later wording — "Estop then tap 1 corners of the lcd" — is the
gesture, and the earlier "press stop twice" sketch is dropped.** Dropping it
was forced by the code: the LCD has no Stop button independent of the merged
Start/Stop widget, and that widget reads "Stop" **only while a firing is
RUNNING or PAUSED** — the exact condition under which the gesture must be
impossible.

**Owner-confirmed 2026-09-16 (closes item 13's open assumption):** "tap 1
corners" means tap each of the four corners once, in order — **top-left,
top-right, bottom-left, bottom-right**.

**The gesture:**

1. E-stop asserted (`safety_link_status_t.flags & SAFETY_FLAG_ESTOP`), and
2. no firing in progress, and `heat_enable_is_granted()` false, then
3. each of the four LCD corners tapped once, in order, within 10 seconds —
   an out-of-order or missed corner resets the sequence — which arms the reset
   and shows a plain-text banner in the existing `s_ui_home_trip_strip` —
   "AUTH RESET ARMED — confirm within 30 s", then
4. a `ui_confirm` dialog whose confirm button must be pressed. One deliberate
   confirmation, not a second ambiguous tap.

Why this shape: the E-stop precondition makes it unreachable remotely by
construction (E-stop is a physical contact read by the Pico, not a writable
field), an ordered four-corner sequence is not hittable by accident, and the
heat and firing gates satisfy "impossible during a firing or while heat is
enabled" without depending on a widget's caption. The E-stop and no-heat
preconditions and the explicit confirm dialog are this plan's safety envelope
around the gesture; the corner taps replace only the tap-and-press part.

**New work this needs, and what it no longer needs.** Corner taps are cheaper
than the graph tap this plan first proposed: **no `LV_OBJ_FLAG_CLICKABLE` on
the chart and no chart click handler are required**, and the chart stays a
draw-only widget. What is needed is four small transparent hit zones (about
40x40 px) at the corners of the Dashboard, non-visual, adding no colour, plus a
small accessor for the E-stop bit — the bit is already populated and cached on
the ESP but **has no consumer anywhere**, so that is a read, not new plumbing.
One bench item: confirm no existing Dashboard control already occupies those
four 40x40 regions, and shrink the zones rather than steal a tap if one does.

**What the reset actually does:** restores a default administrator credential
with `must_change = true`, so the next login is forced to set a new password
before anything else, and clears the administrator's LCD PIN record so it
reads as not-configured. It does **not** disable authentication, does not
touch the `user` password or the `user` LCD PIN, and does not clear any
config. Reasoning: the failure being recovered from is a forgotten
credential, and a forgotten LCD PIN is exactly that failure — the gesture
must be able to recover from it the same way it recovers a forgotten web
password — while silently unlocking the whole board is still a bigger hole
than the one being closed, which is why the `user` role's credentials are
untouched. (2026-09-17 adversarial review, commit `1179e2d3`: the original
implementation cleared only the administrator web password, leaving a lost
administrator LCD PIN unrecoverable by this gesture — fixed in
`web_auth_store_clear_for_physical_reset()`.)

**Logging:** `ESP_LOGE` at arm, at confirm and at cancel, naming E-stop state
and firing state, in the same loud style as the `boot_button_ota_bypass_active()`
branch. One line each; this is a rare event and must be findable afterwards.

*Acceptance:* a host test on the pure gate predicate asserts the gesture is
refused with E-stop clear, refused with a firing running, refused with heat
enabled, and armed only with all three conditions met; that an out-of-order
corner sequence does not arm; and that arming expires after 30 s. On the bench,
a `TOUCH_CMD_INJECT` sequence at the four corner **coordinates** performs the
gesture with no human present — coordinate injection needs no widget name, so
this is testable headlessly without naming the hit zones — and
`capture_lcd.ps1` confirms the armed banner is on screen.
*Negative test:* invert the E-stop condition in the production predicate,
confirm the "refused with E-stop clear" assertion goes RED, restore by hand.

**Status, 2026-09-18 — hardware-gated, not merely not-done.** The gesture and
its host tests have landed (section 0); what is outstanding is the bench
exercise, and no software change can unblock it. Step 1 of the gesture
requires `SAFETY_FLAG_ESTOP` actually asserted, and on this fixture the E-stop
jumper is **fitted**, which means the normally-closed loop is closed and
E-stop is *not* asserted — GPIO9 reads low and S7 correctly stays quiet
(`firmware/SaftyFW/docs/HARDWARE.md` §5). **What the owner must do:**
physically break the E-stop loop for the duration of the exercise — unplug
that jumper, or open a real E-stop contact fitted in its place — and confirm
the flag reads asserted before running the four-corner sequence. The bit is
read by the RP2040 from a physical contact and is not writable, so it cannot
be forced from a tool.

---

## 11. Auth disabled, first boot, and field upgrade

**Auth defaults to OFF for both interfaces.** Owner decision; planned for, not
around.

**With auth off, every tier collapses to full access.** This must be the
normal, correct behaviour of the tier model, not a special case bolted on: the
enforcement pre-handler's first check is `web_enabled`, and if it is false it
returns ALLOW immediately, before any session lookup. The same for
`lcd_enabled` and the keypad. Consequence, stated plainly: **a board with auth
off is exactly as open as the board is today.** That is the point of the
default, and it is why the setup wizard must ask.

**One named exception: the nine OTA-family routes are never ungated.** With web
auth off they fall back to today's AP-password challenge rather than becoming
open, because anything else would loosen the only routes that are authenticated
today. Full reasoning in item 2b. "Exactly as open as today" therefore means
precisely that — today's board already requires a credential for OTA, factory
reset, `sw_reset` and the `cfgfs` format, and so does this one.

**"No credential set" is not an error state.** It is the shipped default. A
board with auth off and no credential is fully functional. Enabling auth is
refused unless a credential for that interface exists — which is the only
place the two interact, and it is what prevents locking the owner out.

**Field upgrade: no regression, explicitly.** An upgraded board has no
`auth_policy` record, which reads as `{ web_enabled: false, lcd_enabled:
false }` — identical to the behaviour it had before the upgrade. Nothing
changes for that board until someone deliberately turns auth on. No migration
step, no schema bump on any existing blob, and **`ZONES_CFG_VERSION` stays at
26.**

**The two interfaces are independent.** Web on with LCD off, LCD on with web
off, both, or neither are all valid.

**May auth be disabled during a firing? Yes.** Recommendation and reasoning:
disabling auth only ever widens access, and the safety argument does not rest
on authentication — it rests on the Pico's independent guards. Refusing the
change mid-firing would leave an operator locked out of their own kiln for
hours with the kiln hot, which is the worse outcome. **Enabling** auth during
a firing is also permitted, for the same reason, and drops no session.

**What happens to live sessions on a policy change.** Disabling auth: sessions
become irrelevant but are kept, so re-enabling does not surprise anyone.
Enabling auth: every existing session is cleared, because sessions created
while auth was off carry no proven credential and must not be promoted into
authenticated ones.

**The setup wizard should recommend enabling it.** It presents an "enable
authentication" step that is skippable, defaults the checkbox to off to match
the shipped default, and states in one line what is at stake: with it off,
anyone on the network can start a firing or change a safety limit. Recommend
without overriding the owner's default.

*Acceptance:* a host test asserts an absent policy record reads as both-off
and that every route in the table returns ALLOW in that state; that enabling
web auth with no administrator credential is refused; that enabling clears all
sessions and disabling does not.
*Negative test:* make the absent-record default read as `enabled`, confirm the
field-upgrade assertion goes RED, restore by hand.

**2026-09-17 adversarial review, commit `1179e2d3` — two defects in this
section's own wiring, both fixed:**

1. "Enabling web auth with no administrator credential is refused" was true
   at the store layer (`web_auth_policy_check_transition()` correctly checks
   for the ADMINISTRATOR password specifically) but the HTTP-facing backend,
   `web_auth_backend_set_policy()`, re-derived its own looser rule — refusing
   only when NEITHER the admin nor the user password was set. Enabling with
   only a user password configured therefore passed the backend's check,
   left `admin_bootstrap_needed` true, and denied every ADMIN route,
   including the undo route itself. Fixed by making the backend call the
   store's own `web_auth_policy_check_transition()` rather than
   re-implementing the rule — there is now exactly one place that decides
   transition legality.
2. "Enabling clears all sessions" had no consumer: `web_auth_policy_check_
   transition()`'s `out_clear_web_sessions`/`out_clear_lcd_session` outputs
   were computed and asserted only in `test_web_auth_store.c`, and the
   backend's `invalidate_sessions_for_role()` was an explicit no-op. Fixed
   by wiring both outputs to the real session state in
   `web_auth_backend_set_policy()` (`web_auth_table_destroy_all()` against
   `http_session_table()`, and `ui_lcd_lock_force_lock()` for the LCD side).

**Status, 2026-09-17: Landed, verified against origin/main in a clean
worktree.** All three acceptance clauses hold non-vacuously in production
code with real callers, plus the field-upgrade negative test:
- Absent-policy-both-off: `web_auth_store.c`'s `WEB_AUTH_LOAD_ABSENT` case
  collapses to `false`; proven by `test_web_auth_store.c`'s
  `test_policy_absent_is_off()`. Route-table coverage strengthened: added
  `test_every_real_route_allows_with_auth_off()` in
  `test_http_auth_enforce.c`, which now walks the real `kRouteTierTable`
  (via `ROUTE_TIER_TABLE_COUNT`) rather than a hand-written tier list.
- No-admin-credential refusal and enable-clears/disable-preserves-sessions:
  both hold via `web_auth_policy_check_transition()`, with
  `web_auth_backend_set_policy()` delegating to it and wiring
  `web_auth_table_destroy_all()`/`ui_lcd_lock_force_lock()` (the two
  2026-09-17 defects above), and a real production call site at
  `security_http_core.c`'s `vt->set_policy(&req->policy)`.
- `web_enabled`/`lcd_enabled` are the literal first checks in
  `http_auth_check()` (`http_auth_enforce.c`), ALLOW before session lookup.
- The nine OTA-family routes fall back to AP-password/HMAC challenge with
  auth off (`ota_http.c`), never becoming open; confirmed both by reading
  and by collateral RED in `test_ota_http.c` during the negative test below.
- Negative test performed: flipped `WEB_AUTH_LOAD_ABSENT` to `true` in
  `web_auth_store.c`, confirmed RED in `test_web_auth_store.c` (and
  collateral RED in `test_ota_http.c`), hand-restored via `Edit`, confirmed
  empty `git diff` and matching `git hash-object`, deleted `build/`, forced
  a full rebuild, confirmed GREEN.
- Gap found and closed: the setup wizard had no "enable authentication"
  step at all. Added step 13 to `setup_wizard_page.html`'s `WIZARD_STEPS`
  (skippable, checkbox default off, one-line stakes statement, links to
  `/settings/security` rather than duplicating that UI), covered by a new
  test in `test_setup_wizard.js`.
- `ZONES_CFG_VERSION` unchanged at 26 (`zones_config_json.h:63`).
- Full suite (`tools/run_all_checks.ps1`, clean rebuilt worktree): 106
  passed, 0 skipped, 1 failed (107 total, matches expected tip) — the one
  failure was `check_ui_responsive_sweep.ps1` (headless browser
  `Page.loadEventFired` timeout), confirmed flaky and unrelated by an
  isolated re-run that passed standalone.

---

## 12. Testing

**Landed.** The host-test matrix this section calls for is in place; see the
section 12 bullet in section 0 for what it covers and how it was
negative-tested. What remains below is the bench work, which no host test can
stand in for.

**Host tests** (`firmware/KilnFW/App/test/test_web_auth.c`, added to
`build_host_tests.ps1`'s `$sources` with `$totalExpected` bumped from 47 to 48
— the count is a hard gate and a mismatch is a refusal, not a warning). The
new module must be host-buildable, which means the same split `ota_auth.c`
already uses: the pure logic — tier resolution, session table, timeout
arithmetic against an injectable clock, strength validation, the gesture gate
predicate, the auth-off collapse — lives in a file with no `esp_http_server.h`
dependency, and only the header parsing and response sending stay in the HTTP
layer.

**Every test above states the behaviour it proves, and a test that passes
against pre-change code proves nothing.** So each item's negative test
sabotages the production function, confirms RED, hand-restores (never
`git checkout --`; this tree is shared), confirms GREEN, and **forces a full
rebuild** so no poisoned binary survives the restore.

A `BUILD FAILED` or "the API does not exist yet" failure is **not** a
behavioural test result and must not be recorded as one.

**What genuinely needs the bench:**

- The keypad overlay fits 480x320 with no scrolling and adds no colour —
  `capture_lcd.ps1` plus numeric pixel sampling against a bezel reference.
- The reset gesture end to end, with a real E-stop asserted.
- LCD Stop working while the LCD is locked and a firing is running.
- Internal DRAM low-water re-measured after the feature lands, compared
  against the 27,015 B baseline.
- The session table under real concurrent browsers, against the ~6-socket
  accept-mailbox ceiling.

**Blocked on the owner, 2026-09-18.** Live verification of web auth against
the board needs `KILNCTL_WEB_USERNAME` and `KILNCTL_WEB_PASSWORD` set as
Windows *user* environment variables: the PcTools HTTP clients read the
credential from those two variables and from nowhere else
(`tools/PcTools/src/kilnctrl/http_auth.py:56`-`57`), so while they are unset
every authenticated request from the tooling goes out unauthenticated and the
policy cannot be exercised. No credential value belongs in any file in this
repository — only the variable names.

---

## 12b. Credentials versus the config package, OTA and factory reset

**Recommendation: credentials are excluded from the config package entirely.**
They live in their own NVS records (item 2), are never serialised into an
export, and are never read by an import. The alternative — carrying them in
the package and merging on restore — is rejected.

Why exclusion, not merge:

- **A package is a file that travels.** It is exported to disk and moved
  between boards. Password hashes inside it means the credentials leak
  wherever that file goes, and a package imported from another kiln would
  silently install *that* kiln's passwords. The import path already has
  precedent for "this field does not travel": commit `3efdac65` added the
  board-identity comparison that forces `calibrated = false` and clears
  `i_normal_a[]` when `source_board` does not match the running board.
- **Credentials are per-installation, not per-configuration.** Restoring last
  week's config should not roll back who may log in.
- A merge rule would need a conflict policy for every restore, and every such
  policy has a wrong answer: prefer-package leaks, prefer-board makes the
  package's copy dead weight that still leaks.

Exclusion is structurally cheap here: `backup_export.c` hand-writes named
sections and contains **no generic NVS enumeration** (zero `nvs_entry_*` calls),
so a credential record cannot be swept into an export by accident. It is not in
the file because nothing writes it there, not because a filter removes it.

### One property, not several rules: the record lives outside config storage

Owner requirement: **credentials are board-scoped, not config-scoped.** Every
config slot shares one set of credentials; there is no per-slot password. The
plan states that as a single structural property rather than as a list of rules
that could drift apart:

> The credential record lives in its own namespace on its own partition
> (item 2), and **no config operation reads or writes it.**

"Does not travel in a backup" and "survives a slot switch" are then the same
fact, not two rules. Every operation that touches configuration, and what it
does to the credential record:

| Operation | Route / entry point | Effect on credentials |
| --- | --- | --- |
| Save current config to a slot | `POST /api/kiln_configs/save` | none — writes `kiln_cfg` only |
| Clone a slot | `POST /api/kiln_configs/clone` | none |
| **Apply / switch slot** | `POST /api/kiln_configs/apply` | **none — this is the slot switch, and the one credential set stays in force across it** |
| Rename a slot | `POST /api/kiln_configs/rename` | none |
| Delete a slot | `POST /api/kiln_configs/delete` | none |
| Export one slot's package | `GET /api/kiln_configs/export` | not in the file |
| Import a package, including a foreign one | `POST /api/kiln_configs/import` | none — never opened, so a foreign package cannot install another kiln's passwords |
| Whole-board backup export | `GET /api/backup/export` | not in the file (`backup_export.c` has no generic NVS enumeration) |
| Whole-board restore | `POST /api/backup/import` | none |
| Autosave from live | `kiln_cfg_store_autosave_from_live()` | none |
| Quarantine, and quarantine clear | `kiln_cfg_store_quarantine_clear()` | none |
| Zones config, prefs, profiles writes | `kiln_nvs` / `profiles_nvs` writers | none — different partitions |
| `cfg` LittleFS dual-write, and its format | `cfg_fs_*` | none — credentials are NVS-only (item 2) |
| Factory reset, any of the four scopes | `POST /api/factory_reset` | none — see case 5, answered explicitly |

The precedent for a field that deliberately does not travel is `3efdac65`'s
board-identity check, which forces `calibrated = false` and clears
`i_normal_a[]` when a package's `source_board` does not match the running board.
Credentials go one step further: there is no field to neutralise on import,
because there is no credential field in the package at all.

*Acceptance:* one host test walks that table — for each operation it sets
credentials, performs the operation, and asserts all three records are
byte-identical afterwards. The slot-apply row must be a real switch between two
populated slots, not a no-op apply of the already-active slot.
*Negative test:* add a credential section to the export writer, confirm the
"export contains none of the set password's bytes" assertion goes RED, restore
by hand, confirm GREEN, and force a full rebuild.

### Case-by-case outcomes

1. **OTA upgrade to newer firmware.** Credentials survive untouched. An OTA
   writes an app partition; it does not touch NVS.
2. **OTA rollback to older firmware.** This is the case where the storage
   choice matters, so concretely: the older firmware finds a `web_auth` /
   `auth_policy` record whose `version` is higher than it knows. **It must not
   read that as "no credentials set".** The rule: a record that is present but
   unreadable means **authentication stays in whatever enabled state
   `auth_policy` last recorded, and login is refused for that interface** —
   fail closed, not open, so a rollback never silently opens the kiln. The
   owner is not locked out either, because the physical reset gesture (item 10)
   is available and is the documented recovery, and rolling forward again
   restores the record intact since **flash is left untouched**. Firmware
   predating this feature entirely ignores the keys, which is the auth-off
   behaviour it already has. This is the same shape as the documented
   `zones_cfg` rollback hazard, with the opposite default: zones falls back to
   firmware defaults, credentials must not.

   **Ordering hazard, created by item 2b and resolved here.** Folding OTA into
   administrator auth means an OTA route is now gated by the very record an OTA
   must preserve. If an unreadable credential record refused the OTA routes the
   way it refuses a web login, a rollback past a credential-schema bump would
   remove the means of repair: the operator could no longer OTA forward to the
   firmware that understands the record, leaving only the physical gesture or
   JTAG. So the two rules differ deliberately — **a web login is refused when
   the record is unreadable; the nine OTA-family routes instead fall back to
   the AP-password challenge** (item 2b's auth-off path), which needs no
   credential record at all. Refusing a web login costs nothing that is not
   recoverable; refusing OTA removes the repair path. The
   `boot_button_ota_bypass_active()` physical-presence window remains beneath
   both as the last resort.
3. **Config backup / export.** Credentials are **not** in the file. State it on
   the backup page in one line — "Backup does not include usernames,
   passwords or the LCD PIN. Those stay on this board." — so an operator
   restoring onto a replacement board is not surprised, and knows they must set
   credentials again on new hardware.
4. **Config restore, swap, or a package from a different board.** Credentials
   are untouched by the restore, in all three cases, including a foreign
   package. The import path never opens the credential keys.
5. **Factory reset and config erase — answered explicitly, because the owner
   expectation cuts both ways.** A factory reset **does not clear
   credentials**, in any of the four scopes. None of them can: each erases only
   the partitions its own table row names (`wifi_nvs`, `kiln_nvs`,
   `profiles_nvs`), and the credentials are on `nvs` (item 2). With that
   placement this is a structural outcome, not a filter someone has to remember
   to maintain. "all" still formats the `cfg` partition and restores builtin
   profiles; the three credential keys stand.

   **Why not clear them.** Making "all" clear credentials would create a
   *second* credential-reset path alongside the corner-tap gesture — and a
   worse one, because `POST /api/factory_reset` is remote where the gesture is
   deliberately not. Every precaution in item 10 (E-stop asserted, no firing,
   heat not enabled, explicit confirm) would have to be replicated on it, or the
   protection would simply have a second door.

   **The counter-expectation, acknowledged rather than dismissed.** An owner may
   reasonably read "factory reset, scope: all" as clearing everything. Two
   things answer that instead of leaving it implicit: the danger-zone copy for
   "all" must state in one line that login credentials are **not** included and
   name the two ways to clear them; and a separate, explicitly labelled "Clear
   login credentials" action sits beside it as its own ADMIN route. That route
   is not a weakening — it requires an already-authenticated administrator, who
   can change the passwords anyway — and it is not a substitute for the gesture,
   which exists for the case where nobody can log in at all.

**Schema versions:** the credential records carry their own
`WEB_AUTH_STORE_VERSION`, separate from every config version precisely so that
a config-schema bump and a credential-schema bump can never force each other.
**`ZONES_CFG_VERSION` stays at 26.** Credentials are NVS-only and not
dual-written to `cfg` (item 2), so the inert, unformatted `cfg` partition is
irrelevant to all five cases above.

*Acceptance:* a host test asserts an export's JSON contains none of the three
credential key names and none of a set password's bytes; asserts an import of
a package carrying injected credential fields leaves the stored records
byte-identical; asserts a version-too-new record resolves to "refuse login",
never to "auth off"; asserts each of the four factory-reset scopes leaves the
credential records intact.
*Negative test:* make the version-too-new path return "no credentials set",
confirm the rollback assertion goes RED, restore by hand, confirm GREEN, and
force a full rebuild.

**Status, 2026-09-17 — landed (the "Clear login credentials" ADMIN route).**
The rest of this section (exclusion from export/import/backup/slot-apply/
factory-reset, and the fail-closed OTA-rollback handling) was already landed;
the one remaining unmet acceptance item was the explicitly labelled ADMIN
route this section calls for, plus the danger-zone disclosure line, and both
now ship:

- `web_auth_store.h`/`.c` — `web_auth_store_clear_all_credentials()`: clears
  both roles' web passwords AND both roles' LCD PINs (both `web_auth`/`lcd_auth`
  blobs), read-back verified the same way as
  `web_auth_store_clear_for_physical_reset()`, and deliberately leaves
  `auth_policy` untouched. This is a *different* mechanism from that
  physical-reset function (administrator-only, gesture-gated) — this one is
  both roles, reached only through an authenticated ADMIN session.
- `security_backend.h`/`security_backend_web_auth.c`/`security_backend_placeholder.c`
  — a `clear_all_credentials` vtable entry, mapping a failed read-back to
  `SECURITY_ERR_STORAGE` (500), never a false success.
- `security_http_core.h`/`.c` — new `SECURITY_CMD_CLEAR_CREDENTIALS`, gated by
  the same ADMIN-only check every other command on this page already has, and
  on success invalidating both `SECURITY_ROLE_ADMIN` and `SECURITY_ROLE_USER`
  sessions (every session for both roles must drop, not only the caller's).
- `security_http.c` — reuses the existing `POST /api/auth/security` route
  (already `ROUTE_TIER_ADMIN`) with `cmd=clear_credentials`; no new route, no
  route-tier-table change.
- `net/security_page.html` — a labelled "Clear login credentials" button,
  separate from the policy/password controls above it, posting `cmd=clear_credentials`.
- `http/settings_page.html` — the danger-zone copy now states in one line that
  login credentials are not included in any of the four reset scopes and names
  both ways to clear them (this route, and the physical gesture).

*Negative test performed:* sabotaged
`web_auth_store_clear_all_credentials()` to skip clearing the `user` password
record (a "reset one side of a pair" shape), confirming RED —
`kilnctl_host_tests_web_auth_store.exe` dropped from 149/149 to 144/149, with
5 named `FAIL` lines pointing at `test_web_auth_store.c:514/520/522/524/537`.
Restored by hand with Edit; `git diff` on `web_auth_store.c` showed only this
session's real additions, and `git hash-object` matched the pre-sabotage
blob exactly (that blob was `web_auth_store.c` as committed in `8714ee31`;
the file has legitimately moved on since -- most recently `ca7a7d31` -- so
this is recorded as the point-in-time evidence it was, not a citation meant
to keep grading against HEAD's current blob). A forced full rebuild (the
`App/test/build` directory deleted first) then reported "all 50 host test
executables built and passed", with `kilnctl_host_tests_web_auth_store.exe`
at 149/149 and the combined `kilnctl_host_tests.exe` at 8789/8789.

---

## 13. Owner decisions — settled

Settled 2026-09-16. Two went against this plan's first-draft recommendation;
the plan follows the owner's decision and records the cost rather than
re-arguing it.

1. **LCD gets two PINs, not one tier** — a `user` PIN and an `administrator`
   PIN, both 4–8 digits. Decided against the draft's recommendation. The
   accepted tradeoff and the rate-limiting that makes it tolerable are recorded
   in item 7.
2. **Separate lock timeouts for web and LCD**, independently settable, each
   with its own 10-second prompt. Decided against the draft's recommendation
   (item 8).
3. **OTA uses the administrator credential**, with no OTA password of its own
   (item 2b).
4. **The physical credential-reset gesture (item 10) is confirmed**: "E-stop
   asserted, then tap each of the four LCD corners once — top-left, top-right,
   bottom-left, bottom-right, in that order." No longer an assumption.
