#include "ui_page_temperature.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"

#include "MAX31856.h"
#include "dashboard_http.h"
#include "kiln_io.h"
#include "ui_theme.h"
#include "ui_topbar.h"
#include "zones_http.h"

// TODO.md 10.3's real "Temperature" page -- individual per-zone manual
// relay control, the touchscreen equivalent of section 2's web dashboard's
// manual relay override (dashboard_http.c's POST /api/relay). Replaces the
// pre-this-pass title+Back stub (see git history).
//
// Per TODO.md 10.1a, every write here goes through dashboard_set_relay()
// (dashboard_http.h/.c) -- extracted THIS pass from relay_post_handler(),
// the same way dashboard_get_status() was extracted from status_get_handler()
// last pass -- so this page cannot drift from the web dashboard's ownership
// (relay_authority_manual_blocked_by_owner()) and safety-fault
// (relay_authority_on_blocked(), ON direction only -- OFF is never gated,
// see relay_authority.h) gating, or from its kiln_io_set_relay() write.
// Nothing here calls kiln_io or relay_authority directly.
//
// Built this pass: per-zone current temperature (same dashboard_get_status()
// snapshot ui_page_home.c reads), one toggle button per relay bit set in
// that zone's relay_mask (zones_config_get_relay_mask()), and an honest
// refusal message -- if dashboard_set_relay() refuses a command (owned by a
// running profile, blocked by a safety fault, no board attached, or the
// kiln_io write itself failing), that reason is shown in a status label, not
// silently swallowed. A relay's button reflects live state (ON/OFF/"no
// board") every UI_PAGE_TEMPERATURE_REFRESH_MS tick, same cadence as
// ui_page_home.c.
//
// Known gaps, documented rather than silently missing:
// - **Zone names.** Same gap ui_page_home.c already flagged: zones_http.h
//   exposes no public getter for a zone's configured name, so each zone
//   card shows "Zone N".
// - **Per-relay, not per-zone, toggles.** The web dashboard's manual
//   override (POST /api/relay) always addressed one relay at a time, and
//   this page follows that shape exactly rather than inventing a
//   "toggle all of this zone's relays together" action the backend has
//   never had -- a zone with more than one relay in its mask gets more than
//   one button.
// - **A relay mask shared by more than one zone** (legal per zones_http.h,
//   though unusual) is rendered as one real toggle button under whichever
//   zone claims it first and a "(shared)" note under any later zone that
//   also claims it, rather than a second independent button for the same
//   physical relay -- two buttons for one relay would let the on-screen
//   state of one silently go stale the instant the other is tapped.

static const char *TAG = "ui_page_temperature";

#define UI_PAGE_TEMPERATURE_REFRESH_MS 1000

/* Relays are numbered from 0 for the operator, matching the thermocouple and
 * zone numbering used everywhere else on this page and on the web UI -- this
 * page previously read "Zone 0 / Relay 1", "Zone 1 / Relay 2", two schemes one
 * line apart. Only the LABEL changes: kiln_io/dashboard_set_relay still take
 * the 1-based board index, so the conversion happens here and nowhere else. */
#define UI_RELAY_DISPLAY(one_based) ((unsigned)((one_based) - 1u))

/* UI_PLAN.md section 3, LCD item 2: relay_row used to be LV_SIZE_CONTENT
 * (build_zone_row()'s comment below), so a zone with several relays wrapping
 * onto a second/third button row grew the card's -- and therefore the whole
 * page's -- drawn height without bound, at odds with every other page's
 * compile-time-fixed content budget (UI_THEME_PAGE_CONTENT_BUDGET_PX).
 * Capped here at a fixed pixel height and left scrollable -- same sanctioned
 * "small internally-scrollable list, not page-level scrolling" pattern
 * ui_page_network.c's saved-network list uses (fixed lv_obj height,
 * LV_OBJ_FLAG_SCROLLABLE left set rather than cleared, so LVGL's own
 * touch-drag scroll handles overflow inside that one bounded box).
 *
 * 2026-08-24 re-derivation (TODO.md's two open LCD-budget items): the
 * original 80px (room for 2 rows of 36px buttons) bounded EACH card's own
 * height, but nobody had summed MAX31856_CHANNEL_COUNT of those capped
 * cards against the real page budget -- doing that arithmetic now (see the
 * _Static_assert below) shows 3 full 80px-relay-row cards alone are already
 * past UI_THEME_PAGE_CONTENT_BUDGET_PX, regardless of relay count per zone.
 * Shrunk to a single button row's worth (36px button + a few px of
 * tolerance, not two rows) so the assert has real margin; a zone whose
 * relay_mask has enough bits set to wrap past one row still fits via this
 * container's own vertical scroll -- it was never validated against fewer
 * than 2 visible rows, so this is a real (if minor) UX narrowing, not free,
 * but the alternative is a page that silently overflows every time
 * MAX31856_CHANNEL_COUNT zones are configured, which no amount of relay-cap
 * bookkeeping fixes on its own. */
