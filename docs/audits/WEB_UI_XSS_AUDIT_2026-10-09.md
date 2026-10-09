# Web UI XSS / CSRF / redirect audit (2026-10-09)

Scope: the pages KilnFW serves (`firmware/KilnFW/App/drivers/http/*.html`,
`app.js`, `nav.js`, `commissioning_shared.js`, `net/ota_page.html`,
`net/security_page.html`, `net/wifi_provision_page.html`) at origin/dev
`1e6d375f`, plus the firmware JSON emitters that feed them. Read-only review;
no code was changed.

Method: every `innerHTML` / `insertAdjacentHTML` / `outerHTML` statement was
extracted (244 non-trivial sinks) and every concatenated operand that is not
a literal or wrapped in an escaper was traced back to its source (272
operands). Board-data sources that a third party can set: profile names,
zone names, relay names (all 15 bytes max: `PROFILE_NAME_MAX_LEN`,
`ZONE_NAME_MAX_LEN`, `RELAY_NAME_MAX_LEN`), scanned SSIDs (32 bytes, set by
any nearby AP), and imported profile/backup files.

Who can write a name: with web auth ON, every name writer is ADMIN tier, so
stored XSS is admin-to-admin or arrives through an imported profile `.json` /
backup file someone was handed. With web auth OFF, any LAN client can write
names, so a stored payload needs no credential at all.

## Findings

### F1 (Medium) Stored XSS: profile name in the main-page feasibility popups -- FIXED (page side, see Fix status below)

- `firmware/KilnFW/App/drivers/http/main_page.html:2881` (`'<h3>Configured-limit notice — ' + name`)
- `main_page.html:2935` (`'<h3>Feasibility notice — ' + name`)
- `main_page.html:2947` (`'<h3>Start "' + name + '" anyway?</h3>'`, name from line 3054)

`name = profileTitleById[id]`, filled at line 1415 from
`profileDisplayName(p)` (line 1270), which returns the raw `p.title || p.name`
from `/api/profiles`. It is never escaped before reaching `box.innerHTML`.
Elsewhere on the same page the same map is escaped (lines 2284, 2375), so
these three sites are the outliers.

Exploit: a profile saved (or imported from a shared `.json`) under a name
such as `<a href=//e.co>` or `<svg onload=x//` renders as live markup when the
operator clicks the feasibility icon, or presses Start on a profile whose
verdict is not OK. The 15-byte cap keeps a self-contained script payload
tight, but markup and link injection fit, and a handler that calls an
existing global, followed by a `//` comment, also fits.

Fix: `window.kcEscapeHtml(name)` at all three sites, or build the `<h3>` with
`textContent`.

### F2 (Medium) Stored XSS: zone names in the adaptive-tune table -- FIXED (page side, see Fix status below)

- `zones_page.html:4055` (`' (' + current.zones[i].name + ')'`), used at line
  4072 (`'<tr><td>Zone ' + i + name + '</td>'`). The assembled `html` reaches
  `el.innerHTML` in `pollAdaptiveTune()`.

Exploit: the table holds up to three zone names in one HTML string, so a
payload can be split across zones to get past the 15-byte limit. For example,
zone 0 `<svg/onload='/*` and zone 1 `*/eval(name)'>`, with the intervening
markup commented out. That runs script in the zones page (an ADMIN page) for
whoever opens the adaptive-tune card. With auth off, any LAN client can plant
it through `POST /api/zones`.

Fix: `kgEsc(current.zones[i].name)` or `window.kcEscapeHtml(...)`.

### F3 (Medium) Stored XSS: profile name in the firing-stats caption -- FIXED (page side, see Fix status below)

- `zones_page.html:3923` (`caption = rec.profile_name + ' -- ' + ...`),
  emitted unescaped at `zones_page.html:3833`
  (`'<caption>' + caption + '</caption>'`) by `renderFiringStatsTable()`.

The source is `/api/firing_history`. A firing run stores its profile name,
which is the same name F1 covers. It persists in history after the profile is
renamed or deleted, so cleaning up the profile does not remove the payload.
Several records render into the same view.

Fix: escape `rec.profile_name` when building `caption`, or set the caption
with `textContent`.

### F4 (Medium, auth OFF only) DNS rebinding defeats the CSRF origin check

