#include "kiln_ui.h"

#include <string.h>

#include "esp_log.h"

#include "ui_page_board_health.h"
#include "ui_page_config.h"
#include "ui_page_diagnostics.h"
#include "ui_page_history.h"
#include "ui_page_home.h"
#include "ui_page_network.h"
#include "ui_page_safety.h"
#include "ui_page_temperature.h"
#include "ui_page_thermo_faults.h"
#include "ui_page_touch_cal.h"
#include "ui_page_touch_test.h"
#include "touch_cal_store.h"
#include "lvgl_port.h"

static const char *TAG = "kiln_ui";

/* A handful of pages (home, settings, temperature, config -- see TODO.md
 * 10.3's page list), not a dynamic set: a fixed array avoids pulling in
 * dynamic allocation for something this small and bounded. Raise this if
 * 10.3 ends up wanting more top-level pages than that. */
#define KILN_UI_MAX_PAGES 16

typedef struct {
    const char *name;          /* borrowed, see kiln_ui_register_page */
    kiln_ui_page_build_fn build;
    lv_obj_t *screen;          /* NULL until first shown */
} kiln_ui_page_t;

static kiln_ui_page_t s_pages[KILN_UI_MAX_PAGES];
static size_t s_page_count;
static const char *s_current_page_name;

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

    /* NS2009 touch calibration -- see ui_page_touch_cal.c/.h. Linked from
     * ui_page_config.c's nav hub like every other diagnostic/settings page. */
    err = kiln_ui_register_page("touch_cal", ui_page_touch_cal_build);
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

            ESP_LOGI(TAG, "  tap target%*s (%d,%d)-(%d,%d) centre=(%d,%d) \"%s\"", depth * 2, "",
                     (int)area.x1, (int)area.y1, (int)area.x2, (int)area.y2,
                     (int)((area.x1 + area.x2) / 2), (int)((area.y1 + area.y2) / 2), text);
        }

        log_tap_targets(child, depth + 1);
    }
}

esp_err_t kiln_ui_show(const char *name)
{
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
     * navigation-log comment above for why that distinction matters here. */
    log_tap_targets(page->screen, 0);

    lvgl_port_set_input_enabled(true);
    return ESP_OK;
}

const char *kiln_ui_current_page(void)
{
    return s_current_page_name;
}
