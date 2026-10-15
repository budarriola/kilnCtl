# AUDIT_WIFI: KilnFW Wi-Fi provisioning and connectivity

Date 2026-10-14. Base origin/dev `d5bbf13fc`. Code-only review; no board access.

HIGH 0, MED 2, LOW 3, INFO 3

Scope: `firmware/KilnFW/App/drivers/net/wifi_prov.c`, `wifi_prov_link.c`,
`wifi_prov_api.c`, `wifi_prov_nvs.c` (skimmed), `drivers/http/factory_reset.c`
(wifi scope), `firmware/KilnFW_recovery/main/recovery_wifi.c` and
`recovery_main.c`. Items already fixed by `WIFI_REVIEW_2026-10-09.md`
(`52c19a8d0`), `REVIEW_WIFI_WEB_BATCHES_2026-10-10.md` (`2cd02753c`) and
`RECOVERY_IMAGE_REVIEW_2026-10-09.md` (`959aee923`) are not repeated.

Key question: can the board end up with no STA and no AP, without a reboot?
Yes, on two paths: MED-1 (application) and MED-2 (recovery image).

## Findings

### MED-1: a single dropped STA_DISCONNECTED while CONNECTED strands the board with no STA and no AP

Evidence:
- `wifi_prov_link.c:894-904` `on_wifi_event()` posts `CMD_EV_STA_DISCONNECTED`
  through `post_event()` (`wifi_prov.c:427-442`), a 0-tick `xQueueSend` into a
  6-deep queue (`wifi_prov.c:119`). A full queue drops the event; nothing
  retries it.
- The header comment's self-heal claim (`wifi_prov.c:50-52`, "the driver keeps
  emitting disconnect events while the link is down, and the rescan timer
  independently re-picks") does not hold once the link was CONNECTED. The
  ESP-IDF driver emits one `WIFI_EVENT_STA_DISCONNECTED` and does not
  reconnect unless `esp_wifi_connect()` is called, and the only callers are on
  the owner task (`wifi_prov_link.c:762, 806, 825, 880`, `wifi_prov_api.c:590`).
- `do_rescan_tick()` returns at once while `s_wifi.state == CONNECTED`
  (`wifi_prov_link.c:746-749`). `reconcile_sta_state()` repairs only the other
  direction (state not CONNECTED, radio actually up).
- At GOT_IP the fallback timer is cancelled and the AP is dropped
  (`do_ev_got_ip`, `wifi_prov_link.c:907-969`, then
  `try_drop_fallback_ap_after_join`, `:503-536`), so `mode` is STA-only and
  `ap_fallback_active` is false.
- `IP_EVENT_STA_LOST_IP` is not consumed (`wifi_prov_link.c:1143-1152`), so it
  gives no second chance.

Failure scenario: the board is CONNECTED with the AP torn down. The owner task
is blocked in a radio scan (LCD Network page or `/api/wifi/scan`,
`do_scan()` up to `WIFI_OWNER_SCAN_WAIT_MS`), and HTTP, LCD and UART producers
plus rescan ticks fill the 6 queue slots. The router reboots or the board
loses the beacon at that moment, and the one DISCONNECTED event is dropped.
After that `state` stays CONNECTED forever, every rescan tick returns early,
the fallback timer is never armed, nothing calls `esp_wifi_connect()`, and the
AP stays down. The board cannot be reached until someone reboots it at the
bench, so an unattended GitHub update check or install never runs. The LCD
keeps showing the stale CONNECTED state and the cached IP
(`wifi_status_ui.c:22-45`).

This needs a full queue at the moment of the drop, so it is uncommon. When it
happens it is permanent, which is why it is MED.

Suggested fix: in `do_rescan_tick()`, when `mode == HOME`, `state ==
CONNECTED` and no teardown is pending, check `esp_wifi_sta_get_ap_info()`. If
it reports not associated (`ESP_ERR_WIFI_NOT_CONNECT`), run the same body as
`do_ev_sta_disconnected()`: set RECONNECTING, call `esp_wifi_connect()` and
arm `ap_fallback_timer`. This makes the 30 s tick a real ground-truth check in
both directions. Also correct the `wifi_prov.c:50-52` comment.

Host test: none. `test_wifi_prov.c` drives `do_rescan_tick()` (for example
`test_disconnect_while_fallback_ap_up_waits_for_rescan`, `:1436`) but never
with `state == CONNECTED` and the stub reporting not associated.

### MED-2: the recovery image never retries a failed first SoftAP bring-up, so a manually entered recovery session can be unreachable until a power cycle

Evidence:
- `recovery_wifi.c:296-321` `start_softap()` returns false when
  `esp_wifi_set_mode`/`set_config`/`esp_wifi_start` fails, or when no
  `WIFI_EVENT_AP_START` arrives within 5 s. Every path wipes the passphrase.
- `recovery_wifi.c:410-416` `recovery_wifi_start()` only logs "NO NETWORK
  (HTTP still started)" and sets the LCD to no-network. There is no retry and
  no `restart_image_for_wifi()`.
- That is unlike both neighbouring failure paths. A later `AP_STOP` re-raises
  the AP and, if `esp_wifi_start()` fails, feeds the capped image restart
  (`recovery_wifi.c:193-218`). An HTTP start failure retries and then restarts
  the image (`recovery_main.c:94-120`).
- Late-AP_START sub-case: if `AP_START` arrives after the 5 s wait, the handler
  (`recovery_wifi.c:171-178`) sets `s_up = true` and the LCD to AP_UP. But the
  passphrase was already wiped and never drawn, so the AP is up and nobody can
  join it.

