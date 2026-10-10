// Host test for ui_lcd_lock.c (HOST_TEST_COVERAGE_GAPS round 2, R2-10).
//
// The REAL ui_lcd_lock.c and lcd_auth_state.c run unmodified. Only the LVGL
// surface is faked: stubs_lcd_lock/lvgl.h declares the lv_* symbols and this
// file records what the lock does to its "widgets" (hidden flags, label text,
// registered callbacks) and drives the 1 s tick through the lv_timer callback
// ui_lcd_lock_init() registers, with a fake clock behind lv_tick_get().
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

int g_test_failures = 0;
int g_test_count = 0;

#include "test_common.h"

#include <stdbool.h>
#include <stdatomic.h>
// MSVC /experimental:c11atomics mis-expands the one-arg atomic_load() macro when its call spans
// lines inside ui_lcd_lock.c; route it through the explicit form.
#undef atomic_load
static inline bool tb_load_bool(atomic_bool *p) { return atomic_load_explicit(p, memory_order_seq_cst); }
#define atomic_load(p) tb_load_bool(p)

// Skip the real lvgl_port.h (it drags in panel_spi.h/screen_idle.h/touch_dev.h,
// none host-compilable); declare the one symbol ui_lcd_lock.c uses.
#define LVGL_PORT_H
#include "lvgl.h"
lv_indev_t *lvgl_port_get_indev(void);

// ---- fake LVGL ---------------------------------------------------------------
enum { OBJ_BACKDROP = 1, OBJ_MBOX, OBJ_FOOTER, OBJ_BTN, OBJ_LABEL, OBJ_COUNT };
static lv_obj_t g_objs[OBJ_COUNT];
static uint32_t g_flags[OBJ_COUNT];
static int g_mbox_creates;
static char g_label_text[128];
static lv_event_cb_t g_btn_cb, g_backdrop_cb, g_touch_cb;
static lv_timer_cb_t g_timer_cb;
static uint32_t g_timer_period;
static int g_timer_creates;
static uint32_t g_now;
static lv_indev_t g_indev;
static lv_indev_t *g_indev_ret = &g_indev;

lv_obj_t *lv_layer_top(void) { return NULL; }
lv_obj_t *lv_msgbox_create(lv_obj_t *parent)
{
    (void)parent;
    g_mbox_creates++;
    for (int i = 0; i < OBJ_COUNT; i++) { g_objs[i].id = i; g_flags[i] = 0; }
    return &g_objs[OBJ_MBOX];
}
void lv_obj_set_width(lv_obj_t *o, int w) { (void)o; (void)w; }
void lv_obj_set_height(lv_obj_t *o, int h) { (void)o; (void)h; }
void lv_msgbox_add_title(lv_obj_t *m, const char *t) { (void)m; (void)t; }
lv_obj_t *lv_msgbox_add_text(lv_obj_t *m, const char *t) { (void)m; (void)t; return &g_objs[OBJ_LABEL]; }
lv_obj_t *lv_msgbox_add_footer_button(lv_obj_t *m, const char *t) { (void)m; (void)t; return &g_objs[OBJ_BTN]; }
lv_obj_t *lv_msgbox_get_footer(lv_obj_t *m) { (void)m; return &g_objs[OBJ_FOOTER]; }
void lv_obj_set_style_bg_color(lv_obj_t *o, lv_color_t c, int s) { (void)o; (void)c; (void)s; }
void lv_obj_add_event_cb(lv_obj_t *o, lv_event_cb_t cb, lv_event_code_t code, void *ud)
{
    (void)ud;
    if (o->id == OBJ_BTN && code == LV_EVENT_CLICKED) g_btn_cb = cb;
    if (o->id == OBJ_BACKDROP && code == LV_EVENT_CLICKED) g_backdrop_cb = cb;
}
lv_obj_t *lv_obj_get_parent(const lv_obj_t *o) { return (o->id == OBJ_MBOX) ? &g_objs[OBJ_BACKDROP] : NULL; }
void lv_obj_add_flag(lv_obj_t *o, uint32_t f) { g_flags[o->id] |= f; }
void lv_obj_remove_flag(lv_obj_t *o, uint32_t f) { g_flags[o->id] &= ~f; }
bool lv_obj_has_flag(const lv_obj_t *o, uint32_t f) { return (g_flags[o->id] & f) != 0; }
void lv_obj_move_foreground(lv_obj_t *o) { (void)o; }
void lv_label_set_text_fmt(lv_obj_t *l, const char *fmt, ...)
{
    (void)l;
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_label_text, sizeof(g_label_text), fmt, ap);
    va_end(ap);
}
uint32_t lv_tick_get(void) { return g_now; }
lv_timer_t *lv_timer_create(lv_timer_cb_t cb, uint32_t period, void *ud)
{
    static lv_timer_t t;
    (void)ud;
    g_timer_cb = cb;
    g_timer_period = period;
    g_timer_creates++;
    return &t;
}
void lv_indev_add_event_cb(lv_indev_t *i, lv_event_cb_t cb, lv_event_code_t code, void *ud)
{
    (void)i; (void)ud;
    if (code == LV_EVENT_PRESSED) g_touch_cb = cb;
}
lv_indev_t *lvgl_port_get_indev(void) { return g_indev_ret; }

