# Web UI code duplication and drift audit — 2026-09-18

Read-only audit of the served web code (`firmware/KilnFW/App/drivers/http/*.html|*.js`,
`firmware/KilnFW/App/drivers/net/*.html`), the recovery firmware's web server
(`firmware/KilnFW_recovery/main/`), and the HTTP handler C files where a
client/server contract is duplicated. Nothing was edited. The failure class hunted
here is narrow: functionality that exists in more than one place and has either
already drifted, or is structured so that it will drift the moment one copy is
edited.

The worked example supplied with the assignment — `settings_page.html`'s
`POST /api/sw_reset` sending no `X-Ota-Mac` header against a route that requires the
OTA handshake — is owned by another session and is not analysed or redesigned here.
It is treated only as the shape to look for elsewhere. Section 2 reports two further
instances of exactly that shape, on the same page, that are not the one being fixed.

Every claim below was verified by reading both copies. Where two copies turned out
to still agree, or turned out to be only superficially similar, that is stated
rather than padded into a finding.

## 1. What is already mechanically guarded, and by what

Four guards exist, and between them they cover much less of the web code than their
existence suggests.

**`firmware/KilnFW/App/test/lint_pages.js`** parses every inline `<script>` in every
served page with `new vm.Script()` and applies a CSS brace-balance and
glued-comment-end heuristic. That is a syntax gate, not a duplication gate. Its only
duplication coverage is two hand-written C↔JS string pairs, compared after
concatenating adjacent C string literals:
`OTA_INTERLOCK_NO_SAFETY_WARNING` in `ota_interlock.h` against `NO_SAFETY_WARNING` in
`http/app.js:124`, and `WATCHDOG_CFG_FIRING_WARNING` in `watchdog_cfg.h` against
`WATCHDOG_CFG_FIRING_WARNING_JS` in `http/main_page.html`. Those two mirrored strings
are genuinely protected; no third pair is.

Its reach has a second limit that matters more than its narrow scope: it is invoked
**only** from `tools/verify.ps1:84`. `tools/run_all_checks.ps1` discovers checks by
globbing `check_*.ps1` (`tools/run_all_checks.ps1:157`), and `lint_pages.js` matches
no such pattern, so a full `run_all_checks.ps1` run does not execute it at all. Any
claim that the page lint "runs in CI" should be checked against which entry point was
actually used.

**Closing note, 2026-09-19:** the reach limit above is closed. `a42ac369` added
`tools/check_lint_pages.ps1`, which `run_all_checks.ps1`'s glob now discovers and
which `tools/verify.ps1`'s `lint` stage was repointed to delegate to, so
`lint_pages.js` runs exactly once per invocation of either entry point and is now
part of the standing check suite. The duplication-coverage scope described above
(two hand-written C↔JS string pairs, nothing more) is unchanged by this fix and
still stands as written.

**`firmware/KilnFW/App/test/check_no_duplicate_commissioning_impl.ps1`** is the one
check in the tree built specifically against this failure class, and it is built
correctly: it is keyed on three distinctive marker strings rather than on file paths,
so a file split or rename cannot silently disarm it. Each marker
(`"a profile is currently firing (state: running)"`,
`"Committed and confirmed by read-back: "`,
`"does NOT match what was just written"`) must appear in exactly one served page —
zero hits fail as vacuous, more than one fails as a duplicate implementation. This
guards the commissioning write/read-back flow and nothing else.

**`tools/check_route_tier_coverage.ps1`**, with its negative test at
`firmware/KilnFW/App/test/test_check_route_tier_coverage.ps1`, guarantees every route
that exists in code has a tier assigned in `route_tier_table.h`, fail-closed. It
guards route *classification* completeness. It says nothing about whether the client
that calls a route sends what that route's handler demands, which is precisely the
gap the worked example fell through.

**`firmware/KilnFW/App/test/webcheck/ui_sweep.js`** is the only existing check whose
stated scope ("overflow, overlap, small tap targets, console errors, dead buttons")
could structurally have caught the worked example, since a button whose fetch returns
400 is a dead button. It needs a live board, and it runs in `run_all_checks.ps1`'s
serial phase 3 as `check_ui_responsive_sweep.ps1`.

