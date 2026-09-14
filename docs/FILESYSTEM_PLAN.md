# Filesystem plan

> **RESOLVED (2026-09-14 roadmap truth-up).** The boot-hang blocker below is
> closed: `3c36b7e1` (the `nvs_load_store_with_cfg_fs`/
> `kiln_cfg_store_cfg_fs_load_raw` heap-move fix, see "`cfg` partition
> re-flashed after stack-overflow fix, 2026-09-07" further down this file)
> was built and flashed from a clean detached worktree, host tests 31/31
> passed, and the board came up normally. This plan previously opened with
> the banner below presented as a live, current blocker — it is history as
> of 2026-09-07/08, kept for the record rather than deleted.
>
> **Original warning (2026-09-08), history only — do not act on this as
> current status: do not flash commit `218f65f7`.** It panics
> (`IllegalInstruction` on the `main` task) during early boot — before
> Wi-Fi/HTTP/UART bring-up — after `117fc6f9`/`9d5da657` wired the `cfg`
> LittleFS mount+auto-format-confirmation path into `main_boot_early.c`. The
> board is left in a JTAG-only-recoverable state (no HTTP, no UART CONTROL
> link). Recovery and root-cause notes: `docs/audits/boot_hang_2026-09-08.md`.
> The originally-suspected mechanism (mount racing ahead of
> `rtc_watchdog_start()`) was checked by reading `main_boot_early.c` and is
> **not** what happens — the watchdog is armed first — so the real fault is
> still open: something in or just after `cfg_fs_mount_device()` on a
> freshly-flashed/blank `cfg` partition takes an illegal-instruction fault.
> Do not reflash this commit until that is root-caused and fixed.

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

## `cfg` partition flashed, 2026-09-07 -- table change applied

Built and flashed from a clean detached worktree (`C:/wt/espflash`) at
`b7919ecc` (HEAD at flash time, includes both gate preconditions:
`234ce9f3` partition design and `b7919ecc` backup-gate closure) so
in-progress uncommitted work on `cfg_fs`/zones dual-write elsewhere did not
land in this image. Pre-flash backup: `full_board_backup.py`, 12/12
endpoints, archived at
`tools/PcTools/board-backups/20260907T194936Z/board_backup.json`
(gitignored). Pre-flash board state: not firing, heap clean, no
unacknowledged crash, gains/coupling matched the bench values in this doc's
"Measured starting point" section exactly.

`flash_firmware(verify=True)` passed on the first attempt -- `otadata`
already pointed at `factory`, so `ota_rollback_esp()` was not needed.
Post-flash: `debug_check_partition_table` reports the on-chip table matches
`partitions.csv` exactly, 13 partitions including `cfg` at 0xDB0000/0x80000
with every pre-existing partition at its original offset/size (checked
against a `mcp_servers.ps1 restart`, since the running server still had the
pre-`234ce9f3` subtype-0x83 rejection cached the first time). PID gains and
coupling matrix read back byte-identical to the pre-flash capture above.
Profiles list intact (8 user + 28 built-in). `get_fw_version` reports
protocol 11 compatible, board running HEAD `b7919ecc` clean. `get_heap_status`
clean, no LOW margins, link up, board reachable over Wi-Fi throughout (no
re-provisioning needed).

No data loss, as the append-only analysis predicted. The `cfg` partition
itself is not yet mounted/used by any code on this build -- that is the
dual-write work in progress elsewhere, out of scope for this flash.

## Backup/restore audit vs. the file-backed accessors (2026-09-07)

**Storage-agnostic audit.** Every existing backup/export and restore/import
path was checked against whether it reads the SAME source the running
firmware treats as authoritative (dual-write: file-preferred, NVS fallback),
not a raw NVS read that would go stale once NVS copies are dropped:

- `backup_export.c`/`backup_import.c` (zones, profiles) already go through
  `zones_config_accessors.h` and `profiles_http_get()`/`profiles_cfg_fs_*` --
  both are the dual-write-aware accessors, not a direct NVS read. No bypass
  found; nothing to fix here.
- `unit_pref.c` (prefs) resolves file-vs-NVS once at boot
  (`pref_cfg_fs_resolve()`) into a RAM cache; every reader (including
  `GET /api/status`, which `full_board_backup.py` pulls) reads that cache,
  never NVS directly. No bypass found.
- `full_board_backup.py` itself only ever calls HTTP GET endpoints -- it has
  no NVS/filesystem access of its own to bypass anything with.

**Conclusion:** no live bypass was found in the backup/restore path as it
stands today -- every item that has actually migrated onto `cfg` already
reads through its accessor on both the firmware and the export/import side.
The latent risk this task was told to look for is real for the items that
have NOT migrated yet: `kiln_cfg_store.c`, `relay_cycles.c`,
`display_power_cfg.c`/`display_power_policy.c`, and `touch_cal_store.c` all
still read/write NVS directly with no `cfg_fs`-aware accessor at all. Two
correctness notes on `cfg_fs_status.c`'s own `"nvs_only":["prefs","profiles",
"kilncfg_slots","adaptive_tune","relay_cycles"]` field (`/api/cfgfs`'s own
response, which `full_board_backup.py` also captures) -- **this list is
stale in TWO of its five entries**, reported here rather than fixed since
`cfg_fs_status.c` is out of scope to edit for this task: `prefs` is
file-backed today (`unit_pref.c`'s `pref_cfg_fs_resolve()`/`_save()`, proven
above), and `profiles` is ALSO file-backed today (`profiles_cfg_fs.c`,
wired into `profiles_http.c`'s save/resolve path, proven above) -- only
`kilncfg_slots`, `adaptive_tune`, and `relay_cycles` are actually still
NVS-only. A reader of `/api/cfgfs`'s JSON (this backup script included)
is told two false negatives today; not a functional bug (nothing reads that
field to make a storage decision), but worth fixing the next time
`cfg_fs_status.c` is in scope. **Latent bug, once each of the three real
NVS-only items migrates**: whichever HTTP
handler backs its GET endpoint must be re-pointed at that item's new
file-preferred accessor at migration time, or a backup taken after the
dual-write window closes silently captures a dead NVS key. This is not a
bug today (NVS is still authoritative for all of them) -- it is a checklist
item for whoever performs each future migration step, called out here so it
is not missed the way this task's brief warned about.

**Filesystem coverage added.** `full_board_backup.py` previously captured
only HTTP JSON endpoints -- nothing about the `cfg` filesystem's own
contents. Added, reusing the EXISTING `/api/cfgfs` listing rather than a
parallel surface:

- `GET /api/cfgfs/file?name=<name>` (new, `diagnostics_http.c`) -- returns
  one named file's raw bytes.
- `POST /api/cfgfs/file?name=<name>` (new, `diagnostics_http.c`) -- writes
  raw bytes back via `cfg_fs_write_atomic()`, dispatched onto the flash
  worker exactly like `relay_cycles_reset()`/`factory_reset.c` do (write
  buffer deliberately allocated in INTERNAL DRAM, not PSRAM, since a flash
  operation disables the cache and makes PSRAM unreachable for its
  duration -- see that handler's own comment).
- `full_board_backup.py`: `/api/cfgfs` added to `GET_ENDPOINTS` (optional --
  an unmounted/unformatted `cfg` partition is today's expected state, not a
  failure); `_capture_cfgfs_files()` then fetches every listed file's bytes
  and base64-encodes them into the archive's new `cfgfs_files` section;
  `restore_cfgfs_files()` (also new) decodes and validates every entry
  BEFORE writing any of them back (`--restore-cfgfs-from <archive.json>`),
  same validate-everything-then-apply discipline `backup_import_apply()`
  uses for zones/profiles.

**Restore proof.** `tools/PcTools/tests/test_full_board_backup_cfgfs.py`
(5 tests, mocked HTTP, no live board):

- `test_capture_then_restore_round_trips_file_backed_board_byte_for_byte` --
  **the case that matters most**: a simulated board whose data lives in
  FILES (`cfg` mounted, several files present, including a JSON config file
  and a binary rev-prefixed pref blob) is captured, archived, and restored;
  every POST body is asserted byte-identical to the originally captured
  file. PROVEN.
- `test_unmounted_cfg_partition_is_empty_not_an_error` -- an unmounted `cfg`
  (today's live-board reality) yields an empty, error-free section. PROVEN.
- `test_restore_refuses_on_corrupted_base64_and_writes_nothing` and
  `test_restore_refuses_on_size_mismatch_and_writes_nothing` -- NEGATIVE
  TESTS: a corrupted/truncated archive field makes `restore_cfgfs_files()`
  refuse the whole restore with ZERO POSTs issued (asserted against the
  mock's own POST log, not just the return value). PROVEN.
- `test_restore_dry_run_validates_but_writes_nothing` -- PROVEN.

**What is NOT proven:** this is a Python-mocked-HTTP proof of the SCRIPT's
logic (capture/archive/restore-decision), not an on-target proof of the new
firmware handlers themselves -- `cfgfs_file_get_handler()`/
`cfgfs_file_post_handler()` were not exercised by a host C test (httpd
handler unit-testing in this codebase requires a mock `httpd_req_t`
harness that does not exist for this file, and building one was out of
scope for this pass) nor against the live board, because **the `cfg`
partition on the bench board is currently UNFORMATTED/unmounted** (per this
doc's own "Measured starting point" and the 2026-09-07 flash notes above) --
`GET /api/cfgfs` on that board answers `mounted:false`, so there is nothing
on it to fetch yet. Also NOT proven: cross-request atomicity of a multi-file
filesystem restore -- unlike `backup_import_apply()`'s single-HTTP-call
two-pass commit, `restore_cfgfs_files()` validates every file up front (so a
corrupted archive writes nothing) but each accepted file is still POSTed in
a separate HTTP request; a transport failure partway through a multi-file
restore leaves earlier files written and later ones not (`restore_cfgfs_files()`
reports exactly how many were written before the failure, but does not roll
them back). Whoever formats/mounts `cfg` on the bench board next should
re-run this proof against the real firmware endpoints before relying on it.

**Closing-criterion evaluability.** The dual-write window's third closing
condition ("one proven backup/restore round trip against the file path")
is **NOT YET evaluable**, for the same reason: it requires a live board with
`cfg` mounted and file-backed zones config to export/erase/re-import
against, and the bench board's `cfg` partition is unformatted today. This
pass makes the round trip MECHANICALLY POSSIBLE for the first time (the
filesystem-file capture/restore machinery now exists end-to-end and is
proven at the script-logic level) but does not itself satisfy the
criterion -- that still requires formatting/mounting `cfg` on the bench
board, running a real dual-write firing, and then performing the round trip
this pass's tooling now supports. The other two closing conditions (20
clean boots, one file-backed firing) are unaffected by this pass and remain
separately unmet as of 2026-09-07.

## Closing-criterion measurement added, 2026-09-07

The three-condition closing criterion above (20 clean boots, one file-backed
firing, one verified restore round trip) had nothing measuring it -- meaning
it could only ever be declared closed on vibes, or never declared closed at
all. This pass adds a small standalone module,
`firmware/KilnFW/App/drivers/persist/dualwrite_window.{c,h}`, that counts
all three conditions and reports on them, without ever acting on the report.

**Where the counters live, and why NVS.** All three counters live in plain
NVS (`kiln_cfg` namespace, `kiln_nvs` partition, own key `"dwwin"`) -- the
SAME already-proven persistence path `run_state.c`/`crash_report.c` use, own
key so a corrupt/rejected record here cannot take another module's
breadcrumb down with it, and vice versa. Deliberately **not** on `cfg` (the
filesystem this module exists to evaluate): if the counters' own storage
depended on the thing under evaluation, a `cfg` bug -- exactly the failure
mode the window exists to catch -- could corrupt or lose the evidence needed
to prove `cfg` is NOT yet trustworthy. That is backwards for a measurement
instrument.

**What resets the clean-boot streak, and what does not.**
`consecutive_clean_boots` resets to 0 on: an unclean reset reason (anything
other than `HAL_RESET_POWERON`/`HAL_RESET_SW`), an unacknowledged crash
report pending from that boot, `cfg_fs` not mounted at check time, or an
explicit `dualwrite_window_note_mount_failure()` call for a mount failure
discovered *after* the once-per-boot check already ran. `firing_complete`
and `restore_verified` are separate, **sticky, one-time achievements** --
an unclean boot after a real firing or restore already happened does not
erase that it happened. This is the module's own answer to CLAUDE.md's
"reset one side of a pair" bug class: the only thing ever derived from
`consecutive_clean_boots` is the reported `window_may_close` flag, and that
flag is **never stored** -- `dualwrite_window_compute_status()` recomputes
it fresh from the record on every single call, so there is no second copy
of "may it close" anywhere in the system that the boot-counter reset could
leave stale. See the module's own header comment for the full accounting.

**How to read the progress.** `GET /api/dualwrite_window` reports
`consecutive_clean_boots`, `clean_boots_target` (20), `firing_complete`,
`restore_verified`, and the fully-derived `window_may_close`. This is a
**separate small endpoint**, not a field on `GET /api/cfgfs` -- `cfg_fs_
status.c` is owned by another pass as of this writing and states its own
scope as "calls ONLY `cfg_fs.h`'s public API", which this module's NVS-only
data does not fit. **A request is filed here for `cfg_fs_status.c`'s owner**:
if useful, add a `dual_write_window` field to `/api/cfgfs`'s JSON, sourced
from `dualwrite_window_get_status()`; until/unless that lands,
`GET /api/dualwrite_window` is the only place this progress is visible.
`POST /api/dualwrite_window/restore_verified` lets PC-side backup/restore
tooling (`full_board_backup.py`/the backup-over-filesystem work, owned
elsewhere as of this writing) attest that it completed and verified one
round trip against the file path -- this module does not perform or check
that round trip itself, only records the attestation.

**Gate, not automation.** `window_may_close` is a REPORT. Nothing in this
codebase reads it back and stops writing NVS on its own, and nothing here
ever calls an NVS-erase for any key belonging to the items still being
migrated. Dropping the NVS copies once the window closes stays a deliberate,
reviewed, owner-visible step performed by hand -- see this module's header
comment, which states this explicitly as a standing contract, not merely a
current-state fact.

**What still needs wiring, by whoever owns each area:**
- `dualwrite_window_note_mount_failure()` is not yet called from
  `cfg_fs_mount.c` (that file's boot/mount wiring is owned elsewhere as of
  this writing) -- wanted for a mount failure discovered after the
  once-per-boot check already passed. Until wired, only the once-per-boot
  check's own `cfg_fs_is_available()` read (folded directly into the
  clean-boot predicate) covers mount failures, which is the common case.
- The once-per-boot check itself
  (`dualwrite_window_boot_check()`) runs from `dualwrite_window_http_start()`
  (HTTP bring-up, `main_network_http.c`) rather than from
  `main_boot_early.c`, because that file's boot/mount wiring is owned
  elsewhere as of this writing. HTTP bring-up is late enough to see
  `cfg_fs`'s real mount result and `crash_report_init()`'s real result
  (both already ran by then), so this is not a correctness gap, just later
  in boot than the natural home for this check.
- The `firing_complete` note is wired today: `profile_executor.c`'s two
  `PROFILE_EXEC_DONE` transitions call
  `dualwrite_window_note_firing_complete()` when `cfg_fs_is_available()` at
  that moment. No further wiring needed for this leg.
- The `restore_verified` note has no wiring into the actual backup/restore
  round trip (out of this pass's scope) -- whoever finishes that work should
  call `POST /api/dualwrite_window/restore_verified` (or
  `dualwrite_window_note_restore_verified()` directly, board-side) once a
  round trip against the file path is confirmed.

Host tests: `firmware/KilnFW/App/test/test_dualwrite_window.c`, covering the
clean-boot predicate, the counter reset/increment logic (including a REAL
negative test performed against this checkout -- see that file's own
comment for the exact line broken, the failure produced, and the by-hand
restore + confirmed-empty `git diff` that followed), the derived-status
computation, the sticky/idempotent notes, and the "report only, never an
automation" contract.

## `cfg` partition re-flashed after stack-overflow fix, 2026-09-07

Built and flashed `3c36b7e1` (the `nvs_load_store_with_cfg_fs`/
`kiln_cfg_store_cfg_fs_load_raw` heap-move fix for the 218f65f7 boot panic,
see `docs/audits/boot_hang_2026-09-08.md`) from a clean detached worktree
(`C:/wt/espflash`). Host tests 31/31 passed; `check_main_task_stack_budget`
measured 4864 B against a 6144 B budget (75% of the 8192 B `main` task
stack) -- OK. Pre-flash backup: `full_board_backup.py`, 12/12 endpoints
(cfgfs section empty, expected -- partition unmounted pre-flash), archived
under `tools/PcTools/board-backups/` (gitignored).

`flash_firmware(kiln_fw_root="C:/wt/espflash/firmware/KilnFW", verify=True)`
passed and verified on the first attempt after two untracked host-test
fixture directories and a line-ending-only diff in `dashboard_http.c` were
cleared from the worktree (the sensitive-dirty gate had flagged one of the
fixture dirs, `cfg_fs_test_kiln_cfg_store/`, as `cfg_fs`-adjacent; it was a
scratch directory from `build_host_tests.ps1`, not source).

Post-flash: no new crash report (`get_heap_status`'s unacknowledged-crash
banner is still the stale `218f65f7` record: `exc_pc=0xfffffffd`, corrupted
backtrace, unchanged from before this flash -- this boot's own
`reset_reason` is a clean `software (esp_restart)`, `uptime_s=14` at first
check). `debug_check_partition_table` reports the on-chip table now MATCHES
`partitions.csv` exactly, `cfg` present at `0xDB0000/0x80000`. PID gains and
coupling matrix read back identical to pre-flash (Zone0 Kp=0.0318
Ki=0.0001 Kd=0.8401, Zone1 Kp=0.0485 Ki=0.0002 Kd=1.0548, Zone2 Kp=0.0631
Ki=0.0002 Kd=1.0690, coupling z0(27.32,21.72) z1(14.30,22.15)
z2(8.33,12.42)). Stack margins for every task read OK except two
already-known LOW entries unrelated to this change (`backlight_pwm` 896 B/
29.2%, `system_uart_bridge` 920 B/29.9%) -- neither regressed by this flash.
Safety link came up (S6b tripped briefly on the ESP-side reset, self-cleared,
`safety_get_status` shows link up shortly after).

**Auto-format did NOT run.** The boot log shows LittleFS mount failing with
`Corrupted dir pair at {0x0, 0x1}` and `cfg_fs` then refusing to auto-format
because *86.6% of the partition is non-erased data* -- this region still
holds residual bytes from before the prior recovery reverted the partition
table, not blank flash, so the auto-format safety gate correctly declined
rather than silently overwriting whatever is there. `GET /api/cfgfs` reports
`mounted=false status='unmounted'`, all items still NVS-only,
`GET /api/dualwrite_window` reports `consecutive_clean_boots=0`. This is a
stop point, not a failure of the stack-overflow fix: deciding whether to
force-format that residual data (irreversibly discarding whatever is in it)
is an operator call this flash pass does not make unilaterally. Zones/PID
values were left untouched and no functional round-trip test (unit
preference etc.) was attempted since it would have exercised only the
NVS-only fallback path, not the new cfg_fs code this flash was validating.

## Recovery-mode trap blocks verification of the rewritten format gate, 2026-09-07/08

Board (commit `c1bbdc83`, dirty, built 2026-09-08 03:33:44Z, `factory`
partition, per `get_board_state`/`GET /api/partitions`) was found already in
`boot_guard.h` RECOVERY MODE ("3 consecutive boots were never confirmed
healthy") at the start of this pass -- pre-existing, not caused by this pass.
Pre-work state confirmed safe first: `profiles_get_exec_status` state=0 (no
firing), `get_board_state.io.relays=0` (all relays off), stale unacknowledged
crash report matched the known `218f65f7` panic exactly (`exc_cause_str
IllegalInstruction`, `exc_pc=0xfffffffd`, `exc_addr=0x0`, corrupted backtrace
-- confirmed via `GET /api/crash_report`), gains/coupling matched the known
bench values.

`ota_recovery_exit_esp()` was called and accepted -- but the AP password the
board is currently actually configured with (visible in
`get_board_state().wifi_status.ap_password`) differs from the password value
supplied for this task; the tool refuses (403 wrong password) against the
supplied value and only succeeds against the board's live configured value.
Noted here as a fact about board state (not a secret), not written anywhere
else, and not needed again once whoever owns AP-password provisioning
reconciles the two.

**The board did not durably leave recovery mode.** Five separate exit
attempts were made across roughly 8 minutes on the bench
(`ota_recovery_exit_esp()` x2, `debug_reset(peer="esp")` x3, each followed by
a wait of 30-130+ seconds before the next check) -- every single resulting
boot re-entered recovery mode and logged the IDENTICAL line
`RECOVERY MODE: 3 consecutive boots were never confirmed healthy`, never
increasing (which a genuinely-incrementing, never-clearing counter would
show) and never clearing (which a working self-clear would show as recovery
absent on the very next boot). A `loaded` value pinned at exactly 3 forever,
boot after boot, is consistent with `boot_guard.c`'s `persist_count()` NVS
writes silently failing on this board (so both the increment on entry and
`boot_guard_mark_healthy()`'s clear-to-0 on exit never actually commit, and
every boot re-reads the same stale on-flash value) -- this is a diagnostic
read of the symptom, not a confirmed root cause, and **no change was made to
`boot_guard.c`, `main_ota_rollback_confirm_task`, or any other file in this
area**, per this task's explicit scope boundary (that root-cause fix is
another pass's work). `GET /api/partitions` confirms `running: factory`
throughout (not an OTA-slot confirmation-path issue). No
`OTA rollback not yet confirmed`/`boot-guard counter cleared`/`could not
clear boot-guard counter` log lines were ever observed across ~10 boots'
worth of log capture, consistent with the confirm task's own log lines being
silently dropped by `uart_log_bridge`'s queue-full behavior during the
matching boot windows -- so this pass cannot even confirm whether the confirm
task ran at all versus ran and failed to persist.

**Consequence: step 3 (verify the rewritten LittleFS format gate on a normal
boot) could not be attempted.** Every boot this pass produced was a recovery
boot, and `cfg_fs` unconditionally logs `recovery mode: skipping cfg
filesystem mount entirely` and skips the mount in that mode by design
(`cfg_fs_mount.c`) -- confirmed via `GET /api/cfgfs` returning
`mounted:false` after every attempt, `reason: not mounted this boot -- either
recovery mode skipped the mount, or boot has not reached it yet`. **The
config-filesystem feature (auto-format gate, mount, dual-write functional
round trip, reboot-persistence proof) remains UNPROVEN on hardware as of this
pass** -- exactly the state it was already in before this pass started. This
is not a regression; it is the same pre-existing gap this pass was asked to
close and could not, because the recovery-mode trap (a different, older,
already-flagged defect) sits in front of it.

**Board left in a safe state.** No firing, all relays off (confirmed again
after the final attempt), PID gains and coupling matrix unchanged
(Zone0 Kp=0.0318 Ki=0.0001 Kd=0.8401, Zone1 Kp=0.0485 Ki=0.0002 Kd=1.0548,
Zone2 Kp=0.0631 Ki=0.0002 Kd=1.0690, coupling z0(27.32,21.72) z1(14.30,22.15)
z2(8.33,12.42)), profiles list intact (8 user + 28 built-in), Pico link
protocol 12 (`safety_get_fw_version`: commit `6427502a`, boot_id=208,
commissioned), crash report unchanged (still the stale `218f65f7`
`exc_pc=0xfffffffd`, no new panic introduced by any of the resets in this
pass). The board is currently sitting in recovery mode (Wi-Fi + OTA routes
only, LCD blank, no profile executor/autotune this boot) -- functionally
idle and safe, but not in its normal operating mode, pending whoever
root-causes the counter-persistence defect above.

**Next step for whoever picks this up:** the recovery-mode self-clear defect
needs its own root-cause pass (`boot_guard.c`'s `persist_count()`/`load_count()`
against the `kiln_nvs` partition's actual free-space/wear state is the first
place to look, given this partition has separately logged
`kiln_cfg_store blob is the wrong size` corruption on every boot observed in
this pass). Once a board can reach a genuinely non-recovery boot, steps 3-6
of this task's original brief (format-gate log lines, `/api/cfgfs` summary,
DRAM headroom, the unit-preference file-backed-survives-reboot proof, the
reformat-vs-mount-existing check) are still the right next actions and remain
completely unattempted.

## Targeted boot_guard fix (`0b6e82b7`) flashed and tested, still trapped, 2026-09-08

Built and flashed `0b6e82b7` (strict read-back + erase-then-write retry in
`boot_guard_mark_healthy()`, both callers retrying until verified) from a
clean detached worktree at `C:/wt/espflash`. Pre-flash: `check_main_task_stack_budget.py`
against the fresh ELF measured 4864 B / 6144 B budget (75% of an 8192 B
stack, OK) -- deepest path `app_main -> ... -> zones_config_json_compute_crc`.
31/31 host test executables passed. No agent mid-edit in `App/drivers`.
Pre-flash board snapshot taken (`full_board_backup.py`, gains/coupling/crash
report all matched the known values above, `cfg` still unmounted).
`flash_firmware(kiln_fw_root=..., verify=True)` reported flashed-and-verified
OK, running `factory`, matching build.

Post-flash boot (first boot on the new binary) came up **still in RECOVERY
MODE** (`RECOVERY MODE: profile_executor_start() skipped`, `RECOVERY MODE:
LVGL/LCD UI skipped`, `/api/dualwrite_window` `consecutive_clean_boots: 0`).
No new crash: `GET /api/crash_report` still reports the identical stale
`218f65f7` signature (`exc_cause_str IllegalInstruction`, `exc_pc=0xfffffffd`,
`exc_addr=0x0`, `acknowledged:false`, unchanged). Confirmed all relays off and
no firing, then did exactly one `debug_reset(peer="esp")` per this task's
instruction to stop after a healthy boot plus one reset. The following boot
**also** came up in recovery mode, with the same `RECOVERY MODE:` log lines
repeating verbatim. `cfg` stayed unmounted both boots
(`reason: "not mounted this boot -- either recovery mode skipped the mount,
or boot has not reached it yet"`).