// ---- fake keypad / confirm ---------------------------------------------------
#include "ui_lcd_keypad.h"
#include "ui_confirm.h"
static bool g_keypad_open;
static int g_keypad_show_calls, g_keypad_close_calls;
static char g_keypad_prompt[64];
static ui_lcd_keypad_done_cb_t g_keypad_done;
static bool g_confirm_open;
static int g_confirm_close_calls;
void ui_lcd_keypad_show(const char *prompt, ui_lcd_keypad_done_cb_t on_done, void *ud)
{
    (void)ud;
    g_keypad_show_calls++;
    g_keypad_open = true;
    g_keypad_done = on_done;
    snprintf(g_keypad_prompt, sizeof(g_keypad_prompt), "%s", prompt);
}
bool ui_lcd_keypad_is_open(void) { return g_keypad_open; }
void ui_lcd_keypad_force_close(void) { g_keypad_open = false; g_keypad_close_calls++; }
bool ui_confirm_is_open(void) { return g_confirm_open; }
void ui_confirm_close_open(void) { g_confirm_open = false; g_confirm_close_calls++; }
void ui_confirm_show(const ui_confirm_params_t *p) { (void)p; }

#include "../drivers/ui/ui_lcd_lock.c"

// ---- harness -----------------------------------------------------------------
static ui_lcd_lock_policy_t g_policy;
static ui_lcd_lock_policy_t policy_fn(void) { return g_policy; }
static int g_relock_calls;
static void relock_cb(void) { g_relock_calls++; }
static int g_action_calls;
static void *g_action_ud;
static void action(void *ud) { g_action_calls++; g_action_ud = ud; }

static bool prompt_visible(void)
{
    return s_prompt_mbox && !(g_flags[OBJ_MBOX] & LV_OBJ_FLAG_HIDDEN);
}

static void tick_at(uint32_t ms)
{
    g_now = ms;
    g_timer_cb(NULL);
}

// Fresh state: auth ON, 60 s timeout, session locked, edge latch settled.
static void fresh(bool enabled, uint32_t timeout_s)
{
    g_policy.enabled = false;
    g_policy.timeout_s = timeout_s;
    ui_lcd_lock_set_policy_fn(policy_fn);
    ui_lcd_lock_set_relock_cb(relock_cb);
    g_keypad_open = false;
    g_confirm_open = false;
    g_now = 1000;
    if (g_timer_cb) tick_at(0);   /* policy disabled: settles s_was_locked=false, force-locks */
    atomic_store(&s_force_lock_requested, false);
    g_now = 1000;
    ui_lcd_lock_init();         /* s_lock locked, last_activity=1000 */
    g_policy.enabled = enabled;
    g_relock_calls = g_action_calls = 0;
    g_keypad_show_calls = g_keypad_close_calls = g_confirm_close_calls = 0;
    s_was_locked = true;        /* as at boot: no edge on the first tick */
    s_keypad_is_pending_lock_gate = false;
    if (s_prompt_mbox) { g_flags[OBJ_MBOX] |= LV_OBJ_FLAG_HIDDEN; g_flags[OBJ_BACKDROP] |= LV_OBJ_FLAG_HIDDEN; }
}

static void unlock_as(lcd_pin_role_t role, uint32_t at_ms)
{
    g_now = at_ms;
    lcd_lock_grant(&s_lock, role, at_ms);
}