`http_origin_check.h` (called from `http_auth_http.c:314` for every
non-GET/HEAD request) compares the Origin (or Referer) host:port with the
request's own `Host` header. In a DNS-rebinding attack, both headers carry
the attacker's hostname. For example, a page on `rebind.attacker.example`
whose DNS later answers with the board's LAN IP sends
`Origin: http://rebind.attacker.example` and
`Host: rebind.attacker.example`. They match, so the request passes. When web
auth is off, nothing else stops it, so any LAN user who visits a malicious
site can have their browser read every GET route and drive every POST,
including relays, profiles, Wi-Fi and OTA staging. With auth on, the session
cookie is scoped to the board's real origin and is not sent, so only the
auth-off configuration is exposed.

Fix: refuse any request whose `Host` is not one of the board's own names: the
STA IP, the AP IP `192.168.4.1`, and the mDNS hostname if one is advertised.
Apply this to all methods, at least while web auth is off. This closes
rebinding for GETs too, which the origin check never covered.

### F5 (Low-Medium) Narrow JSON escapers let control characters through, which breaks pages (stored DoS)

Several emitters use a "quote and backslash only" escaper:

- `dashboard_json.c:96` `json_escape()`, used for profile names at
  `dashboard_exec_http.c:71,174,365` and `dashboard_json.c:466`
  (`/api/profile_exec`, `/api/firing_history`)
- `zones_http_get.c:59` `zones_json_escape()` (zone names, relay names at line
  336, timing-profile names at line 370)
- `backup_export.c:164` (profile, zone and relay names in the backup file)
- the `wifi_provision_http.c` uses at lines 269, 274 and 423 (saved SSID, AP
  SSID, and scan results)
- the static copies in `diagnostics_http.c:366`, `kiln_cfg_http.c:98` and
  `readiness_http.c:65`

`http_form_find_field()` (`drivers/common/http_form.h`) percent-decodes, and
no name writer filters control bytes. `zones_http_post_parse.c:49`,
`zones_http_post.c:495-505` and `profiles_edit_http.c:43-53` all copy the raw
decoded bytes. A name holding `%0A` or `%01` is therefore stored verbatim and
emitted as a raw control byte inside a JSON string. `JSON.parse` rejects that,
so the page's `r.json()` fails.

Exploit:
- A zone or relay name containing a newline makes `/api/zones` unparseable.
  The zones page, which is the page that would rename it, can no longer load.
  Any page that reads zone data degrades the same way.
- A profile name with a control byte breaks `/api/profile_exec` and
  `/api/firing_history`. The profiles catalog itself uses the correct
  `profiles_http_json_escape()` (`profiles_http_internal.h:90`) and stays
  readable.
- The backup export becomes invalid JSON that cannot be re-imported.
- A nearby AP advertising an SSID that contains a control byte (802.11 allows
  any 0-32 bytes) makes the `/api/wifi/scan` JSON invalid. The provisioning
  page then shows "Network list unavailable" to the operator trying to join a
  network. No credential is needed for that one.

These outputs reach `textContent` or escaped sinks, so this is not an XSS
vector. It is an integrity and availability problem.

Fix: make every emitter use one shared escaper that also emits `\u00XX` for
bytes below 0x20 (`profiles_http_json_escape()` already does this). Then
reject control bytes in name writers at parse time, which also protects the
LCD.

### F6 (Low) Open-redirect bypass in `loginReturnPath()` -- FIXED (page side, see Fix status below)

`login_page.html:67` accepts `return` when `charAt(0) === '/'` and
`charAt(1)` is neither `/` nor `\`. The WHATWG URL parser strips ASCII tab,
LF and CR from anywhere in the input. `/login?return=/%09/evil.example`
therefore decodes to `"/\t/evil.example"`, passes the guard, and
`window.location.href` (line 85) navigates to `//evil.example`.

Exploit: a phishing link to the real board's login page sends the operator to
an attacker look-alike right after a successful login. That look-alike could,
for example, claim "session expired, re-enter password".

Fix: strip `[\t\n\r]` before the checks, or resolve with
`new URL(ret, location.origin)` and require
`u.origin === location.origin`, then navigate to `u.pathname + u.search + u.hash`.

### F7 (Low) No anti-framing header (clickjacking, auth OFF)

No response sets `X-Frame-Options` or a CSP `frame-ancestors`. With auth on,
the `SameSite=Strict` session cookie (`web_auth_login_http.c:556`) is not sent
to a cross-site frame, so a framed page loads unauthenticated. With auth off,
an attacker page can frame `/` or `/zones` and trick a click onto Start,
Stop or a relay toggle.

Fix: send `X-Frame-Options: DENY` and `Content-Security-Policy: frame-ancestors 'none'`
on every HTML response.

### F8 (Low, defence in depth) Firmware-generated strings put into innerHTML unescaped -- FIXED (page side, see Fix status below)

