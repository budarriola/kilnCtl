#include "kiln_ui.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"

#include "ui_page_config.h"
#include "ui_page_diagnostics.h"
#include "ui_page_home.h"
#include "ui_page_network.h"
#include "ui_page_network_manage.h"
#include "ui_page_profile_builder_review.h"
#include "ui_page_profile_builder_segment.h"
#include "ui_page_profile_builder_zones.h"
#include "ui_page_profile_detail.h"
#include "ui_page_profile_segments.h"
#include "ui_page_profile_picker.h"
#include "ui_page_profiles.h"
#include "ui_page_temperature.h"
#include "ui_page_touch_cal.h"
#include "ui_page_touch_test.h"
#include "touch_cal_store.h"
#include "lvgl_port.h"
#include "ui_lcd_lock.h"
#include "lcd_credential_bridge.h"

/* lv_buttonmatrix_t's real fields (button_areas, btn_cnt) are declared in
 * this private header, not lv_buttonmatrix.h -- the public header only
 * exposes the opaque lv_buttonmatrix_class/lv_obj_t handle. Confirmed by
 * reading lv_buttonmatrix_private.h at
 * components/lvgl/src/widgets/buttonmatrix/lv_buttonmatrix_private.h: it
 * defines struct _lv_buttonmatrix_t with button_areas/ctrl_bits/btn_cnt. The
 * path below is relative to components/lvgl/src, which esp.cmake
 * (components/lvgl/env_support/cmake/esp.cmake) registers as one of this
 * component's public INCLUDE_DIRS alongside the component root -- the same
 * reason lv_buttonmatrix.c's own "widgets/buttonmatrix/..." style includes
 * resolve. This only reads the library's private struct layout; nothing
 * under components/lvgl/ is modified. */
#include "widgets/buttonmatrix/lv_buttonmatrix_private.h"

static const char *TAG = "kiln_ui";

/* A handful of pages (home, settings, temperature, config -- see TODO.md
 * 10.3's page list), not a dynamic set: a fixed array avoids pulling in
 * dynamic allocation for something this small and bounded. Raise this if
 * 10.3 ends up wanting more top-level pages than that.
 *
 * Raised 16 -> 24 this pass: the LCD Profiles tree (ui_page_profiles.c and
 * its five siblings, see this file's registrations below) added 6 pages to
 * the 11 that existed before it, which would have overflowed the old 16.
 *
 * Raised 24 -> 26: the new Kiln Setup hub ("kiln_setup") and Kiln Config
 * management screen ("kiln_cfg_setup") added 2 more pages, exactly filling
 * the old 24-page cap otherwise.
 *
 * 2026-08-22: "zones", "zones_detail", and "history" were removed (owner
 * request -- see this file's registration comments), freeing 3 slots.
 *
 * 2026-08-27: "board_health", "safety", "thermo_faults", "tc_types",
 * "kiln_setup", and "kiln_cfg_setup" were removed (three folded into
 * "diagnostics", three deleted outright -- see this file's registration
 * comments and ui_page_diagnostics.c's header comment), freeing 6 more
 * slots. Left at 26 rather than lowered further: the array is tiny (a few
 * dozen bytes per slot) and a future page addition should not have to
 * re-derive this arithmetic from scratch the next time the count changes. */
#define KILN_UI_MAX_PAGES 26

typedef struct {
    const char *name;          /* borrowed, see kiln_ui_register_page */
    kiln_ui_page_build_fn build;
    lv_obj_t *screen;          /* NULL until first shown */
} kiln_ui_page_t;

static kiln_ui_page_t s_pages[KILN_UI_MAX_PAGES];
static size_t s_page_count;
static const char *s_current_page_name;

/* Pull-based kiln_ui_show() entry/exit counters (2026-08-21) -- see
 * lvgl_port.c's touch-diag counters for the same rationale: a push-based log
 * line can be dropped by uart_log_bridge's queue during exactly the boot
 * burst under investigation, making "the line never printed" indistinguishable
 * from "the line printed but was dropped". These are read on demand over
 * TOUCH_CMD_GET_STATE instead, so a drop anywhere in the log pipeline can't
 * hide the answer to "does kiln_ui_show() ever reach its exit?". Both
 * single-writer from kiln_ui_show() itself (whichever task calls it -- today
 * always lvgl_port_task), read from the UART bridge task via the accessor
 * below. s_show_exits is incremented on the line immediately AFTER
 * lvgl_port_set_input_enabled(true) at the bottom of kiln_ui_show(), so
 * entries > exits proves a path out of that function returns (or ends up
 * stuck) before ever reaching the re-enable call. */
static volatile uint32_t s_show_entries;
static volatile uint32_t s_show_exits;