static void test_init_and_has_role(void)
{
    TEST_SECTION("ui_lcd_lock_init / has_role: registration, auth-off collapse, tier ordering");
    fresh(false, 60);
    TEST_CHECK(g_timer_creates == 1 && g_timer_period == 1000, "one 1000 ms tick timer created (not re-created on a second init)");
    TEST_CHECK(g_touch_cb == on_touch_pressed, "touch-pressed hook registered on the indev");
    TEST_CHECK(s_lock.timeout_s == 60 && s_lock.last_activity_ms == 1000, "state init takes policy timeout and current tick");
    TEST_CHECK(lcd_lock_is_locked(&s_lock), "boots locked");

    TEST_CHECK(ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN) && ui_lcd_lock_has_role(LCD_PIN_ROLE_USER), "auth OFF: every tier allowed even while locked");

    g_policy.enabled = true;
    TEST_CHECK(!ui_lcd_lock_has_role(LCD_PIN_ROLE_USER) && !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN), "auth ON, locked: nothing allowed");
    unlock_as(LCD_PIN_ROLE_USER, 1000);
    TEST_CHECK(ui_lcd_lock_has_role(LCD_PIN_ROLE_USER), "USER session satisfies USER");
    TEST_CHECK(!ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN), "USER session does not satisfy ADMIN");
    unlock_as(LCD_PIN_ROLE_ADMIN, 1000);
    TEST_CHECK(ui_lcd_lock_has_role(LCD_PIN_ROLE_USER) && ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN), "ADMIN session satisfies both");

    ui_lcd_lock_force_lock();
    TEST_CHECK(!ui_lcd_lock_has_role(LCD_PIN_ROLE_USER) && !ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN),
               "a pending force-lock counts as locked immediately, before the tick consumes it");
    TEST_CHECK(!lcd_lock_is_locked(&s_lock), "has_role itself never touches s_lock");

    g_indev_ret = NULL;
    g_touch_cb = NULL;
    ui_lcd_lock_init();
    TEST_CHECK(g_touch_cb == NULL, "no indev: no touch hook, no crash");
    g_indev_ret = &g_indev;
}

static void test_note_activity_via_touch_hook(void)
{
    TEST_SECTION("touch hook extends the session");
    fresh(true, 60);
    unlock_as(LCD_PIN_ROLE_USER, 1000);
    g_now = 30000;
    g_touch_cb(NULL);
    TEST_CHECK(s_lock.last_activity_ms == 30000, "touch stamps last_activity with the tick");
    tick_at(80000); /* 50 s idle since the touch: still inside the 60 s window, before the prompt (50 s) edge */
    TEST_CHECK(!lcd_lock_is_locked(&s_lock), "still unlocked");
}

static void test_run_gated(void)
{
    TEST_SECTION("run_gated: runs when permitted, otherwise raises the keypad; insufficient PIN grants but does not run");
    fresh(true, 60);
    int tag = 0;
    unlock_as(LCD_PIN_ROLE_ADMIN, 1000);
    ui_lcd_lock_run_gated("Enter PIN", LCD_PIN_ROLE_ADMIN, action, &tag);
    TEST_CHECK(g_action_calls == 1 && g_action_ud == &tag && g_keypad_show_calls == 0, "permitted: action runs now with its user_data, no keypad");
    ui_lcd_lock_run_gated("Enter PIN", LCD_PIN_ROLE_ADMIN, NULL, NULL);
    TEST_CHECK(g_action_calls == 1, "NULL action tolerated");

    fresh(true, 60);
    ui_lcd_lock_run_gated("Admin PIN", LCD_PIN_ROLE_ADMIN, action, &tag);
    TEST_CHECK(g_action_calls == 0 && g_keypad_show_calls == 1 && strcmp(g_keypad_prompt, "Admin PIN") == 0, "locked: keypad raised with the prompt, action deferred");
    ui_lcd_lock_run_gated("Second", LCD_PIN_ROLE_ADMIN, action, NULL);
    TEST_CHECK(g_keypad_show_calls == 1 && strcmp(g_keypad_prompt, "Admin PIN") == 0, "keypad already open: the second request is dropped, first context kept");

    g_now = 5000;
    g_keypad_done(true, LCD_PIN_ROLE_USER, NULL);
    TEST_CHECK(g_action_calls == 0, "USER PIN against an ADMIN action: action does NOT run");
    TEST_CHECK(!lcd_lock_is_locked(&s_lock) && s_lock.granted_role == LCD_PIN_ROLE_USER && s_lock.last_activity_ms == 5000,
               "...but the session is granted at USER tier with a fresh activity stamp");

    g_keypad_open = false;
    ui_lcd_lock_run_gated("Admin PIN", LCD_PIN_ROLE_ADMIN, action, &tag);
    TEST_CHECK(g_keypad_show_calls == 2, "role upgrade raises the keypad again");
    g_keypad_done(true, LCD_PIN_ROLE_ADMIN, NULL);
    TEST_CHECK(g_action_calls == 1 && g_action_ud == &tag && s_lock.granted_role == LCD_PIN_ROLE_ADMIN, "ADMIN PIN: action runs with the context captured at raise time");

    fresh(true, 60);
    ui_lcd_lock_run_gated("PIN", LCD_PIN_ROLE_USER, action, &tag);
    g_keypad_done(false, LCD_PIN_ROLE_NONE, NULL);
    TEST_CHECK(g_action_calls == 0 && lcd_lock_is_locked(&s_lock), "denied/cancelled keypad: nothing runs, still locked");

    fresh(false, 60);
    ui_lcd_lock_run_gated("PIN", LCD_PIN_ROLE_ADMIN, action, &tag);
    TEST_CHECK(g_action_calls == 1 && g_keypad_show_calls == 0, "auth OFF: runs immediately, never a keypad");
}

