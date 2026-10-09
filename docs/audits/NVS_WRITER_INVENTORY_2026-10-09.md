# NVS writer inventory after the cfg dual-write close (2026-10-09)

Scope: firmware/KilnFW (App) and firmware/KilnFW_recovery. Read-only audit of the tree at
origin/dev `676c503f`. Owner decision 2026-10-05/07: cfg saves are LittleFS-only; NVS stays a
read fallback (migrated at first boot) for user-data stores.

## Re-run commands

Raw ESP-IDF calls (only the recovery image uses them; the app goes through the HAL):

    grep -rnE '\b(nvs_set_[a-z0-9_]+|nvs_commit|nvs_erase_key|nvs_erase_all)\s*\(' \
      firmware/KilnFW firmware/KilnFW_recovery firmware/CommonFW --include=*.c --include=*.h \
      --exclude-dir=build --exclude-dir=managed_components --exclude-dir=elf_archive | grep -v '/test/'

HAL wrappers (hwAbstraction/esp/kv/hal_kv_esp.c maps these 1:1 to nvs_set_*/nvs_erase_key/nvs_commit):

    grep -rnE '\bhal_kv_(set_blob|set_str|set_u32|set_u8|erase_key|erase_partition|commit)\s*\(' \
      firmware/KilnFW/App firmware/KilnFW_recovery --include=*.c | grep -v '/test/'

Completeness cross-check (every module that opens a handle READ_WRITE must appear below):

    grep -rn 'HAL_KV_MODE_READ_WRITE' firmware/KilnFW/App --include=*.c | grep -v /test/

Wrappers followed one level: `nvs_save_slot`/`nvs_erase_slot` (profiles_http.c),
`nvs_save_*` (wifi_prov_nvs.c), `nvs_save_store` (safety_cfg_store.c), `erase_key`
(totp_config.c), `erase_then_persist_count` (boot_guard.c), `used_bitmap_save` (profiles_http.c).

Classes: (a) intentional NVS-only; (b) leftover dual-write of data now in cfgfs;
(c) migration marker / erase-after-migrate / erase-first so the fallback cannot resurrect.

## Result

Class (b): **0 sites.** No remaining writer duplicates cfg-owned data into NVS. Stores confirmed
to have no NVS write at all: zones_config_store, kiln_cfg_store (save), unit_pref, time_sync,
ramp_assist_cfg, display_power_cfg, ct_verify_store, iter_tune_store, profiles_favorites,
setup_wizard_progress, profiles_builtin, update_settings, relay_cycles (steady-state persist;
the old NVS-only persist_locked() is gone).
Approximate counts: (a) about 37 write sites, (c) about 14, (b) 0 (a few lines are shared).

## Call-site table

Paths are under firmware/KilnFW/App/drivers/ unless noted.

