// tc_fault_state.c -- storage + lock-free seq-counter protocol for
// tc_fault_state.h. Mirrors sim_snapshot.h's torn-read-retry pattern
// exactly: RP2040 SMP cores are in-order with no data cache, so a volatile
// sequence counter plus a compiler memory-clobber barrier is sufficient for
// cross-core visibility here, same as it is for sim_snapshot -- no
// FreeRTOS mutex/semaphore needed, and so no init-order dependency on which
// task starts first (every static below is valid, zero-initialized "no
// fault" state before any task runs).
#include "tc_fault_state.h"

#include <string.h>

// Compiler-reordering barrier only (no CPU fence needed -- see file header:
// RP2040 SMP cores are in-order with no data cache, so preventing the
// *compiler* from reordering the plain struct copy around the volatile seq
// writes/reads is the whole job). GCC/Clang's empty asm-with-memory-clobber
// does that on the embedded target; MSVC has no inline-asm story on x64
// (cl.exe x64 rejects __asm entirely, unlike x86), so this pass's host-test
// build (test/build_host_tests.ps1, MSVC x64) needs its own equivalent --
// _ReadWriteBarrier() is exactly that on MSVC. Neither generates any actual
// instruction; both just pin the surrounding memory accesses in program
// order for the optimizer.
#if defined(_MSC_VER)
#include <intrin.h>
#define TC_FAULT_STATE_BARRIER() _ReadWriteBarrier()
#else
#define TC_FAULT_STATE_BARRIER() __asm volatile("" ::: "memory")
#endif

typedef struct {
    volatile uint32_t seq; /* even = stable, odd = write in progress */
    tc_fault_override_t override;
} tc_fault_slot_t;

static tc_fault_slot_t s_slots[TC_FAULT_CHANNEL_COUNT];

bool tc_fault_state_write(tc_fault_channel_t channel, const tc_fault_override_t *override)
{
    if ((unsigned)channel >= TC_FAULT_CHANNEL_COUNT || override == NULL) {
        return false;
    }

    tc_fault_slot_t *slot = &s_slots[channel];
    uint32_t seq = slot->seq;

    slot->seq = seq + 1u; /* odd: readers must retry */
    TC_FAULT_STATE_BARRIER();
    slot->override = *override;
    TC_FAULT_STATE_BARRIER();
    slot->seq = seq + 2u; /* even: publish complete */

    return true;
}

bool tc_fault_state_clear(tc_fault_channel_t channel)
{
    tc_fault_override_t none;
    memset(&none, 0, sizeof(none));
    return tc_fault_state_write(channel, &none);
}

bool tc_fault_state_read(tc_fault_channel_t channel, tc_fault_override_t *out_override)
{
    if ((unsigned)channel >= TC_FAULT_CHANNEL_COUNT || out_override == NULL) {
        return false;
    }

    tc_fault_slot_t *slot = &s_slots[channel];

    for (;;) {
        uint32_t seq_before = slot->seq;
        if (seq_before & 1u) {
            continue; /* writer in progress -- spin and retry */
        }
        TC_FAULT_STATE_BARRIER();
        tc_fault_override_t copy = slot->override;
        TC_FAULT_STATE_BARRIER();
        uint32_t seq_after = slot->seq;

        if (seq_after == seq_before) {
            *out_override = copy;
            return true;
        }
        /* torn read -- seq changed mid-copy, retry */
    }
}