#define UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX 40

/* UI_PLAN.md's other open item: "no relays-per-zone cap". KILN_IO_RELAY_COUNT
 * (kiln_io.h) is the WHOLE BOARD's relay count, so a single zone can never
 * legitimately claim more relays than that -- relay_mask is a
 * KILN_IO_RELAY_COUNT-wide bitmask (zones_config_get_relay_mask()) by
 * construction. Named as its own constant, tied back to KILN_IO_RELAY_COUNT
 * by the _Static_assert below, so a future change to either is caught
 * rather than silently drifting apart; build_zone_row() additionally
 * defends against a corrupted/out-of-range mask at runtime (see its own
 * comment) since relay_mask is persisted config data no compile-time check
 * can bound on its own -- a stray bit in a mask this page didn't create
 * itself is exactly the kind of "runtime data, not compile-time-provable"
 * case TODO.md's own discipline calls out for a loud runtime guard instead
 * of a silent trust. */
#define UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE KILN_IO_RELAY_COUNT
_Static_assert(UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE == KILN_IO_RELAY_COUNT,
               "ui_page_temperature.c: UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE must track "
               "KILN_IO_RELAY_COUNT (kiln_io.h) -- a single zone can never claim more relays "
               "than the whole board has; update both together if this ever needs to change.");

/* ---- Worst-case page-height arithmetic (TODO.md's "no relays-per-zone cap"
 * item) ----
 *
 * Mirrors build_zone_row()/ui_page_temperature_build()'s real lv_obj_set_*
 * calls exactly -- if a padding/gap constant below changes, update this
 * arithmetic in the same commit, the same discipline
 * UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX's own header comment already
 * follows.
 *
 * Per-card height (build_zone_row()'s `row`): its own top+bottom pad_all
 * (UI_THEME_PADDING_PX/2, twice) + header (one font line) + the header<->
 * relay_row pad_gap (UI_THEME_PADDING_PX/4) + the fixed relay_row. Relay
 * COUNT never appears in this arithmetic at all -- relay_row's height is
 * fixed regardless, per its own header comment -- so this bound holds no
 * matter what zones_config_get_relay_mask() returns at runtime. */
#define UI_PAGE_TEMPERATURE_CARD_HEIGHT_PX \
    (((UI_THEME_PADDING_PX / 2) * 2) + UI_THEME_FONT_LINE_HEIGHT_PX + (UI_THEME_PADDING_PX / 4) + \
     UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX)

/* Worst case: MAX31856_CHANNEL_COUNT zone cards stacked in `content`, plus
 * s_msg_label (one line), plus one UI_THEME_PADDING_PX/2 inter-child gap
 * after every card including the one before s_msg_label. */
#define UI_PAGE_TEMPERATURE_WORST_CASE_HEIGHT_PX \
    ((MAX31856_CHANNEL_COUNT * UI_PAGE_TEMPERATURE_CARD_HEIGHT_PX) + UI_THEME_FONT_LINE_HEIGHT_PX + \
     (MAX31856_CHANNEL_COUNT * (UI_THEME_PADDING_PX / 2)))

