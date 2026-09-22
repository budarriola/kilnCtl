# Audit: does `factory_reset(scope=wifi)` leave STA credentials in flash?

Triage of the C6 finding recorded in `docs/BENCH_TEST_LOG.md`
("2026-09-21 C26 redo + C6") and `docs/COMMISSIONING_TEST_MATRIX.md` row
"Reset Wi-Fi only". Source review only -- no board was touched. Repo state:
origin/main at c8e042c8.

## Verdict

Split verdict. The bench agent's **observation** is real, but the
**mechanism** it proposed is refuted, and a **different, larger defect** than
the one reported is confirmed.

1. **REFUTED** -- "the ESP-IDF driver auto-reconnected from its own
   flash-persisted config for a few seconds after the reboot" cannot happen on
   the post-erase boot. The app never enables the STA interface on that boot
   and never calls `esp_wifi_connect()`, and the status field the bench read is
   derived from app RAM state that no code path can set to CONNECTED under
   those conditions. Evidence in section 2.
2. **CONFIRMED, and wider than reported** -- ESP-IDF's Wi-Fi driver does keep
   its own persistent copy of the STA (and AP) config, in the `nvs.net80211`
   namespace of the **default `nvs` partition**. **No** `factory_reset` scope
   erases that partition -- not `wifi`, and not `all`. So the STA SSID + PSK
   and the board's AP password survive a "factory default -- erase everything".
   This is a data-retention gap independent of the transient, and it is what
   should be fixed.
3. The transient itself is most likely a read against the **pre-reboot**
   firmware instance (the reset path deliberately delays the reboot) or a
   stale PC-side value. It is not evidence of an auto-reconnect, and no
   source-level fix in the Wi-Fi state machine is warranted for it on this
   evidence. Reproducing it with a monotonic uptime read alongside each status
   read would settle it; see section 5, step 7.

## 1. What the driver persists, and where

- `CONFIG_ESP_WIFI_NVS_ENABLED=y` -- `firmware/KilnFW/sdkconfig:2185`.
- Default storage is flash: "The default value is WIFI_STORAGE_FLASH",
  `components/esp_wifi/include/esp_wifi.h:1088` in the pinned IDF
  (`C:\esp\v6.0.2\esp-idf`). Cited IDF-relative on purpose: a bare
  `esp_wifi.h:NNNN` resolves, in `check_doc_citations.ps1`'s repo-wide
  basename index, to this repo's own 213-line host stub
  (`firmware/KilnFW/App/test/stubs/esp_wifi.h`) and fails the check.
- At the time of this audit, nothing under `firmware/KilnFW/App/` called
  `esp_wifi_set_storage()` (repo-wide grep over `firmware/**/*.c,*.h`: zero
  hits outside the IDF). The bench agent's premise on this point is correct.
  The fix below is what introduced the first callers.
- The namespace is `nvs.net80211`, confirmed by a literal-string scan of
  `components/esp_wifi/lib/esp32s3/libnet80211.a` (the only `nvs.*` literal in
  that archive). It lives in the default `nvs` partition.
- The app writes into it on every join and every AP config apply:
  - `firmware/KilnFW/App/drivers/net/wifi_prov_link.c:125` --
    `esp_wifi_set_config(WIFI_IF_STA, &sta_cfg)` with SSID + password.
  - `firmware/KilnFW/App/drivers/net/wifi_prov_link.c:58` --
    `esp_wifi_set_config(WIFI_IF_AP, &ap_cfg)` with the AP password.

### What the reset scopes actually erase

`firmware/KilnFW/App/drivers/http/factory_reset.c:98-107`:

- `kWifiOnly[] = { "wifi_nvs" }`
- `kKilnOnly[] = { "kiln_nvs" }`
- `kProfilesOnly[] = { "profiles_nvs" }`
- `kAll[] = { "wifi_nvs", "kiln_nvs", "profiles_nvs" }`

The default `nvs` partition appears in none of them. That is **deliberate for
a different reason**: the web admin credential namespace `kiln_auth`
(`firmware/KilnFW/App/drivers/persist/web_auth_store.c:24`, opened with
`partition == NULL`, i.e. the default partition -- see that file's lines
18-20) must survive all four scopes per WEB_AUTH_PLAN item 12b, and four host
tests assert exactly that
(`firmware/KilnFW/App/test/test_ota_http.c:1455,1467,1479,1491`). So the
driver's credential copy rides along in a partition the reset is
contractually forbidden from erasing.

