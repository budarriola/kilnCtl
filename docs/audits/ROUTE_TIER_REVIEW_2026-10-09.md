# Web-auth route tier review, 2026-10-09

Scope: every route in `firmware/KilnFW/App/drivers/http/route_tier_table.h`
on `origin/dev` at 9584737f, with focus on routes added or changed since
2026-10-04 (`git log -p --since=2026-10-04 origin/dev -- firmware/KilnFW/App/drivers`).
Read-only review. No code changed, no board touched.

Method: a comment-stripping script listed every `.uri/.method/.handler`
initializer under `App/drivers/**/*.c` and every `ROUTE_TIER()` row, then
compared the two sets in both directions. Every GET handler body was scanned for
calls named `*_set/_save/_write/_erase/_clear/_reset/_commit/_start/_apply/_delete`,
and each hit was read by hand.

## Routes added since 2026-10-04

| Route | Method | Tier | Effect | Verdict |
|---|---|---|---|---|
| /api/aux_outputs | GET | ADMIN | read aux bindings | ok |
| /api/aux_outputs | POST | ADMIN | aux config write (flash) | ok |
| /api/aux_outputs/manual | POST | ADMIN | energises a spare relay | ok |
| /api/update/stage | POST | ADMIN | writes the `stage` partition | ok |
| /api/update/stage/clear | POST | ADMIN | erases stage | ok |
| /api/update/stage | GET | ADMIN | staged version/hash | ok |
| /api/update/settings | GET/POST | ADMIN | repo setting (flash) | ok |
| /api/update/check | POST | ADMIN | outbound GitHub query | ok |
| /api/update/download | POST | ADMIN | fetch into stage (flash) | ok |
| /api/update/fetch | GET | ADMIN | job status | ok |
| /api/update/fetch/cancel | POST | ADMIN | cancels job | ok |

Changed behaviour on existing routes:
- `POST /api/zones` (ADMIN): `move_zone_to_aux` (rewrites zones, aux and every
  profile rule) and `relay<N>_type`. ADMIN is correct.
- `POST /api/safety/commissioning/relay_type` (ADMIN). Correct.
- `POST /api/ota/esp/recovery_boot` (ADMIN; reboots into recovery). Correct.

No firmware route exists for "config-convert". `convert_config` is PC-side only
(`tools/PcTools/src/kilnctrl/config_convert.py`), so it needs no tier row.

## (a) Tier correctness

