#include "ui_page_temperature.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "dashboard_http.h"
#include "kiln_io.h"
#include "safety_trip_words.h" /* safety_fault_source_words() -- decode the safety fault-source
                                 * mask instead of showing the operator a bare hex value
                                 * (ROADMAP.md M13). */
#include "ui_theme.h"
#include "ui_topbar.h"
#include "zones_http.h"

// TODO.md 10.3's real "Temperature" page -- individual per-zone live
// temperature plus manual relay control for whichever relays are NOT owned
// by a zone. Every write here goes through dashboard_set_relay()
// (dashboard_http.h/.c) -- extracted THIS pass from relay_post_handler(),
// the same way dashboard_get_status() was extracted from status_get_handler()
// -- so this page cannot drift from the web dashboard's ownership
// (relay_authority_manual_blocked_by_owner()) and safety-fault
// (relay_authority_on_blocked(), ON direction only -- OFF is never gated,
// see relay_authority.h) gating, or from its kiln_io_set_relay() write.
// Nothing here calls kiln_io or relay_authority directly.
//
// 2026-08-27 owner request, verbatim: "in the lcd. the temp page should not
// allow the manual toggeling of zone relays but should allow the toggel of
// non zone ralays". This landed about an hour after the kiln's heating
// elements were physically wired to this board for the first time, so it is
// not cosmetic -- a relay assigned to a zone must never be manually
// commandable from here, only from the zone's own control mode
// (PID/bang-bang/OFF), because the operator can flip that mode back on at
// any moment and two owners (a manual tap here, and the zone's own control
// loop) would then fight over the same physical contact.
//
// That exact rule already exists, in exactly this shape, for the rule
// engine -- rules_http.c's check_relay_not_zone_owned() and
// rules_task.c's compute_heater_relay_mask()/sync_ownership(): "a relay is
// zone-assigned if its bit appears in ANY zone's relay_mask" (a relay_mask
// shared by more than one zone is legal, per zones_http.h). This page reuses
// that same test -- relay_is_zone_owned() below -- rather than inventing a
// second, possibly-diverging definition of "zone-owned".
//
// A zone-owned relay's button stays VISIBLE, never hidden: an operator who
// cannot see relay state loses information they have today, and the ask was
// to remove the manual CONTROL, not the display. Its button still shows live
// ON/OFF every refresh_cb() tick, but is LV_STATE_DISABLED (LVGL does not
// deliver LV_EVENT_CLICKED to a disabled widget at all -- confirmed against
// components/lvgl/src/indev/lv_indev.c's press handling before relying on
// it) and its label names the reason ("(zone)") rather than silently
// swallowing a tap. relay_toggle_cb() ALSO re-checks relay_is_zone_owned()
// before writing, independent of the widget's disabled state, so a stale
// UI state can never let a write through -- see that function's comment.
//
// Zone assignment is runtime config (zones_http.h's relay_mask, editable
// from /settings/zones at any time), so relay_is_zone_owned() is computed
// FRESH on every refresh_cb() tick and again inside relay_toggle_cb() at
// click time -- never cached from page-build time. A relay that becomes
// zone-assigned after this page was built must go from toggleable to
// view-only on its very next refresh, not stay toggleable until reboot.
//
// STRUCTURE, changed this pass: relay buttons used to be nested one per
// zone-card (one button per bit in THAT zone's relay_mask, with a "(shared)"
// label for a relay two zones both claimed). Now that every zone-owned relay
// is display-only here regardless of which zone(s) claim it, there is no
// reason to keep that per-zone nesting or its shared-relay special case --
// one flat "Relays" section lists each of the KILN_IO_RELAY_COUNT physical
// relays exactly once. Zone cards above it now show only what they still
// need: name + live temperature. This also shrinks the page's worst-case
// height enough to fit the no-scroll budget with the new always-visible
// non-zone-relay control added -- see the arithmetic below
// UI_PAGE_TEMPERATURE_WORST_CASE_HEIGHT_PX.
//
// Known gaps, documented rather than silently missing:
// - **Zone names.** zones_http.h exposes no public getter for a zone's
//   configured name, so each zone row shows "Zone N".
//
// 2026-08-27+1 owner request ("the user should be able to assign names to
// relays not assigned to zones as well"): non-zone-owned relay buttons now
// show the operator-entered zones_config_get_relay_name() label in place of
// "Relay N" whenever one has been saved -- see refresh_cb()'s relay-label
// block below. Zone-owned relays never show a custom name (relay_names_cfg_t
// is scoped to relays not claimed by any zone), so their "(zone)" label is
// unchanged.

