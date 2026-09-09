/* Start/Stop/Pause action handlers for the home page (ui_page_home.c) --
 * split out 2026-09-04, ROADMAP.md M15's 1500-line item. Confirm dialogs,
 * the merged fire/pause buttons' event callbacks, the topbar Menu-gear nav
 * callback, and the small build_button() helper used to construct them.
 * See ui_page_home_internal.h for the shared statics/prototypes this file
 * reaches across the split, and ui_page_home.c's own header comment for the
 * page's overall design history. */
#include "ui_page_home_internal.h"

static bool ui_home_resolve_start_profile_id(uint8_t *out_id)
{
    profile_exec_status_t st;
    profile_executor_get_status(&st);
    if (st.state != PROFILE_EXEC_IDLE) {
        *out_id = st.profile_id;
        return true;
    }
    run_state_record_t rec;
    if (run_state_get_boot_record(&rec)) {
        *out_id = rec.profile_id;
        return true;
    }
    return false;
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
void ui_home_fire_btn_cb(lv_event_t *e)
{
    (void)e;
    profile_exec_status_t st;
    profile_executor_get_status(&st);
    if (st.state == PROFILE_EXEC_RUNNING || st.state == PROFILE_EXEC_PAUSED) {
        ui_home_show_stop_confirm();
    } else {
        ui_home_show_start_confirm();
    }
}

/* Merged Pause/Resume button -- owner request 2026-08-30: mirrors app.js's
 * sticky-bar toggle on the web dashboard (one button, label/action flips
 * with state) rather than two separate buttons. Reads state at click time
 * for the same stale-tap reason ui_home_fire_btn_cb() does. No confirmation dialog
 * -- neither web surface asks before pausing or resuming, only before
 * Start/Stop. */
void ui_home_pause_resume_btn_cb(lv_event_t *e)
{
    (void)e;
    profile_exec_status_t st;
    profile_executor_get_status(&st);
    if (st.state == PROFILE_EXEC_RUNNING) {
        profile_executor_pause();
    } else if (st.state == PROFILE_EXEC_PAUSED) {
        profile_executor_resume();
    }
}

void ui_home_menu_nav_cb(lv_event_t *e)
{
    (void)e;
    /* Single "Menu" button replaces the old separate Configuration and
     * Temperature nav buttons -- see this file's header comment on the
     * budget this page is fit to. Temperature (manual relay control) is now
     * reached via ui_page_config.c's hub, one tap further than before.
     *
     * Rewind the hub first: it is built once and keeps its paging position,
     * so without this, Menu drops you on whichever hub page you were last on.
     * Back from a sub-page deliberately still returns to the page you left
     * from -- only Menu means "take me to the top of the menu". */
    ui_page_config_reset_to_first_page();
    kiln_ui_show("config");
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
