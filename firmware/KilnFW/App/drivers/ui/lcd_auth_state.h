// lcd_auth_state.h -- the pure, host-testable half of the LCD's two-PIN
// keypad (docs/WEB_AUTH_PLAN.md section 7) and the LCD's own inactivity
// lock / 10-second stay-unlocked prompt (section 8, LCD half only -- the web
// half of section 8 belongs to whichever session module the web credential
// work lands in, not here).
//
// Deliberately mirrors ota_auth.h's split: no LVGL, no ESP-IDF, no NVS --
// this file is #include-able by the host test harness exactly like
// App/drivers/net/ota_auth.h already is (see App/test/build_host_tests.ps1).
// The LVGL glue (the actual lv_msgbox overlay, the buttonmatrix, the touch
// activity hook) lives in ui_lcd_keypad.c, which is thin and untested here
// on purpose -- the state machine below is where the real logic (and the
// real risk of an off-by-one that locks an operator out of their own kiln)
// lives, so it is the part worth a host test.
//
// THE SEAM (read this before wiring the real credential store in):
// lcd_auth_state_set_verify_fn() is the one call a future session swaps to
// go from "no credential store exists yet" to "verify against the real
// PBKDF2-hashed lcd_auth NVS record" (docs/WEB_AUTH_PLAN.md item 2). Until
// that lands, the default verify function (lcd_auth_default_verify(),
// installed automatically) always returns LCD_PIN_ROLE_NONE -- it accepts no
// PIN at all. That is intentionally the *safe* default: auth ships OFF
// (item 11), so with lcd_enabled false the keypad in ui_lcd_keypad.c never
// even appears and this function is never called; if a developer flips
// lcd_enabled on before the credential module lands, the stub fails closed
// (denies every PIN) rather than granting spurious access. Swapping in the
// real verifier is exactly one call:
//   lcd_auth_state_set_verify_fn(lcd_credential_verify_pin);
// at boot, once that symbol exists, and nothing else in this file or in
// ui_lcd_keypad.c changes.
#ifndef KILNCTL_LCD_AUTH_STATE_H
#define KILNCTL_LCD_AUTH_STATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "../net/login_backoff.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- PIN entry buffer (section 7: "OK is inert below 4 digits and entry
// stops accepting at 8") -----------------------------------------------

#define LCD_PIN_MIN_DIGITS 4u
#define LCD_PIN_MAX_DIGITS 8u

typedef struct {
    char    digits[LCD_PIN_MAX_DIGITS + 1]; // NUL-terminated, ASCII '0'-'9'
    uint8_t len;
} lcd_pin_entry_t;

void lcd_pin_entry_reset(lcd_pin_entry_t *e);

// Appends one digit ('0'-'9'). Returns false and does nothing if `e` is
// already at LCD_PIN_MAX_DIGITS or `digit` is not a decimal digit -- "entry
// stops accepting at 8" is enforced here, not left to the caller.
bool lcd_pin_entry_push_digit(lcd_pin_entry_t *e, char digit);

// Removes the last digit. Returns false (no-op) if already empty.
bool lcd_pin_entry_backspace(lcd_pin_entry_t *e);

// True iff len is in [LCD_PIN_MIN_DIGITS, LCD_PIN_MAX_DIGITS] -- "OK is
// inert below 4 digits" is this function returning false, checked by the
// caller (the keypad's OK button handler) before it ever calls
// lcd_auth_state_submit().
bool lcd_pin_entry_can_submit(const lcd_pin_entry_t *e);

// --- Which PIN, which tier (section 7: "the PIN itself is the role
// selector") -------------------------------------------------------------

typedef enum {
    LCD_PIN_ROLE_NONE = 0, // wrong PIN, or no credential configured yet
    LCD_PIN_ROLE_USER,
    LCD_PIN_ROLE_ADMIN,
} lcd_pin_role_t;

// The seam. Implemented for real once the credential-storage module lands;
// see this header's top comment. Must run in bounded time/stack (no I/O,
// this is called from the keypad's OK handler on the LVGL/UI task).
typedef lcd_pin_role_t (*lcd_pin_verify_fn_t)(const char *digits, uint8_t len);

// Fails closed: accepts no PIN. Exported so a host test can restore the
// default explicitly after pointing the seam elsewhere.
lcd_pin_role_t lcd_auth_default_verify(const char *digits, uint8_t len);

// Installs the verify function used by lcd_auth_state_submit(). Passing NULL
// restores lcd_auth_default_verify(). Not thread-safe by design -- call this
// once at boot from a single task, the same convention every other seam in
// this codebase (e.g. watchdog_cfg's panic-disabled hook) uses.
void lcd_auth_state_set_verify_fn(lcd_pin_verify_fn_t fn);

