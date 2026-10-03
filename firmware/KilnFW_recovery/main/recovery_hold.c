// recovery_hold.c -- see recovery_hold.h.
#include "recovery_hold.h"

void rhold_init(rhold_state_t *st)
{
    st->fault = false;
    st->fault_seen_s_valid = false;
    st->fault_s = 0;
    st->ever_ok = false;
    st->last_ok_s = 0;
    st->mismatch_count = 0;
    st->reassert_fail_count = 0;
}

bool rhold_regs_match(uint16_t dir, uint16_t data, uint16_t out_dir_mask, uint16_t hold_mask)
{
    bool dir_ok = (uint16_t)(dir & out_dir_mask) == 0; // output pins: direction bit clear
    bool data_ok = (uint16_t)(data & hold_mask) == 0;  // held pins read low
    return dir_ok && data_ok;
}

rhold_action_t rhold_observe(rhold_state_t *st, bool read_ok, uint16_t dir, uint16_t data,
                             uint16_t out_dir_mask, uint16_t hold_mask, uint32_t now_s)
{
    if (read_ok && rhold_regs_match(dir, data, out_dir_mask, hold_mask)) {
        st->ever_ok = true;
        st->last_ok_s = now_s;
        return RHOLD_HEALTHY;
    }
    st->mismatch_count++;
    if (!st->fault) {
        st->fault = true;
        st->fault_seen_s_valid = true;
        st->fault_s = now_s;
    }
    return RHOLD_REASSERT;
}

void rhold_reassert_result(rhold_state_t *st, bool verified, uint32_t now_s)
{
    if (verified) {
        st->ever_ok = true;
        st->last_ok_s = now_s;
    } else {
        st->reassert_fail_count++;
    }
    // st->fault is deliberately left as-is: it latches until reboot.
}
