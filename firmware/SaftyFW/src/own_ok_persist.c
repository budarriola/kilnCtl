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
    if (on) {
        (void)hal_scratch_write_u32(OWN_OK_PERSIST_SCRATCH, own_ok_persist_encode(true));
        return;
    }
    uint32_t raw = 0u;
    (void)hal_scratch_read_u32(OWN_OK_PERSIST_SCRATCH, &raw, OWN_OK_PERSIST_SCRATCH, 0u, NULL);
    if (own_ok_persist_decode(raw)) {
        (void)hal_scratch_write_u32(OWN_OK_PERSIST_SCRATCH, 0u);
    }
}
