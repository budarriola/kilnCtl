# ESP32-S3 brought up to current HEAD (2026-09-10)

Previous flash was HEAD `6355c822` (2026-09-06, per the old
`flash_provenance.json`). Board rebuilt and reflashed from a clean detached
worktree at `origin/main`.

## Build

Worktree: `C:\wt\opus_espflash_0910`, detached at `origin/main`. HEAD moved
once (`aa698221` -> `170f4b75`) between worktree creation and build; rebuilt
against the newer HEAD before flashing so the flashed image matches the
actual current `origin/main` tip. `git submodule update --init` pulled
`firmware/KilnFW/components/lvgl` (`85aa60d1`, unchanged across the HEAD
move). `sdkconfig` copied from the main tree and diffed identical before
building. Build run via the PowerShell tool (`idf.py -C
C:\wt\opus_espflash_0910\firmware\KilnFW build`), not Bash. Build succeeded
both times (full build at `aa698221`, incremental relink at `170f4b75`).

## Flash

`flash_firmware(kiln_fw_root="C:/wt/opus_espflash_0910/firmware/KilnFW",
verify=True)` — flashed and verified OK (bootloader + partition table + app).
Only the ESP was reset (not a dual reflash), so no S6a mainFault trip
occurred; `safety_get_status` showed the link straight up and armed,
`trip_mask 0x0000`, `tx_dropped 0`.

## Pre-flight (before flash)

- `get_heap_status`: `reset_reason='panic/exception'` (unclean boot) from a
  prior session, `uptime_s=12050`; no `UNACKNOWLEDGED CRASH REPORT` banner
  was printed, and the post-flash `/api/readiness` `crash_report` item later
  read `status: ok` ("last crash on record has been acknowledged") — so this
  was a stale, already-acknowledged crash, not a live blocker.
- `safety_get_status`: link up, armed, not tripped, `mains_voltage_v=120`.
- `safety_get_commissioning`: `commissioned=True`, S1 `abs_max_temp_c=80C`
  ARMED, S8 `max_rate_c_per_min=20C/min` ARMED, `mains_voltage_v=120`, S14/S15
  DORMANT (`i_normal_a not measured`).
- Thermocouples: CH0/1/2 ~36.6-36.8 C (cooled from the earlier 45 C plateau).
- Relays: R1-R4 all 0 (off).

## Post-flash verification

- `get_heap_status`: `reset_reason='software (esp_restart)'` (clean),
  `uptime_s=15` at read time. Running partition/build confirmed current by
  `flash_firmware`'s own verify step (RUNNING marker + `fw_build` vs. the
  `.bin`'s embedded build time).
- `debug_check_partition_table`: on-chip table MATCHES `partitions.csv`.
- `get_cfgfs_status`: `mounted=True`, same 6 files as before the flash
  (`display_power.dat`, `ramp_assist.dat`, `relay_cycles.dat`,
  `relay_names.dat`, `unit_pref.dat`, `zones.json`, 77824 B used) — the `cfg`
  LittleFS partition survived the flash mounted, not reformatted.
- Thermocouples: CH0/1/2 ~35.9-36.0 C, plausible. Relays: R1-R4 all 0 (off).
- `safety_get_commissioning` (Pico side, unchanged firmware): `commissioned=
  True`, S8 still `20C/min` ARMED, `mains_voltage_v` still `120` — both
  persisted correctly across the ESP-only flash, no regression.
- **S1 `abs_max_temp_c` changed 80C -> 85C.** Traced to
  `safety_ceiling_policy.c` / `safety_ceiling_sync.h` (new in this HEAD,
  landed today) — the ESP now pushes the configured zone ceiling to the
  Pico's S1 threshold on link-up. This is the new feature working as
  intended, not a persistence defect: nothing was flashed to the Pico.
- `/api/readiness` item `safety_commissioned`: `not_done`, "3 of 65
  applicable safety parameters still have no value" — the 3 are exactly
  `i_normal_a[0..2]` (per `safety_get_commissioning`'s S14/S15 DORMANT
  reasons). `ct_channel_map` is no longer counted, confirming the
  summed-topology mirror fix is now live on the board. ESP and Pico sides
  agree on this.
- `find_crash_elf()` and the `elf_archive` manifest's last entry both point
  at `KilnCtrl-4361467872a4.elf`, `git_commit: 170f4b75` (the real flashed
  HEAD, not the fabricated `abc1234` another session is fixing separately in
  the manifest-writing test) — archive entry for this flash looks correct.

## What this flash newly enables

The `i_normal_a` write path is now present on the board (this HEAD includes
the CT-topology work), so a future CT sweep could populate `i_normal_a[0..2]`
and let S14/S15 arm. No sweep was run and nothing was armed in this task.

## End state

Board left idle: relays off (R1-R4 = 0), no trip latched (`trip_mask
0x0000`), link up and armed, commissioned, recovery_mode false, crash report
acknowledged, `cfg` filesystem mounted normally.
