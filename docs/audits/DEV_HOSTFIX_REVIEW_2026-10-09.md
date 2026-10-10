# Review: hostfix 2d1f637d and profiles seqlock tests cdc44d2e (2026-10-09)

Read-only review on origin/dev at `2a379f0d`. Commits:

- `2d1f637d`: absolute captive 302 (`http_captive_location()`), Host allow-list
  first-label rule, `window.kcHostRefusalText` 403 alert in `app.js`, zones page
  blank-optional omission and Save disable, wizard step 11 read-back.
- `cdc44d2e`: `test_profiles_http.c` slot-generation tests for edit, retarget
  commit/revert and delete.

Out of scope: the first-label rule accepting any suffix (`kilnctl.attacker.example`),
a known DNS-rebinding regression with a fix in flight. Nothing beyond it was found
in the Host rule itself.

## What was run

| Command | Result |
|---|---|
| `tools\check_lint_pages.ps1` (runs `firmware/KilnFW/App/test/lint_pages.js` on drivers + recovery) | PASS, 67 script/style blocks, 0 problems |
| `node firmware/KilnFW/App/test/lint_pages.js` with no arguments | crashes (`readdirSync(undefined)`). The script needs the directory arguments the wrapper passes. This is how it is invoked, not a defect |
| `node firmware/KilnFW/App/test/test_host_refusal_pages.js` | 17 passed, 0 failed |
| `build_host_tests.ps1 -Only 'http_auth_enforce\|profiles_http'` | 13/13 executables built and passed |
| `tools\negtest.ps1`: move `profiles_slot_gen_begin(id)` below the RAM assign in `retarget_commit` (`profiles_http.c:2435-2436`), `-Only profiles_http` | **MISSED** (baseline PASS, mutant exit 0). See M2 |

## Findings

### M1 (MEDIUM): captive 302 sends LAN clients to the SoftAP IP

**FIXED in 9a99223a.**

`firmware/KilnFW/App/drivers/http/wifi_provision_http.c:884-889`

The 404 handler runs on every interface and every Host. It always reads the
`WIFI_AP_DEF` netif. That netif is created once (`wifi_prov.c:537`) and never
destroyed, and the code never checks whether the request came in on it. In
STA-only mode the AP netif is down, but `esp_netif_get_ip_info()` still returns
its stored static config (192.168.4.1) with `ESP_OK`, so `http_captive_location()`
succeeds.

- **STA-only:** a LAN browser at `http://192.168.1.50/` asks for any unregistered
  non-`/api/` path: an old bookmark, a mistyped page, `/robots.txt`, or a
  `favicon.ico` request if there is no route for it. It now gets
  `302 Location: http://192.168.4.1/`. That address cannot be reached from the
  LAN, so the tab hangs and shows a connection error. Before this commit the
  relative `/` loaded the dashboard. If the LAN itself is 192.168.4.0/24, the
  redirect lands on some other device (often the router).
- **AP+STA:** clients on the AP get the right target. LAN clients reaching the
  STA address get the same unreachable redirect as above.
- **Fix:** derive the target from the socket that received the request.
  Either use the local address of `httpd_req_to_sockfd(req)` (the
  `get_local_ipv4_string()` logic already in `wifi_prov_link.c`), or emit the
  absolute AP URL only when `wifi_prov_request_arrived_on_ap(sockfd)` is true and
  keep `/` otherwise. Add a host test for the "not on AP" branch.

Header injection: none. The value is built only from four numeric octets, the
24-byte worst case (`http://255.255.255.255/` plus NUL) is guarded by `cap < 24`,
and `loc` stays alive until `httpd_resp_send()` (esp_http_server does not copy
header values, and the send happens in the same frame).

### M2 (MEDIUM, test gap): the seqlock tests cannot see the RAM assign, only the persist

**FIXED in 2974eacd.**

`firmware/KilnFW/App/test/test_profiles_http.c:3982` (`sg_write_fn`) and the
header comment above it. The relevant source is `profiles_http.c:2435-2436`,
`profiles_edit_http.c:715-716`, `profiles_http.c:2366-2367`.

