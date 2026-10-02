/* Start/Stop/Pause action handlers for the home page (ui_page_home.c) --
 * split out 2026-09-04, ROADMAP.md M15's 1500-line item. Confirm dialogs,
 * the merged fire/pause buttons' event callbacks, the topbar Menu-gear nav
 * callback, and the small build_button() helper used to construct them.
 * See ui_page_home_internal.h for the shared statics/prototypes this file
 * reaches across the split, and ui_page_home.c's own header comment for the
 * page's overall design history. */
#include "ui_page_home_internal.h"
#include "ui_lcd_lock.h"
#include "ui_page_edit_firing.h" /* owner request 2026-09-28 -- Edit-firing button */
#include "ui_page_live_decide.h" /* "Keep?" -- end-of-run Discard/Save as/Overwrite */
#include "profiles_live_http.h" /* profiles_live_decide_status() */
#include "ui_page_profile_picker.h" /* ui_page_profile_picker_pick_refresh() -- UI_PLAN.md 6.1 */
#include "profiles_builtin.h" /* profiles_builtin_get(), PROFILE_BUILTIN_ID_BASE -- UI_PLAN.md 6.1 */
#include "hal_time.h" /* hal_time_now_us() -- auth_reset_gesture's now_ms argument */
#include <stdlib.h> /* free() -- exec-status heap-alloc pattern below */
#include "esp_heap_caps.h" /* heap_caps_malloc() */

/* UI_PLAN.md 6.1 -- the profile the operator picked on the LCD this boot.
 * There is no persisted "selected profile" anywhere in this firmware: before
 * 6.1 the Start button resolved what to run from the executor snapshot, else
 * the run_state boot record, and an operator who wanted anything else had to
 * use the web dashboard. The picker adds a third, highest-priority source
 * for the IDLE case, deliberately RAM-only and boot-scoped -- writing it to
 * NVS would mean inventing a new persisted key and a new schema, which 6.1
 * does not ask for and which would collide with run_state's own record. */
static bool    s_ui_home_picked_valid;
static uint8_t s_ui_home_picked_id;

/* Shared by the Start button and by the dashboard's profile-name label
 * (ui_home_profile_label_refresh()), so the two can never disagree about
 * which profile Start would actually run -- taking the caller's already-held
 * snapshot rather than fetching a second one on the LVGL tick.
 *
 * Note the IDLE branch is not optional: profile_executor_get_status()
 * memsets its output and fills profile_id ONLY when the state is not IDLE
 * (profile_executor_status.c), so st->profile_id reads 0 for every idle
 * board. Trusting it while idle would name user slot 0 no matter what is
 * actually selected. */
bool ui_home_resolve_profile_id(const profile_exec_status_t *st, uint8_t *out_id)
{
    if (st->state != PROFILE_EXEC_IDLE) {
        *out_id = st->profile_id;
        return true;
    }
    if (s_ui_home_picked_valid) {
        *out_id = s_ui_home_picked_id;
        return true;
    }
    run_state_record_t rec;
    if (run_state_get_boot_record(&rec)) {
        *out_id = rec.profile_id;
        return true;
    }
    return false;
}

/* Out-of-line on purpose: profile_t is this firmware's largest UI-reachable
 * value type (PROFILE_MAX_SEGMENTS segments), and the lvgl task's 4880 B
 * ceiling has very little headroom, so it lives in its own frame rather than
 * in ui_home_profile_label_refresh()'s alongside run_state_record_t. Neither
 * getter locks or touches NVS on this path: profiles_builtin_get() reads a
 * const table and profiles_http_get() copies from the resident RAM store. */
bool ui_home_profile_name_for_id(uint8_t id, char *out, size_t out_cap)
{
    profile_t prof;
    bool found = (id >= PROFILE_BUILTIN_ID_BASE) ? profiles_builtin_get(id, &prof)
                                                  : profiles_http_get(id, &prof);
    if (!found) {
        return false;
    }
    snprintf(out, out_cap, "%s", prof.name);
    return out[0] != '\0';
}