Not attacker-controlled today: each is a firmware constant, an enum name or
numeric text. Each one is a latent XSS the moment any of those strings starts
carrying user data.

- `profiles_page.html:2267`: `result.data.warnings` (built by
  `profiles_edit_http.c` `append_warning`, numeric text today)
- `zones_page.html:3659`: `rga.reason`
- `zones_page.html:1046`: `err.message` inside `kgPanel`
- `main_page.html:1005`: `at.state`
- `main_page.html:2282`: `lr.phase`
- `main_page.html:2377`: `st.state`, also in a class attribute
- `readiness_page.html:173`: `it.status` in a class attribute
- `setup_wizard_page.html:1101` / `1171` / `1175` / `1179`:
  `st.ct_map_reason` is concatenated into `v.text`, which then reaches
  `innerHTML`
- `setup_wizard_page.html:1622`: `state` in a class attribute and label
  (`pillHtml`)

Fix: route these through `kcEscapeHtml` too, so the escaping rule is "every
non-literal", not "every value judged risky".

## Checked and found sound

- **Escapers.** `window.kcEscapeHtml` (`app.js:39`) escapes `& < > " '`. So do
  the page-local `escapeHtml` (live_profile), `esc` (safety_config, ota),
  `kgEsc` (zones) and safety_page's `row()` (line 429), which escapes both
  label and value.
- **main_page.**
  - `zoneName()` (line 1782) escapes.
  - The last-run and exec cards escape the profile name.
  - The aux card escapes relay names.
  - The profile picker and `kcConfirm` use `textContent`.
- **profiles_page.**
  - List labels use `textContent` and segment zones use `createTextNode`.
  - Relay-name options escape (`ioTargetOptionsHtml`).
  - The on/off rule summary uses `textContent`.
- **zones_page.**
  - Relay rows, settings-source options, CT warnings (line 2448), aux titles,
    the name input value (line 1604) and the autotune abort reason all escape.
  - `RELAY_NAMES` labels are constants.
- **wifi_provision_page.**
  - Scanned SSIDs render via `textContent` (line 1029).
  - Every `innerHTML` on the page is a literal.
- **Other pages.**
  - diagnostics: partition labels and cfgfs names escape. The relay and
    thermocouple rows are numeric.
  - setup_wizard: gate reasons, titles, details, errors and relay names escape.
  - safety_commissioning: field names escape. `f.id`/`f.type` come from a
    constant table.
  - ota_page: everything goes through `esc()`.
  - `commissioning_shared.js:224`: change lines feed a text `confirm`, not HTML.
- **Session token.**
  - The session is an `HttpOnly; SameSite=Strict` cookie.
  - No token is stored in `localStorage` or `sessionStorage`, or put in a URL.
    localStorage holds only UI state: the last profile, info-panel open flags
    and the theme.
  - The TOTP reset token is held in a closure, sent once in a POST body
    (`app.js:954-960`) and nulled.
- **Same-origin requests.**
  - Every `fetch`/XHR uses a relative URL, including the OTA stage XHR
    (`ota_page.html:752`).
  - There are no `<form action>` posts, no `eval`/`new Function` and no
    `window.open`.
  - Browsers send `Origin` on these same-origin POSTs, so the new CSRF check
    accepts them as matching.
  - Other redirects go to fixed paths. The `/readiness#` fragment is
    `encodeURIComponent`'d.
- **Firmware escaping.**
  - `profiles_http_json_escape()` handles quote, backslash and control bytes.
  - `dashboard_status_http.c:1069` filters to printable ASCII.

## Summary

| ID | Severity | Area |
|----|----------|------|
| F1 | Medium | main_page profile name, 3 innerHTML sites |
| F2 | Medium | zones_page adaptive-tune zone names |
| F3 | Medium | zones_page firing-stats caption profile name |
| F4 | Medium (auth off) | DNS rebinding passes Origin==Host check |
| F5 | Low-Medium | narrow JSON escapers, control bytes break JSON |
| F6 | Low | open redirect via tab/newline in `return` |
| F7 | Low | no anti-framing header |
| F8 | Low | firmware strings in innerHTML unescaped |

## Fix status (page side)

F1, F2, F3, F6 and F8 are fixed in the page sources: every cited sink now goes through `kcEscapeHtml`/`kgEsc`, and `loginReturnPath()` rejects control characters and whitespace and requires a same-origin `new URL` result. Regression test: `firmware/KilnFW/App/test/test_web_xss_fixes.js`. F4, F5 and F7 are firmware-side and remain open.
