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
// display_power_cfg_blob_t (5 bytes) is the largest today. Callers whose
// item exceeds this fail loudly (ESP_ERR_INVALID_SIZE) rather than silently
// truncate.
#define PREF_CFG_FS_MAX_ITEM 32

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

#ifdef __cplusplus
}
#endif

#endif // PREF_CFG_FS_H
