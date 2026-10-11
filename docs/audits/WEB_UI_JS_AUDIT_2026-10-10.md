# Web UI JS audit (2026-10-10)

Scope: every page and script the KilnFW firmware serves, as of origin/dev `d93cf774a`. That is
`firmware/KilnFW/App/drivers/http/*.html|*.js`, `firmware/KilnFW/App/drivers/net/*.html` and
`firmware/KilnFW_recovery/main/recovery_page.html`. Read-only review; no page was edited.

Five questions were asked:

1. Injection of board- or user-controlled strings.
2. State changes made without confirmation, or shown as successful after a refusal.
3. Writes built from stale data (GET, merge, POST).
4. Polling after 401, and credentials leaking into URLs or storage.
5. Consistency with the CSRF origin check (`http_origin_check.h`).

Prior audits were read so their fixed items are not filed again: `WEB_UI_XSS_AUDIT_2026-10-09`
F1-F8, `DEV_WEB_REVIEW_2026-10-09`, `DEV_WEBFIX_REVIEW_2026-10-09`, `REVIEW_WEBFIX_2026-10-10`
and `REVIEW_WEB_BATCH_2026-10-10`. Each of those fixes was confirmed present, including
DEV_WEBFIX MEDIUM-1: `stepSaveFailed` / `postStepStateOrThrow` now cover wizard steps 0-9 and 11.

Paths below are relative to `firmware/KilnFW/App/drivers/http/` unless they begin with `net/`
or `KilnFW_recovery/`.

## Summary

- **Injection: no exploitable sink found.** Every dynamic innerHTML value traced is either
  escaped (`kcEscapeHtml` or a page-local five-character `esc()`) or a firmware number or
  constant. What remains is defence-in-depth (I-1).
- **CSRF: consistent.** Every state change is a same-origin, relative-URL `fetch` or XHR POST.
  There is no `<form>` post, no `Referrer-Policy` header or `<meta name=referrer>` (which would
  make a same-origin POST carry `Origin: null` and be refused), no sandboxed iframe, no absolute
  URL, and no state change done through a GET. The recovery page is covered by its own
  `origin_guard` (`recovery_http.c:271-281`).
- **Credentials: clean.** Passwords and reset tokens travel only in POST bodies. localStorage
  holds only the theme, the last profile picked and whether info panels are open. Nothing uses
  sessionStorage or the console for them. The TOTP secret is wiped on cancel or confirm.
- **The main open class is stale whole-object writes (M-1 to M-3).** The firmware's lost-update
  guards cover only a write that lands inside the same request. The page never sends back what
  it read, so the board cannot detect a write made after the client's GET. Second is error
  handling that swallows failures on safety-relevant buttons (L-1 to L-4).

| ID | Sev | Area |
|----|-----|------|
| M-1 | MED (FIXED) | zones Save, safety_config Save and wizard zone steps re-post the whole `/api/zones` page from an old GET; no client generation |
| M-2 | MED (FIXED) | profile save overwrites slot `id` with no rev check; a deleted-and-reused slot gets clobbered |
| M-3 | MED (FIXED) | safety_commissioning re-renders every 5 s, wiping unsaved edits; Save all then reports "Committed." |
| L-1 | LOW (FIXED) | STOP FIRING / Pause / Resume / Ack give no feedback on failure |
| L-2 | LOW (FIXED) | Clear Trip refusal text erased by the next poll; plain-text refusals show nothing |
| L-3 | LOW (FIXED) | zones sweep: Abort is fire-and-forget; the status poll dies after one failed fetch |
| L-4 | LOW (FIXED) | zones autotune Start swallows non-JSON 400s, network errors and 403s |
| L-5 | LOW (FIXED) | wizard step 8 shows "Not running." while a sweep is energizing relays |
| L-6 | LOW (FIXED) | diagnostics crash "Clear" (erases coredump) / "Acknowledge" have no confirm and ignore the response |
| L-7 | LOW (FIXED) | diagnostics watchdog toggle trusts any JSON body, no `r.ok` |
| L-8 | LOW (FIXED) | live profile save/decide can go out with no `gen`, and firmware accepts an absent gen |
| L-9 | LOW (FIXED) | `kcUnit.set` is optimistic; a refused unit POST leaves the wrong unit shown until reload |
| L-10 | LOW (FIXED) | safety_page escapes trip cause/remedy twice (`&#39;` shown) |
| L-11 | LOW (FIXED) | wizard step 11 text says OTA/factory-reset still need the AP password (retired 2026-09-29) |
| L-12 | LOW (FIXED) | wizard step 11 silently drops a password typed without a username |
| L-13 | LOW (FIXED) | recovery page: "Upload and boot" and Pico Abort-while-finishing have no confirm |
| L-14 | LOW (FIXED) | kiln_configs writes and ota GitHub Cancel swallow network/auth failures |
| L-15 | LOW (FIXED) | backup and profile export are plain navigations to auth-gated routes |
| L-16 | LOW (FIXED) | wizard step 7 ceiling check passes when it cannot read `/api/zones` |
| L-17 | LOW (FIXED) | settings_display Save re-posts all four fields from the load |
| I-1..I-6 | INFO | see the end |

