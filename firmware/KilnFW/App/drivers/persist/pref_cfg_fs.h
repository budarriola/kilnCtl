// pref_cfg_fs -- generic read-through/dual-write bridge between a small
// scalar/struct NVS preference (unit_pref.c, control/ramp_assist_cfg.c,
// display_power_cfg.c today) and the `cfg` LittleFS partition
// (cfg_fs.h/cfg_fs_mount.h), per docs/FILESYSTEM_USER_DATA_PLAN.md section 5
// step 3 ("Migrate prefs (10,11,12,14) -- the lowest-stakes items").
//
// WHY GENERIC: unlike zones config (zones_config_cfg_fs.c/.h, a single
// 22-version migration chain worth its own module), every preference item
// this bridges is a tiny fixed-size blob with NO version history -- a u8
// enum/bool, or a small `_Static_assert`-free struct. Three copies of the
// same rev-prefixed-file/tie-break logic would be a "reset one side of a
// pair" bug waiting to happen (project_reset_one_side_bug_class) the moment
// one copy's tie-break got hand-edited and the others didn't. One generic
// module, parameterized by path/size/validator, means there is exactly one
// place this policy lives.
//
// FILE FORMAT: `<4-byte little-endian rev><item_size raw bytes>` -- same
// shape as zones_config_cfg_fs.c's file, minus the version-migration byte
// (these items have no migration chain to preserve). `item_size` is fixed
// per call site (the caller's own struct size), never varies at runtime.
//
// READ-THROUGH POLICY / DIVERGENCE TIE-BREAK: identical to
// zones_config_cfg_fs.c's documented policy -- reads prefer the file when it
// decodes to something `validate` accepts; otherwise fall back to the
// caller's NVS candidate (and opportunistically migrate it to the file, if
// valid). When both sides are valid and their bytes differ, the higher rev
// wins, logged (ESP_LOGW naming the path and both revs), and the loser is
// resynced from the winner.
//
// VALIDATION: `validate` re-runs each caller's OWN existing NVS-load
// validation logic (in-range check) against file bytes too -- required by
// this task ("validated on load exactly as its NVS path validates today").
// A byte count that does not match `item_size` is never valid, checked
// before `validate` is ever called.
//
// PARTITION ABSENT / MOUNT FAILED: exactly like zones_config_cfg_fs.c,
// cfg_fs_is_available() is checked first in every function; false makes
// every function here a no-op that reports "no file" and defers entirely to
// the caller's NVS candidate. This is the path every board runs today (no
// `cfg` partition mounted in the boot sequence yet).
#ifndef PREF_CFG_FS_H
#define PREF_CFG_FS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

// Generous upper bound on any one preference item's raw byte size --
// relay_names_cfg_t (docs/FILESYSTEM_USER_DATA_PLAN.md item 3, 1 + 4*16 + 4
// types + 4 crc = 73 bytes of fields, 76 with alignment padding as of
// RELAY_NAMES_CFG_VERSION 2; it was 69/72 at v1) is the largest today,
// previously display_power_cfg_blob_t
// (5 bytes). Raised from 32 to 128 to fit relay names with headroom, rather
// than giving relay names its own bespoke bridge module -- it has no
// migration chain of its own, just a fixed-size struct, exactly the shape
// this generic module targets. Callers whose item exceeds this fail loudly
// (ESP_ERR_INVALID_SIZE) rather than silently truncate.
#define PREF_CFG_FS_MAX_ITEM 128

// Upper bound for the LARGE items (setup-wizard progress blob, live-edit
// working profile). Items above PREF_CFG_FS_MAX_ITEM use a short-lived heap
// block for the file image instead of a stack buffer; callers' own copies of
// the item (the nvs_bytes / out_bytes arguments) are still theirs to place.
#define PREF_CFG_FS_MAX_LARGE_ITEM 2048

// Variable-length item (live-edit working profile): reads the file at
// rel_path into out (capacity cap, <= PREF_CFG_FS_MAX_LARGE_ITEM), reporting
// the item length (file size minus the 4-byte rev) and rev. Returns false when
// cfg is unmounted, the file is absent, unreadable or larger than cap + 4. No
// validation: the caller decodes/validates the bytes itself. Written back with
// pref_cfg_fs_save()/pref_cfg_fs_commit() using the actual length.
bool pref_cfg_fs_load_var(const char *rel_path, void *out, size_t cap, size_t *out_len, uint32_t *out_rev);

// Deletes the file at rel_path. ESP_OK when it did not exist either;
// ESP_ERR_INVALID_STATE when cfg is not mounted.
esp_err_t pref_cfg_fs_remove(const char *rel_path);

// Matches cfg_fs_write_atomic()'s signature (cfg_fs.h) and
// cfg_fs_write_atomic_device()'s (cfg_fs_mount.h) -- same seam
// zones_config_cfg_fs.h uses: host tests exercise the real cfg_fs_write_atomic()
// (host-safe no-op-if-unmounted), device boot would install
// cfg_fs_write_atomic_device() (routes through the flash worker -- HAZARD:
// never call this while already running ON the flash worker task,
// project_flash_worker_reentrancy). Defaults to cfg_fs_write_atomic() if
// never set.
typedef esp_err_t (*pref_cfg_fs_write_fn_t)(const char *rel_path, const void *data, size_t len);
void pref_cfg_fs_set_write_fn(pref_cfg_fs_write_fn_t fn);

// Test-only reset back to the default write function (cfg_fs_write_atomic).
// No on-device call site.
void pref_cfg_fs_reset_write_fn_for_test(void);