static bool ui_home_resolve_start_profile_id(uint8_t *out_id)
{
    /* Runs on the LVGL task (button press callback) -- heap-allocate rather
     * than add a 1384-byte profile_exec_status_t stack local (see
     * ui_page_home_refresh.c's own comment on this task's measured stack
     * ceiling for why it is treated as tight, not generous). Needs the full
     * struct's .state and .profile_id, so the narrow
     * profile_executor_get_active_id() accessor does not fit here. */
    profile_exec_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!st) {
        return false; /* out of memory -- treat as "no known profile id" */
    }
    profile_executor_get_status(st);
    bool found = ui_home_resolve_profile_id(st, out_id);
    free(st);
    return found;
}

static void ui_home_do_start(void)
{
    /* Same action function dashboard_http.c's POST /api/profile_exec/start
     * handler calls (profile_exec_start_post_handler()) -- TODO.md 10.1a.
     *
     * No picker on this page anymore (see this file's header comment for
     * why) -- always the fallback chain: whatever's currently known this
     * boot (non-idle profile_id), else the last boot record. An operator
     * who wants a *different* profile than either of those has to use the
     * web dashboard's picker. */
    uint8_t id = 0;
    if (!ui_home_resolve_start_profile_id(&id)) {
        ESP_LOGW(UI_HOME_TAG, "Start pressed with no known profile id -- nothing has run this boot "
                      "and no picker on this page (TODO.md 10.3's no-scroll rewrite)");
        return;
    }

    char err_msg[160] = ""; /* 64 -> 160, 2026-09-09: the readiness interlock's refusals (readiness_gate.h) name an item AND a remedy; at 64 the remedy was cut off. */
    if (!profile_executor_run(id, err_msg, sizeof(err_msg))) {
        ESP_LOGW(UI_HOME_TAG, "profile_executor_run(%u) refused: %s", id, err_msg);
        /* Same "Cannot Start" modal ui_page_profile_detail.c's confirm_start_cb()
         * shows -- reusing ui_confirm_show() rather than a second copy so the
         * operator sees the refusal reason instead of a Start tap that appears
         * to do nothing (found in review of e6fd5ed3). */
        ui_confirm_params_t err_params = {
            .title = "Cannot Start",
            .body = err_msg[0] ? err_msg : "Refused for an unknown reason.",
            .confirm_label = "OK",
            .confirm_color = UI_THEME_COLOR_CARD,
            .on_confirm = NULL,
            .user_data = NULL,
        };
        ui_confirm_show(&err_params);
    }
}

static void ui_home_do_stop(void)
{
    /* Same action function dashboard_http.c's POST /api/profile_exec/stop
     * handler calls -- TODO.md 10.1a. profile_executor_halt() itself is the
     * only gate on a stop today -- no confirmation dialog exists anywhere in
     * this call path (see this file's header/report note: flagged back to
     * the requester rather than silently added here). */
    profile_executor_halt();
}

/* ---- Start/Stop confirmation overlay --------------------------------
 *
 * Both actions were firing immediately with no confirmation anywhere in this
 * call path (ui_home_do_start()/ui_home_do_stop()'s own header comments used to flag this
 * back to the requester rather than silently adding one). Starting energises
 * heaters for hours; stopping mid-firing aborts a load. Both now go through
 * a modal confirm/cancel step first, via the shared ui_confirm.c helper
 * (factored out of what used to be a hand-rolled lv_msgbox pair here, so the
 * new Profiles-hub detail page's START action -- see ui_page_profile_detail.c
 * -- reuses the exact same dialog instead of a second copy-pasted
 * implementation). See ui_confirm.h for the FLEX TRAP / cancel-safe-by-
 * default rationale that used to live in this comment. */

/* Second dialog's Yes -- the operator has now read the watchdog-panic-
 * disabled warning too. Same chaining pattern as ui_page_kiln_cfg_setup.c's
 * apply_no_safety_ack_cb()/apply_confirm_yes_cb() pair for its own
 * no-safety-processor warning. */
static void ui_home_confirm_start_watchdog_ack_cb(void *user_data)
{
    (void)user_data;
    ui_home_do_start();
}

