# The config filesystem (`cfg` partition)

Operator-facing reference for the LittleFS migration in progress on KilnFW.
For the design rationale and remaining implementation steps, see
`docs/FILESYSTEM_USER_DATA_PLAN.md`; for why LittleFS was adopted at all
after twice being assessed and declined, see `docs/LITTLEFS_ASSESSMENT.md`
and `docs/audits/flash_endurance_review_2026-09-07.md`.

## What moved, and why boot-critical items did not

User-editable config and user-created data is moving off NVS onto a new
`cfg` LittleFS partition, one JSON-shaped file per domain or object
(zones config, each fire profile, preferences, kiln config slots, ...).
The reason is not wear or corruption risk — NVS was measured fine on both —
it is that a 900-byte `_Static_assert`-pinned C struct in an opaque NVS
blob is not something you can read, diff, or back up by hand, and a file
is.

Some items **stay in NVS permanently, on purpose**, because they are read
or written before the filesystem can possibly be mounted, or because they
are the only thing standing between a bricked board and a recoverable one:

- **Wi-Fi credentials** — provisioning runs before any mount, and Wi-Fi is
  the recovery channel. A filesystem failure must never also take the
  network.
- **Boot-guard counter, watchdog panic-disable, OTA record, crash report**
  — read/written from early boot or panic paths that cannot assume a
  healthy filesystem.
- **Touch calibration** — read during early display bring-up, before
  mount; without it the operator loses the local recovery UI.
- **The RP2040 safety config** — a different chip, with its own flash and
  its own 4 K record store. The ESP filesystem cannot reach it and never
  will.

See `docs/FILESYSTEM_USER_DATA_PLAN.md` §2 for the full item-by-item
MOVE/KEEP table (24 items).

## What the `cfg` partition is

LittleFS at `0xDB0000` on the ESP32-S3's 16 MB flash, added as an
append-only row in `firmware/KilnFW/partitions.csv` — no existing partition
moved or resized. **Grown 2026-09-19** (`docs/PROFILE_SLOTS_100_PLAN.md`
section 7 task 5) from its original 512 KiB to `0x250000` (2.31 MiB), taking
the entire remaining flash tail, to hold the 100-profile-slot table a future
task (plan task 6, gated on plan task 1) will need; `check_flash_partition_map.ps1`
pins the new size. It mounts with `format_if_mount_failed=false`
(a corrupt or blank `cfg` is reported, never silently reformatted — the
opposite policy from the `logs` partition, deliberately: silently erasing
tuning data is the worst failure mode here). It is skipped entirely in
recovery mode.

**Kiln packages live here too, and they are the one thing in `cfg` that
spans both processors.** `kiln_cfg_store` keeps up to 10 named slots, each
holding a zones-config blob *and* a copy of the RP2040's commissioning
parameters, dual-written to `cfg` alongside NVS on the same NVS-authoritative
rule as everything else in this document. Applying one is therefore not a
file write — it is a two-processor transaction (`kiln_cfg_swap.c`), which
records its progress in a marker so a crash mid-apply is recoverable at the
next boot rather than leaving the two processors silently holding different
configurations. The marker states and the recovery rules are owned by
`docs/KILN_PROFILES_PLAN.md` §4.4; what matters for *this* document is that
`cfg` is not the authority for the Pico's half — the Pico's own 4 K record
store is, and the copy here is a snapshot used to re-push it.

**As of 2026-09-07 `cfg_fs_mount_device()` (`main_boot_early.c`) now runs at
boot and auto-formats any partition with no valid LittleFS filesystem in
it** — see "Auto-format and the ask-first path" below. This is source and
host-test evidence only, not a hardware observation: as of 2026-09-17 this
code has never run on the bench board, which is still on commit `3b0c82e`
(built 2026-09-05, 1057 commits behind), so nothing below in this paragraph
has been seen actually happen. **Superseded 2026-09-21:** the bench board,
now running `8ab3b81a`, has `cfg` mounted and populated with 7 files
(including `zones.json`, 900 B), confirmed via `GET /api/cfgfs` on
hardware, not inferred from source; NVS remains authoritative and the
rollback hazard described elsewhere in this repo is unchanged.
**Stale as of 2026-09-30:** by then the count had grown to 9 files
(`display_power.dat`, `ki_base.dat`, `kiln_configs.json`,
`ramp_assist.dat`, `relay_cycles.dat`, `relay_names.dat`, `tz.dat`,
`unit_pref.dat`, `zones.json`) as later work migrated more items to
dual-write, and that same day an owner-approved `backup_export` +
`cfgfs_format(confirm=True)` reformatted the partition, taking the count
to 0 -- see `docs/BENCH_TEST_LOG.md`'s "cfg LittleFS partition backed up
and reformatted" entry. Files stay at 0 until each store's next save,
since the dual-write bridge is write-through only with no seed-from-NVS
on boot (`cfg_fs_mount.c`); NVS remains authoritative throughout, so this
is not a data-loss event. Do not read "7 files" (here or below) as the
current live count. The bench board's `cfg` partition reads
86.6% non-erased (residual bytes left over from before the `cfg` partition
existed in `partitions.csv`, not a filesystem — the density-based gate this
section originally shipped with wrongly treated that as evidence of content
and refused to format it), so once this code is next flashed to the bench
board, its NEXT boot is expected — per the source, unconfirmed on hardware
— to auto-format and mount, making the dual-write bridges live for the
first time. A board whose `cfg` partition instead contains real (if
mount-failed) data is NOT touched automatically — see below. See "Open
items (2026-09-07)" below for the full not-yet-reflashed note.

## Auto-format and the ask-first path

