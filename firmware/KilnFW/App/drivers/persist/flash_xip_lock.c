/* Portable core of the flash mmap-reader vs SPI1 writer lock; see flash_xip_lock.h. */
#include "flash_xip_lock.h"

#include <stddef.h>

static const flash_xip_lock_ops_t *s_ops;

void flash_xip_lock_set_ops(const flash_xip_lock_ops_t *ops)
{
    s_ops = ops;
}

int flash_xip_lock_wrap_start(flash_xip_start_fn orig, void *arg, uint32_t flags)
{
    const flash_xip_lock_ops_t *ops = s_ops;
    if (ops) {
        ops->take();
    }
    int rc = orig(arg, flags);
    if (rc != 0 && ops) {
        /* IDF does not call end() after a failed start(): release here. */
        ops->give();
    }
    return rc;
}

int flash_xip_lock_wrap_end(flash_xip_end_fn orig, void *arg)
{
    const flash_xip_lock_ops_t *ops = s_ops;
    int rc = orig(arg);
    if (ops) {
        ops->give();
    }
    return rc;
}

void flash_xip_reader_enter(void)
{
    if (s_ops) {
        s_ops->take();
    }
}

void flash_xip_reader_exit(void)
{
    if (s_ops) {
        s_ops->give();
    }
}