static void ui_home_confirm_start_yes_cb(void *user_data)
{
    (void)user_data;

    /* watchdog_cfg.h: a second, EXTRA confirmation on top of the one the
     * operator just answered, only when the task-watchdog panic is
     * currently disabled (dev/bench mode) -- same text on this dialog, the
     * web start flow (main_page.html), and watchdog_cfg.h's own macro
     * definition; App/test/lint_pages.js pins the web copy against the C
     * macro so they cannot drift. */
    if (watchdog_cfg_panic_disabled()) {
        ui_confirm_params_t warn = {
            .title = "Task-Watchdog Panic Disabled",
            .body = WATCHDOG_CFG_FIRING_WARNING,
            .confirm_label = "Start Anyway",
            .confirm_color = UI_THEME_ACCENT_5,
            .on_confirm = ui_home_confirm_start_watchdog_ack_cb,
            .user_data = NULL,
        };
        ui_confirm_show(&warn);
        return;
    }

    ui_home_do_start();
}

static void ui_home_confirm_stop_yes_cb(void *user_data)
{
    (void)user_data;
    ui_home_do_stop();
}

static void ui_home_show_start_confirm(void)
{
    uint8_t id = 0;
    bool have_id = ui_home_resolve_start_profile_id(&id);

    profile_t prof;
    bool have_prof = have_id && profiles_http_get(id, &prof);

    char body[256];
    if (have_prof) {
        /* Zones this profile drives -- cheap here (profiles_http_get() is a
         * plain NVS-backed struct copy, same call profile_executor.c itself
         * uses to run the profile, not a second read path), unlike trying to
         * derive it from profile_exec_status_t, which only carries a
         * meaningful zone_mask once the run has actually started. */
        char zones_buf[96];
        size_t zlen = 0;
        zones_buf[0] = '\0';
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT && zlen < sizeof(zones_buf) - 1; zi++) {
            if (!(prof.zone_mask & (1u << zi))) {
                continue;
            }
            char name[16];
            const char *zname = (zones_config_get_name(zi, name, sizeof(name)) && name[0]) ? name : NULL;
            char piece[24];
            if (zname) {
                snprintf(piece, sizeof(piece), "%s%s", zlen ? ", " : "", zname);
            } else {
                snprintf(piece, sizeof(piece), "%sZone %u", zlen ? ", " : "", (unsigned)zi);
            }
            size_t piece_len = strlen(piece);
            if (zlen + piece_len < sizeof(zones_buf)) {
                memcpy(zones_buf + zlen, piece, piece_len + 1);
                zlen += piece_len;
            }
        }
        snprintf(body, sizeof(body), "Start \"%s\" now? This will energise %s for the duration of the "
                                      "firing, which can be hours.",
                 prof.name, zones_buf[0] ? zones_buf : "no zones");
    } else if (have_id) {
        /* Have an id but profiles_http_get() failed (slot no longer stored,
         * e.g. deleted between boot and now) -- name what we can rather than
         * a blank, and say plainly the rest could not be read. */
        snprintf(body, sizeof(body), "Start profile id %u now? Its saved details could not be read, "
                                      "but starting will energise heaters for the duration of the "
                                      "firing, which can be hours.",
                 (unsigned)id);
    } else {
        /* No non-idle profile this boot and no boot record either -- the
         * honest answer is "nothing to name," not a blank dialog. */
        snprintf(body, sizeof(body), "No profile can be identified to start (nothing has run yet this "
                                      "boot). Use the web dashboard's profile picker to choose one.");
    }

    ui_confirm_params_t params = {
        .title = "Confirm Start",
        .body = body,
        .confirm_label = "Start",
        .confirm_color = UI_THEME_ACCENT_4, /* start-green, matches the fire button */
        .on_confirm = ui_home_confirm_start_yes_cb,
        .user_data = NULL,
    };
    ui_confirm_show(&params);
}

static void ui_home_show_stop_confirm(void)
{
    ui_confirm_params_t params = {
        .title = "Confirm Stop",
        .body = "Stop this firing now? This aborts the run in progress -- it cannot "
                "be resumed, and the load will not finish firing.",
        .confirm_label = "Stop",
        .confirm_color = UI_THEME_ACCENT_5, /* stop-red, matches the fire button */
        .on_confirm = ui_home_confirm_stop_yes_cb,
        .user_data = NULL,
    };
    ui_confirm_show(&params);
}