### Is it a defect?

Yes. `docs/COMMISSIONING_BACKEND_RUNBOOK.md:167` describes C6 as "erases live
config/credentials". The operator-facing promise of a Wi-Fi reset -- and more
strongly of "factory default -- erase everything" -- is that the network
credentials are gone from the device. They are not: a later reader of that
flash (a JTAG dump, a reused or RMA'd board, a firmware that does not set
`WIFI_STORAGE_RAM`) recovers the last SSID and PSK. Severity: moderate --
no runtime misbehaviour, a real data-retention gap.

## 2. Why the auto-reconnect mechanism is refuted

On a boot where `wifi_nvs` has been erased, `nvs_load_saved_nets()` yields
`s_wifi.saved_nets.count == 0`, so `wifi_prov_start()` takes its third branch
(`firmware/KilnFW/App/drivers/net/wifi_prov.c:361-368`):
`esp_wifi_set_mode(WIFI_MODE_AP)` at line 363 plus `apply_ap_config()`, and
nothing else. The STA interface is never started that boot, and
`apply_sta_config()` is not called.

Nothing then connects:

- `do_ev_sta_start()` (`wifi_prov_link.c:449`) calls `esp_wifi_connect()`
  only when `saved_nets.count > 0 && mode == HOME`.
- `do_rescan_tick()` (`wifi_prov_link.c:374`) returns immediately when
  `saved_nets.count == 0`.
- `do_ev_sta_disconnected()` (`wifi_prov_link.c:457`) likewise returns when
  `saved_nets.count == 0`.

And nothing can report CONNECTED:

- The UART `WIFI_CMD_GET_STATUS` reply's connected byte is
  `wifi_prov_is_sta_connected()`
  (`firmware/KilnFW/App/drivers/bridge/uart_bridge_ext_wifi.c:39`), a direct
  read of `s_wifi.state == WIFI_PROV_STATE_CONNECTED`
  (`firmware/KilnFW/App/drivers/net/wifi_prov_api.c:562`).
- `s_wifi.state` is set to CONNECTED only in `do_ev_got_ip()`, i.e. only on
  `IP_EVENT_STA_GOT_IP` -- stated at `wifi_prov_link.c:263` and true of every
  assignment in the file.
- The SSID in the same reply is `wifi_prov_get_saved_ssid()`
  (`wifi_prov.c:459`), which returns `""` when both `active_ssid` and the
  saved list are empty -- which they are on that boot.

So a post-erase boot cannot emit "connected, old SSID, old IP" regardless of
what the IDF driver holds in `nvs.net80211`. Independently, the IDF driver
does not auto-connect on `esp_wifi_start()`: no auto-connect facility exists
in `esp_wifi.h` for this version, and association requires an explicit
`esp_wifi_connect()`.

The plausible alternative: the reset path erases on the flash worker and then
creates `reboot_task`, which sleeps 500 ms before rebooting
(`factory_reset.c:119-122`), on top of the erase time itself. A status read
issued in that window answers from the **still-running old instance**, whose
`s_wifi` still holds the live association -- exactly "connected, old SSID, old
IP", followed seconds later by the correct empty state. The bench note says a
fresh boot-push of the firmware version was seen first, which argues against
this; but that push is consumed PC-side (`tools/PcTools/src/kilnctrl/gui.py:711`,
`_on_boot_push`) and the two status reads came from two different PC-side
sessions, so ordering across them is not established by the log. This audit
does not claim to have proven the alternative -- only that the proposed
mechanism is impossible.

## 3. Proposed minimal fix

Two parts. Part A stops new credentials from ever reaching the driver's
store; part B clears what previous firmware already wrote.

### A. Stop persisting (one call, `wifi_prov.c`)

Immediately after the `esp_wifi_init()` success check at
`firmware/KilnFW/App/drivers/net/wifi_prov.c:284-288`, before the event
handlers are registered:

