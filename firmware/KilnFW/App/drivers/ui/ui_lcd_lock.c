#include "ui_lcd_lock.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_log.h"

#include "lvgl_port.h"
#include "ui_confirm.h"
#include "ui_lcd_keypad.h"
#include "ui_theme.h"

static const char *TAG = "ui_lcd_lock";

#define UI_LCD_LOCK_TICK_PERIOD_MS 1000u

static lcd_lock_state_t s_lock;
static lv_timer_t      *s_tick_timer;

// The 10s stay-unlocked prompt -- a small dedicated lv_msgbox rather than
// ui_confirm.c's shared dialog, because this one needs to auto-dismiss
// itself when the lock actually expires ("ignoring it locks at expiry and
// the prompt closes itself") and to redraw a live countdown every tick,
// neither of which ui_confirm.c's generic Yes/Cancel shape supports without
// widening it for every OTHER call site. Still lv_msgbox_create(NULL) on
// lv_layer_top(), same FLEX TRAP-avoidance as every other overlay here.
static lv_obj_t *s_prompt_mbox;
static lv_obj_t *s_prompt_countdown_label;

static ui_lcd_lock_policy_fn_t s_policy_fn;
static ui_lcd_lock_relock_cb_t s_relock_cb; // see ui_lcd_lock.h's doc comment

// Last lock state tick_timer_cb() observed while the policy was enabled, for
// edge-detecting EVERY unlocked->locked transition, not only the inactivity
// timeout (2026-09-28 review of 3e7bb20b): ui_lcd_lock_force_lock() (web
// policy transition, LCD PIN/password change) and enabling auth while the
// panel sits on a non-home page all lock the session without passing through
// LCD_LOCK_TICK_EXPIRED, and used to leave the operator on a gated page with
// no session. Starts true so the boot page (home, or touch_cal on an
// uncalibrated panel) is never kicked on the first tick. Set false while the
// policy is disabled, so enabling auth reads as an edge on its first tick.
static bool s_was_locked = true;

// Set by ui_lcd_lock_force_lock(), which runs on the httpd task
// (security_backend_web_auth.c's policy/credential-change call sites) --
// never touch s_lock or any LVGL object from there (LVGL is only safe to
// touch from the LVGL task/port lock). This is the whole hand-off: a single
// atomic flag, no lock held across any producer call. tick_timer_cb() below
// (LVGL task) is the only reader/clearer, and it is also the only place
// lcd_lock_force_lock(&s_lock) and the LVGL close_prompt()/keypad calls run.
static atomic_bool s_force_lock_requested = false;

// LVGL-task-only (set/read/cleared only from ui_lcd_lock_run_gated() and
// tick_timer_cb(), both on the LVGL task -- no atomic needed). True when the
// keypad currently open, if any, was raised by ui_lcd_lock_run_gated() while
// a ui_lcd_lock_force_lock() request was still pending (i.e. it already IS
// the PIN gate for the incoming locked state, not leftover UI from the
// session that request is revoking). LCD-19 (2026-09-30 bench run,
// 20260930T190017Z_lcd): a policy force-lock landed, the harness tapped
// Start within the same ~1s tick window, has_role() correctly denied and
// raised the keypad -- and the very next tick, which applied the pending
// lock, then immediately force-closed that same keypad via the unconditional
// "locked_now && !s_was_locked" edge below, because that edge cannot tell a
// fresh PIN gate from a stale session's leftover UI. One-shot: cleared at the
// end of every tick_timer_cb() call so it only exempts the single tick that
// applies the force-lock it was raised for -- a later, unrelated edge
// (inactivity timeout, or the NEXT force-lock request) still closes it.
static bool s_keypad_is_pending_lock_gate = false;

static ui_lcd_lock_policy_t default_policy(void)
{
    ui_lcd_lock_policy_t p = { .enabled = false, .timeout_s = LCD_LOCK_TIMEOUT_NEVER };
    return p;
}

void ui_lcd_lock_set_policy_fn(ui_lcd_lock_policy_fn_t fn)
{
    s_policy_fn = fn;
}

void ui_lcd_lock_set_relock_cb(ui_lcd_lock_relock_cb_t fn)
{
    s_relock_cb = fn;
}

static ui_lcd_lock_policy_t current_policy(void)
{
    return s_policy_fn ? s_policy_fn() : default_policy();
}

