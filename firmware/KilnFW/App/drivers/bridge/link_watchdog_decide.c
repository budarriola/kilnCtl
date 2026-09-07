#include "link_watchdog_decide.h"

#include "relay_authority.h"

uint8_t link_watchdog_decide_unowned_mask(bool danger_mode_is_active)
{
    if (danger_mode_is_active) {
        return 0;
    }
    uint8_t unowned_mask = 0;
    for (uint8_t relay = 1; relay <= LINK_WATCHDOG_MAX_RELAYS; relay++) {
        if (!relay_authority_manual_blocked_by_owner(relay)) {
            unowned_mask |= (uint8_t)(1u << (relay - 1u));
        }
    }
    return unowned_mask;
}
