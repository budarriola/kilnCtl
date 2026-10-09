<!-- Completed 2026-10-09: renamed from FILESYSTEM_USER_DATA_PLAN.md (no pending items remain; step 7 closed 2026-10-05, flash-worker device write fns installed by cfg_fs_mount.c and asserted by cfg_fs_assert_device_write_fns_installed, pre-fire interlock decided 2026-10-06). Still-hardware-gated bench soaks are tracked in docs/CONFIG_FILESYSTEM.md. Design record only; CONFIG_FILESYSTEM.md is authoritative for what shipped. -->

# User data on the filesystem — migration design

> **Design record; `docs/CONFIG_FILESYSTEM.md` is authoritative** for what has
> shipped. Two items below that read as open are closed: the **pre-fire
> interlock** (every "NOT done" mention) was decided 2026-10-06 and is
> recorded at `firmware/KilnFW/App/drivers/http/readiness_http.h`
> (`readiness_cfg_fs_status()`): with `cfg` unmounted every save route refuses
> 503 and the readiness item is NOT_DONE with a format prompt, but it does
> **not** gate a firing (a firing runs from the config already in RAM); and
> **step 7** (closing the dual-write window) closed 2026-10-05 (saves are
> cfg-file-only, NVS is a read-only legacy source).

Companion to `docs/FILESYSTEM.md` (which stages the `logs` SPIFFS→LittleFS
track). **Owner decision, 2026-09-07:** user-editable configuration and
user-created profiles move onto a filesystem. This document designs that; it
does not re-argue the decision. Endurance is *not* the case — the endurance
review (`docs/audits/flash_endurance_review_2026-09-07.md`, `157eac42`) found
current NVS wear fine. The case is architectural: structured, inspectable,
diffable, backup/restore-able user data with one file per thing instead of
opaque `_Static_assert`-pinned C structs in KV blobs.

Design is complete here. **STALE as of 2026-09-14 (roadmap truth-up): "nothing
below is implemented" is no longer true.** Zones config, profiles, and
preferences now dual-write to the `cfg` partition designed below (NVS stays
authoritative and unconditional) — this is implemented in source and
host-tested, not merely designed. **Update, 2026-09-17: "live and proven"
overstated the evidence.** The mount/dual-write code exists and passes host
tests, but as of 2026-09-17 it has never run on the bench board — that board
is still on commit `3b0c82e` (built 2026-09-05, 1057 commits behind HEAD),
and `GET /api/cfgfs` on it answers "no such endpoint". Nothing here has been
observed on hardware yet. `docs/CONFIG_FILESYSTEM.md` is the current,
authoritative status doc for what has actually shipped and what remains
open (including the not-yet-reflashed caveat in its "Open items" section);
treat this document as the design record it was written as, and
`CONFIG_FILESYSTEM.md` as superseding it on implementation status (repo
convention: newest doc wins on conflict).

---

## 1. Inventory — everything the user can change at runtime

Swept from `firmware/KilnFW/App/drivers` (`NVS_KEY*` defines, `hal_kv_open`
call sites, `partitions.csv`) plus `firmware/SaftyFW/src/config_store.h`.
Sizes are the persisted blob, not the in-RAM struct, where they differ.

| # | Item | Where today (partition / ns / key) | Size | Write freq | Versioned? | Migration? | Safety-relevant? |
|---|---|---|---|---|---|---|---|
| 1 | Zones config: PID gains, FOPDT, coupling matrix, guards, wiring, per-zone tc_type | `kiln_nvs` / `kiln_cfg` / `zones_cfg` | ≤896 B (`ZONES_CONFIG_BLOB_MAX_SIZE`) | on every edit / autotune apply | yes, `ZONES_CFG_VERSION` **22** | yes, full chain `zones_config_migrate.c` | **YES** (guard thresholds, limits) |
| 2 | Zone normals | `kiln_nvs` / `kiln_cfg` / `zone_norm_cfg` | small blob | rare | with (1) | with (1) | partly |
| 3 | Relay names | `kiln_nvs` / `kiln_cfg` / `relay_names_cfg` | small blob | rare | with (1) | with (1) | no |
| 4 | Relay cycle counters | `kiln_nvs` / `relay_cyc` | small | ≤1 per 600 s (`RELAY_CYCLES_PERSIST_INTERVAL_S`) + flush | no | no | no (advisory) |
| 5 | User fire profiles | `profiles_nvs` / `kiln_cfg` / per-profile keys + `prof_used` | ≤8 (`PROFILES_MAX_COUNT`), ~KB each | on save | yes, `profile_persisted_t.version` (v1→v2 snapshots) | yes | no |
| 6 | Hidden built-in profiles mask | `profiles_nvs` / `prof_bihid` | u8/blob | rare | no | no | no |
| 7 | Firing stats / history (5 runs per profile) | `profiles_nvs` / `fire_stats` / `fs_<id>` | small blob per profile | once per run end | struct-pinned | none | no |
| 8 | Named kiln configs (slots) | `kiln_nvs` / `kiln_cfg` / `kilncfgs` | 8 slots × ≤896 B opaque | on save/apply | inherits (1)'s blob version | inherits (1) | **YES** (applying rewrites guards) |
| 9 | Adaptive-tune Ki base + opt-in mask | `kiln_nvs` / `adap_tune` / `ki_base`, `en_mask`, `en_migrated` | small | per tune | ad-hoc, has a namespace migration already | partial | partly |
| 10 | Ramp-assist enable | `kiln_nvs` / `kiln_cfg` / `ramp_assist` | u8 | rare | no | no | no |
| 11 | Unit preference (C/F) | `kiln_nvs` / `kiln_cfg` / `unit_pref` | u8 | rare | no | no | no |
| 12 | Display power / backlight policy | `kiln_nvs` / `display_power` | small | rare | no | no | no |
| 13 | Touch calibration | `kiln_nvs` / `affine_v1` | small blob | rare (cal run) | key-encoded (`affine_v1`) | key-rename only | no |
| 14 | Time-sync TZ | `kiln_nvs` / `time_tz` | string | rare | no | no | no |
| 15 | Wi-Fi creds + saved nets + static IP/AP fields | `wifi_nvs` (11 keys, `wifi_prov_nvs.c`) | small | on provision | no | legacy `local_only` read-only | no, but **recovery-critical** |
| 16 | Run-state breadcrumb (resume after reboot) | `kiln_nvs` / `kiln_cfg` / `run_state` | small | during a firing | no | no | **YES** (drives resume) |
| 17 | Boot-guard unconfirmed-boot counter | `kiln_nvs` / `count` (`boot_guard.c`) | u8 | every boot | no | no | **YES** (recovery mode) |
| 18 | Watchdog panic-disable flag | `kiln_nvs` / `panic_dis` | u8 | rare | no | no | **YES** |
| 19 | OTA record | `kiln_nvs` / `ota_record` | small | per OTA | no | no | **YES** (rollback) |
| 20 | Crash report + ack | `kiln_nvs` / `crash_rpt` | small | on panic / ack | no | no | **YES** |
| 21 | Safety commissioning mirror: `ct_installed`, `ct_topology`, `tc_type`, abs-max ceilings | ESP: `kiln_nvs`/`kiln_cfg`/`safetycfg`, `safetyctcal`, `safetyrelay` — **authoritative copy is on the RP2040** | small | at commissioning | kilnlink config-page versioned | link-protocol level | **YES** |
| 22 | RP2040 config store (the real safety config) | SaftyFW `config_store.c`, one 4 K sector, CRC'd record, **different chip** | ≤4 K | at commissioning | record version | in-place | **YES** |
| 23 | Firing/autotune telemetry logs | `logs` SPIFFS (→LittleFS), `/logs` | 256 KiB/kind | ~1 line/5 s | file format | n/a | no |
| 24 | Core dump | `coredump` partition | ≤1 MiB | on panic | ESP-IDF | n/a | diagnostic |

24 items.

---

## 2. Classification

### MOVE to filesystem (11)

| Item | Reason |
|---|---|
| 1 zones config | The whole point: 896 B of struct with a 22-step migration chain is the least inspectable thing on the board. Read *after* mount is fine — it is consumed by the control stack, which recovery mode already skips. |
| 2 zone normals | Travels with (1). |
| 3 relay names | Pure presentation, travels with (1). |
| 5 user profiles | Naturally file-shaped, user-authored, already has an export/import feature. One file per profile ends the `prof_used` bitmap-and-key-suffix scheme. |
| 6 hidden-builtin mask | Belongs in the profiles domain file. |
| 7 firing stats/history | Append-shaped, grows, wants to be readable. |
| 8 named kiln config slots | Eight opaque 896 B blobs → eight files. Biggest readability win after (1). |
| 9 adaptive-tune state | Derived tuning output; already carries a namespace migration scar. |
| 10 ramp assist, 11 unit pref, 14 TZ | Small user preferences; fold into one `ui.json` / `prefs.json` rather than three NVS scalars. |
| 12 display power policy | Same preferences domain. |

### KEEP in NVS (11) — with the reason