static void close_prompt(void)
{
    if (s_prompt_mbox) {
        lv_obj_add_flag(s_prompt_mbox, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *backdrop = lv_obj_get_parent(s_prompt_mbox);
        if (backdrop) {
            lv_obj_add_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void prompt_stay_unlocked_cb(lv_event_t *e)
{
    (void)e;
    lcd_lock_note_activity(&s_lock, (uint32_t)lv_tick_get());
    close_prompt();
}

static void prompt_backdrop_click_cb(lv_event_t *e)
{
    /* Dismissing (tap outside) is NOT the same as accepting -- just closes
     * the prompt, does not extend the session (see build_prompt() comment). */
    (void)e;
    close_prompt();
}

static void build_prompt(void)
{
    s_prompt_mbox = lv_msgbox_create(NULL);
    lv_obj_set_width(s_prompt_mbox, 360);
    lv_msgbox_add_title(s_prompt_mbox, "Stay unlocked?");
    s_prompt_countdown_label = lv_msgbox_add_text(s_prompt_mbox, "");

    lv_obj_t *btn = lv_msgbox_add_footer_button(s_prompt_mbox, "Stay Unlocked");
    lv_obj_set_style_bg_color(btn, UI_THEME_ACCENT_4, 0);
    lv_obj_add_event_cb(btn, prompt_stay_unlocked_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *footer = lv_msgbox_get_footer(s_prompt_mbox);
    lv_obj_set_height(footer, UI_THEME_MIN_TOUCH_TARGET_PX);

    /* Section 8: "dismissed instantly by any touch outside it, not only by
     * its own buttons" -- same backdrop-click-cancels wiring ui_lcd_keypad.c
     * uses, except here dismissal does NOT count as accepting: it just
     * closes the prompt and lets the tick keep counting down normally
     * (dismissing is not the same as extending). */
    lv_obj_t *backdrop = lv_obj_get_parent(s_prompt_mbox);
    if (backdrop) {
        lv_obj_add_flag(backdrop, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(backdrop, prompt_backdrop_click_cb, LV_EVENT_CLICKED, NULL);
    }
}

static void show_prompt(uint32_t seconds_left)
{
    if (!s_prompt_mbox) {
        build_prompt();
    }
    lv_label_set_text_fmt(s_prompt_countdown_label, "Locking in %u s. Tap Stay Unlocked to continue.",
                           (unsigned)seconds_left);
    lv_obj_t *backdrop = lv_obj_get_parent(s_prompt_mbox);
    if (backdrop) {
        lv_obj_remove_flag(backdrop, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(backdrop);
    }
    lv_obj_remove_flag(s_prompt_mbox, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_prompt_mbox);
}

static void tick_timer_cb(lv_timer_t *t)
{
    (void)t;

    // Consume any pending ui_lcd_lock_force_lock() request from the httpd
    // task here, on the LVGL task, so lcd_lock_force_lock(&s_lock) is only
    // ever called from this one place. The actual close_prompt()/keypad
    // teardown for this transition happens below via the existing
    // "locked_now && !s_was_locked" edge detection -- no need to duplicate it
    // here. s_was_locked is cleared on an actual transition because the
    // session may have been granted (keypad PIN -> gated action already ran,
    // e.g. the edit-firing page or a Confirm Stop dialog opened) AFTER the
    // previous tick recorded s_was_locked = true; without this the edge
    // below would see locked->locked and leave that page/dialog open with no
    // session.
    if (atomic_exchange(&s_force_lock_requested, false)) {
        if (!lcd_lock_is_locked(&s_lock)) {
            lcd_lock_force_lock(&s_lock);
            s_was_locked = false;
            ESP_LOGI(TAG, "LCD session force-locked (policy transition)");
        } else {
            // Requirement: log a consumed flag even when it was a no-op (the
            // session was already locked -- e.g. the earlier inactivity-
            // timeout edge, or a second policy write landing before the
            // first's flag was consumed).
            ESP_LOGI(TAG, "LCD force-lock request consumed (session already locked, no-op)");
        }
    }

    ui_lcd_lock_policy_t policy = current_policy();
    if (!policy.enabled) {
        // Item 4b fix (2026-09-17 adversarial review, 1179e2d3): this used
        // to just return here, leaving s_lock.granted_role untouched.
        // Failure sequence: unlock with a PIN (granted_role = ADMIN) ->
        // disable auth (policy.enabled false, this branch taken every tick
        // from here on) -> re-enable auth. At re-enable, s_lock.granted_role
        // was NEVER cleared, so lcd_lock_is_locked() reads false and the
        // panel is immediately usable at ADMIN tier again with no fresh PIN
        // demanded -- the reset-one-side-of-a-pair class (CLAUDE.md): the
        // policy's enabled bit was reset without revisiting the session
        // state derived from it. Force-locking every tick while disabled
        // means that by the time enabled flips back true, the session has
        // already been torn down, so re-enabling always finds the panel
        // locked, matching section 11's "enabling auth clears every
        // session" for the LCD side without needing a separate push from
        // the policy-write call site.
        if (!lcd_lock_is_locked(&s_lock)) {
            lcd_lock_force_lock(&s_lock);
            bool had_prompt = s_prompt_mbox && !lv_obj_has_flag(s_prompt_mbox, LV_OBJ_FLAG_HIDDEN);
            close_prompt();
            bool had_keypad = ui_lcd_keypad_is_open();
            if (had_keypad) {
                ui_lcd_keypad_force_close();
            }
            if (had_prompt || had_keypad) {
                ESP_LOGI(TAG, "LCD lock: closed%s%s (policy disabled)",
                         had_prompt ? " stay-unlocked prompt" : "",
                         had_keypad ? " keypad" : "");
            }
        }
        s_keypad_is_pending_lock_gate = false;
        s_was_locked = false; // auth off: the panel is effectively unlocked
        return; // section 11: auth off, nothing further to tick
    }
    if (s_lock.timeout_s != policy.timeout_s) {
        s_lock.timeout_s = policy.timeout_s; // pick up a live policy change without a reboot
    }

    uint32_t now_ms = (uint32_t)lv_tick_get();
    lcd_lock_tick_result_t r = lcd_lock_tick(&s_lock, now_ms);

    switch (r) {
        case LCD_LOCK_TICK_PROMPT: {
            uint32_t timeout_ms = s_lock.timeout_s * 1000u;
            uint32_t elapsed_ms = now_ms - s_lock.last_activity_ms;
            uint32_t remaining_ms = (timeout_ms > elapsed_ms) ? (timeout_ms - elapsed_ms) : 0u;
            show_prompt((remaining_ms + 999u) / 1000u);
            break;
        }
        case LCD_LOCK_TICK_EXPIRED: {
            close_prompt();
            /* A stranded keypad behind a lock that just expired would sit on
             * screen authorising nothing -- close it rather than leave it. */
            bool had_keypad = ui_lcd_keypad_is_open();
            if (had_keypad) {
                ui_lcd_keypad_force_close();
            }
            ESP_LOGI(TAG, "LCD session locked (inactivity timeout)%s",
                     had_keypad ? " -- closed open keypad" : "");
            break; // the relock itself runs on the edge below
        }
        case LCD_LOCK_TICK_OK:
            close_prompt();
            break;
        case LCD_LOCK_TICK_LOCKED:
        default:
            break;
    }

    /* Owner decision 2026-09-28: without a session only the dashboard is
     * reachable. On every unlocked->locked edge -- inactivity timeout,
     * ui_lcd_lock_force_lock() from a policy/credential change, or auth just
     * enabled -- close anything a lapsed session left tappable (an open
     * Confirm Start/Stop dialog, a keypad) and return to home. Runs here on
     * the LVGL timer, never from the httpd task that may have force-locked. */
    bool locked_now = lcd_lock_is_locked(&s_lock);
    if (locked_now && !s_was_locked) {
        bool had_prompt = s_prompt_mbox && !lv_obj_has_flag(s_prompt_mbox, LV_OBJ_FLAG_HIDDEN);
        close_prompt();
        if (had_prompt) {
            ESP_LOGI(TAG, "LCD relock edge: closed stay-unlocked prompt");
        }

        bool keypad_open = ui_lcd_keypad_is_open();
        if (lcd_lock_relock_should_close_keypad(keypad_open, s_keypad_is_pending_lock_gate)) {
            ui_lcd_keypad_force_close();
            ESP_LOGI(TAG, "LCD relock edge: closed open keypad");
        } else if (keypad_open) {
            ESP_LOGI(TAG, "LCD relock edge: keypad kept open (raised as PIN gate for this lock)");
        }

        bool had_confirm = ui_confirm_is_open();
        ui_confirm_close_open();
        if (had_confirm) {
            ESP_LOGI(TAG, "LCD relock edge: closed open confirm dialog");
        }

        if (s_relock_cb) {
            s_relock_cb();
        }
    }
    // One-shot: this exemption only ever protects the single tick that
    // applies the force-lock request the exempted keypad was raised for.
    s_keypad_is_pending_lock_gate = false;
    s_was_locked = locked_now;
}

static void on_touch_pressed(lv_event_t *e)
{
    (void)e;
    ui_lcd_lock_note_activity();
}

void ui_lcd_lock_init(void)
{
    lcd_lock_state_init(&s_lock, current_policy().timeout_s, (uint32_t)lv_tick_get());

    if (!s_tick_timer) {
        s_tick_timer = lv_timer_create(tick_timer_cb, UI_LCD_LOCK_TICK_PERIOD_MS, NULL);
    }

    lv_indev_t *indev = lvgl_port_get_indev();
    if (indev) {
        lv_indev_add_event_cb(indev, on_touch_pressed, LV_EVENT_PRESSED, NULL);
    }
}

bool ui_lcd_lock_has_role(lcd_pin_role_t role)
{
    ui_lcd_lock_policy_t policy = current_policy();
    if (!policy.enabled) {
        return true; // item 11: auth off collapses every tier to full access
    }
    // A ui_lcd_lock_force_lock() the tick has not consumed yet counts as
    // locked here, so a gated tap in the <= one-tick gap after a web
    // policy/credential change prompts for a PIN instead of running on the
    // session that change just revoked. Read-only: the lock transition and
    // its teardown stay in tick_timer_cb().
    if (lcd_lock_is_locked(&s_lock) || atomic_load(&s_force_lock_requested)) {
        return false;
    }
    if (role == LCD_PIN_ROLE_ADMIN) {
        return s_lock.granted_role == LCD_PIN_ROLE_ADMIN;
    }
    return true; // any granted role (USER or ADMIN) satisfies a USER-tier ask
}

void ui_lcd_lock_note_activity(void)
{
    lcd_lock_note_activity(&s_lock, (uint32_t)lv_tick_get());
}

void ui_lcd_lock_force_lock(void)
{
    // Called from the httpd task (security_backend_web_auth.c). Must never
    // touch s_lock or any LVGL object directly here -- LVGL is only safe to
    // touch from the LVGL task/port lock, and s_lock is otherwise only ever
    // read/written from that same task's tick_timer_cb(). Post the request
    // and return; tick_timer_cb() performs the actual lock transition and
    // any resulting close_prompt()/keypad teardown within one tick
    // (UI_LCD_LOCK_TICK_PERIOD_MS).
    atomic_store(&s_force_lock_requested, true);
}

typedef struct {
    ui_lcd_lock_gated_cb_t action;
    void                  *user_data;
    lcd_pin_role_t         min_role;
} gate_ctx_t;

static gate_ctx_t s_gate_ctx; // one gate in flight at a time -- same
                               // "nothing in this codebase shows two of
                               // these at once" convention as ui_confirm.c

static void gate_keypad_done_cb(bool granted, lcd_pin_role_t role, void *user_data)
{
    (void)user_data;
    if (!granted) {
        return;
    }
    lcd_lock_grant(&s_lock, role, (uint32_t)lv_tick_get());
    if (role < s_gate_ctx.min_role) {
        /* A correct but insufficient PIN (user PIN against an admin-only
         * action) grants the session at its own tier -- it just doesn't
         * satisfy THIS particular gated action. */
        return;
    }
    if (s_gate_ctx.action) {
        s_gate_ctx.action(s_gate_ctx.user_data);
    }
}

void ui_lcd_lock_run_gated(const char *prompt, lcd_pin_role_t min_role, ui_lcd_lock_gated_cb_t action,
                            void *user_data)
{
    if (ui_lcd_lock_has_role(min_role)) {
        if (action) {
            action(user_data);
        }
        return;
    }

    // has_role() just returned false, for one of exactly two reasons: the
    // panel is locked (or a lock is pending), or a session is genuinely
    // active but its role is too low for this action. Only the first is
    // "this keypad IS the incoming/current lock's own PIN gate" -- the
    // second is a role-upgrade prompt against a still-active session, which
    // must still be torn down if that session is later revoked. Use the
    // same pure disjunction has_role() itself checks (currently locked OR a
    // force-lock is pending) rather than requiring "not yet locked" -- see
    // lcd_lock_keypad_raise_is_lock_gate()'s header comment for why an
    // earlier version of this stamp, which required `!lcd_lock_is_locked()`,
    // missed the case where ui_lcd_lock.c's own disabled-policy branch had
    // already force-locked s_lock directly (LCD-19, 2026-09-30).
    s_keypad_is_pending_lock_gate =
        lcd_lock_keypad_raise_is_lock_gate(lcd_lock_is_locked(&s_lock), atomic_load(&s_force_lock_requested));

    s_gate_ctx.action = action;
    s_gate_ctx.user_data = user_data;
    s_gate_ctx.min_role = min_role;
    ui_lcd_keypad_show(prompt, gate_keypad_done_cb, NULL);
}
