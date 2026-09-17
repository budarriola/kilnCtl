// ui_lcd_keypad -- the LCD PIN-entry overlay (docs/WEB_AUTH_PLAN.md section
// 7). Thin LVGL glue over the pure state machine in lcd_auth_state.h/.c --
// this file owns no logic worth a host test itself (no branch here decides
// whether a PIN is right, whether entry can submit, or whether a lockout is
// active; lcd_auth_state.c decides all of that and is what the host tests in
// App/test/test_lcd_auth_state.c exercise).
//
// Built exactly like ui_confirm.c / ui_num_pad.c: lv_msgbox_create(NULL),
// which LVGL auto-parents onto lv_layer_top(), so this modal takes no part
// in any page's flex column and consumes none of the 480x320 no-scroll
// content budget (see ui_confirm.h's header comment for the fuller
// rationale, and ui_page_home.c's FLEX TRAP comment for why that distinction
// matters on this codebase specifically). kiln_ui.c's log_all_tap_targets()
// already walks lv_layer_top() for exactly this reason, so this overlay's
// keys are discoverable by UI_TEST_CMD_LIST_TAP_TARGETS / kiln_ui_click_by_name()
// with no changes needed there.
#ifndef UI_LCD_KEYPAD_H
#define UI_LCD_KEYPAD_H

#include <stdbool.h>

#include "lvgl.h"

#include "lcd_auth_state.h"

#ifdef __cplusplus
extern "C" {
#endif

// Called once, after the overlay has closed. `granted` is false on Cancel,
// a backdrop tap, or the lock timeout expiring while the keypad was open;
// true on a correct PIN, with `role` telling which PIN was entered ("the PIN
// itself is the role selector" -- section 7). role is LCD_PIN_ROLE_NONE when
// granted is false.
typedef void (*ui_lcd_keypad_done_cb_t)(bool granted, lcd_pin_role_t role, void *user_data);

// Builds (first call) or reconfigures (later calls) the singleton overlay
// and shows it. `prompt` is the caption line (e.g. "Enter PIN to continue").
// Only one instance is ever needed, same convention as ui_confirm.c /
// ui_num_pad.c -- nothing in this codebase shows two of these at once.
void ui_lcd_keypad_show(const char *prompt, ui_lcd_keypad_done_cb_t on_done, void *user_data);

// True while the overlay is currently visible. Used by the lock-tick glue
// (ui_lcd_lock.c) to decide whether an expiring lock should also dismiss an
// open keypad rather than leave it stranded on screen behind a Dashboard the
// tick already returned to.
bool ui_lcd_keypad_is_open(void);

// Force-closes the overlay with granted=false, as if Cancel had been
// pressed. Used when the lock timeout expires while the keypad is open, and
// safe to call when it is already closed (a no-op).
void ui_lcd_keypad_force_close(void);

#ifdef __cplusplus
}
#endif

#endif // UI_LCD_KEYPAD_H
