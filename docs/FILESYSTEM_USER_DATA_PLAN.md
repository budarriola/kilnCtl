# User data on the filesystem — migration design

Companion to `docs/FILESYSTEM_PLAN.md` (which stages the `logs` SPIFFS→LittleFS
track). **Owner decision, 2026-09-07:** user-editable configuration and
user-created profiles move onto a filesystem. This document designs that; it
does not re-argue the decision. Endurance is *not* the case — the endurance
review (`docs/audits/flash_endurance_review_2026-09-07.md`, `157eac42`) found
current NVS wear fine. The case is architectural: structured, inspectable,
diffable, backup/restore-able user data with one file per thing instead of
opaque `_Static_assert`-pinned C structs in KV blobs.

Design is complete here; **nothing below is implemented.**

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
   not for heating.

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
| 0 | (prereq) `logs` LittleFS track steps 1–4 in `FILESYSTEM_PLAN.md` land first — do not stand up a second filesystem implementation. | no | none | as documented there |
| 1 | Add `cfg` partition (512 K at `0xDB0000`, append-only), `cfg_fs_mount()` with `format_if_mount_failed=false`, recovery-mode gate, mount-failure banner, `/api/cfgfs` status. Mounts and does nothing else. | **YES** — and the table change means `otadata` erase + bootloader reflash per `partitions.csv` §7 | none | Host: mount stub. Bench: flash, confirm mount, confirm a *deliberately erased* `cfg` partition still boots and shows the banner. Negative-test the recovery-mode gate by forcing recovery mode. |
| 2 | `cfg_fs_write_atomic()` + temp sweep + flash-worker routing. No callers. | no | none | Host test: interrupt between write and rename (inject failure), assert old file intact and temp swept. **Negative-test by breaking the production function, not a test-local copy.** |
| 3 | Migrate **prefs** (10,11,12,14) — the lowest-stakes items. Read-through + dual-write + `rev` counter. | no | Backup export/import must read/write through the same accessors, not NVS directly — verify `/api/backup/export` output is byte-identical before/after. | Host round-trip; bench: change unit pref, reboot, power-cut during write. |
| 4 | Migrate **profiles** (5,6) and **firing stats** (7). | no | **Highest interaction.** Profile export/import and `backup_import.c` both go through `profiles_http_get()/_save()/_delete()` — keep them as the sole entry points so the storage swap is invisible. Explicitly re-test profile export → factory reset → import. | Host: 8-profile fill, delete, re-save. Bench: export/import round trip; confirm `prof_used` bitmap path is gone, not merely unused. |
| 5 | Migrate **zones config** (1,2,3) + **kiln config slots** (8) + **adaptive tune** (9). Schema 22 becomes `"schema": 22`; migration chain retained. Add the pre-fire interlock: refuse to start a firing if the config FS did not mount. | no | Backup format version stays as-is; export is regenerated from the same getters. | Host: every version 1..22 fixture file parses to the same struct the blob chain produces — **bind the JSON reader to the C migration chain with vector comparison**, per `project_binding_a_python_mirror_to_c`. Bench: full firing on migrated config, then `ota_rollback_esp()` and confirm the rolled-back build reads the same gains (this is the trap being tested). |
| 6 | Migrate **relay cycle counters** (4). | no | counters appear in backup export | Bench soak: confirm the 600 s write cadence lands and survives 24 h. |
| 7 | *(Owner-gated, not scheduled)* Stop dual-writing to NVS. **Not cheaply reversible** — this is the point of no return for rollback. | no | none | Requires an explicit owner decision that no older firmware will be booted again. |

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
