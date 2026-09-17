#include "lcd_credential_bridge.h"

#include "lcd_auth_state.h"
#include "ui_lcd_lock.h"
#include "../persist/web_auth_store.h"

// --- Verify seam: lcd_auth_state_set_verify_fn() --------------------------
//
// The PIN itself is the role selector (section 7): try each role's stored
// PIN and report whichever one matched. web_auth_pin_check()'s
// SAME_AS_OTHER rule guarantees the two PINs never collide, so at most one
// of these two calls can ever succeed for a given `digits`.
static lcd_pin_role_t lcd_credential_verify_pin(const char *digits, uint8_t len)
{
    (void)len; // digits is NUL-terminated (lcd_pin_entry_t's invariant);
               // web_auth_store_verify_pin() takes a NUL-terminated string.
    if (web_auth_store_verify_pin(WEB_AUTH_ROLE_ADMINISTRATOR, digits)) {
        return LCD_PIN_ROLE_ADMIN;
    }
    if (web_auth_store_verify_pin(WEB_AUTH_ROLE_USER, digits)) {
        return LCD_PIN_ROLE_USER;
    }
    return LCD_PIN_ROLE_NONE;
}

// --- Policy seam: ui_lcd_lock_set_policy_fn() ------------------------------
//
// web_auth_policy_effective_enabled() -- not the raw stored flag -- decides
// "enabled": ABSENT collapses to false (shipped default / field upgrade,
// item 11), OK passes the stored flag through, and UNREADABLE collapses to
// true so an unparseable record fails closed rather than silently disabling
// the lock (item 12b's OTA-rollback-past-a-schema-bump case). Since the
// underlying PIN records are then also UNREADABLE in that same case,
// lcd_credential_verify_pin() above denies every PIN unconditionally -- the
// panel locks and stays locked until the physical reset gesture (item 10).
static ui_lcd_lock_policy_t lcd_credential_load_policy(void)
{
    web_auth_policy_t stored;
    web_auth_load_status_t status = web_auth_store_load_policy(&stored);

    ui_lcd_lock_policy_t p;
    p.enabled = web_auth_policy_effective_enabled(status, stored.lcd_enabled);

    // -1 == "never" (web_auth_store.h); LCD_LOCK_TIMEOUT_NEVER == 0 is the
    // same sentinel on this side (lcd_auth_state.h). ABSENT/UNREADABLE leave
    // `stored` zeroed, i.e. lcd_timeout_s == 0, which already maps to
    // "never" below -- harmless either way since a fail-closed UNREADABLE
    // record also fails every PIN check, so no session is ever granted for
    // the timeout to apply to.
    p.timeout_s = (stored.lcd_timeout_s <= 0) ? LCD_LOCK_TIMEOUT_NEVER
                                               : (uint32_t)stored.lcd_timeout_s;
    return p;
}

void lcd_credential_bridge_init(void)
{
    lcd_auth_state_set_verify_fn(lcd_credential_verify_pin);
    ui_lcd_lock_set_policy_fn(lcd_credential_load_policy);
}