static const char *TAG = "ui_page_temperature";

#define UI_PAGE_TEMPERATURE_REFRESH_MS 1000

/* Relays are numbered from 0 for the operator, matching the thermocouple and
 * zone numbering used everywhere else on this page and on the web UI --
 * kiln_io/dashboard_set_relay still take the 1-based board index, so the
 * conversion happens here and nowhere else. */
#define UI_RELAY_DISPLAY(one_based) ((unsigned)((one_based) - 1u))

/* ---- Worst-case page-height arithmetic ----------------------------------
 *
 * Mirrors build_zone_temp_row()/build_relays_section()/
 * ui_page_temperature_build()'s real lv_obj_set_* calls exactly -- if a
 * padding/gap constant below changes, update this arithmetic in the same
 * commit (same discipline every other page's worst-case block follows).
 *
 * Zone row: a single line ("Zone N: --.- C") in a card whose height is its
 * own top+bottom pad_all (UI_THEME_PADDING_PX/2, twice) plus one font line --
 * no relay content any more, so this does not vary with relay_mask at all. */
#define UI_PAGE_TEMPERATURE_ZONE_ROW_HEIGHT_PX \
    (((UI_THEME_PADDING_PX / 2) * 2) + UI_THEME_FONT_LINE_HEIGHT_PX)

/* Relay button row -- fixed height + internally scrollable beyond that, same
 * sanctioned "small internally-scrollable list, not page-level scrolling"
 * pattern ui_page_network.c's saved-network list uses (fixed lv_obj height,
 * LV_OBJ_FLAG_SCROLLABLE left set). KILN_IO_RELAY_COUNT buttons at
 * UI_THEME_MIN_TOUCH_TARGET_PX*2 wide comfortably wrap to 2 rows within one
 * button row's worth of height on a 480px-wide panel; a future relay count
 * that needs more rows scrolls inside this box rather than growing the page. */
#define UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX 40

/* Board-wide relay ceiling a single zone (or the flat Relays section) can
 * ever claim -- named so a future change to either constant is caught by the
 * _Static_assert below rather than silently drifting apart. Kept even though
 * this page no longer nests relay buttons per zone: check_ui_budget_asserts.ps1
 * pins this exact assertion text, and the invariant it states ("a relay
 * index this page reasons about never exceeds the board's real relay count")
 * is still real and still worth a compile-time check. */
#define UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE KILN_IO_RELAY_COUNT
_Static_assert(UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE == KILN_IO_RELAY_COUNT,
               "ui_page_temperature.c: UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE must track "
               "KILN_IO_RELAY_COUNT (kiln_io.h) -- a single zone can never claim more relays "
               "than the whole board has; update both together if this ever needs to change.");

/* One "Relays" card: its own top+bottom pad_all, one header line ("Relays --
 * zone relays are view-only"), the header<->relay_row gap, and the fixed
 * relay_row. */
#define UI_PAGE_TEMPERATURE_RELAYS_CARD_HEIGHT_PX \
    (((UI_THEME_PADDING_PX / 2) * 2) + UI_THEME_FONT_LINE_HEIGHT_PX + (UI_THEME_PADDING_PX / 4) + \
     UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX)

/* Worst case: MAX31856_CHANNEL_COUNT zone rows, the one Relays card, and
 * s_msg_label (one line) stacked in `content`, with a UI_THEME_PADDING_PX/2
 * inter-child gap after every one of those (MAX31856_CHANNEL_COUNT + 2)
 * children. */
#define UI_PAGE_TEMPERATURE_WORST_CASE_HEIGHT_PX \
    ((MAX31856_CHANNEL_COUNT * UI_PAGE_TEMPERATURE_ZONE_ROW_HEIGHT_PX) + \
     UI_PAGE_TEMPERATURE_RELAYS_CARD_HEIGHT_PX + UI_THEME_FONT_LINE_HEIGHT_PX + \
     ((MAX31856_CHANNEL_COUNT + 2) * (UI_THEME_PADDING_PX / 2)))

