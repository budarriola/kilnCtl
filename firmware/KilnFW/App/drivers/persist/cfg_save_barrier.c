#include "cfg_save_barrier.h"

#include "cfg_save_lock.h"
#include "pref_cfg_fs.h"

void persist_reset_barrier(void)
{
    size_t n = pref_cfg_fs_lock_registry_count();
    for (size_t i = 0; i < n; i++) {
        cfg_save_lock_t *l = (cfg_save_lock_t *)pref_cfg_fs_lock_registry_get(i);
        if (l == NULL) {
            continue;
        }
        cfg_save_lock_take(l);
        cfg_save_lock_give(l);
    }
}