## MED

### M-1 Whole-page `/api/zones` writes from a stale GET (three pages)

**FIXED (webfx5).** POST /api/zones takes optional `expected_generation`; a stale value answers 409 `zones_config_stale` and writes nothing. zones_page, safety_config_page, the wizard zone steps and the PcTools zone writers (`build_post_body`) send it.

**Where.** Each of these posts every zone field, taken from the GET made when the page or step
loaded:

- `zones_page.html:3024`. The params are built at about 2854-3000: PID gains, plant model
  k/tau/deadtime from `div.dataset` (2899-2901), coupling cells (2907-2911), timing profiles
  echoed from `current.timing_profiles` (2971-2983), and relay names and types (2991-3000).
- `safety_config_page.html:171-186`, the `PASSTHROUGH` list, re-posted at 363-371 and sent at
  379. It covers name, masks, `pid_kp/ki/kd`, `control_mode`, max/min temperature, timing and
  guard fields. A field the GET omitted is posted as `0` or `''`, not left out.
- `setup_wizard_page.html:767-788`, `submitZonesConfig` through `zoneToPostParams` (697-741).
  Used by wizard steps 2, 4, 5 and 6.

**Why the firmware does not catch it.**

- `zones_http_post.c:341` snapshots `s_config_generation` when the handler starts, and `:748`
  refuses a write that landed in between (409 `zones_config_changed_concurrently`). That covers
  only the time the handler itself runs.
- `GET /api/zones` does print `"generation"` (`zones_http_get.c:275`). No page stores it, and
  the POST has no parameter to receive it.

**Scenario.** An operator opens Zones or Safety config, or leaves the wizard on step 4. Then one
of these happens:

- an autotune Accept from another tab, the LCD or MCP writes new gains and plant model;
- `control_set_zone_limits` / `control_set_zone_type` / `control_set_zone_coupling` changes a
  value;
- a second browser edits timing.

The operator then changes one field and saves. The old gains, limits, zone_type, coupling and
timing are written back without any warning. A reverted `zone_type` has already caused a silent
failure once (HP-02 in CLAUDE.md).

**Fix.**

- Echo `generation` from the GET as an `expected_generation` field and have the firmware return
  409 on a mismatch. The page path for showing a 409 reason already exists on zones Save.
- Additionally or instead, stop posting fields the page does not own. `zones_http_post_parse.c`
  already keeps omitted guard thresholds, xzone, relay names, model and coupling unchanged.
- The same narrower pattern applies to `saveAuxRow` (`zones_page.html:3260`). It posts all six
  fields of a relay row, although `aux_outputs_http_core.c:85` keeps absent fields unchanged.
- Side effect today: the wizard and safety_config pages round `tau`/`deadtime` to the GET's
  `%.1f` print on every save (`setup_wizard_page.html:728-729`). Not sending them fixes that too.

### M-2 Profile save overwrites a slot with no concurrency token

**FIXED (webfx5).** GET /api/profile adds `slot_rev`; POST /api/profile takes optional `expected_rev` and `expected_name` and answers 409 `profile_changed` (nothing written, also for a deleted slot). The page sends them and offers reload.

**Where.** The `profiles_page.html` save handler, about 2240-2318, posts `id=editingId`.
`profiles_edit_http.c:527-551` treats an id in 0..99 as "create or overwrite this slot". It
reads only id, name, zone_mask, seg_count, favorite, hidden and segments. There is no rev or
generation check.

**Scenario.**

