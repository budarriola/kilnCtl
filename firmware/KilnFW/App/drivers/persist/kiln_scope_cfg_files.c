// See kiln_scope_cfg_files.h for the rationale.
#include "kiln_scope_cfg_files.h"

#include "esp_log.h"

#include "adaptive_tune.h"
#include "aux_outputs_cfg.h"
#include "cfg_fs.h"
#include "ct_verify_store.h"
#include "display_power_cfg.h"
#include "iter_tune_store.h"
#include "kiln_cfg_store_cfg_fs.h"
#include "ramp_assist_cfg.h"
#include "relay_cycles.h"
#include "setup_wizard_progress.h"
#include "time_sync.h"
#include "unit_pref.h"
#include "update_settings.h"
#include "zones_config_cfg_fs.h"
#include "zones_http_internal.h"

static const char *KSCF_TAG = "kiln_scope_cfg";

static const char *const kKilnScopeFiles[] = {
    ZONES_CFG_FILE_PATH,             /* zones_cfg blob (zones_config_cfg_fs) */
    KILN_CFG_STORE_FILE_PATH,        /* kiln config slots */
    RELAY_NAMES_FILE_PATH,           /* relay names */
    ZONE_NORMALS_FILE_PATH,          /* zone normals */
    RELAY_CYCLES_FILE_PATH,          /* relay cycle counters */
    RAMP_ASSIST_FILE_PATH,           /* ramp assist preference */
    UNIT_PREF_FILE_PATH,             /* temperature unit preference */
    DISPLAY_POWER_FILE_PATH,         /* display power policy */
    TIME_SYNC_TZ_FILE_PATH,          /* timezone */
    ADAPTIVE_TUNE_KIBASE_FILE_PATH,  /* adaptive-tune Ki baseline (ADAPTIVE_TUNE_NVS_PARTITION == kiln_nvs) */
    ITER_TUNE_CFG_FILE_PATH,         /* iterative-tune store */
    AUX_OUTPUTS_FILE_PATH,           /* spare-relay aux outputs */
    UPDATE_SETTINGS_FILE_PATH,       /* update repo preference */
    CT_VERIFY_CFG_FILE_PATH,         /* CT verify store (cfg only since 2026-10-07) */
    SETUP_WIZARD_PROGRESS_FILE_PATH, /* setup wizard progress (cfg only since 2026-10-07) */
};

const char *const *kiln_scope_cfg_files_list(size_t *out_count)
{
    if (out_count) {
        *out_count = sizeof(kKilnScopeFiles) / sizeof(kKilnScopeFiles[0]);
    }
    return kKilnScopeFiles;
}

esp_err_t kiln_scope_cfg_files_delete(int *out_deleted)
{
    esp_err_t first_err = ESP_OK;
    int deleted = 0;
    size_t n = 0;
    const char *const *paths = kiln_scope_cfg_files_list(&n);
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = cfg_fs_delete(paths[i]);
        if (err == ESP_OK) {
            deleted++;
        } else if (err != ESP_ERR_NOT_FOUND && err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(KSCF_TAG, "kiln reset: could not delete cfg file %s: %s", paths[i], esp_err_to_name(err));
            if (first_err == ESP_OK) {
                first_err = err;
            }
        }
    }
    if (out_deleted) {
        *out_deleted = deleted;
    }
    return first_err;
}
