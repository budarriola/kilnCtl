// lcd_credential_bridge.h -- wires the LCD keypad/lock seams
// (lcd_auth_state.h's lcd_auth_state_set_verify_fn(), ui_lcd_lock.h's
// ui_lcd_lock_set_policy_fn()) to the real credential store
// (persist/web_auth_store.h, docs/WEB_AUTH_PLAN.md sections 2/3/11) now that
// it has landed. Deliberately a separate, tiny file rather than folding this
// into ui_lcd_lock.c: it is the one place in the LCD auth slice that reaches
// into persist/, so a future audit of "what touches web_auth_store.h" finds
// exactly one small file, not scattered #includes across the UI glue.
//
// This file DOES carry its own logic worth testing directly, even though it
// is thin: the try-administrator-then-user role ordering in
// lcd_credential_verify_pin(), and the ABSENT/OK/UNREADABLE -> effective
// "enabled" collapse in lcd_credential_load_policy(). web_auth_store.c's own
// host tests cover web_auth_store_verify_pin()/web_auth_policy_effective_
// enabled() in isolation, but not how this file combines them -- see
// App/test/test_lcd_credential_bridge.c.
#ifndef KILNCTL_LCD_CREDENTIAL_BRIDGE_H
#define KILNCTL_LCD_CREDENTIAL_BRIDGE_H

#include <stdint.h>

#include "lcd_auth_state.h"
#include "ui_lcd_lock.h"

#ifdef __cplusplus
extern "C" {
#endif

// Call once at boot, from the same place ui_lcd_lock_init() is called
// (kiln_ui_init()). Installs lcd_credential_verify_pin() as the keypad's
// verify seam and lcd_credential_load_policy() as the lock's policy seam,
// replacing the fail-closed/auth-off stubs those modules default to.
void lcd_credential_bridge_init(void);

// --- Exposed for host testing (App/test/test_lcd_credential_bridge.c) -----
// Production code never calls these directly -- it goes through the seams
// via lcd_credential_bridge_init() above. Not static so the two decisions
// this file makes on its own (role ordering, effective-enabled collapse)
// can be exercised against a fake_kv-backed web_auth_store, the same
// "exposed for host tests" convention as adaptive_tune.h and
// safety_stack_margin_http.h use for their own pure helpers.
lcd_pin_role_t lcd_credential_verify_pin(const char *digits, uint8_t len);
ui_lcd_lock_policy_t lcd_credential_load_policy(void);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_LCD_CREDENTIAL_BRIDGE_H
