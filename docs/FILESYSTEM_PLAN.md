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

### Step 1 — add the managed component, host-build only — DONE (`ca5d90c5`)
Added `joltwallet/littlefs: "^1"` to `App/idf_component.yml` (the registry
name is `joltwallet/littlefs`, not `esp_littlefs` as first written above).
`build_kilnfw` ran green (132.2s) and resolved `joltwallet/littlefs 1.22.3`
into `firmware/KilnFW/managed_components/joltwallet__littlefs`, hash-pinned
in `firmware/KilnFW/dependencies.lock`. Nothing in `App/drivers` references
the component yet — it compiles into a static lib (`libjoltwallet__littlefs.a`)
that nothing links against, so this build carries zero new RAM/flash cost at
runtime. No flash to a board, no partition-table change, no mount call, no
`logs`-partition content touched. Fully reversible (revert the yml edit;
`dependencies.lock` regenerates on the next build).
Test: `build_kilnfw` green, confirmed by grepping its log for
`joltwallet/littlefs (1.22.3)` and the new component's build steps. Added
`firmware/KilnFW/App/test/check_littlefs_component_pinned.ps1` (picked up by
`tools/run_all_checks.ps1`'s `check_*.ps1` auto-discovery) asserting
`idf_component.yml` declares the dependency and `dependencies.lock` has a
matching hash-pinned entry, so a future revert or a bad resolve fails fast
without needing a full build. Negative-tested: renamed the yml key to
`joltwallet/littlefs_typo`, confirmed the check failed with
`idf_component.yml does not declare 'joltwallet/littlefs' as a dependency`,
restored the line by hand, re-ran the check green, and confirmed
`git diff -- firmware/KilnFW/App/idf_component.yml` showed only the intended
addition.

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

## Pre-partition-change backup/restore runbook (2026-09-07)

Before any partition-table flash, run in order:

1. `python tools/PcTools/scripts/full_board_backup.py --host <board-ip>` —
   pulls all 12 inventoried HTTP-reachable endpoints (zones config incl. PID/
   coupling, profiles, kiln config slots, adaptive-tune, status/relay
   counters, ramp-assist, display power, safety commissioning mirror) into
   one timestamped archive under `tools/PcTools/board-backups/<ts>/`
   (gitignored — never commit). Exit code 0 and "0 failed" required before
   proceeding.
2. Verify the archive: confirm `endpoints./api/backup/export.zones[i]` shows
   the expected `pid_kp/pid_ki/pid_kd` and `coupling_c1/coupling_c2` for
   every zone against the known bench values before trusting the backup.
3. **Provably restorable today** (host-test-proven byte-equality, fake-KV
   harness, `firmware/KilnFW/App/test/test_backup_import.c ::
   test_export_round_trips_through_import_to_identical_config`): zones
   config (PID gains, FOPDT model, coupling matrix, guards, wiring) and user
   fire profiles — the two items that actually gate the flash, per the
   irreplaceable-data list in this runbook's originating task. Restore path:
   `POST /api/backup/export`'s output back through `backup_import_apply()`
   (`/api/backup/import`).
4. **Captured but NOT provably restorable via existing firmware**: kiln
   config slots, adaptive-tune state, relay cycle counters, ramp-assist,
   display power policy, RP2040 safety commissioning mirror — no
   corresponding import/POST-replay path was exercised or proven in this
   pass. Treat these as read-only diagnostic capture; do not assume a
   restore works for them without separately testing it.
5. Wi-Fi credentials are deliberately NOT captured (no GET endpoint exists
   by design) — re-provision manually via `/wifi` after any restore that
   follows an `otadata` erase / bootloader reflash.
6. After the partition change and any restore, re-verify zone0/1/2
   `pid_kp/pid_ki/pid_kd` and `coupling_c1/coupling_c2` against the bench
   values above via `GET /api/zones` before starting any firing.

## Backup-gate closure, 2026-09-07 -- the 6 uncovered items

Commit `031ededb` proved the backup gate above was NOT met: zones config and
profiles round-trip (host-test-proven, unchanged), but 6 items captured by
`full_board_backup.py` had no proven import path. Resolved item by item:

1. **Kiln config slots** (`/api/kiln_configs`) -- NEEDS a restore path, and
   already HAS one: `POST /api/kiln_configs/save` (plus `/clone`, `/rename`)
   already accept the same fields the GET returns. Restoring a slot is
   replaying its captured `name`/zone data through `save`. No new firmware
   code needed -- this was a documentation gap (the runbook never said the
   existing endpoints ARE the restore path), not a missing-capability gap.
2. **Adaptive-tune state** (`/api/adaptive_tune`) -- does NOT need a byte-
   exact restore. The Ki baseline is re-derived from live firing behavior
   (`adaptive_tune_zone_tick()`/`adaptive_tune_run_end()`); a fresh board
   re-learns it over the next firing or two, same as a factory-new board
   would. The only durable *preference* here is the opt-in enable flag,
   which already has its own restorable POST (`/api/adaptive_tune/enable`).
   Losing the baseline costs one extra firing of slightly-off Ki before it
   re-converges -- not an irreplaceable loss.
3. **Relay cycle counters / relay_life** -- NEEDS a restore path and had
   none: `relay_cycles.c` had a setter for relay *type* and a *reset-to-
   zero*, but nothing to set an arbitrary count back from a backup. Losing
   these on a reflash silently resets every relay's wear accounting to 0,
   which reads as "brand new contact" to the 80%/90% budget-tier warning --
   the real cost is a false sense of remaining contact life, not just lost
   history. **Implemented**: `relay_cycles_restore_all()`
   (`firmware/KilnFW/App/drivers/persist/relay_cycles.c`/`.h`) plus
   `POST /api/relay_cycles/restore` (`firmware/KilnFW/App/drivers/http/diagnostics_http.c`,
   form body `c0=N&c1=N&c2=N&c3=N&c4=N`). Reuses the exact snapshot-then-
   flash-worker-dispatch shape `relay_cycles_reset()` already used (no
   parallel persistence mechanism). Validates every count against a sanity
   ceiling (100,000,000) BEFORE writing anything -- all-or-nothing, so a
   truncated/corrupted archive field cannot land a partial restore.
   Idempotent: restoring the same array twice writes byte-identical blobs.
   Proven by `firmware/KilnFW/App/test/test_relay_cycles.c`'s new
   `test_restore_all_sets_every_count_and_persists`,
   `test_restore_all_is_idempotent`,
   `test_restore_all_refuses_out_of_range_count_and_writes_nothing`
   (NEGATIVE TEST: one field past the ceiling refuses the whole restore, RAM
   counts fully unchanged, no NVS namespace ever created -- proven, not
   asserted) and `test_restore_all_rejects_null_pointer`. All pass:
   `run_state_relay_cycles` executable green, `212/212` host test binaries
   overall unaffected. NOT yet wired into the native `/api/backup/export`
   format -- it is a standalone POST an operator (or a future backup-import
   script) calls with the values `full_board_backup.py` already captured
   under `/api/status`'s `relay_counts`.
4. **Ramp-assist** (`/api/ramp_assist`) -- a preference (single enable flag).
   Regenerable in the sense that losing it just means re-flipping a switch,
   but it already HAS a restore path: `POST /api/ramp_assist` (GET/POST pair
   already existed, `diagnostics_http.c`). No new code needed.
5. **Display power policy** (`/api/settings/display_power`) -- a preference
   (brightness/timeout/keep-on-while-firing/display-on-error). Same
   situation as ramp-assist: `POST /api/settings/display_power` already
   exists and accepts every field the GET returns. No new code needed.
6. **RP2040 safety commissioning mirror** (`/api/safety/commissioning`) --
   **removed from the gate.** This is a *mirror* on the ESP side of data
   whose master copy lives on the RP2040's own 4K CRC'd config store, on a
   physically separate chip with its own flash. An ESP partition-table
   change does not touch the RP2040 at all -- the RP2040 is simply
   unaffected. If the RP2040 itself needed reflashing/re-commissioning that
   would be a SaftyFW concern with its own backup story (out of scope here),
   not a reason to hold the ESP partition flash. Restoring the ESP's read-
   only mirror of RP2040 state would not even be meaningful: the ESP has no
   write path that pushes values INTO the RP2040 config store from this
   mirror (`/api/safety/commissioning`'s POST writes live RP2040 params over
   the safety link during commissioning, which is a different, intentional,
   operator-driven action -- not a backup-restore replay).

### Net result

Of the 6, only #3 (relay counters) needed and received new firmware code.
#1/#4/#5 already had working restore paths that the original runbook simply
never named as such. #2 is honestly re-derivable, not restorable-or-bust.
#6 does not belong in an ESP-partition backup gate at all.

**Wi-Fi credential gap (unchanged, restated for this pass):** there is no
GET endpoint for Wi-Fi credentials (by design -- see `backup_http.c`'s
header comment), so `full_board_backup.py` cannot and does not capture them.
If a partition change moves or erases `wifi_nvs` (this revision's `cfg`
addition does not -- see "Data loss" above), Wi-Fi must be re-provisioned
manually via `/wifi` afterward. This is a known, permanent, by-design gap,
not something this pass could close.

**THE PARTITION FLASH GATE IS NOW MET** for the 6 items this pass covered,
given the above verdicts (2 already restorable via existing endpoints, 1
newly implemented and host-test-proven including a negative test, 1
honestly re-derivable, 1 removed as inapplicable) plus the Wi-Fi caveat
above, which was already a documented, accepted gap before this pass and
remains one now. This does not re-run the live-board round trip in step 5
of the original runbook below -- that still needs to happen against the
actual board before flashing, per that runbook's own step 5.

Headlines: 11 items MOVE, 11 KEEP, 2 undecided. Wi-Fi credentials, boot-guard
state, touch calibration, watchdog/OTA/crash records and the RP2040 config
store all stay in NVS — every one of them is read before any mount, or lives on
the other chip. A new `cfg` LittleFS partition (append-only into the free tail)
mounts with `format_if_mount_failed=false` and is skipped entirely in recovery
mode; a mount failure degrades to firmware defaults with a banner and never
blocks boot.

## Applying the table change — runbook (`cfg` partition, 2026-09-07)

Staged, **not flashed**. The CSV row is in `firmware/KilnFW/partitions.csv`
(`cfg, data, littlefs, 0xDB0000, 0x80000`) with its full sizing rationale in
that file's own `cfg` comment block; `firmware/KilnFW/App/test/check_flash_partition_map.ps1`
pins its offset/size and `firmware/KilnFW/App/test/check_partition_labels_vs_firmware.ps1`
cross-checks every label/subtype in the table against the firmware sources.

### Measured starting point (live board, 2026-09-07, `GET /api/partitions`)

12 partitions, byte-identical to the committed CSV, `running: factory`
(so `otadata` currently selects the factory slot, not an OTA one).
`otadata` 0x200000/8K; `ota_0` 0x210000, `ota_1` 0x510000, `factory` 0x810000,
each 0x300000 (app image 2,065,872 B → 34.3% free); NVS partitions `nvs`
0x9000/24K, `wifi_nvs` 0x187000/24K, `kiln_nvs` 0x18D000/64K, `profiles_nvs`
0x19D000/384K; `coredump` 0xBF0000/1M; `logs` 0xCF0000/768K SPIFFS.
**Free contiguous tail today: 0xDB0000..0x1000000 = 2,424,832 B (2.31 MiB)**
of the 16 MB N16R8 chip. After `cfg` takes 512K the tail is 0xE30000..0x1000000
= 1,900,544 B (1.81 MiB), reserved for raising `logs` retention later.

### HARD GATE — do not flash until both are true

1. **A backup/restore round trip has been *proven*** on this board (the
   separate backup/restore proof track): export, wipe, re-import, and confirm
   the tuned PID gains, FOPDT parameters and the coupling matrix read back
   identical. Those are hours of bench firings; this gate exists for them.
   *No exceptions — an append-only table change is low risk, not zero risk,
   and the bootloader/partition-table write is not itself reversible on a
   board that fails to boot afterwards.*
2. `tools/run_all_checks.ps1` green, including both partition checks above.

### Procedure

1. `GET /api/backup/export` → save to a dated file off-board. Also archive
   `nvs`, `wifi_nvs`, `kiln_nvs`, `profiles_nvs` (checklist item 4 in
   `docs/FLASH_BUDGET.md` section 5) and read out `coredump` with
   `espcoredump.py` if anything is in it.
2. `debug_check_partition_table` / `GET /api/partitions` → record the
   pre-change table.
3. `build_kilnfw` (picks the CSV up automatically: `sdkconfig.defaults`
   already sets `CONFIG_PARTITION_TABLE_CUSTOM=y` and
   `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME="partitions.csv"`).
4. `flash_firmware()` — writes bootloader @0x0, **partition table @0x8000**,
   and app @0x810000 (`factory`). This is what makes the table change take
   effect; the bootloader is reflashed in the same call, which is required
   against any new table.
5. **`otadata`:** `flash_firmware()` never writes it. For *this* revision no
   app partition moves or changes size, so `otadata` stays valid and **must
   not** be erased. But if it points at `ota_0`/`ota_1` (any board that has
   ever OTA'd), the board boots the OLD app against the NEW table and the
   just-flashed `factory` image is not what is running — `flash_firmware(verify=True)`
   fails loudly in that case. Fix with `ota_rollback_esp()` to restore the
   factory boot target, then re-verify. *This board reads `running: factory`
   today, so no rollback is expected.* A future revision that DOES move or
   resize an app partition must erase `otadata` as part of the flash — an
   OTA-selected boot against moved app offsets does not boot.
6. Re-read `GET /api/partitions` and confirm 13 partitions with `cfg` at
   0xDB0000/524288, then `GET /api/zones/config` before any heating (the
   rollback hazard in CLAUDE.md).

### Data loss

**None.** The change is append-only: no partition above the new row moves,
resizes, or changes subtype (diff `partitions.csv` against the previous
commit to confirm — that is checklist item 5 and cannot be verified from a
snapshot). The region `cfg` claims was unallocated, so there is nothing in
it to lose. `nvs`/`wifi_nvs`/`kiln_nvs`/`profiles_nvs`/`logs`/`coredump`
keep their contents. The backups in step 1 are insurance against a failed
flash, not against an expected loss.

### Rollback

Reverting the CSV row and reflashing restores the previous 12-partition
table; nothing else is disturbed, because nothing moved. The only thing lost
is whatever had been written into `cfg` itself. So the *table* change is
reversible. What is **one-way** is the data migration built on top of it —
once NVS copies are dropped (below), going back to a pre-`cfg` firmware
means restoring from a backup export, not from NVS.

### Bounded dual-write window and its closing criterion

Per the owner decision: while user data migrates onto `cfg`, every write goes
to **both** the file and the existing NVS blob, and reads prefer the file and
fall back to NVS. That window closes — NVS copies dropped, writes go to files
only — when **all** of these hold on the bench board:

- **20 consecutive clean boots** on the file-backed path with no mount
  failure, no fallback-to-NVS read, and no defaults banner; and
- **one complete firing** (start → ramp → dwell → cool, a real profile, not a
  bench smoke test) run entirely from file-backed config, with the tuned PID
  gains and coupling matrix read back from `/cfg/zones.json` and confirmed
  byte-identical to the NVS copy afterwards; and
- **one proven backup/restore round trip against the file path** (export,
  erase `cfg`, re-import, verify).

Only then does the step that removes the NVS writers land. Until all three
are met the dual write stays, regardless of elapsed time.
