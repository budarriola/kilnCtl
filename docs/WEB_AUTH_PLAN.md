# Web + LCD authentication plan

> **Status:** plan · **Opened:** 2026-09-16. Nothing here is implemented. No
> board was flashed, no heating run was performed, and no `.kicad_*` file was
> touched while writing it.

Username/password authentication and roles for the web GUI, a numeric PIN for
the LCD, an inactivity lock on both, and a physical-presence credential reset.

---

## 0. Read this first: what already exists, and what does not

| Already exists | Where |
|---|---|
| A complete, host-tested challenge–response auth state machine: nonce issue/check/invalidate, single-use, 30 s expiry, constant-time compare, per-context escalating lockout | `firmware/KilnFW/App/drivers/net/ota_auth.h`, `ota_auth.c`, `firmware/KilnFW/App/test/test_ota_auth.c` |
| A request-level wrapper that parses the `X-Ota-Mac` header, hex-decodes it, verifies, and sends the 400/403 itself | `ota_http_authenticate_request()`, `firmware/KilnFW/App/drivers/http/ota_http.c` |
| PSA crypto initialised at HTTP start, HMAC-SHA256 available and linked | `ota_http_start()`, `psa/crypto.h` includes in the `ota_http_*` family |
| A physical-presence auth bypass with precedent for logging it loudly | `boot_button_ota_bypass_active()` branch in `ota_http_verify_request()` |
| A designed session layer — random 32-byte tokens, a fixed 8-slot RAM table, token hashes compared constant-time, LRU eviction, sliding idle plus hard cap, `HttpOnly; SameSite=Strict` cookie, IP binding, RAM-only across reboot | `firmware/KilnFW/docs/UI_PLAN.md`, section "Open, explicitly deferred by the owner: auth / session layer" |
| A shared LCD modal built on `lv_msgbox` parented to `lv_layer_top()`, so it consumes none of a page's no-scroll content budget | `firmware/KilnFW/App/drivers/ui/ui_confirm.h` |
| Headless LCD tap injection and tap-target discovery, already wired to the UART bridge | `kiln_ui_click_by_name()`, `UI_TEST_CMD_LIST_TAP_TARGETS`, `UI_TEST_CMD_CLICK_BY_NAME`, `TOUCH_CMD_INJECT` |
| Live E-stop state, already arriving from the Pico every frame | `safety_link_status_t.flags & SAFETY_FLAG_ESTOP`, packed by `link_frame_pack_status()` |
| A persisted, versioned-blob NVS pattern with a compile-time 15-char key guard | `firmware/KilnFW/App/drivers/persist/nvs_key_check.h`, `kiln_cfg_store.c` |

**The session design in `UI_PLAN.md` is not re-planned here.** Items below
reference it and change it only where the owner's new requirements force a
change — and it carries a "Do not implement" marker that this plan supersedes.

What does **not** exist, and is the whole of the work below:

- Roles. Nothing in the tree distinguishes a `user` from an `administrator`.
- Any authentication on any non-OTA route. Exactly five routes authenticate
  today: `POST /api/ota/esp`, `/api/ota/esp/rollback`,
  `/api/ota/esp/recovery_exit`, `/api/ota/esp/boot_guard_reset`,
  `/api/ota/pico`, `/api/ota/pico/rollback`, `/api/factory_reset`,
  `/api/cfgfs/format_confirm` and `/api/sw_reset`. Every other mutating route
  — including `POST /api/zones`, `/api/profile_exec/start`,
  `/api/safety/clear_trip` and `/api/diagnostics/danger/relay` — is open.
- A stored web credential. The only password in the firmware today is the AP
  password, returned in plain text by `wifi_prov_get_ap_password()` and
  deliberately published by `/status`. It cannot satisfy "hashed, salted,
  never recoverable" and is not reused as the web credential.
- A password-setting page, an LCD PIN, an inactivity lock, a fail-closed
  enforcement point, and a physical reset gesture.

### Two facts that shape the design

1. **`GET /api/ota/challenge` and the `X-Ota-Mac` scheme stay exactly as they
   are.** The OTA family keeps its own per-request HMAC challenge regardless of
   whether a session is present. This layer sits alongside it and never
   replaces it — so an OTA push still needs the AP-password-derived MAC even
   from an authenticated administrator session.
2. **Auth defaults to OFF for both interfaces** (owner decision). The tier
   model must behave correctly in that configuration, not treat it as an error.
   See item 11.

---

## 1. Route classification — three tiers