- (a) Two tabs, or a tab plus MCP, edit profile N. The last save silently wins.
- (b) Tab A has profile N open. Someone deletes N and creates a new profile, which takes the
  lowest free slot, N again. Tab A saves and replaces an unrelated profile.

The live-edit routes (`?gen=`) and zones (in-request generation) at least have a guard; this
route has none.

**Fix.** Echo a per-slot rev and return 409 on a mismatch. At minimum, also send the expected
name and refuse when it differs.

### M-3 Safety commissioning page wipes unsaved edits every 5 s

**FIXED (webfx5).** The 5 s background reload skips `renderGroups` while the form is dirty, so Save posts what the user typed.

**Where.**

- `safety_commissioning_page.html:2355` runs `setInterval(loadCurrent, 5000)`.
- `loadCurrent` (1676) leads to `renderGroups` (1218), which does `el.innerHTML = ''` and
  rebuilds every field from the GET.
- There is no check for unsaved edits or for which field has focus. Only the guided flow's
  `gPrefill` has a run-once latch.

**Scenario.**

- In the Advanced section the operator types a new `tc_offset_c`, a guard threshold or CT
  `A_fs` / `zero_mv`, then reads the notes for more than 5 s.
- The field silently reverts. The "override" checkbox state and focus are lost too, and
  keystrokes typed across a refresh vanish.
- "Save all" then posts the stored values. With no critical change there is no confirm, and the
  page shows "Committed." The operator believes a safety parameter changed when it did not.

**Fix.** Skip `renderGroups` while any `#fields` input has focus or differs from the last GET.
This is the same idea as the zones page's `zonesFormDirty` / `reloadUnlessDirty`.

## LOW

### L-1 Firing controls give no feedback when they fail

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

- **STOP FIRING**, `app.js:1966-1974`:
  `fetch('/api/profile_exec/stop',{method:'POST'}).then(()=>btn.disabled=false).catch(()=>btn.disabled=false)`.
  - The firmware always answers ok (`dashboard_exec_http.c:832-836`), so a server refusal is not
    the risk.
  - What does happen: a network error, a cancelled sign-in (USER tier) or an unhandled 403. The
    operator confirmed "Stop this firing now?" and sees nothing; only the stop bar staying up
    hints that the firing still runs.
  - A visible "Stop was NOT sent" message is warranted.
- **Ack**, `app.js:1989-2001`: same pattern.
- **Pause/Resume**, `app.js:1952-1958`. The 400s "nothing running to pause" / "nothing paused to
  resume" (`dashboard_exec_http.c:842,852`), network errors and a cancelled sign-in all give no
  feedback. The label only corrects on the next heartbeat.
  - Resume puts heat back on with one click and no confirm, while Stop and Ack confirm. That is
    an owner policy call, not filed as a defect.

### L-2 Clear Trip refusal messages disappear or never appear

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`main_page.html:2039-2049`:

- A refusal row is inserted after `#clearTripBtn` inside `#safetyTripBanner`. `renderSafetyTrip()`
  rewrites that banner's innerHTML on every `poll()` (4482), every 2-5 s, so the message is gone
  almost at once.
- The plain-text 500 "safety link not wired up" (`dashboard_exec_http.c:903`) parses to `{}` and
  shows nothing.
- Network errors and AuthCancelled are swallowed by `.catch(function(){})`.
- Each refused click while the banner stays up appends another row.

### L-3 zones current sweep: Abort and the status poll

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

- **Abort**, `zones_page.html:2591`: `fetch('/api/zones/current_sweep/abort', {method:'POST'});`
  has no `.then` or `.catch`. A failure becomes an unhandled rejection with no feedback, on the
  button that stops relays being energized.
- **Status poll**, `zones_page.html:2572-2576`. `pollSweepStatus` sets `sweepPollTimer = null`
  first, and only `renderSweepStatus` reschedules it (2560). The catch, commented "next poll ...
  will retry", does not.
  - One dropped request (a Wi-Fi blip or an expired session) freezes the status at "running" and
    stops the Start/Abort state updates and the completion `reloadUnlessDirty`, until the page
    is reloaded.
- **Autotune Abort** (4319) also uses `.catch(function(){})`, but its poll keeps running.