So: mirrored C↔JS warning strings (two of them), the commissioning implementation,
and route tier coverage are mechanically held. Client/server header contracts,
escaping helpers, unit conversion, safety word tables, and staleness constants are
not held by anything.

## 2. Confirmed drift — copies that disagree today

### 2.1 Two more unauthenticated mutating calls on the settings page (highest impact)

`http/settings_page.html` contains three POSTs to routes whose handlers require the
OTA challenge/HMAC handshake. One is the known example. The other two are:

- `http/settings_page.html:274` — `fetch('/api/cfgfs/format_confirm', { method: 'POST' })`,
  no headers at all. Handler: `http/cfg_fs_format_http.c:45` calls
  `ota_http_authenticate_request()` as its first action.
- `http/settings_page.html:320` — `fetch('/api/factory_reset', { method:'POST', headers:{'Content-Type':'application/x-www-form-urlencoded'}, body:'scope='+... })`.
  A `Content-Type` header is set; `X-Ota-Mac` is not. Handler:
  `http/factory_reset.c:287`, same authenticate-first structure.

The only client implementation of the handshake anywhere in the web UI is inline in
`net/ota_page.html`; `app.js` exposes no helper for it. Both routes are
`ROUTE_TIER_ADMIN` (`http/route_tier_table.h:287-289`), so an authenticated admin
session does not help: the tier pre-handler and the OTA MAC check are independent
gates, and passing the first does not satisfy the second.

Neither bypass rescues these calls, and this is worth stating precisely because it is
the reason the failure is total rather than configuration-dependent. Both bypasses —
the boot-button window and `http_auth_policy_web_enabled()` — lived *inside*
`ota_http_verify_request()`, which `ota_http_authenticate_request()` reached only
after its unconditional header checks: a 64-character length test that ran first, in
every auth mode, on every board. A request with no `X-Ota-Mac` header could not get
past it. (**2026-09-29 update:** `ota_http_verify_request()` and this whole header
check, and `ota_http_authenticate_request()` itself, were all deleted
outright in the AP-password HMAC retirement (`f0643c98`); the
specific line numbers this section originally cited no longer exist. See git history
at or before commit `a1ca2b13` for the removed code this section describes.)

Observable consequence: pressing "Format config filesystem" (confirm step) or
"Factory reset" on the Settings page fails with HTTP 400 and the body
`missing or malformed X-Ota-Mac header (want 64 hex chars)`, on every board, in every
configuration. These are dead buttons for destructive operations — the operator is
told the reset did not happen, which is at least loud, but two documented recovery
paths are unavailable from the UI.

Ranked first because it is user-facing, total, and affects factory reset and config
filesystem recovery, the two things an operator reaches for when something else has
already gone wrong.

### 2.2 The X-Ota-Mac check is copied seven times, and the recovery copy disagrees on every axis