Tier names: **OPEN** (no credential, ever), **USER** (`user` or
`administrator`), **ADMIN** (`administrator` only).

The 140 registered routes are classified below. The rule that decides the
ambiguous cases: **a route is OPEN only if its response cannot be used to
change the kiln's behaviour and reveals nothing an onlooker at the kiln cannot
already see.** Anything that writes is at least USER.

### OPEN — the always-viewable dashboard

The dashboard must be monitorable by anyone, so these are the routes
`app.js` actually fetches to render it, plus the static assets and page shells
needed to load it:

`GET /`, `/app.js`, `/nav.js`, `/theme.css`, `/commissioning_shared.js`,
`GET /api/status`, `GET /api/profile_exec`, `GET /api/readiness`,
`GET /api/unit_pref`, `GET /api/ota/esp/status`, `GET /api/history.csv`,
`GET /api/profile_plan`, `GET /api/board_temps`, `GET /api/firing_history`.

Also OPEN, read-only and needed before anyone can log in at all:
`GET /status`, `GET /scan`, `GET /networks`, `GET /wifi`.

### USER — start and stop a firing, and nothing else

`POST /api/profile_exec/start`, `/api/profile_exec/stop`,
`/api/profile_exec/pause`, `/api/profile_exec/resume`,
`/api/profile_exec/ack_last_run`.
Plus the reads a start needs: `GET /api/profile`, `/api/profiles`,
`/api/profiles/builtin`, `/api/profile/export`, `/api/kiln_configs`.

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
`/api/profile/builtin/restore`, `POST /api/kiln_configs/{apply,clone,delete,import,rename,save}`.

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

*Network writes:* `POST /provision`, `/forget`, `/ip_config`.

*Page shells other than `/`:* `/status`, `/settings`, `/settings/backup`,
`/settings/display`, `/settings/zones`, `/settings/safety`, `/safety`,
`/safety/commissioning`, `/profiles`, `/diagnostics`, `/readiness`, `/setup`,
`/ota`. A page shell is HTML only, but an unauthenticated visitor who can load
the diagnostics shell learns the board's full feature surface, and it is
cheaper to gate the shell than to audit every widget inside it.

`GET /api/ota/challenge` stays OPEN — it issues a nonce that is worthless
without the AP password, and gating it would break the OTA flow it exists to
serve.

### Ambiguous routes, with a recommendation

| Route | Why ambiguous | Recommendation |
|---|---|---|
| `GET /api/history.csv`, `/api/firing_history` | Full thermal history is more than a glance at the panel | **OPEN.** The owner's requirement is that a run can be monitored; a run's temperature curve is the monitoring. |
| `GET /api/zones` | Read-only, but exposes PID gains and per-zone calibration | **ADMIN.** It is a config dump, not telemetry. Its temperatures are already in `/api/status`. |
| `GET /api/readiness` | Enumerates every unfinished commissioning step | **OPEN.** The dashboard renders a readiness banner from it, and it reports nothing secret. |
| `GET /api/unit_pref` | Needed to render the dashboard | **OPEN for GET, ADMIN for POST.** |
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

**Record layout.** One versioned blob per credential in NVS namespace
`kiln_cfg` on `KILN_NVS_PARTITION`, the existing namespace, so no new
partition and no new namespace:

- key `web_auth` — `{ version, flags, per-role records }` where each record is
  `{ username[33], salt[16], hash[32], iterations, must_change }`.
  Two records: one `administrator`, one `user`.
- key `lcd_auth` — `{ version, salt[16], hash[32], iterations, digits }` for
  the LCD PIN. Separate key so a PIN change does not rewrite the web blob.
- key `auth_policy` — `{ version, web_enabled, lcd_enabled, lock_timeout_s }`.

Three keys, 8, 8 and 11 characters — all inside NVS's 15-character limit, each
declared with `NVS_KEY_LEN_CHECK()` so the limit is a compile-time failure, and
each also caught by `tools/check_nvs_key_length.ps1`'s independent backstop.

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

**The PIN is hashed identically to a password.** A 4–8 digit PIN has at most
10^8 candidates, so its hash is brute-forceable offline in any case; hashing it
costs nothing and stops a casual NVS dump from reading it directly. The real
protection is that PIN entry is physically local and rate-limited by the
existing lockout.

*Acceptance:* a host test sets a password, reads back the stored blob, and
asserts the plaintext appears nowhere in it; two identical passwords set on
two boards produce different hashes (salt is actually random and actually
stored); verification of the correct password succeeds and of a
one-character variant fails.
*Negative test:* replace the salt with a fixed constant in the production
setter, confirm the "two boards differ" assertion goes RED, restore by hand,
confirm GREEN.