### L-4 zones autotune Start swallows failures

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`zones_page.html:4304-4310`: `.then(r=>r.json()).then(j=>{ if(!j.ok) ...'Start failed: '+j.error }).catch(function(){})`.

- JSON refusals are shown.
- These are not, so the click appears to do nothing:
  - the non-JSON `httpd_resp_send_err` 400s (`dashboard_autotune_http.c:296/304/318/322/326/364`),
    which throw a SyntaxError;
  - network errors;
  - origin and role 403s.
- The sweep Start handler (2582-2588) is the right pattern: stay quiet on AuthCancelled, show
  everything else.

### L-5 Wizard step 8 hides a running sweep

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`setup_wizard_page.html:1140` hard-codes `Not running.`. The status is only fetched after
Start or Abort (1205-1214), never when the step renders, and leaving the step clears the poll.
After a reload, or after coming back to the step, a sweep that is cycling relays reads
"Not running.". Abort still works.

**Fix.** Fetch `/api/zones/current_sweep/status` in `renderStep8` and resume polling if a sweep
is running.

### L-6 Diagnostics crash-report buttons

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`diagnostics_page.html:1000` (Acknowledge) and `:1004` (Clear):
`fetch('/api/crash_report/clear',{method:'POST'}).then(pollCrash);`

- Clear erases the coredump irreversibly with one click. The MCP tool for the same action
  requires `confirm=True`, plus `allow_unacknowledged=True` for an unacknowledged record.
- A non-2xx response is never reported; only the re-poll shows anything.
- A network error is an unhandled rejection.

### L-7 Diagnostics watchdog toggle trusts any JSON body

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`diagnostics_page.html:846-851`: `.then(r=>r.json()).then(result=>renderWatchdogCfg(!!result.panic_disabled)).catch(function(){})`.
There is no `r.ok` check. A refused POST whose JSON lacks `panic_disabled` renders the panic
watchdog as enabled while it may still be disabled, which is the unsafe direction.

### L-8 Live profile edit can be sent without `gen`

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

- `live_profile_page.html:313-315`: `genQuery()` returns `''` when `lastGen` is null.
- `lastGen` becomes null in these cases:
  - Reload (469-471);
  - after a fork (423) or a decide (487);
  - a save response with no generation (454);
  - `loadWorkingCopy` (284-311) when `editSeq` changed during the fetch. It shows the stale
    banner but does not restore `lastGen`.
- `profiles_live_http.c` `live_gen_stale()` (about 236-261) accepts an absent gen. Its comment
  says the page always sends it, which is not true.
- **Scenario:** press Reload, type while the fetch is in flight, then Save. The request goes out
  without `gen`, and a working copy changed elsewhere is overwritten.
- **Fix.** Refuse save/decide while `lastGen` is null, and/or have the firmware require `gen`.

### L-9 `kcUnit.set` is optimistic and never corrected

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

- `app.js:1473-1484` updates `cached`, fires `kcunitchange`, then posts `/api/unit_pref`. The
  POST is ADMIN tier (`route_tier_table.h:250`). Only a rejected promise is handled, and only
  with `console.warn`.
- A non-2xx response is not checked at all.
- The comment says the next status poll corrects it. But the only button is on
  `settings_display_page.html` (`buildUnitBtn`, `app.js:1547-1549`), and only `main_page.html`
  calls `updateFromStatus` on a poll. Every other page reads `/api/status` once at load
  (`app.js:1570`).
- **Scenario:** a USER-role operator, or one who cancels sign-in, clicks Units. The page shows
  °F, while the device and the LCD stay on °C until the page is reloaded.
- Display only: inputs and POSTs stay in °C by design.

### L-10 safety_page escapes twice

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`safety_page.html:309-311` and `:388-390` call `row('Cause', window.kcEscapeHtml(st.trip_reason_cause))`,
but `row()` (429-433) already escapes its value. Trip words containing an apostrophe, such as
`safety/safety_trip_words.h:77,252` "the borrowed zone's thermocouple", render as `zone&#39;s`.
That is on the card an operator reads during a trip. Display bug, not a security issue.

**Fix.** Drop the inner `kcEscapeHtml` calls.

### L-11 Wizard step 11 gives false security information

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`setup_wizard_page.html:1435-1437` says: "the OTA/factory-reset/system-reset routes still require
the AP password either way".

