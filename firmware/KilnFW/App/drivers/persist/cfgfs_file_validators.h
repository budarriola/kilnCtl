// cfgfs_file_validators -- the load-path validators of the flat cfg files,
// exposed so POST /api/cfgfs/file (cfgfs_file_validate.c) judges a body with
// the very function each module's loader passes to pref_cfg_fs_load_raw().
// Each takes the item bytes AFTER the 4-byte rev prefix and checks its own
// exact size. Definitions live in the owning module, not here.
#ifndef CFGFS_FILE_VALIDATORS_H
#define CFGFS_FILE_VALIDATORS_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool adaptive_tune_kibase_file_validate(const void *bytes, size_t len); /* ki_base.dat */
bool ramp_assist_cfg_file_validate(const void *bytes, size_t len);      /* ramp_assist.dat */
bool time_sync_tz_file_validate(const void *bytes, size_t len);         /* tz.dat */
bool aux_outputs_cfg_file_validate(const void *bytes, size_t len);      /* aux_out.dat */
bool display_power_cfg_file_validate(const void *bytes, size_t len);    /* display_power.dat */
bool profiles_builtin_hidden_file_validate(const void *bytes, size_t len); /* profiles/hidden.json */
bool profiles_favorites_file_validate(const void *bytes, size_t len);   /* prof_fav.bin */
bool relay_cycles_file_validate(const void *bytes, size_t len);         /* relay_cycles.dat */
bool setup_wizard_progress_file_validate(const void *bytes, size_t len); /* setup_wiz.bin */
bool update_settings_file_validate(const void *bytes, size_t len);      /* update_repo.dat */
/* ct_verify.bin: ct_verify_blob_validate() (ct_verify_store.h);
 * iter_tune.bin: iter_tune_store_blob_validate() (iter_tune_store.h). */

#ifdef __cplusplus
}
#endif

#endif
