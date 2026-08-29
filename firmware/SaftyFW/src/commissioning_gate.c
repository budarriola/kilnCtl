// commissioning_gate.c -- see commissioning_gate.h.
#include "commissioning_gate.h"

#include "config_params.h" // config_params_all_required_set()

bool commissioning_gate_is_commissioned(const config_store_record_t *rec)
{
    if (!rec) {
        return false;
    }
    // Both facts, ANDed -- see the header's "What commissioned means here".
    // The stored verdict and the recomputed one must agree; a disagreement
    // in either direction lands on "not commissioned", never on "close
    // enough".
    if (rec->calibration_missing) {
        return false;
    }
    return config_params_all_required_set(rec);
}

bool commissioning_gate_energize_allowed(bool enable, const config_store_record_t *rec)
{
    if (!enable) {
        return true; // de-energizing is never gated
    }
    return commissioning_gate_is_commissioned(rec);
}