/* Gates the AUTOMATIC tap-target dump inside kiln_ui_show() (every page
 * switch) -- the explicit kiln_ui_log_tap_targets() call always dumps
 * regardless of this flag. Off by default: the automatic dump is what
 * flooded the log during boot/navigation churn, but the previous fix for
 * that (demoting the dump's ESP_LOGI calls to ESP_LOGD) was wrong -- this
 * project builds with CONFIG_LOG_MAXIMUM_LEVEL=3, which compiles ESP_LOGD out
 * of the binary entirely, so it didn't quiet the automatic dump, it deleted
 * the only way to find a widget's on-screen position (no framebuffer
 * readback exists on this panel). Gating the call site instead keeps the
 * capability in the binary at all times; kiln_ui_set_auto_tap_dump() (wired
 * to a TOUCH bridge subcommand in uart_bridge.c) is what turns it on from the
 * PC side when someone actually wants the per-navigation dump. */
static bool s_auto_tap_dump;

void kiln_ui_set_auto_tap_dump(bool enable)
{
    s_auto_tap_dump = enable;
    ESP_LOGI(TAG, "auto tap-target dump %s", enable ? "enabled" : "disabled");
}

static kiln_ui_page_t *find_page(const char *name)
{
    for (size_t i = 0; i < s_page_count; ++i) {
        if (strcmp(s_pages[i].name, name) == 0) return &s_pages[i];
    }
    return NULL;
}

