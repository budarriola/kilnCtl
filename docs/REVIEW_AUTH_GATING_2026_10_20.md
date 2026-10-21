# Review: KilnFW web and LCD auth gating (2026-10-20)

Static review of origin/dev (after 3475c8c29) against the owner auth rules. No code changed, no board access.
Scope: every `kiln_http_register` route and its `route_tier_table.h` row, the page-shell gate (KG-01),
CSRF/origin and Host checks, session expiry and idle, lockout tables, the HR-02 409 gate, and the LCD PIN gates.
AU-02 (absent policy record counts as auth off) is being fixed by another agent and is not re-reported.

Result: HIGH 0, MED 2, LOW 3, INFO 3.

## Rules confirmed (no finding)
- Unauthenticated web = dashboards only: OPEN rows are `/`, `/login`, `/status`, static assets (`app.js`, `nav.js`,
  `theme.css`), dashboard reads (`/api/status`, `/api/profile_exec`, `/api/readiness`, `/api/history.csv`,
  `/api/profile_plan`) and the login/forgot/reset/session routes. Every other page shell is ADMIN.
- Untabled routes fail closed to ADMIN (`http_auth_enforce.c:25`, register path in `http_auth_http.c`); an unknown
  decision denies. No route bypasses `kiln_http_register` (the only raw registration is the wrapper itself); the 404
  handler only redirects to `/` or returns a JSON 404.
- Page shells: a page-shell GET without the role gets a 302 to `/?next=<page>` and the shell is never sent (KG-01,
  `http_auth_http.c` DENY branches); the location builder allows only `[A-Za-z0-9_/]` and `kcSafeNextPath` filters
  the target client-side.
- Stop (`/api/profile_exec/stop`) is USER; current_sweep/abort, autotune/abort and danger/stop are ADMIN;
  `ROUTE_TIER_SAFETY_REDUCE` has no rows.
- `/wifi`, `/networks`, `/scan` are `ROUTE_TIER_WIFI_SETUP`: open only while unprovisioned, ADMIN otherwise.
- boot_guard_reset and factory_reset are ADMIN; the HMAC scheme is retired and the ADMIN tier is the only gate.
- CSRF: a non-GET/HEAD with a cross-origin Origin/Referer (or `Origin: null`, or overlong headers) gets 403; a
  foreign Host is refused on every write; the cookie is `HttpOnly; SameSite=Strict; Path=/`.
- Sessions: idle timeout 1..60 min or never, 12 h absolute lifetime, IP-bound, constant-time token compare;
  OPEN polls (including `/api/auth/session`) do not count as activity.
- A full login lockout table evicts the oldest entry (AZ-04, `web_auth_login_http.c:213-252`).
- HR-02: every safety_cfg POST (commissioning, relay_type, ct_cal, ct_trim, ct_auto_zero, bench_preset,
  rate_guard/auto) runs `safety_cfg_mode_gate_refuse` (409) first.
- LCD: Menu, profile picker, pause/resume and Start/Stop need the USER PIN; Edit firing and Clear trip need the ADMIN
  PIN; the trip strip opens Safety only through the USER PIN (`ui_page_home.c:234`); relock returns home and closes
  the keypad, confirm dialog and num pad (`ui_lcd_lock.c`).

## Findings

### AG-01 MED: the dashboard polls an ADMIN route every 10 s, so the web idle timeout never fires
`firmware/KilnFW/App/drivers/http/main_page.html:4290,4682`; touch at `http_auth_http.c:459`, activity set at
`http_auth_enforce.c:153`.
`setInterval(pollAutotune, 10000)` runs unconditionally on the dashboard and fetches `GET /api/autotune` (ADMIN).
For a signed-in admin each poll is an ALLOW on an ADMIN tier, which `http_auth_decision_counts_as_activity` counts
as activity, so the session is touched every 10 s. An idle dashboard tab keeps an admin session alive up to the 12 h
absolute cap, which defeats the idle lock. Signed out, the same poll returns 401 every 10 s. The other dashboard
ADMIN fetches (`/api/zones`, `/api/aux_outputs`) run on load or user action, not periodically.
Fix: poll autotune only when the session poll reports `role == admin` and a run is active, or fold the dashboard's
autotune summary into `/api/status`. Alternatively mark background fetches (for example `X-Kiln-Background: 1`) and
skip the activity touch for them in the pre-handler. Add a source-scan test that no periodic dashboard poll targets
a non-OPEN route.