The probe runs inside the cfg write seam, so it only proves the generation is
odd while `nvs_save_slot_locked()` runs. The bracket exists so that a reader
(`profiles_http_slot_runnable_rev()` / the executor's captured rev) never sees new
RAM content under an even generation. The header comment says the tests check
"odd while the RAM assign/persist is in flight", but they cannot observe the
assign. Mutation evidence: moving `profiles_slot_gen_begin(id)` below
`s_profiles.profiles[id] = *trial;` in `retarget_commit` is **MISSED** by the
whole `profiles_http` executable (negtest above). The same reorder in the edit
path (`profiles_edit_http.c`) and in `retarget_revert` has the same blind spot.

- **Failure scenario:** a later refactor puts the assign before `gen_begin`.
  An executor start on another task captures the even, old generation, reads the
  half-rewritten slot, then passes the rev re-check because the generation only
  moves afterward. That is exactly the torn read the seqlock prevents, and these
  tests stay green.
- **Fix:** probe the assign itself. One option is a test hook called right after
  each RAM assign (or the existing `profiles_slot_gen_begin` seam) that records
  whether the generation is already odd at that moment. Another is a fake
  `persist_scratch`/memcpy hook. Then negtest the reorder.

Order dependence (the generation counters are never reset): this does **not**
hide failures. Every assertion is relative to a `before` taken inside the test,
or is a parity check. If an earlier test left a slot odd, the strict "exactly one
of slots 0..3 odd" check and the parity checks would fail loudly (gen_begin would
make it even mid-write), not pass. Two minor weaknesses: the retarget
`advanced` check passes if any one slot moved, not every journalled slot; and the
edit/delete failure paths (persist error) have no bracket test.

### L1 (LOW): login and bootstrap 403s bypass the refusal alert and show raw JSON

**FIXED in 6453f331.**

`firmware/KilnFW/App/drivers/http/app.js:347-357` (`isAuthExemptUrl`) and `:626-649`;
`login_page.html:90-110`, `:129-151`

`/api/auth/login`, `/api/auth/bootstrap_password`, `/forgot` and `/reset` are
POSTs, so the F4 Host check (`http_auth_http.c:343-351`, which runs before auth)
applies to them. The wrapper returns `nativeFetch` for them before the new 403
hook at `app.js:1257`, and both login UIs print `resp.text()`.

- **Failure scenario:** web auth is on and the operator opens the board under a
  name that is not accepted, for example a reverse-proxy name, a different router
  DNS name, or `captive.apple.com` after the netif read failed (L3). Their first
  action is to log in, and the modal shows `{"error":"bad_host"}` with no
  guidance. The new message never appears, because with auth on the read GETs are
  not Host-checked and the login is the first refused request.
- **Fix:** apply `kcHostRefusalText` in both login paths, and in the bootstrap
  and forgot/reset handlers.

### L2 (LOW): the OTA stage upload uses XHR, so it gets no refusal alert

**FIXED in 6453f331.**

`firmware/KilnFW/App/drivers/net/ota_page.html:766-783`

`stageXhrOnce` uses `XMLHttpRequest`, which the `window.fetch` wrapper never
sees. A `bad_host` / `cross_origin` 403 becomes `Error('{"error":"bad_host"}')`
in the stage message. Every other write on every page that loads `app.js`
(all 18 pages, including `wifi_provision_page.html`) goes through `fetch` and is
covered once `app.js` has run. The recovery image's pages (`recovery_http.c:273-278`
also emits `cross_origin`) do not load `app.js` at all. That predates this commit
and is noted only for completeness.

### L3 (LOW): the netif-read-failure fallback is untested and leaves the captive flow broken

**FIXED in 9a99223a.**

`wifi_provision_http.c:883-888`

