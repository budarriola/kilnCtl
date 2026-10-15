# KilnFW persistence audit (2026-10-14)

Scope: the cfg LittleFS stores now that the dual-write window is closed
(saves are cfg-file-only, NVS is a frozen read-only legacy source), schema
versions and the migration chain, backup export/import, the kiln_configs
store and its quarantine, factory_reset scopes, boot-time load of corrupt or
torn blobs, and config compatibility across a GitHub update or downgrade.
The audit was code-only. The bench board was not touched. Base: origin/dev
153c12cf5.

Already-fixed items from earlier audits are not repeated here. Those audits
are REVIEW_PERSIST_BATCHES_2026-10-10, UNCHECKED_PERSIST_RESULT_AUDIT_2026-10-09
(including L2, fixed in 4d79ff1a), NVS_WRITER_INVENTORY_2026-10-09,
BENCH_PROF1_DIVERGE_ZONES_WIPE_2026-10-09 and the BACKUP_CFGFS_COVERAGE audit.

Counts: HIGH 0, MED 3, LOW 2, INFO 4.

## Context: the downgrade-to-pre.3 case

Every persisted schema version is the same in v1.0.0-pre.3 and dev HEAD:

| Constant | Value |
|---|---|
| ZONES_CFG_VERSION | 26 |
| PROFILE_VERSION | 4 |
| KILN_CFG_STORE_VERSION | 3 |
| AUX_OUTPUTS_CFG_VERSION | 1 |
| ITER_TUNE_STORE_VERSION | 2 |
| RELAY_NAMES_CFG_VERSION | 2 |
| ZONE_NORMALS_CFG_VERSION | 3 |
| CT_VERIFY_BLOB_VERSION | 1 |
| LIVE_EDIT_RECORD_VERSION | 1 |
| BACKUP_FORMAT_VERSION | 7 |

Between pre.3 and dev, `persist/` differs only by a PSRAM attribute on
`kiln_cfg_store.c` `s_store`. The only new persisted store since pre.3 is
`update_install_record`, which lives in NVS.

A downgrade to pre.3 is therefore lossless today. The risk arrives with the
first future version bump of any store. After that bump, the downgraded image
reads the newer file with pre.3's code, and that code is the code audited
below. MED-1 to MED-3 describe what pre.3 (and dev) do to a file they cannot
decode. A fix landed now protects only future downgrade targets. The fix
notes therefore also cover what the newer firmware must do before it lets a
board go back to an unfixed release.

The zones store already handles this case correctly, and it is the model for
the fixes below. A newer-version `zones.json` is neither adopted nor
overwritten, a rejected file is preserved as `zones.json.bad`, and the load
fault is latched (`zones_config_store.c:468-472`, `:664-676`).

## MED

### MED-1. pref_cfg_fs adopt variant overwrites an undecodable or newer-version file with the frozen NVS copy

- **Where:** `firmware/KilnFW/App/drivers/persist/pref_cfg_fs.c:511-527`, the
  `!file_valid` branch of `resolve_with_file()` for
  `adopt_nvs_if_unreadable == true`, reached through `pref_cfg_fs_resolve()`.
- **Affected stores (callers):**

  | Store | Caller |
  |---|---|
  | update_settings | `update/update_settings.c:172` |
  | display_power | `persist/display_power_cfg.c:160` |
  | time_sync | `net/time_sync.c:187` |
  | hidden-builtin mask | `persist/profiles_builtin.c:179` |
  | favorites | `persist/profiles_favorites.c:248` |
  | relay_cycles | `persist/relay_cycles.c:485` |
  | setup_wizard | `persist/setup_wizard_progress.c:503` |
  | unit_pref | `persist/unit_pref.c:124` |
  | relay_names | `persist/zones_config_store.c:1362` |
  | zone_normals | `persist/zones_config_store.c:1613` |

- **Scenario:** A file exists and is readable, but it fails the store's
  validate callback. Two cases reach this:
  - A future release bumps a store version without changing the struct size.
    For relay_names, `relay_names_validate()` (`zones_config_store.c:1145-1159`)
    rejects any `version != RELAY_NAMES_CFG_VERSION`. The board is then
    downgraded, or reflashed over JTAG to an older dev build.
  - A file is CRC-corrupt.

  Size-only newer detection (`pref_cfg_fs_probe_newer_wrong_size()`,
  `pref_cfg_fs.c:243-289`) is called only by iter_tune
  (`iter_tune_store.c:203`).