```c
    /* The app is the sole owner of Wi-Fi credential persistence (wifi_nvs,
     * wifi_prov_nvs.c). ESP-IDF's driver defaults to WIFI_STORAGE_FLASH and
     * would keep its OWN copy of the STA/AP config in the default `nvs`
     * partition's nvs.net80211 namespace -- a partition no factory_reset
     * scope may erase (kiln_auth lives there, WEB_AUTH_PLAN 12b). That copy
     * is never read by this module (every boot re-applies config from
     * wifi_nvs) and survives factory_reset(scope=all), so it is pure
     * retained-credential exposure. */
    esp_err_t store_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
    if (store_err != ESP_OK) {
        ESP_LOGW(WIFI_PROV_TAG, "esp_wifi_set_storage(RAM) failed: %s -- driver will keep its own "
                 "credential copy in the default NVS partition", esp_err_to_name(store_err));
    }
```

Side effects, checked:

- **AP config persistence**: none lost. `apply_ap_config()` is called from
  `wifi_prov_start()` on every boot in all three mode branches, from
  `s_wifi.ap_ssid`/`ap_password`, which are loaded from `wifi_nvs`. Same for
  `apply_sta_config()`.
- **Provisioning flow**: unchanged. `wifi_prov_add_network()` persists to
  `wifi_nvs` itself, then joins.
- **Bring-up order**: `esp_wifi_set_storage()` requires only that
  `esp_wifi_init()` has run, which it has at that point, and must precede the
  first `esp_wifi_set_config()` -- the first of those is in the mode branches
  further down the same function (lines 333-368).
- **boot_guard / `nvs_report_capture()`**: untouched. Nothing here changes
  which partitions exist or are initialised; the default partition is still
  initialised at `wifi_prov.c:214`.
- **Recovery firmware**: `firmware/KilnFW_recovery/main/recovery_wifi.c` also
  calls `esp_wifi_set_config()` (lines 88, 127) with default FLASH storage, so
  a recovery boot would repopulate the namespace. Follow-up, not a blocker for
  the app-side fix.
- **Standing guard, 2026-09-21**: `tools/check_wifi_ram_storage_mirror.ps1`
  (+ `.py`) now guards this pair mechanically -- it fails if either
  `wifi_prov.c` or `recovery_wifi.c` loses its
  `esp_wifi_set_storage(WIFI_STORAGE_RAM)` call, gains a second one, or has it
  reordered to after the file's first Wi-Fi config apply, so one side being
  edited away silently while the other still holds is no longer possible
  without a red `run_all_checks.ps1`.

### B. Clear what is already there

`esp_wifi_restore()` (`components/esp_wifi/include/esp_wifi.h:437-449` in the
pinned IDF -- same IDF-relative form as section 1, for the same reason) restores "Wi-Fi stack persistent
settings to default values", explicitly including "esp_wifi_set_config
related". Call it in `execute_scope_job()` (`factory_reset.c:157`) for any
scope whose partition list contains `WIFI_NVS_PARTITION` -- i.e. `wifi` and
`all` -- after the erase loop at `factory_reset.c:182-194` and before the
reboot:

```c
    for (size_t i = 0; scope->partitions[i] != NULL; i++) {
        if (strcmp(scope->partitions[i], WIFI_NVS_PARTITION) != 0) {
            continue;
        }
        /* The app's own credentials went with wifi_nvs above; this clears the
         * SEPARATE copy ESP-IDF's driver keeps in the default `nvs` partition
         * (nvs.net80211), which no scope's partition list may erase wholesale
         * because kiln_auth shares that partition. Best-effort, same as every
         * other step here -- the reboot follows either way. */
        esp_err_t restore_err = esp_wifi_restore();
        if (restore_err != ESP_OK) {
            ESP_LOGW(TAG, "esp_wifi_restore() failed: %s -- the Wi-Fi driver's own persisted "
                     "config may survive this reset", esp_err_to_name(restore_err));
        }
        break;
    }
```

**Ordering caveat, unverified.** If part A lands first, the driver's storage
mode is RAM when `esp_wifi_restore()` runs, and whether IDF still erases the
pre-existing flash blob in that mode is not documented and was not verified
here. Two acceptable resolutions: (i) land B and verify on the bench before
landing A, or (ii) in part B, bracket the call with
`esp_wifi_set_storage(WIFI_STORAGE_FLASH)` then
`esp_wifi_set_storage(WIFI_STORAGE_RAM)`. Whoever implements this must settle
it with a real read-back of the namespace, not by reading the header.