| Item | Why it must not move |
|---|---|
| 15 Wi-Fi creds | Provisioning runs before any mount, and Wi-Fi is *the* recovery channel. `partitions.csv`'s own header explains why `wifi_nvs` was split off: a config corruption must never strand the board off the network. A filesystem that fails to mount must not also take the network. |
| 17 boot-guard counter | Read and incremented in `main_boot_early.c:405`, long before `log_store_mount()` (`main_bridges_bringup.c:181`). It decides *whether* the rest of boot runs. Circular dependency if it needs a mount. |
| 18 watchdog panic-disable | Same boot phase. |
| 19 OTA record, 20 crash report | Must be writable from the panic/OTA paths, which cannot assume a healthy VFS. |
| 16 run-state breadcrumb | UNDECIDED-leaning-KEEP, see below. |
| 13 touch calibration | Read by `lvgl_port.c` during display bring-up (~line 773 of early boot), before mount. Without it the touchscreen is unusable — i.e. the operator loses the local recovery UI at exactly the wrong moment. |
| 21 safety mirror (ESP side) | It is a *cache* of (22). Keeping it in NVS keeps the safety path independent of the filesystem. |
| 22 RP2040 config store | Different chip. The ESP has no filesystem there and is not getting one. Out of scope, permanently. |
| 23 logs, 24 coredump | Already on their own partitions; not user config. |

### UNDECIDED (2)

- **16 run-state breadcrumb.** Written during a firing, read at boot to offer
  resume. Boot-phase read argues KEEP; the "user-visible state" framing argues
  MOVE. **Recommendation: KEEP**, and revisit only if resume moves later in
  boot. Cheap to leave alone; it is 30 bytes.
- **4 relay cycle counters.** Not user-*edited*, but user-*visible* and
  monotonic. Moving them makes them readable; keeping them avoids a 10-minute-
  cadence write on the new path before that path has soak time.
  **Recommendation: MOVE, but last** (step 6), after everything else has flown.

### The mount-failure contract (non-negotiable)

This board has been bricked into a recovery loop twice (`e7b8efc`, 2026-08-22).
Therefore:

1. **Mount failure is never fatal and never blocks boot.** `cfg_fs_mount()`
   returns an error, boot continues, exactly like `log_store_mount()` today.
2. **Every MOVE item falls back to firmware defaults, loudly**, with a
   persistent banner in `/api/status` and on the LCD: *"config filesystem
   unavailable — running firmware defaults, do not fire."*
3. **Recovery mode does not mount at all.** `boot_guard_is_recovery_mode()`
   gates the mount call, the same way it gates `profile_executor` /
   `autotune_engine`. Recovery mode must be reachable with the config
   partition physically erased.
4. **`format_if_mount_failed` is FALSE for the config filesystem** — the
   opposite of `logs`. Silently reformatting user tuning data is the worst
   possible failure. A corrupt config FS is reported, not erased; erasing is an
   explicit operator action on the factory-reset page.
5. **No safety decision may depend on a file.** Guard thresholds live in (1)
   which moves — so the pre-fire interlock gains a check: *if the config FS did
   not mount this boot, refuse to start a firing.* Defaults are for surviving,
   not for heating. **Superseded 2026-10-06:** the firing refusal was not
   built; saves refuse 503 instead and a firing runs from RAM (banner above).

---

## 3. Storage format

**Filesystem:** LittleFS on a **new** `cfg` partition — not `logs`. Separate
partition so a log-volume problem cannot reach config, mirroring the
`wifi_nvs`/`kiln_nvs` split rationale already in `partitions.csv`. Size 512 K
out of the 2.31 MiB contiguous tail at `0xDB0000`; append-only table change, no
existing partition moves. `format_if_mount_failed=false` (see above).

**Layout — one file per domain, one file per user-created object:**

```
/cfg/zones.json          (1)(2)(3)  — the zones document
/cfg/prefs.json          (10)(11)(12)(14) — small preferences
/cfg/tune.json           (9) adaptive-tune state
/cfg/relay_cycles.json   (4)  [step 6]
/cfg/profiles/<id>.json  (5) one file per user profile, id 0..7
/cfg/profiles/hidden.json(6)
/cfg/kilncfg/<slot>.json (8) one file per named kiln-config slot
/cfg/stats/<id>.json     (7) firing history, per profile
```

**Text, JSON, not binary.** Inspectability is the entire justification for the
move; a binary file is an NVS blob that costs a partition. The parsing cost is
already paid — `backup_json.c` is a purpose-built reader for exactly this shape
of fixed schema and is the reader to reuse (do **not** add cJSON; that decision
is documented in `backup_json.h` and still holds). Writers keep the existing
`snprintf` hand-build convention.

**Versioning moves into the document.** Every file carries
`{"schema": <int>, ...}` as its first key. `ZONES_CFG_VERSION 22` becomes
`"schema": 22` in `zones.json` and the existing migration chain in
`zones_config_migrate.c` is *retained* — it still runs, it just consumes a
parsed struct instead of a memcpy'd blob. Two properties JSON buys that the
blob does not:

- **Unknown fields are ignored, missing fields take defaults.** Most of the 22
  version bumps were "a field was added." Those become no-ops: a v20 file read
  by v22 firmware simply lacks two keys and gets their defaults. The migration
  chain shrinks to the cases where a field's *meaning* changed.
- **`_Static_assert`-pinned layouts stop being load-bearing.** Keep the asserts
  on the in-RAM struct for the NVS compatibility window (step 5 and earlier),
  then they are free to relax.

**Atomic write — temp-then-rename, one helper, no exceptions.** The RP2040
config store's lack of atomicity was just identified as a real defect; do not
repeat it.

```
cfg_fs_write_atomic(path, bytes, len):
   1. write /cfg/.tmp/<basename>   (fully, fflush + fsync)
   2. fsync
   3. rename(tmp, path)            — LittleFS rename is atomic and
                                     power-loss-safe by design
   4. on any failure: unlink tmp, return error, leave the old file intact
```

At mount, sweep `/cfg/.tmp/` and delete anything found — a leftover temp means
an interrupted write, and the old file is still correct. Readers never see a
partial file, only the old one or the new one. **No A/B slot scheme** — rename
already gives the guarantee and A/B doubles the reasoning surface.

Two mandatory constraints on writers, both already established hazards:

- **Every config-FS write goes through the flash worker**
  (`uart_bridge_ext_run_on_flash_worker()`), for the same reason every NVS
  write does: a flash write from a PSRAM-stacked task asserts every time
  (`project_psram_stack_nvs_panic`). `safety_cfg_store.c` is the pattern to
  copy.
- **Never dispatch to the flash worker from a handler already on it** — that
  deadlocks the board and the host stub cannot see it
  (`project_flash_worker_reentrancy`). The atomic-write helper must document
  that it is already-on-worker-safe or must not be called from one.

---

## 4. Migration, NVS → file

**Read-through, write-forward. No flag day, no one-shot batch converter.**

Per item, the shape is identical:

```
load():
   if file exists and parses      -> use it, done
   else read the NVS blob         -> migrate struct -> serialise ->
                                     cfg_fs_write_atomic() -> use it
   else                           -> firmware defaults (banner if FS is down)

save():
   always write the FILE.
   ALSO write the NVS blob, unchanged, for the whole compatibility window.
```

- **When it runs:** lazily, at each module's existing `_load()` call site. No
  new boot phase, no migration task, no ordering dependency between items.
- **Idempotent:** the file's existence is the "already migrated" flag. Running
  it twice is a no-op. No separate `*_migrated` marker key (item 9's
  `en_migrated` is exactly the scar to avoid repeating).
- **Half-completion:** each item migrates independently and atomically. A
  reboot mid-migration leaves some items on files and some on NVS; the next
  boot finishes the rest. There is no consistent-cutover requirement between
  items because nothing cross-references another item's storage.
