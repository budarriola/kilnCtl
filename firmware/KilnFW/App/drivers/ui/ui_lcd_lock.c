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
    // here.
    if (atomic_exchange(&s_force_lock_requested, false)) {
        if (!lcd_lock_is_locked(&s_lock)) {
            lcd_lock_force_lock(&s_lock);
            ESP_LOGI(TAG, "LCD session force-locked (policy transition)");
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
            close_prompt();
            if (ui_lcd_keypad_is_open()) {
                ui_lcd_keypad_force_close();
            }
        }
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
        case LCD_LOCK_TICK_EXPIRED:
            close_prompt();
            /* A stranded keypad behind a lock that just expired would sit on
             * screen authorising nothing -- close it rather than leave it. */
            if (ui_lcd_keypad_is_open()) {
                ui_lcd_keypad_force_close();
            }
            ESP_LOGI(TAG, "LCD session locked (inactivity timeout)");
            break; // the relock itself runs on the edge below
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
        close_prompt();
        if (ui_lcd_keypad_is_open()) {
            ui_lcd_keypad_force_close();
        }
        ui_confirm_close_open();
        if (s_relock_cb) {
            s_relock_cb();
        }
    }
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
    if (lcd_lock_is_locked(&s_lock)) {
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

    s_gate_ctx.action = action;
    s_gate_ctx.user_data = user_data;
    s_gate_ctx.min_role = min_role;
    ui_lcd_keypad_show(prompt, gate_keypad_done_cb, NULL);
}
