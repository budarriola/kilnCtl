// See profiles_scope_cfg_files.h for the rationale.
#include "profiles_scope_cfg_files.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"

#include "cfg_fs.h"
#include "firing_stats_cfg_fs.h"
#include "profiles_cfg_fs.h"

static const char *PSCF_TAG = "profiles_scope_cfg";

/* One entry per owner: the directory it writes into and its per-id path
 * format. The format is the owner's own macro, never re-spelled here. */
typedef struct {
    const char *dir;
    const char *path_fmt; /* "<dir>/<name containing %u>" */
} pscf_family_t;

static const pscf_family_t kFamilies[] = {
    { PROFILES_CFG_FS_DIR, PROFILES_CFG_FS_PATH_FMT },         /* profile slots */
    { FIRING_STATS_CFG_FS_DIR, FIRING_STATS_CFG_FS_PATH_FMT }, /* firing history */
};

static const char *const kDirs[] = { PROFILES_CFG_FS_DIR, FIRING_STATS_CFG_FS_DIR };

/* Entries fetched per cfg_fs_list() call (heap, not stack: this runs on the
 * flash worker). More than this in one directory just takes another pass. */
#define PSCF_LIST_MAX 128
#define PSCF_MAX_PASSES 8
#define PSCF_PATH_MAX (CFG_FS_MAX_NAME + 32)

const char *const *profiles_scope_cfg_files_dirs(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(kDirs) / sizeof(kDirs[0]);
    }
    return kDirs;
}

/* True if `name` (bare, inside fam->dir) is exactly what the owner's path
 * format produces for some id -- rejects hidden.json and anything foreign. */
static bool pscf_name_is_owned(const pscf_family_t *fam, const char *name)
{
    const char *digits = name;
    while (*digits && (*digits < '0' || *digits > '9')) {
        digits++;
    }
    if (!*digits) {
        return false;
    }
    unsigned long id = strtoul(digits, NULL, 10);
    char rebuilt[PSCF_PATH_MAX];
    char actual[PSCF_PATH_MAX];
    snprintf(rebuilt, sizeof(rebuilt), fam->path_fmt, (unsigned)id);
    snprintf(actual, sizeof(actual), "%s/%s", fam->dir, name);
    return strcmp(rebuilt, actual) == 0;
}

esp_err_t profiles_scope_cfg_files_delete(int *out_deleted)
{
    esp_err_t first_err = ESP_OK;
    int deleted = 0;
    cfg_fs_entry_t *ents = malloc(PSCF_LIST_MAX * sizeof(*ents));
    if (!ents) {
        if (out_deleted) {
            *out_deleted = 0;
        }
        return ESP_ERR_NO_MEM;
    }
    for (size_t f = 0; f < sizeof(kFamilies) / sizeof(kFamilies[0]); f++) {
        const pscf_family_t *fam = &kFamilies[f];
        for (int pass = 0; pass < PSCF_MAX_PASSES; pass++) {
            size_t n = 0;
            esp_err_t lerr = cfg_fs_list(fam->dir, ents, PSCF_LIST_MAX, &n);
            if (lerr == ESP_ERR_INVALID_STATE) {
                break; /* cfg_fs not mounted: no mirrors to delete */
            }
            if (lerr != ESP_OK) {
                ESP_LOGW(PSCF_TAG, "profiles reset: could not list cfg dir %s: %s", fam->dir,
                         esp_err_to_name(lerr));
                if (first_err == ESP_OK) {
                    first_err = lerr;
                }
                break;
            }
            int removed_this_pass = 0;
            for (size_t i = 0; i < n; i++) {
                if (!pscf_name_is_owned(fam, ents[i].name)) {
                    continue;
                }
                char path[PSCF_PATH_MAX];
                snprintf(path, sizeof(path), "%s/%s", fam->dir, ents[i].name);
                esp_err_t err = cfg_fs_delete(path);
                if (err == ESP_OK) {
                    deleted++;
                    removed_this_pass++;
                } else if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_STATE) {
                    ESP_LOGW(PSCF_TAG, "profiles reset: could not delete cfg file %s: %s", path,
                             esp_err_to_name(err));
                    if (first_err == ESP_OK) {
                        first_err = err;
                    }
                }
            }
            /* A full listing may have hidden more files behind the clamp; go
             * again only if this pass made progress. Running out of passes
             * with the listing still full means files may remain: fail the
             * reset loudly rather than leave a stale mirror behind. */
            if (n < PSCF_LIST_MAX || removed_this_pass == 0) {
                break;
            }
            if (pass == PSCF_MAX_PASSES - 1) {
                ESP_LOGE(PSCF_TAG, "profiles reset: cfg dir %s still full after %d passes; files may remain",
                         fam->dir, PSCF_MAX_PASSES);
                if (first_err == ESP_OK) {
                    first_err = ESP_ERR_INVALID_SIZE;
                }
            }
        }
    }
    free(ents);
    if (out_deleted) {
        *out_deleted = deleted;
    }
    return first_err;
}