The AP-password HMAC was retired 2026-09-29. `route_tier_table.h` lists `/api/ota/esp`,
`/api/ota/esp/boot_guard_reset`, `/api/factory_reset` and `/api/sw_reset` as plain ADMIN, which
is open when login is off. The text understates the risk at the exact moment the operator is
deciding whether to turn login on.

### L-12 Wizard step 11 silently drops a typed password

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`setup_wizard_page.html:1468`: `if (username && password)`.

- A password typed with a blank username is never sent.
- So is a changed username with a blank password when a credential already exists.
- In both cases `set_policy` still runs, the page shows "Saved." and the step is marked done.

**Fix.** Refuse when exactly one of the two fields is filled.

### L-13 Recovery page has unconfirmed destructive actions

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`KilnFW_recovery/main/recovery_page.html`:

- "Upload and boot" (`:21`, `upload()`) overwrites the application with no `confirm()`. Exit,
  Wi-Fi reset, apply-staged and the Pico slot override all confirm.
- Pico Abort (`:34`/`:95`) posts immediately. That includes the finishing/retransmit phase, where
  the button is relabelled "Stop (END sent: outcome will be UNKNOWN)".

### L-14 Swallowed failures in kiln_configs and the OTA GitHub Cancel

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

- `kiln_configs_page.html:259-265`: `kcPost` has no `.catch`. Neither do its callers (save 451,
  overwrite 464, clone 475, rename 489, delete 502). A network error or a cancelled sign-in is
  an unhandled rejection with no message.
- `net/ota_page.html:1076-1078`, GitHub Cancel: `.then(loadGh).catch(function(){})`. A refused
  cancel (409, or a 403 origin refusal) gives no feedback.

### L-15 Exports done as navigations to auth-gated routes

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

- `backup_page.html:113`: `<a href="/api/backup/export" download=...>` is an ADMIN route that
  bypasses the fetch wrapper, so there is no login modal. On a missing or expired session,
  Chrome fails the download, and other browsers may save the 401 text as `kilnctl_backup.json`:
  a file that looks like a backup but is not.
- `profiles_page.html:~1789`, single-profile export, sets `kcSuppressUnload = true` and then
  navigates. With an expired session (USER tier) the tab lands on the plain-text
  authentication-required page, and unsaved editor changes are lost without the leave prompt.
- **Fix.** Fetch the file as a blob and check `r.ok`, as the kiln_configs export already does.
- Related: `bulkExport` (`profiles_page.html:1662-1676`) calls `r.json()` without checking
  `r.ok`, so a 404 "no such profile" is reported as "could not reach the board".

### L-16 Wizard step 7 ceiling check passes when the zones read fails

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

`setup_wizard_page.html`, step 7 save, about line 2940: `fetchJsonOr('/api/zones', null)`.

- If the read fails, the list of zone maxima is empty.
- `validateStep7` (1602-1604) then skips the rule that `abs_max_temp_c` must be at least the
  highest zone `max_temp_c`.
- The firmware ceiling sync (`safety_ceiling_sync.c`) still backs this up, so it is defence in
  depth only.

**Fix.** Refuse when `freshZones` is null.

### L-17 settings_display re-posts all fields

**FIXED (webfx6, e36279885).** Page change with a node source/behaviour test in `test_web_ui_js_audit_fixes.js`.

- `settings_display_page.html:199-207`: Save posts all four fields from the form, so it
  overwrites a change made since load from the LCD or another tab.
- The load at 168-170 parses JSON without checking `r.ok`.
- Low stakes: display power settings only.

## INFO

- **I-1. Unescaped firmware values in innerHTML.** All are numbers or enums today, so none is
  exploitable; this is F8-class hardening.
  - `zones_page.html:3360`/`3462`: the autotune `s.state` and `s.rule` fallbacks, rendered at
    3466/3489/3523.
  - `main_page.html:1023-1027`: PID popup `z.pid_kp/ki/kd` inside `value="..."`.
  - Profile and live segment `value="..."` attributes: `live_profile_page.html:175-183`,
    `rampFieldsHtml` / `ioFieldsHtml`.
- **I-2. Background polls keep hitting ADMIN routes after a 401 or a declined login.**
  - zones: about six loops at 3-5 s. Also live_profile (2 s), readiness, safety, the wizard
    step 8 poll and ota `ghPoll`.
  - `app.js` never re-prompts (`authPromptAllowed`, 1118-1122), so this is quiet 401 traffic,
    not a prompt loop. There is no "signed out" indication, and the sections simply stop
    updating.
  - This is the app-wide design. A shared back-off after `authPromptDeclined` would remove it.