static void test_prompt_and_expiry(void)
{
    TEST_SECTION("tick: stay-unlocked prompt window, countdown text, dismiss vs accept, inactivity expiry edge");
    fresh(true, 60);
    unlock_as(LCD_PIN_ROLE_ADMIN, 1000);
    g_keypad_open = true; /* a keypad left open behind the session */
    g_confirm_open = true;

    tick_at(40000);
    TEST_CHECK(!prompt_visible() && g_relock_calls == 0, "39 s idle: no prompt, no relock");
    tick_at(51000);
    TEST_CHECK(prompt_visible(), "50 s idle (10 s before expiry): prompt shown");
    TEST_CHECK(strcmp(g_label_text, "Locking in 10 s. Tap Stay Unlocked to continue.") == 0, "countdown text exact at 10 s");
    tick_at(56001);
    TEST_CHECK(strcmp(g_label_text, "Locking in 5 s. Tap Stay Unlocked to continue.") == 0, "countdown rounds up: 4.999 s left reads 5");
    TEST_CHECK(g_mbox_creates >= 1 && g_btn_cb == prompt_stay_unlocked_cb && g_backdrop_cb == prompt_backdrop_click_cb, "button and backdrop callbacks wired");

    g_backdrop_cb(NULL);
    TEST_CHECK(!prompt_visible() && s_lock.last_activity_ms == 1000, "backdrop tap dismisses WITHOUT extending the session");

    g_btn_cb(NULL);
    TEST_CHECK(s_lock.last_activity_ms == 56001 && !prompt_visible(), "Stay Unlocked extends and closes");

    g_now = 56001 + 60000;
    g_timer_cb(NULL);
    TEST_CHECK(lcd_lock_is_locked(&s_lock), "idle for the full timeout after the extension: locked");
    TEST_CHECK(g_relock_calls == 1, "relock callback fires exactly once on the unlocked->locked edge");
    TEST_CHECK(!g_keypad_open && g_keypad_close_calls >= 1, "stranded keypad force-closed");
    TEST_CHECK(!g_confirm_open && g_confirm_close_calls == 1, "open confirm dialog closed on the edge");
    TEST_CHECK(!prompt_visible(), "prompt closed");
    tick_at(56001 + 61000);
    TEST_CHECK(g_relock_calls == 1, "staying locked does not re-fire the relock callback");
}

static void test_never_timeout(void)
{
    TEST_SECTION("tick: timeout 'never' keeps the session; a live policy timeout change is picked up without reboot");
    fresh(true, LCD_LOCK_TIMEOUT_NEVER);
    unlock_as(LCD_PIN_ROLE_ADMIN, 1000);
    tick_at(10000000);
    TEST_CHECK(!lcd_lock_is_locked(&s_lock) && !prompt_visible() && g_relock_calls == 0, "never: still unlocked after ~2.7 h idle");
    g_policy.timeout_s = 60;
    tick_at(10000001);
    TEST_CHECK(s_lock.timeout_s == 60, "new timeout adopted on the next tick");
    TEST_CHECK(lcd_lock_is_locked(&s_lock) && g_relock_calls == 1, "and, long idle, it locks at once with one relock");
}