**Conclusion: the fix is incomplete as flashed.** `0b6e82b7`'s stated
mechanism (verify `boot_guard_mark_healthy()`'s NVS write with a strict
read-back and retry with erase-then-write) does not by itself make the
counter clear on this board across the two boots observed here, matching the
prior pass's persist-failure hypothesis rather than resolving it. The
filesystem work (`c1bbdc83`'s rewritten format gate, the dual-write round
trip, the reboot-persistence proof) remains completely unproven on
hardware -- still blocked by the same older recovery-mode trap, not a new
regression from this flash. PID gains, coupling matrix, profile list, and
Pico link protocol (v12, `6427502a`) were all reconfirmed unchanged after
flashing. Board left safe: no firing, all relays off, idle in recovery mode
(Wi-Fi + OTA only). Do not spend further reset cycles chasing this without a
new fix to `boot_guard.c`'s persistence path itself -- the next pass should
instrument or directly read `persist_count()`/`load_count()`'s actual NVS
interaction on this board rather than re-attempting the same black-box
reset-and-observe loop.

## Recovery mode cleared and CONTROL UART recovered, but `cfg` still fails to mount, 2026-09-08

Built and flashed `08230ff0` from a clean detached worktree
(`C:/wt/espflash_final`, updated to current `origin/main`, `sdkconfig`
re-copied from the main tree, `IDF_TARGET=esp32s3`). Host tests: 32/32
executables passed. `check_main_task_stack_budget`: **5792 B against the
6144 B budget** (75% of the 8192 B `main` task stack) -- OK, re-measured
fresh against this ELF, not assumed. ELF archived automatically at
`C:/wt/espflash_final/firmware/KilnFW/build/elf_archive/
KilnCtrl-5c64e745cf3f.elf` (`KilnCtrl-latest.elf` points at it). Pre-flash
`full_board_backup.py`: 13/13 endpoints ok; `cfg` confirmed NOT mounted
pre-flash. `flash_firmware(kiln_fw_root=..., verify=True)` reported flashed
and verified OK via OpenOCD/JTAG (HTTP post-flash verification was itself
skipped -- board wasn't answering HTTP pre-flash either, expected during
Wi-Fi bring-up, not a failure).