Owner decision, 2026-09-07, refined the same day: "The auto check should be
looking to see if it is a valid file system, not just data. If it is just
data and not file system then just format it." When
`esp_vfs_littlefs_register()` fails, `cfg_fs_mount_device()` reads the raw
partition back (`cfg_fs_format_gate.c`, pure and host-tested) and asks one
question: **is there a valid LittleFS filesystem in the first two blocks
(the redundant superblock metadata-block pair) of this partition?** It
walks each block's real on-disk tag chain (revision count, tagged
CRC-protected commits) looking for a SUPERBLOCK+INLINESTRUCT commit whose
CRC-32 actually checks out and whose version field is sane — exactly what
`lfs_dir_fetchmatch()`/`lfs_format_()` in the pinned `joltwallet/littlefs`
component write and validate, reproduced directly rather than approximated
with byte statistics. Byte density (how much of the partition reads as
non-erased) is **no longer a gating signal** — it is still logged/reported
for operator context, but a partition full of leftover, pre-existing
non-erased data that never formed a real filesystem is auto-formatted
regardless of how much of it there is:

- **No valid LittleFS superblock found** (whether the region reads as
  erased, or is mostly/entirely non-erased residual data with no real
  filesystem structure in it) → formats automatically and remounts. Logged
  at WARN. This is the path every board takes today, including the bench
  board's actual partition (86.6% non-erased leftover data, no filesystem).
- **A LittleFS superblock structure is found** — whether fully valid (a
  real, otherwise-mountable filesystem that failed to mount for some other
  reason) or one that decoded far enough to be recognizably real but failed
  its CRC/version check (a genuine filesystem that is itself corrupt) —
  → refuses to format either way. Logged at ERROR with a loud boot-log
  banner, and surfaced three other ways: `GET /api/cfgfs/format_pending`
  (`{"pending":true,"reason":"..."}`); `/api/status` fields
  `cfg_fs_format_pending`/`cfg_fs_format_reason` (emitted only while pending;
  the reason only to an admin session or with web auth off; host-tested only,
  not bench-verified), which drive an informational banner on the web dashboard and a lowest-priority
  strip on the LCD home page (no action from the LCD; clears once the format
  completes); and a banner on the Settings page
  with an explicit confirm button, which POSTs to
  `/api/cfgfs/format_confirm` (authenticated the same way `/api/factory_reset`
  is — the same danger tier, deliberately reusing that lockout budget
  rather than adding a fifth one). Nothing is ever erased without one of
  these two triggers.

Either outcome never blocks boot, never touches any partition other than
`cfg`, and is skipped entirely in recovery mode (same gate as before).

## Reading `/api/cfgfs`

Since `9310367b` the status path's transient scratch (the handler buffer, the two `cfg_fs_entry_t[32]` arrays, the firing-stats and profiles dual-write status blobs and `firing_stats_cfg_fs_load_raw`'s file buffer) is allocated PSRAM-first through `persist_scratch_alloc()`, so a poll does not move the internal heap low-water mark (bench: 16519 B before and after two GETs); `firing_stats_cfg_fs_save()` is the deliberate exception and stays MALLOC_CAP_INTERNAL because it writes flash from its buffer. The diagnostics page polls this route every 10 s.

`GET /api/cfgfs` reports:

- `"mounted"` / `"status"` — whether `cfg` is mounted this boot, and why
  not if it isn't (recovery mode, never reached that point in boot yet, or
  a real mount failure).
- `"tmp_entries_now"` — how many files currently sit in `cfg`'s internal
  `.tmp/` staging directory. **This should always read 0.** Every write is
  temp-file-then-atomic-rename; a file left in `.tmp/` at any time other
  than mid-write means the last write to that item was interrupted (power
  cut, reset) before the rename committed. It is not data loss — the old,
  previously-committed file is untouched and still what gets read — but a
  non-zero count that persists across a boot (past the point where startup
  sweeps `.tmp/` clean) is worth a look.
- `"files"` / `"subdirs"` — `files` lists the regular files in the cfg root
  only (the directory listing skips subdirectories). `subdirs` summarizes each
  known one-level subdirectory as `{"name","file_count","size_bytes",
  "unknown_size"}`; today that is `profiles` (`prof<N>.json`, `hidden.json`).
  A summary rather than names keeps the response inside its fixed buffer and
  matches `/api/cfgfs/file`, which refuses names containing `/`.
- `"dual_write"` — one row per migrated item in `dual_write.items[]`, e.g.
  `{"name": "zones", "file_backed": true, "file_rev": N, "nvs_backed": true,
  "nvs_rev": N, "diverged": false, "nvs_stale": false, "migration_deferred": false}`.
  `file_backed` means this item currently reads from its file rather than
  falling back to NVS. `file_rev`/`nvs_rev` are the write-sequence counters
  the two sides carry to resolve which copy is newer if they disagree.
  `diverged: true` means the two copies had different content at last
  check — the newer revision won and resynced the loser, so this should
  self-clear; a `diverged` flag that stays true across repeated reads
  means something is repeatedly re-diverging, not resolving.
  Rows today (15): `zones`, `kiln_cfg_store`, `unit_pref`, `profiles_hidden`,
  `zone_normals`, `ramp_assist`, `display_power`, `update_repo`, `tz`, one
  aggregate `profiles` row, `relay_cycles`, `adaptive_tune`, `firing_stats`,
  `relay_names`, `aux_outputs` (this said 13 until 2026-10-05: `aux_outputs` made
  it 14 unnoticed, `update_repo` from `update_settings_get_dualwrite_status()`,
  GitHub-release WP9, the 15th; the `update_repo` row reads `update_repo.dat` and
  NVS key `update_repo` / rev key `upd_repo_rev`). The
  `zone_normals` row comes from `zone_normals_get_dualwrite_status()`
  (`zones_config_store.c`): a fresh read of `zone_normals.dat` and of NVS key
  `zone_norm_cfg` with rev key `znorm_rev`, never a resync write. The
  `relay_names` row (`relay_names_get_dualwrite_status()`) does the same for
  `relay_names.dat` and NVS key `relay_names_cfg` / rev key `relnames_rev`;
  a still-v1 NVS blob reads as valid (upgraded in memory). Every dual-written
  item now has a row (`adaptive_tune` is the `ki_base.dat` row).
  **Struct padding is not data.** A status comparison must never `memcmp()`
  a whole struct that has padding bytes, and a candidate struct handed to
  `pref_cfg_fs_resolve()` must be zero-initialised (`memset`) before it is
  filled field by field, because resolve compares raw bytes. Found on the bench
  2026-10-04: `relay_cycles` read `diverged: true` at `file_rev == nvs_rev`
  because `relay_cycles_init()` built an un-zeroed candidate (stack garbage in
  the padding) and the status accessor compared the whole struct. Fixed by
  zeroing the candidate and comparing field by field. A `relay_cycles` file
  written by the old init resync still carries garbage padding, so the first
  boot of the fixed firmware logs one more "DIVERGED, adopting NVS" at equal
  revs and rewrites the file with zero padding, after which it stays in sync.
  The status accessors read NVS with the quiet variants
  (`pref_cfg_fs_load_raw_quiet()`, `relay_names_decode_any_impl(quiet)`;
  `ca44e060`), so a `GET /api/cfgfs` poll never logs wrong-size/REJECTED
  warnings. The standing suite's `check_flash_worker_lint.ps1` allowlists the
  three factory-reset scope-sweep `cfg_fs_delete` sites in
  `flash_worker_lint.py`'s `CFG_FS_ALLOWLIST` (`7570ac19`).