If `esp_netif_get_handle_from_ifkey()` returns NULL or `esp_netif_get_ip_info()`
fails, the Location stays `/`. Only the helper is host-tested, not this glue. On
that path the phone's captive browser loads the page under `captive.apple.com`
(or similar), and `POST /provision` returns `403 bad_host`. The new alert then
says to open the controller "by its IP address or <name>.local", which a captive
mini-browser often cannot do. That was also the behavior before the fix, so it is
not a regression, but nothing logs the failure. **Fix:** fall back to the
well-known AP default (192.168.4.1, already assumed in `wifi_prov_link.c:1090`)
when the request arrived on the AP, and add an `ESP_LOGW`.

### L4 (LOW): blank-omit resets the 8 guard thresholds and hides typos; the page comment is wrong

**FIXED in 6453f331.**

`firmware/KilnFW/App/drivers/http/zones_page.html:2654-2661`;
`zones_http_post_parse.c:578-665`

The comment says `zones_http_post_parse.c` "omit-PRESERVES each one". That holds
for `k/tau/deadtime/fuzzy_strength/coupling_*/easeoffmult/approachratecap/*band`.
It is false for `wrongdirwindow, wrongdirrate, offsettle, runawayrate,
runawaymargin, driftperiod, debounce, frozenwindow`: when absent, those stay 0
(`z` is zero-initialized) and fall back to the firmware default.
`<input type="number">.value` is `""` both when the field is cleared and when it
holds unparseable text.

- **Failure scenario:** an operator types `90-` (a typo) into Runaway margin, or
  clears a tuned Drift period to retype it, then clicks Save. Before this commit
  that was a 400 and nothing changed. Now the save succeeds with "Saved". The
  guard threshold silently reverts to the firmware default. For `k/tau` the edit
  is silently dropped and the old value kept. In both cases the operator believes
  their value was stored.
- **Fix:** omit only fields the user never touched, or validate blanks on the
  client and refuse with a field-named message. Correct the comment either way.

Save-disable re-enable: correct on every path that settles. Validation `return`s
all happen before `saveBtnEl.disabled = true` (`:2906`). `.then` re-enables first
(`:2911`). Non-ok responses, JSON 409s, network errors and `AuthCancelled` all
reach `.catch`, which re-enables (`:2923`). Nothing between the disable and the
`fetch` can throw. The one gap: there is no timeout, so a stalled TCP connection
leaves Save disabled until the browser gives up (minutes). That is cosmetic,
and a reload clears it.

### L5 (LOW): the wizard marks step 11 "done" even when the read-back disagrees or fails

**FIXED in 6453f331.**

`firmware/KilnFW/App/drivers/http/setup_wizard_page.html:1513-1522`;
`security_http.c:79-89`

The read-back text itself is honest. It claims only "sent" when the GET fails,
and it calls out a mismatch. But the chain always continues to
`postStepState(11, 'done')`, so the wizard checklist records success for a policy
the board reports differently, or one that could not be read.

A false "off" is also possible. `security_config_get_handler()` returns 200
with `web_enabled:false` when the backend `get_config()` fails, and
`web_auth_backend_get_config()` reports `false` when the policy store load fails.
So for `wantEnable=false`, a store-read failure reads back as a confirmed "web
login is now off". For `wantEnable=true` the same failure shows the mismatch text,
which is honest.

When enabling login with no session, the read-back GET (ADMIN tier) can 401 and
go through the wrapper. A background GET is not prompted, so this lands in the
"could not be read back" text, which is correct.

**Fix:** mark the step done only when `cfg.web_enabled === wantEnable`. Have the
GET handler signal a load failure (for example a `"load_ok":false` field or a
500) instead of a default.

## Not findings (checked)

- `http_captive_location()`: octet order matches `esp_ip4_addr_t` (first octet in
  the low byte; test `0x0104a8c0 -> 192.168.4.1`). Multi-digit and 255 octets are
  correct, and the small-buffer path leaves `/`.
- `hostRefusalShown` latches once per page load, and the `alreadyRetried` skip
  cannot hide a host refusal, because the host check runs before auth and a
  retried request was already admitted once.
- `kcAlert` renders through `textContent`, so the literal `<name>` in the message
  is shown, not parsed.
- Zones page initial loads wait for `DOMContentLoaded`, so they go through the
  wrapper after `app.js` (deferred) has installed it.
