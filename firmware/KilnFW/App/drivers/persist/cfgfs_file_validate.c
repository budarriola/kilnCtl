#include "cfgfs_file_validate.h"

#include <string.h>

#include "persist_scratch.h"
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
    if (raw) {
        return CFGFS_FILE_CHECK_OK;
    }
    *reason = "no validator for this file; pass raw=1 to write it unchecked";
    return CFGFS_FILE_CHECK_NO_VALIDATOR;
}
