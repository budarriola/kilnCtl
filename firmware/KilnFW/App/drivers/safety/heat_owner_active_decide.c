#include "heat_owner_active_decide.h"

bool heat_owner_active_decide(profile_exec_state_t profile_state, bool profile_claimant_held,
                               bool autotune_claimant_held, bool danger_mode_is_active)
{
    return profile_state == PROFILE_EXEC_RUNNING || profile_state == PROFILE_EXEC_PAUSED ||
           profile_claimant_held || autotune_claimant_held || danger_mode_is_active;
}