_Static_assert(UI_PAGE_TEMPERATURE_WORST_CASE_HEIGHT_PX <= UI_THEME_PAGE_CONTENT_BUDGET_PX,
               "ui_page_temperature.c: MAX31856_CHANNEL_COUNT zone cards plus the status label "
               "exceed UI_THEME_PAGE_CONTENT_BUDGET_PX (ui_theme.h) -- shrink "
               "UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX or the per-card padding/gap, don't widen "
               "the budget to match. See TODO.md's no-scroll LCD item for this page.");

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
static lv_obj_t *s_relay_btn[KILN_IO_RELAY_COUNT];   /* NULL if that relay has no button on this page */
static lv_obj_t *s_relay_label[KILN_IO_RELAY_COUNT]; /* label inside s_relay_btn[r] */
static bool s_relay_claimed[KILN_IO_RELAY_COUNT];    /* true once some zone's row has built this relay's button */

static lv_obj_t *s_msg_label; /* last refusal (or "") -- see this file's header comment */

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
         * dashboard_http.h. Showing a number that has not been refreshed in
         * over ten seconds as if it were live is how a kiln gets watched
         * against a temperature that stopped moving; "--" is the honest
         * answer, and it matches what the web page and the PC tools show for
         * the same reading. */
        /* ROADMAP.md 2026-08-21 shared unit preference -- see ui_page_home.c's
         * identical block for the full reasoning (same dashboard_get_status()
         * snapshot, same ABSOLUTE-not-rate conversion). */
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
        char label_buf[24];
        if (!ds.io_ready) {
            snprintf(label_buf, sizeof(label_buf), "Relay %u\nno board", (unsigned)(r + 1));
        } else {
            snprintf(label_buf, sizeof(label_buf), "Relay %u\n%s", (unsigned)(r + 1), on ? "ON" : "OFF");
        }
        lv_label_set_text(s_relay_label[r], label_buf);
        lv_obj_set_style_bg_color(s_relay_btn[r], on ? UI_THEME_ACCENT_5 : UI_THEME_COLOR_CARD, 0);
    }
}