Every route that energises a relay, writes flash, or reboots is ADMIN. The one
exception is `profile_exec/{start,stop,pause,resume,ack_last_run}`, which are
USER under the owner decision of 2026-09-28. The checked routes include
aux manual, the danger/* routes, current_sweep, autotune, `/api/zones`, safety
commissioning, every OTA/update/stage route, `sw_reset`, `factory_reset`,
`boot_guard_reset`, `recovery_boot`/`recovery_exit`, `cfgfs/format_confirm`,
`cfgfs/file` POST, `backup/import` and the provisioning writes. `boot_guard_reset`
and `factory_reset` are ADMIN, as the owner decided on 2026-09-29.

The OPEN set is: `/`, `/login`, the `auth/login|forgot|reset|session` routes,
the static JS/CSS, `/api/status`, `/api/profile_exec` GET, `/api/readiness`,
`/api/history.csv`, `/api/profile_plan`, `/status` and `/api/ota/esp/status`.
The three `ROUTE_TIER_WIFI_SETUP` routes are open only while the board is
unprovisioned. This matches "unauthenticated = dashboards only". `/status` and
`/api/ota/esp/status` are not dashboards strictly speaking, but they redact by
role and interface (`http_auth_may_disclose()`, the `on_ap` gate). See LOW-3.

`ROUTE_TIER_SAFETY_REDUCE` has no remaining row. It is documented as
intentionally unused.

## (b) Table and registration agree

- 175 registered (uri, method) pairs and 175 rows. Every registered route has a
  row, and every row has a registered route. There are no stale rows.
- Every route goes through `kiln_http_register()`. The only raw
  `httpd_register_uri_handler()` call is inside that wrapper
  (`http_auth_http.c:473`). A missing row fails closed to ADMIN at runtime
  (`http_auth_http.c` near line 444).
- Existing check coverage:
  - `tools/check_route_tier_coverage.ps1` covers only code to table: a route
    with no row fails. It does not detect a stale row (LOW-1).
  - `tools/check_uri_handler_cap.ps1` covers `max_uri_handlers` against the
    route count. Nothing checks the second cap, `KILN_HTTP_MAX_ROUTES` (LOW-2).

## (c) Handler capacity

- `config.max_uri_handlers = 184` (`wifi_provision_http.c:1214`) against 175
  routes leaves 9 spare slots. CLAUDE.md still says 163/170 (LOW-4).
- `KILN_HTTP_MAX_ROUTES = 192` (`http_auth_http.c:50`) is above both, which is
  fine today. If it ever fell below the route count, `kiln_http_register()`
  would refuse the extra routes, which is also fail-closed.
- The recovery image (`KilnFW_recovery/main/recovery_http.c:988`) registers 14
  routes against a cap of 16, enforced by `_Static_assert`.

## (d) Mutating routes reject GET

- Every mutating route is registered as `HTTP_POST` only. esp_http_server
  answers GET on a POST-only URI with 405, so a GET can never reach a mutating
  handler.
- No GET handler mutates state. The scan flagged 23 GET handlers by call name,
  and each one was read:
  - Readiness `pico_auto_update_state_set_last_decision` and zones
    `zone_ct_map_set` appear only in comments.
  - `zones_config_apply_cal()` is a pure function.
  - The kiln_configs `list_get_handler` hits were a brace-matcher overrun into
    the next handler.
  - The rest are `web_set_asset_cache_headers` or read accessors.
- `/status` calls `wifi_prov_note_possible_static_reachability()`. This is a
  documented no-op unless a static-IP join is still unconfirmed. It is benign
  and intended.
- With auth ON, the session cookie is `HttpOnly; SameSite=Strict`
  (`web_auth_login_http.c:556`), so a cross-site POST carries no session. With
  auth OFF, nothing stops a cross-site POST (MED-1).

## (e) Legacy single-slot OTA routes

`POST /api/ota/esp` and `POST /api/ota/esp/rollback` are still registered
(`ota_http.c:714`, `ota_http.c:771`), both ADMIN.
- `/api/ota/esp`: after the interlock check and the update-mutex claim,
  `ota_http_esp_target_usable()` refuses with 409 because the next OTA slot is
  the running `app` partition. It then drains up to the whole body (30 s cap).
  It cannot write flash.
- `/rollback`: `esp_ota_check_rollback_is_possible()` cannot be true on a
  single-ota_x table, so it always answers 409.
- Neither route can do harm. Each still costs a route slot and an ADMIN-tier
  entry point. Each also keeps a 4 KB-chunk drain loop reachable on the httpd
  task, and that loop has produced TASK_WDT trouble before (the 2026-10-03
  bench finding). See LOW-5.

## Findings

No HIGH findings.

**MED-1. With web auth off, every route can be driven cross-site (CSRF and DNS
rebinding).** `http_auth_check()` returns ALLOW for every tier when
`!web_enabled`. That follows the owner decision "open when login off". Nothing
else stands in the way, though:
- No handler checks Origin, Referer or Host. A tree-wide grep finds none.
- No handler requires a non-simple Content-Type. Many POST handlers parse
  `application/x-www-form-urlencoded` through `http_form.h`.

So any web page that a LAN user opens can submit a plain HTML form to
`http://<board>/api/aux_outputs/manual`, `/api/diagnostics/danger/relay`,
`/api/profile_exec/start`, `/api/factory_reset` or `/api/update/stage`. The
browser sends no CORS preflight for such a form. "Open when login off" was a
decision about LAN users. It does not grant access to every website those users
visit.

Fix: in `kiln_http_prehandler()`, for every non-GET method, reject a request
whose `Origin` header is present and does not match the request's `Host`.
Optionally also reject a `Host` that is neither the board IP, its mDNS name nor
`192.168.4.1`. This must apply whether auth is on or off. It is about 20 lines
plus host tests. It does not change the owner's auth-off semantics for
same-origin or non-browser clients (MCP tools send no Origin).

**LOW-1. `check_route_tier_coverage.ps1` is one-directional.** A stale row
cannot open anything, but it can hide a later re-added route that gets a tier
nobody re-reviewed. Fix: also fail on rows with no registration (the table's
header comment already says "should still be deleted when noticed"). The set is
clean today (0 stale rows), so this is free to enforce.

**LOW-2. No check guards `KILN_HTTP_MAX_ROUTES` (192).** It is the second,
independent route cap. Overflowing it fails closed, but the result would be an
unexplained 404 that looks like the old `max_uri_handlers` bug. Fix: extend
`check_uri_handler_cap.ps1` to assert `KILN_HTTP_MAX_ROUTES >= max_uri_handlers`.

**LOW-3 (RESOLVED 2026-10-09). `/api/ota/esp/status` is OPEN but is not a dashboard read.** Resolved: `recovery_mode` is now emitted by `GET /api/status` (OPEN), app.js `pollRecoveryMode()` reads it there, and `/api/ota/esp/status` is ROUTE_TIER_ADMIN. Original finding: Its
payload is redacted for non-admins. `recovery_mode` is deliberately left
unredacted for app.js's `pollRecoveryMode()`. Either confirm that every field
an unauthenticated caller receives is dashboard-necessary, or split
`recovery_mode` into `/api/status` and make this route ADMIN.

**LOW-4. CLAUDE.md is stale in two places:**
- It says the URI cap is "163 routes / cap 170, 7 spare". The actual figures are
  175 routes, cap 184, 9 spare.
- It describes `GET /api/boot_guard` as "unauthenticated". It is ADMIN in the
  application (`route_tier_table.h`). It is unauthenticated only in the
  recovery image.

Fix: correct both lines.

**LOW-5 (kept by owner decision 2026-10-09; routes, `ota_rollback_esp` and the OTA matrix cases stay as they are). Legacy `POST /api/ota/esp` and `POST /api/ota/esp/rollback` in the
application image are dead.** Each always answers 409 and adds attack surface
for no benefit. Fix: once the ota_page.html buttons are gone (another agent is
removing them now) and the PcTools callers (`ota_rollback_esp`, the OTA matrix
cases) are moved to recovery/stage or retired, unregister both routes. Delete
their `ROUTE_TIER` rows in the same change, which frees 2 handler slots. Do not
touch the recovery image's own `POST /api/ota/esp`, which is the sanctioned
push path. A lighter option is to keep both URIs as a tiny handler that returns
410 Gone without claiming the mutex or draining a body. That removes the drain
loop and keeps clear errors for old clients.

## Not verified

- Live behaviour on a board (405 on GET to a POST route, an actual cross-site
  form POST with auth off). This review did not touch the board.
- The LCD side of "unauthenticated = dashboards only". This review covers HTTP
  routes only.
