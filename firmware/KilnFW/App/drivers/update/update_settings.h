// update_settings -- the persisted "update repo" preference of the GitHub
// release update feature (docs/GITHUB_RELEASE_UPDATE_PLAN.md section 9, WP9).
//
// The setting is ONE string, `owner/name`, naming the GitHub repository the
// board fetches releases from. Only owner/name is stored, never a URL, so the
// fetcher (WP8) can only build requests against github.com for that repo.
//
// DEFAULT: UPDATE_SETTINGS_DEFAULT_REPO is compiled in. An unset setting
// (fresh board, no valid stored value, or an operator reset) reads as the
// default. A non-default repo needs no extra confirm (owner decision D5).
//
// PERSISTENCE: same shape as display_power_cfg.c -- one small versioned blob
// in kiln_nvs/kiln_cfg (NVS authoritative), dual-written to the cfg LittleFS
// file update_repo.dat through pref_cfg_fs.c (higher rev wins). The file is
// also named in kiln_scope_cfg_files.c.
//
// RAM: two 64-byte buffers. The accessor returns a pointer into the live one;
// a set writes the other buffer and then flips an index, so a reader on
// another task never sees a half-written string.
#ifndef UPDATE_SETTINGS_H
#define UPDATE_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define UPDATE_SETTINGS_DEFAULT_REPO "budarriola/kilnCtl"

// Longest accepted "owner/name" (characters, NUL excluded). GitHub allows a
// 39-character owner and 100-character name; this firmware caps the whole
// string at 62 to keep the RAM and the stored blob at 64 bytes.
#define UPDATE_SETTINGS_REPO_MAX_LEN 62

// cfg_fs relative path of this item's dual-write mirror (NVS side: kiln_nvs).
#define UPDATE_SETTINGS_FILE_PATH "update_repo.dat"

// Pure validator. true iff `repo` is a well-formed "owner/name":
//   3..UPDATE_SETTINGS_REPO_MAX_LEN characters, exactly one '/';
//   owner 1..39 chars of [A-Za-z0-9._-], not starting or ending with '-';
//   name 1..100 chars of [A-Za-z0-9._-]; no ".." anywhere.
// NULL and "" are invalid here ("" is a reset spelling of update_settings_set()).
bool update_settings_repo_is_valid(const char *repo);

// Loads the persisted value or leaves the default. Non-fatal, idempotent;
// call once at boot before any reader.
esp_err_t update_settings_start(void);

// The repo to fetch from: stored value, or the default when unset. Never NULL,
// always a valid "owner/name", O(1), no NVS access, safe from any task.
const char *update_settings_repo(void);

// true when update_settings_repo() is the compiled-in default.
bool update_settings_repo_is_default(void);

// Validates and persists `repo`. NULL or "" resets to the default. Invalid
// input returns ESP_ERR_INVALID_ARG with nothing changed. The in-RAM value
// updates first, so a persistence failure (returned as the error) means the
// choice will not survive a reboot, not that it failed to apply now.
esp_err_t update_settings_set(const char *repo);

// Test-only: forget the in-RAM value as a power cycle would (NVS/file stay).
void update_settings_reset_ram_for_test(void);

#ifdef __cplusplus
}
#endif

#endif // UPDATE_SETTINGS_H