/* Read the currently-installed write function -- used by cfg_fs_mount.c's
 * real-build assert that the device writer is installed whenever the
 * filesystem is mounted (docs/audits/filesystem_migration_review_2026-09-07.md
 * section 1). */
pref_cfg_fs_write_fn_t pref_cfg_fs_get_write_fn(void);

// Returns true if `bytes` (exactly `len` bytes, always == the call site's
// item_size) is a value this build considers valid and safe to adopt --
// same discipline as unit_pref_start()'s range check, ramp_assist_cfg_start()'s
// 0/1 check, display_power_cfg_start()'s field-range checks. MUST NOT treat a
// legitimate sentinel (e.g. a documented "0 means use firmware default")
// as corruption -- that trap was hit and fixed elsewhere in this codebase
// with progress_band_c; every caller's validator here is the caller's own
// pre-existing NVS-load check, reused verbatim, so this module introduces no
// new sentinel handling of its own.
typedef bool (*pref_cfg_fs_validate_fn_t)(const void *bytes, size_t len);

// Reads and validates the file at `rel_path` only, no NVS comparison.
// *out_valid mirrors the caller's own NVS-load convention: true only if the
// file exists, is exactly `item_size` bytes past the rev prefix, and
// `validate` accepts it. On any other outcome (absent, wrong size, rejected)
// *out_bytes is zeroed, *out_rev is 0, *out_valid is false -- never a
// half-populated buffer.
void pref_cfg_fs_load_raw(const char *rel_path, size_t item_size, pref_cfg_fs_validate_fn_t validate,
                           void *out_bytes, uint32_t *out_rev, bool *out_valid);

// Identical to pref_cfg_fs_load_raw() in every result, but logs nothing for a
// wrong-size or validator-rejected file. For read-only status polls (GET
// /api/cfgfs) that must not repeat a boot-time warning on every request.
void pref_cfg_fs_load_raw_quiet(const char *rel_path, size_t item_size, pref_cfg_fs_validate_fn_t validate,
                                void *out_bytes, uint32_t *out_rev, bool *out_valid);

// Newer-schema probe for a file whose length does NOT match the caller's
// current struct (a size-changing newer-firmware blob is otherwise
// indistinguishable from corruption: load_raw/resolve reject any wrong-size
// file before the validator runs). Returns true only when the file exists, its
// length differs from 4 + item_size, is long enough to hold the version byte,
// and that byte (at item offset `version_offset`) is above `current_version`;
// *out_version then holds it. Read-only: never writes or erases. A wrong-size
// file at current-or-older version, an exact-size file, an absent file or an
// unmounted cfg all return false (corruption/normal paths unchanged).
// Callers that get true must not let pref_cfg_fs_resolve() run, since its
// NVS->file migration would overwrite the newer file.
bool pref_cfg_fs_probe_newer_wrong_size(const char *rel_path, size_t item_size, size_t version_offset,
                                        uint8_t current_version, uint8_t *out_version);

// Core of the read-through policy. `nvs_bytes`/`nvs_valid`/`nvs_rev` are
// whatever the caller's existing NVS load already produced this boot --
// never read or written by this function, a pure decision given these
// inputs plus whatever is on the file. `out_bytes` (capacity `item_size`)
// and `out_rev` are always written; `out_used_file` says which side won
// (informational).
//
// Returns true if `out_bytes` is trustworthy, false if neither side
// produced anything valid (out_bytes zeroed, out_rev 0, out_used_file
// false) -- the caller falls back to its firmware default in that case,
// exactly as it does today when its own NVS load fails.
//
// May perform a resync WRITE as a side effect (self-heal a stale/missing
// file from a valid NVS candidate, or vice versa) through whatever function
// pref_cfg_fs_set_write_fn() installed -- a failed resync write is logged
// and otherwise ignored.
bool pref_cfg_fs_resolve(const char *rel_path, const void *nvs_bytes, size_t item_size, bool nvs_valid,
                          uint32_t nvs_rev, pref_cfg_fs_validate_fn_t validate, void *out_bytes, uint32_t *out_rev,
                          bool *out_used_file);

// Writes `bytes` (`item_size` bytes, must already be validated -- this
// function does not call `validate`) to the file at `rel_path`, at `rev`.
// No-op returning ESP_ERR_INVALID_STATE if cfg_fs never mounted -- callers
// must treat that as expected on every board today, not a surfaced error
// beyond a debug log (mount-failure contract,
// docs/FILESYSTEM_USER_DATA_PLAN.md). ESP_ERR_INVALID_SIZE if item_size
// exceeds PREF_CFG_FS_MAX_ITEM.
esp_err_t pref_cfg_fs_save(const char *rel_path, const void *bytes, size_t item_size, uint32_t rev);

// The persistence step of every preference setter since the dual-write window
// closed (docs/CONFIG_FILESYSTEM.md, "Dual-write window: closed"): the cfg file
// is the ONLY place a save goes -- no NVS write follows it, and a failure here
// is never papered over by one. pref_cfg_fs_save() (cfg_fs_write_atomic()'s
// temp-file/rename/read-back-verify underneath) plus a loud ESP_LOGE naming
// `what` on any failure, INCLUDING ESP_ERR_INVALID_STATE (cfg not mounted):
// before the close that code was an expected non-error because NVS carried the
// save; now it means the setting was NOT persisted. The caller must return the
// error to its own caller and must NOT advance its in-RAM rev counter on
// failure.
esp_err_t pref_cfg_fs_commit(const char *rel_path, const void *bytes, size_t item_size, uint32_t rev,
                             const char *what);

#ifdef __cplusplus
}
#endif

#endif // PREF_CFG_FS_H
