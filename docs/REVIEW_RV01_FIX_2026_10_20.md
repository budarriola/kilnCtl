# Review: RV-01..RV-04 fix (d3782b2ef), 2026-10-20

Static review of `d3782b2ef` (web_auth_store.c/.h, ui_page_home_refresh.c, host tests). Read-only, no board.
Findings: HIGH 0, MED 2, LOW 3, INFO 4.

## Flow walk-through

| Flow | End state | Verdict |
|---|---|---|
| 1. Fresh board, AP wizard (password, then set_policy), admin login | First set_password finds no policy and no marker, writes both-off (web_auth_store.c:674-685), then the credential. set_policy(web on) passes the transition (admin configured), writes marker then policy. Login mints a session. | OK |
| 2. Security page (user pw, admin pw, policy) | The user call writes both-off. The admin call sees the policy and skips. set_policy enables. | OK |
| 3a. Factory reset | Never touches `kiln_auth` (owner 2026-10-16). Policy, marker and `auth_mig` survive. | OK |
| 3b. clear_credentials (admin session) | Credentials wiped, policy kept, admin must_change. Bootstrap opens, set_password skips (policy present). | OK |
| 3c. LCD reset gesture | Same as 3b when the policy is present. When the policy is lost, see RF2-01 and RF2-02. | see findings |
| 3d. FS-02 wipe | Latched: migration and the both-off pre-write are skipped. resolve writes an enabled policy. `auth_mig` was erased with the partition and is rewritten on the next boot (policy present, no-op). | OK |
| 3e. Backup restore with or without auth | Backups never carry `kiln_auth` (check_kiln_auth_config_isolation). No interaction. `auth_mig` survives. | OK |
| 4. Legacy board (password, no policy, no marker) | Boot migration writes both-off, then `auth_mig`. Behaviour stays auth off, as before. | OK |
| 5a. Policy-only loss (marker kept), any time | UNREADABLE. No session resolves, so the banner's "admin log in" cannot work. Gesture then bootstrap: set_password skips the pre-write (marker), and the policy stays UNREADABLE. **Lockout loop**, RF2-01. | lockout |
| 5b. Policy and marker loss, after migration | UNREADABLE (credential present). Gesture or RV-03 bootstrap: set_password writes **both-off** and auth is fully off. RF2-02. | opening |
| 5c. Policy and marker loss, before migration | Migration cannot tell this from a legacy board, so it writes both-off and the board opens. Inherent, RF2-08. | accepted |

`auth_mig`: written once on the first boot of this firmware on every board (even fresh boards), and skipped while FS-02 is latched. Nothing erases it except a partition wipe, which re-arms it correctly. Its retention is correct in every flow above.

## Findings

### RF2-01 MED: losing the policy with the marker kept is still an unrecoverable lockout, and the banner tells the admin to do something impossible
`web_auth_store.c:674`, `:970`; `http_session_iface.c:276`. When load_policy is UNREADABLE, every session resolves NONE, so "admin log in, save security" (the RV-04 banner) cannot be done. After the gesture, bootstrap sets the admin password, but set_password skips the pre-write because the marker is present. No path writes a policy, so the board loops: the RV-01 "gesture leads back into the same loop" case is still open for the most likely loss (one key). Recovery needs an NVS erase by flashing.
Fix: in bootstrap after the gesture (admin unconfigured with must_change, policy UNREADABLE with the marker present), write an enabled policy (web on, LCD on), as storage_reset_resolve does. Change the banner to "do the reset gesture, then set admin". Add a host test: marker kept, policy dropped, gesture, bootstrap, admin login works.

### RF2-02 MED: set_password reopens a provisioned board that lost its policy and marker
`web_auth_store.c:674-676`. The pre-write checks only "no policy and no marker". It does not check that no credential record exists. After migration, a lost-policy board with a user-only credential (the RV-03 state) lets any network caller bootstrap an admin, and now that same call also writes both-off: the attacker gets the admin account and auth is off for everyone. Before this commit, the bootstrapped admin could not log in (UNREADABLE). After the gesture on the same state, the board also ends with auth off and no banner.
Fix: write the both-off policy only when the web blob is HAL_NOT_FOUND, meaning truly never provisioned. Flows 1 and 2 still pass, because the first call finds no blob. Otherwise leave the policy absent, or write it enabled (see RF2-01). Add a test for the RV-03 state: bootstrap must not end with web_enabled false.

