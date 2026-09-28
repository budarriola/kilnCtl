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
// `min_role`, calls `action(user_data)` immediately -- no keypad, no delay.
// Otherwise shows the keypad overlay with `prompt` and calls
// `action(user_data)` only after a PIN is entered whose role meets or
// exceeds `min_role`; a wrong or insufficient PIN, lockout, or Cancel simply
// leaves the caller's action unexecuted.
//
// **Owner decision, 2026-09-28: this IS now wired to the Stop half of the
// merged Start/Stop button, reversing the rule this comment used to state
// here** ("only ever wired to the Start half ... never to Stop", item 9).
// The owner's own words: "stop needs login. there is an estop button" -- the
// LCD PIN surface is no longer treated as the thing standing between an
// operator and stopping a firing, because a hardware E-stop
// (docs/SAFETY_CASE.md H7, firmware-mediated on this bench per that doc) is
// the actual safety backstop, independent of this keypad and of LCD auth
// state entirely. Only a logged-in operator (or a board with LCD auth
// disabled -- see ui_lcd_lock_has_role()'s item-11 collapse) may now press
// Stop from the LCD; ui_home_fire_btn_cb() (ui_page_home_actions.c) gates
// both halves of the merged button, and
// tools/check_stop_path_requires_pin.ps1 enforces this mechanically (the
// check this comment used to name, tools/check_stop_path_never_gated.ps1,
// asserted the opposite rule and has been renamed/inverted to match). Every
// OTHER LCD page and action -- not only Start/Stop -- is also gated per that
// same 2026-09-28 decision when navigated to from the home/dashboard page:
// see ui_page_home_actions.c's ui_home_menu_nav_cb()/ui_home_profile_btn_cb()
// for the two gated exits off "home", and kiln_ui.c's page-registry comment
// for the short list of what deliberately stays reachable without a PIN
// (the physical credential-reset gesture on the home page's corner taps,
// which never navigates through this gate at all; and whatever a board
// needs before any PIN has ever been set, which the item-11 collapse above
// already covers automatically, since `enabled` only ever becomes true once
// a PIN is configured). The existing confirm dialog (ui_confirm.h's
// "Confirm Stop") is unchanged -- it still runs AFTER the PIN, exactly as it
// already did for Start.
typedef void (*ui_lcd_lock_gated_cb_t)(void *user_data);
void ui_lcd_lock_run_gated(const char *prompt, lcd_pin_role_t min_role, ui_lcd_lock_gated_cb_t action,
                            void *user_data);

// Installs a callback the lock calls right after the inactivity timeout
// re-locks the session (LCD_LOCK_TICK_EXPIRED, ui_lcd_lock.c's
// tick_timer_cb()) -- never on a policy-transition force-lock, which already
// happens on whatever page the operator is looking at and does not need a
// forced navigation. docs/WEB_AUTH_PLAN.md section 8 says locking "returns
// the interface to the Dashboard"; before page-level gating (2026-09-28
// owner decision, see ui_lcd_lock_run_gated()'s comment above) that was true
// for the web GUI but not enforced on the LCD, since only the Start button
// itself was ever gated and an expired session simply meant the NEXT tap on
// Start would re-prompt from wherever the operator already was. Now that
// entire pages are gated at the two exits off "home"
// (ui_page_home_actions.c's ui_home_menu_nav_cb()/ui_home_profile_btn_cb()),
// a session that expires while the operator is several pages deep in the
// Config hub would otherwise leave every button on THAT page usable with no
// fresh PIN demanded until they navigated back out and back in -- so this
// seam forces a return to "home" on expiry instead, making the single
// dashboard-exit gate sufficient rather than requiring a gate on every
// individual page transition. Passing NULL (the default) restores today's
// current-page-only behaviour, which is what every host test and the
// LVGL-free build gets since kiln_ui.c (the only real caller) is not linked
// there.
typedef void (*ui_lcd_lock_relock_cb_t)(void);
void ui_lcd_lock_set_relock_cb(ui_lcd_lock_relock_cb_t fn);

#ifdef __cplusplus
}
#endif

#endif // UI_LCD_LOCK_H
