// Profiles-scope factory reset: the cfg_fs mirrors whose NVS side lives in
// the `profiles_nvs` partition.
//
// factory_reset.c's "profiles" scope erases the whole profiles_nvs
// partition. Three families of dual-written items keep their NVS copy there
// and also have files under `cfg`, which would win the next boot's resolve
// (higher rev, or NVS empty) and silently undo the reset:
//   - profiles/prof<N>.json  user profile slots (profiles_cfg_fs.c)
//   - stats/fs<N>.dat        per-profile firing history (firing_stats_cfg_fs.c);
//                            N is a user slot OR a 3-digit builtin id, so the
//                            set is not enumerable -- the directory is listed
//   - profiles/hidden.json   builtin-schedule hide mask; deleted separately by
//                            profiles_builtin_discard_file() (not repeated here)
// Neither of the first two families has a fixed file list, so this module
// lists the owning directory with cfg_fs_list() and deletes every file whose
// name round-trips through the OWNER's path format. Files that do not match
// (hidden.json, any decoy) are left alone.
//
// When adding a dual-written item whose NVS partition is profiles_nvs, add it
// here; tests/test_zone_normals_cfg_fs.c checks the covered directories.
#ifndef PROFILES_SCOPE_CFG_FILES_H
#define PROFILES_SCOPE_CFG_FILES_H

#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The cfg_fs directories this module sweeps (static storage). */
const char *const *profiles_scope_cfg_files_dirs(size_t *out_count);

/* Best-effort cfg_fs_delete() of every profile-slot and firing-stats mirror
 * file. Absent files/directories and an unmounted cfg_fs are normal (not
 * errors). Every file is attempted even after a failure; returns the FIRST
 * hard error (a stale file that survives must fail the reset), ESP_OK
 * otherwise. *out_deleted (optional) receives the number of files removed. */
esp_err_t profiles_scope_cfg_files_delete(int *out_deleted);

#ifdef __cplusplus
}
#endif

#endif // PROFILES_SCOPE_CFG_FILES_H
