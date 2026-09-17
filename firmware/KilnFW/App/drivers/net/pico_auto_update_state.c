#include "pico_auto_update_state.h"

/* See pico_auto_update_state.h's top comment: this is a placeholder body
 * until docs/PICO_AUTO_UPDATE_PLAN.md's boot-time glue (step 3) replaces it
 * with the real verdict. Deliberately not `static const bool` -- step 3
 * needs a settable value, and giving this its own translation unit now
 * means that later change touches only this file, not any of its callers. */
bool pico_auto_update_state_is_blocking(void)
{
    return false;
}