static void relay_toggle_cb(lv_event_t *e)
{
    relay_ctx_t *ctx = (relay_ctx_t *)lv_event_get_user_data(e);

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
    case DASHBOARD_RELAY_ERR_SAFETY:
        snprintf(msg, sizeof(msg), "Relay %u refused -- safety fault 0x%02X", UI_RELAY_DISPLAY(ctx->relay_index),
                 (unsigned)sources);
        break;
    case DASHBOARD_RELAY_ERR_UPDATING:
        /* 2026-08-21: distinct from ERR_SAFETY above -- nothing is faulted,
         * a firmware update is in progress (dashboard_http.h's
         * DASHBOARD_RELAY_ERR_UPDATING comment). */
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

/* 2026-08-18 no-scroll pass: card padding/gap trimmed and relay buttons
 * shrunk from UI_THEME_MIN_TOUCH_TARGET_PX (72px) to 36px tall (still using
 * ui_theme_apply_touch_area(..., true), which extends an undersized
 * widget's effective click area back toward that same 72px minimum per its
 * own documented behavior -- see ui_theme.h) so up to MAX31856_CHANNEL_COUNT
 * (3) zone cards, each with a wrapped row of relay buttons, plus a status
 * label have a real chance of fitting this page's content budget (480x320
 * landscape -- see ui_page_home.c's header comment for that number's
 * derivation). The in-content Back button that used to share this budget
 * moved into the shared top bar (ui_topbar.c) in the 2026-08-21 icon-topbar
 * pass, freeing 36px + the 4px row gap back to content. Unlike ui_page_home.c/ui_page_board_health.c,
 * this page's actual height still depends on how many relays are assigned
 * per zone (zones_config_get_relay_mask()), which is runtime config this
 * pass cannot bound at compile time -- a zone with several relays wrapping
 * onto a second button row will still grow past a single-row estimate. This
 * is a real, currently-unresolved risk for that configuration, not
 * something this pass can rule out without real hardware or a fixed cap on
 * relays-per-zone; flagged honestly in TODO.md 10.1/10.3's status note.
 *
 * 2026-08-19 relay-count-bound pass (UI_PLAN.md section 3, LCD item 2): the
 * risk above is now bounded. relay_row is no longer LV_SIZE_CONTENT --
 * UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX's header comment above has the
 * arithmetic (fixed height, internally scrollable beyond that, same pattern
 * ui_page_network.c's saved-network list uses). Each zone card's own
 * worst-case height is therefore fixed at compile time regardless of
 * zones_config_get_relay_mask()'s runtime relay count.
 *
 * 2026-08-24 pass (TODO.md's two remaining LCD-budget items, both closed
 * this pass): the per-card bound above was real but nobody had summed
 * MAX31856_CHANNEL_COUNT of those cards against the actual page budget --
 * doing that arithmetic turned up a real overflow independent of relay
 * count (see UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX's own updated header
 * comment). UI_PAGE_TEMPERATURE_CARD_HEIGHT_PX/_WORST_CASE_HEIGHT_PX above,
 * plus the _Static_assert right after them, now make that arithmetic a
 * compile-time fact instead of a comment -- shrink
 * UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX or reduce
 * MAX31856_CHANNEL_COUNT's real card count and the build breaks loudly
 * instead of silently overflowing. relays-per-zone itself is now formalized
 * as UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE (== KILN_IO_RELAY_COUNT, the
 * true structural ceiling -- a zone can't claim more relays than the board
 * has), with a runtime clamp+log in this function for a mask that
 * (should-never-but-defensively) claims a relay index beyond that, since
 * relay_mask is persisted config data no static assert can see the value
 * of. Still NOT verified against real hardware (no ILI9488 panel attached
 * in this environment) -- this is arithmetic against ui_theme.h's real
 * constants, not a pixel-verified fit, same caveat every other page's
 * budget arithmetic carries. */
static void build_zone_row(lv_obj_t *parent, uint8_t zone_index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_width(row, lv_pct(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(row, UI_THEME_COLOR_CARD, 0);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(row, UI_THEME_CORNER_RADIUS_PX, 0);
    lv_obj_set_style_pad_all(row, UI_THEME_PADDING_PX / 2, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_gap(row, UI_THEME_PADDING_PX / 4, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *header = lv_obj_create(row);
    lv_obj_set_width(header, lv_pct(100));
    lv_obj_set_height(header, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(header, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(header, 0, 0);
    lv_obj_set_style_pad_all(header, 0, 0);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *name = lv_label_create(header);
    lv_obj_set_style_text_color(name, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    char name_buf[16];
    /* Same "no zone-name getter" gap as ui_page_home.c -- see this file's
     * header comment. */
    snprintf(name_buf, sizeof(name_buf), "Zone %u", (unsigned)zone_index);
    lv_label_set_text(name, name_buf);

    lv_obj_t *temp = lv_label_create(header);
    lv_obj_set_style_text_color(temp, UI_THEME_COLOR_TEXT_PRIMARY, 0);
    lv_label_set_text(temp, "-- C");
    s_zone[zone_index].temp_label = temp;

    uint8_t relay_mask = 0;
    bool have_mask = zones_config_get_relay_mask(zone_index, &relay_mask);

    /* Runtime guard, not a static one -- relay_mask is persisted config data
     * (zones_http.h) that no _Static_assert can see the value of. Any bit at
     * or above KILN_IO_RELAY_COUNT is a corrupted/out-of-range mask (the
     * setter is supposed to reject those, but this page reads through a
     * getter, not the setter, and should not trust it blindly); clamp it off
     * and say so loudly rather than either drawing a button for a relay
     * index build_zone_row()'s own `for (r = 0; r < KILN_IO_RELAY_COUNT...)`
     * loop below would silently never reach, or -- the actual UI_PLAN.md
     * concern -- letting a corrupted mask claim more buttons than
     * UI_PAGE_TEMPERATURE_MAX_RELAYS_PER_ZONE assumed when this page's
     * worst-case height was computed above. */
    if (have_mask) {
        uint8_t valid_mask = (uint8_t)((1u << KILN_IO_RELAY_COUNT) - 1u);
        if (relay_mask & (uint8_t)~valid_mask) {
            ESP_LOGE(TAG, "zone %u: relay_mask 0x%02X has bit(s) beyond KILN_IO_RELAY_COUNT (%u) -- "
                          "clamping to the valid range",
                     (unsigned)zone_index, (unsigned)relay_mask, (unsigned)KILN_IO_RELAY_COUNT);
            relay_mask = (uint8_t)(relay_mask & valid_mask);
        }
    }

    lv_obj_t *relay_row = lv_obj_create(row);
    lv_obj_set_width(relay_row, lv_pct(100));
    /* Fixed height + left scrollable, NOT LV_SIZE_CONTENT + SCROLLABLE
     * cleared -- see UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX's header
     * comment (LCD item 2). This is the one internally-scrollable container
     * on this page, same as ui_page_network.c's lists; it is not a violation
     * of the page-level no-scroll rule any more than those are. */
    lv_obj_set_height(relay_row, UI_PAGE_TEMPERATURE_RELAY_ROW_HEIGHT_PX);
    lv_obj_set_scroll_dir(relay_row, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(relay_row, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_bg_opa(relay_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(relay_row, 0, 0);
    lv_obj_set_style_pad_all(relay_row, 0, 0);
    lv_obj_set_flex_flow(relay_row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_gap(relay_row, UI_THEME_PADDING_PX / 2, 0);

    if (!have_mask || relay_mask == 0) {
        lv_obj_t *none = lv_label_create(relay_row);
        lv_obj_set_style_text_color(none, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_label_set_text(none, "no relay assigned");
        return;
    }

    for (uint8_t r = 0; r < KILN_IO_RELAY_COUNT; r++) {
        if (!(relay_mask & (1u << r))) {
            continue;
        }

        if (s_relay_claimed[r]) {
            /* See this file's header comment on the shared-relay edge
             * case -- a note, not a second independent button. */
            lv_obj_t *dup = lv_label_create(relay_row);
            lv_obj_set_style_text_color(dup, UI_THEME_COLOR_TEXT_SECONDARY, 0);
            char buf[24];
            snprintf(buf, sizeof(buf), "Relay %u (shared)", (unsigned)r);
            lv_label_set_text(dup, buf);
            continue;
        }
        s_relay_claimed[r] = true;

        lv_obj_t *btn = lv_button_create(relay_row);
        lv_obj_set_width(btn, UI_THEME_MIN_TOUCH_TARGET_PX * 2);
        lv_obj_set_height(btn, 36); /* see build_zone_row()'s comment on this page's height budget */
        lv_obj_set_style_bg_color(btn, UI_THEME_COLOR_CARD, 0);
        lv_obj_set_style_radius(btn, UI_THEME_CORNER_RADIUS_PX, 0);
        /* Visual affordance for the 36px-drawn/72px-effective touch target
         * (UI_PLAN.md section 3, LCD item 3, same reasoning as
         * ui_page_network.c's shrunk mode/scan/toggle buttons) -- a subtle
         * dimmed border, not a theme-color change, so the drawn size doesn't
         * mislead about where the tap actually registers. */
        lv_obj_set_style_border_width(btn, 1, 0);
        lv_obj_set_style_border_color(btn, UI_THEME_COLOR_TEXT_SECONDARY, 0);
        lv_obj_set_style_border_opa(btn, LV_OPA_40, 0);
        s_relay_ctx[r].relay_index = (uint8_t)(r + 1);
        lv_obj_add_event_cb(btn, relay_toggle_cb, LV_EVENT_CLICKED, &s_relay_ctx[r]);

        lv_obj_t *label = lv_label_create(btn);
        lv_obj_set_style_text_color(label, UI_THEME_COLOR_TEXT_PRIMARY, 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
        char label_buf[24];
        snprintf(label_buf, sizeof(label_buf), "Relay %u\n--", (unsigned)(r + 1));
        lv_label_set_text(label, label_buf);
        lv_obj_center(label);

        /* TODO.md 10.4's touch hit-area helper -- a wrapped row of several
         * relay buttons is the "dense grid" case (compact_layout=true), not
         * ui_page_home.c's sparse single-row button layout. */
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

    memset(s_relay_claimed, 0, sizeof(s_relay_claimed));
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
            build_zone_row(content, zi);
        }
    }

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