| file:line | op | key / data | class | note |
|---|---|---|---|---|
| firmware/KilnFW_recovery/main/recovery_http.c:573,578 | nvs_erase_key + commit | boot_guard counter reset | a | recovery escape hatch |
| firmware/KilnFW_recovery/main/recovery_http.c:704,713 | nvs_erase_key + commit | WIFI_RESET_KEYS | a | Wi-Fi forget; Wi-Fi is NVS-only |
| net/wifi_prov_nvs.c:413,415 | set_blob+commit | saved_nets | a | |
| net/wifi_prov_nvs.c:428,430 | set_u8+commit | mode | a | |
| net/wifi_prov_nvs.c:443,445,448 | set_str/u8+commit | AP ssid + flag | a | |
| net/wifi_prov_nvs.c:461,463,466 | set_str/u8+commit | AP password + flag | a | |
| net/wifi_prov_nvs.c:483-500 | set_u8/str+commit | ip_mode, static ip/mask/gw/dns/dns2 | a | |
| persist/web_auth_store.c:402,404 | set_blob+commit | web auth store | a | |
| persist/totp_config.c:90,92 | set_blob+commit | TOTP secret / counter | a | |
| persist/totp_config.c:116,121 (callers 193,197) | erase_key+commit | TOTP secret, last ctr | a | |
| persist/boot_guard.c:227,253 | set_blob+commit | boot counter record | a | |
| persist/boot_guard.c:242,244 | erase_key+commit | legacy `count` key | c | erase-after-migrate |
| persist/boot_guard.c:455 | erase_key | rec (erase-then-write) | a | verified clear |
| persist/ota_record.c:265,267 | set_blob+commit | OTA record | a | |
| persist/pico_image_manifest.c:79,81 | set_blob+commit | manifest | a | |
| persist/pico_image_manifest.c:131,183,186 | erase_key(+commit) | manifest | a | |
| persist/pico_update_attempts.c:128,130 | set_blob+commit | attempt counter | a | |
| persist/pico_update_attempts.c:208,341,344 | erase_key(+commit) | attempts | a | |
| persist/kiln_cfg_swap.c:146,148 | set_blob+commit | swap_pending | a | owner-listed NVS-stay |
| persist/aux_outputs_cfg.c:336,338 | set_blob+commit | aux convert journal | a | owner-listed NVS-stay |
| persist/aux_outputs_cfg.c:357,359 | erase_key+commit | journal clear | a | |
| persist/dualwrite_window.c:119,121 | set_blob+commit | dwwin record | a | cfg-health breadcrumb, deliberately not cfg_fs |
| persist/touch_cal_store.c:125,127 | set_blob+commit | touch affine | a (borderline 3) | not on the owner close list |
| persist/kiln_cfg_store.c:1805-1807 | erase_key x2+commit | legacy kilncfgs blob + rev | c | quarantine discard only |
| persist/live_profile.c:745-747 | erase_key x2+commit | legacy live record/profile | c | erase-first |
| persist/firing_stats_cfg_fs.c:191,197 | erase_key+commit | legacy fsr_<id> | c | erase-first on delete |
| control/profile_executor_firing_stats.c:891,898 | erase_key+commit | legacy fs_<id> | c | erase-first on delete |
| http/profiles_http.c:760 (callers ~1050) | set_blob | used bitmap | c | only in nvs_erase_slot |
| http/profiles_http.c:1018 | erase_key | legacy profN blob | c | erase-first on delete |
| http/profiles_http.c:1061,1066 | set_blob+commit | profile rev array | c | bumped rev floor for reused ids |
| control/adaptive_tune.c:933,935 | set_u8+commit | enmask-migrated marker | c | one-shot flag |
| persist/relay_cycles.c:253,255 | set_blob+commit | relay cycle counts | c (borderline 1) | one-way copy default partition -> kiln_nvs |
| control/run_state.c:154,156 | set_blob+commit | run record, migration copy | c | default partition -> kiln_nvs |
| control/run_state.c:247,249 | set_blob+commit | run record | a | crash breadcrumb; owner-listed NVS-stay |
| control/firing_shadow.c:156,158 | set_blob+commit | shadow counters | a | owner-listed NVS-stay |
| safety/crash_report.c:364,366 | set_blob+commit | last-crash record | a | |
| safety/crash_report.c:708,710 | erase_key+commit | crash clear | a | |
| safety/estop_verification.c:83,85 | set_blob+commit | e-stop verified | a | |
| safety/estop_verification.c:135,137 | erase_key+commit | clear | a | |
| safety/watchdog_cfg.c:126,128 | set_blob+commit | panic_dis | a | |
| safety/safety_cfg_store.c:625,627 | set_blob+commit | Pico param cache | a | |
| safety/safety_cfg_store.c:795,797 | set_blob+commit | safety relay blob | a | |
| safety/safety_cfg_store.c:932,934 | set_blob+commit | CT cal | a | |
| safety/safety_cfg_store.c:1037,1039 | set_blob+commit | rate-guard meta | a | |
| http/factory_reset.c:237 | hal_kv_erase_partition | partition per scope | a | reset path |

## Class (b) detail: which value wins at boot, can it resurrect?

There are no class-(b) writers, so nothing newly produces a stale NVS copy. The remaining
question is the read fallback the close design keeps: NVS is read only when the cfg file is
absent or lower-rev, then migrated into cfg. Resurrection risk therefore lives in erase paths,
and each is erase-first and checked: `nvs_erase_slot` (profiles), firing stats (both halves),
`live_profile_clear`, and the `kiln_cfg_store` quarantine discard (legacy NVS key erased before
the file; failure aborts, the item stays).

Not verified here: whether factory_reset kiln/profiles scopes also erase the legacy NVS keys of
a never-migrated pre-close board. If a scope only deletes cfg files, such a board could
resurrect old NVS data through the fallback. Recommend a follow-up trace of factory_reset.c
scope lists. Backup import writes cfg files only, so no NVS writer can race it.

## Borderline items

1. `persist/relay_cycles.c:253` writes cfg-owned data (now pref_cfg_fs) into `kiln_nvs`, but only
   in a one-time migration from the default partition, guarded by "kiln_nvs has no blob". The
   old default-partition copy is never deleted, so after a reset erasing `kiln_nvs` it could
   re-seed stale counts. Low impact (counters); not traced further.
2. `http/profiles_http.c:760,1061`: the only live NVS writes in profiles; delete-time rev and
   bitmap floors only. They keep the NVS fallback alive for profiles.
3. `persist/touch_cal_store.c`: user calibration, NVS-only, absent from the owner close list.
   Probably intentional (hardware calibration); confirm.
