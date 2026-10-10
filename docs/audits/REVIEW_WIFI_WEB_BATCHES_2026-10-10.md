# Review: Wi-Fi batch, recovery Wi-Fi rf4, web batch 2 (2026-10-10)

Review only; nothing was fixed. Reviewed on origin/dev at `1d3f7dac8`.

| Batch | Commits | Scope |
|---|---|---|
| A. Wi-Fi | `52c19a8d0`, `82bea6511`, `864ef649c` | `docs/audits/WIFI_REVIEW_2026-10-09.md` MED-1 (DNS hijack admission), LOW-1..6, INFO |
| B. rf4 | `110650ae0`, `aecc6a4a7` | recovery Wi-Fi N1-N4: `hal_kv_key_exists` legacy probe, fake_kv wrong-type NOT_FOUND |
| C. web batch 2 | `af305abbf`, `9538a47b7` | W1 reset-password precheck (app.js + PcTools), W2, W4 `handleSaveAbort`, W5 `reloadUnlessDirty` |

## Verdict

No HIGH or MED-severity regressions. No auth or CSRF regression. One
migration interaction (F1) is worth a small fix. The rest are LOW/INFO
usability and test-fidelity notes.

## Questions asked

**Q1. Can `s_saved_nets_refused` strand a board whose record is merely
old-format?** No. `saved_nets_blob_t` has kept the same layout through its whole
history: `SAVED_NETS_VERSION` 1, `WIFI_PROV_MAX_SAVED_NETWORKS` 8,
`{ssid[33], password[65]}` x 8. So the only "old format" is the legacy
single-key credential set, and that goes through
`nvs_load_legacy_single()`/migration. The list loader never refuses it.
`nvs_load_saved_nets_from()` sets refused only on:
- a newer version (a rollback)
- a wrong size (no historical size exists that would trigger this)
- `count > 8` (corruption)
- an open or read error other than NOT_FOUND

While refused, add and forget return `ESP_ERR_NOT_SUPPORTED`, and
`wifi_prov_is_unprovisioned()` is false, so the open WIFI_SETUP tier closes and
a login is required. That fails closed. `factory_reset(scope=wifi)` gets out of
it, but it also wipes the AP identity and the static IP (see F3).

**Q2. Does strict static-IP validation refuse configs users already saved?
Would an upgrade lose a working static IP?** No upgrade loss. The boot-time
apply in `wifi_prov_link.c` still uses the lenient `parse_ipv4`, so a stored
config keeps working. Backup export does not carry the IP config, so restore is
not affected. The strict rules apply only when someone edits the config (F4).

**Q3. Does legacy migration interact with rf4's probe?** Yes, see F1.

**Q4. Does the client precheck drift from the firmware rule (byte-exact
comparison)?** The rules shared with `web_auth_password_check()`
(`web_auth_store.c`) match:
- length counted in bytes (10..64): `TextEncoder` in JS, UTF-8 `len` in Python, `strlen` in C
- all-lowercase means every character is in a-z: `/^[a-z]+$/`, `[a-z]` per byte
- case-insensitive "password"/"kiln"
- equal to the username

Two differences remain:
- **Missing rule (F2):** the firmware also refuses a password equal to the AP
  SSID or the AP password. Neither client checks that.
- **Unicode (harmless):** JS `toLowerCase()` and Python `.lower()` map U+212A
  KELVIN SIGN to `k`, so the clients refuse "Kiln..."-style passwords that
  the firmware's ASCII check would accept. That makes the clients stricter than
  the firmware, never looser.

**Q5. Any auth or CSRF regressions?** None found:
- `/api/auth/reset` stays OPEN tier, behind the origin check.
- The strength check still runs before the token is consumed, and still records
  a backoff failure. W2's new fuzz check proves an immediate retry after a weak
  refusal returns 429.
- `zones_page.html` and `/provision` gained no new routes and no tier changes.

## Findings

### F1 (LOW-MED): a refused saved_nets record plus a surviving legacy namespace re-adopts stale mode and AP identity on every boot

`wifi_prov_migrate_from_default_partition()` (`wifi_prov_nvs.c:594`) decides
`adopt = !nvs_saved_nets_record_present(WIFI_NVS_PARTITION)`.
`nvs_saved_nets_record_present()` (`:266`) returns false for an absent record and
for an unreadable one alike: open failure, read error, wrong size, newer
version, `count > 8`. When the record is refused and a default-partition
`wifi_cfg` namespace still exists, these steps run on every boot:

- The migration adopts the legacy copy. It writes the legacy mode, AP SSID and
  AP password over the `wifi_nvs` values (`nvs_save_mode`/`nvs_save_ap_ssid`/
  `nvs_save_ap_password`). Those are values the user may have changed since the
  migration.
- `nvs_load_saved_nets()` then sees `s_saved_nets_refused` (`:676`) and sets
  `s_legacy_erase_pending = false`. The legacy copy is never erased.
- So it happens again on the next boot.

rf4 widens the window. The probe is now type-agnostic
(`hal_kv_key_exists`, `:144`), so a legacy namespace that holds only u8 keys
now counts as found when it previously did not.

Reaching this needs both conditions at once:
- a legacy namespace that survived, through an earlier failed erase or a
  rollback past the partition split that rewrote `wifi_cfg`
- a refused record, through a rollback past a future `SAVED_NETS_VERSION` bump,
  corruption, or a read error

That combination is rare, which is why this is not rated MED.