---

## 3. Password and PIN strength rules

**LCD PIN: 4 to 8 digits**, enforced in two places — the keypad refuses to
submit outside the range, and the web setter rejects out-of-range before
hashing. The setter's check is the authoritative one; the keypad's is a
convenience. Both must exist, because a PIN set out of range from a script
would otherwise be unenterable at the panel and lock the LCD permanently.

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
password, the LCD PIN, the lock timeout, and the two auth-enabled switches.
**The LCD PIN is set here and only here** — the LCD never sets its own PIN,
so a stolen panel session cannot change the credential that protects it.

**Changing a password invalidates every session for that role, including the
caller's.** The caller is redirected to a fresh login. Reasoning: the usual
reason to change a password is that the old one is compromised, and leaving
the sessions it authorised alive defeats the change. Changing the LCD PIN
drops the LCD session. Changing the lock timeout does not invalidate anything
— it is a preference, not a credential.

*Acceptance:* a host test asserts that a password change clears exactly the
slots holding that role and leaves other-role slots intact; a `user`-role
session receives 403 from every method on the security endpoints.

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

**Does the PIN carry a role? No — one tier.** Recommendation: the LCD PIN
grants the **USER** tier only. Reasoning: the owner described two roles for the
web GUI specifically, the LCD has no text entry for a username, and no
administrator action in the route table has an LCD equivalent worth the risk —
zones config, calibration, OTA and the danger zone are all web-only surfaces.
A single-tier PIN also means the PIN can never be the credential that
authorises a factory reset. **This is a recommendation, not a settled
decision — see item 13.**

*Acceptance:* `UI_TEST_CMD_LIST_TAP_TARGETS` reports all 12 keypad keys plus
`OK` and `Cancel` as named, non-hidden tap targets; a scripted
`kiln_ui_click_by_name()` sequence entering a correct PIN unlocks, and a wrong
one does not; `capture_lcd.ps1` plus numeric pixel sampling confirms the
overlay fits inside the panel with no scrollbar and introduces no colour
outside the theme set.

---

## 8. The inactivity lock and the 10-second prompt

**One shared timeout value, not two.** Recommendation, and the reason: the
owner asked for it "configurable in the same password section", one field is
one thing to explain and one thing to get wrong, and there is no argued reason
the two surfaces should differ. Range 1–60 minutes plus `never`, stored in
`auth_policy`. `never` is meaningful and must work: an operator who wants the
panel permanently unlocked while standing at the kiln should not have to
disable auth to get it.

**What counts as activity.** Web: any authenticated request that is not the
keepalive itself — so a dashboard polling `/api/status` from an idle browser
tab does **not** keep an admin session alive, which is the whole point. LCD:
any touch event delivered to LVGL, including a touch that only scrolls or is
swallowed by a backdrop.

**What locking does.** The session drops to the unauthenticated tier and the
interface returns to the Dashboard. The display does not blank — backlight
behaviour stays entirely `display_power_cfg`'s business, which already has its
own separate idle timeout and keep-on-while-firing setting. **A firing is
never interrupted by a lock**; the executor does not read auth state at all.

**The 10-second prompt.** At `timeout - 10 s`, both interfaces show a "Stay
unlocked" prompt. On the LCD it is a `ui_confirm` dialog with a 10-second
countdown in its body; on the web it is a modal in `app.js`. Accepting
refreshes `last_seen_ms`. Ignoring it locks at expiry and the prompt closes
itself.

**The prompt must never block a safety action.** It is a normal
`lv_layer_top()` modal, so the Stop control is behind it for those 10 seconds
— that is unacceptable, so the LCD prompt is shown **only when a gated action
is possible in the first place**, and it is dismissed instantly by any touch
outside it, not only by its own buttons. On the web the modal is
non-blocking: the dashboard behind it stays interactive. State plainly: no
code path in this feature may take a lock that a stop or trip path also takes.

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

*Acceptance:* a host test with an injectable clock asserts a session is valid
at `timeout - 1 ms`, invalid at `timeout + 1 ms`, that the prompt window opens
at `timeout - 10 s`, that accepting extends by the full timeout, and that a
keepalive request alone does not extend it.
*Negative test:* make the activity check treat the keepalive as activity,
confirm the "keepalive does not extend" assertion goes RED, restore by hand.

---

## 9. Safety interaction