- `"nvs_only"` — items that have not moved to file backing yet. **As of
  `2e88e90a` (2026-09-08) this array is empty** — every item this doc's
  migration table (below) tracks as MOVE (1-9) now has a real cfg-filesystem
  bridge and its own `dual_write.items[]` row; nothing genuinely NVS-only
  remains among the migrated set. `cfgfs_nvs_only_drift_check.py` fails the
  build the moment a new `persist/*_cfg_fs.c` bridge lands without a matching
  `dual_write.items[]` row, so this array should not go stale silently a
  third time (see `2e88e90a`'s commit message for the second time it did).
  Zone normals (table item 2) now has its bridge and its own row
  (`zone_normals`, above), so nothing is NVS-only any more.
- `"nvs_permanent"` — stores that stay in NVS by design, so an operator does
  not read them as pending migrations. The list is hand-maintained in
  `cfg_fs_status.c` and mirrors every store that uses NVS with no cfg file
  path: the original boot-ordering and safety set (`wifi_creds`,
  `boot_guard_counter`, `watchdog_panic_disable`, `ota_record`,
  `crash_report`, `touch_cal`, `run_state_breadcrumb`, `safety_mirror_esp`,
  `rp2040_config_store`, `logs`, `coredump`) plus, since 2026-10-07,
  `profiles_favorites`, `live_profile`, `firing_shadow`, `ct_verify_store`,
  `kiln_cfg_swap`, `aux_convert_journal`, `setup_wizard_progress`,
  `pico_update_attempts`, `pico_image_manifest`, `estop_verification` and
  `dualwrite_window`. The web-auth and TOTP
  credential stores are also NVS-only but are deliberately not named here
  (`check_kiln_auth_config_isolation.ps1` keeps credential names out of every
  config-path file). `iter_tune_store` is deliberately absent pending an
  owner ruling. `test_cfg_fs_status.c` pins the post-2026-10-07 names.

## If the filesystem fails to mount

Saves are refused until cfg mounts (owner decision 2026-10-06, see "cfg
unmounted: refuse and prompt the format" below). Loads still fall back to the
legacy NVS copy, so the board boots and runs on the last values it had. A persistent mount failure (as opposed to
"not yet formatted") is reported via `/api/cfgfs`'s `status` field; when the failure is the ask-first gate refusing to format,
the LCD/web dashboard banner described above (`/api/status`) shows it too.
No safety decision
is allowed to depend on a file successfully mounting — guard thresholds
still resolve from NVS whenever the file side is unavailable.

## What factory reset does to it

The Settings page's "Factory default (erase everything)" button (scope
`all`) now also erases and reformats `cfg`, unconditionally — its own
confirm dialog IS the explicit operator action the mount-failure contract
calls for, so this path never goes through the ask-first flow above.
"Wi-Fi only"/"kiln config only"/"profiles only" never format `cfg` — it
holds a mix of kiln-config-shaped and profile-shaped data. Each narrower
scope deletes only its own mirror files: "kiln config only" deletes the
mirrors whose NVS side lives in `kiln_nvs` (`kiln_scope_cfg_files.c`),
"profiles only" deletes `hidden.json` plus every profile-slot
(`profiles/prof<N>.json`) and firing-history (`stats/fs<N>.dat`) file
(`profiles_scope_cfg_files.c`, which lists those two directories), "Wi-Fi
only" has none. Leaving a
mirror behind would let the file win the next boot and undo the reset.

## State of the migration, 2026-09-07

**Update 2026-10-06:** the "dual-write" label in this table is historical. Every item marked dual-write below is now cfg-only on save, with NVS kept as a read-only legacy source (see "NVS dual-write close" at the end of this document).

Of 24 inventoried runtime-changeable items:

| # | Item | Status | Commit |
|---|---|---|---|
| 1 | Zones config (PID/FOPDT/coupling/guards/wiring/tc_type) | dual-write | `19f74959` |
| 2 | Zone normals | dual-write (`zone_normals.dat` via `pref_cfg_fs`; `/api/cfgfs` row `zone_normals`) | `208de3d4` |
| 3 | Relay names | dual-write (`relay_names.dat` via `pref_cfg_fs`; `/api/cfgfs` row `relay_names`) | `288dc91c` |
| - | Aux outputs (spare-relay on/off; not one of the 24) | dual-write (`aux_out.dat` via `pref_cfg_fs`; NVS key `aux_out_cfg`, rev key `aux_out_rev`, both in `kiln_nvs`; `/api/cfgfs` row `aux_outputs`; `persist/aux_outputs_cfg.c`; NVS authoritative). Factory reset: the `kiln` and `all` scopes erase `kiln_nvs` and `kiln_scope_cfg_files.c` lists `aux_out.dat`, so aux returns to all-disabled. In the backup export/import as the top-level `aux_outputs` array (omitted when the store is quarantined; an absent key on import keeps the board's aux configuration). `docs/SPARE_RELAY_ONOFF_PLAN.md` | `1f70c419` |
| 4 | Relay cycle counters | dual-write | `762bb29e` bridge, `2e88e90a` /api/cfgfs reporting |
| 5 | User fire profile slots 0-7 | dual-write | `530dc2f7` |
| 6 | Hidden-builtin profile mask | dual-write, `/cfg/profiles/hidden.json` (NVS key `prof_bihid` + rev `prof_bihid_rev`; `/api/cfgfs` row `profiles_hidden`) | `2749be53` |
| 7 | Firing stats / history | dual-write | `762bb29e` bridge, `2e88e90a` /api/cfgfs reporting |
| 8 | Named kiln config slots | dual-write (`kiln_configs.json`, `kiln_cfg_store_cfg_fs.c`) | `9bd29cff` |
| 9 | Adaptive-tune Ki baseline (opt-in mask lives in zone config, off this table) | dual-write | `762bb29e` bridge, `2e88e90a` /api/cfgfs reporting |
| 10 | Ramp-assist enable | dual-write | `34927a77` |
| 11 | Unit preference (C/F) | dual-write | `34927a77` |
| 12 | Display power / backlight policy | dual-write | `34927a77` |
| 13 | Touch calibration | deliberately staying in NVS (pre-mount boot read) | — |
| 14 | Time-sync TZ | dual-write (`tz.dat` via `pref_cfg_fs`; host-tested by `test_time_sync.c` through an `esp_netif_sntp.h` stub) | `288dc91c` |
| 15 | Wi-Fi credentials | deliberately staying in NVS (recovery channel) | — |
| 16 | Run-state resume breadcrumb | deliberately staying in NVS (boot-phase read) | — |
| 17 | Boot-guard unconfirmed-boot counter | deliberately staying in NVS (pre-mount) | — |
| 18 | Watchdog panic-disable flag | deliberately staying in NVS (pre-mount) | — |
| 19 | OTA record | deliberately staying in NVS (must survive a bad VFS) | — |
| 20 | Crash report + ack | deliberately staying in NVS (must survive a bad VFS) | — |
| 21 | Safety commissioning mirror (ESP-side cache) | deliberately staying in NVS (cache of #22) | — |
| 22 | RP2040 config store | deliberately staying (different chip, no ESP filesystem reaches it) | — |
| 23 | Firing/autotune telemetry logs | on `logs` (SPIFFS), separate track, not this partition | — |
| 24 | Core dump | deliberately staying (own partition, not user config) | — |

Supporting infrastructure, not tied to one inventory item:

| Change | Commit |
|---|---|
| `cfg` partition added to the table (append-only) | `234ce9f3` |
| `cfg` partition table flashed to the bench board | `c4b4e65d` |
| `cfg_fs` foundation module (mount/read/write-atomic/delete/list) | `d2a1358d` |
| `/api/cfgfs` status endpoint | `b79b5ef5` |
| LittleFS component pinned (host-build only, prerequisite) | `ca5d90c5` |
| Equal-rev tie-break defect found and fixed (zones + profiles) | `2c7bd240` |
| Boot-time mount call, auto-format-or-ask gate, `/api/cfgfs/format_pending`+`format_confirm`, factory-reset "all" scope format | *(this pass, 2026-09-07)* |

13 items are dual-written (1-12, 14), 10 stay in NVS deliberately (13, 15-22, 24),
and 1 (23) is on the separate `logs` track.

## Open items (2026-09-07)

- **Not yet reflashed to the bench board (added 2026-09-17) — superseded
  2026-09-21.** The bench board now runs `8ab3b81a` and `cfg` is mounted
  and populated (7 files, observed via `GET /api/cfgfs`, not inferred from
  source); NVS is still authoritative and the rollback hazard is
  unchanged. The rest of this bullet is kept for history. Everything
  below in this section describes what the mount/auto-format code in
  `main_boot_early.c`/`cfg_fs_mount.c` does; none of it has actually run on
  the bench board as of 2026-09-17. `get_fw_version()` against the live
  board reports commit `3b0c82e`, built 2026-09-05 — 1057 commits behind the
  HEAD this mount wiring landed on — and `GET /api/cfgfs` on that board
  answers `no such endpoint`. Read every "the bench board's next boot..."
  and "makes the dual-write bridges live" sentence below as a description of
  intended behavior on the CURRENT source, not an observed result, until the
  board is next reflashed and this note is updated with what actually
  happened. See `docs/RELEASE_HARDENING_PLAN.md` section 10 for the
  decision this drove (parked, not finished, pending bench time).
- **Dual-write window (closing, see "NVS dual-write close" at the end of this
  document).** Superseded: the paragraph below describes the window as it was
  before the owner decision of 2026-10-05; saves are now cfg-file-only. Every
  migrated item used to write both the
  file and its NVS copy; reads prefer the file. This closes — NVS writers
  removed — only once, on the bench board: 20 consecutive clean boots with
  no mount failure/fallback/defaults-banner, one complete firing run
  entirely from file-backed config with gains/coupling read back
  byte-identical to the NVS copy, and one proven backup/restore round trip
  against the file path. None of the three has started; the clock has not
  begun because nothing is mounted on any board yet.
- **`cfg` was unformatted on the bench board — now resolved.** The mount
  call and the auto-format-or-ask gate both landed this pass (see
  "Auto-format and the ask-first path" above); the bench board's next boot
  auto-formats and mounts. **Confirmed on hardware 2026-09-21:** the bench
  board (`8ab3b81a`) now has `cfg` mounted with 7 files present, per
  `GET /api/cfgfs`. The ask-first refusal path now also shows on the LCD
  home strip and in `/api/status`/the web dashboard (not just the boot log
  and Settings page); host-tested only, not bench-verified. **Stale as of 2026-09-30:** the file count had
  grown to 9 and was then reset to 0 by an owner-approved backup/reformat
  (`backup_export` + `cfgfs_format(confirm=True)`); see the note above and
  `docs/BENCH_TEST_LOG.md`.
- **RP2040 `config_store`'s zero-valid-copies erase window — fixed.** The
  endurance review found that the old single-sector, 8-slot round-robin
  erased the *entire* 4 KiB sector before reprogramming slot 0, leaving
  **zero valid copies** of the safety configuration (TC type,
  `abs_max_temp_c`, `max_rate_c_per_min`, CT cal) during that window — a
  power cut or watchdog reset there booted the RP2040 uncommissioned.
  `24090c9a` adds sector B (`SAFTYFW_CONFIG_STORE_FLASH_OFFSET_B`, inside
  the already-reserved 64 KiB region — no partition/layout change).
  Writes now always target whichever sector is NOT current, so a crash
  during erase/program leaves the other, untouched sector's committed
  record intact; `config_store_find_latest_multi_ex()` is the sole
  arbiter, scanning both sectors' 16 slots and keeping the highest-`seq`
  CRC-valid record. There is no separate "which sector is active" pointer
  to tear — sector choice is derived fresh from the seq/CRC scan every
  boot, which is why the "reset one side of a pair" bug class does not
  apply here. What CRC still cannot catch: a write that completes with a
  valid CRC but wrong content — the ordinary limit of any CRC scheme.
  169/169 config_store host tests pass, including power-cut injection at
  each step of the erase/program sequence. Follow-ons `4f1b9a4f` (a
  discarded flash-program failure inside `config_store_write_cb()`, now
  checked and logged) and `fd02df05` (TC type + cal-offset commissioning
  UI) build on top of it.

  A separate defect in the same file, found and fixed 2026-09-09: the
  in-RAM cache (`s_cached_record`) was written by link_task on core 0 with
  a plain struct assignment and read with no synchronisation by
  safety_core/thermo_task/current_task on core 1, including the trip
  path — confirmed live, with `safety_config_version` changing under a
  concurrent guard-threshold read during a heating run. `b202fe56` adds a
  seqlock (`config_store_seqlock_read()`/`_write()`): the writer bumps a
  volatile counter odd/even around the struct write, readers snapshot and
  retry up to 4 times before falling back to the last stable snapshot
  rather than block the trip path or return a torn struct. Reverting the
  writer to a plain assignment reproduced 20,945 torn reads over ~190k
  writes on the host harness; with the seqlock, zero. `5671ee03` fixed the
  barrier primitive itself (`hal_barrier.h`'s pico backend must use
  pico-sdk's `hardware/sync.h` `__dmb()`, not a bare CMSIS `__DMB()` that
  depended on an unguaranteed include order).

  A third fix landed the same day, `cb1ba325`: review of `b202fe56`/
  `5671ee03` found the seqlock's own fallback snapshot (`s_last_good_record`)
  was written by *every successful reader* on either core via a plain
  unsynchronised ~512 B struct assignment — two concurrent readers (link
  path on core 0, trip path on core 1) could race each other writing it,
  a torn read on exactly the path the seqlock exists to protect, invisible
  to the single-reader host test. Fixed by making the fallback a
  writer-owned double buffer (`s_fallback_buf[2]` + `s_fallback_active`):
  only `config_store_seqlock_write()` (the sole writer, core 0) ever writes
  either slot, into whichever is not currently active, flipping the index
  only after that write lands — readers never write shared state.

  A fourth, distinct defect in the same file — write atomicity (finding D2,
  `docs/audits/unreviewed_changes_review_2026-09-08.md`) — is now fixed too,
  2026-09-14: `config_store_next_write_slot()` picked the next slot by
  arithmetic alone (`latest_valid + 1`), trusting it was still blank. A slot
  torn by a power cut mid-program fails its own CRC and is correctly skipped
  by the boot scan, but its bytes stay non-erased — the very next write
  landed back on that same slot and, per `hal_flash_program()`'s
  AND-programming semantics, silently corrupted onto it while every flash
  call still reported `HAL_OK`: the caller believed the write succeeded and
  the RAM cache adopted the new value, both wrongly. Fixed by (a) checking
  the target slot is actually still erased before programming into it —
  switching to the other sector and erasing it first if not, same as the
  natural 8th-write case — and (b) a read-back verify after every program,
  so a mismatch is reported as a real write failure rather than silently
  trusted. This is a THIRD defect distinct from both the A/B erase-window
  fix and the RAM-cache seqlock above — neither of those ever inspected
  whether the target flash slot was actually erased. Full detail,
  including why (a)/(b) are not the same fix as either prior one and the
  new host-test coverage (torn-slot-reuse, both the ordinary and
  never-committed cases): `docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md`.
  **Bench-verified 2026-09-14** (`88bb4333`, flashed from a clean detached
  worktree at HEAD, probe serial E66540F0A36C6E21/COM10): pre-flash board
  was healthy (link up, armed, no trip, relays off, no unacknowledged
  crash; Pico was on `a01a0f43`, config_version=149,
  `abs_max_temp_c`=80C, S8=20 C/min over 60s, `tc_type`=3,
  `mains_voltage_v`=120, CT channels uncalibrated). The flash tripped the
  expected S6a `mainFault` (`trip_mask`=0x0020) in the reflash handshake
  window; link came back up and the trip cleared normally. Verified on
  real flash: (1) a benign, reversible commissioning field
  (`mains_voltage_v`) was written, confirmed by read-back, and round-
  tripped through two real `debug_reset(peer="pico")` reboots (121 then
  back to 120), proving the fixed write path persists correctly across
  reboots on hardware, not just in the host-test fake-flash harness; (2)
  every one of those on-hardware writes exercised (b)'s read-back-verify
  path on its non-failure branch and reported success, which is as far as
  this path can be exercised safely — deliberately tearing a real flash
  program to trigger a mismatch was judged unsafe/irreversible on the only
  bench Pico and was **not** attempted, so (b)'s actual mismatch-detected
  branch, and (a)'s slot-skip-and-switch behavior for a genuinely torn
  slot, remain proven only by the host-test power-cut injection harness
  (`test_config_store_flash.c`), not on real hardware; (3) `abs_max_temp_c`,
  the S8 rate guard, `tc_type`, and CT calibration were read back unchanged
  after the whole sequence, confirming no regression to safety-relevant
  config across the flash and three reboots. `abs_max_temp_c` was
  deliberately never touched. Final state: both processors healthy, link
  up, SaftyFW ARMED, no trip, relays off, no unacknowledged crash, all
  commissioned values restored to their pre-flash readings. Full detail:
  `docs/audits/rp2040_config_store_write_atomicity_2026-09-14.md` §8.

  The A/B sector fix (`24090c9a`) is flashed to the bench Pico (`b7af9ebe`,
  2026-09-08: commissioning config read back byte-for-byte across the
  migration, CRC unchanged). The seqlock fix (`b202fe56`/`5671ee03`/
  `cb1ba325`) was flashed 2026-09-09 (`ae23aba4`, from a clean detached
  worktree at HEAD — commissioning and S8 survived the reset) and again
  2026-09-10 to a later HEAD (`b88ea6ba`); commissioning survived both
  resets. Full detail:
  `docs/audits/flash_endurance_review_2026-09-07.md` §5, R2.
- **`config_version` can regress across a reboot — by design, now named.**
  `config_store_write_volatile()` (`SAFETY_CMD_APPLY_CONFIG_VOLATILE`,
  KILN_PROFILES_PLAN.md item 15) bumps `s_cached_record.seq` — and
  therefore the reported `config_version`/`config_crc` — in RAM only; it
  never touches flash. A reboot right after one drops `config_version`
  back to whatever was last durably committed (bench-observed 171 -> 167),
  which reads as a durability bug unless you know the RAM-only path
  exists. `config_store_is_volatile_dirty()`/
  `config_store_get_persisted_config_version()` (2026-09-23) name this
  explicitly, and the divergence is now also visible off-board: DIAG frame
  bit7 (`KILNLINK_DIAG_FLAG_CONFIG_VOLATILE_DIRTY`, purely additive, no
  `KILNLINK_PROTOCOL_VERSION` bump) carries it to the ESP, decoded into
  `GET /api/status`'s unconditional `config_volatile_dirty` field.

## Dual-write window: now measured (2026-09-07)

The "Dual-write window" open item above previously had nothing checking
whether its three conditions were actually met — see
`docs/FILESYSTEM_PLAN.md`'s "Closing-criterion measurement added,
2026-09-07" section for the full design. Summary for anyone landing here
first: `firmware/KilnFW/App/drivers/persist/dualwrite_window.{c,h}` counts
consecutive clean boots (reset on an unclean reset reason, a pending
unacknowledged crash, `cfg_fs` not mounted, or an explicit mount-failure
note) and records the two one-time achievements (a file-backed firing
completed; a restore round trip verified) in plain NVS — deliberately not
on `cfg` itself, so a `cfg` bug cannot corrupt the evidence being used to
judge `cfg`. Read progress at `GET /api/dualwrite_window`; a
`dual_write_window` object is also on `/api/cfgfs` (same five fields, or
`{"known":false}` if unreadable; emitted by `cfg_fs_status_build_json_ex()`). The reported
`window_may_close` is a **report only** — closing the window (removing the
NVS writers) stays a deliberate, reviewed, owner-visible step performed by
hand once all three conditions read true; nothing in this codebase acts on
that flag automatically.

## Dual-write window: evidence, 2026-10-05

Bench fixture (192.168.1.156, firmware `5bbdb714`), all three conditions run
on the board with no NVS writer removed:

- **20 consecutive clean boots.** 20 `debug_reset(peer="esp")` boots, each
  followed by `GET /api/cfgfs`: mounted, 10 files, 0 `.tmp` entries, 0
  `nvs_only`, and all 14 rows `diverged:false` on every read (14 rows today:
  `aux_outputs` is new since the 13 listed above). `persisted_count` stayed 0
  (boot_guard threshold 3), no unacknowledged crash banner, no trip after
  any counted boot, post-boot `heap_internal` min_free 15.5-17.1 KB. One
  extra boot (R1) is not counted: the SX1509 expander failed init ("no
  SX1509 at 0x3E"), latching S6a; `safety_clear_trip` refused while the
  fault line was asserted and a second reset cleared it. Three resets
  (after counted boots 4, 13 and 18) failed with both cores halted at PC
  0x403C8908 and the board dark; each was recovered at once with
  `debug_reset(allow_dark_rereset=True)`, with no S6b and a clean boot. That
  is a reset-tooling observation, not a cfg result.
- **File-backed firing.** Profile #0 `M18C_TEST` (40 C then 30 C, 3 min of
  dwell), run to completion (exec state 3, no fault, zones peaked about
  47 C, relays off, safety armed, no trip), then acknowledged. `/api/cfgfs`
  afterwards: 10 files, 0 tmp, 14 rows `diverged:false`.
- **Backup round trip.** `backup_export` (11019 B), `backup_import
  (confirm=True)` ("ok - restored", 0.83 s, readiness 20 ok / 1 not_done /
  3 other before and after), re-export (11019 B). The `profiles`, `zones`,
  `timing_profiles`, `safety_tc_type` and informational sections are
  identical. The files are not byte-identical: the `kiln_configs` package
  `esp_blob_hex` differs in 11 of 896 bytes (7 float-field bytes in the
  zone coupling tau/dead-time area, plus the 4-byte CRC) and `pkg_hash`
  differs with it, consistent with the blob being rebuilt from the
  1-decimal rounded live values; the live `zones` data did not change.
  `/api/cfgfs` afterwards: 10 files, 0 tmp, 14 rows `diverged:false`.

The board's own counter reads `consecutive_clean_boots` 24 (target 20),
`firing_complete:true`, `restore_verified:true`, `window_may_close:true`.
This is a report only; removing the NVS writers remains an owner decision.

## NVS dual-write close (owner decision 2026-10-05)

The three preconditions in "Open items" were met on the bench (the evidence
section above, plus the backup round-trip drift fix on origin/main and a
fresh soak), so the dual-write window closes: every item that `/api/cfgfs`
lists (15 rows) is saved to the `cfg` partition only. This section is the
plan; it was written before the code was finished and the implementation
follows it.

### Read order (unchanged in shape, NVS stays readable)

1. The cfg file wins when it is valid.
2. If the file is missing or invalid and a valid legacy NVS copy exists, the
   item loads from NVS and is then migrated once: a verified write into cfg
   (`cfg_fs_write_atomic` already reads the file back). A board upgrading
   from older firmware therefore keeps its settings and ends up file-backed
   after its first boot.
3. If both exist and differ, the strictly higher rev wins. At equal revs with
   different bytes NVS wins, because the only writer that can produce that
   state is a legacy or rolled-back firmware that wrote NVS without touching
   the file (`pref_cfg_fs_resolve()`, unchanged).
4. After the close a save writes the file at rev `max(nvs_rev, file_rev) + 1`
   and never touches the NVS copy, so the NVS rev stays old and the file rev
   is strictly higher.

Marker and migration writes that are not config saves stay as they are:
relay_cycles' NVS-to-NVS default-partition migration, adaptive_tune's
enable-mask migrated marker, and the iter_tune store (not a `/api/cfgfs`
item). Each is documented in the code where it lives.

### Write failure: fail loud, never fall back to NVS

A failed cfg write (partition not mounted, `cfg_fs_write_atomic` or its
read-back verify failing, injected failure in tests) is returned to the
caller as an error and logged as "NOT persisted ... NVS is no longer
written". The in-RAM value takes effect first, exactly as before, so a
running firing is not interrupted by a storage fault, but the caller (HTTP
handler, backup import, LCD page) is told the save did not stick and the rev
only advances after a verified write. Nothing silently writes the NVS copy
instead; that would recreate the stale-NVS problem below in the opposite
direction. A board with no cfg partition mounted (recovery mode, mount
failure) therefore cannot persist these settings until cfg mounts; this is
deliberate, and the owner confirmed it on 2026-10-06 (see the last section).

### What `/api/cfgfs` and readiness report afterwards

- `dual_write.write_mode` is `"cfg_only"`.
- A per-item row gains `nvs_stale`: true when the legacy NVS copy is valid,
  its content differs from the file, and the file rev is strictly higher.
  This is the expected state of any item saved after the upgrade and is not a
  fault. `diverged` keeps its meaning for the dangerous cases only: equal
  revs or an NVS rev ahead of the file, with different content. The two
  flags are mutually exclusive (`cfg_fs_status_item_nvs_stale()`).
- The profiles and firing_stats aggregate rows are reclassified per slot and
  per id the same way.
- The `dualwrite_window` counters (clean boots, firing complete, restore
  verified) stay as a historical record; the window they gate is closed.
- Readiness item `cfg_fs` is now `not_done` when cfg is unmounted (see the last section).

### Rollback hazard

An older firmware reads NVS only, so after any post-close save it sees the
values from before the upgrade (or from the last save made by a pre-close
firmware), not the current ones. This is the same class as the `zones_cfg`
schema-bump hazard in CLAUDE.md's `ota_rollback_esp()` note: flash is left
alone, reflashing the newer firmware restores everything, but a firing
started right after a rollback runs on stale values. After any rollback,
read back `control_get_zones` and the other settings before heating. The
legacy NVS copies are deliberately left in place for this reason (an older
image still boots with something sane) and are only erased by the delete
paths below.

### Delete design change: erase NVS first, checked, then the file

Previously a delete removed the file and left the NVS copy, which was safe
only because every save rewrote both. With NVS no longer written, deleting
only the file would resurrect the stale NVS copy through read step 2. So a
delete now erases the legacy NVS keys first and checks the result; if that
fails the delete fails loud with the file left intact; only then is the cfg
file deleted. There are no tombstones: after the erase the item is simply
absent in both stores, which already means "defaults". The profiles delete
also persists its rev bump, and `kiln_cfg_store_quarantine_clear` erases the
legacy NVS blob and rev key explicitly so a cleared quarantine does not
re-trigger from the old NVS copy.

### Tests

Host tests cover: a cfg-only save (NVS untouched), legacy NVS-only load plus
migration into cfg, cfg write failure (error returned, rev unadvanced, no NVS
fallback, file intact), delete not resurrecting, `nvs_stale` versus
`diverged`, and the backup export/import round trip. The key ones were
negative-tested.

### Results and review pass (2026-10-06)

Mutators report failure. Every public `kiln_cfg_store` mutator (save,
clone, apply-id, delete, rename via the new `kiln_cfg_store_rename_ex`,
`set_active_id_raw`) now returns false when the cfg file write fails, rolls
its RAM change back, and puts `KILN_CFG_PERSIST_FAIL_TEXT` in the reason.
`kiln_cfg_http.c` answers such a failure with HTTP 500 (400 stays for
ordinary refusals) and `backup_import.c` appends the reason to its error.
`kiln_cfg_store_quarantine_clear` refuses from a PSRAM-stacked caller before
its legacy NVS erase, like every other write path in the file.

Verification:

- Host tests (`check_00_kilnfw_host_tests`, clean build each run): pass.
  New `test_cfg_fs_all_mutators_report_persist_failure`; the unmounted,
  mount-failed, tie-break, stale-delete and nvs_fallback tests were updated
  to the fail-loud behaviour.
- Negative test: an NVS write injected into `update_settings.c`'s save path
  made `test_update_settings` fail, and, after adding an NVS-absent
  assertion to the update-settings round trip in `test_backup_import.c`,
  `test_backup_import` fails too. The source was restored by hand, compared
  byte-identical, and a clean rebuild passed.
- Target build: `.dram0.bss` 100200 B against the 101000 ceiling (800 B
  headroom, same as origin/main; `zones_config_store.c`'s verify buffer moved
  from a static to `persist_scratch_alloc()` to keep it there). Stack budget
  checks pass: httpd honest free 3240 B (39.6% of 8192 B), executor 3132 B
  (51.0% of 6144 B). `check_uri_handler_cap`: 171 routes against a cap of
  175. The `/api/cfgfs` 4608 B buffer is part of a `persist_scratch_alloc()`
  allocation, not a stack buffer.
- `flash_worker_lint.py`: seven stale `ALLOWLIST` entries (the converted
  preference files) removed; lint clean.

Behaviour when the cfg partition is NOT mounted (owner decision 2026-10-06,
last section): these paths refuse to save and return an error rather than falling
back to NVS: `pref_cfg_fs_save`/`pref_cfg_fs_commit` (unit_pref,
update_settings, aux_outputs_cfg, display_power_cfg, ramp_assist_cfg,
time_sync, relay_cycles snapshot, adaptive_tune ki baseline, hidden-builtin
mask, relay_names, zone_normals); every `kiln_cfg_store` mutator and, through
them, backup import; `profiles_cfg_fs_save`/`_delete`; `zones_config_cfg_fs_save`
(zones POST, autosave, migration write-back, import); `firing_stats_cfg_fs_write`.
`iter_tune_store.c` keeps writing NVS and silently skips its cfg write
(deliberate exception, documented in the last section). Loads fall back
to the legacy NVS copy.

### cfg unmounted: refuse and prompt the format (owner decision 2026-10-06)

Decision: "Refuse, and prompt the format." Saves stay refused while cfg is not
mounted, and every surface now says why and what to do instead of failing
quietly or reporting success.

- One shared message, `CFG_FS_NOT_MOUNTED_TEXT` in `persist/cfg_fs.h`:
  "settings storage (cfg) not mounted - confirm format via POST
  /api/cfgfs/format_confirm". It is a string literal so handlers can
  concatenate it into JSON without a stack buffer.
- Shared HTTP helper, `common/cfg_fs_refusal_http.h` (header-only):
  `cfg_fs_http_refuse_if_unmounted(req)` is a pre-check at the top of each save
  handler, before any RAM mutation, and answers `503` with
  `{"ok":false,"error":"<text>"}`. `cfg_fs_http_persist_failed(req)` is the
  post-failure response: `503` with the same text when cfg is unmounted, `500`
  `{"ok":false,"error":"could not be saved to flash"}` when it is mounted but
  the write failed. Neither is ever a success body.
- Save routes using it: unit preference, update settings, `settings/tz`,
  `settings/display_power`, ramp assist, relay cycles reset and restore, the
  profile save, delete, builtin hide and builtin restore routes, `POST
  /api/zones` (pre-check, and a failed zones or relay-names save is now a 500
  instead of `ok`), `POST /api/aux_outputs` (the aux core maps
  `ESP_ERR_INVALID_STATE` to a 409 "zone conflict", so it needs the pre-check),
  the four `kiln_cfg_http.c` mutators (save, clone, delete, rename; a persist
  failure with cfg unmounted is a 503 instead of a 500), and the backup import
  job (a real import is refused with 503 before anything is applied; a dry run
  still works because it writes nothing).
- `GET /api/readiness` item `cfg_fs` is `not_done` (was `deliberately_off`)
  when cfg is unmounted, with a detail naming `POST /api/cfgfs/format_confirm`,
  and a variant saying "awaiting format confirmation" when the ask-first gate
  refused to auto-format. It stays non-gating for a firing: only the four gate
  items (recovery_mode, safety_trip, crash_report, estop_verified) block one,
  because a firing runs from the config already in RAM.
- LCD: the one existing path is the home-page strip shown while the format
  confirmation is pending; its text now reads "SAVES REFUSED: CONFIG FS NEEDS
  FORMAT CONFIRM -- see web Settings". No new page. The web banner says the
  same. A plain unmounted board with no pending confirmation has no LCD
  message; the readiness item and the 503 bodies carry it.
- Recovery mode: `cfg_fs_mount_or_skip(true, ...)` deliberately never mounts
  cfg (recovery must work with the partition erased), so saves are refused
  there too. Because the partition then still holds the board's only
  up-to-date config, recovery mode must never advise a format:
  `cfg_fs_skipped_for_recovery()` records the skip, the 503 body uses
  `CFG_FS_RECOVERY_SKIPPED_TEXT` ("... not mounted in recovery mode - leave
  recovery mode (POST /api/ota/esp/recovery_exit) to save settings; do not
  format"), the readiness `cfg_fs` detail says the same, and `POST
  /api/cfgfs/format_confirm` answers `409` instead of formatting. The separate
  `firmware/KilnFW_recovery` image does not link `cfg_fs` at all and has no
  settings save routes. No mount was added to recovery.
- Profile delete: `nvs_erase_slot()` erases the legacy `profN` key first, then
  clears only that slot's bit and rev entry in the legacy NVS bitmap and rev
  array (read-modify-write). Writing the whole in-RAM bitmap and rev array, as
  the dual-write code did, made every other slot saved after the close look
  like an equal-rev divergence (the stale NVS copy won at the next boot) and
  deleted the file of any slot created after the close.
- Persisted read-backs: `zones_config_persisted_equals_ram()` (formerly
  `zones_config_nvs_equals_ram()`) and `aux_outputs_cfg_verify_persisted()`
  re-read the cfg file, not NVS. The zone-to-aux conversion's final read-back
  (`zone_aux_convert_http.c`, `op_verify_persisted()`) calls both; read
  against NVS, which no save updates any more, every conversion would have
  ended in a failed read-back. The profile retarget step's per-slot read-back
  (`profiles_http.c`, `retarget_verify_slot()`) re-reads the slot's cfg file
  through `profiles_cfg_fs_load_raw()` at the RAM rev for the same reason.
- `iter_tune_store.c`: its cfg write stays a silent no-op when cfg is not
  mounted. It runs from the autotune/iteration path with no request to answer,
  and its NVS write still holds the data. This is documented at
  `cfg_fs_save_raw()` and is intentionally not turned into an error.

Tests: `test_readiness_commissioning.c` pins the item status and detail text
(recovery variant included), `test_cfg_fs.c` pins the recovery-skip flag and
text, `test_profiles_http.c`
(`test_pcfg_delete_does_not_revert_or_drop_other_file_only_slots`) covers the
profile delete,
`test_zones_http.c` (`test_zones_persisted_equals_ram_reads_the_cfg_file`) and
`test_aux_outputs_store.c` (`test_raw_verify_and_journal`) cover the
read-backs, `test_zones_http.c` also covers the 503 and 500 responses of `POST /api/zones` and
the shared helper, and `test_backup_import.c` covers the update-settings
route's 503 and 500 responses.
