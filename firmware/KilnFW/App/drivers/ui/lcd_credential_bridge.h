// lcd_credential_bridge.h -- wires the LCD keypad/lock seams
// (lcd_auth_state.h's lcd_auth_state_set_verify_fn(), ui_lcd_lock.h's
// ui_lcd_lock_set_policy_fn()) to the real credential store
// (persist/web_auth_store.h, docs/WEB_AUTH_PLAN.md sections 2/3/11) now that
// it has landed. Deliberately a separate, tiny file rather than folding this
// into ui_lcd_lock.c: it is the one place in the LCD auth slice that reaches
// into persist/, so a future audit of "what touches web_auth_store.h" finds
// exactly one small file, not scattered #includes across the UI glue.
//
// Not host-tested itself (thin glue over two already-tested modules, same
// convention as ui_lcd_keypad.c/ui_lcd_lock.c) -- what IS tested is that
// lcd_auth_state.c's submit logic and web_auth_store.c's verify logic each
// do the right thing in isolation; this file just connects them.
#ifndef KILNCTL_LCD_CREDENTIAL_BRIDGE_H
#define KILNCTL_LCD_CREDENTIAL_BRIDGE_H

#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot, from the same place ui_lcd_lock_init() is called
// (kiln_ui_init()). Installs lcd_credential_verify_pin() as the keypad's
// verify seam and lcd_credential_load_policy() as the lock's policy seam,
// replacing the fail-closed/auth-off stubs those modules default to.
void lcd_credential_bridge_init(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_LCD_CREDENTIAL_BRIDGE_H
