# Review: KilnFW boot on erased / factory-fresh data partitions (2026-10-17)

Trigger: the bench ESP's data partitions (nvs, kiln_nvs, profiles_nvs, cfg; wifi_nvs kept) were erased before a reflash.
The next boot logged LittleFS -84 on {0x0,0x1}, a cfg auto-format in 751 ms and "zones config UNTRUSTWORTHY", and the
E-stop verified flag was lost. Review only, from source on origin/dev; no bench access. Paths are relative to
`firmware/KilnFW/App/` unless stated.

Counts: HIGH 0, MED 2, LOW 3, INFO 3.

## Answers

1. **-84 is expected and the auto-format is safe.** An all-0xFF partition has no valid commit in metadata pair {0,1},
   so littlefs returns LFS_ERR_CORRUPT (-84). The format is gated. `register_cfg_vfs()` uses
   `format_if_mount_failed=false` (drivers/persist/cfg_fs_mount.c:191). `maybe_auto_format_and_remount()` scans the whole
   partition and formats only if neither block 0 nor block 1 holds a SUPERBLOCK tag chain, valid or corrupt
   (drivers/persist/cfg_fs_format_gate.c:185-217). A read error, an OOM, or any superblock (even one that fails CRC, or an
   unknown disk version) sets awaiting-confirmation and leaves flash untouched. A transient read error on valid data
   cannot cause a format: the error path refuses, and good bytes show the superblock. Remaining gap: FS-03.
2. **No heat is possible.** Zones load as invalid and zone commanding is refused (drivers/http/zones_http.c:749,779-782).
   The E-stop flag (kiln_nvs) reads not-verified, and `readiness_gate_collect()` (drivers/safety/readiness_gate.c) blocks
   profile and autotune starts. Fail-closed, except for the stale-copy case FS-01.
3. **Auth bootstrap.** With `nvs` erased, the policy and the AU-02 `auth_ever` marker are both NOT_FOUND. The policy is
   ABSENT, so auth is off and bootstrap is not offered. That matches first-boot semantics, but see FS-02.
4. **Readiness** lists cfg_fs, storage, estop_verified, the thermocouple/relay/mode/guard items and profile_saved, so the
   re-commissioning list is complete. Wording problems: FS-04.
5. **Silent defaults and divergence:** FS-01 (the stale legacy NVS copy is adopted) and FS-05 (loaders run before the deferred format
   finishes, with no reload afterward).
6. **RAM floor** is respected: FS-06.

## Findings

### MED
- **FS-01** drivers/persist/zones_config_cfg_fs.c:395-401. When zones.json is missing and the kiln_nvs blob is valid, the NVS
  copy is adopted as trustworthy (valid=true) and written into the new file. Since the cfg dual-write close, no code
  writes NVS any more, so that blob is frozen at close time. Scenario: cfg is erased or blank-formatted (an erase like today's but
  without kiln_nvs, or the FS-03 case) while kiln_nvs keeps a pre-close blob. The board silently goes back to months-old
  zone limits, mapping and modes, and readiness "storage" shows green. No log says the copy is legacy. Fix: after the
  close, treat a missing file plus a valid NVS blob as needs-confirmation (or adopt it with a load-fault latch and
  readiness NOT_DONE). Alternatively, erase the legacy key once the file has been written.
- **FS-02** drivers/net/wifi_prov.c:504 -> firmware/hwAbstraction/esp/kv/hal_kv_esp.c:394-406. The default `nvs` init calls
  `hal_kv_init_partition()`, which erases the whole partition on NO_FREE_PAGES/NEW_VERSION_FOUND. Web auth credentials,
  the policy and the AU-02 `auth_ever` marker all live in that partition (drivers/persist/web_auth_store.c:19-34).
  Scenario: an OTA to an IDF with a different NVS format, or a full or interrupted default nvs, makes boot auto-erase
  it. The policy and the marker then read NOT_FOUND, so auth is off and every admin route (OTA, factory reset, safety
  clears) is open on the LAN over the kept wifi_nvs STA link. The only trace is one WARN line, and readiness has no auth
  item. The AU-02/AZ-02 fail-closed design does not cover this, because the marker is erased together with what it
  protects. A deliberate data-partition erase (today) has the same effect. Fix: keep `auth_ever` in wifi_nvs (or
  another partition outside the erase), or refuse the erase-retry for `nvs` and fail closed. Add a readiness/LCD
  notice that a storage reset turned auth off.

### LOW
- **FS-03** drivers/persist/cfg_fs_format_gate.c:198-217. The gate checks only blocks 0/1 for a superblock and ignores
  non-erased bytes anywhere else. It auto-formats even when it reports, for example, 30% non-erased data ("not a filesystem"),
  although the owner decision of 2026-09-07 asks to prompt when files may be present. Scenario: both metadata blocks are
  damaged (a bad erase, or a partition-table shift that moves `cfg`) while file blocks survive; the partition is formatted
  without a prompt. Fix: require confirmation above a small non-erased threshold.
- **FS-04** drivers/http/readiness_http.c:885,1116. On a fresh board "storage" says "zone config failed to load cleanly"
  and the E-stop item says "never confirmed". Both read as faults rather than "storage was reset, re-commission".
  Scenario: after today's erase an operator cannot tell a fresh board from corruption. Fix: distinguish found=false
  (uncommissioned) from a load failure, and name the storage reset.
- **FS-05** drivers/persist/cfg_fs_mount.c:343 (deferred format) vs main_network_http.c:398. The format waits for the
  flash worker and can take "tens of seconds worst case". The zones/profiles/prefs loaders run once, and nothing re-runs
  them when the format completes. Scenario: if the format is slow, every cfg-backed load sees "unavailable (cfg not
  mounted)". Saves in that window get 503, and a valid NVS copy (FS-01) is adopted but not written back. Harmless today
  (751 ms, fresh NVS), but the outcome depends on timing. Fix: reload the cfg-backed modules from the format-completed
  path, or hold the loads until the format settles.

### INFO
- **FS-06** (RAM) drivers/persist/cfg_fs_mount.c:158,407. The gate struct (>8192 B) goes to PSRAM under
  SPIRAM_MALLOC_ALWAYSINTERNAL=8192. The 4 KiB scan chunk is internal and freed before the format. The cfg_autofmt stack is
  3072 B internal and is freed on vTaskDelete. Peak transient internal use is about 7 KiB, inside RESERVE_INTERNAL
  32 KiB, with no steady-state cost, so the 8192 B floor is unaffected. Not bench-measured in this review.
- **FS-07** drivers/persist/cfg_fs_mount.c:340-343. The comment says "3072 words", but IDF `xTaskCreate` takes bytes, so
  the stack is 3 KiB. Comment only.
- **FS-08** Losing the E-stop flag with kiln_nvs is expected and fail-closed (re-attest per the owner decision of 2026-10-10).