`ota_http_authenticate_request()` (formerly `http/ota_http.c:929-957`,
documented at the former `http/ota_http.h:160`) used to consolidate the
"header present and exactly 64 hex chars → hex-decode → verify" sequence; it
was deleted outright by `f0643c98` (2026-09-29, "Retire the AP-password HMAC
on the nine main-app admin OTA/reset routes") along with the whole main-app
X-Ota-Mac scheme -- see this file's 2026-09-19 update below and CLAUDE.md's
flash/OTA section. At the time of this finding, six
handlers in the same firmware image did not call it and hand-rolled that sequence
instead:

- `http/ota_http_esp.c:397-415` (esp update)
- `http/ota_http_esp.c:678-696` (esp rollback)
- `http/ota_http_pico.c:309-325` (pico update)
- `http/ota_http_pico.c:456-473` (pico rollback)
- `http/ota_http_recovery.c:113-131` (recovery exit)
- `http/ota_http_recovery.c:226-243` (boot guard reset)

I compared these six against the helper text-for-text. They currently agree exactly,
including all three error strings and the log wording, differing only in the
operation name interpolated into `ESP_LOGW`. Several carry the comment "same order as
every other mutating handler in this file", which is an accurate description of
present state and no protection at all. This is six-fold latent duplication, not
drift — see 3.1.

The eighth implementation is the one that has actually drifted.
`firmware/KilnFW_recovery/main/recovery_http.c:165-185` implements the same wire
contract independently, and at the time of this audit disagreed with the main app's
now-deleted `ota_http.c` HMAC-verify block on all four axes below. (**2026-09-29
update:** the main-app column describes code removed in the AP-password HMAC
retirement -- see the note above. `recovery_http.c:165-185` and its own
mirror-drift check, `recovery_ota_auth_mirror_drift_check.py`, are unaffected and
still describe live code.)

| | Main app, as it read pre-2026-09-29 (`ota_http.c`, since deleted) | Recovery (`recovery_http.c:165-185`) |
|---|---|---|
| Error string, absent header | `missing or malformed X-Ota-Mac header (want 64 hex chars)` | `missing X-Ota-Mac` (line 174) |
| Error string, bad hex | `X-Ota-Mac must be 64 hex characters` | `malformed X-Ota-Mac` (line 179) |
| Length check | explicit `httpd_req_get_hdr_value_len() != 64` before any read | none; `strlen(in) != out_len * 2` inside `hex_decode` (line 80) |
| Hex validation | `ota_http_hex_decode()` nibble table, rejects any non-hex byte | `sscanf(in + i*2, "%2x", &v)` per byte |
| Ordering | header → hex → verify (lock, lockout, nonce) | **lockout first** → header → hex → nonce → password → HMAC → compare |
| Boot-button / web-auth bypass | both present inside `ota_http_verify_request()` | neither |
| Crypto backend | PSA | `mbedtls_md_hmac()` |

Two differences here are behavioural, not cosmetic. First, ordering: the recovery
firmware answers **429 `locked out, retry later`** to a request carrying no header at
all, where the main app answers 400 and names the missing header. A client probing
both images gets a different diagnosis of the same malformed request depending on
which firmware is running, and the 429 is actively misleading — it says "back off",
not "your request is malformed". Second, `sscanf("%2x")` is a weaker validator than
the nibble table: it accepts leading whitespace and a `0x`-style prefix in positions
where the table rejects them, so some inputs the main app refuses as malformed are
accepted and hex-decoded by the recovery image. That is a permissiveness difference
in an authentication path, and it is on the more privileged of the two images.

`recovery_http.c`'s header comment (lines 1-32) documents deliberate divergence on
the crypto backend and on refusing to decode `boot_guard`'s struct. It does not claim
the error strings or check ordering were deliberately changed, and the PC-side client
(`tools/PcTools/src/kilnctrl/ota_http_client.py:198`) documents itself as matching
"the exact order `ota_esp_post_handler()`" uses — i.e. against the main app's order,
not recovery's.

**2026-09-19: fixed, guarded by `check_recovery_ota_auth_mirror.ps1`.**
`recovery_http.c`'s `ota_esp_post()` now matches `ota_http_authenticate_request()`
on all four axes above: same wire strings (including the header-read-failure
string, "could not read X-Ota-Mac header", found missing from the table above
during review), an explicit `httpd_req_get_hdr_value_len() != 64` check before any
read, a strict `[0-9a-fA-F]` nibble table replacing `sscanf`, and header/hex
validation running before the lockout check. `firmware/KilnFW_recovery/main/
recovery_ota_auth_mirror_drift_check.py` (wired into `run_all_checks.ps1` as
`check_recovery_ota_auth_mirror.ps1`) now pins the hex-decode nibble logic and the
handler's check ordering/wire strings against the main app's copy so this cannot
silently drift again. The crypto-backend and `boot_guard`-non-decode differences
above remain unchanged, as documented -- both are deliberate.

**Open follow-up, not fixed here:** `recovery_http.c`'s `boot_guard_reset_post()`
(`POST /api/ota/esp/boot_guard_reset`) and `sw_reset_post()` (`POST /api/sw_reset`)
have no `X-Ota-Mac` check at all -- any request to either route is accepted
unauthenticated. The main app authenticates the equivalent operations: its
boot_guard-reset path only runs after `flash_firmware()`'s own verified
post-flash success calls it with an `ap_password` (see CLAUDE.md's flash section),
and, at the time of this finding, `sw_reset_http.c` claimed
`ota_http_verify_request(OTA_HTTP_CONTEXT_SW_RESET, ...)` before acting, per
`ota_http.c`'s context table -- that table (and the whole main-app X-Ota-Mac
scheme it served) was removed outright by `f0643c98` (2026-09-29); the main
app's `sw_reset_http.c` now gates on ADMIN-tier session auth alone
(route_tier_table.h). The recovery
image's two equivalents are reachable by anyone who can reach the board's
recovery-mode AP with no password check at all. This is a real gap on the more
privileged of the two images (recovery mode already implies OTA write access via
`/api/ota/esp`, which *is* authenticated) and should be closed in a follow-up
pass, not folded into this fix, which was scoped to the drifted `X-Ota-Mac` check
on `/api/ota/esp` only.

**2026-09-19: closed.** The auth sequence that used to live inline in
`ota_esp_post()` (header length/read, hex decode, lockout, nonce, password,
HMAC, constant-time compare) is now factored into a shared
`recovery_authenticate_request()` in `recovery_http.c`, and
`boot_guard_reset_post()` and `sw_reset_post()` both call it before acting,
returning the same status/body shape `ota_esp_post()` does on any failure.
This closes the gap by construction rather than by a ninth hand-copy of the
check -- a future new mutating route in this file gets authentication for
free by calling the helper. `recovery_ota_auth_mirror_drift_check.py` (still
wired in as `check_recovery_ota_auth_mirror.ps1`) was extended two ways: its
ordering/wire-string comparison now targets `recovery_authenticate_request()`
instead of `ota_esp_post()` (which is now just a caller), and a new
route-coverage assertion parses `recovery_http.c`'s `routes[]` table itself
(every entry whose `.method` is `HTTP_POST`/`HTTP_PUT`/`HTTP_DELETE`/
`HTTP_ANY`, currently `ota_esp_post`/`boot_guard_reset_post`/`sw_reset_post`)
rather than checking against a hand-maintained list, and fails if any
discovered handler no longer calls the helper -- so a future unauthenticated
mutating route fails this check instead of sitting undetected next to it,
which is exactly how these two got missed the first time; because the
handler set is derived from `routes[]` rather than hand-copied, that claim
holds even for a route nobody remembered to add to a list by hand, which an
earlier hand-maintained-list version of this check could not say. A
follow-up review found the auth helper also hardcoded the HMAC context to
`"esp"` for all three routes (mismatching the main app's/PC client's
per-route contexts) and shared one lockout across all three routes (so
failures against one route could lock out an unrelated one); both are fixed
in the same pass -- the helper now takes `context`/`lockout` parameters and
each route passes its own (`"esp"`/`"boot-guard-reset"`/`"sw-reset"` with
`s_lockout_esp`/`s_lockout_boot_guard_reset`/`s_lockout_sw_reset`). The
route-coverage substring test now also strips comments first, so a
commented-out call to the helper does not count as coverage. Negative-tested:
removing the call from `sw_reset_post()` failed the check with a
named-handler message; adding an unauthenticated fake `/api/wipe` POST route
to `routes[]` also failed the check (new route-discovery coverage, not just
the fixed three); restoring by hand in both cases reproduced the exact
previously-committed blob (`git diff` clean, `git hash-object` match) before
re-confirming green. `check_00_kilnfw_recovery_target_build.ps1`,
`check_recovery_image_size.ps1` and `check_recovery_ota_auth_mirror.ps1` all
pass. Fixed in `dd15cb24`, hardened further in `8dafa48f` on top.

### 2.3 `/api/boot_guard` and `/api/partitions` return different JSON shapes in the two images

Same route, same method, two incompatible response bodies:

- `GET /api/boot_guard`: `http/ota_http_recovery.c:290` sends
  `{"boot_count":%lu,"recovery_mode":%s}`; `recovery_http.c` sends
  `{"record_present":%s,"record_len":%u}`. No field name is shared. A client reading
  `boot_count` gets `undefined` against a recovery-mode board.
- `GET /api/partitions`: `http/partition_info_http.c:98` sends
  `{"running":"%s","partitions":[...]}`; `recovery_http.c` sends
  `{"running":"%s","running_offset":"0x%06x","next_update":"%s"}`.

The partitions divergence has a confirmed consumer.
`tools/PcTools/src/kilnctrl/partition_http_client.py:104` raises
`GET /api/partitions response missing 'running'/'partitions'` when `partitions` is
absent, and `:108` requires it to be a list. So `fetch_partitions()` — and therefore
`flash_firmware()`'s post-flash verification step, which per CLAUDE.md polls
`/api/partitions`' RUNNING marker — fails with a "malformed response" style error
against a board running the recovery image, rather than reporting the accurate and
much more useful fact that the board came up in recovery mode. Given that CLAUDE.md
records the board being walked into recovery mode by ordinary flashing three separate
times, this is the failure path most likely to be hit, and it misdiagnoses itself.

The `boot_guard` divergence is lower impact: `recovery_http.c`'s own header comment
explains that the recovery image deliberately refuses to decode `boot_guard`'s struct
and can only report that a record exists. That is a defensible design choice badly
expressed — the same route name now means two different questions.

### 2.4 `safety_config_page.html`'s `esc()` is a weakened copy of `kcEscapeHtml`

Four HTML-escape helpers exist. `http/app.js:39-47` (`kcEscapeHtml`) is canonical and
escapes five characters: `& < > " '`. `http/zones_page.html:788-791` (`kgEsc`) and
`net/ota_page.html:396-399` (`esc`) each duplicate all five with identical behaviour.
`http/safety_config_page.html:190-193` (`esc`) stops after `&quot;` and omits the
`'` → `&#39;` replacement.

That is a real behavioural difference, verified by reading all four bodies. Its
present impact is nil, and I checked rather than assumed: the four call sites
(`:217`, `:236`, `:255`) interpolate into element text and into `value="..."` and
`<option value="...">` attributes that are delimited by double quotes, which the
helper does escape. A single quote in a thermocouple preset name or zone name renders
as a literal apostrophe and breaks nothing.

The drift is confirmed and the consequence is latent. It matters because the next
person to add an attribute to this page using single-quoted delimiters — the
prevailing style in the surrounding JS string concatenation — gets an escaping hole
in the page that configures safety limits, and the weakened helper looks exactly like
the three correct ones at a glance.

**Fixed 2026-09-22.** `safety_config_page.html`'s local `esc()` now delegates to
`window.kcEscapeHtml` directly (app.js is already loaded on this page) instead of
keeping a second, driftable copy. `tools/check_html_escape_helpers.ps1` was added to
catch this class mechanically going forward: it finds every HTML-escape-shaped helper
under `firmware/KilnFW/App/drivers/http/**/*.html` and `app.js` and fails if one is
missing a replacement for any of the five characters `& < > " '`.

### 2.5 Three temperature-delta formatters, two of which are wrong for Fahrenheit

- `http/app.js:230-234` — `kcUnit.toDisplay`, `return get() === 'f' ? (c * 9 / 5 + 32) : c;`
  Correct for an absolute temperature.
- `http/main_page.html:908-911` — `fmtDeltaC`, multiplies by 9/5 with no `+32`,
  `toFixed(1)`, appends `kcUnit.label()`.
- `http/zones_page.html:3603-3607` — local `d()`, multiplies by 9/5 with no `+32`,
  `toFixed(2)`, no label; callers strip a leading `+` with `.replace('+','')`.

The two delta formatters are *correct* to omit `+32` — a temperature difference
converts by ratio only. The drift is between the two of them and, more importantly,
the fact that three separate conversions of the same physical quantity exist with the
canonical one being the one that must not be used for deltas. Observable difference
today is precision and labelling only: the same 2.5 °C delta reads `4.5 °F` on the
dashboard and `4.50` on the zones page. Ranked last in this section because no value
displayed today is wrong.

### 2.6 Safety word tables: two JS mirrors, three C tables, all differently worded

`safety/safety_trip_words.h` holds three C tables — short form (lines 32-46), cause
prose (64-80), remedy prose (238+) — plus `safety_warn_words_short()` (line 269) with
its own eight-entry warn-bit table.

Two JS tables mirror parts of this independently:

- `http/main_page.html:1675-1685` — `SAFETY_TRIP_WORDS`, comment says "Keep in sync
  with that C table", referring to SaftyFW's `safety_guards.h` enum. Its wording
  matches neither the C short form nor the C cause prose; it is a third vocabulary.
- `http/safety_page.html:445-453` — `WARN_MASK_WORDS`, a second independent mirror,
  this one of `safety_warn_words_short()`'s bit table.

I compared `WARN_MASK_WORDS` against the C table bit by bit. The C table covers bits
3, 4, 9, 10, 12, 13, 14, 15 with strings like `"S10 safety TC disagrees with zone TCs"`
and, for an unlisted bit, deliberately refuses to say "none" —
`"unrecognised warn bits 0x%04X"`. The JS mirror does not reproduce that
unrecognised-bit fallback, so a warn mask containing a bit added to firmware but not
to the page renders as nothing at all on the Safety page while the C side would name
it as unrecognised. That is the drift: not a wording mismatch that a user would
notice today, but a *silent omission* where the C side was explicitly written to be
loud.

`main_page.html`'s table is lower impact than it looks, and I checked before ranking
it: the live path prefers server-decoded trip fields and falls back to this table only
against firmware old enough not to send them. It is a compatibility shim whose drift
shows up only on old firmware.

## 3. Latent duplication with nothing keeping it in agreement

### 3.1 Six hand-rolled copies of the OTA header check (see 2.2)

All six currently agree with `ota_http_authenticate_request()` exactly. Nothing
enforces that. If a security fix lands in the shared helper — a constant-time
comparison, a tightened length check, an added audit log — six handlers keep the old
behaviour, and four of them are the actual firmware update paths. The failure would be
silent: every one of the six still returns 400 on a malformed header, so no test that
merely asserts "malformed header is refused" would notice. `test_ota_http.c` does
exactly that kind of assertion (`:864`, `:1349`, `:1663` check status 400 and that the
message contains `X-Ota-Mac`), so the existing tests would stay green through a
divergence.

### 3.2 `SAFETY_LINK_STALE_MS` and the TRIPPED diag-state literal

`safety/safety_link.h:423` defines `1500u` authoritatively. Two JS copies exist:
`http/main_page.html:1709` and `http/zones_page.html:786` (as `KG_DIAG_STALE_MS`, whose
comment cross-references main_page rather than the C header). Both currently read
1500. Separately, `diag_state === 4` meaning TRIPPED is written three times:
`http/main_page.html:1708`, `http/zones_page.html:785`, and `http/safety_page.html:248`
as a bare `4` with a `// TRIPPED` comment.

Changing the C constant leaves both pages judging staleness on the old threshold —
they would show stale-data indications at the wrong moment, in opposite directions
depending on which way the constant moved. Renumbering the diag-state enum is worse:
`safety_page.html:248`'s bare literal would silently point at whatever state took
slot 4.

### 3.3 `login_page.html` shares nothing with `app.js`

Every served page loads `<script defer src="/nav.js">` and `/app.js` except
`http/login_page.html`, which loads neither and hand-rolls its own `fetch` to
`/api/auth/login` plus its own 429 message
(`Too many attempts -- try again later.`). Meanwhile `app.js:955-990` owns session
polling, the inactivity lock prompt, and the `role === 'none'` expiry transition.

This one I judge correct as-is, and say so here rather than in section 4 because the
reasoning is the interesting part: the login page must work when there is no session,
and loading the session poller on it would have `pollSession()` observing
`role: 'none'` and potentially redirecting away from the login form. The duplication
is deliberate isolation. The only latent cost is that the page's 429 copy and error
styling will drift from the rest of the UI, which is cosmetic.

### 3.4 Thirteen direct `window.confirm` calls bypassing `kcConfirm`

`app.js:101-103` defines `kcConfirm` as a one-line wrapper around `window.confirm`.
Thirteen call sites across `kiln_configs_page.html` (`:282`, `:431`, `:468`),
`main_page.html:2672`, `zones_page.html` (`:2082`, `:3908`), `net/ota_page.html`
(`:549`, `:607`, `:638`), `net/security_page.html:453` and
`net/wifi_provision_page.html:941` call `confirm()` directly instead.

Today `kcConfirm` adds nothing, so there is no drift and no defect. The seam exists
precisely so confirmation can later become a themed modal — LCD pages must not scroll,
and a native `confirm()` on a 320x480 panel is the kind of thing that gets replaced.
The moment `kcConfirm` grows a real implementation, eleven destructive confirmations
keep using the browser dialog, including "Start firing" and "Clear the administrator
password".

## 4. Remedies, and whether each is worth doing

**2.1, the two dead Settings buttons — worth doing, and the highest-value item here.**
The fix is not a redesign: the handshake client already exists inline in
`net/ota_page.html`, and another session is by assumption already extracting or
reimplementing it for `/api/sw_reset`. These two call sites should be routed through
whatever that session lands, and the correct sequencing is to wait for it rather than
build a second extraction in parallel. What is worth doing independently and now is
recording that `/api/cfgfs/format_confirm` and `/api/factory_reset` are the same
defect, so they are not left behind when the `sw_reset` fix is declared complete —
that is the specific way this class of bug survives its own fix.

**2.2 / 3.1, the eight X-Ota-Mac implementations — worth doing, in two unequal halves.**
Converting the six in-image handlers to call `ota_http_authenticate_request()` is
mechanical, removes roughly 120 lines, and the helper was written for exactly this.
Do it. Reconciling `recovery_http.c` with the main app is a different proposition: the
separate crypto backend is deliberate and documented, and the two images cannot share
a translation unit. The right scope there is to align the three error strings and the
check *ordering* (move the lockout test after the header/hex validation, so a
malformed request is diagnosed as malformed) and to replace `sscanf("%2x")` with a
strict nibble decode. The ordering change is the one with real value: a 429 in
response to a missing header will mislead whoever debugs the recovery image next.
Full consolidation is not worth it and should not be attempted.

**2.3, the two JSON shapes — the partitions half is worth doing, the boot_guard half is not.**
For `/api/partitions`, the cheapest correct fix is on the client, not the firmware:
`partition_http_client.py` should detect the recovery shape (a `running` field with
`next_update` and no `partitions`) and raise "board is running the recovery image"
instead of "response missing 'running'/'partitions'". That converts a misdiagnosis
into the single most useful sentence the tool could emit, and it does not require
enlarging the recovery image, which is size-constrained (there is a dedicated
`check_recovery_image_size.ps1`). For `/api/boot_guard`, leave the payloads alone —
the divergence reflects a real capability difference — but the recovery image's route
should be renamed or should include an explicit
`"image":"recovery"` discriminator so a client can tell which question it just asked.

*Closing note, 2026-09-19:* the `/api/partitions` half landed, as a raise rather than
a normalize. `partition_http_client.get_partitions()` now raises
`RecoveryImageResponse` (a `PartitionHttpError` subclass carrying `running`,
`running_offset`, `next_update`) instead of returning a shape with an
empty `partitions` list — an earlier version of this fix normalized to
`partitions: []`, which was rejected in review because it let a recovery-mode
board misread as "board has no partitions" rather than "board didn't answer with
a table at all." `mcp_server_flash._verify_flash_landed()` and
`_preflash_board_address()` both catch it explicitly;
`partition_table.read_chip_partition_table_from_http()` and
`_check_app_flash_offset_matches_chip()` let it propagate/fall through to their
existing `PartitionHttpError` handling unchanged. The `/api/boot_guard`
`"image":"recovery"` discriminator half of this recommendation remains open.

**2.4, the weakened `esc()` — worth doing, and it is a two-line change.**
Either add the missing `'` replacement, or better, delete all three page-local copies
(`safety_config_page.html:190`, `zones_page.html:788`, `ota_page.html:396`) and use
`kcEscapeHtml`, which every one of those pages already loads. The three copies exist
for no reason I could find. If only one thing from this report is done beyond 2.1,
this is the cheapest.

**2.5, the three formatters — not worth consolidating, worth one comment.**
The two delta functions are correct and differ only in precision, which is a
legitimate per-page choice (dashboard at 0.1, zones diagnostics at 0.01). Merging them
would force a shared precision on two contexts that want different ones. The real
hazard is that `kcUnit.toDisplay` is adjacent to them and silently wrong for deltas;
a one-line comment on `app.js:230` saying so is the whole remedy.

**2.6, the safety word tables — partly worth doing.**
Adding the `unrecognised warn bits 0x%04X` fallback to
`safety_page.html:445-453` is worth doing: it restores the loudness the C side was
deliberately written to have, and costs three lines. Consolidating the tables
themselves is not worth it — they are four different vocabularies for four different
presentation contexts (terse status line, cause prose, remedy prose, dashboard
fallback), and the correct long-term direction is the one the live path already
takes, namely having the server send decoded strings so the JS tables can eventually
be deleted rather than synchronised. `main_page.html:1675-1685` should be left alone
and allowed to die with old-firmware support.

**3.2, the staleness constant and diag-state literal — worth a check, not a refactor.**
The JS cannot include a C header, so some duplication is unavoidable. This is a good
candidate for the same marker-keyed treatment as
`check_no_duplicate_commissioning_impl.ps1`: a check that greps `safety_link.h` for
`SAFETY_LINK_STALE_MS`'s value and asserts the same number appears in both pages.
That is a narrow, honest mirror-drift check with a stable pair, which is the bar
CLAUDE.md sets for this kind of check. Replacing `safety_page.html:248`'s bare `4`
with a named constant is worth doing on its own merits.

**3.3, `login_page.html` — acceptable as-is.** Deliberate isolation. Do not
"fix" it by loading `app.js`; that risks the session poller redirecting away from the
login form.

**3.4, the `kcConfirm` bypasses — worth doing only when `kcConfirm` grows teeth.**
Converting thirteen call sites today has zero behavioural effect and carries a small
risk of typo. The remedy that actually holds is to convert them *as part of* whatever
change gives `kcConfirm` a real implementation, and to add a check at that time
forbidding bare `confirm(` in served pages. Noting it now is enough.

## 5. Checked and found clean

- **`http/nav.js:73-120`** — the menu entry list is genuinely single-source. No page
  hard-codes a duplicate nav. This is the pattern the rest of the UI should follow.
- **`thermalGuardWords` / `THERMAL_GUARD_WORDS`, `app.js:81-89`** — previously
  duplicated in `main_page.html` and `diagnostics_page.html`, now correctly
  centralised with only one definition repo-wide. Already fixed; no action.
- **`kcSafetyTcIsSeparate`, `app.js:66-70`** — one definition, no copies.
- **`kcFetchWithSafetyAck`, `app.js:128-158`** — the 428 / operator-ack / retry-with-
  `X-Ota-Ack-No-Safety` flow exists once. This is the counter-example that shows the
  shared-helper approach works when someone bothers: the safety-ack contract is
  strictly more complex than the OTA MAC contract, and it has one implementation
  while the simpler one has eight.
- **Three uptime/duration formatters** — `diagnostics_page.html:529 fmtUptime`
  (`d hh:mm:ss`), `main_page.html:1311 fmtDuration` (`12m 34s`) and `fmtTick`
  (`m:ss`), `zones_page.html:3562 fmtStatsElapsed` (`12m34s`). Read all four. They
  produce four genuinely different output formats for four different display
  contexts. Superficially similar only; consolidating them would be wrong and is
  explicitly not recommended.
- **No third C trip-word table** — `safety_trip_reason_words` appears only in
  `safety/safety_trip_words.h`; there is no `control/profile_executor.c` copy.
- **`http/settings_page.html:258`** — `fetch('/api/cfgfs/format_pending')` is a GET
  against a route that requires no authentication. Correct as written; not part of
  finding 2.1.
- **The PC-side OTA client** — `tools/PcTools/src/kilnctrl/ota_http_client.py` derives
  the MAC in one place (`derive_mac`, line 134) with one KDF context constant
  (line 82), and all six of its callers use it. The Python client is not part of this
  problem.