_Static_assert(UI_PAGE_TEMPERATURE_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_temperature.c: MAX31856_CHANNEL_COUNT zone rows plus the Relays card and "
               "the status label exceed UI_THEME_PAGE_CONTENT_BUDGET_PX (ui_theme.h) -- shrink "
               "UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX or the per-row padding/gap, don't widen "
               "the budget to match.");

typedef struct {
    lv_obj_t *temp_label;
} zone_widgets_t;

/* Ties a relay toggle button's LV_EVENT_CLICKED callback back to which
 * relay it controls -- one static ctx per relay (board-wide, 1-based index
 * matching kiln_io_set_relay's convention), passed as the event's
 * user_data. Never freed/reused across pages (this page's screen, like
 * every other one, is built once and kept alive forever -- kiln_ui.h's
 * header comment), so a plain static array outlives every button that
 * points into it. */
typedef struct {
    uint8_t relay_index; /* 1-based */
} relay_ctx_t;

static zone_widgets_t s_zone[MAX31856_CHANNEL_COUNT];
static uint8_t s_zone_count;

static relay_ctx_t s_relay_ctx[KILN_IO_RELAY_COUNT];
static lv_obj_t *s_relay_btn[KILN_IO_RELAY_COUNT];
static lv_obj_t *s_relay_label[KILN_IO_RELAY_COUNT];

static lv_obj_t *s_msg_label; /* last refusal (or "") -- see this file's header comment */

/* THE reuse point for "in the lcd. the temp page should not allow the manual
 * toggeling of zone relays but should allow the toggel of non zone ralays".
 * Same test as rules_http.c's check_relay_not_zone_owned() and
 * rules_task.c's compute_heater_relay_mask(): a relay is zone-owned if its
 * bit is set in ANY configured zone's relay_mask (zones_config_get_relay_mask()),
 * not just the "first" zone that claims it -- a relay_mask shared by more
 * than one zone is legal (zones_http.h) and must still read as zone-owned.
 *
 * Computed fresh on every call, never cached: zone_index is runtime config,
 * editable from /settings/zones at any time this page is on screen, so a
 * relay that changes zone assignment mid-session must be reflected on this
 * page's very next refresh, not after a reboot. */
static bool relay_is_zone_owned(uint8_t relay_index_1based)
{
    uint8_t bit = (uint8_t)(1u << (relay_index_1based - 1u));
    uint8_t zone_count = zones_config_get_thermo_count();
    for (uint8_t zi = 0; zi < zone_count; zi++) {
        uint8_t zone_mask = 0;
        if (!zones_config_get_relay_mask(zi, &zone_mask)) {
            continue;
        }
        if (zone_mask & bit) {
            return true;
        }
    }
    return false;
}

