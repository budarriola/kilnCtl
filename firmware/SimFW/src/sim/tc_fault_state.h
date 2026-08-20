// tc_fault_state -- shared contract between fault_sched (sole writer) and
// spi_emu_a/b (readers) for TC/sensor-fault corruption state, PLAN.md
// section 7's fault-injection framework applied to the "Thermocouple /
// sensor faults" catalog (section 7.1).
//
// Why this header exists (see fault_sched.c's own header comment for the
// full reasoning): firing a TC/sensor fault means "flipping the target's
// override in the owning module" (PLAN.md 7.3) -- but the owning module for
// TC corruption is max31856_regs.c's per-channel max31856_channel_t, which
// lives inside spi_emu_a/b, a module fault_sched must never touch directly
// (single-owner-per-peripheral doctrine, PLAN.md section 4). This header is
// the minimal, independently-host-testable interface both sides agree on --
// fault_sched writes, spi_emu_a/b reads, exactly the same shape sim_snapshot.h
// already established for sim_engine/its readers.
//
// Ownership: fault_sched.c is the ONLY writer (tc_fault_state_write/_clear).
// spi_emu_a.c (channels MAIN_0..MAIN_2) and spi_emu_b.c (channel SAFETY) are
// the intended readers, called from each owner's own task context -- the
// slow write-back half of their loop (PLAN.md 3.2.1: "the spi_emu_* task
// drains [RX FIFO writes]... after CS-rise... and raises a
// master_config_changed event"), never from PIO/ISR context, since this
// module is not designed for sub-microsecond latency. The expected call
// site is immediately before max31856_regs_advance_conversion(): read this
// channel's tc_fault_override_t, copy .corruption into the channel's
// max31856_channel_t.corruption field (max31856_regs.h's own corruption
// struct, reused verbatim here -- no re-invention), call
// advance_conversion(), then OR .force_sr_bits into the channel's
// regs[MAX31856_REG_SR] byte (see below for why that second step is a
// separate field instead of living inside max31856_corruption_t).
//
// Storage/locking: tc_fault_state.c implements a lock-free, sequence-counter
// torn-read-retry protocol per channel -- the exact same discipline
// sim_snapshot.h documents and sim_engine.c implements for the zone
// snapshot (RP2040 SMP: in-order cores, no data cache, so a volatile seq
// counter + retry is sufficient with no FreeRTOS primitives at all). This
// header declares the contract only; storage is private to the .c file.
#ifndef SIMFW_SIM_TC_FAULT_STATE_H
#define SIMFW_SIM_TC_FAULT_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "max31856_regs.h" /* max31856_corruption_t, MAX31856_FAULT_* bits */

#ifdef __cplusplus
extern "C" {
#endif

/* One entry per physical MAX31856 channel the fixture emulates (PLAN.md
 * section 1/3.2: three main-side channels behind J6, CS0/CS1/CS2, one
 * safety-side channel behind J7). MAIN_0/1/2 index-match the natural
 * CS0/CS1/CS2 <-> zone0/1/2 mapping sim_engine.c documents and assumes for
 * its own relay-to-zone wiring -- see that file's header comment, section
 * "zone-to-relay/TC-channel mapping assumption". */
typedef enum {
    TC_FAULT_CHANNEL_MAIN_0 = 0, /* ESP bus (spi_emu_a), CS0 */
    TC_FAULT_CHANNEL_MAIN_1 = 1, /* ESP bus (spi_emu_a), CS1 */
    TC_FAULT_CHANNEL_MAIN_2 = 2, /* ESP bus (spi_emu_a), CS2 */
    TC_FAULT_CHANNEL_SAFETY = 3, /* safety bus (spi_emu_b), CS0 */
    TC_FAULT_CHANNEL_COUNT
} tc_fault_channel_t;

/* Corruption state for one channel. `corruption` is max31856_regs.h's own
 * struct, applied by the reader exactly as that module's doc comment
 * describes (assign into max31856_channel_t.corruption before calling
 * advance_conversion()).
 *
 * `force_sr_bits` exists because max31856_regs.c's own advance_conversion()
 * comment says OPEN/OVUV "this module has no independent electrical-fault
 * input -- fault_engine (or a direct test) sets those bits by driving SR
 * through corruption/override at a higher layer than 'compare temp to
 * threshold'. Nothing here ever sets them." That "higher layer" is exactly
 * this struct: the reader ORs force_sr_bits (MAX31856_FAULT_OPEN /
 * MAX31856_FAULT_OVUV from max31856_regs.h) into the channel's
 * regs[MAX31856_REG_SR] immediately after advance_conversion() returns.
 * Comparator-mode conversions overwrite SR wholesale each call
 * (advance_conversion() sets `ch->regs[MAX31856_REG_SR] = computed_sr`), so
 * this OR must be re-applied every conversion -- it is not a one-shot
 * latch; a caller in interrupt mode gets the same result since ORing an
 * already-set bit is a no-op. Only OPEN and OVUV are meaningful here (every
 * other SR bit is threshold-derived and already handled inside
 * max31856_regs.c itself); a reader should mask force_sr_bits to
 * (MAX31856_FAULT_OPEN | MAX31856_FAULT_OVUV) defensively. */
typedef struct {
    max31856_corruption_t corruption;
    uint8_t force_sr_bits;
} tc_fault_override_t;

/* Sole writer: fault_sched.c. Overwrites channel's entire override state
 * (fault_sched recomputes it from scratch every tick from the active fault
 * slot set -- see fault_sched.c's header comment for why "recompute, don't
 * patch" is this pass's composition strategy). Returns false only for an
 * out-of-range channel or a NULL pointer. */
bool tc_fault_state_write(tc_fault_channel_t channel, const tc_fault_override_t *override);

/* Resets one channel to "no fault" (all-zero override, MAX31856_DEAD_NONE,
 * force_sr_bits == 0). Equivalent to tc_fault_state_write() with a
 * zeroed struct -- provided as its own entry point so a caller's intent
 * ("clear this channel") reads clearly at the call site. */
bool tc_fault_state_clear(tc_fault_channel_t channel);

/* Reader API -- safe to call from any task on either core (spi_emu_a/b's own
 * task context, per this header's ownership note above). Returns false only
 * for an out-of-range channel or a NULL pointer; a channel that has never
 * been written reads back as all-zero/no-fault (statics zero-init to a seq
 * of 0, which is even -- a valid "nothing published yet, treat as no
 * fault" state, so callers do not need a separate "has fault_sched run yet"
 * check). */
bool tc_fault_state_read(tc_fault_channel_t channel, tc_fault_override_t *out_override);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_SIM_TC_FAULT_STATE_H