### RF2-03 LOW: the RV-03 acceptance rests on false premises
`web_auth_store.c:1067-1070`; KNOWN_ISSUES RV-03. The text says "set_password now always stores a policy first", but the write is skipped when the marker is present or FS-02 is latched. It says "the LCD shows a banner", but the banner clears once the RF2-02 bootstrap writes both-off. The state is also worse than "fully open before AU-02": it is now open with an attacker-chosen admin.
Fix: after RF2-02, re-state RV-03 (bootstrap must not disable auth), or gate bootstrap on the gesture in this state, as RT-01 does.

### RF2-04 LOW: the policy-lost banner goes stale after a credential wipe
`web_auth_store.c:960`, `:1400-1416`; `auth_reset_gesture_wiring.c:59`. s_policy_lost is refreshed only at boot and after policy, PIN and marker writes. After the gesture on a board with policy and marker lost, load_policy reads ABSENT (no configured credential), so auth is fully off, yet the banner still says "policy lost" until reboot. The fully-off state after the gesture is itself an AU-02 opening (the gesture normally leaves bootstrap-only).
Fix: call lcd_pin_pending_refresh() at the end of both wipe functions and after set_password, and treat a wiped web blob (present, unconfigured) as provisioned in load_policy.

### RF2-05 LOW: test gaps
`test_web_auth_login_http.c:1658-1726`. There is no test for RF2-01 (marker kept, gesture, bootstrap), RF2-02 (RV-03 bootstrap outcome), the pre-write failure path (`:680`), or migration with an unreadable `auth_mig`. KNOWN_ISSUES RV-02 records no negtest run for the new tests.
Fix: add these tests and negtest them with tools/negtest.ps1 (kilnfw-host ExpectPattern).

### RF2-06 INFO: extra NVS writes
One policy write (with read-back) on the first credential of a fresh board. One `auth_mig` write per board lifetime, plus one after each partition wipe. The rest are reads only (policy and marker per set_password, `auth_mig` per boot, load_policy per refresh). Negligible wear.

### RF2-07 INFO: RAM and stack
Internal RAM: +1 B bss (s_policy_lost). The banner string is in flash. **wifi_prov_owner: no change.** Its only web_auth path, http_auth_any_ap_session_active() -> resolve_timeout_s() -> load_policy, has no code change (comments only). The pre-write makes the credential-lookup fallback inside load_policy rarer, so that path costs slightly less in practice. The callers of set_password (httpd worker, bootstrap handler, auth_totp_http) gain the nested set_policy -> set_blob_verified frame (256 B read-back buffer) plus lcd_pin_pending_refresh (load_policy and load_pin): about 350-450 B more peak. The boot task gains the same through legacy_policy_migrate. Fix (optional): measure httpd_worker headroom on the next bench flash.

### RF2-08 INFO: migration opens a board that lost both keys before upgrading
`web_auth_store.c:977-998`. A board that ran AU-02-era firmware and lost the policy and marker before its first boot of this firmware gets migrated to both-off. This cannot be told apart from a legacy board. Accept it and note it in KNOWN_ISSUES. Separately, the header of `web_auth_store_clear_policy_and_marker()` (`web_auth_store.h:316-319`) still says it restores the fresh open state. Since AU-02, with credentials present it gives UNREADABLE instead. It has no production caller.

### RF2-09 INFO: the pre-write runs outside cred_store_lock
`web_auth_store.c:674-685`. The check and the both-off write are not atomic with a concurrent set_policy. A race would need set_policy(enable) to succeed between them, which needs an admin credential that this call has not yet written. Only theoretical with a single httpd worker.