static void refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Same plain-C getter dashboard_http.c's GET /api/status handler and
     * ui_page_home.c's refresh both call -- TODO.md 10.1a. */
    dashboard_status_t ds;
    dashboard_get_status(&ds);

    for (uint8_t zi = 0; zi < s_zone_count; zi++) {
        char buf[24];
        const dashboard_channel_status_t *ch = NULL;
        for (size_t i = 0; i < ds.channel_count; i++) {
            /* Legacy zone_index == MAX31856 channel mapping, same as
             * ui_page_home.c / dashboard_http.c. */
            if (ds.channels[i].channel == zi) {
                ch = &ds.channels[i];
                break;
            }
        }
        /* ch->stale is the shared user-facing rule (older than
         * KILN_TEMP_STALE_AGE_MS), not the driver's per-poll flag -- see
         * dashboard_http.h. "--" is the honest answer for a stale reading. */
        if (ch && ch->valid && !ch->stale) {
            snprintf(buf, sizeof(buf), "%.1f %s",
                     (double)unit_pref_convert(ch->temp_c, ds.temp_unit, UNIT_PREF_KIND_ABSOLUTE),
                     unit_pref_suffix(ds.temp_unit));
        } else {
            snprintf(buf, sizeof(buf), "-- %s", unit_pref_suffix(ds.temp_unit));
        }
        lv_label_set_text(s_zone[zi].temp_label, buf);
    }

    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        if (!s_relay_btn[r]) {
            continue;
        }
        bool on = ds.io_ready && ds.relay_on[r];
        /* Recomputed every tick -- see relay_is_zone_owned()'s own comment
         * on why this can never be a build-time-only check. */
        bool zone_owned = relay_is_zone_owned((uint8_t)(r + 1));

        char label_buf[48];
        if (!ds.io_ready) {
            snprintf(label_buf, sizeof(label_buf), "Relay %u\nno board", (unsigned)r);
        } else if (zone_owned) {
            /* Legible reason, not a control that silently does nothing --
             * this file's header comment / the owner's own requirement:
             * "an operator who cannot see the relay state loses information
             * they have today". Zone-owned relays are never operator-named
             * (relay_names_cfg_t is scoped to relays NOT claimed by a zone --
             * see zones_http.h's own header comment on that pair), so this
             * branch always uses the generic "Relay N" label. */
            snprintf(label_buf, sizeof(label_buf), "Relay %u\n%s (zone)", (unsigned)r, on ? "ON" : "OFF");
        } else {
            /* Owner request (zones_http.h's relay_names_cfg_t header
             * comment): "the user should be able to assign names to relays
             * not assigned to zones as well". zones_config_get_relay_name()
             * returns true with an empty string for a relay that was never
             * named -- fall back to the generic "Relay N" label in that
             * case, same convention every other name getter in this
             * codebase uses (zones_config_get_name() etc). */
            char rname[RELAY_NAME_MAX_LEN + 1] = "";
            (void)zones_config_get_relay_name((uint8_t)(r + 1), rname, sizeof(rname));
            if (rname[0] != '\0') {
                snprintf(label_buf, sizeof(label_buf), "%s\n%s", rname, on ? "ON" : "OFF");
            } else {
                snprintf(label_buf, sizeof(label_buf), "Relay %u\n%s", (unsigned)r, on ? "ON" : "OFF");
            }
        }
        lv_label_set_text(s_relay_label[r], label_buf);

        if (zone_owned) {
            /* LV_STATE_DISABLED -- LVGL does not deliver LV_EVENT_CLICKED to
             * a disabled widget at all (components/lvgl/src/indev's press
             * handling checks this before generating the event), so this is
             * a real gate, not just a visual dimming. relay_toggle_cb()
             * below still re-checks independently as a second gate. */
            lv_obj_add_state(s_relay_btn[r], LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(s_relay_btn[r], UI_THEME_COLOR_CARD, 0);
            lv_obj_set_style_text_color(s_relay_label[r], UI_THEME_COLOR_TEXT_SECONDARY, 0);
        } else {
            lv_obj_remove_state(s_relay_btn[r], LV_STATE_DISABLED);
            lv_obj_set_style_bg_color(s_relay_btn[r], on ? UI_THEME_ACCENT_5 : UI_THEME_COLOR_CARD, 0);
            lv_obj_set_style_text_color(s_relay_label[r], UI_THEME_COLOR_TEXT_PRIMARY, 0);
        }
    }
}