static void test_force_lock_hand_off(void)
{
    TEST_SECTION("force_lock: httpd-task request consumed on the next tick, one relock edge, LCD-19 keypad exemption is one-shot");
    fresh(true, 600);
    unlock_as(LCD_PIN_ROLE_ADMIN, 1000);
    tick_at(2000);
    TEST_CHECK(g_relock_calls == 0, "settled unlocked");
    g_confirm_open = true;
    ui_lcd_lock_force_lock();
    TEST_CHECK(!lcd_lock_is_locked(&s_lock), "request alone changes nothing");
    tick_at(3000);
    TEST_CHECK(lcd_lock_is_locked(&s_lock) && g_relock_calls == 1 && !g_confirm_open, "tick locks, fires relock once, closes the confirm dialog");
    TEST_CHECK(!atomic_load(&s_force_lock_requested), "flag cleared");

    /* a request that lands while already locked is a logged no-op, no extra edge */
    ui_lcd_lock_force_lock();
    tick_at(4000);
    TEST_CHECK(g_relock_calls == 1, "already locked: consumed, no second relock");

    /* LCD-19: keypad raised by a gated tap while a force-lock is pending survives the tick that applies it */
    fresh(true, 600);
    unlock_as(LCD_PIN_ROLE_USER, 1000);
    tick_at(2000);
    ui_lcd_lock_force_lock();
    ui_lcd_lock_run_gated("PIN", LCD_PIN_ROLE_ADMIN, action, NULL);
    TEST_CHECK(g_keypad_open && s_keypad_is_pending_lock_gate, "keypad raised under a pending lock is flagged as the PIN gate");
    tick_at(3000);
    TEST_CHECK(lcd_lock_is_locked(&s_lock) && g_relock_calls == 1, "lock applied");
    TEST_CHECK(g_keypad_open && g_keypad_close_calls == 0, "the fresh PIN gate keypad is NOT torn down by the edge it was raised for");
    TEST_CHECK(!s_keypad_is_pending_lock_gate, "exemption is one-shot");

    /* a role-upgrade keypad against a live session is NOT exempt */
    fresh(true, 600);
    unlock_as(LCD_PIN_ROLE_USER, 1000);
    tick_at(2000);
    ui_lcd_lock_run_gated("PIN", LCD_PIN_ROLE_ADMIN, action, NULL);
    TEST_CHECK(g_keypad_open && !s_keypad_is_pending_lock_gate, "upgrade prompt on a live session is not a lock gate");
    ui_lcd_lock_force_lock();
    tick_at(3000);
    TEST_CHECK(!g_keypad_open && g_keypad_close_calls == 1, "so a later revoking lock closes it");
}

static void test_policy_disabled_and_reenabled(void)
{
    TEST_SECTION("policy off: session torn down every tick; re-enable finds the panel locked (reset-one-side-of-a-pair)");
    fresh(true, 600);
    unlock_as(LCD_PIN_ROLE_ADMIN, 1000);
    tick_at(2000);
    g_policy.enabled = false;
    g_keypad_open = true;
    tick_at(3000);
    TEST_CHECK(lcd_lock_is_locked(&s_lock), "auth off: stale ADMIN session cleared");
    TEST_CHECK(!g_keypad_open && g_keypad_close_calls == 1, "auth off: open keypad closed");
    TEST_CHECK(g_relock_calls == 0, "auth off: panel is effectively unlocked, so no relock page kick");
    TEST_CHECK(ui_lcd_lock_has_role(LCD_PIN_ROLE_ADMIN), "auth off: has_role still full access");

    g_policy.enabled = true;
    TEST_CHECK(!ui_lcd_lock_has_role(LCD_PIN_ROLE_USER), "re-enabled: locked, no residual ADMIN grant");
    tick_at(4000);
    TEST_CHECK(g_relock_calls == 1, "re-enable reads as an unlocked->locked edge on its first tick (page returns home)");
    tick_at(5000);
    TEST_CHECK(g_relock_calls == 1, "and only once");
}

int main(void)
{
    test_init_and_has_role();
    test_note_activity_via_touch_hook();
    test_run_gated();
    test_prompt_and_expiry();
    test_never_timeout();
    test_force_lock_hand_off();
    test_policy_disabled_and_reenabled();
    printf("ui_lcd_lock: %d checks, %d failures\n", g_test_count, g_test_failures);
    return g_test_failures ? 1 : 0;
}