// --- Submit + lockout (section 7: "every wrong PIN feeds the existing
// lockout under its own new context ... keyed to the panel as a whole, not
// per PIN") -----------------------------------------------------------------
//
// Owner decision, 2026-09-28 ("apply the web password policies to the LCD
// PIN too"): this used to be ota_auth_lockout_state_t (the OTA surface's
// 3-failures/60s-doubling-to-900s scheme). That was itself a drift bug by
// then -- the web login moved OFF that same shared scheme on 2026-09-21 onto
// its own 5s/10s/30s/60s/300s ladder (login_backoff_state_t,
// ../net/login_backoff.h) and the LCD was never updated to follow. Switched
// here to login_backoff_state_t so the LCD PIN now mirrors the web login's
// ACTUAL CURRENT lockout thresholds and backoff shape by calling the exact
// same pure functions, not a second copy of the numbers. Still one instance
// = one new, LCD-only STATE (WEB_AUTH_PLAN.md section 7) -- never shared
// with, compared to, or merged with the web login's own per-IP table
// entries or an OTA context's instance; only the ladder/functions are
// shared, never the counters.
typedef struct {
    lcd_pin_entry_t           entry;
    login_backoff_state_t     lockout; // one instance = one new, LCD-only context;
                                        // shares login_backoff.h's policy with the web
                                        // login, never its state
} lcd_keypad_state_t;

void lcd_keypad_state_init(lcd_keypad_state_t *ks);

typedef enum {
    LCD_KEYPAD_SUBMIT_LOCKED_OUT, // panel-wide lockout active -- PIN was NOT checked
    LCD_KEYPAD_SUBMIT_TOO_SHORT,  // called with entry.len < LCD_PIN_MIN_DIGITS -- not a
                                   // real attempt, does not touch the lockout counter
                                   // (defensive: the UI must not call this at all below
                                   // 4 digits, but this function does not trust that)
    LCD_KEYPAD_SUBMIT_DENIED,     // a real attempt, PIN was wrong -- lockout failure recorded
    LCD_KEYPAD_SUBMIT_GRANTED,    // PIN correct -- lockout reset, *out_role tells which PIN
} lcd_keypad_submit_result_t;

// Checks ks->lockout first (a locked-out panel never even calls the verify
// seam -- "a locked-out panel still shows the Dashboard and still stops a
// firing" is a property of the caller never routing Stop through this
// function at all; see ui_lcd_keypad.c and item 9). On a real attempt, calls
// the installed verify function against ks->entry, then records success or
// failure on ks->lockout accordingly. Does not reset ks->entry -- the caller
// does that (lcd_pin_entry_reset()) once it has read the result, so a denied
// attempt's digits are still available to redraw/clear as the caller sees
// fit.
lcd_keypad_submit_result_t lcd_keypad_state_submit(lcd_keypad_state_t *ks, uint32_t now_ms,
                                                    lcd_pin_role_t *out_role);

// --- LCD inactivity lock + 10s stay-unlocked prompt (section 8, LCD half) -

// timeout_s == 0 is the "never" sentinel (section 8: "'never' is meaningful
// and must work"). Any other value is whole seconds, expected range 60-3600
// (1-60 minutes) per the plan's stated range, but this module does not
// enforce that range itself -- the web setter (owned elsewhere) is the
// authoritative bound-check, same division of labor as section 3's PIN
// length rule.
#define LCD_LOCK_TIMEOUT_NEVER   0u
#define LCD_LOCK_PROMPT_WINDOW_MS 10000u // "at timeout - 10s, ... show a Stay unlocked prompt"

typedef struct {
    lcd_pin_role_t granted_role;  // NONE == locked
    uint32_t       timeout_s;
    uint32_t       last_activity_ms;
    bool           prompt_open;
} lcd_lock_state_t;

// Starts locked (granted_role = NONE), timeout_s as given, last_activity_ms
// = now_ms so a freshly-booted board is not instantly treated as having sat
// idle since time 0.
void lcd_lock_state_init(lcd_lock_state_t *ls, uint32_t timeout_s, uint32_t now_ms);

// True iff no role is currently granted -- callers gate the keypad's
// appearance on this (item 7: "when a touch on the LCD attempts a gated
// action while the LCD is locked").
bool lcd_lock_is_locked(const lcd_lock_state_t *ls);

// Called once a keypad submit returns GRANTED. Starts (or restarts) the
// session as `role`, resets the activity clock to now_ms, and closes the
// prompt if one was open.
void lcd_lock_grant(lcd_lock_state_t *ls, lcd_pin_role_t role, uint32_t now_ms);

// Explicit lock -- e.g. an operator-visible "lock now", or the reset gesture
// (item 10) landing while a session happens to be open. Equivalent to what
// tick() does on expiry, exposed directly so a caller never has to fake a
// clock jump to get the same effect.
void lcd_lock_force_lock(lcd_lock_state_t *ls);

// "What counts as activity ... any touch event delivered to LVGL, including
// a touch that only scrolls or is swallowed by a backdrop." Extends the
// session and closes the prompt if it was open. A no-op while already
// locked (there is nothing to extend) or while timeout_s is the "never"
// sentinel (nothing to extend against).
void lcd_lock_note_activity(lcd_lock_state_t *ls, uint32_t now_ms);