static void relay_toggle_cb(lv_event_t *e)
{
    relay_ctx_t *ctx = (relay_ctx_t *)lv_event_get_user_data(e);

    /* Second, independent gate against a zone-owned relay -- the button is
     * LV_STATE_DISABLED (refresh_cb() above) whenever this is true, which
     * already stops LVGL from delivering the click at all, but this check
     * does not trust that alone: it is the same "never trust cached widget
     * state, read through the same getter" discipline as the current-state
     * read two lines below, applied to the gate itself. */
    if (relay_is_zone_owned(ctx->relay_index)) {
        char msg[80];
        snprintf(msg, sizeof(msg), "Relay %u is assigned to a zone -- use the zone's control mode",
                 UI_RELAY_DISPLAY(ctx->relay_index));
        ESP_LOGW(TAG, "%s", msg);
        lv_label_set_text(s_msg_label, msg);
        lv_obj_set_style_text_color(s_msg_label, UI_THEME_ACCENT_5, 0);
        return;
    }

    /* Read current state fresh rather than trusting the button's last-drawn
     * color -- the same "read through the same getter, don't trust cached
     * widget state" discipline as everything else on this page. */
    dashboard_status_t ds;
    dashboard_get_status(&ds);
    bool currently_on = ds.io_ready && ds.relay_on[ctx->relay_index - 1];
    bool want_on = !currently_on;

    /* The one write path -- see this file's header comment. */
    uint32_t sources = 0;
    dashboard_relay_result_t result = dashboard_set_relay(ctx->relay_index, want_on, &sources);

    char msg[64];
    switch (result) {
    case DASHBOARD_RELAY_OK:
        msg[0] = '\0';
        break;
    case DASHBOARD_RELAY_ERR_NO_BOARD:
        snprintf(msg, sizeof(msg), "Relay %u: no relay board attached", UI_RELAY_DISPLAY(ctx->relay_index));
        break;
    case DASHBOARD_RELAY_ERR_RANGE:
        /* Should not happen -- ctx->relay_index is always a valid
         * board-wide relay index built from KILN_IO_RELAY_COUNT below --
         * but handled rather than assumed unreachable. */
        snprintf(msg, sizeof(msg), "Relay %u: out of range", UI_RELAY_DISPLAY(ctx->relay_index));
        break;
    case DASHBOARD_RELAY_ERR_OWNED:
        snprintf(msg, sizeof(msg), "Relay %u refused -- owned by a running profile", UI_RELAY_DISPLAY(ctx->relay_index));
        break;
    case DASHBOARD_RELAY_ERR_SAFETY: {
        /* ROADMAP.md M13: decode the fault-source mask instead of showing a
         * bare hex value. msg is only char[64] (shared by every case in this
         * switch), and the full comma-joined safety_fault_source_words()
         * sentence can run to 141 bytes (see safety_trip_words.h's comment),
         * so the full decode does not fit here -- take just the first
         * asserted source's name and note "(+more)" if others are also set,
         * same reasoning ui_page_diagnostics.c uses for its unwrappable LCD
         * line. The "%.16s" precision (not just a big buffer) is what lets
         * -Werror=format-truncation prove this can never overflow msg,
         * regardless of how long the decoded word actually is:
         * "Relay " + up to 10 digits + " refused -- safety: " + 16 + " (+more)"
         * = 6 + 10 + 20 + 16 + 8 = 60 bytes, plus the NUL, fits in 64. */
        char src_words[160];
        safety_fault_source_words(sources, src_words, sizeof(src_words));
        char *comma = strchr(src_words, ',');
        bool more = (comma != NULL);
        if (comma != NULL) {
            *comma = '\0';
        }
        snprintf(msg, sizeof(msg), "Relay %u refused -- safety: %.16s%s", UI_RELAY_DISPLAY(ctx->relay_index),
                 src_words, more ? " (+more)" : "");
        break;
    }
    case DASHBOARD_RELAY_ERR_UPDATING:
        snprintf(msg, sizeof(msg), "Relay %u refused -- firmware update in progress", UI_RELAY_DISPLAY(ctx->relay_index));
        break;
    case DASHBOARD_RELAY_ERR_IO_FAIL:
    default:
        snprintf(msg, sizeof(msg), "Relay %u: command failed", UI_RELAY_DISPLAY(ctx->relay_index));
        break;
    }

    if (result != DASHBOARD_RELAY_OK) {
        ESP_LOGW(TAG, "%s", msg);
    }
    lv_label_set_text(s_msg_label, msg);
    lv_obj_set_style_text_color(s_msg_label, result == DASHBOARD_RELAY_OK ? UI_THEME_COLOR_TEXT_SECONDARY
                                                                           : UI_THEME_ACCENT_5,
                                 0);

    refresh_cb(NULL); /* repaint immediately instead of waiting one tick */
}

/* Zone row: name + live temperature only -- no relay content any more (see
 * this file's header comment for why relay buttons moved to one flat
 * "Relays" section below). */
