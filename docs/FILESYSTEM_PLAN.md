# Filesystem plan

Status check before planning new work: ROADMAP.md's only filesystem-shaped
item is `docs/LITTLEFS_ASSESSMENT.md` (2026-09-06), and its answer was **not
adopted** — NVS already wear-levels every KV write, SaftyFW has one 4 K
sector, and the ESP's one filesystem partition (`logs`, SPIFFS) already
solved the one real gap (event/telemetry log storage). Re-verified against
the current tree today rather than trusted from the doc:

- `LOG_STORE_MAX_SEGMENTS` is still `8` (`App/drivers/persist/log_store.h`),
  256 KiB/kind, 512 KiB combined — the assessment's stated revisit trigger
  ("if `logs` retention is raised well past 256 KiB/kind") has **not**
  fired. If anything retention moved the opposite way (1 MiB → 256 KiB,
  2026-09-02).
- Live partition table matches `partitions.csv` exactly
  (`debug_check_partition_table`).
- App partition (`factory`/`ota_0`/`ota_1`) is `0x300000` = 3,145,728 B;
  `KilnCtrl.bin` is 2,065,872 B → **1,079,856 B free, 34.3%** — matches the
  ~34% figure already in CLAUDE.md, not stale.
- Internal DRAM right now: `free=78,371 B`, `min_free=65,171 B` low-water
  since boot (`get_heap_status`) — comfortably above the 11.9 kB level that
  previously caused HTTP socket resets, but internal heap total is only
  331,595 B, so it is not headroom to spend carelessly on new buffers.
- Unused contiguous flash tail: `0xDB0000..0x1000000` (~2.31 MiB, from
  `partitions.csv`'s own arithmetic comments).

**Conclusion: there is no pending filesystem feature to build.** The
assessment's decision still holds under current numbers, and it holds more
strongly than in September (retention went down, not up). This document
does not propose adopting LittleFS project-wide. What follows is the one
concrete, bounded, reversible track that the assessment already flagged as
viable *if the trigger ever fires* — kept staged here so it can start
immediately without re-doing the analysis, not because it is due now.

## The one viable track: `logs` SPIFFS → LittleFS, if retention grows

Scope: swap the filesystem backing the existing `logs` partition only.
Nothing else in the tree is file-shaped (SaftyFW's `config_store` is a
raw CRC'd record, explicitly not a filesystem candidate per the
assessment). No partition-table change is required for this track — same
offset, same size, same data-loss-on-reformat behavior SPIFFS already has.

**Trigger to actually start:** `logs` retention requirement raised past
what 256 KiB/kind covers (~1.9 h of firing telemetry today), OR
`joltwallet/esp_littlefs` becomes available as a managed component without
a network fetch at build time being a problem.

### Step 1 — add the managed component, host-build only
Add `joltwallet/esp_littlefs` to `App/idf_component.yml`. Confirm
`build_kilnfw` still succeeds (this exercises the registry fetch once).
No flash. Fully reversible (revert the yml edit).
Test: `build_kilnfw` green; diff `build/` output shows the new component
pulled, nothing else changed.

### Step 2 — subtype + mount call swap, guarded by a build flag
In `log_store_mount.c`, add `esp_vfs_littlefs_register()` behind a
compile-time flag alongside the existing `esp_vfs_spiffs_register()` call;
default flag keeps SPIFFS. `log_store.c` itself needs no change — it is
plain stdio (`fopen`/`fwrite`/`remove`/`rename`) per the assessment.
`partitions.csv`'s `logs` line subtype changes from `spiffs` to `littlefs`
only when the flag is flipped.
Test: host test (or a stub-backed unit test) exercising `log_store.c`'s
rotation logic against both mount paths compiles under both flag states;
add this as a new host test rather than modifying an existing one, so a
regression here doesn't hide inside a shared test.

### Step 3 — bench flash with the flag on, one board
Requires a flash (`flash_firmware`, `verify=True` as always) and accepts
the documented one-time loss of whatever is already in `logs` (SPIFFS/
LittleFS reformat on mismatch — same accepted cost the 2026-09-02 shrink
already took). No partition-table change, so no OTA/rollback interaction
beyond the ordinary `flash_firmware` path. Confirm via `GET /api/status`
or the diagnostics page that log writes still land and rotate at the
8-segment cap, and watch `get_heap_status` before/after — LittleFS's
lookahead buffer is a small, bounded RAM cost but this board's internal
heap has no spare to lose silently.
Test: manual bench check (log write, rotation, reboot, read-back) +
`get_heap_status` internal-heap delta recorded in the commit.

### Step 4 — flip the default, remove the SPIFFS path
Only after step 3 has run through at least one full firing on the bench
with no regression. Delete the SPIFFS branch and the build flag from
step 2; `idf_component.yml` dependency becomes unconditional.
This step is the only one that is hard to walk back cheaply (removing the
SPIFFS code path), so it stays last and gated on a real firing, not a
bench smoke test.

### What stays untouched, on purpose
- `zones_cfg`, `profile_t`, `kiln_cfg_store`, `wifi_prov`, `ota_record` —
  all NVS, all outside this track. Nothing here touches their versioned
  blobs or migration paths.
- `pico_img`, `coredump`, `otadata`/`ota_0`/`ota_1`/`factory` offsets and
  sizes — unchanged in every step above. No partition-table revision, so
  none of `flash_firmware`'s factory-only-write hazard, the OTA-rollback
  zones-schema hazard, or the flash-worker/PSRAM-stack rules are newly
  triggered by this track (they remain live constraints on the rest of the
  firmware, unrelated to this change).
- SaftyFW `config_store` — explicitly out of scope; the assessment gives
  the reason (single 4 K sector, ARMED-write refusal is a property the
  generic store would weaken).

## Risks

- **Flag-day risk:** none of the above needs to land atomically — steps
  1-2 are host-only and reversible by revert; step 3 is one bench board
  with an accepted, already-precedented log-loss cost; step 4 is the only
  point of no return and is explicitly gated behind a real firing.
- **Zones/config exposure:** zero. This track never touches `kiln_nvs`,
  `profiles_nvs`, or `wifi_nvs`, and changes no partition offsets those
  partitions depend on.
- **Real risk if this were done prematurely:** spending the managed-
  component network-fetch dependency and the RAM/complexity cost of a
  second filesystem implementation for a problem (`logs` capacity) that
  is currently *shrinking*, not growing. That is the actual reason to wait
  for the trigger rather than start now.

## Scope expansion, 2026-09-07 — user data moves to the filesystem

Owner decision: **configs and user-created profiles should live on the
filesystem too, and anything else the user can change at runtime should
probably be there as well.** That is a larger, separate track from the `logs`
swap staged above (which stays exactly as written and is a prerequisite — do
not stand up a second filesystem implementation for it).

The inventory (24 runtime-changeable items), the MOVE/KEEP/UNDECIDED
classification, the JSON-on-LittleFS format with atomic temp-then-rename
writes, the read-through NVS→file migration with dual-write (no flag day), the
seven shippable steps, and the risks live in the companion doc:

**→ `docs/FILESYSTEM_USER_DATA_PLAN.md`**

Headlines: 11 items MOVE, 11 KEEP, 2 undecided. Wi-Fi credentials, boot-guard
state, touch calibration, watchdog/OTA/crash records and the RP2040 config
store all stay in NVS — every one of them is read before any mount, or lives on
the other chip. A new `cfg` LittleFS partition (append-only into the free tail)
mounts with `format_if_mount_failed=false` and is skipped entirely in recovery
mode; a mount failure degrades to firmware defaults with a banner and never
blocks boot.