/* Merged Start/Stop button (single user-visible request: "the start stop
 * button should be one button on the lcd"). Idle/Done/Faulted -> "Start" +
 * confirm -> ui_home_do_start(); Running/Paused -> "Stop" + confirm -> ui_home_do_stop().
 * One callback reads current state at click time rather than two callbacks
 * each assuming a fixed action, so a state change between ui_home_refresh_cb() ticks
 * and the actual tap can never fire the stale action -- the same reasoning
 * now also decides which of the two confirmation dialogs to show. */
static void ui_home_show_start_confirm_gated_cb(void *user_data)
{
    (void)user_data;
    ui_home_show_start_confirm();
}

static void ui_home_show_stop_confirm_gated_cb(void *user_data)
{
    (void)user_data;
    ui_home_show_stop_confirm();
}

void ui_home_fire_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Only "is a firing active" is needed here, not which fields -- use the
     * narrow accessor profile_executor.h recommends over a 1384-byte
     * profile_exec_status_t stack local (this runs on the lvgl task, whose
     * own 2026-09-04 stack-corruption panic is exactly why this file's
     * budget is tracked by check_all_task_stack_budgets.py). */
    uint8_t active_id = 0;
    if (profile_executor_get_active_id(&active_id)) {
        /* Owner decision, 2026-09-28 (reverses the old "Stop is never
         * gated" rule that used to sit here): Stop now requires the same PIN
         * as Start. The owner's own words: "stop needs login. there is an
         * estop button" -- the hardware E-stop (docs/SAFETY_CASE.md H7) is
         * the independent safety backstop, not this keypad, so an LCD PIN
         * gate no longer needs to stay clear of Stop.
         * tools/check_stop_path_requires_pin.ps1 enforces this mechanically. */
        ui_lcd_lock_run_gated("Enter PIN to stop firing", LCD_PIN_ROLE_USER,
                               ui_home_show_stop_confirm_gated_cb, NULL);
    } else {
        ui_lcd_lock_run_gated("Enter PIN to start firing", LCD_PIN_ROLE_USER,
                               ui_home_show_start_confirm_gated_cb, NULL);
    }
}

/* Merged Pause/Resume button -- owner request 2026-08-30: mirrors app.js's
 * sticky-bar toggle on the web dashboard (one button, label/action flips
 * with state) rather than two separate buttons. Reads state at action time
 * (inside the PIN-gated callback) for the same stale-tap reason
 * ui_home_fire_btn_cb() does. No confirmation dialog
 * -- neither web surface asks before pausing or resuming, only before
 * Start/Stop. */
static void ui_home_pause_resume_gated_cb(void *user_data)
{
    (void)user_data;
    /* Runs on the LVGL task -- heap-allocate rather than add another
     * 1384-byte profile_exec_status_t stack local; needs the full struct's
     * .state to distinguish RUNNING from PAUSED, which the narrow
     * profile_executor_get_active_id() accessor cannot report (it collapses
     * both into a single "active" bool). */
    profile_exec_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!st) {
        return; /* out of memory -- skip this tap, next one tries again */
    }
    profile_executor_get_status(st);
    if (st->state == PROFILE_EXEC_RUNNING) {
        profile_executor_pause();
    } else if (st->state == PROFILE_EXEC_PAUSED) {
        profile_executor_resume();
    }
    free(st);
}

void ui_home_pause_resume_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Owner decision, 2026-09-28: every LCD action other than viewing the
     * dashboard needs the PIN -- Pause/Resume included, same as Start/Stop.
     * State is read inside the gated callback (after any PIN entry), never
     * here, so a firing that changed state while the keypad was open is
     * acted on as it is now, not as it was at the tap.
     * tools/check_lcd_home_nav_gated.ps1 enforces this mechanically. */
    ui_lcd_lock_run_gated("Enter PIN to pause/resume", LCD_PIN_ROLE_USER,
                           ui_home_pause_resume_gated_cb, NULL);
}

/* Owner request 2026-09-28: the LCD equivalent of the web's "Edit firing"
 * button (d484e51a) next to Start/Stop -- see ui_page_edit_firing.c. */