- **Consequence:**
  - If the frozen legacy NVS copy (state as of the 2026-10-05 close) is valid,
    it is adopted. Line 521 then calls `pref_cfg_fs_save()` and writes that
    copy over the file at the lower NVS rev. The newer file is destroyed with
    no `.bad` copy, and the board silently runs on months-old relay names,
    relay types, zone normals, unit preference, relay cycle counters or update
    settings. A roll-forward to the newer firmware cannot recover the data.
  - If NVS is not valid, the store runs on defaults. The path is not marked
    rev-unknown, so the next save overwrites the newer file anyway.
- **Fix:**
  1. Have the validate callback (or `pref_cfg_fs_load_raw_checked()`)
     distinguish "version newer than this firmware" from "corrupt".
  2. On newer: call `unknown_set(rel_path, true)`, write nothing, and run on
     the NVS copy in RAM only, or on defaults. The M1 save refusal then
     protects the file.
  3. On corrupt: copy the file to `<path>.bad`, as zones does, before any
     resync.
  4. Never resync the file from the frozen NVS copy at a lower rev than the
     file's own rev prefix, which is still readable even when the payload is
     not.

### MED-2. A rejected profile file is overwritten from frozen NVS, or its slot is reported free

- **Where:**
  - `firmware/KilnFW/App/drivers/persist/profiles_cfg_fs.c:128-134`: any
    `profile_decode_blob()` result other than OK, including
    `PROFILE_DECODE_NEWER`, is logged and treated as no file.
  - `profiles_cfg_fs.c:220-234`: in the `!file_valid` branch of
    `profiles_cfg_fs_resolve_ex()`, NVS invalid returns false (slot unused,
    line 222). NVS valid adopts NVS and writes it over the file (line 230).
- **Scenario:**
  - The first future `PROFILE_VERSION` bump, followed by a downgrade or an
    older JTAG build.
  - Or a corrupt profile file on a slot that was saved after the dual-write
    close. Its NVS blob is either frozen or absent.
- **Consequence:**
  - With a frozen NVS blob: the older profile content replaces the newer file
    with no warning beyond a log line, and the newer schedule is gone for good.
  - With no NVS blob (every profile created after 2026-10-05): the slot shows
    as empty. The next "save as new" or import that allocates the lowest free
    slot writes over the newer file.
  - The NVS side already refuses to wipe a NEWER blob on purpose
    (`firmware/KilnFW/App/drivers/http/profiles_http.c:1276-1293`). The file
    side, which is now the authoritative one, has no such guard.
- **Fix:**
  - Return `PROFILE_DECODE_NEWER` through `profiles_cfg_fs_load_raw_ex()` as
    `*out_error = true` (or a distinct flag). The resolve then takes the
    existing review-7 L3 fail-closed path (`profiles_cfg_fs.c:211-218`, and
    `slot_res_err` in `profiles_http.c:1338-1341`): the slot is unknown,
    refused for writes, and never reported free.
  - For CORRUPT, preserve a `.bad` copy before the NVS resync.

### MED-3. kiln_configs: file-side failures bypass the H3 quarantine, and the frozen NVS blob overwrites the file

- **Where:**
  - `firmware/KilnFW/App/drivers/persist/kiln_cfg_store_cfg_fs.c:70-95` and
    `:107-114`: allocation failure, read error, wrong size and
    `version != KILN_CFG_STORE_VERSION` are all treated as "file absent, NVS
    candidate decides".
  - `kiln_cfg_store_cfg_fs.c:177-189`: in the `!file_valid` branch, a valid
    NVS blob is written over the file at the NVS rev (line 185).
  - The legacy NVS blob is never retired. It is erased only by
    `kiln_cfg_store_quarantine_clear()` (`kiln_cfg_store.c:1853-1854`), so on
    a migrated board it stays a valid, frozen copy forever.
- **Scenario:**
  - A transient LittleFS read error or OOM at boot for the ~7.5 KiB scratch
    buffer.
  - Or a future `KILN_CFG_STORE_VERSION` bump followed by a downgrade or an
    older build.
- **Consequence:**
  - Every kiln config saved, edited, renamed or deleted since the dual-write
    close is replaced by the frozen NVS store, and that overwrite is
    persisted. The H3 quarantine and its newer-version refusal
    (`kiln_cfg_store.c:555-582`) inspect only the NVS blob. They never fire
    for the file, which is the store that is now written.
  - On a board with no NVS blob, the store comes up empty and not quarantined.
    The first operator save writes a one-entry store over the unread file.
  - The pref and profile stores already fail closed on an unreadable file
    (pref M1, profiles review-7 L3). This store does not.
