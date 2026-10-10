#include "lcd_auth_state.h"

#include <string.h>

// --- PIN entry -------------------------------------------------------

void lcd_pin_entry_reset(lcd_pin_entry_t *e)
{
    if (!e) {
        return;
    }
    memset(e, 0, sizeof(*e));
}

bool lcd_pin_entry_push_digit(lcd_pin_entry_t *e, char digit)
{
    if (!e || digit < '0' || digit > '9') {
        return false;
    }
    if (e->len >= LCD_PIN_MAX_DIGITS) {
        return false; // "entry stops accepting at 8"
    }
    e->digits[e->len] = digit;
    e->len++;
    e->digits[e->len] = '\0';
    return true;
}

bool lcd_pin_entry_backspace(lcd_pin_entry_t *e)
{
    if (!e || e->len == 0) {
        return false;
    }
    e->len--;
    e->digits[e->len] = '\0';
    return true;
}

bool lcd_pin_entry_can_submit(const lcd_pin_entry_t *e)
{
    if (!e) {
        return false;
    }
    return e->len >= LCD_PIN_MIN_DIGITS && e->len <= LCD_PIN_MAX_DIGITS;
}

// --- Verify seam -------------------------------------------------------

lcd_pin_role_t lcd_auth_default_verify(const char *digits, uint8_t len)
{
    (void)digits;
    (void)len;
    return LCD_PIN_ROLE_NONE; // fails closed -- see this module's header comment
}

static lcd_pin_verify_fn_t s_verify_fn = lcd_auth_default_verify;

void lcd_auth_state_set_verify_fn(lcd_pin_verify_fn_t fn)
{
    s_verify_fn = fn ? fn : lcd_auth_default_verify;
}

// --- Submit + lockout ----------------------------------------------------

void lcd_keypad_state_init(lcd_keypad_state_t *ks)
{
    if (!ks) {
        return;
    }
    memset(ks, 0, sizeof(*ks));
}

lcd_keypad_submit_result_t lcd_keypad_state_submit(lcd_keypad_state_t *ks, uint32_t now_ms,
                                                    lcd_pin_role_t *out_role)
{
    if (out_role) {
        *out_role = LCD_PIN_ROLE_NONE;
    }
    if (!ks) {
        return LCD_KEYPAD_SUBMIT_TOO_SHORT;
    }

    // "The cycle resets" (login_backoff.h) once the ladder's last step has
    // both been reached and its lock has expired -- must run before the
    // is_locked() check below on every attempt, same as the web login's own
    // login_lockout_slot_for() does on every lookup.
    login_backoff_cycle_reset_if_due(&ks->lockout, now_ms);

    // A locked-out panel never even reaches the verify seam -- the PIN is
    // not checked, so no information about it leaks through timing either.
    if (login_backoff_is_locked(&ks->lockout, now_ms)) {
        return LCD_KEYPAD_SUBMIT_LOCKED_OUT;
    }

    if (!lcd_pin_entry_can_submit(&ks->entry)) {
        // Defensive only -- the keypad's OK handler must never call this
        // below 4 digits in the first place ("OK is inert below 4 digits").
        // Not a real attempt: the lockout counter is untouched.
        return LCD_KEYPAD_SUBMIT_TOO_SHORT;
    }

    lcd_pin_role_t role = s_verify_fn(ks->entry.digits, ks->entry.len);
    if (role == LCD_PIN_ROLE_NONE) {
        login_backoff_record_failure(&ks->lockout, now_ms);
        return LCD_KEYPAD_SUBMIT_DENIED;
    }

    login_backoff_record_success(&ks->lockout);
    if (out_role) {
        *out_role = role;
    }
    return LCD_KEYPAD_SUBMIT_GRANTED;
}

// --- Inactivity lock + 10s prompt ----------------------------------------

void lcd_lock_state_init(lcd_lock_state_t *ls, uint32_t timeout_s, uint32_t now_ms)
{
    if (!ls) {
        return;
    }
    memset(ls, 0, sizeof(*ls));
    ls->granted_role = LCD_PIN_ROLE_NONE;
    ls->timeout_s = timeout_s;
    ls->last_activity_ms = now_ms;
    ls->prompt_open = false;
}

bool lcd_lock_is_locked(const lcd_lock_state_t *ls)
{
    return !ls || ls->granted_role == LCD_PIN_ROLE_NONE;
}

void lcd_lock_grant(lcd_lock_state_t *ls, lcd_pin_role_t role, uint32_t now_ms)
{
    if (!ls) {
        return;
    }
    ls->granted_role = role;
    ls->last_activity_ms = now_ms;
    ls->prompt_open = false;
}

void lcd_lock_force_lock(lcd_lock_state_t *ls)
{
    if (!ls) {
        return;
    }
    ls->granted_role = LCD_PIN_ROLE_NONE;
    ls->prompt_open = false;
}

void lcd_lock_note_activity(lcd_lock_state_t *ls, uint32_t now_ms)
{
    if (!ls || lcd_lock_is_locked(ls) || ls->timeout_s == LCD_LOCK_TIMEOUT_NEVER) {
        return;
    }
    ls->last_activity_ms = now_ms;
    ls->prompt_open = false;
}

lcd_lock_tick_result_t lcd_lock_tick(lcd_lock_state_t *ls, uint32_t now_ms)
{
    if (!ls) {
        return LCD_LOCK_TICK_LOCKED;
    }
    if (lcd_lock_is_locked(ls)) {
        return LCD_LOCK_TICK_LOCKED;
    }
    if (ls->timeout_s == LCD_LOCK_TIMEOUT_NEVER) {
        ls->prompt_open = false;
        return LCD_LOCK_TICK_OK;
    }

    uint32_t timeout_ms = ls->timeout_s * 1000u;
    uint32_t elapsed_ms = now_ms - ls->last_activity_ms; // matches this codebase's existing
                                                          // uint32-tick-diff convention (ota_auth.c)

    if (elapsed_ms >= timeout_ms) {
        ls->granted_role = LCD_PIN_ROLE_NONE;
        ls->prompt_open = false;
        return LCD_LOCK_TICK_EXPIRED;
    }

    // Saturating: a timeout shorter than the 10s prompt window still shows
    // the prompt for however much time is actually left, rather than
    // underflowing to a huge threshold that never fires.
    uint32_t prompt_at_ms = (timeout_ms > LCD_LOCK_PROMPT_WINDOW_MS) ? (timeout_ms - LCD_LOCK_PROMPT_WINDOW_MS) : 0u;

    if (elapsed_ms >= prompt_at_ms) {
        ls->prompt_open = true;
        return LCD_LOCK_TICK_PROMPT;
    }

    ls->prompt_open = false;
    return LCD_LOCK_TICK_OK;
}

bool lcd_lock_keypad_raise_is_lock_gate(bool currently_locked, bool force_lock_pending)
{
    return currently_locked || force_lock_pending;
}

const char *lcd_touch_cal_exit_target(bool has_user_role, const char *wanted)
{
    if (!has_user_role || wanted == NULL) {
        return "home";
    }
    return wanted;
}

bool lcd_safety_strip_needs_pin(bool has_user_role)
{
    return !has_user_role;
}

bool lcd_lock_relock_should_close_keypad(bool keypad_open, bool keypad_is_pending_lock_gate)
{
    if (!keypad_open) {
        return false;
    }
    return !keypad_is_pending_lock_gate;
}