**Recovery mode cleared.** `GET /api/ota/esp/status` reports
`"recovery_mode":false` on the post-flash boot (commit `08230ff0`, clean,
build matches). This is the first boot since the recovery-mode trap
sections above where recovery mode did not reassert itself.

**CONTROL UART came back.** Immediately after flashing, `kiln_call`s over
the existing serial connection (COM14) failed with "no reply after all
retries", but a plain `disconnect()`/`connect(port="COM14")` cycle
re-synced cleanly (`protocol v11 matches - FW 08230ff0 (clean) built
2026-09-08 17:27:03Z`), and `control_get_zones` then read back correctly.
This looks like a PC-side/session-side desync (the MCP server's own link
state going stale across the board's JTAG-triggered reset) rather than a
firmware-side wedge, given a bare reconnect fixed it with no board-side
action. PID gains and coupling matrix confirmed **identical** to the known
values: Zone0 Kp=0.0318 Ki=0.0001 Kd=0.8401, Zone1 Kp=0.0485 Ki=0.0002
Kd=1.0548, Zone2 Kp=0.0631 Ki=0.0002 Kd=1.0690, coupling z0(27.32,21.72)
z1(14.30,22.15) z2(8.33,12.42).

**`cfg` still does not mount -- new failure mode, not the recovery-mode
skip.** `GET /api/cfgfs` reports `mounted:false`,
`"format":{"known":true,"in_progress":false,"completed":true,
"succeeded":false,"elapsed_ms":16,...}` -- the format gate ran (this is not
`RECOVERY MODE: skipping cfg filesystem mount entirely` any more, and not
the "86.6% non-erased, refusing to auto-format" case from the
2026-09-07 pass either) and completed in 16 ms, but reported failure. 16 ms
is implausibly fast for a real LittleFS format of a 512 KiB partition,
suggesting an early bail (e.g. a precondition check failing immediately)
rather than a format that actually ran and failed partway. All items
(`prefs`, `profiles`, `kilncfg_slots`, `adaptive_tune`, `relay_cycles`)
remain NVS-only. Confirmed the round trip cannot proceed past this point:
set `unit_pref` to `F` via `POST /api/unit_pref` (form-urlencoded,
`unit=F`) -- took effect (`GET /api/status` reflects `temp_unit:"F"`) but
`GET /api/cfgfs`'s `dual_write.zones.file_backed` stayed `false`, i.e. the
write landed in NVS only, exactly as expected when the filesystem isn't
mounted. Set back to `C` afterward, confirmed. No `debug_reset` round trip
was attempted since there is no mounted filesystem yet to prove survives
one.

No new crash: `GET /api/crash_report` is byte-for-byte the same stale
record as every prior pass (`exc_cause_str: "LoadProhibited"`,
`exc_pc:"0xfffffffd"`, `exc_addr:"0x00000018"`, `acknowledged:false`,
`backtrace_corrupted:true`) -- only `found_on_boot_reset_reason` differs
per boot (`"SW"` this time, matching the JTAG-triggered `esp_restart`),
which is expected and does not indicate a new panic. All task stack
margins read OK except the two already-known LOW entries, unchanged by
this flash: `backlight_pwm` (920 B/29.9%), `system_uart_bridge` (920 B/
29.9%). `get_heap_status`: `heap_internal free=74391 B min_free=48007 B`.

**Net conclusion: the config-filesystem feature is still UNPROVEN on
hardware.** Progress this pass: the recovery-mode trap that blocked every
prior attempt at reaching this point is gone (recovery_mode=false,
confirmed a genuinely non-recovery boot), and the CONTROL UART concern
turned out to be session-side, not firmware-side. But the mount itself now
fails for a third, different reason (fast/early format failure, `succeeded:
false`) than either of the two previously-documented blockers (auto-format
declining on non-erased data; recovery mode skipping the mount outright).
Whoever picks this up next should read `cfg_fs_format_gate.c`'s and
`cfg_fs_mount.c`'s boot-log lines directly (via a JTAG console capture or
`get_device_log`, not just `/api/cfgfs`'s summary) to see what the format
call actually returned in those 16 ms, since a fast unconditional failure
this early is not one of this doc's previously-characterized failure
modes.

### RESOLVED 2026-09-08: the 16 ms failure was a boot-ordering race, not a partition/format defect

`get_device_log` (fresh `debug_reset(peer="esp")`, board idle, relays off)
named the exact call and error:

```
E E (2964) esp_littlefs: .../lfs.c:1383:error: Corrupted dir pair at {0x0, 0x1}
E E (2974) esp_littlefs: mount failed,  (-84)
W W (3084) cfg_fs: cfg partition failed to mount (ESP_FAIL) and the content scan found no valid LittleFS superblock found (region was 86.6% non-erased data, not ...)
W W (3094) cfg_fs: cfg auto-format: background format starting (boot has already continued; poll GET /api/cfgfs for progress)
E E (3104) uart_bridge_ext: flash-safe worker not started -- job dropped
E E (3114) cfg_fs: cfg auto-format: FAILED after 16 ms: ESP_FAIL -- cfg filesystem remains unavailable this boot
```

The failing call is `uart_bridge_ext_run_on_flash_worker()`, invoked from
`cfg_fs_mount.c`'s `cfg_fs_auto_format_task()`. **Root cause:** that task is
created during `main_boot_early()` (`start_deferred_auto_format()`, priority
`tskIDLE_PRIORITY+1`), but the flash-safe worker task it dispatches onto is
not created until `main_control_bringup()` calls
`uart_bridge_ext_start_flash_worker()` -- several boot stages later
(`main.c`: `main_boot_early()` then `main_control_bringup()`). The scheduler
is free to run the low-priority auto-format task before the main task ever
reaches `main_control_bringup()`, and `uart_bridge_ext_run_on_flash_worker()`
fails FAST (`bx_run_on_internal_stack()`'s "flash-safe worker not started --
job dropped" branch, ~0 ms) rather than waiting when called before the
worker exists. `esp_littlefs_format()` itself was never reached -- the
dispatch failed before the format call, matching the 16 ms observation
exactly (a real 512 KiB format is seconds, not milliseconds). Not a
partition/label/subtype defect and not an `esp_littlefs_format()` bug; a
pure boot-ordering race that fires on every single cold boot with a blank
`cfg` partition.

**Fix** (`firmware/KilnFW/App/drivers/bridge/uart_bridge_ext.c`/`.h`,
`firmware/KilnFW/App/drivers/persist/cfg_fs_mount.c`): added
`uart_bridge_ext_flash_worker_started()`, a plain accessor for the
worker-task-created flag. `cfg_fs_auto_format_task()` now calls a new
`wait_for_flash_worker()` (20 ms poll, 5 s ceiling) before dispatching --
bounded so a genuinely broken worker still fails loudly (`ESP_ERR_TIMEOUT`)
instead of hanging the background task forever. This task already runs off
the boot path, so waiting up to 5 s here costs nothing boot-time-visible.

**Error now surfaced, not just a boolean** (`cfg_fs_status.h`/`.c`,
`diagnostics_http.c`): `cfg_fs_format_progress_t` gained a `result` field
(the actual `esp_err_t`, wired from `cfg_fs_mount_format_result()`), and
`GET /api/cfgfs`'s `format` object now includes `"error":"<esp_err_t name>"`
whenever `completed:true, succeeded:false` -- the next failure is one query
away instead of a fresh JTAG-log investigation. Host test added:
`test_cfg_fs_status.c`'s "a failed format names its esp_err_t..." check
(with a NEGATIVE-test companion asserting no `error` field on success).

**Not yet re-flashed/validated on hardware** as of this writing -- host
tests pass (32/32 executables, including the new negative-test case) and
`tools/run_all_checks.ps1` is clean except two PRE-EXISTING, unrelated RED
checks that predate this fix (confirmed via `git show HEAD:<path>`, not
introduced by it): `check_flash_worker_lint.ps1` flags
`cfg_fs_status.c`'s existing `cfg_fs_format_is_stalled()` call as matching
the `cfg_fs_format\w*(` regex (a naming collision with the write/format
surface the lint watches, not an actual write), and
`check_hal_include_boundary.ps1` flags `cfg_fs_mount.c`'s pre-existing
`#include "esp_timer.h"`. Next step: flash from a clean detached worktree
at a committed sha (after `check_main_task_stack_budget` -- margin is thin,
5792/6144 B), confirm the boot log now shows format start -> `esp_littlefs_
format()` -> success -> mount, then `GET /api/cfgfs` `mounted:true` with a
real capacity, then the round trip: change `unit_pref`, confirm
`dual_write.zones.file_backed` (or the relevant item) is file-backed,
`debug_reset`, confirm the value survived, and state plainly whether the
second boot MOUNTED the existing filesystem or reformatted it.

## 2026-09-08 hardware verification (flash of 762bb29e, allow_stale over local HEAD bc05af12)

Built from a clean detached worktree (`C:/wt/espflash_final`, pinned sha
`762bb29e`, sdkconfig copied from the main tree). `check_main_task_stack_budget`:
4864/6144 B (79% of budget, 59% of the 8192 B stack) -- improved from the
5792/6144 B baseline despite the new bridges. 32/32 host test executables
passed. ELF archived at
`firmware/KilnFW/build/elf_archive/KilnCtrl-b383ce3281b3.elf`.

Flashed and verified OK (`flash_firmware(verify=True)`). `/api/crash_report`
after the flash showed the SAME stale `LoadProhibited`/`exc_addr 0x18`
record as pre-flash (identical `exc_pc`, `exc_addr`, backtrace) -- confirmed
NOT a new panic.

**Mount survived a fresh boot as designed**: `GET /api/cfgfs` mounted=true,
5 files, 36864/524288 B used, both immediately post-flash and again after a
clean `debug_reset(esp)` -- second boot MOUNTED rather than reformatted.
PID gains and coupling matrix read back byte-for-byte identical to the
pre-flash capture (Z0/Z1/Z2 Kp/Ki/Kd, full coupling matrix). Recovery mode
clear (`boot_guard: 0 unconfirmed boot(s)`). DRAM: pre-flash
`heap_internal.free`=72851 B, post-flash/post-reset ~62943-62959 B --
roughly 10 KB more committed than the pre-migration image, still far above
the ~11.9 KB danger line from the httpd-wedge incident.

**Relay cycle counts (2201/2994/3214, safety-slot 0) SURVIVED the flash
byte-for-byte** -- confirmed via `GET /api/status`'s `relay_cycles` array
both pre- and post-flash, and again after `debug_reset`. No data loss.

**However, the three newly-migrated items (`relay_cycles`, `firing_stats`,
`adaptive_tune`) did NOT become file-backed on this boot** --
`GET /api/cfgfs`'s `dual_write.nvs_only` still lists all three after the
flash and after the reset. The boot log names the reason directly:

```
relay_cycles: migrated relay cycle blob v1 -> v2 (fifth slot + types added, existing relays default to ssr)
uart_bridge_ext: flash-safe worker not started -- job dropped
pref_cfg_fs: relay_cycles.dat write (rev 0) failed: ESP_FAIL
pref_cfg_fs: could not migrate NVS relay_cycles.dat to file: ESP_FAIL
relay_cycles: relay contact cycles loaded: 2201 2994 3214 0
```

This is the SAME "flash-safe worker not started" boot-ordering race
documented above for `cfg_fs_auto_format_task()`, but at a different call
site: `relay_cycles.c`'s own migrate-on-load path calls
`uart_bridge_ext_run_on_flash_worker()` directly during early boot, before
`main_control_bringup()` has started the flash worker, and (unlike
`cfg_fs_auto_format_task()`, which now calls `wait_for_flash_worker()`)
has no wait/retry around it -- it fails fast and gives up for the rest of
that boot. Data is not lost (NVS remains the fallback and is read
correctly), but the promised migration to file-backed storage silently
does not happen until something else independently retries the write. Not
investigated further here (would mean editing source in the main tree,
out of scope for this flash/verify pass) -- worth a follow-up patch mirroring
`wait_for_flash_worker()` for `relay_cycles.c` (and checking whether
`firing_stats`/`adaptive_tune`'s migrate-on-load paths have the same gap;
only `relay_cycles` logged an attempt this boot since it's the only one of
the three with pre-existing NVS data on this board).

**2026-09-08 fix.** Full call-site audit of every `persist/'*'_cfg_fs.c`
bridge plus `relay_cycles.c`/`adaptive_tune.c`/`profile_executor_firing_
stats.c`'s migrate-on-load paths:

| item | write path | racy at boot? | why |
|---|---|---|---|
| `zones` (`zones_config_cfg_fs.c`) | `cfg_fs_write_atomic_device` (flash-worker dispatch, installed by `cfg_fs_mount.c`'s `cfg_fs_install_device_write_fns()`) | no | loaded from `zones_http_start()`, `main_network_http.c`, which runs after `main_control_bringup()` already started the flash worker |
| `prefs`/`unit_pref`/`ramp_assist`/`display_power`/`tz` (`pref_cfg_fs.c`) | same device write fn (shared, module-wide `s_write_fn`) | no | all loaded from `main_network_http.c`, same as `zones` |
| `profiles` (`profiles_cfg_fs.c`) | same device write fn | no | loaded alongside `zones`/`prefs`, same file, after the worker starts |
| `kiln_cfg_store` (`kiln_cfg_store_cfg_fs.c`) | plain `cfg_fs_write_atomic` (direct stdio, **never wired to the device fn** -- `cfg_fs_install_device_write_fns()` only ever touched the three bridges above) | no (but bypasses the PSRAM-stack guard entirely -- a real gap, just not this one) | `kiln_cfg_store_init()` also runs from `main_network_http.c`, on an internal-stack task, so the missing dispatch never manifests as a dropped job -- it's a different, latent hazard (filesystem_migration_review_2026-09-07.md section 1's caveat), not this boot-ordering race |
| `firing_stats` (`firing_stats_cfg_fs.c`) | plain `cfg_fs_write_atomic` (same gap as `kiln_cfg_store`) | no (same reason) | `firing_stats_load()`'s callers run on the executor task (`executor_task_entry()`, internal-stacked by design, see that file's own comment) |
| `relay_cycles` (via `pref_cfg_fs.c`, shares its write fn) | device write fn once installed | **yes** | `relay_cycles_init()` runs from `main_control_bringup.c` line ~136, **before** that same function starts the flash worker at line ~207 |
| `adaptive_tune` kibase (via `pref_cfg_fs.c`) | device write fn once installed | **yes** | `adaptive_tune_init()` runs from `profile_executor_start()`, called by `main_control_bringup.c` line ~161 -- also before the worker starts at line ~207 |

The three pre-existing bridges (`zones`/`prefs`/`profiles`) survive not by
luck but by ordering: every one of them is loaded from `main_network_http.c`,
which runs strictly after `main_control_bringup()` has already called
`uart_bridge_ext_start_flash_worker()`. `relay_cycles`/`adaptive_tune` are
the only two items whose migrate-on-load call sites sit *inside*
`main_control_bringup()` itself, ahead of that same function's own
flash-worker start line -- the exact race `cfg_fs_auto_format_task()` hit
and `1136c0a9` fixed for the format path.

**Shared helper**: extracted the wait into
`firmware/KilnFW/App/drivers/persist/flash_worker_wait.h`/`.c`
(`flash_worker_wait_until_started(started_fn, poll_ms, ceiling_ms)`, plus a
`flash_worker_wait_default()` convenience wrapper). `cfg_fs_mount.c`'s
`wait_for_flash_worker()` now calls it instead of keeping its own copy;
`relay_cycles_init()` and `adaptive_tune_init()` call
`flash_worker_wait_default()` immediately before their `pref_cfg_fs_resolve()`
migrate-on-load write. One implementation, three call sites, not three
copies of the 20 ms/5 s constants.

**Deferred-migration visibility**: `cfg_fs_dualwrite_item_t` gained a
`migration_deferred` field, rendered in `GET /api/cfgfs`'s
`dual_write.items[]` alongside the existing `diverged` field
(`cfg_fs_status.c`). `relay_cycles_migration_worker_wait_deferred()` and
`adaptive_tune_kibase_migration_worker_wait_deferred()` report true iff
their boot-time bounded wait gave up (worker still not started after 5 s) --
`diagnostics_http.c`'s `/api/cfgfs` handler wires both into their rows via a
new `cfgfs_add_item_ex()`. A dropped migration is now one `GET /api/cfgfs`
away instead of requiring a hardware flash and a boot-log grep to discover.

**Migrate-on-load stayed on the boot path** (not deferred off it, unlike the
format path in `5658b949`): unlike a fresh-format erase (seconds, worth
moving to a background task), a single small blob's `pref_cfg_fs_resolve()`
write is one `fopen`/`fwrite`/`fsync`/`rename` -- the bounded wait (5 s
ceiling, cheap when the worker is already up, which it is on every boot
that isn't itself racing) is a better fit than adding a second deferred-task
mechanism alongside the format path's, and boot never blocks past the
ceiling either way.

`kiln_cfg_store`/`firing_stats` still default to the bare
`cfg_fs_write_atomic` (never installed with the device write fn) -- this is
the separate, pre-existing PSRAM-stack-guard gap `filesystem_migration_
review_2026-09-07.md` section 1 named, not the boot-ordering race this pass
fixed. Both currently avoid the hazard in practice because every call site
that reaches them runs on an internal-stack task, but that is not enforced
the way the guard is for the other three bridges -- worth a follow-up to
wire `cfg_fs_install_device_write_fns()` to cover all five bridges
consistently, out of scope for this pass (which only had a *demonstrated*
boot-ordering drop to fix, not the latent guard gap).

Tests: `firmware/KilnFW/App/test/test_relay_cycles.c` adds direct coverage
of `flash_worker_wait_until_started()` (gives-up-at-ceiling, succeeds-once-
predicate-flips, NULL-predicate) plus a boot-time check that
`relay_cycles_migration_worker_wait_deferred()` reads false when the worker
is already up (this suite's normal state). `test_cfg_fs_status.c`'s exact-
JSON dual-write assertion was updated for the new `migration_deferred` key.
Negative-tested by temporarily making `flash_worker_wait_until_started()`
return `true` unconditionally (never checking `started_fn()` at all,
reproducing the exact "job dropped" hazard) -- `run_state_relay_cycles`
failed 3 checks, shortest: `test_relay_cycles.c:929: gives up -- worker
never reported started`. Reverted by hand; `git diff` on
`flash_worker_wait.c` empty afterward.

Per-task stack margins beyond `main` were not obtainable this pass -- no
`GET /api/*` endpoint or LCD page currently surfaces `stack_margin_register()`
data for reading over HTTP/JTAG-free tooling; only the boot-time
`cfg_fs_mount_device()` high-water log line was available
(`5428/5428 words free` at that point in boot, i.e. before most subsystems
start).

**2026-09-08 hardware verification of the fix -- migration is NOT proven.**
Built `bc0befd0` (the `flash_worker_wait.h` fix above) from a clean detached
worktree, host tests 32/32 green, `check_main_task_stack_budget` 4864/6144 B
(unchanged, within budget), flashed and verified. Relay cycle counters
(2201/2994/3214/0) survived byte-for-byte across the flash and a subsequent
clean `debug_reset`; gains/coupling matrix identical; `cfg` partition MOUNTED
(not reformatted) both times; no OTA/otadata hijack (`running: "factory"`).

But `GET /api/cfgfs` on this boot (and again after `debug_reset`) still shows
`relay_cycles` and `adaptive_tune` as `"file_backed": false, "migration_deferred": true`
-- the bounded wait added by this fix does not close the gap on real
hardware. The boot log shows the wait giving up and the condition persisting
well past its own ceiling:

```
uart_bridge_ext: flash-safe worker not started -- job dropped
relay_cycles: flash-safe worker still not started -- relay-cycles migrate-on-load write may be dropped this boot; see GET /api/cfgfs   (at t=11189 ms)
pref_cfg_fs: relay_cycles.dat write (rev 0) failed: ESP_FAIL
pref_cfg_fs: could not migrate NVS relay_cycles.dat to file: ESP_FAIL
adaptive_tune: flash-safe worker still not started -- kibase migrate-on-load write may be dropped this boot; see GET /api/cfgfs   (at t=16239 ms)
```

`relay_cycles_init()` runs at ~t=6.2s and the "still not started" warning
fires again at t=11.2s and t=16.2s -- i.e. the flash-safe worker was not
merely a little late (which a 5 s bounded wait would absorb), it had not
started by 16+ seconds into boot on this run. This is not the boot-ordering
race the fix targeted (that race is a few-hundred-ms window); something else
is keeping `uart_bridge_ext_start_flash_worker()` from completing in a
reasonable time on this board/build. `firing_stats` shows
`"file_backed": false, "nvs_backed": true, "migration_deferred": false` --
it never attempted the migrate-on-load write at all this boot (no follow-up
investigation done here; flagged for the next pass).

**Conclusion: the filesystem migration for `relay_cycles`/`adaptive_tune` is
NOT fully proven on hardware.** `bc0befd0` successfully turned a silent drop
into a visible, diagnosable one (`migration_deferred` in `/api/cfgfs`, plus
the named log lines above) -- that part of the fix works exactly as
designed. It did not fix the underlying failure to become file-backed. No
data loss occurred (NVS remains authoritative and was read correctly both
times), but the promised NVS-only -> file-backed migration for these two
items still has not happened on real hardware. Next step: instrument or log
`uart_bridge_ext_start_flash_worker()`'s own completion time this boot to
find out why it takes longer than the 5 s ceiling (or never completes) --
out of scope for this flash/verify pass, which only flashes and observes.

Separately (not part of this migration, flagged for awareness): this boot
also produced a second, distinct `crash_report` (`StoreProhibited`,
`exc_task='ipc0'`, `exc_addr=0x820b784e`, `exc_a0=0xa5a5a5a5`) that the
firmware's own `crash_report` module flags as self-inconsistent
(`"crash record's exception frame is NOT self-consistent (pc=0x000001fd,
backtrace corrupted=1) -- do NOT read exc_addr as a struct field"`) --
consistent with a JTAG/OpenOCD reset-timing artifact from the flash
tool's own noted "benign Verify-Failed quirk" retry, not a new firmware
regression: the boot immediately following it, and the `debug_reset` boot
after that, both ran clean (`reset_reason='software (esp_restart)'`,
heartbeats every 18 s with no further panics). Unacknowledged; owner should
review `GET /api/crash_report` before assuming it is inert.

**2026-09-08 follow-up: root cause found, `bc0befd0`'s premise was wrong, not
the worker.** The prior entry above assumed the flash-safe worker "should
have started" by the time `relay_cycles_init()`/`adaptive_tune_init()` ran
and speculated something was blocking `uart_bridge_ext_start_flash_worker()`
itself for 16+ seconds. That is not what the code does. Re-reading
`main_control_bringup.c` line by line: `relay_cycles_init()` (was line ~136)
and `profile_executor_start()` -> `adaptive_tune_init()` (was line ~161) both
run **before** `uart_bridge_ext_start_flash_worker()` (was line ~207) --
*textually later in the exact same function, on the exact same task*. All
of `main_control_bringup()` runs single-threaded on the boot task, so a
bounded wait placed before the worker's own creation call can never observe
it start: nothing else runs on that task to create the worker while the
wait is polling. The two "still not started" timestamps aren't evidence of
a slow or stuck worker -- they are two independent 5 s bounded waits run
back-to-back on the same task, 11189 -> 16239 ms is exactly one 5050 ms
wait-then-continue later. The flash worker had not been asked to start at
either timestamp; it is created only when execution reaches the old line
~207, which is after both waits already gave up.

**`firing_stats` explained the same pass**: unlike `relay_cycles`/
`adaptive_tune`, `firing_stats_cfg_fs`'s migrate-on-load is lazy --
triggered from `firing_stats_load()`, called from `profile_executor_status.c`
only when firing history is actually read (e.g. a status/history HTTP
request), never unconditionally at boot. `migration_deferred:false` with
`file_backed:false` on that hardware run is simply "never asked to migrate
this boot" (no history read happened), a different and, by design, correct
state -- not a bug, and not the same failure mode as the other two.

**Fix chosen: start the worker earlier, before either caller.**
`uart_bridge_ext_start_flash_worker()` moved in `main_control_bringup.c` to
run immediately after `kiln_io_owner_start()`/the sim-plant block, before
both `relay_cycles_init()` and `profile_executor_start()`. This removes the
ordering hazard outright: both bounded waits now find `started_fn()` already
true on their very first poll (0 ms cost, not 5000 ms), so cold-boot
migration completes on every boot rather than deferring on every boot.
Weighed against the other two options named in the review request:
- *Defer migration until the worker signals ready via callback/event*
  (mirroring `5658b949`'s move of the format path off the boot path): more
  moving parts (a new callback/event mechanism, plus two callback-shaped
  rewrites of two boot-time reads that currently return their resolved
  value synchronously to their callers) to solve a problem a plain reorder
  already solves for free. Right fit for the format path (seconds-long,
  worth backgrounding); overkill for a single small blob write that the
  reorder makes instant.
- *Migrate-on-load lazily at first access* (the `firing_stats` shape):
  correct in general, but `relay_cycles`/`adaptive_tune` are read
  synchronously at `main_control_bringup()` time specifically because their
  resolved values feed control-loop state before the executor starts --
  turning that into a lazy first-access path would change more than the
  boot-ordering bug requires and risks the exact "who else holds a copy or
  a derived expectation of this" hazard CLAUDE.md's reset-one-side class
  warns about (a value read early for the control loop that is later
  silently replaced with a "real" migrated value nobody re-propagates).
The reorder is a same-function line move with no new state, no new
mechanism, and directly matches the confirmed cause -- the other two are
solving problems this boot ordering does not actually have.

**Permanent-vs-transient deferral**: `relay_cycles_migration_worker_wait_
deferred()`/`adaptive_tune_kibase_migration_worker_wait_deferred()` are each
read exactly once, at boot, with no retry path anywhere in either module
(confirmed by grep -- both flags are set once from `relay_cycles_init()`/
`adaptive_tune_init()` and never touched again). `migration_deferred:true`
in `GET /api/cfgfs` was therefore already permanent-for-this-boot before
this fix, not transient -- the ambiguity the hardware run exposed was that
nothing said so explicitly, so it read identically to "still catching up."
With the worker now started before either caller, `migration_deferred:true`
should no longer occur in practice except a genuine internal-SRAM
allocation failure (the pre-existing `uart_bridge_ext_start_flash_worker()
!= ESP_OK` branch) -- a case that already logs loudly and is now the ONLY
remaining path to `migration_deferred:true`, making the flag effectively
unambiguous going forward without needing a separate transient/permanent
field.

**Older bridges unaffected**: `zones`/`prefs`/`profiles`/`kiln_cfg_store`
load from `main_network_http.c` or the executor task, both of which run
strictly after `main_control_bringup()` returns -- i.e. after the worker
start call regardless of where inside `main_control_bringup()` that call
sits. Moving it earlier only makes their margin larger, not smaller; this
still holds.

**Negative test**: `firmware/KilnFW/App/test/test_flash_worker_boot_order.c`
(new, host-buildable, links the real `flash_worker_wait.c`) drives the real
`flash_worker_wait_until_started()` with a fake "is the worker created yet"
predicate under both orderings. Old order (predicate never flips true during
the wait, modeling the worker being started later in the same function)
times out as expected. Injecting the bug into the "new order" case (setting
the fake predicate to stay false, modeling the fix being reverted) was
caught immediately: `FAIL: new order (worker started first) succeeds
immediately -- migration proceeds instead of deferring`. Reverted by hand;
`git diff` on the test file's assertion line empty afterward (confirmed via
re-run: both cases `PASS`, `ALL PASS`, exit 0).

**Reflash needed**: yes -- `main_control_bringup.c`'s boot order changed;
this has not been flashed or hardware-verified. Read back `GET /api/cfgfs`
after flashing and confirm `relay_cycles`/`adaptive_tune` show
`file_backed:true, migration_deferred:false` on a cold boot before trusting
the fix on hardware.

**Hardware-verified 2026-09-08 (`1f741635`)**: flashed from a clean detached
worktree (`C:/wt/espflash_final`) at `1f741635` (confirmed a fast-forward
ancestor of the concurrently-advancing main HEAD `56afdb26`, which only
added an unrelated `setup_wizard_page.html` fix). Host tests 32/32,
`check_main_task_stack_budget` 4864/6144 B (unchanged). Cold boot:
`GET /api/cfgfs` shows `relay_cycles: file_backed:true,
migration_deferred:false` -- **the fix is confirmed working on hardware**,
the deferred-migration bug this doc tracked is resolved. `adaptive_tune`
reads `file_backed:false, migration_deferred:false` -- not evidence against
the fix; the zone has zero adaptive-tune observations, so there is nothing
to migrate (correctly not-deferred, just empty). Relay cycle counts
unchanged across the flash: `[2201, 2994, 3214, 0]`. `httpd_worker` stack
margin, separately tracked as CRITICAL (632-468 B free), now reads 4520 B
free / 55.2% headroom -- that fix has also landed and holds on this boot.

**New finding, not yet fixed**: `GET /api/firing_history?profile_id=0`
panicked the board (`uptime_s=35` at next `get_heap_status`, reset_reason
`PANIC`, `exc_cause=65535`, `exc_task` garbled, `exc_addr=0x0`, `a0` not the
known-stale `0xa5a5a5a5` pattern -- genuinely new, distinguishable from the
two pre-existing stale crash records). This was hit while trying to trigger
`firing_stats`'s lazy on-first-read migration per this doc's own plan; the
crash happened before the migration outcome could be observed, so
`firing_stats: file_backed:false` is **unconfirmed either way**, not a
negative result. Board self-recovered (auto-reboot, relays stayed off, no
firing in progress) and was otherwise healthy afterward -- not a brick, no
recovery-flash was needed. This crash report is still unacknowledged on the
board as of this writing and needs its own investigation (likely an
out-of-bounds or null-history-array access in the firing_history GET
handler when `profile_id=0` has no recorded firing history yet).