**These paths bypass authentication unconditionally, in every auth state —
enabled, disabled, locked, or mid-prompt:**

- `POST /api/profile_exec/stop`. Nominally USER, but a stop is never refused
  for lack of a session. A stop makes the kiln safer; there is no threat model
  in which blocking it is the safe choice, and `app.js` already calls it from
  the unauthenticated dashboard.
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

---

## 10. The physical credential reset

**The owner's sketch cannot be implemented literally**, for a reason found in
the code: the LCD has no Stop button independent of the merged Start/Stop
widget, and that widget reads "Stop" **only while a firing is RUNNING or
PAUSED**. A gesture ending in "press stop, press stop" would therefore require
a firing to be in progress — the exact condition under which the gesture must
be impossible.

**Recommended gesture instead:**

1. E-stop asserted (`safety_link_status_t.flags & SAFETY_FLAG_ESTOP`), and
2. no firing in progress, and `heat_enable_is_granted()` false, then
3. three taps on the Dashboard graph within 3 seconds, which arms the reset and
   shows a plain-text banner in the existing `s_ui_home_trip_strip` — "AUTH
   RESET ARMED — confirm within 30 s", then
4. a `ui_confirm` dialog whose confirm button must be pressed. One deliberate
   confirmation, not a second ambiguous tap.

Why this shape: the E-stop precondition makes it unreachable remotely by
construction (E-stop is a physical contact read by the Pico, not a writable
field), the three-tap-then-confirm sequence is not hittable by accident, and
the heat and firing gates satisfy "impossible during a firing or while heat is
enabled" without depending on a widget's caption.

**New work this needs:** the home chart is not a tap target today — it has no
`LV_OBJ_FLAG_CLICKABLE` and no `LV_EVENT_CLICKED` handler, only a draw
callback. Make it clickable and give it a name so `kiln_ui_click_by_name()`
can reach it; a nameless chart cannot be clicked by name. Also add a small
accessor for the E-stop bit — the bit is already populated and cached on the
ESP but **has no consumer anywhere**, so this is a read, not new plumbing.

**What the reset actually does:** restores a default administrator credential
with `must_change = true`, so the next login is forced to set a new password
before anything else. It does **not** disable authentication, does not touch
the `user` password or the LCD PIN, and does not clear any config. Reasoning:
the failure being recovered from is a forgotten password, and silently
unlocking the whole board is a bigger hole than the one being closed.

**Logging:** `ESP_LOGE` at arm, at confirm and at cancel, naming E-stop state
and firing state, in the same loud style as the `boot_button_ota_bypass_active()`
branch. One line each; this is a rare event and must be findable afterwards.

*Acceptance:* a host test on the pure gate predicate asserts the gesture is
refused with E-stop clear, refused with a firing running, refused with heat
enabled, and armed only with all three conditions met, and that arming expires
after 30 s. On the bench, a `TOUCH_CMD_INJECT` / `kiln_ui_click_by_name()`
sequence performs the gesture with no human present and `capture_lcd.ps1`
confirms the armed banner is on screen.
*Negative test:* invert the E-stop condition in the production predicate,
confirm the "refused with E-stop clear" assertion goes RED, restore by hand.

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

---

## 12. Testing

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
3. **Config backup / export.** Credentials are **not** in the file. State it on
   the backup page in one line — "Backup does not include usernames,
   passwords or the LCD PIN. Those stay on this board." — so an operator
   restoring onto a replacement board is not surprised, and knows they must set
   credentials again on new hardware.
4. **Config restore, swap, or a package from a different board.** Credentials
   are untouched by the restore, in all three cases, including a foreign
   package. The import path never opens the credential keys.
5. **Factory reset and config erase.** Credentials are **out of scope for
   every existing reset scope** — "wifi", "kiln", "profiles" and "all". The
   deliberate credential-recovery path is the physical gesture, which already
   covers the real need and leaves a log entry; an accidental "all" should not
   also silently clear who may log in. Note the asymmetry deliberately: "all"
   formats the `cfg` partition and restores builtin profiles but leaves the
   three credential keys standing.

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

---

## 13. Owner decisions still needed

1. **Does the LCD PIN grant `user` only, or may an administrator use the
   panel?** Recommendation: `user` only (item 7). Needs a yes.
2. **Is one shared lock timeout acceptable, or are separate web and LCD values
   wanted?** Recommendation: one shared value (item 8).
3. **Is the revised reset gesture acceptable?** The owner's
   "stop, stop" ending cannot be built as described (item 10).