static void ui_home_edit_btn_gated_cb(void *user_data)
{
    (void)user_data;
    /* While a firing runs this is "Edit"; once it has ended with a working
     * copy still owed a decision the same button reads "Keep?" and opens the
     * Discard / Save as / Overwrite page instead (same shared decide API as
     * the web). Re-derived here, after the PIN, not trusted from the tap. */
    profiles_live_decide_status_t ds;
    profiles_live_decide_status(&ds);
    if (ds.pending_decision) {
        ui_page_live_decide_prepare();
        kiln_ui_show("live_decide");
        return;
    }
    ui_page_edit_firing_prepare();
    kiln_ui_show("edit_firing");
}

void ui_home_edit_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Owner decision 2026-09-28: every LCD action other than viewing the
     * dashboard needs the PIN. tools/check_lcd_home_nav_gated.ps1 enforces
     * this mechanically. */
    ui_lcd_lock_run_gated("Enter PIN to edit firing", LCD_PIN_ROLE_USER,
                           ui_home_edit_btn_gated_cb, NULL);
}

static void ui_home_menu_nav_gated_cb(void *user_data)
{
    (void)user_data;
    ui_page_config_reset_to_first_page();
    kiln_ui_show("config");
}

void ui_home_menu_nav_cb(lv_event_t *e)
{
    (void)e;
    /* Single "Menu" button replaces the old separate Configuration and
     * Temperature nav buttons -- see this file's header comment on the
     * budget this page is fit to. Temperature (manual relay control) is now
     * reached via ui_page_config.c's hub, one tap further than before.
     *
     * Owner decision, 2026-09-28: every page reachable off the home
     * dashboard other than the dashboard itself now requires the PIN (see
     * ui_lcd_lock.h's ui_lcd_lock_run_gated() doc comment) -- this is one of
     * the two gated exits off "home" that comment names.
     *
     * Rewind the hub first: it is built once and keeps its paging position,
     * so without this, Menu drops you on whichever hub page you were last on.
     * Back from a sub-page deliberately still returns to the page you left
     * from -- only Menu means "take me to the top of the menu". Rewinding
     * happens inside the gated callback so a refused/cancelled PIN prompt
     * never resets the hub's paging position for nothing. */
    ui_lcd_lock_run_gated("Enter PIN to open Menu", LCD_PIN_ROLE_USER,
                           ui_home_menu_nav_gated_cb, NULL);
}

/* UI_PLAN.md 6.1: tap the profile name left of Start to open the picker in
 * pick mode ("profile_picker" -- kiln_ui_register_page() in kiln_ui.c wires
 * that name to ui_page_profile_picker_build_pick(); "profiles" is the
 * separate unified manage list reached from the menu instead).
 *
 * Refresh before showing, for the same reason ui_page_config.c's
 * profiles_nav_cb() does: kiln_ui_show() caches the page after its first
 * build, so without this the pick list would be frozen at whatever the
 * profile set was the first time this button was pressed, surviving any
 * later create/delete/import made from the builder or the web dashboard. */
static void ui_home_profile_btn_gated_cb(void *user_data)
{
    (void)user_data;
    ui_page_profile_picker_pick_refresh();
    kiln_ui_show("profile_picker");
}

void ui_home_profile_btn_cb(lv_event_t *e)
{
    (void)e;
    /* Owner decision, 2026-09-28: the other gated exit off "home" (see
     * ui_home_menu_nav_cb() above and ui_lcd_lock.h's doc comment) -- the
     * refresh happens inside the gated callback for the same
     * cancel-should-cost-nothing reason. */
    ui_lcd_lock_run_gated("Enter PIN to pick a profile", LCD_PIN_ROLE_USER,
                           ui_home_profile_btn_gated_cb, NULL);
}

/* UI_PLAN.md 6.1 -- the picker's PICK-mode selection callback, registered
 * once from ui_page_home_build(). Records the choice for this boot and
 * returns to the dashboard; it deliberately does NOT start anything, so the
 * Start button (and its confirmation dialog, and its PIN gate) remains the
 * only way to energise elements. The id is stored unresolved: a profile
 * deleted between the pick and the Start is caught by
 * ui_home_show_start_confirm()'s existing "have an id but the getter failed"
 * branch, exactly as a stale boot record already is. */