Part B runs on `bx_flash_worker` (`execute_scope()` dispatch,
`factory_reset.c:245`) -- the same stack that took a 2026-09-21 overflow and
was raised to 10240. `esp_wifi_restore()` is an NVS erase of one namespace,
comparable to the `hal_kv_erase_partition()` calls already on that stack, but
the margin must be re-measured after the change (`get_stack_margin`), not
assumed.

### Option explicitly rejected

Adding the default `nvs` partition to `kWifiOnly[]`/`kAll[]` would erase
`kiln_auth` and lock the operator out of the web UI. It contradicts
WEB_AUTH_PLAN 12b and would fail all four
`test_credential_survives_factory_reset_*_scope()` tests
(`test_ota_http.c:1455-1502`). Do not do this.

## 4. Test coverage today

- **No `check_*.ps1` covers `factory_reset` scopes.** A grep over `tools/*.ps1`
  hits only `check_stack_margin_registration.ps1` and
  `check_uri_handler_cap.ps1`, both incidentally (task and route inventories).
- **Host tests cover only the survival direction**: the four
  `test_credential_survives_factory_reset_*_scope()` cases in
  `test_ota_http.c` assert `kiln_auth` is NOT erased. Nothing asserts what a
  scope DOES erase, and nothing covers the IDF driver's own store -- the host
  build stubs `esp_wifi.h` entirely
  (`firmware/KilnFW/App/test/stubs/esp_wifi.h:119` for `esp_wifi_set_config`,
  `:142` for `esp_wifi_connect`), with no `esp_wifi_set_storage` or
  `esp_wifi_restore` stub present.

## 5. Test plan for the fix

1. Add `esp_wifi_set_storage()` and `esp_wifi_restore()` to
   `App/test/stubs/esp_wifi.h` with call-recording counters.
2. New host test in `test_ota_http.c` (which already `#include`s
   `factory_reset.c` directly): assert `esp_wifi_restore()` is called exactly
   once for the `wifi` and `all` scopes and zero times for `kiln` and
   `profiles`. The existing four survival tests must stay green unchanged.
   The existing four already call `factory_reset_execute()` directly and so
   already run `execute_scope_job()` against the fake flash worker and
   `fake_kv` (`test_ota_http.c:1491-1502` for the ALL case), so the new cases
   can follow the same shape. Note the stale comment at `test_ota_http.c:235`
   claiming "the tests in this file never reach a scope that executes" -- it
   describes the HTTP-handler path only, and is contradicted by those four.
3. Negative test, mandatory: invert the `strcmp` in the new loop so the call
   fires for the wrong scopes, rebuild the host tests from a **forced full
   rebuild**, confirm the new test fails, then restore by hand and rebuild
   again before measuring anything. An empty `git diff` is not sufficient
   evidence -- the binary must be rebuilt.
4. Target build: `check_00_kilnfw_target_build.ps1`, since part A touches a
   file only the target build compiles against the real `esp_wifi.h`.
5. Stack: re-measure `bx_flash_worker` margin after a `factory_reset(wifi)` on
   hardware.
6. Bench, owner-authorized only (C6 is destructive): run
   `factory_reset(scope=wifi)`, and on the next boot dump the default `nvs`
   partition's `nvs.net80211` namespace to confirm the STA entry is gone.
   Re-provision afterward from the STA environment variables. Same-day
   follow-up: this no longer needs a JTAG memory read --
   `GET /api/nvs/keys?partition=nvs&namespace=nvs.net80211`
   (`diagnostics_http.c`'s `nvs_keys_get_handler()`, ROUTE_TIER_ADMIN, MCP
   tool `nvs_list_keys`) lists that namespace's key names and types
   directly; call it before and after the reset and diff the lists.