- **I-3. Clicks that energize relays or heat have no confirm.** zones sweep Start (2578) and
  autotune Start (4299) are server-gated (readiness, mode gate, trip/link checks) and have hint
  text. Judgment call. Also unconfirmed: wifi_provision's switch to AP mode
  (`net/wifi_provision_page.html:1119-1130`), and Save Policy turning web auth off
  (`net/security_page.html`).
- **I-4. Other no-feedback paths.**
  - `app.js:2254-2263` "Stay unlocked" hides the prompt on any HTTP status; `pollSession`
    re-shows it within about 5 s.
  - `app.js:2026` "Edit firing" navigates whatever the status.
  - `nav.js:443-460` logout navigates even on non-2xx; the comment says this is deliberate.
- **I-5. Minor robustness and wording.**
  - `readiness_page.html:172`: `decodeURIComponent(location.hash)` throws on a malformed hash
    such as `#%zz`, and the poll's catch then replaces the list with an error every 5 s.
  - `recovery_page.html:90`: `ppoll` calls `JSON.parse` without a try, so a non-JSON 5xx stops
    the Pico status poll.
  - `kiln_configs_page.html:526`: `revokeObjectURL` runs right after `a.click()`.
  - `settings_page.html:328-332`: the sw_reset confirm says S6a latches "every time", but a
    2026-10-01 dual sw_reset did not latch it.
- **I-6. security_page.**
  - `state.ap_ssid` / `ap_password` are never filled in, so the client-side "differs from AP"
    check never fires. The server enforces the rule (`security_backend_web_auth.c:155`).
  - TOTP confirm/disable shows "Incorrect code" for every non-ok reply, because the firmware
    sends a generic `{"ok":false}` (`security_http.c:244`).
  - Wizard step 3 save has no `lockSave`, so a double-click can submit twice.

## Checked and found sound (brief)

- **Escaping.**
  - `kcEscapeHtml` (`app.js:39`) escapes `& < > " '`, and so do the page-local `esc()`/`kgEsc`.
  - kcConfirm and kcAlert render through textContent.
  - Wi-Fi scan SSIDs and saved networks are built with createElement/textContent
    (`renderNetworkList`).
  - These are all escaped or set via textContent: profile, zone, relay and aux names, kiln
    config names, readiness label/detail/fix_url, NVS/partition/cfgfs names, crash rows, GitHub
    tag/repo/sha/error, trip words and remedy, autotune refusal/abort reasons, the tuning
    recommendation, adaptive refusals, and host-refusal text.
  - Imported profile and backup JSON is only sent to the server, never rendered locally.
  - The recovery page uses no innerHTML at all.
- **Confirmations and response checks.** Each of these confirms and checks the response:
  - firing Start (with the watchdog confirm);
  - Stop and Ack;
  - fail-safe ON;
  - autotune Accept (unsettled acknowledgement);
  - safety Clear Trip on safety_page (two steps);
  - factory reset, sw_reset and cfgfs format (`kcOtaAuthedFetch`);
  - OTA stage clear, download, install, Pico rollback and recovery exit (downgrade requires
    typing the version);
  - backup import (dry run, plan confirm, `X-Kiln-Config-Ack-Delete` count);
  - kiln config apply, overwrite and delete (ids never reused);
  - profile delete and bulk delete;
  - live discard and overwrite;
  - clear credentials and TOTP disable;
  - Wi-Fi forget;
  - diagnostics relay reset and danger mode.
  - Shared commissioning `commitAndVerify` treats `ok:false` as a refusal and reads back
    independently.
- **Auth.** Background requests never open the login modal. A declined login on a gated page
  sends the browser to `/`. The bootstrap return path is encoded, and `loginReturnPath` blocks
  open redirects (F6). The F4 host allow-list and the F7 anti-framing headers are present in
  both images.


## webfx6 status

L-1..L-17 fixed in e36279885. INFO: I-1 (main PID popup, live_profile Number()) and I-5 (non-JSON reply handling) fixed. Not done: I-1 zones_page s.state/s.rule (unclear whether output is innerHTML), I-2, I-3, I-4, I-6.
