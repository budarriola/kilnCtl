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

512 KiB of LittleFS at `0xDB0000` on the ESP32-S3's 16 MB flash, added as
an append-only row in `firmware/KilnFW/partitions.csv` — no existing
partition moved or resized. It mounts with `format_if_mount_failed=false`
(a corrupt or blank `cfg` is reported, never silently reformatted — the
opposite policy from the `logs` partition, deliberately: silently erasing
tuning data is the worst failure mode here). It is skipped entirely in
recovery mode.

**As of 2026-09-07 `cfg_fs_mount_device()` (`main_boot_early.c`) now runs at
boot and auto-formats any partition with no valid LittleFS filesystem in
it** — see "Auto-format and the ask-first path" below. The bench board's
`cfg` partition reads 86.6% non-erased (residual bytes left over from
before the `cfg` partition existed in `partitions.csv`, not a filesystem —
the density-based gate this section originally shipped with wrongly
treated that as evidence of content and refused to format it), so its NEXT
boot after this fix lands will auto-format and mount cleanly, making the
dual-write bridges live for the first time. A board whose `cfg` partition
instead contains real (if
mount-failed) data is NOT touched automatically — see below.

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
  banner, and surfaced two other ways: `GET /api/cfgfs/format_pending`
  (`{"pending":true,"reason":"..."}`) and a banner on the Settings page
  with an explicit confirm button, which POSTs to
  `/api/cfgfs/format_confirm` (authenticated the same way `/api/factory_reset`
  is — the same danger tier, deliberately reusing that lockout budget
  rather than adding a fifth one). Nothing is ever erased without one of
  these two triggers.

Either outcome never blocks boot, never touches any partition other than
`cfg`, and is skipped entirely in recovery mode (same gate as before).

## Reading `/api/cfgfs`

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
- `"dual_write"` — one entry per migrated item, e.g. `"zones":
  {"file_backed": true, "file_rev": N, "nvs_rev": N, "diverged": false}`.
  `file_backed` means this item currently reads from its file rather than
  falling back to NVS. `file_rev`/`nvs_rev` are the write-sequence counters
  the two sides carry to resolve which copy is newer if they disagree.
  `diverged: true` means the two copies had different content at last
  check — the newer revision won and resynced the loser, so this should
  self-clear; a `diverged` flag that stays true across repeated reads
  means something is repeatedly re-diverging, not resolving.
- `"nvs_only"` — items that have not moved to file backing yet. **As of
  `2e88e90a` (2026-09-08) this array is empty** — every item this doc's
  migration table (below) tracks as MOVE (1-9) now has a real cfg-filesystem
  bridge and its own `dual_write.items[]` row; nothing genuinely NVS-only
  remains among the migrated set. `cfgfs_nvs_only_drift_check.py` fails the
  build the moment a new `persist/*_cfg_fs.c` bridge lands without a matching
  `dual_write.items[]` row, so this array should not go stale silently a
  third time (see `2e88e90a`'s commit message for the second time it did).

## If the filesystem fails to mount

Nothing to do at the board — every item falls back to its NVS copy and the
board runs on firmware defaults for anything genuinely file-only (nothing
is file-only yet; see above). A persistent mount failure (as opposed to
"not yet formatted") is reported via `/api/cfgfs`'s `status` field and is
meant to also show a banner in `/api/status` and on the LCD once that
banner is wired in (not done yet, tracked in the plan). No safety decision
is allowed to depend on a file successfully mounting — guard thresholds
still resolve from NVS whenever the file side is unavailable.

## What factory reset does to it

The Settings page's "Factory default (erase everything)" button (scope
`all`) now also erases and reformats `cfg`, unconditionally — its own
confirm dialog IS the explicit operator action the mount-failure contract
calls for, so this path never goes through the ask-first flow above.
"Wi-Fi only"/"kiln config only"/"profiles only" do **not** touch `cfg` — it
holds a mix of kiln-config-shaped and profile-shaped data today, and those
narrower buttons promise to erase only what they say.

## State of the migration, 2026-09-07

Of 24 inventoried runtime-changeable items:

| # | Item | Status | Commit |
|---|---|---|---|
| 1 | Zones config (PID/FOPDT/coupling/guards/wiring/tc_type) | dual-write | `19f74959` |
| 2 | Zone normals | still NVS-only | — |
| 3 | Relay names | still NVS-only (grouped with zones, deliberately deferred) | — |
| 4 | Relay cycle counters | dual-write | `762bb29e` bridge, `2e88e90a` /api/cfgfs reporting |
| 5 | User fire profile slots 0-7 | dual-write | `530dc2f7` |
| 6 | Hidden-builtin profile mask | still NVS-only | — |
| 7 | Firing stats / history | dual-write | `762bb29e` bridge, `2e88e90a` /api/cfgfs reporting |
| 8 | Named kiln config slots | still NVS-only | — |
| 9 | Adaptive-tune Ki baseline (opt-in mask lives in zone config, off this table) | dual-write | `762bb29e` bridge, `2e88e90a` /api/cfgfs reporting |
| 10 | Ramp-assist enable | dual-write | `34927a77` |
| 11 | Unit preference (C/F) | dual-write | `34927a77` |
| 12 | Display power / backlight policy | dual-write | `34927a77` |
| 13 | Touch calibration | deliberately staying in NVS (pre-mount boot read) | — |
| 14 | Time-sync TZ | still NVS-only (no host stub for the owning module yet) | — |
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

11 items moved to dual-write, 11 stay in NVS deliberately, 2 (relay names,
TZ) are still NVS-only pending a follow-up pass, matching the plan's own
count.

## Open items (2026-09-07)

- **Dual-write window.** Every migrated item currently writes both the
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
  auto-formats and mounts. The LCD/`/api/status` banner for the ask-first
  refusal path is still not wired in — only the boot log and the Settings
  page know about it today.
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

  The A/B sector fix (`24090c9a`) is flashed to the bench Pico (`b7af9ebe`,
  2026-09-08: commissioning config read back byte-for-byte across the
  migration, CRC unchanged). The seqlock fix (`b202fe56`/`5671ee03`,
  2026-09-09) is NOT yet flashed — correct in source and host-tested, not
  yet on the board. Full detail:
  `docs/audits/flash_endurance_review_2026-09-07.md` §5, R2.

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
`dual_write_window` field on `/api/cfgfs` has been requested from that
endpoint's owner as a follow-up, not yet added. The reported
`window_may_close` is a **report only** — closing the window (removing the
NVS writers) stays a deliberate, reviewed, owner-visible step performed by
hand once all three conditions read true; nothing in this codebase acts on
that flag automatically.