7. While at it, settle the transient: issue paired reads of Wi-Fi status and
   board uptime from the SAME session, at roughly 200 ms cadence, across the
   reset. If every "old SSID" sample carries a pre-reboot uptime, the
   transient is the pre-reboot window (section 2's alternative) and needs no
   further work.

## 6. Recommendation

File the retention gap (section 1) as the real defect and fix it as in
section 3. Do NOT file "the Wi-Fi state machine auto-reconnects after a
reset" -- that mechanism does not exist in this code. The
`docs/COMMISSIONING_TEST_MATRIX.md` C6 cell and the `docs/BENCH_TEST_LOG.md`
C6 entry should be corrected to say so, rather than left implying an
`esp_wifi_disconnect()` is needed in the no-saved-networks path.

## 7. Bench verification of the fix (2026-09-21, post-1319e051)

Fix landed in source at `1319e051` (`esp_wifi_set_storage(WIFI_STORAGE_RAM)`
in `wifi_prov.c` plus `esp_wifi_restore()` in `factory_reset.c`'s
`execute_scope_job()` for the `wifi`/`all` scopes, per section 3). Bench-ran
this run at ESP commit `08f1c451` (board flashed and verified from a clean
worktree; the fix commit itself predates `08f1c451`, so it was present).

Ran section 5 step 6's plan as far as this bench setup allows: the real
web-route `factory_reset(scope=wifi)` (challenge/HMAC via `kcOtaAuthedFetch`'s
algorithm, context `factory-reset`), then `nvs_list_keys` (the new MCP tool
over `GET /api/nvs/keys`, added `8f1f44ff` since this audit was written)
against `nvs`/`nvs.net80211` before and after.

**Result: inconclusive, not a pass or a fail.** Before: 86 keys, matching
section 1's expectation. The reset correctly dropped the board off the LAN
(no saved STA network to rejoin -- `wifi_get_status()` over UART confirmed
`sta_connected=False`, `ssid=''`), which is exactly what makes the intended
diff unreachable with this bench's setup: `GET /api/nvs/keys` is HTTP-only,
and the bench PC has no route to the board's own provisioning AP
(`192.168.4.1`) to poll it in the window between the erase and the next STA
join. Re-provisioning over the UART link hub (the only reachable path) and
re-checking after rejoining showed the same 86 keys again -- but this is
uninformative either way, because `esp_wifi_set_config()` (called by
`wifi_prov_link.c` on every join, section 1) repopulates `nvs.net80211` as an
ordinary side effect of establishing *that new* connection, regardless of
whether the namespace was empty or untouched immediately beforehand. A read
taken only after a rejoin cannot separate "the wipe never happened" from "the
wipe worked and the very next join refilled it."

Confirmed independently, not affected by the above: `kiln_auth` is still
refused by this route (client attempt was rejected before the request was
even sent, matching the documented 403), and the board's own web
authentication was still enforced post-reset (one authenticated
`GET /api/status` succeeded via a real login, HTTP 200) -- so the reset did
not collaterally disturb `kiln_auth`, consistent with section 3's design.

**What would actually settle this**, neither done this run: (a) join the
bench PC to the board's `kilnCtl` AP SSID and poll `GET /api/nvs/keys` at
`192.168.4.1` in the window right after the erase and before any
re-provisioning, or (b) a raw NVS-partition read (JTAG or `esptool
read_flash` + an NVS parser) taken in that same window, independent of the
app's own HTTP stack entirely. Until one of those runs, `1319e051`'s effect
on hardware remains unverified by this audit -- the source-level reasoning in
section 3 still stands on its own.

**Update 2026-09-22 (0edfb313).** A third path landed: `factory_reset.c` now logs
`factory_reset: nvs/net80211 key count before|after esp_wifi_restore(): N` on
the UART log stream (count only, never a key name or value), so a bench run can
catch the pair over serial while Wi-Fi is down. Research ruled out the other
two paths on this PC: there is exactly one Wi-Fi adapter (cannot join the AP and
stay on the LAN), and OpenOCD `read_memory` cannot reach flash offsets, so a
raw NVS read is a dead end without new probe capability. Caveat: the ESP-IDF
Wi-Fi driver ships as a prebuilt library, so whether `esp_wifi_restore()`
completes its NVS erase before returning could not be determined from source.
An after-count of 0 is a positive verdict; a nonzero after-count is INCONCLUSIVE
(erase may still be in flight), not proof the erase failed. Recipe: start
`get_device_log` polling over serial before `POST /api/factory_reset` scope=wifi;
the RAM log ring does not survive the reboot that follows.