Fix direction: give `record_present` a tri-state result (absent / verified /
unreadable). Do not adopt while the record is unreadable or newer.

### F2 (LOW): the client precheck omits the AP SSID / AP password equality rule, and a refusal costs a fresh TOTP code

The firmware refuses a new password equal to the AP SSID or the AP password
(when non-empty). Neither `kcResetPasswordProblem()` (app.js) nor
`reset_password_problem()` (`totp_http_client.py`) can check this, because
neither has those values. The firmware refusal itself does not spend the token,
but app.js's `onStep2Submit` sets `forgotResetToken = null` before the POST and
returns to step 1 on any failure. So the user has to wait for a new TOTP code.
Fix direction: on a 400 weak-password reply, keep the token and stay on step 2.

### F3 (LOW): refused-record errors do not say how to recover

- A refused add returns 400 "could not save credentials". A refused forget
  returns 500 "could not forget network" (`wifi_provision_http.c`).
- The LCD shows "Could not save credentials" (`ui_page_network_manage.c:261`).
- `/api/wifi/status` gives no hint that the saved-network record is unreadable
  or newer, or that `factory_reset(scope=wifi)` is the way out.

The only symptom a user sees is that saves keep failing.

Related: a transient read error, either at boot or during the migration
read-back (which re-runs `nvs_load_saved_nets_from()`), latches refused until
the next reboot, while RAM still holds the adopted network. A reboot recovers.

### F4 (LOW): strict static-IP edits refuse some formerly accepted configs, and the error text is misleading

`wifi_prov_static_ip_config_valid()` now refuses all of these:
- an off-subnet gateway
- /31 and /32 masks (`inv < 3u`)
- leading-zero octets
- network or broadcast host addresses

A board that already runs with such a config keeps it, because boot uses the
lenient parser. But re-saving it unchanged from the network page now fails.
The 400 reply only says the fields "must each be a valid dotted-quad". It does
not mention the subnet or gateway rule.

### F5 (LOW): W5 dirty tracking does not cover `#relayNames`

`zonesFormDirty` is set only by input/change listeners on `#zones`,
`thermoCount`, `relayCount`, `maxSimultaneous` and `continueOnZoneTrip`.
`#relayNames` (`zones_page.html:371`) lives outside `#zones`. Yet `saveBtn`
posts `relay<N>_name`/`relay<N>_type` from it (`:2982`). So an unsaved relay
name or type edit is still silently discarded when a sweep finishes or an
autotune Accept calls `reloadUnlessDirty()`. That is exactly the W5 symptom,
for those fields.

### F6 (INFO): the WPA2 minimum drops WPA1-only networks

`threshold.authmode = WIFI_AUTH_WPA2_PSK` (with PMF capable, not required)
means a board already joined to a WPA/TKIP-only access point stops associating
after the upgrade and falls back to AP mode. This is intended hardening; it
should go in the release notes.

### F7 (INFO): DNS admission trusts 192.168.4.0/24 as SoftAP clients

`wifi_prov_dns_query_acceptable()` admits any source in 192.168.4.0/24 except
.0, .1 and .255. If the STA-side LAN is also 192.168.4.0/24, LAN hosts pass
admission while the hijack runs. The hijack runs only in AP or AP+STA fallback,
so the exposure is small.

### F8 (INFO): fake_kv still diverges from target for get_blob on a string key

rf4 made fake_kv typed getters return NOT_FOUND on a type mismatch, which
matches ESP-IDF: `Storage::findItem` skips TYPE_MISMATCH items. But fake_kv's
`get_blob` still accepts a key written as a string. On target that is
NOT_FOUND. This predates rf4. `fake_kv.h` names `kiln_cfg_store.c` and
`touch_cal_store.c` as depending on that path. A host test there can pass for
behavior the target does not have.

## rf4 correctness

The changes are correct:
- `hal_kv_key_exists()` maps to `nvs_find_key()` on target.
- The legacy probe now detects any typed key.
- The fake_kv wrong-type NOT_FOUND matches target.

The only interaction is F1.

## Negative tests (`tools\negtest.ps1`)

Each mutation was run in a throwaway copy. Results:

**`build_host_tests.ps1 -Only test_wifi_prov`** (expect `RUN FAILURES`; baseline passed):

| Mutation | Result |
|---|---|
| `wifi_prov_link.c`: `inv < 3u` changed to `inv < 1u` (allow /31, /32) | MISSED, an equivalent mutation (see below) |
| `wifi_prov_link.c`: DNS source-port check `== 53` changed to `== 5353` | CAUGHT |
| `wifi_prov.c`: `is_unprovisioned` drops `&& !s_saved_nets_refused` | CAUGHT |
| `wifi_prov_api.c`: add-network ignores refused (`if (false)`) | CAUGHT |
| `wifi_prov_nvs.c`: migration ignores refused (`if (false)`) | CAUGHT |

**pytest `tests/test_mcp_server_totp.py`:**

| Mutation | Result |
|---|---|
| `totp_http_client.py`: `if n < 10:` changed to `if n < 9:` | CAUGHT (`test_rule_boundaries`) |

Why the MISSED mutation is equivalent: under a /31 or /32 mask every address is
the network address or the broadcast address. `ipv4_usable_host()` therefore
refuses every ip and gateway, whatever the `inv < 3u` clause says. That clause
is defence in depth with no reachable effect, so the MISSED result is not a
test gap. The app.js precheck was not mutation-tested in this review.
