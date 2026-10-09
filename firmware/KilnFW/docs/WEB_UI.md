# Web UI — page inventory and HTTP API reference

The board runs a single `esp_http_server` instance, started by
`wifi_provision_http_start()` and shared by every other module that serves
anything. 13 HTML pages and well over eighty JSON/CSV/plain-text endpoints
hang off it (plus two more in a simulated-plant build). This document is the
wire reference for all of them; see each `*_http.c`'s `.uri = "..."`
registrations for the exact, current list — `grep -n '\.uri = "' App/drivers/*.c`
is the ground truth this section summarizes.

**`rules_http.c`/`rules_eval.c`/`rules_task.c` (the relay rule DSL editor,
`/settings/relays`, `/api/rules`) were deleted 2026-08-27** — see
`App/main.c`'s comment at the rule-evaluator init site and
`App/drivers/CMakeLists.txt`'s "Relays &..." comment. `manual_page.html`
(`/manual`) and its only backing endpoint were also removed the same day
(`dashboard_http.c`'s 2026-08-27 comment). Neither route exists any more;
don't look for them.

Source of truth:
- `App/drivers/http/wifi_provision_http.c` — owns the server, `/`, `/wifi`,
  `/status`, `/scan`, `/provision`, `/networks`, `/forget`, `/ip_config`,
  and the shared static assets `theme.css`, `nav.js`, `app.js`
- `App/drivers/http/dashboard_http.c` — `/api/status`, the profile executor and
  autotune endpoints, the CSV exports, safety trip/log-level control
- `App/drivers/http/zones_http.c` — `/settings/zones`, `/settings/safety`,
  `/api/zones`, current-sweep, tuning-recommendation endpoints
- `App/drivers/http/profiles_http.c` — `/profiles`, the profile CRUD API
- `App/drivers/http/diagnostics_http.c` — `/diagnostics`, `/safety`, crash
  report, watchdog config, ramp-assist toggle, thermo faults
- `App/drivers/http/safety_cfg_http.c` — `/safety/commissioning`,
  `/api/safety/commissioning`
- `App/drivers/http/readiness_http.c` — `/readiness`, `/api/readiness`
- `App/drivers/http/settings_http.c` — `/settings`, `/settings/display`
- `App/drivers/http/backup_http.c` — `/settings/backup`, config export/import
- `App/drivers/http/ota_http.c` — `/ota`, ESP/Pico OTA and rollback endpoints
- `App/drivers/http/kiln_cfg_http.c`, `log_http.c`, `board_temps.c`,
  `partition_info_http.c`, `factory_reset.c`, `adaptive_tune_http.c` — API-only,
  no page of their own
- `App/drivers/sim/sim_backend.c` — `/api/sim`, `CONFIG_KILNCTL_SIM_PLANT` only
- Pages: `App/drivers/*_page.html` (13 files — run `ls App/drivers/*_page.html`
  rather than trusting a count restated here)

TODO.md sections 2/3/5 remain the authoritative design docs and change logs
for this UI, with dated "Implemented"/"Not built" notes; this file is the
"what does the wire actually look like" companion to them. Where this file
and the code disagree, the code wins — fix whichever one is wrong.

**HARDWARE STATUS**: per `docs/PROJECT_STATUS.md`, the page routes then
existing and the zones/profiles JSON APIs (the relay-rule DSL editor was
still present at the time; it was deleted 2026-08-27, see above) were
verified live against a real board on 2026-08-11, and `GET /api/status` /
the then-current manual relay-override endpoint on 2026-08-10 — in every
case at first against a bench unit with **no thermocouple daughterboard,
relay expander, display or safety peer physically attached**. That changed
2026-08-20: three MAX31856 ICs and their thermocouples are now fitted on this
board (the ESP32-S3/`KilnFW` side — the safety processor still has none), and
`GET /api/status` has served real readings since (channels 0/1/2 around
31-32 °C, cold junctions tracking ~0.3 °C below, no faults). The relay
expander, display and safety peer are unaffected by that change and remain
unverified against real hardware. `/api/sim`'s handlers have never run at all
— that build configuration compiles but has never been flashed.

## The server itself

`httpd_start()` with three deliberate deviations from `HTTPD_DEFAULT_CONFIG()`:

| Setting | Value | Why |
|---|---|---|
| `lru_purge_enable` | `true` | recycle the oldest connection under pressure rather than refusing new ones — a stuck client must not lock a phone out permanently |
| `max_uri_handlers` | 40 | this repeatedly turned out to be too tight as routes grew — **found the hard way (2026-08-11)** when 24 was exactly one too few and, because `httpd_register_uri_handler` failures are logged and non-fatal, `profiles_http.c` (registered last at the time) silently lost all five of its routes and `/profiles` 404'd on the live board. The route count has grown well past 30 since (this doc no longer restates a total — `grep -c '\.uri = "' App/drivers/*.c` is the live count); `tools/check_uri_handler_cap.ps1`, referenced in `wifi_provision_http.c`, is what actually guards this now, not a number in this doc |
| `stack_size` | 8192 | `zones_post_handler` alone stacks a 3201-byte body buffer plus a `zones_cfg_t` scratch copy; the 4096 default was observed to hang/reset under load rather than assert — see `docs/PROJECT_STATUS.md` |

Registration order follows `app_main`'s bring-up order across every module
listed in "Source of truth" above; `rules_http.c` no longer exists (deleted
2026-08-27) so it is not part of that order any more.
`sim_backend_register_http()` adds 2 more routes in a sim build.

Every module after `wifi_provision_http.c` calls
`wifi_provision_http_get_server()` and returns `ESP_ERR_INVALID_STATE` if the
server isn't up — the same non-fatal bring-up convention every driver in this
firmware uses. A page whose module failed to start is simply absent that
boot; nothing else is affected.

## Everything is polled, nothing is pushed

There is no WebSocket and no SSE. `CONFIG_HTTPD_WS_SUPPORT` is **not set** in
`sdkconfig`, and TODO.md section 2's last bullet is where that was settled:
push (WebSocket, or SSE via `httpd_resp_send_chunk`) was designed for but not
justified for the increment that built the live status view, so it uses 2 s
polling instead — the same pattern `wifi_provision_page.html` had already
established. That bullet stays open, to be revisited "if 2 s polling proves
too coarse once real hardware is attached and actually being watched during a
firing." Nobody has watched a real firing yet, so that evidence does not
exist.

Actual poll cadences, as coded in the pages:

| Page | Endpoint | Period |
|---|---|---|
| `main_page.html` | `/api/status` + `/api/profile_exec` | 2 s (`setTimeout` chain, so a slow response delays rather than stacks) |
| `main_page.html` | `/api/history.csv` | 15 s — the ring buffer only gains a sample every 30 s (`HISTORY_SAMPLE_PERIOD_S`) |
| `zones_page.html` | `/api/autotune` | 3 s |
| `zones_page.html` | `/api/autotune/matrix` | 5 s |
| `wifi_provision_page.html` | `/status` | 2 s, **only while** `state` is `connecting`/`reconnecting`; otherwise one-shot |

Two endpoints have no page consumer at all: `GET /api/control` exists because
TODO.md 6A.9 asks for a control-focused status shape for a future tuning UI,
and `/api/sim` is driven by hand (curl/`Invoke-WebRequest`) during
development.

## Pages

All 13 are compiled into the binary via `EMBED_TXTFILES` and served straight
out of flash as `text/html`; there is no filesystem and no templating.

| Route | Page | Notes |
|---|---|---|
| `GET /` | `main_page.html` **or** `wifi_provision_page.html` | `index_get_handler` picks: the dashboard if `wifi_prov_is_sta_connected()`, the setup page otherwise. A phone that just joined the fallback AP expects setup; a browser reaching an already-provisioned board expects the dashboard |
| `GET /wifi` | `wifi_provision_page.html` | unconditional — the way back to setup from a working dashboard |
| `GET /profiles` | `profiles_page.html` | profile editor; see [`docs/PROFILES.md`](PROFILES.md) |
| `GET /settings/zones` | `zones_page.html` | zone/thermocouple config, PID gains, autotune UI, tuning-recommendation panel |
| `GET /settings/safety` | `safety_config_page.html` | safety timing config, served by `zones_http.c` |
| `GET /readiness` | `readiness_page.html` | pre-fire checklist |
| `GET /diagnostics` | `diagnostics_page.html` | crash reports, watchdog config, ramp-assist toggle, Danger Zone manual relay control |
| `GET /safety` | `safety_page.html` | safety processor live status |
| `GET /safety/commissioning` | `safety_commissioning_page.html` | safety commissioning workflow |
| `GET /settings` | `settings_page.html` | general settings, factory reset |
| `GET /settings/display` | `settings_display_page.html` | theme and unit preference |
| `GET /settings/backup` | `backup_page.html` | config export/import |
| `GET /ota` | `ota_page.html` | ESP/Pico firmware update |

`rules_page.html` (`/settings/relays`) and `manual_page.html` no longer
exist — both were deleted 2026-08-27, see above.

Cross-page navigation is `nav.js`'s `NAV_LINKS` table (`App/drivers/http/nav.js`),
injected as a top-bar dropdown menu on every page — not the two or three
hardcoded links this section used to describe. There is no authentication of
any kind — anyone who can reach the board's IP can switch a relay.

## Request conventions

Every POST body on this server is `application/x-www-form-urlencoded`,
parsed by `http_form_find_field()` / `http_form_url_decode()`
(`App/drivers/common/http_form.h`) — **not JSON**, in either direction of a POST.
Responses are JSON, `text/plain`, or `text/csv` depending on the endpoint.

`http_form_find_field()` returns the decoded length, `-1` if the field is
absent, or `-2` if it was present but too long to decode into the caller's
buffer. Handlers distinguish those: a too-long value is a rejected request,
never a truncated one, since silently truncating e.g. a password is worse
than refusing it.

Every POST handler follows the same discipline, and a reader auditing a new
one should expect all four steps:

1. `req->content_len` is checked against a per-handler ceiling **before a
   single byte is read**, so a client claiming a huge body gets a 400 rather
   than a read sized from its own claim.
2. The body is read into a fixed stack buffer with an explicit short-read
   loop; a client that disappears mid-body gets a 400, and the partial buffer
   is never trusted.
3. Everything is parsed and validated into a scratch struct; the live config
   and NVS are touched only after the whole submission validates. A
   partially-applied relay or zone config is a worse failure mode than a
   rejected one.
4. NVS write failure is logged and the change is still applied live — the
   operator asked for it now, whether or not it survives a reboot.

Body caps by handler (representative, not exhaustive — grep `_BODY_MAX` /
`content_len >` in each `*_http.c` for the current, authoritative list):
`/provision` 384 (`PROV_BODY_MAX`), `/api/diagnostics/danger/start` 32
(`DANGER_START_BODY_MAX`), `/api/profile_exec/start` 32,
`/api/autotune/start` **192** (raised from 64 on 2026-08-12 when the relay
method added `method`/`setpoint_c`/`relay_d`/`relay_h`/`rule` on top of
`zone`), `/api/profile/delete` 64, `/api/profile` 2048, `/api/zones` 3200
(`ZONES_BODY_MAX`), `/api/sim` 256. **`POST /api/relay` and `POST /api/rules`
no longer exist** — both were removed 2026-08-27 along with `rules_http.c`
and `manual_page.html`; manual relay control now goes through
`diagnostics_http.c`'s Danger Zone (`/api/diagnostics/danger/relay`).

**Numeric field parsing** must reject trailing garbage, not just a wholly
non-numeric value: check `end == val` (nothing parsed at all) **and**
`*end != '\0'` (something parsed, but there were leftover bytes) after every
`strtol`/`strtof` call on a decoded form field. `end == val` alone lets
`"1200X"` through as `1200` — found and fixed 2026-08-24 in
`zones_http.c`'s `parse_u8_field()`/`parse_float_field()` and its two inline
`strtol` call sites (`max_simultaneous_relays`, `safety_tc_type`); every
numeric field this server accepts from an operator is safety-relevant
(thresholds, gains, masks), so a value that isn't *entirely* the number it
claims to be must be refused outright, same "reject outright, never guess"
discipline as the `-2`/too-long case above. Before tightening an existing
parser this way, confirm the real page never sends what the stricter check
would newly reject (a stray unit suffix, trailing whitespace, a locale
decimal comma) — `zones_page.html`'s save handler pushes raw
`<input type="number">.value` straight into the body, which is always plain
ASCII digits/`.`/`-` per the HTML spec regardless of browser locale, so this
was safe to tighten with no page-side change needed; a handler whose page
does something fancier (client-side formatting, a text input instead of
`type="number"`) would need to check that first.

## Wi-Fi endpoints (`wifi_provision_http.c`)

Full semantics are in [`docs/WIFI_PROVISIONING.md`](WIFI_PROVISIONING.md);
this is the wire shape only. Note these three live at the server root, not
under `/api/` — they predate that prefix and are frozen where they are.

### `GET /status`

```json
{"mode":"home|ap","state":"ap|unprovisioned|connecting|connected|reconnecting",
 "ssid":"<saved station SSID>","sta_connected":true|false,
 "sta_ip":"192.168.1.42","ap_ssid":"<this board's own AP SSID>"}
```

`sta_ip` is `""` unless connected. `ssid`/`ap_ssid` are escaped for `"` and
`\` only; anything else passes through, because this is a status readout and
not truncating a legitimate SSID matters more than rejecting an odd one.
Never fails — a truncated buffer is sent as-is rather than erroring.

### `GET /scan`

`[{"ssid":"...","rssi":-58,"secure":true}, ...]`, at most 20 entries.

- **400** `local-only mode: scanning is disabled` when
  `wifi_prov_get_mode() == WIFI_PROV_MODE_AP` — that mode's guarantee is no
  station-radio activity at all, and a scan still brings the STA interface
  up. (The message string still uses the pre-2026-08-11 "local-only" name for
  what is now AP mode.)
- **500** `scan failed` for any other driver error.

Blocking: the scan runs on the httpd worker task for up to ~150 ms per
channel.

### `POST /provision`

Three mutually-exclusive intents, resolved in this order — the first group of
fields present wins and the handler returns immediately:

| Fields | Effect |
|---|---|
| `mode=home\|ap` | sets and persists the Wi-Fi mode |
| `ap_ssid=` and/or `ap_password=` | changes the board's **own** AP identity |
| `ssid=` and optional `password=` | saves **station** credentials for a different network and starts a join |

Responses: `ok` (200, `text/plain`) on success. 400 for `mode must be 'home'
or 'ap'`, `ap_ssid too long`, `ap_password too long`, `ap_ssid must be 1-32
characters`, `ap_password must be empty (open) or 8-63 characters`, `ssid
missing or too long`, `password too long`, `could not save credentials`, or
the shared `body missing or too large` / `body read failed`. 500 for `could
not change mode`.

## Dashboard endpoints (`dashboard_http.c`)

### `GET /api/status`

The live hardware view the dashboard's Thermocouples and Relays sections
render.

```json
{"io_ready":true,
 "relays":[{"relay":1,"on":false}, ...],        // only when io_ready
 "io_read_failed":true,                          // only when the I2C read failed
 "relay_cycles":[0,0,0,0],                       // lifetime contact cycles, always present
 "thermo_ready":true,
 "channels":[{"channel":0,"temp_c":21.50,"cj_c":22.00,"valid":true,
              "fault_status":0,"spi_failed":false,"stale":false,
              "age_ms":1500}, ...],   // age_ms: integer ms since the last
                                       // conversion, null if never converted
 "safety_ready":false}
```

- `io_ready` is "a `kiln_io_t` was handed to `dashboard_http_start()`", i.e.
  the expander came up at boot. `relays[].on` is the **relay shadow** (last
  commanded), not a pin readback — see `docs/UART_PROTOCOL.md`'s IO task for
  why those are kept separate.
- `relay_cycles[i]` is relay *i*'s (0-based) lifetime on/off transition count
  (`relay_cycles.c`, persisted in NVS, added 2026-08-12). Reported even when
  `io_ready` is false: it is stored history, not live hardware state. The
  Relays & Rules page renders it as its contact-wear table; nothing else on
  the dashboard uses it.
- `thermo_ready` deliberately means "at least one channel answered", not "the
  SPI bus exists": a board-less bus reads back `count == 0` with no error, so
  `thermo_bus->initialized` alone would report a healthy front end where
  there is none.
- `temp_c` has this zone's `cal_offset_c` applied
  (`zones_config_apply_cal()`). The UART bridge does **not** apply it — a
  known, explicit inconsistency documented at `zones_http.h`'s
  `zones_config_apply_cal()` doc comment, not a silently dropped
  requirement.
- NaN has no JSON literal, so an unreadable channel reports `temp_c: 0`
  alongside `valid: false` rather than emitting something a naive
  `parseFloat()` breaks on. `valid` is the field to trust; the number next to
  it when `valid` is false is meaningless, not a reading of zero.
- **`stale` vs `age_ms` (2026-08-20).** The driver's `stale` flag means "no
  new conversion since the previous read" — true whenever the poll outruns
  the conversion, which can happen seconds into a normal boot and does not
  mean the reading is old. It is unchanged and still what `safety_link.c`
  relies on. What changed is what the UI calls "stale": `age_ms` (ms since
  the channel's last conversion, `null` if it has never converted) plus a
  single shared 10 s threshold now decide whether the web dashboard, the
  LCD, and the UART status path each show a temperature as stale — applied
  identically on all three so they cannot disagree with each other.
- In a `CONFIG_KILNCTL_SIM_PLANT` build the channels come from
  `sim_backend_read_all()` instead, so the dashboard shows the same
  fabricated kiln the executor is controlling.
- The whole response is built into one 768-byte buffer with a truncate-on-
  overflow `APPEND` macro; a truncated response is sent rather than an error.

### `GET`/`POST /api/ramp_assist` (`diagnostics_http.c`)

The kiln-wide (not per-zone) on/off switch for the forthcoming "ramp assist"
feature (`ramp_assist_cfg.c`/`.h`, `docs/PID_EXPANSION_PLAN.md` §7.5): during
a real firing, if the kiln cannot keep up with a commanded ramp rate, the
executor will warn, auto-stretch the schedule so every target is still
reached, and credit dwell time already spent near the target. **That
behaviour is not built yet** — this endpoint is the on/off flag only, same
GET/POST-toggle shape as `/api/watchdog_cfg` just above it in
`diagnostics_http.c`.

```
GET /api/ramp_assist  ->  {"enabled":false}
POST /api/ramp_assist body="enabled=1"  ->  {"ok":true,"enabled":true}
```

- Persisted (`kiln_nvs` partition, `kiln_cfg` namespace — same single-scalar
  pattern as `unit_pref.c`'s preference, not `kiln_cfg_store.c`'s versioned
  per-slot blob), default **OFF**. A board that has never heard of this key,
  or whose stored byte is out of range, always comes up disabled — never the
  other way.
- Also reported on `GET /api/status`'s `ramp_assist_enabled` field
  (`dashboard_http.c`) so a client doesn't need a second round trip.
- `diagnostics_page.html` has the toggle, under its own "Ramp assist"
  section. It reads the board's actual current state on load, not an assumed
  default, and shows an explicit error state (not a misleading "off") if the
  read fails.
- **Testing hazard:** enabling this during a PID tuning run or an A/B
  controller comparison lets the executor silently stretch a ramp or shorten
  a dwell mid-run, which invalidates every tracking-error number the run
  produces — see `ramp_assist_cfg.h`'s header comment. `tools/PcTools`'
  `config_presets.py` makes `ramp_assist_enabled` a REQUIRED field on every
  preset for exactly this reason: an experiment must PIN the flag, never
  inherit whatever the board already has.
- Reachable from tooling as `ramp_assist_get_enabled`/`ramp_assist_set_enabled`
  (`mcp_server_ramp_assist.py`); the setter is confirm-gated, same idiom as
  `adaptive_tune_set_enabled`.

### `POST /api/diagnostics/danger/relay` (`diagnostics_http.c`)

`dashboard_http.c`'s old `POST /api/relay` manual-override endpoint was
**removed 2026-08-27** along with `manual_page.html`, its only caller (see
`dashboard_http.h`'s `dashboard_relay_result_t` comment). The replacement
lives on the Diagnostics page's Danger Zone and is deliberately gated on that
section being armed first, so an operator can never mistake "the section
isn't armed" for "the relay refused to move":

Body `relay=<1..KILN_IO_RELAY_COUNT>&on=<0|1>`.

| Outcome | Status | Body |
|---|---|---|
| danger mode not active | **409** | `danger mode is not active` |
| success | 200 | `{"ok":true,"remaining_ms":<n>}` |
| body missing/oversized/short | 400 | `body missing or too large` / `body read failed` |
| `relay` or `on` absent | 400 | `relay/on missing` |
| `relay` outside 1..`KILN_IO_RELAY_COUNT` | 400 | `relay out of range` |
| `dashboard_set_relay` failed | 400 | `relay write failed` |

`on` is tested as `on_val[0] == '1'` — anything else is OFF. The actual write
goes through the same `dashboard_set_relay()` the old endpoint used, which
`kiln_io_owner.c`'s `relay_on_blocked()` already bypasses when
`danger_mode_active()` is true — this handler adds only the up-front 409
check and the `danger_mode_touch()` that extends the window, not a second
copy of the gating logic. A successful call extends the danger-mode window;
a refusal does not. See `App/drivers/http/diagnostics_page.html`'s Danger Zone
section for the operator-facing arm/disarm flow this sits behind.

### `GET /api/profile_exec`

Exec-lifecycle view of `profile_executor_get_status()`.

```json
{"state":"idle|running|paused|done|faulted",
 "profile_id":0,"profile_name":"bisque","zone_mask":5,
 "segment_index":0,"segment_count":3,"dwelling":false,"target_c":312.50,
 "segment_elapsed_s":900,"dwell_remaining_s":0,
 "ramp_lock_held":false,"ramp_lock_lagging_mask":0,
 "fault_reason":"","fault_guard":0,
 "last_run":{"present":true,"interrupted":true,"phase":"running",
             "profile_id":0,"profile_name":"bisque","zone_mask":5,
             "segment_index":1,"segment_count":3,"dwelling":false,
             "target_c":940.00,"segment_elapsed_s":4200,
             "uptime_at_write_s":18300,"fault_guard":0,"fault_reason":""},
 "zones":[{"zone":0,"actual_c":310.20,"actual_valid":true,"relay_on":true,
           "duty":0.750,"control_mode":2,"faulted":false,
           "fault_reason":"","fault_guard":0}, ...]}
```

`last_run` (added 2026-08-12, TODO.md 6A.3's "no auto-resume across reboot")
is the record `run_state.c` persisted for the *previous* run, loaded at boot —
it is deliberately a separate object, never merged with the live fields, and
is `{"present":false}` when there is nothing to report. `interrupted` is the
one an operator actually cares about: true means the firing never reached a
clean ending, i.e. the power went out rather than someone stopping it.
`uptime_at_write_s` is uptime, not wall-clock — the board has no RTC, and
fabricating a timestamp would be worse than admitting that. Nothing acts on
this record: there is no path from it to a run or a relay.
`POST /api/profile_exec/ack_last_run` dismisses it (400 if there was nothing
to acknowledge); the dashboard shows it as a dismissible banner.

Only zones with `active == true` in the run's `zone_mask` appear in `zones[]`.
`control_mode` is `zone_control_mode_t` (0 OFF, 1 BANGBANG, 2 PID);
`fault_guard` is `thermal_guard_trip_t` (0 = none). `fault_reason` strings are
`"`/`\`-escaped. Semantics of every field are in
[`docs/PROFILES.md`](PROFILES.md) and [`docs/PID_CONTROL.md`](PID_CONTROL.md).

### `GET /api/control`

TODO.md 6A.9's control-focused endpoint — the same
`profile_executor_get_status()` snapshot, reshaped for a tuning UI. A
separate endpoint rather than more fields on `/api/profile_exec` because
6A.9 asks for one and a tuning client wants a stable shape independent of the
exec lifecycle.

```json
{"state":"running","zone_mask":5,"target_c":312.50,
 "ramp_lock_held":false,"ramp_lock_lagging_mask":0,
 "zones":[{"zone":0,"control_mode":2,"actual_c":310.20,"actual_valid":true,
           "duty":0.750,"relay_on":true,
           "pid_p":0.4210,"pid_i":0.3300,"pid_d":-0.0060,"pid_ff":0.0000,
           "faulted":false,"fault_guard":0}, ...]}
```

The PID term breakdown is only meaningful for a zone in PID mode; other modes
report zeros. `pid_ff` is always 0 — feedforward is not built (TODO.md 6A.4).
No page consumes this endpoint today.

### `POST /api/profile_exec/start`

Body `id=<0-7>`.

- **200** `{"ok":true}`
- **400** `{"ok":false,"error":"<reason>"}` — the reason comes straight from
  `profile_executor_run()`: no such profile, profile has no segments, profile
  targets no zones, a profile is already running, a thermal guard is latched
  (Stop first), an autotune run is active on a targeted zone, or a segment's
  ramp rate exceeds a participating zone's *current* ceiling. That last one is
  the start-time half of the feasibility check — see
  [`docs/PROFILES.md`](PROFILES.md).
- **400** plain-text `body missing or too large` / `body read failed` /
  `id missing or invalid` for malformed requests. Note the shape difference:
  validation failures inside the executor answer JSON, request-level failures
  answer `httpd_resp_send_err`'s HTML error body. A client must handle both.

### `POST /api/profile_exec/stop`, `/pause`, `/resume`

No body. `stop` → `profile_executor_halt()`, always 200 `ok`, including from
IDLE (it is the acknowledgement that clears a latched guard, so it must never
refuse). `pause` → 400 `nothing running to pause` if not RUNNING; `resume` →
400 `nothing paused to resume` if not PAUSED; both 200 `ok` otherwise.

### `GET /api/history.csv`

`text/csv`, `Content-Disposition: attachment; filename="kiln_history.csv"`.

```
elapsed_s,actual_c,desired_c,duty,guard
0,21.50,21.50,0.000,0
30,24.10,22.00,1.000,0
```

One row per 30 s sample of the current (or last) run, oldest first, for the
run's single representative zone. `guard` is `thermal_guard_trip_t`, so the
dashboard graph can mark trips on the timeline without a second request.
Streamed in 128-row batches via `httpd_resp_send_chunk()`; **500** `out of
memory` only if the two small per-request allocations fail. See
`docs/PID_CONTROL.md`'s closing section for why this is streamed — the
one-big-buffer version returned a real `500 out of memory` against the actual
board.

### Autotune endpoints

Two identification methods share these endpoints, one state machine and one
trace buffer (`autotune_engine.h`): the open-loop **step** test (the default)
and, since 2026-08-12, **relay** feedback (Åström–Hägglund), which bang-bangs
the zone around an operator-supplied setpoint and recovers (Ku, Tu) from the
limit cycle. Which one a run is using is reported as `method`, and it decides
which half of the response is meaningful.

**Neither method has ever completed a run on this board.** The bench unit has
no thermocouples attached, so every on-target attempt aborts on guard 6
(sensor invalid) within seconds; the relay path has never run at all. Every
field below is a wire shape, not an observation.

`GET /api/autotune`:

```json
{"state":"idle|settling|stepping|relay_approach|relay_cycling|done|aborted",
 "method":"step|relay","zone":0,"elapsed_s":120,
 "sample_count":24,"actual_c":85.30,"actual_valid":true,"duty":0.500,
 "abort_reason":"","model_valid":true,
 "k_gain_c_per_duty":812.400,"tau_s":3600.0,"dead_time_s":45.0,
 "proposed_kp":0.00120,"proposed_ki":0.00003,"proposed_kd":0.01000,
 "rule":"simc|ziegler-nichols|tyreus-luyben|unknown",
 "predicted_max_ramp_c_per_hr":210.0,
 "relay_setpoint_c":900.0,"relay_d":0.350,"relay_h_c":2.00,
 "relay_cycles_seen":2,"relay_cycles_target":5,
 "relay_valid":true,"relay_ku":0.00420,"relay_tu_s":840.0,
 "relay_amplitude_c":11.40,"relay_cycles_used":3,"relay_reason":""}
```

Every key is always present — one 900-byte `snprintf`, no conditional
members — so a page never has to test for a field's existence, only for
whether this `method` gives it meaning.

- `state`: `relay_approach` and `relay_cycling` are the relay method's two
  phases and appear only for it, exactly as `settling`/`stepping` appear only
  for the step method. They are separated because only the second one records:
  the approach is an ordinary heat-up under the same relay law, and folding it
  into the trace would put a long monotonic ramp in front of the oscillation.
- `method` mirrors the run's `autotune_method_t`; a zeroed status reads as
  `"step"`.
- `rule` is the tuning rule behind `proposed_*`, reported **by name** rather
  than as an enum value because it is the one part of a proposal an operator
  has to be able to judge: `"ziegler-nichols"` is a warning label (it is
  designed to leave the loop oscillating), and a bare integer would not be.
- `relay_setpoint_c` / `relay_d` / `relay_h_c` echo back the run's parameters
  so a page loaded mid-run can still say what is being done to the kiln.
  `relay_d` is the **half**-amplitude in duty units and `relay_h_c` the
  **half**-width of the switching band, following `pid_autotune.h`'s
  convention throughout — the single easiest way to get Ku wrong by a factor
  of two.
- `relay_cycles_seen` / `_target`: live progress. The target is
  `AUTOTUNE_RELAY_TARGET_CYCLES` (5) — 3 fitted plus 2 discarded as the
  transient converging *toward* the limit cycle.
- The `relay_*` result fields are only meaningful on `state == "done"` with
  `method == "relay"`. `relay_amplitude_c` is again half of peak-to-peak,
  averaged over `relay_cycles_used` trailing complete cycles.
- **`model_valid` and `relay_valid` are never both true.** A step test yields
  a FOPDT model (`k_gain_c_per_duty`/`tau_s`/`dead_time_s`) and no relay
  result; a relay test yields one point of the frequency response
  (`relay_ku`/`relay_tu_s`) and no model at all. Both are cleared at the start
  of every run, so a stale `valid` from the previous run cannot let
  `/accept` write gains this run never produced.
- `predicted_max_ramp_c_per_hr` is step-method-only: it needs `tau`, which a
  relay test never measures.
- `relay_reason` is `relay_model_t.invalid_reason` verbatim, `"`/`\`-escaped,
  and is reported alongside `abort_reason` rather than folded into it: "not a
  limit cycle" and "amplitude inside the hysteresis band" are different
  findings about the kiln and the page must not flatten them.

`GET /api/autotune/matrix` — the cross-zone coupling matrix (TODO.md
6A.5(b)), small enough to send whole:

```json
{"zone_count":3,"cells":[{"i":0,"j":0,"valid":true,"k":812.400,"tau_s":3600.0,
                          "dead_time_s":45.0},
                         {"i":0,"j":1,"valid":false}, ...],
 "rga":{"available":true,"n":2,"det":3.0,"zones":[0,2],
        "lambda":[[1.33,-0.33],[-0.33,1.33]]}}
```

`rga` (added 2026-08-12, TODO.md 6A.5(c)) is the Relative Gain Array computed
from the same snapshot of `K` as `cells`, over the largest principal
sub-block whose every cell is measured — `zones` names which zones that
covers. When it cannot be computed it is
`{"available":false,"code":<autotune_rga_status_t>,"reason":"..."}`;
it refuses on an incomplete matrix, a scale-aware singular one, or any
non-finite entry, because an RGA computed over zero-padding would claim the
zones are independent when nobody has measured whether they are.

No cell has ever been filled against real hardware — see
`docs/PID_CONTROL.md`.

`POST /api/autotune/start` — body cap **192** bytes.

| Field | Required | Applies to | Meaning |
|---|---|---|---|
| `zone` | **yes** | both | 0..255, re-checked by the engine |
| `method` | no | — | `step` (default) or `relay`; **only the literal `relay` opts in** |
| `step_duty` | no | step | defaults to 0.5 if absent |
| `setpoint_c` | **yes for `relay`** | relay | the temperature the kiln is held oscillating at. No default |
| `relay_d` | no | relay | half-amplitude in duty; absent (or ≤0) = engine default 0.35 |
| `relay_h` | no | relay | half-width of the switching band in °C; absent (or ≤0) = engine default 2.0 |
| `rule` | no | relay | `tl` (default, Tyreus-Luyben) or `zn` (Ziegler-Nichols) |

`method` is absent-tolerant on purpose but **not** typo-tolerant: an omitted
field, or an older client that has never heard of it, gets the gentler step
test, while anything that is neither `step` nor `relay` is a 400
(`method must be "step" or "relay"`) rather than a quiet fallback — a typo
must not silently change which test runs on a kiln. `rule` behaves the same
way (`rule must be "tl" or "zn"`); note that `simc` is *not* accepted here,
because it is model-based and a relay test produces no model, so it would
silently propose kp=ki=kd=0.

`setpoint_c` has no default and none could be invented: the operator is
choosing the temperature the chamber will be held oscillating at. Missing it
is a 400 (`relay method requires setpoint_c`). The engine additionally refuses
a setpoint within `AUTOTUNE_RELAY_SETPOINT_HEADROOM_C` (50 °C) of the zone's
`max_temp_c` or `min_temp_c`, up front rather than by aborting later — a relay
test is *designed* to overshoot its band, by however much the dead time
allows, which is the quantity being measured and therefore unknown beforehand.

Responses: 200 `{"ok":true}`, or 400 `{"ok":false,"error":"..."}` — and note
that **parameter rejections take that same JSON route**, not
`httpd_resp_send_err()`'s HTML, because the page parses this response as JSON
and shows `error` verbatim; an error sent the other way would reach the
operator as a silent failure. The exceptions are the request-level failures
that happen before parsing: `body missing or too large`, `body read failed`
and `zone missing or invalid` are still plain `httpd_resp_send_err()`.

`POST /api/autotune/abort` — no body, always 200 `ok`, records the abort
reason `aborted from web UI`.

`POST /api/autotune/accept` — body is optional form-encoded
`ack_unsettled=0|1` and `adopt_ceiling=0|1` (also accepts `true`/`false`; an
absent field, or an older client that predates either one, defaults to `0`).
Both funnel into the single on-target `autotune_engine_accept()` call
(`autotune_accept_opts_t{ack_unsettled, adopt_ceiling}`).

- `ack_unsettled` opts in to persisting a STEP-method fit that reached DONE
  without genuinely settling (or without its extrapolation converging, or
  without tau/L staying consistent with the corrected gain) — see the
  page's own display of the three flags before this is ticked.
- `adopt_ceiling` opts in to also writing the run's predicted ramp ceiling
  (`predicted_max_ramp_ambient_c_per_hr`) into the zone's
  `max_ramp_c_per_hr`, in the same accept action, instead of requiring a
  separate manual edit on this page.

200 response is now `{"ok":true,"ceiling_adoption":"<outcome>",
"ceiling_old_c_per_hr":<float>,"ceiling_new_c_per_hr":<float>}` — the two
ceiling fields are only meaningful when `adopt_ceiling=1` was sent.
`ceiling_adoption` is one of: `ADOPTED`, `SKIPPED_NOT_REQUESTED`,
`SKIPPED_RELAY_METHOD`, `SKIPPED_MODEL_NOT_PERSISTED`, `SKIPPED_ZERO`,
`SKIPPED_WOULD_TIGHTEN` (a stored nonzero ceiling is never silently
tightened), `SKIPPED_READ_FAILED` (the currently stored ceiling could not be
read, so nothing was adopted over an unknown value), `REJECTED_OUT_OF_RANGE`
(the estimate exceeds `ZONE_MAX_RAMP_C_PER_HR_MAX` — refused, never
clamped), or `FAILED_TO_PERSIST` (the estimate was adoptable but the write
itself failed). Or 400 `no completed autotune result to accept` (which
covers both "not DONE" and "DONE but this method's result isn't valid", or
an unsettled STEP fit without `ack_unsettled=1`). This is the **only** path
that writes tuning, through `zones_config_set_pid()`; results are proposed,
never auto-applied.

What gets written depends on the method:

- **step** → gains *and* the fitted FOPDT model, via
  `zones_config_set_model()`, at the same moment and through the same owner.
  A rejected/failed model write is logged, not propagated: the gains are
  already live, and returning failure would tell the operator the acceptance
  failed when the thing they clicked Accept for did land.
- **relay** → **gains only, and deliberately no plant model at all.** A relay
  test measures one point of the frequency response; it does not measure K,
  tau or L, and no combination of (Ku, Tu) recovers them — infinitely many
  FOPDT plants share any given ultimate gain and period. So there is nothing
  to write, and equally importantly nothing to *overwrite*: a model left by an
  earlier step test stays exactly as it was. Clearing it, or synthesising one
  from Ku, would trade a measurement for a guess and do it silently. The
  distinction the code is preserving is between "this run measured no model"
  and "this run measured that there is no model". Consequence for the API: a
  zone tuned only by relay keeps `model_k_dc`/`model_tau_s`/
  `model_dead_time_s` at whatever `GET /api/zones` already reported —
  typically 0, meaning no model identified, and therefore no feedforward.

`GET /api/autotune/trace.csv` — `elapsed_s,measurement_c`, streamed in
128-row batches exactly like the history CSV.

## Zone config (`zones_http.c`)

### `GET /api/zones`

```json
{"thermo_count":3,"relay_count":4,"max_simultaneous_relays":0,
 "zones":[{"index":0,"name":"bottom","relay_mask":3,"cal_offset_c":0.000,
           "pid_kp":0.0012,"pid_ki":0.0000,"pid_kd":0.0100,
           "max_ramp_c_per_hr":150.00,"sanity_rate_c_per_min":0.500,
           "control_mode":2,"max_temp_c":1200.0,"min_temp_c":-20.0,
           "heater_window_ms":60000,"heater_min_on_ms":2000,
           "heater_min_off_ms":2000,"cross_zone_max_delta_c":0.0,
           "model_k_dc":0.0000,"model_tau_s":0.0,"model_dead_time_s":0.0}, ...]}
```

All `MAX31856_CHANNEL_COUNT` zone blocks are always emitted, including ones
past `thermo_count`, so the page can round-trip an unused zone without
special-casing. `max_simultaneous_relays: 0` means unlimited.

The three `model_*` fields are the identified FOPDT plant model —
`zones_config_set_model()`'s stored K, tau and dead time, written by
autotune's step-test acceptance path (a relay acceptance never writes one; see
`POST /api/autotune/accept` above) and consumed by the feedforward term. They
are emitted for **every** zone whether or not a model exists, because an
absent key and a zero would mean the same thing to a client, and always
emitting keeps the page's read-back-and-repost round-trip from depending on
which zones happen to have been autotuned. 0 in any of the three means "no
model identified". `model_k_dc` carries four decimals rather than the usual
one because a small-gain zone's fit can land in the fractional range and the
feedforward divides by it.

Built into a single 2048-byte buffer (1024 → 1536 with the heater timing
fields, → 1792 with `cross_zone_max_delta_c`, → 2048 with the three plant-model
fields) using the same truncate-and-send `APPEND` macro `/api/status` uses: an
overflow jumps straight to the send, so a truncated response is served rather
than an error.

### `POST /api/zones`

Whole-page submit. Top-level fields `thermo_count`, `relay_count`, optional
`max_simultaneous_relays`; then 14 required fields per zone `i`, named
`z<i>_name`, `z<i>_relay_mask`, `z<i>_cal`, `z<i>_kp`, `z<i>_ki`, `z<i>_kd`,
`z<i>_ramp`, `z<i>_sanity`, `z<i>_mode`, `z<i>_maxtemp`, `z<i>_mintemp`,
`z<i>_window`, `z<i>_minon`, `z<i>_minoff`, plus four **optional** ones added
2026-08-12: `z<i>_xzone` (guard 8's threshold) and the plant-model trio
`z<i>_k` / `z<i>_tau` / `z<i>_deadtime`.

The optional four are each probed with `http_form_find_field()` first and only
parsed if present, so the MCP path and older harnesses that post the original
14 keep working. The two groups then differ in what absence means, and the
difference is deliberate: an absent `z<i>_xzone` is whole-page-submit
semantics — 0, guard 8 disabled, *clearing* any previously saved threshold —
while an absent model field leaves the stored model untouched, so an unrelated
edit on the page cannot wipe an identified model the operator never saw a
control for. In either group, present-but-invalid is still a rejection.

The body cap is `ZONES_BODY_MAX` = **3200** (2048 → 2560 with the heater timing
fields, → 2816 with `cross_zone_max_delta_c`, → 3200 with the plant-model trio,
which costs ~96 bytes a zone in the worst case), and the handler stacks a
3201-byte buffer for it — the reason the server runs an 8192-byte stack.

Ranges (firmware sanity bounds against typos, **not** kiln-safety limits):

| Field | Range | Notes |
|---|---|---|
| `thermo_count` | 0..`MAX31856_CHANNEL_COUNT` | |
| `relay_count` | 0..`KILN_IO_RELAY_COUNT` | |
| `max_simultaneous_relays` | 0..`KILN_IO_RELAY_COUNT` | optional; absent = 0 = unlimited. Present-but-invalid is still rejected |
| `z<i>_name` | ≤15 chars | optional; absent = empty |
| `z<i>_relay_mask` | 0..255, but no bit past `relay_count` | |
| `z<i>_cal` | -50..50 °C | |
| `z<i>_kp` / `_ki` / `_kd` | 0..1000 | no natural physical bound; generous headroom |
| `z<i>_ramp` | 0..1000 °C/hr | 0 = never configured |
| `z<i>_sanity` | 0..20 °C/min | 0 = never configured → executor substitutes its default |
| `z<i>_mode` | 0..2 | `zone_control_mode_t` |
| `z<i>_maxtemp` | 0..1400 °C | matches `PROFILE_TARGET_C_MAX`; 0 = no ceiling |
| `z<i>_mintemp` | -50..200 °C | page defaults to -20 |
| `z<i>_window` | 0..600000 ms | 0 = not configured |
| `z<i>_minon` / `_minoff` | 0..60000 ms | 0 = not configured |
| `z<i>_k` / `_tau` / `_deadtime` | 0..`ZONE_MODEL_K_MAX` (5000 °C/duty), 0..`ZONE_MODEL_TIME_MAX_S` (86400 s) each | **optional** for the same backward-compatibility reason as `z<i>_xzone`, but absence means the opposite of it: each is written only if present, so omitting them leaves the stored model alone. The identified FOPDT model, written by autotune's *step*-test acceptance path (`zones_config_set_model()`), shown read-only on the page and echoed back on save. Same bounds as `zones_config_set_model()` itself, so both paths accept exactly the same set of models. 0 in any of the three means "no model identified" |
| `z<i>_xzone` | 0..1000 °C | **optional** — one of the zone fields a submit may omit, so the MCP path and older harnesses that post the original 14 keep working. Absent = 0 = guard 8 disabled, which also *clears* a previously saved threshold (whole-page-submit semantics). Present-but-invalid is still rejected |

Zones at or past `thermo_count` have only their name parsed; their numeric
fields are not required and not range-checked against `relay_count`.

**A save during a firing takes effect on the next control tick** (TODO.md
6A.7, 2026-08-12). `profile_executor.c` watches `zones_config_generation()`
and re-reads every active zone when it moves: gains transfer bumplessly, mode
and relay-mask changes force that zone's relays off first, heater timing
applies at the next window, and every guard-threshold change is logged at WARN
as an operator action. A rejected (400) submit changes nothing and does not
move the generation. Two things are still run-start-only: the active zone set
(shrinking `thermo_count` mid-run logs at ERROR and keeps the zone, rather
than dropping it out of a firing the profile was validated against) and
`max_ramp_c_per_hr`, which gates a run when it starts and is not re-checked
against one already running.

200 `ok`, or 400 with a specific reason naming the offending field (e.g.
`zone relay_mask references an unconfigured relay`, `zone pid_kp missing or
out of range`). Nothing is applied unless everything validates.

## Profiles (`profiles_http.c`)

| Route | Shape |
|---|---|
| `GET /api/profiles` | `[{"id":0,"name":"bisque","zone_mask":5,"segment_count":3}, ...]` — used slots only |
| `GET /api/profile?id=<n>` | `{"id":0,"name":"bisque","zone_mask":5,"segment_count":3,"segments":[{"target_c":100.00,"ramp_c_per_hr":60.00,"dwell_min":30}, ...]}` |
| `POST /api/profile` | create/overwrite; `{"ok":true,"id":<n>,"warnings":[...]}` or 400 `{"ok":false,"error":"..."}` |
| `POST /api/profile/delete` | body `id=<n>`; 200 `ok`, 400 `id missing or out of range`, 404 `no such profile` |
| `GET /api/profiles/builtin` | visible built-ins only, same shape as `GET /api/profiles`; `?all=1` also includes hidden ones |
| `POST /api/profile/builtin/hide` | body `id=<n>&hidden=0\|1` (id 128+); sets one built-in's bit in the NVS hidden mask (either direction) — a `const` table in flash can't be deleted, so "removable" is a hide, not an erase |
| `POST /api/profile/builtin/restore` | no body; clears the hidden mask, bringing every built-in back |

`GET /api/profile` is the only endpoint on this server that takes a **URL
query parameter** rather than a form body; it answers 400 `id missing` or 404
`no such profile`. `id` may be a user slot (0-7) or a built-in (128+n) — same
endpoint, same shape, either id space. Field names, bounds, slot allocation
and the feasibility check that produces `warnings[]` are in
[`docs/PROFILES.md`](PROFILES.md), which also covers the built-in catalogue
(2026-08-20) in full — this table only adds its wire shape.

## `/api/sim` — development builds only

Registered by `sim_backend_register_http()`, which compiles to nothing unless
`CONFIG_KILNCTL_SIM_PLANT` is set. **Neither handler exists in a production
image**, and the option defaults to `n`.

`GET /api/sim`:

```json
{"simulated":true,
 "zones":[{"zone":0,"element_c":812.4,"reading_c":798.1,"relay_on":true,
           "fault":"none"}, ...]}
```

`reading_c` is `null` (not `nan`) for an unreadable simulated sensor.
`element_c` is the model's true element temperature — the ground truth a real
board cannot report.

`POST /api/sim`: body
`zone=<n>&fault=none|element_dead|relay_welded|tc_detached|tc_frozen|tc_open`.
200 `{"ok":true}`; 400 for a missing field, an unknown fault name, a
zone out of range, or a missing/oversized body; 500 `sim unavailable` if the
module's mutex could not be created.

A sim build reports fabricated temperatures while still writing to a real
expander if one is attached. See `docs/PID_CONTROL.md`'s warning: it must not
be flashed to a board wired to elements. The path has never been run on
hardware.

## Relationship to the UART protocol

The HTTP API and the hardened UART protocol
([`docs/UART_PROTOCOL.md`](UART_PROTOCOL.md)) are two transports onto the
same device state, not two subsystems. Both ultimately read `kiln_io` and
`MAX31856` and both pass a relay-ON command through the same
`relay_authority` chokepoint. There is no HTTP-only or UART-only path to
energizing a relay.

Where they differ, and it matters:

- **Refusal visibility.** A safety-refused relay command over UART produces
  **no explicit refusal signal on the wire**: the frame is still ACKed at the
  transport level (it was delivered), no state changes, and like every other
  `SET_*` in that task there is no task-level reply either way. The only way
  to observe the refusal is that the next `READ`/auto-report still shows the
  relay off. Over HTTP, `POST /api/diagnostics/danger/relay` (the
  `/api/relay` manual-override endpoint's replacement since 2026-08-27, see
  above) answers **403 `blocked by safety fault`** — an explicit, immediate
  refusal. Same policy, same chokepoint, very different observability.
- **Push vs. poll.** The UART link pushes: THERMO/IO `SET_AUTO_REPORT`
  deliver unsolicited frames, and IO additionally pushes on every `~INT`
  edge. The web UI has no equivalent and polls at 2 s — the gap TODO.md
  section 2's last bullet tracks.
- **Calibration.** `/api/status` and the profile executor apply the per-zone
  `cal_offset_c`; the UART bridge reports the MAX31856's raw value. Two
  clients can therefore legitimately disagree about the same channel's
  temperature by that offset.
- **Scope.** Profiles, zone config and autotune are HTTP-only — there are
  no UART task IDs for any of them (the relay-rule DSL editor was too,
  before it was deleted 2026-08-27). Conversely the display
  (task 4), raw SX1509 register access, link statistics and the firmware
  version/pin-config queries are UART-only, with no HTTP equivalent.
- **Failure semantics.** A failed operation on the UART side produces no
  reply frame at all, so the PC sees a timeout rather than a fabricated
  value. HTTP always answers something — the discipline that replaces it is
  that an unreadable channel is reported with `valid: false` rather than a
  plausible number.

`pc_tools`' GUI reaches both: its device pages talk UART, and its "Firing
Status" popup polls this HTTP API the same way its Wi-Fi Settings popup does
(read-only by design — anything that starts a firing or writes tuning stays a
web-dashboard action). See `docs/PROJECT_STATUS.md`, 2026-08-11.
