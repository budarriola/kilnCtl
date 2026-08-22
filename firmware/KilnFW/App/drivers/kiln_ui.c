#include "kiln_ui.h"

#include <stdbool.h>
#include <string.h>

#include "esp_log.h"

#include "ui_page_board_health.h"
#include "ui_page_config.h"
#include "ui_page_diagnostics.h"
#include "ui_page_history.h"
#include "ui_page_home.h"
#include "ui_page_network.h"
#include "ui_page_profile_builder_review.h"
#include "ui_page_profile_builder_segment.h"
#include "ui_page_profile_builder_zones.h"
#include "ui_page_profile_detail.h"
#include "ui_page_profile_segments.h"
#include "ui_page_profiles.h"
#include "ui_page_profiles_builtin_list.h"
#include "ui_page_profiles_family.h"
#include "ui_page_profiles_mine.h"
#include "ui_page_safety.h"
#include "ui_page_tc_types.h"
#include "ui_page_temperature.h"
#include "ui_page_thermo_faults.h"
#include "ui_page_touch_cal.h"
#include "ui_page_touch_test.h"
#include "ui_page_zones.h"
#include "touch_cal_store.h"
#include "lvgl_port.h"

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
 * the 11 that existed before it, which would have overflowed the old 16. */
#define KILN_UI_MAX_PAGES 24

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

    /* TODO.md 10.7's LCD-side board-health nav item, linked from
     * ui_page_config.c's "Board Health" button (kiln_ui_show("board_health")).
     * Registration was the one piece left undone when the pass that built
     * ui_page_board_health.c was cut off mid-task (session limit) -- without
     * this, that nav button would fail soft (kiln_ui_show() logs
     * ESP_ERR_NOT_FOUND and does nothing) rather than crash, but the page
     * would never actually be reachable. */
    err = kiln_ui_register_page("board_health", ui_page_board_health_build);
    if (err != ESP_OK) return err;

    /* TODO.md 10.9's LCD-side Wi-Fi settings page, linked from
     * ui_page_config.c's "Network / Wi-Fi" nav item (previously a
     * "not built yet" placeholder row). */
    err = kiln_ui_register_page("network", ui_page_network_build);
    if (err != ESP_OK) return err;

    /* 2026-08-18 no-scroll rewrite -- ui_page_home.c's Safety Processor card
     * and temperature-history chart moved to their own pages (no room left
     * in home's ~264px content budget once zones/run-state/action row were
     * sized to fit), reachable from ui_page_config.c's nav hub. See
     * ui_page_safety.c/ui_page_history.c's header comments. */
    err = kiln_ui_register_page("safety", ui_page_safety_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("history", ui_page_history_build);
    if (err != ESP_OK) return err;

    /* TODO.md's "Diagnostics / System info page" item, ESP-only half
     * (firmware version, uptime, heap, ESP32-S3 die temp) -- see
     * ui_page_diagnostics.c's header comment for what's deliberately still
     * missing (safety-link stats, blocked on M5). Linked from
     * ui_page_config.c's nav hub like every other diagnostic/settings
     * page; the grid there is a fixed-height, internally scrollable
     * container (LCD work-queue item 5), so a 9th entry costs no
     * no-scroll budget the way the older stale comment on "touch_test"
     * below once worried a 9th grid item would. */
    err = kiln_ui_register_page("diagnostics", ui_page_diagnostics_build);
    if (err != ESP_OK) return err;

    /* MAX31856 fault/status page (per-channel SR fault bits, ~FAULT pin,
     * SPI-transfer health) -- a separate page from "diagnostics" above,
     * which is the ESP-only half and never touches the thermocouple ICs.
     * See ui_page_thermo_faults.c's header comment. Linked from
     * ui_page_config.c's nav hub like every other diagnostic page. */
    err = kiln_ui_register_page("thermo_faults", ui_page_thermo_faults_build);
    if (err != ESP_OK) return err;

    /* 2026-08-21, LCD item 1: per-channel/safety-processor thermocouple TYPE
     * selection (Type B/E/J/K/N/R/S/T) -- see ui_page_tc_types.c's header
     * comment for why this is its own page rather than folded into
     * "thermo_faults" above. Linked from ui_page_config.c's nav hub's new
     * third page (that file's UI_CONFIG_HUB_PAGE_COUNT went 2 -> 3 because
     * both existing pages were already full -- see its own header comment). */
    err = kiln_ui_register_page("tc_types", ui_page_tc_types_build);
    if (err != ESP_OK) return err;

    /* TODO.md 406/section 3, 10.8: LCD equivalent of the web zones page's
     * per-zone thermo_mask/relay_mask/cal_offset_c/temp_limits editor -- see
     * ui_page_zones.c's header comment for the two-page (list; paged detail)
     * layout and what stayed deliberately out of scope. Linked from
     * ui_page_config.c's "Zones & Thermocouples" hub cell, which was a
     * non-clickable "not built yet" placeholder until now. "zones_detail" is
     * a second registered page, only ever reached via
     * ui_page_zones_detail_prepare() + kiln_ui_show("zones_detail"), never
     * linked to directly. */
    err = kiln_ui_register_page("zones", ui_page_zones_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("zones_detail", ui_page_zones_detail_build);
    if (err != ESP_OK) return err;

    /* NS2009 touch calibration -- see ui_page_touch_cal.c/.h. Linked from
     * ui_page_config.c's nav hub like every other diagnostic/settings page. */
    err = kiln_ui_register_page("touch_cal", ui_page_touch_cal_build);
    if (err != ESP_OK) return err;

    /* LCD profile browse/start (this pass): "Profiles" hub off
     * ui_page_config.c's nav hub, then My Profiles / Built-ins (family
     * picker -> per-family list) / Restore hidden, then a per-profile detail
     * screen (title, segment count, feasibility colour, START) and a
     * paginated segment list. Closes the gap the user reported: the home
     * page's Start button previously had no way to pick a DIFFERENT profile
     * than whatever the fallback chain resolved to. See
     * ui_page_profiles.c's header comment for the full tree. */
    err = kiln_ui_register_page("profiles", ui_page_profiles_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("profiles_mine", ui_page_profiles_mine_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("profiles_family", ui_page_profiles_family_build);
    if (err != ESP_OK) return err;
    err = kiln_ui_register_page("profiles_builtin_list", ui_page_profiles_builtin_list_build);
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
     * is only what happens at boot, before that has ever run. */
    if (!touch_cal_store_is_calibrated()) {
        ESP_LOGI(TAG, "no touch calibration on file -- starting calibration instead of home");
        return kiln_ui_show("touch_cal");
    }

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

/* Recursive half of the tap-target dump called at the end of kiln_ui_show()
 * -- see the comment at that call site for why this exists. Reports each
 * clickable widget's post-layout rectangle plus its centre point, which is
 * the coordinate a test harness should actually inject, and the widget's
 * label text where it has one so targets are identifiable by name rather
 * than by position alone. Depth is carried only to indent nested targets
 * (a scrollable container's children), keeping the dump readable. */
static void log_tap_targets(lv_obj_t *obj, int depth)
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
                    ESP_LOGI(TAG, "  tap target%*s key[%u] (%d,%d)-(%d,%d) centre=(%d,%d) \"%s\"",
                             depth * 2, "", (unsigned)k, x1, y1, x2, y2,
                             (x1 + x2) / 2, (y1 + y2) / 2, key_text);
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
             * down for it rather than reporting an anonymous rectangle. */
            const char *text = "";
            uint32_t grandchildren = lv_obj_get_child_count(child);
            for (uint32_t j = 0; j < grandchildren; j++) {
                lv_obj_t *grandchild = lv_obj_get_child(child, j);
                if (grandchild && lv_obj_check_type(grandchild, &lv_label_class)) {
                    text = lv_label_get_text(grandchild);
                    break;
                }
            }

            /* Restored to INFO alongside the keyboard-key case above -- see
             * that comment; the flood is now handled by gating the automatic
             * call site, not by deleting this log level. */
            ESP_LOGI(TAG, "  tap target%*s (%d,%d)-(%d,%d) centre=(%d,%d) \"%s\"", depth * 2, "",
                     (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2,
                     (int)((area.x1 + area.x2) / 2), (int)((area.y1 + area.y2) / 2), text);
        }

        log_tap_targets(child, depth + 1);
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
static void log_all_tap_targets(lv_obj_t *screen)
{
    if (screen) {
        log_tap_targets(screen, 0);
    }

    lv_obj_t *top = lv_layer_top();
    if (top && lv_obj_get_child_count(top) > 0) {
        ESP_LOGI(TAG, "  -- top-layer --");
        log_tap_targets(top, 0);
    }

    lv_obj_t *sys = lv_layer_sys();
    if (sys && lv_obj_get_child_count(sys) > 0) {
        ESP_LOGI(TAG, "  -- sys-layer --");
        log_tap_targets(sys, 0);
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
        log_all_tap_targets(page->screen);
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
    log_all_tap_targets(screen);
}

const char *kiln_ui_current_page(void)
{
    return s_current_page_name;
}