typedef enum {
    LCD_LOCK_TICK_OK,      // still unlocked, no prompt
    LCD_LOCK_TICK_PROMPT,  // unlocked, inside the final 10s -- caller should show/keep the prompt
    LCD_LOCK_TICK_EXPIRED, // just transitioned to locked THIS call (granted_role is now NONE);
                            // caller should close the prompt and return to the Dashboard
    LCD_LOCK_TICK_LOCKED,  // already locked, nothing changed
} lcd_lock_tick_result_t;

// Call periodically (an existing UI refresh tick is enough -- "no new task,
// no new timer" per the plan). Never blocks, never allocates, no I/O.
lcd_lock_tick_result_t lcd_lock_tick(lcd_lock_state_t *ls, uint32_t now_ms);

// --- Relock edge: which open UI must be torn down (LCD-19, 2026-09-30) ----
//
// Owner decision 2026-09-28 ("re-lock closes open privileged UI") made every
// unlocked->locked edge force-close an open keypad. That is correct for a
// keypad/dialog left over from a session the edge is revoking, but wrong for
// a keypad raised strictly AFTER the very lock transition driving this edge
// was already under way -- that keypad already IS the PIN gate for the
// incoming locked state, not leftover privileged UI.
//
// LCD-19 bench run 20260930T190017Z_lcd: lcd_enabled flips false->true
// (cases_lcd.py). While disabled, ui_lcd_lock.c's tick_timer_cb() force-locks
// s_lock on *every* tick directly (not via the pending flag) -- so by the
// time the policy write lands and the Start tap follows ~1s later, s_lock is
// already locked, not merely about to become locked. The first fix here
// (2026-09-30, superseded the same day) stamped the exemption only when
// `!lcd_lock_is_locked(&s_lock) && force_pending` -- requiring the lock to
// still be UNLOCKED with a pending flag -- which is exactly backwards for
// this transition: since s_lock was already locked, that condition was
// always false, so the keypad this raise correctly opened as the enable-
// transition's PIN gate was left unprotected and the relock edge closed it.
// A `!s_was_locked`-only stamp (considered and rejected) fails a different
// way: s_was_locked reads false whenever a session is genuinely unlocked and
// idle (not just mid-transition -- see tick_timer_cb()'s normal LOCK_TICK_OK
// path, which keeps s_was_locked in sync with locked_now every tick), so it
// would also exempt a keypad opened for an ordinary role-upgrade prompt
// (has_role() denies on role, not lock) while a session is genuinely active
// -- if a real revoke then landed before that keypad was resolved, this
// stale, no-longer-authorized keypad would wrongly survive the very edge
// meant to close it.
//
// The correct signal is simpler: has_role() itself denies for exactly two
// reasons -- "locked or pending" (this keypad IS the incoming lock's PIN
// gate) vs "role too low while genuinely unlocked" (this keypad belongs to
// a still-active session and must NOT survive a later revoke). This
// function is that same disjunction, made pure and host-testable so a
// future edit to either side of it fails a test rather than only a bench
// run: exempt iff `currently_locked || force_lock_pending` at the moment the
// keypad was raised, which is already true if s_lock was force-locked
// directly (disabled-branch case) OR only pending via the atomic flag (the
// original force_lock() case) -- both are "this keypad IS the lock's own PIN
// gate", never "leftover from an unlocked session".
bool lcd_lock_keypad_raise_is_lock_gate(bool currently_locked, bool force_lock_pending);

// The relock-edge consumer: `keypad_open` false always returns false
// (nothing to close). Otherwise: exempt (false) iff
// `keypad_is_pending_lock_gate` is true (as decided by
// lcd_lock_keypad_raise_is_lock_gate() above at raise time); a later,
// unrelated edge (inactivity timeout, or the NEXT force-lock request) is not
// exempt -- the caller stamps `keypad_is_pending_lock_gate` false again once
// this one tick has consumed it (one-shot).
bool lcd_lock_relock_should_close_keypad(bool keypad_open, bool keypad_is_pending_lock_gate);

// Touch-calibration page exits (LCD review N1, 2026-10-09). The first-boot
// touch_cal page is shown BEFORE home, so a user who never entered a PIN
// can leave it for "config" (Cancel, refused save) and then reach Profiles
// and Start with no relock edge ever firing. Every exit therefore goes to
// "home" unless a USER role is held; with the role (or auth disabled, which
// ui_lcd_lock_has_role() already folds in) the page's own `wanted` target
// stands. Never NULL.
const char *lcd_touch_cal_exit_target(bool has_user_role, const char *wanted);

// Home trip strip -> Safety page (LCD review N5, owner decision 2026-10-09):
// opening the page needs the USER PIN like any non-dashboard page. True when a
// PIN prompt is required first (no USER role held).
bool lcd_safety_strip_needs_pin(bool has_user_role);

#ifdef __cplusplus
}
#endif

#endif // KILNCTL_LCD_AUTH_STATE_H