Failure scenario: an operator, or `recovery_enter`, or the boot_guard
threshold, puts the board into the recovery image to install or repair. A
transient driver error, or a slow first `AP_START`, leaves the image running
with no network until a power cycle. The unattended install
(`recovery_install_boot()`) still runs because it needs no network, so the
GitHub update path is not stranded. The manual paths are stranded: recovery
upload, apply_staged, exit and wifi_reset.

Suggested fix: on a `start_softap()` failure, route through the same capped
policy as the AP_STOP re-raise failure (`rhealth_ap_reraise_failed_action()`,
then `restart_image_for_wifi()`). For the late-AP_START case, keep the
passphrase until either AP_START arrives or the image restarts. Or, when the
wait times out, stop Wi-Fi so the late event cannot leave an AP nobody can
join.

Host test: none for this path. `test_recovery_health.c:209-234` checks source
shape around `start_softap`, not this decision.

### LOW-1: a rescan timer create or start failure is log-only

`wifi_prov.c:598-607`. If the 30 s timer is missing, `do_ev_sta_disconnected()`
with `ap_fallback_active` set (`wifi_prov_link.c:858-866`) leaves all retries
to the rescan tick that no longer exists. STA never rejoins, but the AP stays
up, so the board stays reachable. The deferred-teardown retry and the MED-1
fix also depend on this timer. Fix: treat a failure as fatal to
`wifi_prov_start()`, or retry creating the timer from the owner task. No host
test.

### LOW-2: `IP_EVENT_STA_LOST_IP` is ignored

`wifi_prov_link.c:1143-1152`. When a DHCP lease expires while still
associated, the board has no IP while `state` says CONNECTED and the AP is
down. This heals on its own when DHCP answers again (a new GOT_IP), so it is
not permanent. Fix: post a command for it and run the reconnect or fallback
path. That also covers part of MED-1. No host test.

### LOW-3: if `set_mode(APSTA)` fails in `do_ap_fallback_tick()`, the state says the fallback is in progress when it is not

`wifi_prov_link.c:661-697`. `state` becomes RECONNECTING but
`ap_fallback_active` stays false, so no AP comes up on this tick. Later
connect-retry DISCONNECTED events re-arm the fallback timer, so it heals. But
if one of those events is also dropped (MED-1's mechanism), recovery waits for
the rescan tick, which joins STA and never raises the AP. Fix: when
`set_mode` fails, re-arm `ap_fallback_timer` so the AP attempt repeats. No
host test.

## INFO

- INFO-1: `factory_reset.c:355-377` calls `esp_wifi_set_storage()` and
  `esp_wifi_restore()` from the flash worker, outside the wifi_prov owner task
  that owns every other driver call. The reboot that follows (with its inline
  fallback at `:455-461`, `:741-746`) bounds the window to about 500 ms, so
  there is no lasting effect.
- INFO-2: credential storage. The driver runs with `WIFI_STORAGE_RAM` in both
  images, and the app's saved-network records live in `wifi_nvs`.
  `wifi_prov_nvs.c` was skimmed only; no new issue beyond the fixed items in
  the 2026-10-09 review.
- INFO-3: owner decisions checked and implemented:
  - With auth off, the AP stays up while stations are connected
    (`ap_teardown_should_defer`, `wifi_prov_link.c:482-489`).
  - With auth on, a live AP session defers teardown, with
    WEB_AUTH_TIMEOUT_NEVER_S aged by station presence
    (`http_session_iface.c:294-317`).
  - The AP is torn down at GOT_IP unless a static IP is not yet confirmed.
  - SNTP is armed from `do_ev_got_ip`. No mDNS is registered in this area.

## Test gaps

1. `do_rescan_tick()` with `state == CONNECTED` and the driver reporting not
   associated: should reconnect and arm the fallback (MED-1).
2. Dropped `CMD_EV_STA_DISCONNECTED` from CONNECTED followed by N rescan
   ticks: the board must eventually call `esp_wifi_connect()` or raise the AP
   (MED-1).
3. Recovery `start_softap()` failure: the capped restart decision is taken
   (MED-2). The late AP_START case leaves no up-but-unjoinable AP (MED-2).
4. Rescan timer start failure in `wifi_prov_start()` (LOW-1).
5. `do_ap_fallback_tick()` when `set_mode(APSTA)` fails (LOW-3).

## Resolution (2026-10-14, wififx)

- MED-1 fixed: `do_rescan_tick()` (`wifi_prov_link.c`) now asks
  `esp_wifi_sta_get_ap_info()` while HOME + CONNECTED; two consecutive
  unassociated ticks (about 30 s debounce, one static bool, no new task or
  buffer) run `do_ev_sta_disconnected()`. The `wifi_prov.c` header comment is
  corrected. Test: `test_rescan_tick_recovers_from_missed_disconnect_while_connected`
  (`test_wifi_prov.c`); 4 negtest mutants caught.
- MED-2 fixed: a failed first `start_softap()` in `recovery_wifi_start()` now
  goes through `rhealth_ap_reraise_failed_action()` and
  `restart_image_for_wifi()`; with the cap used up it stops the radio (so a
  late AP_START cannot leave an unjoinable AP), shows no-network and clears the
  count. Test: source-shape assertions in `test_recovery_health.c`; 3 negtest
  mutants caught.
- LOW-1..3 remain open.