- **Fix:**
  1. In `kiln_cfg_store_cfg_fs_load_raw()`, report read error and OOM
     separately from "absent".
  2. In `nvs_load_store_with_cfg_fs()` (`kiln_cfg_store.c:611-644`), call
     `set_quarantine()` on an unreadable file, a wrong-size file or a
     version-mismatched file. Mutators are then refused through
     `refuse_if_quarantined()`.
  3. Never resync the file from NVS once a file has existed. Treat the
     frozen NVS blob as a first-migration source only, as
     `pref_cfg_fs_resolve_nvs_retired()` does, and retire it once the file is
     adopted.

## LOW

### LOW-1. NVS-retired pref stores: an undecodable or newer file gives defaults, and the next save overwrites it

- **Where:** `firmware/KilnFW/App/drivers/persist/pref_cfg_fs.c:489-508`, the
  `pref_cfg_fs_resolve_nvs_retired()` path. Callers without their own newer
  probe:
  - adaptive_tune (`control/adaptive_tune.c:1472`)
  - ct_verify (`persist/ct_verify_store.c:283`)
  - ramp_assist (`control/ramp_assist_cfg.c:126`)
- **Scenario:** Same as MED-1, for these stores.
- **Consequence:** Nothing is written at load, and defaults are the safe
  choice here. But the path is not marked rev-unknown, so the first save
  after a downgrade overwrites the newer file. The loss is silent, and these
  files hold learned tuning state.
- **Fix:** On a version-newer file, call `unknown_set(rel_path, true)` so
  saves are refused, matching the unreadable-file case at
  `pref_cfg_fs.c:467-486`.

### LOW-2. The update policy compares only three schema versions

- **Where:** `firmware/KilnFW/App/drivers/update/update_policy.c:134-157`.
  `schema_lower` and `schema_newer` use only `zones_cfg_version`,
  `kilnlink_version` and `uart_version`. The override message names zones
  only ("read back zones before heating").
- **Scenario:**
  - An install of an image with the same semver but a different commit (a
    dev build, or a re-tagged pre-release) that carries a lower
    PROFILE / KILN_CFG / AUX / ITER_TUNE / RELAY_NAMES / ZONE_NORMALS /
    BACKUP version.
  - Or an override downgrade where those stores, not zones, are the ones
    that changed.
- **Consequence:**
  - The same-semver case is not refused, so MED-1 to MED-3 trigger with no
    typed confirmation.
  - In the override case, the operator is warned about the wrong store.
  - `update_policy_decide_upload()` (from about line 325) treats an image
    without an id as schema-equal, which widens this further.
- **Fix:** Carry a persisted-schema vector (every governed store version) in
  the image id or manifest. Treat any lower entry as `schema_lower`, and name
  the affected stores in the message. Once the MED fixes land, also refuse
  (or require the typed confirm for) a downgrade to a release older than the
  first one containing them, whenever any store version differs.

## INFO

- **INFO-1. Backup import version skew is handled.** `backup_import.c:1030-1033`
  refuses `version` outside `[BACKUP_FORMAT_VERSION_MIN, BACKUP_FORMAT_VERSION]`.
  The two-pass apply reports a commit-phase failure as a 500 partial write
  with `X-Kiln-Partial-Write` (`backup_import.c:3984-4128`, `:4337-4346`).
- **INFO-2. BENCH_PROF1 proposal 2 (seed from a count-0 board) is largely
  addressed.** A v7 backup carries `thermo_count`/`relay_count` and is applied
  onto an empty board. The topology precheck refuses a mismatch before anything
  is written (`backup_import.c:3659-3684`). A pre-v7 backup still refuses zone
  entries on an empty board, with a "Nothing was written" message. That is
  correct behaviour, not a partial write.
- **INFO-3. The zones store is the reference implementation.** A newer file is
  protected, a rejected file is preserved as `.bad`, OOM or I/O failure means
  "cannot decide", and the load fault is latched (`zones_config_store.c:95-151`,
  `:468-472`, `:664-676`). MED-1 to MED-3 are the stores that have not yet
  reached this standard on the file side.
- **INFO-4. Factory-reset scopes delete their cfg files through dedicated
  lists.** These are `kiln_scope_cfg_files.c` and `profiles_scope_cfg_files.c`,
  called at `factory_reset.c:433` and `:444`, and kept in step by
  `check_kiln_scope_cfg_mirrors.ps1`. No gap was found.
