// recovery_hold.h -- pure decision logic for the periodic relay-hold watchdog
// in recovery_io.c. No ESP-IDF dependency, so it is host-testable
// (test_recovery_hold.c).
//
// recovery_io_hold_relays_off() only verifies the SX1509 relay hold once at
// boot. If the expander browns out or resets later (the exact class of fault a
// crash-looping app can leave behind), every relay pin reverts to a floating
// input and nothing in the image notices. A 1 s task reads the registers back
// and re-asserts the hold; this module decides what each observation means.
//
// A mismatch LATCHES `fault` (with the time it first happened) and stays set
// until reboot even if the re-assert succeeds: a hold that needed repair is
// something the operator should see, not something to quietly forget.
#ifndef RECOVERY_HOLD_H
#define RECOVERY_HOLD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool fault;                 // latched: a mismatch or an unreadable expander was seen
    bool fault_seen_s_valid;    // fault_s is meaningful
    uint32_t fault_s;           // uptime seconds of the FIRST mismatch
    bool ever_ok;               // last_ok_s is meaningful
    uint32_t last_ok_s;         // uptime seconds of the last verified-good observation
    uint32_t mismatch_count;    // observations that did not match (or could not be read)
    uint32_t reassert_fail_count; // re-asserts whose read-back still did not verify
} rhold_state_t;

typedef enum {
    RHOLD_HEALTHY = 0,  // registers match the hold; nothing to do
    RHOLD_REASSERT = 1, // mismatch or unreadable: rewrite the hold and verify
} rhold_action_t;

void rhold_init(rhold_state_t *st);

// True when RegDir shows every `out_dir_mask` pin as an output (bit clear) and
// RegData shows every `hold_mask` pin low.
bool rhold_regs_match(uint16_t dir, uint16_t data, uint16_t out_dir_mask, uint16_t hold_mask);

// Feed one periodic read. `read_ok` false (I2C error) is a mismatch: an
// expander that cannot be read cannot be shown to be holding.
rhold_action_t rhold_observe(rhold_state_t *st, bool read_ok, uint16_t dir, uint16_t data,
                             uint16_t out_dir_mask, uint16_t hold_mask, uint32_t now_s);

// Feed the outcome of the re-assert: `verified` is true only when the write
// succeeded AND a fresh read-back matched. Never clears the latched fault.
void rhold_reassert_result(rhold_state_t *st, bool verified, uint32_t now_s);

// The relay fault reported to the status route and LCD: the boot-time fault OR a
// latched hold-watchdog fault.
bool rhold_effective_fault(bool boot_fault, const rhold_state_t *st);

// Relays count as verified off only if the boot verification succeeded AND no
// hold fault has latched since.
bool rhold_effective_verified_off(bool boot_verified, const rhold_state_t *st);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_HOLD_H