void ui_home_profile_picked_cb(uint8_t profile_id)
{
    s_ui_home_picked_id = profile_id;
    s_ui_home_picked_valid = true;
    kiln_ui_show("home");
}

/* out_label, if non-NULL, receives the button's label widget so a caller can
 * change its text/color later (the merged fire button's state-driven text --
 * see ui_home_fire_btn_cb()/ui_home_refresh_cb()).
 *
 * height_px, added 2026-08-21 (single user-visible request: "the start
 * button should sit at the bottom and be about half as tall so the graph can
 * be larger" -- see ui_page_home_build()'s action-row comment for why a
 * drawn height below UI_THEME_MIN_TOUCH_TARGET_PX is acceptable here and how
 * the effective touch area is kept whole regardless). Every other caller of
 * this helper used to get UI_THEME_MIN_TOUCH_TARGET_PX unconditionally; this
 * page has exactly one caller (the merged fire button), so rather than add a
 * second helper for "small button," the one call site now just says what
 * height it wants. */
lv_obj_t *ui_home_build_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_event_cb_t cb,
                               int32_t height_px, lv_obj_t **out_label)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_height(btn, height_px);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_style_bg_color(btn, bg, 0);
    lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *label = lv_label_create(btn);
    lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(label, text);
    lv_obj_center(label);
    if (out_label) {
        *out_label = label;
    }

    /* TODO.md 10.4's touch hit-area helper (ui_theme.c) -- this is a sparse
     * button row, not a dense grid, so compact_layout=false. Forces an
     * immediate layout pass first: ui_theme_apply_touch_area() reads back
     * lv_obj_get_width/height(), which flex_grow leaves unresolved until
     * layout actually runs. */
    lv_obj_update_layout(btn);
    ui_theme_apply_touch_area(btn, false);

    return btn;
}

/* Piecewise-linear sample of a profile_feasibility_plan_curve() point list at
 * time t (seconds from run start). pts[] is ordered by increasing t (the
 * order profile_feasibility_plan_curve() writes them in) -- t before the
 * first point holds the first point's value (there is no "before start"),
 * and t past the last point holds the LAST point's value (the chart's
 * horizon is derived from the last point's own time, so this only matters
 * for a t that lands exactly on it due to float rounding). A zero-width
 * step (t1<=t0, an unknown-duration ramp segment -- see
 * profile_feasibility.h's HONESTY RULE) holds the step's arrival value
 * rather than dividing by zero. Returns NAN if pts is empty. */

/* docs/WEB_AUTH_PLAN.md item 10 -- confirm-step callback for the physical
 * credential-reset gesture. Called AFTER ui_confirm_show()'s dialog is
 * already closed (see ui_confirm.h's doc comment), so it is safe to log and
 * mutate the singleton gesture state directly here. */
static void ui_home_auth_reset_confirm_yes_cb(void *user_data)
{
    (void)user_data;
    auth_reset_gesture_state_t *gesture = auth_reset_gesture_singleton();
    uint32_t now_ms = (uint32_t)(hal_time_now_us() / 1000);
    auth_reset_gesture_confirm_result_t result = auth_reset_gesture_confirm(gesture, now_ms);
    switch (result) {
        case AUTH_RESET_CONFIRM_OK:
            ESP_LOGE(UI_HOME_TAG,
                     "**********************************************************************");
            ESP_LOGE(UI_HOME_TAG,
                     "* PHYSICAL AUTH RESET CONFIRMED -- administrator credential cleared.   *");
            ESP_LOGE(UI_HOME_TAG,
                     "* Web auth stays ENABLED; no administrator password is now configured. *");
            ESP_LOGE(UI_HOME_TAG,
                     "* Login must now force a set-a-new-password flow (WEB_AUTH_PLAN.md      *");
            ESP_LOGE(UI_HOME_TAG,
                     "* section 11, owned separately) -- this is NOT a lockout or a bypass.   *");
            ESP_LOGE(UI_HOME_TAG,
                     "**********************************************************************");
            break;
        case AUTH_RESET_CONFIRM_NOT_ARMED:
            ESP_LOGE(UI_HOME_TAG, "Auth reset confirm pressed but gesture was not armed -- ignored.");
            break;
        case AUTH_RESET_CONFIRM_EXPIRED:
            ESP_LOGE(UI_HOME_TAG,
                     "Auth reset confirm window (30 s) expired before confirm was pressed -- "
                     "no credential change. Re-arm by repeating all four corner taps.");
            break;
        case AUTH_RESET_CONFIRM_NOT_WIRED:
            ESP_LOGE(UI_HOME_TAG,
                     "Auth reset armed and confirmed but clear_credentials_fn is NULL -- the "
                     "credential store was never wired to the gesture. No change made.");
            break;
        case AUTH_RESET_CONFIRM_CLEAR_FAILED:
            ESP_LOGE(UI_HOME_TAG,
                     "Auth reset armed and confirmed but the credential-store clear FAILED its "
                     "own read-back verification -- no credential change confirmed. Retry by "
                     "re-arming the gesture.");
            break;
    }
}

