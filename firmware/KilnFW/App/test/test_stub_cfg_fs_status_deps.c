/* Link stub for host-test executables that link cfg_fs_status.c but never
 * mount cfg: cfg_fs_get_status()/cfg_fs_list() report an unmounted store. */
#include "cfg_fs.h"
cfg_fs_status_t cfg_fs_get_status(void) { return CFG_FS_STATUS_UNMOUNTED; }
esp_err_t cfg_fs_list(const char *rel_dir, cfg_fs_entry_t *out, size_t max_out, size_t *out_count)
{
    (void)rel_dir; (void)out; (void)max_out;
    if (out_count) { *out_count = 0; }
    return ESP_ERR_INVALID_STATE;
}