- **NVS is never deleted during the compatibility window.** Same discipline as
  the 2026-08-13 partition split ("read old location, write through, do NOT
  delete the old copy").
- **Downgrade:** because `save()` dual-writes, firmware rolled back to a
  pre-filesystem build finds fully-current NVS blobs and works. This directly
  avoids the documented `ota_rollback_esp()` trap where rolling back past a
  `zones_cfg` schema bump silently runs firmware-default PID gains — the trap
  is *worse* if data has moved and NVS was truncated, which is precisely why
  the dual-write window is long and the NVS delete is its own late, explicitly
  gated step.
- **Closing the window (step 7, optional and last):** stop dual-writing only
  after the owner confirms no rollback target older than the FS build is
  wanted. Even then, do not erase the NVS keys — leave them stale and ignored.
  Erasing buys 64 K nobody needs.

**Downgrade hazard that remains:** during the window, an edit made on FS
firmware is written to both, so a rollback sees it. But an edit made on
*rolled-back* firmware writes NVS only; rolling forward again then reads the
*file*, which is now stale, and silently loses that edit. Mitigation: each
file stores `"nvs_mtime"`-equivalent — a monotonic `"rev"` counter also written
into the NVS blob's spare byte; on load, if NVS `rev` > file `rev`, re-import
from NVS. Cheap, and it makes the round trip safe in both directions.

---

## 5. Shippable steps

Each is independently flashable; each is revertible unless noted. Riskiest
last.

| # | Step | Partition change? | Backup/export interaction | Test |
|---|---|---|---|---|
| 0 | (prereq) `logs` LittleFS track steps 1–4 in `FILESYSTEM.md` land first — do not stand up a second filesystem implementation. | no | none | as documented there |
| 1 | Add `cfg` partition (512 K at `0xDB0000`, append-only), `cfg_fs_mount()` with `format_if_mount_failed=false`, recovery-mode gate, mount-failure banner, `/api/cfgfs` status. Mounts and does nothing else. | **YES** — `partitions.csv` already carries the `cfg` row (another pass, 2026-09-07). `cfg_fs_mount_device()` (`App/drivers/persist/cfg_fs_mount.c`, 2026-09-07) registers it (`format_if_mount_failed=false`) and delegates to `cfg_fs_mount_or_skip()` for the recovery-mode gate. `check_partition_labels_vs_firmware.ps1`'s `cfg` declared-but-unused reminder removed accordingly. **AUTO-FORMAT / ASK-FIRST added, 2026-09-07 (owner decision), refined the same day** ("The auto check should be looking to see if it is a valid file system, not just data. If it is just data and not file system then just format it."): on a mount failure, `cfg_fs_mount_device()` scans the raw partition (`cfg_fs_format_gate.c`, pure/host-tested) and structurally validates the LittleFS superblock metadata-block pair — replaying the on-disk tag chain, CRC-32, and version check the pinned `joltwallet/littlefs` component itself uses (`lfs_dir_fetchmatch()`/`lfs_format_()` in `lfs.c`) — rather than a byte-density heuristic. Auto-formats whenever no valid superblock structure is found, however much of the partition reads as non-erased (byte density is reported for context only, never gates); refuses only when a real superblock structure is found (fully valid, or one that decoded far enough to be recognizable but failed CRC/version — a genuinely corrupt filesystem), setting `cfg_fs_mount_format_confirmation_pending()`, surfaced via a loud boot-log banner, `GET /api/cfgfs/format_pending`, and a Settings-page banner with an explicit confirm button (`POST /api/cfgfs/format_confirm`, `cfg_fs_format_http.c`, auth reuses `OTA_HTTP_CONTEXT_FACTORY_RESET`). `factory_reset.c`'s "all" scope also formats `cfg` unconditionally (that button's own confirm dialog IS the explicit operator action). The LCD `/api/status` banner is still **NOT done** — only the boot log and the Settings page know about this today. | none | Host: `cfg_fs_mount_or_skip(recovery_mode, ...)` is the real gate function (`test_cfg_fs.c`); `cfg_fs_format_gate.c`'s valid-filesystem-vs-just-data decision is independently host-tested (`test_cfg_fs_format_gate.c` — a spec-accurate constructed superblock commit for the true-positive/corrupt-CRC cases, a magic-split-across-chunks case, a high-non-erased-density-but-no-filesystem case reproducing the bench board's actual state, and a negative-test drill that mutated the magic-length comparison and confirmed 5 checks fail before the edit was reversed by hand). Bench: **NOT done** — no board has been flashed with this table revision in this pass (otadata/bootloader reflash obligation still outstanding; out of scope here, owned by the partition-table track). On the CURRENT board (`cfg` partition reads 86.6% non-erased residual data, no valid LittleFS structure), the next boot after this lands will now auto-format and mount cleanly — this is the first boot the dual-write bridges actually go live on. |
| 2 | `cfg_fs_write_atomic()` + temp sweep + flash-worker routing. No callers. | no | none | **DONE, 2026-09-07.** `App/drivers/persist/cfg_fs.c`/`.h` (pure, host-testable, mirrors `log_store.c`'s split) provide mount/read/write-atomic/delete/exists/list; `App/drivers/persist/cfg_fs_mount.c`/`.h` are the device-only glue (`esp_vfs_littlefs_register`, `uart_bridge_ext_run_on_flash_worker()` routing — no callers yet). Host test: `test_cfg_fs.c`, obstructs the temp file's location and asserts the old final file survives untouched. **Negative-tested by breaking the production function** (redirected `cfg_fs_write_atomic()`'s `fopen()` from `tmp_path` to `final_path`, bypassing the temp file entirely): `test_cfg_fs.c:231: write_atomic() reports failure when it cannot create its own temp file` went RED, restored by hand, `git diff` empty (new, untracked file — confirmed identical to the pre-break version by re-running the full green suite). |
| 3 | Migrate **prefs** (10,11,12,14) — the lowest-stakes items. Read-through + dual-write + `rev` counter. | no | Backup export/import must read/write through the same accessors, not NVS directly — verify `/api/backup/export` output is byte-identical before/after. | Host round-trip; bench: change unit pref, reboot, power-cut during write. **Items 11 (unit pref), 10 (ramp assist), 12 (display power), 14 (TZ) DONE, 2026-09-07.** Item 3 (relay names) **DONE, 2026-09-07** (see step 5's close-out note, since it travels with zones config administratively but reuses this same generic bridge). |
| 4 | Migrate **profiles** (5,6) and **firing stats** (7). | no | **Highest interaction.** Profile export/import and `backup_import.c` both go through `profiles_http_get()/_save()/_delete()` — keep them as the sole entry points so the storage swap is invisible. Explicitly re-test profile export → factory reset → import. | Host: 8-profile fill, delete, re-save. Bench: export/import round trip; confirm `prof_used` bitmap path is gone, not merely unused. **Item 5 (user profile slots 0..7) DONE, 2026-09-07** — see the note immediately below the table. **Item 7 (firing stats/history) DONE, 2026-09-08** — see "Step 6b (item 7 — firing stats/history)" below. Item 6 (hidden-builtin mask) is **DONE, 2026-10-03** (`2749be53`, `/cfg/profiles/hidden.json`). |
| 5 | Migrate **zones config** (1,2,3) + **kiln config slots** (8) + **adaptive tune** (9). Schema 22 becomes `"schema": 22`; migration chain retained. Add the pre-fire interlock: refuse to start a firing if the config FS did not mount. | no | Backup format version stays as-is; export is regenerated from the same getters. | Host: every version 1..22 fixture file parses to the same struct the blob chain produces — **bind the JSON reader to the C migration chain with vector comparison**, per `project_binding_a_python_mirror_to_c`. Bench: full firing on migrated config, then `ota_rollback_esp()` and confirm the rolled-back build reads the same gains (this is the trap being tested). **Item 1 (the `zones_cfg_t` blob itself: PID gains, FOPDT, coupling matrix, guards, wiring, per-zone tc_type) DONE, 2026-09-07, read-through + dual-write** — see the note immediately below the table. Item 3 (relay names) is **also DONE, 2026-09-07** — a SEPARATE NVS key/blob (`relay_names_cfg_t`, its own `relay_names_save()`), dual-written through the generic `pref_cfg_fs.h` bridge rather than this bespoke module — see "Step 3/5 close-out: relay names + TZ" below the step-3 note. Item 8 (kiln config slots) is **also DONE, 2026-09-07** — see the "Step 5 (item 8 — kiln config slots only)" note below the step-4 note. **Item 9 (adaptive-tune Ki baseline) DONE, 2026-09-08** — see "Step 6c (item 9 — adaptive-tune state)" below; deliberately SIMPLER treatment than this row's own "bespoke bridge" framing (generic `pref_cfg_fs.h`, no per-zone divergence forensics), per the re-derivable-over-one-firing audit finding. Item 2 (zone normals) is **DONE, 2026-10-03** — dual-written to `zone_normals.dat` through `pref_cfg_fs.h`, rev key `znorm_rev`, same bridge as relay names. The pre-fire interlock is **closed differently, 2026-10-06**: saves refuse 503 on an unmounted `cfg` and readiness prompts the format; a firing is deliberately not blocked (`readiness_http.h`). |
| 6 | Migrate **relay cycle counters** (4). | no | counters appear in backup export | Bench soak: confirm the 600 s write cadence lands and survives 24 h. **DONE, 2026-09-08** — see "Step 6a (item 4 — relay cycle counters)" below. Bench soak still outstanding (host-proven, board-absent by construction, same as every other item in this plan so far). |
| 7 | *(Owner-gated, not scheduled)* Stop dual-writing to NVS. **Not cheaply reversible** — this is the point of no return for rollback. | no | none | Requires an explicit owner decision that no older firmware will be booted again. |

**Step 5 (item 1 only), 2026-09-07 — done, host-proven, board-absent by
construction.** `App/drivers/persist/zones_config_cfg_fs.c`/`.h` implement
the read-through/dual-write bridge on top of `cfg_fs.c`, called from
`zones_config_store.c`'s existing `nvs_load()`/`nvs_save()` (no new call
sites elsewhere — every existing caller of those two functions gets the new
behavior for free).

- **File format deviation from this doc's "one file, JSON text" design**:
  the file is NOT hand-written JSON. It is a 4-byte little-endian `rev`
  counter followed by the EXACT SAME versioned binary blob
  `zones_config_json_decode_blob()` already migrates from NVS — i.e. byte 0
  of the blob is still the on-flash `zones_cfg` version, and the file is
  migrated by the SAME chain a stored NVS blob is, with zero new parser
  code. This trades away the "inspectable, diffable" property this doc's
  section 3 argues for (a real JSON-text format is future work, tracked as
  not started), in exchange for reusing the tested 22-version migration
  chain unchanged and untouched for this pass — the file's version tag is
  real and forward-migratable, just not human-readable yet.
- **Read/write policy**: reads prefer the file when it decodes valid;
  fall back to the NVS candidate `zones_config_store.c` already decoded
  otherwise. Writes go FILE FIRST, then NVS (NVS write failure is a hard
  error exactly as before; file write failure is logged and swallowed —
  NVS remains the persistence guarantee every existing caller already
  depends on).
- **Divergence tie-break**: a separate NVS key (`zones_rev`, u32 — not a
  field on `zones_cfg_t`, same reasoning as `NVS_KEY_RELAY_NAMES`'s own
  split) and a rev prefix in the file. When both sides decode valid and
  differ, the higher rev wins and the loser is resynced from the winner;
  logged either way (`ESP_LOGW`) naming both revs.
- **Rollback trap, explicitly checked**: NVS is written on every save
  exactly as before this pass (unconditionally, not gated on the file
  write succeeding), so firmware rolled back past this change reads
  current NVS data — this pass does not widen the existing
  `ota_rollback_esp()` / schema-bump hazard already documented in
  CLAUDE.md.
- **Partition-absent path is the one every board runs today**: `cfg_fs_is_available()`
  false makes every file op a fast no-op; `nvs_load()`/`nvs_save()` behave
  byte-identically to before this pass. Host-tested explicitly
  (`test_zones_config_cfg_fs.c`'s `test_partition_absent_falls_through_to_nvs_only`).
- **A real bug found and fixed by this pass's own tests**:
  `zones_config_store.c`'s `nvs_load_from()` calls
  `zones_config_json_normalize_settings_source_cycles()` AFTER the blob's
  CRC has already been validated — that call can mutate the decoded struct
  (collapsing a stored `settings_source` self/cycle reference) without ever
  re-stamping `crc32`. Harmless for NVS alone (the next `nvs_save()`
  restamps unconditionally before writing), but it meant
  `zones_config_cfg_fs_save()` could write a file whose embedded `crc32` no
  longer matched its own content, making that file fail its OWN next
  decode. Fixed by always recomputing/re-stamping the CRC in
  `zones_config_cfg_fs_save()` itself, on a local copy, immediately before
  writing — never trusting a caller's embedded `crc32` as still current.
- **Tests**: `test_zones_config_cfg_fs.c` (partition-absent fallback, NVS-
  to-file migration-on-read, dual-write stays in sync across repeated
  saves, divergence tie-break in both directions with resync, a real v21
  blob decoded through the file path compared byte-for-byte against the
  same bytes decoded directly through `zones_config_json_decode_blob()`,
  and an interrupted/orphaned-temp-file write leaving the old committed
  config intact). **Negative-tested the dual-write itself**: commented out
  `nvs_save()`'s `hal_kv_set_blob(&h, NVS_KEY_ZONES, ...)` call (production
  code, not a test-local mirror), reran the suite, got 20+ failures
  including this project's own pre-existing round-trip test —
  shortest failing line: `test_zones_http.c:1924: a freshly saved
  current-version config must load back found+valid`. Restored the line by
  hand; `git diff -- firmware/KilnFW/App/drivers/persist/zones_config_store.c`
  confirmed clean of the break afterward. Full suite green again
  (29/29 executables, `tools/run_all_checks.ps1`).
- **Not done in this pass** (as of that pass; zone normals (2) and relay names (3) have since landed, `208de3d4` and `288dc91c`):
  kiln config slots (8); adaptive tune
  (9); the pre-fire interlock (since closed, see banner); the JSON-text file format upgrade noted
  above; no board has this flashed. Correction: the bench board's `cfg`
  partition was actually flashed at `c4b4e65d` (2026-09-07) — it exists in
  the partition table, it is merely unformatted (reads as all-0xFF), which
  behaves the same as "absent" through every mount-failure path until it is
  auto-formatted or explicitly confirmed. "No `cfg` partition on any board
  today" was true only before that commit; do not read it as still true.

**Step 3 (items 11, 10, 12 — unit pref, ramp assist, display power),
2026-09-07 — done, host-proven, board-absent by construction.** Relay names
(3) and TZ (14) were finished in a LATER pass (still 2026-09-07) once their
respective blockers cleared — see "Step 3/5 close-out: relay names + TZ"
below for both.

- **New module**: `App/drivers/persist/pref_cfg_fs.c`/`.h` — a GENERIC
  read-through/dual-write bridge (path + item size + validator function,
  parameterized) rather than three hand-copies of
  `zones_config_cfg_fs.c`'s policy. Same file format
  (`<4-byte LE rev><raw item bytes>`), same read-through policy, same
  higher-rev-wins divergence tie-break, same partition-absent/mount-failed
  degrade-to-NVS-only behavior as that module — see `pref_cfg_fs.h`'s header
  comment for why one generic module was chosen over three copies (the
  "reset one side of a pair" bug class this codebase already tracks).
- **Validation**: each item's file bytes are validated by the SAME function
  its NVS load has always used (`unit_pref_validate()`/
  `ramp_assist_validate()`/`display_power_validate()`, extracted from each
  module's existing range checks, not new logic) — no bounds relaxed by the
  move. None of the three has a "0 means use default" sentinel to protect
  (unlike `progress_band_c`, the trap this task's brief calls out) — checked
  explicitly per item: `unit_pref_t`'s 0 IS Celsius (a real value, not a
  sentinel), `ramp_assist`'s 0 IS disabled, and `display_power`'s fields are
  each range-checked independently with no "0 = default" convention.
- **Read/write policy**: reads prefer the file when it decodes valid,
  falling back to the NVS candidate otherwise (and opportunistically
  migrating a valid NVS value out to the file). Writes go FILE FIRST
  (best-effort, a failure is logged and swallowed), THEN NVS (authoritative,
  a failure is returned to the caller) — identical ordering and rationale to
  `zones_config_cfg_fs.c`'s step-5 note.
- **Divergence tie-break**: a separate NVS key per item (`u_pref_rev`,
  `ramp_a_rev`, `disp_pow_rev` — all `NVS_KEY_LEN_CHECK`'d, all under the
  15-character limit) plus a rev prefix in the file. Higher rev wins when
  both sides decode valid and differ; logged (`ESP_LOGW`) naming the path and
  both revs either way, and the losing side is resynced from the winner.
- **Partition-absent / mount-failed**: `cfg_fs_is_available()` false (every
  board today, and the explicit `cfg_fs_init()`-against-a-nonexistent-path
  case) makes every `pref_cfg_fs_*` call a no-op deferring entirely to the
  NVS candidate — every pre-existing test in `test_ramp_assist_cfg.c`/
  `test_display_power_cfg.c` already ran this path unmodified and still
  passes, which is the partition-absent proof; a new explicit
  mount-failed test was added per item too.
- **`/api/cfgfs` surface (requirement 4)**: `cfg_fs_status.c` is
  off-limits/owned in this task's brief, so its hardcoded
  `"nvs_only":["prefs","profiles","kilncfg_slots","adaptive_tune","relay_cycles"]`
  array was left untouched rather than edited. **Flagging here instead**:
  once this lands, `"prefs"` should move out of `nvs_only` into a new
  `dual_write` entry (mirroring the existing `dual_write.zones` shape:
  `file_backed`/`file_rev`/`nvs_rev`/`diverged`, one per migrated pref item
  or one aggregate) — that edit needs a new field on
  `cfg_fs_zones_dualwrite_info_t`-equivalent input and a caller change in
  whatever builds it for the HTTP handler, neither of which this pass
  touched.
- **Tests**: `test_unit_pref.c` (NEW — no host test existed for `unit_pref.c`
  before this pass, confirmed absent from `build_host_tests.ps1`'s
  `$sources`; added both the module's pre-existing default/round-trip/
  corrupt-value coverage it never had, and the new dual-write suite) plus
  extensions to `test_ramp_assist_cfg.c`/`test_display_power_cfg.c`: dual-
  write-lands-on-both-sides, NVS-fallback-then-migrates, divergence tie-
  break (both directions on `unit_pref`/`ramp_assist`), mount-failed, and
  (ramp_assist only) an interrupted-write-leaves-old-file-intact case using
  an always-fails write-fn injection. Full suite: 6782/6782 checks in the
  main host-test executable, `tools/run_all_checks.ps1` 64/64 green.
- **Negative-tested the dual-write itself** (ramp_assist, chosen as the
  representative item): commented out `ramp_assist_cfg_set_enabled()`'s
  `hal_kv_set_u8(&h, NVS_KEY_RAMP_ASSIST, raw)` call (production code, not a
  test-local mirror) by replacing it with `err = HAL_OK;`. Reran the suite:
  5 tests went RED, shortest failing line:
  `test_ramp_assist_cfg.c:113: ramp_assist_cfg_start() on the next boot
  reloads enabled=true from NVS` (a PRE-EXISTING test, not one added by this
  pass) — proving the break was caught by more than just the new coverage.
  Restored the line by hand; `git diff -- firmware/KilnFW/App/drivers/control/ramp_assist_cfg.c`
  showed only the intended dual-write addition (the `hal_kv_set_u8` call
  present, plus the new `hal_kv_set_u32` rev-write line), confirmed clean of
  the break. Full suite green again afterward.
- **Not done in this pass**: the `/api/cfgfs` `dual_write` surface for these
  items (flagged, not implemented); no board has this flashed. Correction:
  `cfg_fs_mount_device()` IS now wired into the boot sequence (`117fc6f9`),
  and the bench board's `cfg` partition IS flashed (`c4b4e65d`) — it is
  merely unformatted (all-0xFF), same as step 1's "no `cfg` partition on any
  board today" note, which is stale for the same reason (see that note's own
  correction above). **Superseded 2026-09-21:** the bench board now has
  `cfg` mounted and populated with 7 files, confirmed via `GET /api/cfgfs`
  on hardware.

**Step 3/5 close-out: relay names (item 3) + TZ (item 14), 2026-09-07 —
done, host-proven, board-absent by construction.** Both items' original
blockers (relay names was administratively tied to the zones-config-move
pass; TZ had no `esp_netif_sntp.h` host stub) cleared, and both are finished
here rather than left pending.

- **Relay names (item 3)**: `zones_config_store.c`'s `relay_names_load()`/
  `relay_names_save()` now dual-write through the SAME generic
  `pref_cfg_fs.h` bridge unit_pref/ramp_assist/display_power use — NOT a
  fourth bespoke module, and NOT `zones_config_cfg_fs.c`'s bespoke chain
  either. `relay_names_cfg_t` (1 + 4×16 + 4 = 69 bytes, padded to ~72) is a
  small fixed-size struct with zero migration history (`RELAY_NAMES_CFG_VERSION`
  has only ever been `1`) — exactly the shape the generic bridge targets, not
  the 22-version chain that justified giving zones config its own module.
  `PREF_CFG_FS_MAX_ITEM` was raised from 32 to 128 bytes (previously sized
  for `display_power_cfg_blob_t`'s 5 bytes) to fit it, rather than writing a
  new bridge. File: `relay_names.dat`; rev key: `relnames_rev` (a separate
  NVS key, same reasoning `zones_rev`/`u_pref_rev` already use — a rev
  counter is not part of the value). Validator (`relay_names_validate()`)
  re-runs the EXACT version+CRC check the NVS path has always applied.
  Read/write policy, tie-break, and partition-absent/mount-failed behavior
  are identical to every other `pref_cfg_fs` item (see that section's own
  bullets above) — no bounds relaxed, no new sentinel invented.
- **TZ (item 14)**: `time_sync.c`'s blocker was the missing
  `esp_netif_sntp.h` host stub, not the storage design — a NEW,
  purpose-scoped stub (`test/stubs/esp_netif_sntp.h`, only the handful of
  symbols `time_sync.c` actually calls: `esp_sntp_config_t`,
  `ESP_NETIF_SNTP_DEFAULT_CONFIG()`, `esp_netif_sntp_init()`,
  `esp_netif_sntp_start()`) closes it, following the same convention as this
  directory's other inert stand-ins (`esp_netif.h`, `esp_wifi.h`). `time_sync.c`
  also needed an MSVC compat shim (`setenv`/`tzset` don't exist under those
  names on MSVC; `_putenv_s`/`_tzset` do) — guarded by `#ifdef _MSC_VER`,
  compiles out entirely on the real ESP-IDF/newlib toolchain. TZ then reuses
  the SAME generic `pref_cfg_fs.h` bridge too: the string is written into a
  fixed `TZ_ITEM_SIZE` (`TIME_SYNC_TZ_MAX_LEN + 1` = 64 bytes) buffer,
  NUL-padded, so the bridge's fixed-item_size contract needs no
  variable-length special case. Validator (`tz_file_validate()`) requires a
  NUL terminator within the buffer and re-runs `time_sync_tz_is_valid()` —
  the identical POSIX-TZ-grammar check `time_sync_start()`'s NVS path has
  always applied (Finding 2's "America/Chicago" rejection included). File:
  `tz.dat`; rev key: `tz_rev`.
- **Test-time-only defect fixed as part of this close-out**: `pref_cfg_fs.c`'s
  divergence tie-break used `file_rev >= nvs_rev` (found by
  `docs/audits/filesystem_migration_review_2026-09-07.md`, grace-listed in
  `check_cfg_fs_tie_break.ps1` as owned by this very pass) — fixed to the
  STRICT `file_rev > nvs_rev` `zones_config_cfg_fs.c` already used, and the
  grace-list entry deleted (the check now enforces the invariant on this
  file unconditionally, same as every other bridge module). This one-line
  fix affects every `pref_cfg_fs` item (unit pref, ramp assist, display
  power, relay names, TZ), not just the two items landed in this pass.
- **Tests**: `test_relay_names_cfg_fs.c` (NEW, own TU linked into the
  `zones_http` host-test executable alongside `test_zones_config_cfg_fs.c`)
  — partition-absent, NVS-fallback-then-migrate, repeated-save sync,
  divergence tie-break both directions, equal-rev-adopts-NVS (the rollback
  round trip), interrupted-write-leaves-old-file-intact, mount-failed.
  `test_time_sync.c` extended the same way (now its OWN executable —
  `time_sync.c` defines the REAL `time_sync_notify_got_ip()`, which collides
  at link time with `test_wifi_prov.c`'s fake of that name, so it could not
  stay in the "main" executable it used to share) with the identical
  coverage shape plus a refused-invalid-TZ case. `tools/run_all_checks.ps1`:
  60/64 checks pass; the 4 failures are pre-existing/concurrent-session
  issues in the `/api/cfgfs` format-policy area explicitly out of scope for
  this task (`check_source_path_drift.ps1`'s stale `flash_worker_lint.py`
  allowlist entry, `check_c_files_in_cmakelists.ps1`'s
  `kiln_cfg_store_cfg_fs.c`, `check_test_has_assertions.ps1`'s
  `test_dualwrite_window.c` vacuous test, `check_uri_handler_cap.ps1`'s
  `cfg_fs_format_http.c`-driven route-count bump) — `check_cfg_fs_tie_break.ps1`
  itself is green.
- **Negative-tested the tie-break fix itself, end to end**: reverted
  `pref_cfg_fs.c`'s `if (file_rev > nvs_rev)` back to `if (file_rev >= nvs_rev)`
  (production code). Reran the suite: went RED, shortest failing line
  `test_relay_names_cfg_fs.c:280: NVS wins the EQUAL-rev tie -- the edit made
  on rolled-back firmware is NOT discarded in favour of the stale file`
  (also broke `test_ramp_assist_cfg.c`'s equal-rev case, proving the shared
  bridge really is shared). Restored the line by hand; `git diff -- firmware/KilnFW/App/drivers/persist/pref_cfg_fs.c`
  confirmed clean of the break afterward (only the intended fix/comment
  changes remained). Full suite green again.
- **Not done in this pass**: the `/api/cfgfs` `dual_write` surface for
  relay names/TZ (same flag as the step-3 note above); no board has this
  flashed. Correction: same as the step-3 note above -- the mount IS wired
  (`117fc6f9`) and the bench board's `cfg` partition IS flashed
  (`c4b4e65d`), merely unformatted.

**Step 4 (item 5 — user profile slots 0..7 only), 2026-09-07 — done
(`530dc2f7`), host-proven, board-absent by construction.** Item 6 (hidden-builtin mask)
is DONE as of 2026-10-03 (`2749be53`, `/cfg/profiles/hidden.json`). Item 7 (firing stats) was NOT done at
this point in the plan's history but is DONE as of Step 6b below
(`762bb29e` bridge, `2e88e90a` /api/cfgfs reporting).

- **File layout**: one file per slot, `profiles/prof<id>.json` (matches this
  doc's section 3 path shape), still **slot-addressed by numeric id 0..7**,
  not renamed to a user-chosen filename — the existing API surface
  (requirement 2) never exposed a filename, only a slot id, so nothing about
  the on-disk naming needed to change for callers to keep working unchanged.
  Same file-format deviation as `zones_config_cfg_fs.c`'s step-5 note: the
  file is a 4-byte little-endian `rev` counter followed by the EXACT SAME
  version-1/2/3 on-flash wrapper bytes `profiles_http.c`'s
  `decode_profile_blob()` already migrates from NVS (widened non-static as
  `profile_decode_blob()`, declared in `profiles_http_internal.h` alongside
  the new `profile_encode_current_blob()` encoder) — one decoder for both
  backends, not two parsers to keep in sync across three historical
  versions.
- **New module**: `App/drivers/persist/profiles_cfg_fs.c`/`.h` — per-slot
  (not a single document, since profiles are individually creatable/
  deletable), otherwise the same shape as `zones_config_cfg_fs.c`: injectable
  write/delete functions (defaulting to `cfg_fs_write_atomic`/`cfg_fs_delete`,
  host-safe no-ops when unmounted), same higher-STRICTLY-rev-wins divergence
  tie-break (`file_rev > nvs_rev`, not `>=` — see
  `check_cfg_fs_tie_break.ps1` and `docs/audits/filesystem_migration_review_2026-09-07.md`,
  which found the `>=` defect in `zones_config_cfg_fs.c` first; this module
  was written with the corrected comparison from the start and passes that
  check).
- **API surface unchanged (requirement 2)**: `profiles_http_get()`/
  `_save()`/`_delete()` — the only entry points `profile_executor.c`,
  `uart_bridge_ext.c`'s UART CONTROL bridge, and `profiles_export_http.c`'s
  export/import handlers call — kept their exact signatures and behavior.
  The dual-write/read-through logic lives entirely inside
  `nvs_save_slot()`/`nvs_erase_slot()`/`nvs_load_all_from()`
  (`profiles_http.c`), none of which are part of the public API. The builtin
  profile table (`profiles_builtin.c`, `.rodata`, read-only) is untouched —
  it was never NVS- or file-backed to begin with.
- **Export/import unaffected, proof**: `profiles_export_http.c`'s
  `export_get_handler()`/`import_post_handler()` call only
  `profiles_http_get()`/`profiles_http_save()` — no direct NVS or file
  access. Since this pass changed nothing about those two functions'
  signatures or externally-observable behavior (same validation, same
  warnings, same "portable across kilns" ceiling handling), the export JSON
  format is provably unaffected: `test_profile_export_import.c` (unmodified
  by this pass) still passes 16/16 unchanged, confirming the export/import
  round trip is still byte-identical.
- **Read/write policy**: reads prefer the file when it decodes valid,
  falling back to the NVS candidate otherwise (and opportunistically
  migrating a valid NVS profile out to the file, one slot at a time, no
  separate migration task or marker key — the file's existence IS
  "already migrated," same as every other module in this plan). Writes go
  FILE FIRST (best-effort — `profiles_cfg_fs_save()`/`_delete()` log and
  swallow a failure), THEN NVS (authoritative — a write failure there is
  still surfaced as `ESP_LOGE`, same as before this pass; the profile stays
  applied live in RAM either way, matching `profiles_http_save()`'s
  pre-existing "applied live but will not survive a reboot" convention,
  since the public API's return value could not change per requirement 2).
- **Rev counter also covers DELETE, not just save** — the one place this
  module's design differs from `zones_config_cfg_fs.c` (which never deletes
  its single document): a new persisted `prof_rev` NVS blob
  (`uint32_t[PROFILES_MAX_COUNT]`) is bumped on BOTH `nvs_save_slot()` and
  `nvs_erase_slot()`. This is what lets `profiles_cfg_fs_resolve()` tell "the
  file is ahead because a save's NVS write failed" apart from "this slot was
  legitimately deleted after its file was written" when NVS shows a slot
  unused but a file still exists for it — without the delete-side rev bump,
  a stale file left behind by an interrupted delete would be silently
  resurrected on the next boot. See `profiles_cfg_fs.h`'s header comment for
  the full four-way resolve table (file-only, NVS-only, both-agree,
  diverged).
- **Validate on load exactly as NVS does (requirement 4)**: guaranteed by
  construction, not by parallel logic — both paths call the same
  `profile_decode_blob()`, so there is no sentinel-value question specific to
  the file path (profiles have no "0 means default" field; every bound is an
  explicit range check in `profiles_http_save()`, unaffected by this pass).
- **Partition-absent / mount-failed**: `cfg_fs_is_available()` false (every
  board today) makes every `profiles_cfg_fs_*` call a no-op deferring
  entirely to the NVS candidate — `nvs_save_slot()`/`nvs_erase_slot()`/
  `nvs_load_all_from()` behave byte-identically to before this pass, proven
  by dedicated host tests (`test_pcfg_partition_absent_behaves_exactly_like_before`,
  `test_pcfg_mount_failed_behaves_like_absent`).
- **`/api/cfgfs` surface**: `cfg_fs_status.c` is off-limits/owned elsewhere
  in this task's brief, so its hardcoded `nvs_only` array (which lists
  `"profiles"`) was left untouched — same call this task's brief asked for:
  flagged here instead of edited. Once `cfg_fs_status.c` grows a
  `dual_write` entry for prefs (per step 3's identical flag), `"profiles"`
  should move there too, one entry per slot or an aggregate
  file-backed-count.
- **Flash-worker / reentrancy (requirement 6)**: no on-device wiring installs
  a flash-worker-routed write/delete function for this module yet — this
  matches `zones_config_cfg_fs.c`'s own CURRENT state exactly (grep-confirmed:
  nothing in `main*.c` calls `zones_config_cfg_fs_set_write_fn()` either), so
  writes today go through the default `cfg_fs_write_atomic()`/`cfg_fs_delete()`
  (host-safe no-ops while `cfg` is unmounted, which is every board today).
  Both `nvs_save_slot()`/`nvs_erase_slot()` already run only on the httpd
  worker task (an internal-SRAM stack, per their existing
  `caller_stack_is_external()` guard) — never on the flash worker itself —
  so the reentrancy hazard (`project_flash_worker_reentrancy`) is not live
  today. Wiring the device write/delete functions (mirroring
  `cfg_fs_write_atomic_device()`'s flash-worker dispatch) is a follow-up,
  shared with the identical gap already open for zones.
- **Tests**: added to the existing `test_profiles_http.c` (its own
  executable, since `nvs_load_all_from()` is `static` — no separate TU could
  reach it) rather than a new file: migration-on-load, divergence tie-break
  both directions, stale-file-after-delete-not-resurrected, partition-absent,
  mount-failed, interrupted-write-leaves-old-file-intact, and a save/load/
  delete round trip through the unmodified public API confirming the file
  backs the slot and is removed on delete. Full suite: 261/261 checks in this
  executable, all 29 host-test executables build and pass,
  `tools/run_all_checks.ps1` 63/64 (the one failure, `check_cfg_fs_tie_break.ps1`
  flagging `pref_cfg_fs.c`'s pre-existing `>=`, is step 3's file, not this
  one — this module's own line passes that check).
- **Negative-tested the divergence tie-break**: flipped
  `profiles_cfg_fs_resolve()`'s `file_rev > nvs_rev` to `file_rev < nvs_rev`
  (production code). Reran the suite: 3 tests went RED, shortest failing
  line: `test_profiles_http.c:505: FILE content wins (higher rev): name
  preserved`. Restored the line by hand; the file is new/untracked so
  `git diff` shows nothing for it by construction — confirmed instead by
  rerunning the full suite green again and diffing the restored line
  character-for-character against what was there before the break.

**Step 5 (item 8 — kiln config slots only), 2026-09-07 — done, host-proven,
board-absent by construction.** Item 2 (zone normals) was NOT done here but is
DONE (2026-10-03, `zone_normals.dat` via `pref_cfg_fs`, same shape as relay names). Item 9 (adaptive tune) was NOT done at this point in the plan's
history but is DONE as of Step 6c below (`762bb29e` bridge, `2e88e90a`
/api/cfgfs reporting). The pre-fire interlock (refuse a firing if `cfg`
did not mount) was NOT done here; closed 2026-10-06 as a save refusal rather than a firing gate (see banner).

- **Shape**: unlike `profiles_cfg_fs.c`'s per-slot files, the whole saved-
  configs store — every slot, `active_id`, `next_id` — was ALREADY one NVS
  blob under one key (`kiln_cfg_store.c`'s `NVS_KEY_STORE`/`kilncfgs`), so
  this bridge copies `zones_config_cfg_fs.c`'s single-document shape instead:
  one file (`kiln_configs.json`), one rev counter for the whole document.
  Every mutating call (save/clone/apply/delete/rename) already funnels
  through `kiln_cfg_store.c`'s one `nvs_save_store()`, so bumping the rev
  there once — including on delete — satisfies the "bump the rev on both
  save and delete" rule `profiles_cfg_fs.c` established, generalized to a
  single document rather than a per-slot array.
- **New files**: `App/drivers/persist/kiln_cfg_store_internal.h` (the
  current-version `kiln_cfg_entry_t`/`kiln_cfg_store_blob_t` layout, moved out
  of `kiln_cfg_store.c` so the bridge module can share it without the whole
  internal layout becoming part of the public `kiln_cfg_store.h`) and
  `App/drivers/persist/kiln_cfg_store_cfg_fs.c`/`.h`. File format: 4-byte
  little-endian `rev` + a byte-for-byte `kiln_cfg_store_blob_t`, always at the
  CURRENT version — this store has no wire-format migration chain of its own
  beyond `kiln_cfg_store.c`'s existing v1→v2 NVS migration, which only ever
  runs against the NVS blob (a `cfg`-file was never produced by a v1-era
  build, since this bridge postdates v2); a file found at any other version
  is simply treated as invalid, same as absent/corrupt.
- **Tie-break, STRICT**: `file_rev > nvs_rev`, never `>=` — same reasoning as
  `zones_config_cfg_fs.c`/`profiles_cfg_fs.c`, written correctly from the
  start. `check_cfg_fs_tie_break.ps1` passes against it.
- **Delete handling / stale-delete-not-resurrected**: proven directly rather
  than via a second per-slot rev array (there is only one rev for the whole
  document): a delete whose file write fails leaves the file at the
  pre-delete rev still showing the deleted slot `in_use`; NVS advances past
  it. A later load adopts NVS (strictly higher rev) and resyncs the file,
  confirmed to no longer carry the deleted slot —
  `test_cfg_fs_stale_delete_not_resurrected` in `test_kiln_cfg_store.c`.
- **Partition-absent / mount-failed**: `cfg_fs_is_available()` false (every
  board today) makes every call a no-op deferring to the NVS candidate,
  proven by `test_cfg_fs_partition_absent_falls_through_to_nvs_only` and
  `test_cfg_fs_mount_failed_falls_through_to_nvs_only`.
- **Stack**: `kiln_cfg_store_blob_t` is several KB (`KILN_CFG_MAX_COUNT`
  slots, each a full `ZONES_CONFIG_BLOB_MAX_SIZE` zones-config blob) — too
  large for a bare local array on a task stack given this codebase's incident
  history. Every buffer of that size in `kiln_cfg_store_cfg_fs.c` is
  heap-allocated (`malloc`/`free`), the same choice `kiln_cfg_store.c`'s own
  v1-migration path already made for the identical reason.
- **Flash-worker wiring (requirement 6) explicitly NOT done in this pass**:
  `cfg_fs_mount.c`'s device-write-function wiring/assert
  (`cfg_fs_assert_device_write_fns_installed()`/
  `cfg_fs_install_device_write_fns()`) is out of this task's scope by brief
  — `kiln_cfg_store_cfg_fs_*_write_fn()` defaults to the host-safe
  `cfg_fs_write_atomic()` and is never wired to
  `cfg_fs_write_atomic_device()`. This is a real gap (flagged, not silently
  left): once that wiring lands for `zones_config_cfg_fs.c`/`pref_cfg_fs.c`/
  `profiles_cfg_fs.c`, this module needs the identical line added alongside
  them. No flash-worker reentrancy hazard is live today either way — every
  caller of `kiln_cfg_store.c`'s public API already runs off the flash worker
  (same `caller_stack_is_external()` guard the module already had).
- **Tests**: added to the existing `test_kiln_cfg_store.c` (kept in the
  "main" host-test executable, since `kiln_cfg_store.c` is `#include`d
  directly there for its `static` state) — partition-absent, mount-failed,
  NVS-fallback-then-migrate, dual-write sync across a mixed save/delete
  sequence, divergence tie-break both directions, equal-rev-adopts-NVS (the
  rollback round trip), stale-delete-not-resurrected, interrupted-write-
  leaves-old-store-intact. Full suite green (6839/6844; the 5 failures are
  `test_dualwrite_window.c`, another session's in-flight, explicitly
  off-limits work). `tools/run_all_checks.ps1`: 62/64; the 2 failures
  (`check_source_path_drift.ps1`'s `dualwrite_window.c` allowlist entry,
  `check_test_has_assertions.ps1`'s `test_dualwrite_window.c` vacuous test)
  are the same concurrent, out-of-scope pass — `check_cfg_fs_tie_break.ps1`
  and `check_c_files_in_cmakelists.ps1` are both green against this module.
- **Negative-tested the tie-break fix itself**: flipped
  `kiln_cfg_store_cfg_fs_resolve()`'s `if (file_rev > nvs_rev)` to
  `if (file_rev >= nvs_rev)` (production code). `check_cfg_fs_tie_break.ps1`
  correctly flagged it (warning, since the file was still untracked at that
  point); reran the host-test suite: went RED, shortest failing line
  `test_kiln_cfg_store.c:1071: NVS wins the EQUAL-rev tie -- the edit made on
  rolled-back firmware is NOT discarded in favour of the stale file`.
  Restored the line by hand; `diff` against a saved pre-break copy of the
  file confirmed byte-identical, and the full suite passed green again.

Only **step 1** needs a partition-table change, and it is append-only into the
free tail — no existing partition moves or resizes, same discipline every prior
revision of that file followed. It does carry the standard `otadata`-erase +
bootloader-reflash obligation.

---

## 6. Risks I would not take, and questions for the owner

**Would not take:**

- **A one-shot batch migration at boot.** Half-completion across items with no
  per-item fallback is how a board bricks. Read-through is strictly safer and
  costs nothing.
- **`format_if_mount_failed=true` on `cfg`.** Correct for `logs`, catastrophic
  for tuning data. A one-line copy-paste from `log_store_mount.c` would do
  exactly this — call it out in review.
- **Moving Wi-Fi credentials, boot-guard state, touch cal, watchdog/OTA/crash
  records.** All read before mount; all needed to *recover* a board whose
  filesystem is the thing that failed.
- **Sharing the `logs` partition.** A log-volume GC problem must not be able to
  reach config.
- **Deleting NVS keys in the same release that starts writing files.** The
  rollback trap is already documented and has already bitten; do not widen it.
- **Touching the RP2040 config store.** Different chip, different failure
  domain, and its ARMED-write refusal is a property a generic store weakens.
  Its atomicity defect is real but is a separate fix, on that chip.
- **Landing step 1 without a bench test of the erased-`cfg` boot path.** That
  test *is* the brick insurance.

**Would ask the owner before starting:**

1. **How far back must rollback work?** This sets the dual-write window length
   and whether step 7 is ever scheduled. Everything else follows from it.
2. **Is the `otadata` erase + bootloader reflash acceptable now?**
   `partitions.csv` notes this obligation has been outstanding across several
   table revisions; step 1 forces it.
3. **512 K for `cfg`, or larger?** Current data is well under 64 K, but if
   firing history is meant to grow past 5 runs/profile (item 7), size it once
   rather than revise the table twice.
4. **Should the pre-fire interlock in step 5 be a hard refusal or an
   override-able warning?** Recommendation is hard refusal; it is the owner's
   call because it makes a mount failure a firing-stopper.
5. **Relay cycle counters: MOVE or KEEP?** Listed UNDECIDED; the plan assumes
   MOVE-last.

---

**Step 6a (item 4 — relay cycle counters), 2026-09-08 — done, host-proven,
board-absent by construction.** The last of the three remaining genuinely
NVS-only items named in this task, and the last item this doc's own
UNDECIDED note above (question 5) flagged — landed as MOVE, last, per the
plan's own recommendation, after every other item.

- **Bridge used**: the GENERIC `pref_cfg_fs.h` bridge (unit_pref/ramp_assist/
  display_power/relay_names/TZ's shared module), not a bespoke one —
  `relay_cycles_blob_t` (version + 5 counts + 5 types + 5 rated-overrides,
  well under `PREF_CFG_FS_MAX_ITEM`) is exactly the "small fixed-size struct"
  shape that bridge targets. File: `relay_cycles.dat`; rev key:
  `relay_cyc_r` (a separate NVS key, same reasoning every other `pref_cfg_fs`
  item uses — `NVS_KEY_LEN_CHECK`'d; the FILE PATH itself is deliberately
  NOT `NVS_KEY_LEN_CHECK`'d, since it is a cfg-filesystem relative path, not
  an NVS key — an early draft of this pass wrongly length-checked it against
  the 15-char NVS limit and failed to build).
- **Hazard handling (bench-accumulated counts, observed 2201/2994/3214,
  cannot be regenerated)**: the v1→v2 in-place migration in
  `relay_cycles_init()`'s existing NVS load is UNTOUCHED — the file bridge
  only ever sees CURRENT-version bytes (`relay_cycles_file_validate()`
  requires `version == RELAY_CYCLES_VERSION`), so an old-version file is
  simply "not valid" and the resolve defers to the (already-migrated) NVS
  candidate, exactly like every other divergence case. Composes with
  `relay_cycles_restore_all()` (the backup-restore path) BY CONSTRUCTION, not
  by a special case: restore drives the same `s_rc.rev + 1` → snapshot →
  `persist_snapshot()` path every other writer uses, so a restored value
  outranks stale file/NVS content the same way an ordinary save would.
  `relay_cycles_reset()` (single-relay zeroing) also composes the same way —
  proven by `test_cfg_fs_reset_all_composes_with_migration_never_loses_counts()`
  (see Tests below), which resets relay 0 after a bench-accumulated,
  file-migrated 3-relay history and confirms relays 1/2 survive untouched.
- **Read/write policy, tie-break, partition-absent/mount-failed**: identical
  to every other `pref_cfg_fs` item — reads prefer the file when valid,
  falling back to NVS otherwise (with opportunistic migration); FILE FIRST
  (best-effort) then NVS (authoritative) on write; STRICT `file_rev >
  nvs_rev`, inherited for free from the shared bridge (this module adds no
  tie-break logic of its own, so it cannot regress
  `check_cfg_fs_tie_break.ps1`'s invariant independently of the shared code).
- **Validated on load exactly as NVS does**: `relay_cycles_file_validate()`
  is a strict subset of `relay_cycles_init()`'s own current-version
  acceptance test (size + version) — no bound relaxed, and there is no "0
  means default" sentinel in this blob to protect (a count of 0 is a real,
  meaningful value: a fresh or just-reset relay).
- **`/api/cfgfs` needs**: DONE, `2e88e90a` (2026-09-08). `relay_cycles_get_
  dualwrite_status()` (new, `relay_cycles.c`) reads the file side via
  `pref_cfg_fs_load_raw()` and the NVS side via its own direct read (not
  `s_rc`'s in-RAM copy), computes `diverged` via `cfg_fs_status_item_
  diverged()`, and is wired into `diagnostics_http.c`'s item list under the
  name `"relay_cycles"` — moved out of `cfg_fs_status.c`'s `nvs_only` array,
  which is now empty. No new bridge-module name needed (reuses `pref_cfg_fs.c`,
  no new `*_cfg_fs.c` file), so `cfgfs_nvs_only_drift_check.py`'s
  `EXPECTED_BRIDGE_MODULES` needed no new entry for it.
- **Tests**: added to `test_relay_cycles.c` (its own executable,
  `kilnctl_host_tests_run_state_relay_cycles.exe`, now also linking
  `cfg_fs.c`/`pref_cfg_fs.c`) — partition-absent, NVS-fallback-then-migrate,
  repeated-flush dual-write sync, divergence tie-break (both the
  strictly-higher-file-wins direction and the EQUAL-rev-adopts-NVS
  direction), and the reset/restore composition test above. 141/141 checks
  pass in that executable; `tools/run_all_checks.ps1` 67/69 (the 2 known RED
  are `cfg_fs_status.c`/`cfg_fs_mount.c`, off-limits/owned elsewhere — named,
  not fixed, per this task's brief).
- **Negative-tested the dual-write itself**: commented out
  `persist_snapshot()`'s `pref_cfg_fs_save(RELAY_CYCLES_FILE_PATH, ...)` call
  (production code, replaced with `esp_err_t file_err = ESP_OK;`). Reran the
  suite: 3 tests went RED, shortest failing line: `test_relay_cycles.c:693:
  the flush's dual-write actually created the file`. Restored the line by
  hand; `git diff -- firmware/KilnFW/App/drivers/persist/relay_cycles.c`
  confirmed the working tree contains only the intended addition (the
  `pref_cfg_fs_save()` call), clean of the break.
- **A found-and-fixed CMake/build wiring gap**: `test_profile_executor_
  prestart.c` (which links `adaptive_tune.c` for real, see Step 6c below)
  had its OWN fake `cfg_fs_is_available()` (always `false`) predating this
  pass — once `pref_cfg_fs.c` needed linking into that same executable for
  item 9, the fake collided (`LNK2005`) with the real `cfg_fs.c`'s
  definition. Fixed by deleting the fake and linking real `cfg_fs.c`
  instead — behaviorally identical for that test file (it never calls
  `cfg_fs_init()`, so the real function also returns `false`).

**Step 6b (item 7 — firing stats/history), 2026-09-08 — done, host-proven,
board-absent by construction.**

- **New bespoke module**: `App/drivers/persist/firing_stats_cfg_fs.c`/`.h` —
  NOT the generic `pref_cfg_fs` bridge, because `profile_firing_history_
  blob_t` is 1364 bytes, far over `PREF_CFG_FS_MAX_ITEM` (128). Shaped like
  `profiles_cfg_fs.c` (per-id files, `stats/fs<id>.dat`), but simpler: the
  blob is VERSIONLESS (see `profile_executor_internal.h`'s own "VERSIONLESS
  HAZARD" comment, pinned by `_Static_assert`s at 1364 bytes) with no decode
  function to share, so the file holds the exact raw bytes and is valid only
  at EXACTLY that size — no tail-append tolerance on the file side (unlike
  the NVS side's existing v1-blob migration, left untouched). Rev is a
  per-id NVS key (`"fsr_<id>"`, `FIRING_STATS_NVS_NAMESPACE`/`_PARTITION`,
  duplicated from `profile_executor_firing_stats.c`'s own file-scope-static
  constants rather than sharing a header, matching that file's existing
  convention of keeping them `static`).
- **Hazard handling ("a move must not silently discard firing history")**:
  `firing_stats_load()` (unchanged public signature — requirement 2) now
  wraps the existing NVS-only loader (renamed `nvs_only_load()`, logic
  byte-for-byte unchanged, including its v1-blob tail-append migration and
  its "any other mismatch discards, loudly" branch) with `firing_stats_
  cfg_fs_resolve()`. The resolve table has NO delete-vs-failed-write
  ambiguity to resolve (unlike profiles, this item never deletes — history
  only ever grows) — a file valid with NVS invalid can only mean "a prior
  save's file write landed and the NVS write failed," so it is trusted
  outright rather than discarded, which is precisely what keeps this pass
  from being a second way to lose history. `firing_stats_load()`'s
  pre-existing `false`-on-corruption contract (checked by one caller,
  `profile_executor_status.c`) is preserved: the wrapper returns `nvs_valid`
  itself in the "neither side has anything" case, which is `false` only for
  genuinely unrecognized/corrupt NVS data — a never-fired profile's `nvs_
  valid` is `true` (an empty, `count==0` blob), so it never hits that branch.
  A pre-existing host test (`test_firing_stats_load_discards_unknown_size_
  blob`, not new) caught the first draft of this wrapper (which always
  returned `true`) getting this wrong — see Tests below.
- **Dual-write policy**: `firing_stats_persist()` (unchanged signature) now
  reads the current rev via `firing_stats_cfg_fs_read_rev()`, writes the
  FILE first (best-effort) at `rev+1`, then the NVS blob (authoritative,
  unchanged code path/error handling), then the rev key (in a separate NVS
  transaction, after the blob's own commit succeeds — a lost rev-key write
  only under-trusts NVS on a future divergence check, it can never lose or
  corrupt the blob itself, which is already durably committed by that
  point).
- **`/api/cfgfs` needs**: DONE, `2e88e90a` (2026-09-08). `firing_stats_get_
  dualwrite_status()` (new, `profile_executor_firing_stats.c`, declared in
  `profile_executor.h`) aggregates over the user profile slots 0..
  `PROFILES_MAX_COUNT`-1 (this bridge has no fixed, enumerable id space —
  any profile can be a user slot OR a 3-digit builtin id, and only ids that
  have actually fired get a key at all — a divergence confined to a builtin
  profile's history is a documented gap this aggregation does not cover) and
  is wired into `diagnostics_http.c`'s item list under `"firing_stats"` —
  moved out of `cfg_fs_status.c`'s `nvs_only` array, which is now empty.
  `cfgfs_nvs_only_drift_check.py`'s `EXPECTED_BRIDGE_MODULES` already carried
  the `"firing_stats"` entry from this item's step 6b bridge landing.
- **Tests**: added to `test_profile_executor_prestart.c` (already links
  `profile_executor_firing_stats.c` for real) — partition-absent,
  NVS-fallback-then-migrate, repeated-persist dual-write sync (ring growing
  to 3 entries, newest first, on both sides), and the negative test. Fixed a
  real test-isolation bug found while adding these: `reset_all_fscf()`'s
  `_rmdir()`/`rmdir()` on the `stats/` subdirectory fails silently when
  non-empty (same "delete known filenames before rmdir" class `test_relay_
  names_cfg_fs.c`'s own `reset_all()` already documents) — without deleting
  each known per-id file first, a leftover `fs7.dat` from an EARLIER run of
  this test binary leaked a stale 5-entry ring into a later test reusing
  profile id 7 (`out.count == 5` instead of the expected `1`, caught while
  debugging, not shipped). 32/32 host-test executables build and pass;
  `tools/run_all_checks.ps1` 67/69 (the 2 known RED, same as item 4's note).
- **Negative-tested the dual-write itself**: commented out `firing_stats_
  persist()`'s `firing_stats_cfg_fs_save(rec->profile_id, &blob, new_rev)`
  call (production code, replaced with `esp_err_t file_err = ESP_OK;`).
  Reran the suite: 5 tests went RED, shortest failing line: `test_profile_
  executor_prestart.c:7821: reloaded run matches what was persisted`.
  Restored the line by hand; `git diff -- firmware/KilnFW/App/drivers/
  control/profile_executor_firing_stats.c` confirmed the working tree
  contains only the intended addition, clean of the break.

**Step 6c (item 9 — adaptive-tune state), 2026-09-08 — done, host-proven,
board-absent by construction. DELIBERATELY SIMPLER than this doc's own
framing of item 9** ("has a namespace migration scar," implying bespoke
divergence forensics like zones config) — this task's brief explicitly
licenses a simpler treatment per the prior audit's finding that adaptive-
tune state is "re-derivable over one firing," and that is the treatment
taken here.

- **What moved**: only the persisted Ki-diagnosis baseline
  (`adaptive_tune_kibase_blob_t` — a `mask` byte + one `float` per zone,
  `ADAPTIVE_TUNE_NVS_KEY_KIBASE`/`"ki_base"`). The opt-in flag already lives
  in the zone config blob (a prior pass moved it there, off-limits territory
  for this task), and `en_mask`/`en_migrated` are a one-shot, already-
  consumed migration scar with nothing left to move.
- **Bridge used**: the GENERIC `pref_cfg_fs.h` bridge, same reasoning as item
  4 — the blob is small and fixed-size. Rev is a single in-memory counter
  (`s_kibase_rev`, resolved at boot from whichever side won, incremented by
  every `save_kibase_job()` call) rather than a per-zone array, since ALL
  THREE call sites that build a `kibase_job_t` (`adaptive_tune_run_end()`,
  `adaptive_tune_clear_ki_baseline()`, `adaptive_tune_revert()`) funnel
  through that one function to reach storage — a single counter there cannot
  desync between them the way one counter per call site could.
- **Validator**: `kibase_file_validate()` checks only that `mask` names no
  zone index past `MAX31856_CHANNEL_COUNT` — everything else (a garbage
  baseline float for a zone whose bit IS set) is exactly as trusted as the
  NVS blob always was. This is the "simpler treatment" made concrete: no
  per-zone divergence logging, no resync-on-mismatch bookkeeping beyond what
  the generic bridge already does uniformly for every item.
- **Read/write policy, tie-break, partition-absent/mount-failed**: identical
  to every other `pref_cfg_fs` item (inherited, not reimplemented) — reads
  prefer the file, FILE FIRST then NVS on write, STRICT `file_rev >
  nvs_rev`. File: `ki_base.dat`; rev key: `kibase_rev`.
- **`/api/cfgfs` needs**: DONE, `2e88e90a` (2026-09-08). `adaptive_tune_get_
  kibase_dualwrite_status()` (new, `adaptive_tune.c`, declared in
  `adaptive_tune.h`) reads the file side via `pref_cfg_fs_load_raw()` and the
  NVS side via its own direct read (never `s_kibase_rev`/the in-RAM zone
  state, which may already have absorbed a resolve this boot), computes
  `diverged` via `cfg_fs_status_item_diverged()` (a real `memcmp` of the
  whole blob -- mask AND every zone's baseline float), and is wired into
  `diagnostics_http.c`'s item list under `"adaptive_tune"` — moved out of
  `cfg_fs_status.c`'s `nvs_only` array, which is now empty. Reuses the
  `"prefs"` bridge entry, no new `EXPECTED_BRIDGE_MODULES` entry needed.
- **Tests**: added to `test_adaptive_tune.c` (already links `adaptive_
  tune.c` for real) — partition-absent, NVS-fallback-then-migrate, and the
  negative test. 352/352 checks pass in that executable.
- **Negative-tested the dual-write itself**: commented out `save_kibase_
  job()`'s `pref_cfg_fs_save(ADAPTIVE_TUNE_KIBASE_FILE_PATH, ...)` call
  (production code, replaced with `esp_err_t file_err = ESP_OK;`). Reran the
  suite: 1 test went RED (`test_adaptive_tune.c:546: the save's dual-write
  actually created the file`) plus a pre-existing, unrelated flaky failure
  in a DIFFERENT executable (`test_unit_pref.c:262`, a concurrent session's
  in-flight `unit_pref.c` edit at the time — gone on the next rebuild, not
  caused by this change). Restored the line by hand; `git diff --
  firmware/KilnFW/App/drivers/control/adaptive_tune.c` confirmed the working
  tree contains only the intended addition, clean of the break.
- **Not done**: this item's read-through is wired into `adaptive_tune_
  init()` only — `adaptive_tune_migrate_enable_flags()`'s own separate
  namespace scar (`en_mask`/`en_migrated`) is untouched, since it is a
  different NVS key with its own already-completed one-shot migration and
  is not part of the Ki-baseline blob this step moved.

**Summary — three remaining genuinely-NVS-only items from `70ed6514`, all
closed 2026-09-08**: relay cycle counters (4, generic bridge, bench-
accumulated counts proven to survive reset/restore composition), firing
stats/history (7, bespoke bridge, versionless blob handled without widening
its existing tolerance, history never silently discarded), adaptive-tune
state (9, generic bridge, deliberately simplified per the re-derivable
audit finding). Tie-break is STRICT `file_rev > nvs_rev` throughout,
inherited from the shared bridge for items 4/9 and hand-written the same
way (and `check_cfg_fs_tie_break.ps1`-clean) for item 7. `cfg_fs_status.c`'s
`nvs_only`/`dual_write` arrays are the one thing still needed to make
`/api/cfgfs` agree with reality for all three — off-limits/owned by a
concurrent pass on that file as of this commit; flagged in each step's own
note above rather than edited.
