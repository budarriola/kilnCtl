// ui_lcd_lock -- LVGL glue for the LCD half of the inactivity lock and its
// 10-second stay-unlocked prompt (docs/WEB_AUTH_PLAN.md section 8, LCD half
// only -- the web GUI's own timeout/prompt belongs to whichever module owns
// app.js's session handling, not here). Sits on top of the pure
// lcd_lock_state_t machinery in lcd_auth_state.h/.c the same way
// ui_lcd_keypad.c sits on top of lcd_keypad_state_t: this file is thin,
// untested glue; the state transitions it drives are what
// App/test/test_lcd_auth_state.c exercises.
//
// THE SEAM: ui_lcd_lock_set_policy_fn() is the one call that swaps the
// "auth ships OFF" stub (lcd_enabled=false, so every gate below is a no-op
// and the keypad never appears) for the real auth_policy NVS record once
// item 11's store lands -- see this header's .c file for the default.
#ifndef UI_LCD_LOCK_H
#define UI_LCD_LOCK_H

#include <stdbool.h>
#include <stdint.h>

#include "lcd_auth_state.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool     enabled;    // auth_policy.lcd_enabled -- false is the shipped default (item 11)
    uint32_t timeout_s;  // auth_policy.lcd_timeout_s, LCD_LOCK_TIMEOUT_NEVER for "never"
} ui_lcd_lock_policy_t;

typedef ui_lcd_lock_policy_t (*ui_lcd_lock_policy_fn_t)(void);

// Installs the policy source. Passing NULL restores the built-in stub
// (enabled=false), matching the "auth ships OFF" default so a build with no
// credential/policy module wired in yet behaves exactly as it does today.
void ui_lcd_lock_set_policy_fn(ui_lcd_lock_policy_fn_t fn);

// One-time setup: creates the periodic tick timer (existing LVGL timer
// infrastructure, no new task -- see lcd_auth_state.h's lcd_lock_tick()
// comment) and initialises the lock in its locked state. Call once from the
// same place kiln_ui_init() wires up the rest of the UI.
void ui_lcd_lock_init(void);

// True iff a session is currently granted (i.e. NOT locked) with at least
// `role`-or-higher access. With the policy's enabled=false this always
// returns true -- "auth off collapses every tier to full access" (item 11)
// applies identically on the LCD.
bool ui_lcd_lock_has_role(lcd_pin_role_t role);

// Explicit teardown of the current LCD session, if any -- e.g. a policy
// write that just flipped lcd_enabled off->on (WEB_AUTH_PLAN.md section 11:
// "enabling auth clears every session"), called from
// security_backend_web_auth.c's set_policy path. A no-op if already locked.
// Equivalent in effect to letting the inactivity tick expire the session,
// exposed directly so a caller never has to fake a clock jump to get the
// same result.
void ui_lcd_lock_force_lock(void);

// Any touch delivered to LVGL counts as activity (section 8: "including a
// touch that only scrolls or is swallowed by a backdrop"). Wired once from
// lvgl_port.c's input read callback.
void ui_lcd_lock_note_activity(void);

// The gate itself. If the policy is disabled, or a session already covers
// `min_role`, calls `action(user_data)` immediately -- no keypad, no delay,
// so this can never be the thing that stands between an operator and a
// Stop (item 9: only ever wired to the Start half of the merged Start/Stop
// button, never to Stop). Otherwise shows the keypad overlay with `prompt`
// and calls `action(user_data)` only after a PIN is entered whose role meets
// or exceeds `min_role`; a wrong or insufficient PIN, lockout, or Cancel
// simply leaves the caller's action unexecuted.
typedef void (*ui_lcd_lock_gated_cb_t)(void *user_data);
void ui_lcd_lock_run_gated(const char *prompt, lcd_pin_role_t min_role, ui_lcd_lock_gated_cb_t action,
                            void *user_data);

#ifdef __cplusplus
}
#endif

#endif // UI_LCD_LOCK_H