### AG-02 MED: the TOTP forgot/reset lockout table refuses new IPs when full instead of evicting the oldest
`firmware/KilnFW/App/drivers/http/auth_totp_http.c:177-210,244-246`.
`totp_lockout_slot_for` evicts only an unlocked LRU slot and returns NULL when all 16 slots are locked; the gate
treats NULL as locked (429 at the maximum ladder step). A LAN host with 16 addresses locks every other local client
out of password reset. This breaks the AZ-04 owner rule (a full lockout table evicts the oldest entry), which was
applied only to the login table.
Fix: mirror `login_lockout_slot_for`: track the oldest in-use slot regardless of lock state and evict it as the last
resort; add the AZ-04 host test for this table and negtest it.

### AG-03 LOW: `/api/watchdog_cfg` POST is not refused while running
`firmware/KilnFW/App/drivers/http/diagnostics_http.c:886-924`.
Disabling the task-watchdog panic mid-firing is accepted with no `system_mode_gate` check, while the other
safety-relevant writes return 409 while running (HR-02). It is ADMIN-only, so the impact is low.
Fix: if the owner counts it as a safety write, call the same mode gate as the cfgfs restore path before the body
read; otherwise record it as an accepted exception.

### AG-04 LOW: stale route-table comments contradict KG-01
`firmware/KilnFW/App/drivers/http/route_tier_table.h:488-505,523-527`.
The `kPageShellUris` header still says the listed shells are "served WITHOUT a session", and the `/wifi` note says
"the shell is still served" once provisioned. Since KG-01 the list means the opposite: redirect, never serve. The
next editor could take the comment as the contract and re-open the shells.
Fix: rewrite both comments to "page shells that get a 302 to /?next= instead of a 401 body", keeping the
WEB-X-03 parse shape.

### AG-05 LOW: the "never" web timeout also disables the 12 h absolute lifetime
`firmware/KilnFW/App/drivers/net/web_auth_session.c:28-34`.
With `web_timeout_min = -1`, `slot_lifetime_exceeded` returns false and `reap_expired` returns early, so a forgotten
or stolen (IP-bound) session lasts until reboot or logout. The idle "never" option need not remove the absolute cap.
Fix: apply `WEB_AUTH_SESSION_MAX_LIFETIME_MS` regardless of the idle setting (or ask the owner whether "never" was
meant to include it), and update the host test.

### AG-06 INFO: auth off opens every ADMIN route, factory reset and boot_guard_reset included
`firmware/KilnFW/App/drivers/http/http_auth_enforce.c:34`. By design (section 11): with web auth disabled, the
routes that replaced the AP-HMAC scheme are unauthenticated. Read "AP-HMAC routes need a web admin" as "when auth
is on".

### AG-07 INFO: `/status` returns the board AP passphrase to any unauthenticated AP client
`firmware/KilnFW/App/drivers/http/wifi_provision_http.c:288-290`, `firmware/KilnFW/App/drivers/net/wifi_prov.h:555-558`.
Only callers already joined to that AP (who know the passphrase) get it; STA callers get "". Acceptable; note only.

### AG-08 INFO: route wrapper headroom is 11
`firmware/KilnFW/App/drivers/http/http_auth_http.c` (`KILN_HTTP_MAX_ROUTES 192`) against 181 tier rows. A route past
the cap is refused with ESP_LOGE (fails closed), not registered unwrapped. Raise the cap with the next route batch.
