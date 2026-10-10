#include "cfgfs_file_validate.h"

#include <string.h>

#include "adaptive_tune.h"
#include "aux_outputs_cfg.h"
#include "cfgfs_file_validators.h"
#include "ct_verify_store.h"
#include "display_power_cfg.h"
#include "iter_tune_store.h"
#include "persist_scratch.h"
#include "profiles_builtin.h"
#include "profiles_favorites.h"
#include "ramp_assist_cfg.h"
#include "relay_cycles.h"
#include "setup_wizard_progress.h"
#include "time_sync.h"
#include "update_settings.h"
#include "zones_config_cfg_fs.h" /* ZONES_CFG_FILE_PATH */
#include "zones_config_json.h"

/* zones.json: 4-byte LE rev + the versioned blob, decoded by the very
 * function zones_config_cfg_fs.c's loader uses (a NEWER or CORRUPT result
 * there makes the loader ignore the file, so it is refused here too). */
static cfgfs_file_check_t check_zones(const uint8_t *body, size_t len, const char **reason)
{
    if (len < 5) {
        *reason = "zones.json too short";
        return CFGFS_FILE_CHECK_INVALID;
    }
    zones_cfg_t *cfg = (zones_cfg_t *)persist_scratch_alloc(sizeof(*cfg));
    if (!cfg) {
        *reason = "out of memory";
        return CFGFS_FILE_CHECK_OOM;
    }
    const char *why = "";
    zones_decode_result_t r = zones_config_json_decode_blob(body + 4, len - 4, cfg, &why);
    free(cfg);
    if (r == ZONES_DECODE_OK) {
        return CFGFS_FILE_CHECK_OK;
    }
    if (r == ZONES_DECODE_OOM) {
        *reason = "out of memory";
        return CFGFS_FILE_CHECK_OOM;
    }
    *reason = (r == ZONES_DECODE_NEWER) ? "zones.json is a newer firmware version" : "zones.json fails validation";
    return CFGFS_FILE_CHECK_INVALID;
}

/* aux_out.dat for the POST rule only: the loader's validator accepts a NEWER
 * version (so start() can quarantine it), but writing one over HTTP would
 * quarantine aux at the next boot, so refuse it here. Byte 0 of the item is
 * the blob version. */
static bool aux_outputs_post_validate(const void *bytes, size_t len)
{
    if (len < 1 || ((const uint8_t *)bytes)[0] > AUX_OUTPUTS_CFG_VERSION) {
        return false;
    }
    return aux_outputs_cfg_file_validate(bytes, len);
}

/* Subdirectory files (profiles/hidden.json) are out of scope: cfgfs_file_name_get
 * refuses '/', so POST /api/cfgfs/file can never address them.
 *
 * Flat pref files: 4-byte LE rev + item, judged by the validator the owning
 * module hands pref_cfg_fs_load_raw() (which itself rejects a wrong size). */
typedef struct {
    const char *name;
    bool (*validate)(const void *bytes, size_t len);
    const char *reason;
} pref_file_rule_t;

static const pref_file_rule_t PREF_FILE_RULES[] = {
    { ADAPTIVE_TUNE_KIBASE_FILE_PATH, adaptive_tune_kibase_file_validate, "ki_base.dat fails validation" },
    { RAMP_ASSIST_FILE_PATH, ramp_assist_cfg_file_validate, "ramp_assist.dat fails validation" },
    { TIME_SYNC_TZ_FILE_PATH, time_sync_tz_file_validate, "tz.dat fails validation" },
    { AUX_OUTPUTS_FILE_PATH, aux_outputs_post_validate, "aux_out.dat fails validation" },
    { DISPLAY_POWER_FILE_PATH, display_power_cfg_file_validate, "display_power.dat fails validation" },
    { PROFILES_FAVORITES_FILE_PATH, profiles_favorites_file_validate, "prof_fav.bin fails validation" },
    { RELAY_CYCLES_FILE_PATH, relay_cycles_file_validate, "relay_cycles.dat fails validation" },
    { SETUP_WIZARD_PROGRESS_FILE_PATH, setup_wizard_progress_file_validate, "setup_wiz.bin fails validation" },
    { UPDATE_SETTINGS_FILE_PATH, update_settings_file_validate, "update_repo.dat fails validation" },
    { CT_VERIFY_CFG_FILE_PATH, ct_verify_blob_validate, "ct_verify.bin fails validation" },
    { ITER_TUNE_CFG_FILE_PATH, iter_tune_store_blob_validate, "iter_tune.bin fails validation" },
};

cfgfs_file_check_t cfgfs_file_check_write(const char *name, bool raw, const void *body, size_t len,
                                          const char **reason)
{
    const char *dummy = "";
    if (!reason) {
        reason = &dummy;
    }
    if (strcmp(name, ZONES_CFG_FILE_PATH) == 0) {
        /* Validated even with raw=1: raw only waives the missing-validator refusal. */
        return check_zones((const uint8_t *)body, len, reason);
    }
    for (size_t i = 0; i < sizeof(PREF_FILE_RULES) / sizeof(PREF_FILE_RULES[0]); i++) {
        if (strcmp(name, PREF_FILE_RULES[i].name) == 0) {
            /* raw=1 never waives a file that has a validator. */
            if (len < 4 || !PREF_FILE_RULES[i].validate((const uint8_t *)body + 4, len - 4)) {
                *reason = PREF_FILE_RULES[i].reason;
                return CFGFS_FILE_CHECK_INVALID;
            }
            return CFGFS_FILE_CHECK_OK;
        }
    }
    if (raw) {
        return CFGFS_FILE_CHECK_OK;
    }
    *reason = "no validator for this file; pass raw=1 to write it unchecked";
    return CFGFS_FILE_CHECK_NO_VALIDATOR;
}