/* One shared handler for all four corner hit zones (see
 * ui_page_home_internal.h's doc comment on this prototype). Reads the three
 * live preconditions fresh on every tap, per auth_reset_gesture.h's own
 * contract, then feeds the tap into the board-wide singleton gesture state.
 * The ARMED transition is the only one that needs action here beyond
 * logging: it pops the confirm dialog immediately (plan item 10 step 4).
 * The persistent "AUTH RESET ARMED" banner and the cancel/expiry logging
 * live on the existing 1 Hz refresh tick instead (ui_page_home_refresh.c)
 * -- see that file for why (ui_confirm.h's Cancel button has no callback
 * hook to log from directly, so natural 30 s expiry, observed on the next
 * tick after it happens, is the mechanism actually used for "cancel"). */
void ui_home_auth_reset_corner_tap_cb(lv_event_t *e)
{
    auth_reset_gesture_corner_t corner =
        (auth_reset_gesture_corner_t)(intptr_t)lv_event_get_user_data(e);

    bool estop_asserted = dashboard_http_estop_asserted();
    /* Only "is a firing active" is needed here -- narrow accessor instead of
     * a 1384-byte profile_exec_status_t stack local (this runs on the lvgl
     * task; see this file's own header comment on that task's stack
     * history). */
    uint8_t active_id = 0;
    bool firing_active = profile_executor_get_active_id(&active_id);
    bool heat_enabled = heat_enable_is_granted();
    uint32_t now_ms = (uint32_t)(hal_time_now_us() / 1000);

    auth_reset_gesture_state_t *gesture = auth_reset_gesture_singleton();
    auth_reset_gesture_tap_result_t result = auth_reset_gesture_on_corner_tap(
        gesture, corner, now_ms, estop_asserted, firing_active, heat_enabled);

    if (result == AUTH_RESET_TAP_ARMED) {
        ESP_LOGE(UI_HOME_TAG,
                 "**********************************************************************");
        ESP_LOGE(UI_HOME_TAG,
                 "* PHYSICAL AUTH RESET ARMED -- E-stop asserted, no firing active.       *");
        ESP_LOGE(UI_HOME_TAG,
                 "* Confirm within 30 s to clear the administrator credential. Auth stays *");
        ESP_LOGE(UI_HOME_TAG,
                 "* ENABLED either way -- see WEB_AUTH_PLAN.md section 10.                *");
        ESP_LOGE(UI_HOME_TAG,
                 "**********************************************************************");

        ui_confirm_params_t params = {
            .title = "Confirm Credential Reset",
            .body = "Clear the administrator password? Web auth stays ENABLED and no other "
                    "config changes. You will need to set a new administrator password on "
                    "next login.",
            .confirm_label = "Clear Credential",
            .confirm_color = UI_THEME_ACCENT_5, /* stop-red -- destructive-ish action */
            .on_confirm = ui_home_auth_reset_confirm_yes_cb,
            .user_data = NULL,
        };
        ui_confirm_show(&params);
    }
    /* IGNORED_PRECONDITIONS/IGNORED_ALREADY_ARMED/ABANDONED/PROGRESS all need
     * no action here -- they are silent-by-design per auth_reset_gesture.h's
     * own doc comment (a panel used for its safety function must not be
     * distracted by a stray tap outside the precondition window). */
}
