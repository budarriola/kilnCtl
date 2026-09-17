// auth_reset_gesture_wiring.c -- the one place auth_reset_gesture.h's pure
// state machine is coupled to the real credential store
// (web_auth_store_clear_for_physical_reset(), firmware/KilnFW/App/drivers/
// persist/web_auth_store.h). Split into its own translation unit,
// deliberately separate from auth_reset_gesture.c, so that file stays free
// of any I/O dependency (no psa/crypto.h, no hal_kv.h) and keeps linking
// into every host-test executable that already includes it, including the
// large combined one that does not otherwise pull in psa/crypto.h's host
// stub (see App/test/build_host_tests.ps1's exe43 comment on
// test_web_auth_store.c for why that stub is kept isolated).
//
// Board-wide singleton: exactly one physical panel exists, so exactly one
// gesture state is needed. Not thread-safe by itself -- callers (the LCD
// PIN/keypad module that owns touch input, a separate slice of this plan)
// must only touch the returned pointer from the single UI/LVGL task, same
// assumption every other LVGL-adjacent module in this tree already makes.
#include "auth_reset_gesture.h"

#include "../persist/web_auth_store.h"

static auth_reset_gesture_state_t s_singleton;
static bool s_singleton_initialized;

auth_reset_gesture_state_t *auth_reset_gesture_singleton(void)
{
    if (!s_singleton_initialized) {
        auth_reset_gesture_reset(&s_singleton);
        s_singleton.clear_credentials_fn = web_auth_store_clear_for_physical_reset;
        s_singleton_initialized = true;
    }
    return &s_singleton;
}