static void build_zone_temp_row(lv_obj_t *parent, uint8_t zone_index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_style_text_color(name, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    char name_buf[16];
    /* Same "no zone-name getter" gap as ui_page_home.c -- see this file's
     * header comment. */
    snprintf(name_buf, sizeof(name_buf), "Zone %u", (unsigned)zone_index);
    lv_label_set_text(name, name_buf);

    lv_obj_t *temp = lv_label_create(row);
    lv_obj_set_style_text_color(temp, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(temp, "-- C");
    s_zone[zone_index].temp_label = temp;
}

/* One flat "Relays" section, every physical relay (1..KILN_IO_RELAY_COUNT)
 * exactly once. Enabled/disabled state and label text are set entirely in
 * refresh_cb() (recomputed every tick, see relay_is_zone_owned()'s comment);
 * this only builds the widgets. */
static void build_relays_section(lv_obj_t *parent)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(card, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(card, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(card, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(card, UI_THEME_PADDING_PX / 4, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_label_create(card);
    lv_obj_set_style_text_color(header, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(header, "Relays -- zone relays are view-only");

    lv_obj_t *relay_row = lv_obj_create(card);
    lv_obj_set_width(relay_row, lv_pct(100));
    /* Fixed height + left scrollable, NOT LV_SIZE_CONTENT + SCROLLABLE
     * cleared -- see UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX's header
     * comment. Same sanctioned internally-scrollable-list pattern as
     * ui_page_network.c's lists. */
    lv_obj_set_height(relay_row, UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX);
    lv_obj_set_scroll_dir(relay_row, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(relay_row, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(relay_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(relay_row, 0, 0);
    lv_obj_set_style_pad_all(relay_row, 0, 0);
    lv_obj_set_flex_flow(relay_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(relay_row, UI_THEME_PADDING_PX / 2, 0);

    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        lv_obj_t *btn = lv_button_create(relay_row);
        lv_obj_set_width(btn, UI_THEME_MIN_TOUCH_TARGET_PX * 2);
        lv_obj_set_height(btn, 36);
        lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
        /* Visual affordance for the 36px-drawn/72px-effective touch target,
         * same reasoning as ui_page_network.c's shrunk buttons -- a subtle
         * dimmed border, not a theme-color change. */
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        s_relay_ctx[r].relay_index = (uint8_t)(r + 1);
        lv_obj_add_event_cb(btn, relay_toggle_cb, LV_EVENT_CLICKED, &s_relay_ctx[r]);

        lv_obj_t *label = lv_label_create(btn);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        char label_buf[24];
        snprintf(label_buf, sizeof(label_buf), "Relay %u\n--", (unsigned)r);
        lv_label_set_text(label, label_buf);
        lv_obj_center(label);

        /* TODO.md 10.4's touch hit-area helper -- a wrapped row of several
         * relay buttons is the "dense grid" case (compact_layout=true). */
        lv_obj_update_layout(btn);
        ui_theme_apply_touch_area(btn, true);

        s_relay_btn[r] = btn;
        s_relay_label[r] = label;
    }
}

lv_obj_t *ui_page_temperature_build(void)
{
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, UI_THEME_COLOR_BG, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(scr, UI_THEME_PADDING_PX, 0);
    lv_obj_set_style_pad_gap(scr, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    static ui_topbar_t tb;
    ui_topbar_create(scr, &(ui_topbar_cfg_t){
        .title = "Temperature -- manual relay control",
        .back_page = "config",
        .show_home = true,
    }, &tb);

    lv_obj_t *content = lv_obj_create(scr);
    lv_obj_set_width(content, lv_pct(100));
    lv_obj_set_flex_grow(content, 1);
    lv_obj_set_style_bg_opa(content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(content, 0, 0);
    lv_obj_set_style_pad_all(content, 0, 0);
    lv_obj_set_flex_flow(content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(content, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    memset(s_relay_btn, 0, sizeof(s_relay_btn));
    memset(s_relay_label, 0, sizeof(s_relay_label));

    s_zone_count = zones_config_get_thermo_count();
    if (s_zone_count > MAX31856_CHANNEL_COUNT) {
        s_zone_count = MAX31856_CHANNEL_COUNT; /* defensive; should never trip */
    }
    if (s_zone_count == 0) {
        lv_obj_t *none = lv_label_create(content);
        lv_obj_set_style_text_color(none, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_label_set_text(none, "No zones configured");
    } else {
        for (uint8_t zi = 0; zi < s_zone_count; zi++) {
            build_zone_temp_row(content, zi);
        }
    }

    build_relays_section(content);

    /* Refusal/status message -- see this file's header comment: a refused
     * write is shown here, never silently dropped. */
    s_msg_label = lv_label_create(content);
    lv_obj_set_width(s_msg_label, lv_pct(100));
    lv_obj_set_style_text_color(s_msg_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_align(s_msg_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_msg_label, "");

    ui_topbar_raise(&tb);

    /* Pages are never torn down (kiln_ui.h's header comment) -- same
     * "create once, keep refreshing forever" timer lifetime as
     * ui_page_home.c. */
    lv_timer_create(refresh_cb, UI_PAGE_TEMPERATURE_REFRESH_MS, NULL);
    refresh_cb(NULL); /* paint real numbers immediately instead of waiting one tick */

    return scr;
}