esp_err_t kiln_ui_init(void)
{
    memset(s_pages, 0, sizeof(s_pages));
    s_page_count = 0;
    s_current_page_name = NULL;

    /* Built-in page 0, ui_page_home.c -- see kiln_ui.h: this file (kiln_ui.c)
     * is the registry/switcher only, every page's actual widget tree lives in
     * its own ui_page_*.c/.h so no single file accumulates every screen. */
    esp_err_t err = kiln_ui_register_page("home", ui_page_home_build);
    if (err != ESP_OK) return err;

    /* TODO.md 10.3's "Configuration"/"Temperature" nav items -- stubs for
     * now (see ui_page_config.c/.h, ui_page_temperature.c/.h), registered
     * here so ui_page_home.c's nav buttons have somewhere real to
     * kiln_ui_show() and the page-switching mechanism gets exercised by more
     * than the one page it's had until now. */
    err = kiln_ui_register_page("config", ui_page_config_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("temperature", ui_page_temperature_build);
    if (err != ESP_OK) return err;

    /* "board_health" (ui_page_board_health.c) was REMOVED 2026-08-27 --
     * folded into "diagnostics" below (owner request: "the diagnostics pages
     * should also contain the safty processor page, the board health, and
     * thermocouple fault page's info. remove the other 3 lcd pages when you
     * combine the info"). See ui_page_diagnostics.c's header comment for the
     * full content inventory of what moved where. */

    /* TODO.md 10.9's LCD-side Wi-Fi settings page, linked from
     * ui_page_config.c's "Network / Wi-Fi" nav item (previously a
     * "not built yet" placeholder row). */
    err = kiln_ui_register_page("network", ui_page_network_build);
    if (err != ESP_OK) return err;

    /* 2026-08-24 no-scroll budget split (TODO.md's "ui_page_network.c's
     * worst-case fit is ~268px against a ~264px budget" item): Scan/Saved/
     * Connect/Forget moved off "network" to their own page here, reachable
     * from a "Manage networks" button via kiln_ui_show("network_manage").
     * See ui_page_network_manage.c's header comment. */
    err = kiln_ui_register_page("network_manage", ui_page_network_manage_build);
    if (err != ESP_OK) return err;

    /* "safety" (ui_page_safety.c) was REMOVED 2026-08-27 -- folded into
     * "diagnostics" below, same owner request as "board_health" above. See
     * ui_page_diagnostics.c's header comment for the content inventory. */

    /* TODO.md's "Diagnostics / System info page" item -- ESP-only system
     * info (firmware version, uptime, heap, ESP32-S3 die temp) plus, since
     * 2026-08-27, the folded-in Safety Processor, Board Health, and
     * Thermocouple Faults content (see ui_page_diagnostics.c's header
     * comment for the full per-page inventory and the six-page paging this
     * now uses). Linked from ui_page_config.c's nav hub like every other
     * diagnostic/settings page. */
    err = kiln_ui_register_page("diagnostics", ui_page_diagnostics_build);
    if (err != ESP_OK) return err;

    /* "thermo_faults" (ui_page_thermo_faults.c) and "tc_types"
     * (ui_page_tc_types.c) were REMOVED 2026-08-27. thermo_faults folded
     * into "diagnostics" above (same owner request as "board_health"/
     * "safety"); tc_types was deleted outright, no destination replaces it
     * (owner request: "remove the kiln setup and thermocouple types pages
     * from the lcd includeing the kiln config page" -- thermocouple TYPE
     * selection is settings, not live status, and the web GUI's zones page
     * already covers it). */

    /* "kiln_setup" (ui_page_kiln_setup.c) and "kiln_cfg_setup"
     * (ui_page_kiln_cfg_setup.c) were REMOVED 2026-08-27, same owner request
     * as "tc_types" above -- deleted outright, no destination replaces
     * either. "Firing Profile", the other half of the old kiln_setup hub,
     * is unaffected: it always pointed straight at the existing "profiles"
     * tree (ui_page_kiln_setup.c's own header comment, git history), which
     * is now reached directly from ui_page_config.c's hub instead of via
     * this now-deleted intermediate hub -- see that file's header comment
     * for request (c), "move the profiles menu item to the top left of the
     * first page". */

    /* "zones"/"zones_detail" (ui_page_zones.c, the LCD equivalent of the web
     * zones page's per-zone thermo_mask/relay_mask/cal_offset_c/temp_limits
     * editor, TODO.md section 3/10.8) were REMOVED outright 2026-08-22 per
     * owner request ("zones and thermocouples page on the lcd can go away").
     * The owner's own framing: settings belong on the web GUI now; the only
     * LCD-side zone information worth keeping is LIVE current temperature
     * and relay status, which ui_page_temperature.c already shows (per-zone
     * temp + per-relay ON/OFF toggle) -- that page stays and needed no
     * change. Nothing replaces "zones"/"zones_detail" here; there is nothing
     * left in their scope that isn't either settings (now web-only) or
     * already covered by ui_page_temperature.c. */

    /* NS2009 touch calibration -- see ui_page_touch_cal.c/.h. Linked from
     * ui_page_config.c's nav hub like every other diagnostic/settings page. */
    err = kiln_ui_register_page("touch_cal", ui_page_touch_cal_build);
    if (err != ESP_OK) return err;

    /* LCD profile browse/start: "Profiles" off ui_page_config.c's nav hub is
     * now the unified, favorites-first, paginated list
     * (ui_page_profile_picker.c's MANAGE mode, UI_PLAN.md Section 6.2) --
     * replacing the old My Profiles / Built-ins (family picker -> per-family
     * list) / Restore hidden four-cell hub and its two sub-pages, deleted the
     * same pass. "profile_picker" is the same widget's PICK mode, for
     * Section 6.1's dashboard picker (wired up in a later wave). Then a
     * per-profile detail screen (title, segment count, feasibility colour,
     * START) and a paginated segment list, unchanged. Closes the gap the
     * user reported: the home page's Start button previously had no way to
     * pick a DIFFERENT profile than whatever the fallback chain resolved to.
     * See ui_page_profiles.c's header comment for the full tree.
     *
     * "profiles_builtin_list" (ui_page_profiles_builtin_list.c, the old
     * per-firing-type builtin browse list) was REMOVED 2026-09-21: it was
     * registered here but nothing anywhere called
     * kiln_ui_show("profiles_builtin_list") after the 6.2 picker rewrite
     * collapsed builtins into "profiles"'s unified list -- confirmed dead by
     * the M18 LCD commissioning run (docs/COMMISSIONING_TEST_MATRIX.md,
     * `profiles_builtin_list` row) and by this file's own now-removed
     * registration comment, which already said so. */
    err = kiln_ui_register_page("profiles", ui_page_profiles_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("profile_picker", ui_page_profile_picker_build_pick);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("profile_detail", ui_page_profile_detail_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("profile_segments", ui_page_profile_segments_build);
    if (err != ESP_OK) return err;

    /* CREATE/EDIT flow off the Profiles hub's new "New Profile" cell (and
     * ui_page_profile_detail.c's "Edit" action): Step 1 name+zones, Step 2
     * one segment per screen (paged), Step 3 review + slot-picker save. See
     * ui_page_profile_builder_zones.h for the full flow description and why
     * editing a builtin is always a copy. */
    err = kiln_ui_register_page("profile_builder_zones", ui_page_profile_builder_zones_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("profile_builder_segment", ui_page_profile_builder_segment_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("profile_builder_review", ui_page_profile_builder_review_build);
    if (err != ESP_OK) return err;

    /* ui_page_touch_cal.c's finish_calibration() navigates here right after
     * a fresh calibration saves -- lets it be checked by eye before trusting
     * every other page's buttons to it. Not reachable from the config nav
     * hub -- not a budget concern any more (LCD work-queue item 5 made that
     * grid a fixed-height, internally scrollable container, and this file's
     * "diagnostics" registration above already added a 9th entry with no
     * issue), just that a manual re-run always starts from "touch_cal" via
     * Config's own nav button, so a second, separate path to "touch_test"
     * would only be reachable mid-calibration anyway. */
    err = kiln_ui_register_page("touch_test", ui_page_touch_test_build);
    if (err != ESP_OK) return err;

    /* A board that has never been calibrated boots straight into
     * calibration rather than home -- an uncalibrated touch mapping means
     * every OTHER page's buttons are unreliable (this session's whole
     * "back buttons don't work" investigation), so showing them first would
     * just repeat that. ui_page_touch_cal_build() navigates to "home" on
     * its own once calibration completes (see its header comment) -- this
     * is only what happens at boot, before that has ever run.
     *
     * Gated on the SHARED calibration-support predicate (touch_dev.h's
     * touch_dev_cal_support(), reached here through lvgl_port), the same one
     * ui_page_config.c's nav cell, ui_page_touch_cal_build() and
     * /api/status's touch_cal_supported field consult -- so this boot gate
     * cannot drift out of agreement with what the rest of the UI offers.
     *
     * This gate used to ask `!lvgl_port_touch_is_self_calibrating()`, which
     * is one INPUT to the decision, not the decision. touch_cal_store_is_
     * calibrated() is permanently false for a self-calibrating controller
     * (FT6336U.h) -- it never calls touch_cal_store_save(), by design,
     * because it never runs that fit at all (touch_dev.h) -- so without a
     * gate such a board would boot into a 3x3 target grid it can never
     * complete, every boot, forever. But `self_calibrating` also reads false
     * for a ZEROED touch_dev_t, i.e. a board whose touch bring-up FAILED
     * (main_boot_early.c logs a WARN and leaves the struct zeroed), so the
     * old condition sent exactly those boards -- the ones with no working
     * touch controller at all -- into that same uncompletable grid, with no
     * way to tap out of it. touch_dev_cal_support() distinguishes the two
     * (see its comment); only a genuinely SUPPORTED controller is forced
     * into calibration, and the other two cases are logged by name rather
     * than silently treated alike. */
    const touch_cal_support_t cal_support = lvgl_port_touch_cal_support();
    if (touch_cal_support_is_offerable(cal_support) && !touch_cal_store_is_calibrated()) {
        ESP_LOGI(TAG, "no touch calibration on file -- starting calibration instead of home");
        return kiln_ui_show("touch_cal");
    }
    if (cal_support == TOUCH_CAL_SUPPORT_NO_TOUCH) {
        /* Never silent: a board that reaches home with no touch controller
         * looks identical to a healthy one until something is tapped. */
        ESP_LOGW(TAG, "no touch controller detected -- touch calibration is not being offered, "
                      "and the LCD will not respond to taps this boot");
    }

    /* docs/WEB_AUTH_PLAN.md section 7/8 (LCD half): one-time setup for the
     * PIN keypad's inactivity lock (tick timer, touch-activity hook), then
     * wire both its seams to the real credential store (persist/
     * web_auth_store.h). Must run after lvgl_port's indev is up (used by
     * ui_lcd_lock_init()) and before any page can be shown, so a gated
     * Start button never fires before the lock exists. */
    ui_lcd_lock_init();
    lcd_credential_bridge_init();

    return kiln_ui_show("home");
}

esp_err_t kiln_ui_register_page(const char *name, kiln_ui_page_build_fn build)
{
    if (!name || !build) return ESP_ERR_INVALID_ARG;
    if (find_page(name)) return ESP_ERR_INVALID_STATE; /* names are unique */
    if (s_page_count >= KILN_UI_MAX_PAGES) return ESP_ERR_NO_MEM;

    s_pages[s_page_count].name = name;
    s_pages[s_page_count].build = build;
    s_pages[s_page_count].screen = NULL;
    s_page_count++;
    return ESP_OK;
}

/* Shared state for one tap-target walk, threaded through the recursion below
 * instead of a growing parameter list. `do_log` keeps the ESP_LOGI dump
 * (kiln_ui_log_tap_targets() / the auto-dump in kiln_ui_show()) working
 * exactly as before; `out` (may be NULL) is the array half added for
 * kiln_ui_collect_tap_targets() -- both can be active at once, though today
 * no caller asks for that. */
typedef struct {
    bool do_log;
    kiln_ui_tap_target_t *out;
    size_t max;
    size_t count;
    bool truncated;
} tap_walk_ctx_t;

static void tap_walk_add(tap_walk_ctx_t *ctx, const char *name, int cx, int cy, bool hidden)
{
    if (!ctx->out) {
        return;
    }
    if (ctx->count >= ctx->max) {
        ctx->truncated = true;
        return;
    }
    kiln_ui_tap_target_t *t = &ctx->out[ctx->count++];
    snprintf(t->name, sizeof(t->name), "%s", name ? name : "");
    t->cx = (int16_t)cx;
    t->cy = (int16_t)cy;
    t->hidden = hidden;
}

/* Recursive half of the tap-target dump called at the end of kiln_ui_show()
 * -- see the comment at that call site for why this exists. Reports each
 * clickable widget's post-layout rectangle plus its centre point, which is
 * the coordinate a test harness should actually inject, and the widget's
 * label text where it has one so targets are identifiable by name rather
 * than by position alone. Depth is carried only to indent nested targets
 * (a scrollable container's children), keeping the dump readable. */
static void log_tap_targets(lv_obj_t *obj, int depth, tap_walk_ctx_t *ctx)
{
    if (!obj || depth > 6) {
        /* Depth cap is a guard against a pathological tree, not a real
         * limit: the deepest page here nests screen > content > container >
         * row > button > label, i.e. well inside 6. */
        return;
    }

    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; i++) {
        lv_obj_t *child = lv_obj_get_child(obj, i);
        if (!child) {
            continue;
        }

        /* Skip hidden subtrees entirely. A hidden widget still reports valid
         * coordinates, so without this the dump advertises targets that
         * cannot be tapped -- ui_page_config.c's paged hub keeps two of its
         * three pages hidden at all times, and listing all three made the
         * dump report three different widgets at the same centre point. The
         * dump's whole purpose is to say where a tap will actually land, so
         * anything not currently hittable has no business in it. */
        if (lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) {
            continue;
        }

        /* An lv_keyboard (and any other lv_buttonmatrix) is a SINGLE lv_obj
         * -- its ~30-odd keys are not child objects, they're entries in an
         * internal button_areas[]/map_p array that lv_buttonmatrix.c draws
         * itself. Walking children the way the rest of this function does
         * would report one big rectangle covering the whole keyboard and no
         * way to aim at an individual key, so a keyboard widget is handled
         * before (and instead of) the generic CLICKABLE check below. */
        if (lv_obj_check_type(child, &lv_buttonmatrix_class)) {
            lv_buttonmatrix_t *bm = (lv_buttonmatrix_t *)child;
            lv_area_t bm_area;
            lv_obj_get_coords(child, &bm_area);

            /* Verified in lv_buttonmatrix.c: button_areas[i] is populated in
             * the widget's OWN coordinate space (relative to its top-left),
             * not screen space -- lv_buttonmatrix.c's own drawing code adds
             * the widget's coords before blitting each key. Adding
             * bm_area.x1/y1 here reproduces that same offset so the numbers
             * this dump prints are screen coordinates the injected-touch
             * harness can use directly. This deliberately does NOT reproduce
             * the library's separate "extra click area" padding math (used
             * internally to make small keys easier to hit) -- the harness
             * only needs the plain key rectangle and its centre, not the
             * enlarged hit-test area. */
            if (bm->button_areas && bm->btn_cnt > 0) {
                for (uint32_t k = 0; k < bm->btn_cnt; k++) {
                    const char *key_text = lv_buttonmatrix_get_button_text(child, k);
                    if (!key_text || key_text[0] == '\0') {
                        /* A NULL/empty caption is a map control entry (row
                         * break, spacer) rather than a real key -- nothing a
                         * test harness would ever want to tap. */
                        continue;
                    }

                    const lv_area_t *ka = &bm->button_areas[k];
                    int x1 = (int)bm_area.x1 + (int)ka->x1;
                    int y1 = (int)bm_area.y1 + (int)ka->y1;
                    int x2 = (int)bm_area.x1 + (int)ka->x2;
                    int y2 = (int)bm_area.y1 + (int)ka->y2;

                    /* This is a burst of ~30+ lines from one widget, well
                     * above this function's usual "a dozen or so lines per
                     * page" volume (see kiln_ui_show()'s call-site comment).
                     * That's accepted here: it only fires on a page switch
                     * or an explicit kiln_ui_log_tap_targets() call, i.e. at
                     * human tap rate, not per poll -- the same volume
                     * argument that comment already makes for the rest of
                     * this dump. */
                    /* Restored to INFO (2026-08-21): CONFIG_LOG_MAXIMUM_LEVEL=3 on
                     * this project compiles ESP_LOGD out of the binary
                     * entirely, so the earlier demotion didn't just quiet
                     * this dump, it deleted the only way to discover a
                     * widget's on-screen position (no framebuffer readback
                     * exists). The real flood contributor is the automatic
                     * call from kiln_ui_show() on every page switch, which is
                     * now gated by s_auto_tap_dump / kiln_ui_set_auto_tap_dump()
                     * below instead of by deleting the log level. The
                     * explicit kiln_ui_log_tap_targets() call (and this
                     * function under it) always logs at INFO. */
                    if (ctx->do_log) {
                        ESP_LOGI(TAG, "  tap target%*s key[%u] (%d,%d)-(%d,%d) centre=(%d,%d) \"%s\"",
                                 depth * 2, "", (unsigned)k, x1, y1, x2, y2,
                                 (x1 + x2) / 2, (y1 + y2) / 2, key_text);
                    }
                    /* A buttonmatrix key is never itself HIDDEN-flagged (its
                     * whole widget was already filtered by the subtree skip
                     * above if hidden) -- always false, matching every key
                     * this walk can ever reach. */
                    tap_walk_add(ctx, key_text, (x1 + x2) / 2, (y1 + y2) / 2, false);
                }
            }

            /* A buttonmatrix has no real children to recurse into (its keys
             * aren't lv_obj_t's), so skip straight to the next sibling
             * rather than falling into the generic path below. */
            continue;
        }

        if (lv_obj_has_flag(child, LV_OBJ_FLAG_CLICKABLE)) {
            lv_area_t area;
            lv_obj_get_coords(child, &area);

            /* A button's caption lives in a child label, so look one level
             * down for it rather than reporting an anonymous rectangle.
             * Hidden labels are skipped: a caption nobody can see is not this
             * object's caption, and taking one was how the home page's body
             * container came to report the hidden safety-trip strip's text. */
            const char *text = "";
            uint32_t grandchildren = lv_obj_get_child_count(child);
            for (uint32_t j = 0; j < grandchildren; j++) {
                lv_obj_t *grandchild = lv_obj_get_child(child, j);
                if (grandchild && lv_obj_check_type(grandchild, &lv_label_class) &&
                    !lv_obj_has_flag(grandchild, LV_OBJ_FLAG_HIDDEN)) {
                    text = lv_label_get_text(grandchild);
                    break;
                }
            }

            /* An explicit tap-name override (ui_topbar.c's build_icon_named())
             * wins over the label text above: an icon-only button's caption
             * is an opaque LVGL symbol glyph, not something a test harness
             * can usefully click by name. Gated on lv_obj_check_type(...,
             * &lv_button_class) first: user_data on a clickable object is NOT
             * reserved for this purpose in general -- ui_confirm.c's msgbox
             * (also clickable, also walked here via lv_layer_top()) stores a
             * heap ui_confirm_ctx_t* in the same field, and reading that as a
             * kiln_ui_tap_name_tag_t while a confirm dialog is open would be
             * an over-read of unrelated heap memory. Only a button can carry
             * this tag, and the magic word is checked before `name` is
             * trusted as a second, independent guard. */
            if (lv_obj_check_type(child, &lv_button_class)) {
                const kiln_ui_tap_name_tag_t *tag =
                    (const kiln_ui_tap_name_tag_t *)lv_obj_get_user_data(child);
                if (tag && tag->magic == KILN_UI_TAP_NAME_MAGIC && tag->name && tag->name[0] != '\0') {
                    text = tag->name;
                }
            }

            /* Restored to INFO alongside the keyboard-key case above -- see
             * that comment; the flood is now handled by gating the automatic
             * call site, not by deleting this log level. */
            /* Two annotations, both added because their absence actively
             * misread the screen during a bench sweep:
             *
             * "(hidden)" -- LVGL skips a LV_OBJ_FLAG_HIDDEN object for hit
             * testing *and* for flex layout, so a hidden strip keeps whatever
             * coordinates it last had and occupies no height. Printed without
             * the marker it looked like a live, full-width tap target. The
             * subtree is still walked: knowing where a hidden widget would
             * appear is the point of this dump.
             *
             * "<- child label" -- the caption is borrowed from the first label
             * grandchild, which is right for a button but wrong for a plain
             * container, where it reports a sibling's text as if it were the
             * container's own. The home page's body container borrowing the
             * hidden safety-trip strip's text read as a live safety trip on a
             * board whose diag_state was ARMED. */
            bool hidden = lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN);
            bool borrowed = (text[0] != '\0') && !lv_obj_check_type(child, &lv_button_class);
            if (ctx->do_log) {
                ESP_LOGI(TAG, "  tap target%*s (%d,%d)-(%d,%d) centre=(%d,%d) \"%s\"%s%s", depth * 2, "",
                         (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2,
                         (int)((area.x1 + area.x2) / 2), (int)((area.y1 + area.y2) / 2), text,
                         borrowed ? " <- child label" : "", hidden ? " (hidden)" : "");
            }
            tap_walk_add(ctx, text, (int)((area.x1 + area.x2) / 2),
                         (int)((area.y1 + area.y2) / 2), hidden);
        }

        log_tap_targets(child, depth + 1, ctx);
    }
}

/* Modal overlays (ui_num_pad.c's numeric keypad, ui_page_network.c's connect
 * modal, ui_confirm.c) are deliberately NOT parented under the active
 * screen -- they're built on lv_layer_top() (or, for lv_msgbox_create(NULL),
 * end up reparented onto it by LVGL) precisely so they draw above whatever
 * page is underneath and survive a screen load/unload. That means
 * log_tap_targets(page->screen, 0) alone never sees them: walking only the
 * screen's tree is a silent gap, not a rendering bug, so a keypad that is
 * fully visible on the glass produces zero dump lines and cannot be aimed
 * at. This helper is the one place both call sites (kiln_ui_show() below and
 * kiln_ui_log_tap_targets()) go through so neither can regress back to
 * screen-only walking. lv_layer_sys() is included too even though nothing in
 * this codebase uses it yet -- it's the same kind of screen-independent
 * layer lv_layer_top() is, so a future toast/system overlay put there gets
 * the same treatment for free. */
static void log_all_tap_targets(lv_obj_t *screen, tap_walk_ctx_t *ctx)
{
    if (screen) {
        log_tap_targets(screen, 0, ctx);
    }

    lv_obj_t *top = lv_layer_top();
    if (top && lv_obj_get_child_count(top) > 0) {
        if (ctx->do_log) {
            ESP_LOGI(TAG, "  -- top-layer --");
        }
        log_tap_targets(top, 0, ctx);
    }

    lv_obj_t *sys = lv_layer_sys();
    if (sys && lv_obj_get_child_count(sys) > 0) {
        if (ctx->do_log) {
            ESP_LOGI(TAG, "  -- sys-layer --");
        }
        log_tap_targets(sys, 0, ctx);
    }
}

esp_err_t kiln_ui_show(const char *name)
{
    s_show_entries++;
    kiln_ui_page_t *page = find_page(name);
    if (!page) {
        ESP_LOGE(TAG, "kiln_ui_show(\"%s\"): no such page", name ? name : "(null)");
        return ESP_ERR_NOT_FOUND;
    }

    /* Touch reports the physical panel is slow enough (ILI9488 blit is a
     * blocking SPI transfer, see lvgl_port.c) that a tap landing right on a
     * page switch reads against whichever screen is still on the glass at
     * that instant -- sometimes the outgoing page's stale pixels under the
     * incoming page's not-yet-drawn ones, sometimes a click firing on the
     * new page's widget that happens to sit under the old page's button the
     * user actually meant to press. Locking input for the duration of the
     * switch and forcing the new screen to finish rendering and flushing
     * before unlocking closes both: nothing is read as a touch until the
     * page the user is looking at is actually the page on the glass. */
    lvgl_port_set_input_enabled(false);

    if (!page->screen) {
        page->screen = page->build();
        if (!page->screen) {
            ESP_LOGE(TAG, "page \"%s\" build() returned NULL", page->name);
            lvgl_port_set_input_enabled(true);
            s_show_exits++;
            return ESP_FAIL;
        }
    }

    lv_screen_load(page->screen);
    const char *from = s_current_page_name ? s_current_page_name : "(none)";
    s_current_page_name = page->name;

    /* One line per navigation, at INFO. Added 2026-08-20 because this panel
     * has no framebuffer readback, so before this line there was NO way --
     * from the PC, over the UART link, or from the device log -- to tell
     * which page was actually on the glass: kiln_ui_show() logged only its
     * failure paths, so a successful navigation was entirely silent. That
     * made every touch-driven UI test unfalsifiable (a tap that hit nothing
     * and a tap that navigated correctly produced identical evidence: none),
     * and in particular made the "a Back button must go back exactly one
     * level" rule checkable only by reading source, never by exercising it.
     *
     * Logging the from->to pair rather than just the destination is what
     * makes that rule directly testable: a Back press from "temperature"
     * must log temperature->config, not temperature->home.
     *
     * Volume is genuinely low -- this fires once per page switch, i.e. at
     * human tap rate, not per poll. That distinction is the one this file's
     * neighbours got wrong twice (NS2009's per-poll log, and the injected
     * touch's per-delivery log), both of which flooded the ring buffer and
     * dropped other lines; see lvgl_port.c's touch_read_cb() for that
     * post-mortem. */
    ESP_LOGI(TAG, "page: %s -> %s", from, page->name);

    /* lv_screen_load() only marks the new screen dirty -- normally the
     * render+flush happens later in the same lv_timer_handler() call this
     * runs inside of. Forcing it now, before input is re-enabled, is what
     * makes the lock above actually cover "until fully loaded" rather than
     * just "until lv_screen_load() returns". */
    lv_refr_now(NULL);

    /* Dump every tap target on the page that just loaded. This runs after
     * lv_refr_now() on purpose: LVGL resolves flex/percentage layout during
     * the render pass, so widget coordinates read before it are all zero.
     *
     * Why this exists: this panel has no framebuffer readback, so the only
     * way to aim an injected touch (lvgl_port.c's TOUCH_CMD_INJECT path) was
     * to hand-compute pixel rectangles from the lv_obj_set_* calls in each
     * ui_page_*.c -- across flex rows, percentage widths, theme padding and
     * an internally-scrollable container whose contents move. That was
     * guesswork, and it was wrong in practice: a tap aimed by hand at the
     * home page's Menu button using exactly that arithmetic missed it, with
     * the miss indistinguishable from a broken injection path because both
     * produce no log output at all.
     *
     * Printing the real post-layout rectangles turns "tap the Menu button"
     * into a lookup instead of a calculation, and makes a miss immediately
     * diagnosable (the target list says where the button actually is). It
     * also self-updates: any future layout change republishes correct
     * coordinates with no test-harness edit.
     *
     * Volume is bounded and tied to human interaction -- one burst per page
     * switch, a dozen or so lines, not per poll. See this function's
     * navigation-log comment above for why that distinction matters here.
     *
     * Gated by s_auto_tap_dump (off by default, 2026-08-21): even at "human
     * tap rate" this still stacks with the per-navigation "page: X -> Y" line
     * and any keyboard's ~30-line key burst, and a normal test session
     * switches pages a lot. The explicit kiln_ui_log_tap_targets() call below
     * is unconditional -- ask for the dump when actually aiming a tap. */
    if (s_auto_tap_dump) {
        tap_walk_ctx_t ctx = { .do_log = true };
        log_all_tap_targets(page->screen, &ctx);
    }

    lvgl_port_set_input_enabled(true);
    s_show_exits++;
    return ESP_OK;
}

void kiln_ui_get_show_diag(uint32_t *show_entries, uint32_t *show_exits)
{
    if (show_entries) *show_entries = s_show_entries;
    if (show_exits) *show_exits = s_show_exits;
}

void kiln_ui_log_tap_targets(void)
{
    lv_obj_t *screen = lv_screen_active();
    tap_walk_ctx_t ctx = { .do_log = true };
    log_all_tap_targets(screen, &ctx);
}

const char *kiln_ui_current_page(void)
{
    return s_current_page_name;
}

size_t kiln_ui_collect_tap_targets(kiln_ui_tap_target_t *out, size_t max, bool *truncated)
{
    tap_walk_ctx_t ctx = { .out = out, .max = max };
    lv_obj_t *screen = lv_screen_active();
    log_all_tap_targets(screen, &ctx);
    if (truncated) {
        *truncated = ctx.truncated;
    }
    return ctx.count;
}

kiln_ui_click_result_t kiln_ui_click_by_name(const char *name, int16_t *out_cx, int16_t *out_cy)
{
    if (!name) {
        return KILN_UI_CLICK_NOT_FOUND;
    }

    /* Sized well past this UI's real per-screen target count (the busiest
     * page here, the numeric keypad, tops out around 30 keys) -- a stack
     * array is fine for a call that never recurses and never runs
     * concurrently with itself (one UART bridge task, one command at a
     * time). */
    kiln_ui_tap_target_t targets[32];
    size_t n = kiln_ui_collect_tap_targets(targets, sizeof(targets) / sizeof(targets[0]), NULL);

    /* Prefer a visible match over a hidden one: a page that show/hides
     * sibling buttons with the same name (a modal's own trigger button,
     * for example) has exactly one *tappable* target even when the tree
     * still contains a hidden one earlier in traversal order. Only fall
     * back to a hidden match when nothing visible matched at all, so the
     * caller still gets HIDDEN rather than a false NOT_FOUND. */
    int match = -1;
    int hidden_match = -1;
    int visible_matches = 0;
    for (size_t i = 0; i < n; i++) {
        if (strcmp(targets[i].name, name) != 0) {
            continue;
        }
        if (!targets[i].hidden) {
            if (match < 0) {
                match = (int)i;
            }
            visible_matches++;
        } else if (hidden_match < 0) {
            hidden_match = (int)i;
        }
    }
    if (match < 0) {
        match = hidden_match;
    }

    if (match < 0) {
        return KILN_UI_CLICK_NOT_FOUND;
    }
    if (out_cx) {
        *out_cx = targets[match].cx;
    }
    if (out_cy) {
        *out_cy = targets[match].cy;
    }
    if (targets[match].hidden) {
        return KILN_UI_CLICK_HIDDEN;
    }
    if (visible_matches > 1) {
        return KILN_UI_CLICK_AMBIGUOUS;
    }

    /* Same lvgl_port_inject_touch() path TOUCH_CMD_INJECT drives -- see that
     * function's header comment (lvgl_port.h) for the coordinate space
     * (already screen pixels) and the press/release latch it implements.
     * Unlike TOUCH_CMD_INJECT, which lets the PC pace its own press/release
     * pair however it likes, this is one call synthesizing both halves of a
     * tap -- the delay between them is required, not cosmetic: touch_read_cb()
     * only samples the latched state on its own ~30ms LVGL poll, so a release
     * written before any poll has sampled the press would overwrite it
     * unseen and no click would ever fire. */
    uint16_t cx = (uint16_t)targets[match].cx;
    uint16_t cy = (uint16_t)targets[match].cy;
    lvgl_port_inject_touch(cx, cy, true);
    vTaskDelay(pdMS_TO_TICKS(50));
    lvgl_port_inject_touch(cx, cy, false);
    return KILN_UI_CLICK_OK;
}
