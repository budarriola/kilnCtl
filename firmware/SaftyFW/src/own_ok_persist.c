// own_ok_persist.c -- see own_ok_persist.h.
#include "own_ok_persist.h"

#include <stddef.h>

#include "hal_scratch.h"

#define OWN_OK_PERSIST_SCRATCH 5u

static bool s_restored;

uint32_t own_ok_persist_encode(bool on)
{
    return on ? (((uint32_t)OWN_OK_PERSIST_MAGIC_BYTE << 24) | 1u) : 0u;
}

bool own_ok_persist_decode(uint32_t word)
{
    return word == own_ok_persist_encode(true);
}

void own_ok_persist_boot_read(bool benign_boot)
{
    uint32_t raw = 0u;
    (void)hal_scratch_read_u32(OWN_OK_PERSIST_SCRATCH, &raw, OWN_OK_PERSIST_SCRATCH, 0u, NULL);
    s_restored = benign_boot && own_ok_persist_decode(raw);
}

bool own_ok_persist_restored(void)
{
    return s_restored;
}

void own_ok_persist_write(bool on)
{
    uint32_t raw = 0u;
    (void)hal_scratch_read_u32(OWN_OK_PERSIST_SCRATCH, &raw, OWN_OK_PERSIST_SCRATCH, 0u, NULL);
    if (on) {
        /* AT-20: never clobber a fatal tag (0xE3/0xB4/0xA5) or the 0xD9 overdue mark; write only
         * over an empty register. Already-set is a no-op, so the caller may re-assert every tick
         * (a recovered 0xD9 mark clears slot 5, which would otherwise drop the allowance). */
        if (raw == 0u) {
            (void)hal_scratch_write_u32(OWN_OK_PERSIST_SCRATCH, own_ok_persist_encode(true));
        }
        return;
    }
    /* AT-21: compare-and-clear. The read-then-write window is a few instructions; the only other
     * slot-5 writers are the watchdog overdue mark (other core) and the fatal hooks (raw MMIO,
     * cannot take a lock). A fatal tag landing inside the window could be erased; accepted. */
    if (own_ok_persist_decode(raw)) {
        (void)hal_scratch_write_u32(OWN_OK_PERSIST_SCRATCH, 0u);
    }
}
