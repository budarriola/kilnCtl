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

**As of 2026-09-07 the `cfg` partition exists on the bench board's
partition table but has never been formatted, and nothing in the boot
sequence mounts it yet.** Until both a first-time-format step and the
mount call are wired in, every file operation is a fast no-op and every
item quietly runs NVS-only — same behavior as before this migration
started, not a fallback that needs diagnosing. Don't read "config
filesystem unavailable" on this board as a fault; it is the expected
state right now.

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
- `"nvs_only"` — items that have not moved to file backing yet. **This
  list is not perfectly current as of 2026-09-07** — the prefs items
  (unit pref, ramp assist, display power) and user profiles already moved
  to dual-write in code that landed after this array was last updated, and
  the array itself is out of scope for the pass that would fix it (see
  `docs/audits/filesystem_migration_review_2026-09-07.md` open item 4).
  Treat the state-of-migration table below, not this array, as the source
  of truth for what has actually moved.

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

Not yet specified or implemented as a distinct step — factory reset today
acts on NVS only. Formatting or reformatting `cfg` is meant to be an
explicit, operator-initiated action on the factory-reset page (not an
automatic fallback on mount failure), because the whole point of
`format_if_mount_failed=false` is that a filesystem problem should be
reported and looked at, not erased out from under the operator. Until that
action exists, there is no way to format `cfg` on this board short of a
full-chip erase, which also takes NVS.

## State of the migration, 2026-09-07

Of 24 inventoried runtime-changeable items:

| # | Item | Status | Commit |
|---|---|---|---|
| 1 | Zones config (PID/FOPDT/coupling/guards/wiring/tc_type) | dual-write | `19f74959` |
| 2 | Zone normals | still NVS-only | — |
| 3 | Relay names | still NVS-only (grouped with zones, deliberately deferred) | — |
| 4 | Relay cycle counters | still NVS-only (planned move-last) | — |
| 5 | User fire profile slots 0-7 | dual-write | `530dc2f7` |
| 6 | Hidden-builtin profile mask | still NVS-only | — |
| 7 | Firing stats / history | still NVS-only | — |
| 8 | Named kiln config slots | still NVS-only | — |
| 9 | Adaptive-tune Ki baseline + opt-in mask | still NVS-only | — |
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
- **`cfg` is unformatted on the bench board.** The partition exists in the
  table and was flashed, but nothing has ever formatted it, and no
  first-time-format code path exists. The feature is inert — every write
  falls through to NVS-only — until both the mount call and a format
  action land. This is not a bug in what shipped; it is a known missing
  prerequisite (`docs/audits/filesystem_migration_review_2026-09-07.md`
  §6).
- **RP2040 `config_store` has no A/B sectors — a real, unfixed defect.**
  Found by the endurance review, not by this migration, but it belongs
  here because it is genuine outstanding work, not a footnote: when the
  8-slot round-robin reaches its 8th write, the store erases the *entire*
  4 KiB sector and then programs slot 0. Between those two steps the board
  holds **zero valid copies** of the safety configuration (TC type,
  `abs_max_temp_c`, `max_rate_c_per_min`, CT cal). A power cut or watchdog
  reset in that window boots the RP2040 uncommissioned. Fix is two
  sectors, A/B, writing the new record to the unused one and erasing the
  old one only after the new one's CRC verifies — 15 spare 4 KiB sectors
  already exist for this (`BOOTLOADER_CONFIG_FLASH_SIZE` is 64 KiB against
  a 4 KiB store), so no flash-layout change is needed. Not started.
  Full detail: `docs/audits/flash_endurance_review_2026-09-07.md` §5, R2.
