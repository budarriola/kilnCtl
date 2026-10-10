/* The home page's 1 Hz periodic refresh (ui_page_home.c) -- split out
 * 2026-09-04, ROADMAP.md M15's 1500-line item. ui_home_refresh_cb() repaints
 * every live number on the page (status text, safety-trip strip, lag
 * notice, progress bar, chart data/axes/legend, fire/pause buttons) from
 * the same plain-C getters dashboard_http.c's HTTP handlers call -- see
 * ui_page_home.c's own header comment for the full "why" history behind
 * each section below. See ui_page_home_internal.h for the shared
 * statics/prototypes this file reaches across the split. */
#include "ui_page_home_internal.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "relay_cycles.h"
#include "kiln_cfg_store.h" /* docs/KILN_PROFILES_PLAN.md section 7.4 -- "Kiln: <name>" line */
#include "safety_ceiling_sync.h" /* 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md,
                                   * HIGH 2): safety_ceiling_sync_is_standing_diverged() surfaced on the LCD. */
#include "config_divergence.h" /* CONFIG_DIVERGENCE_REASON_MAX */
#include "zones_config_accessors.h" /* zones_config_get_load_fault() -- CLAUDE.md's
                                       * ota_rollback_esp() hazard, closed 2026-09-16 */
#include "kiln_cfg_swap.h" /* kiln_cfg_swap_get_boot_fault_kind() -- M13 fix */
#include "live_profile.h" /* live_profile_load_record()/live_profile_generation() -- "Keep?" button */
#include "hal_time.h" /* hal_time_now_us() -- auth_reset_gesture's now_ms argument */
#include "ct_leak_alarm.h" /* H9 CT alarm -- trip-strip branch */
#include "ui_page_safety_logic.h" /* shared live-trip derive for the trip strip */

/* 2026-09-15 review follow-up (review_divergence_wiring_60d6552f_2026-09-15.md,
 * items A/B/C and HIGH 1): the deferred Pico-half recapture poll and its
 * bx_flash_worker dispatch used to live in this file, driven by the 1 Hz
 * LVGL refresh below. That made a config-divergence detector's existence
 * depend on which LCD page the operator happened to open (this timer is
 * created inside ui_page_home.c's lazy build(), so a touch_cal boot never
 * creates it at all), nested it inside an `if (s_ui_home_lag_notice !=
 * NULL)` widget check, and blocked the LVGL task unbounded behind any other
 * caller's flash job. The poll now lives in safety_poll_task()'s steady
 * loop (safety_link_poll.c), which is display-independent and already owns
 * the divergence evaluation. This file keeps only the on-screen "Config
 * mismatch (Pico)" notice, which IS legitimately display-dependent. */

/* docs/WEB_AUTH_PLAN.md item 10 -- tracks the gesture's armed-and-live state
 * across ticks so a transition from true to false (whether by successful
 * confirm, which logs itself in ui_home_auth_reset_confirm_yes_cb(), or by
 * the 30 s window elapsing with no confirm at all) can be told apart and
 * logged as an expiry/cancel exactly once. ui_confirm.h's Cancel button has
 * no callback hook (see its own doc comment) so a dialog-Cancel tap cannot
 * log directly -- this 1 Hz observation of natural expiry is the mechanism
 * actually used for the "cancel" ESP_LOGE, not a true button-press hook. */
static bool s_ui_home_auth_reset_was_armed_live = false;

void ui_home_refresh_cb(lv_timer_t *timer)
{
    (void)timer;

    /* Relay-life budget warning (RELAY_LIFE_BUDGET.md). Cheap --
     * relay_cycles_max_budget_tier() just reads RAM state already loaded at
     * boot -- so this rides the existing 1 Hz tick rather than getting its
     * own timer. Map straight onto ui_topbar_warning_tier_t; both enums are
     * NONE=0/WARN=1/ERROR=2 by construction but this stays an explicit
     * switch rather than a cast so the two headers can drift independently
     * without silently miscoloring the icon. DELIBERATELY not debounced or
     * auto-cleared like the lag notice above: the plan calls for a
     * *persistent* icon once a relay crosses 80%, on purpose, even though
     * main_page.html's own comment elsewhere warns "a warning always present
     * is a warning nobody reads" -- this is the one exception, because the
     * thing it reports (contact life spent) never goes back down on its own
     * the way a lag condition does, so hiding it after N ticks would make an
     * operator believe the wear stopped. See ui_topbar_set_warning()'s
     * header comment for the same note from the other side. */
    ui_topbar_warning_tier_t warn_tier;
    switch (relay_cycles_max_budget_tier()) {
        case RELAY_BUDGET_TIER_ERROR: warn_tier = UI_TOPBAR_WARNING_ERROR; break;
        case RELAY_BUDGET_TIER_WARN:  warn_tier = UI_TOPBAR_WARNING_WARN;  break;
        case RELAY_BUDGET_TIER_NONE:
        default:                      warn_tier = UI_TOPBAR_WARNING_NONE; break;
    }
    ui_topbar_set_warning(&s_ui_home_topbar, warn_tier);

    /* wifi_status_ui.h -- TODO.md 10.9 factored this formatter out of this
     * file into its own shared module so ui_page_network.c can call the
     * exact same "human-readable WiFi state" text instead of a second copy
     * of the switch statement that used to live here. */
    char status_buf[96];
    wifi_status_ui_get_text(status_buf, sizeof(status_buf));

    /* docs/KILN_PROFILES_PLAN.md section 7.4: "the LCD must show the active
     * kiln name" -- appended onto the SAME status label (no new label
     * object, no new colour, still one line, still truncated by the
     * label's own LV_LABEL_LONG_DOT long-mode if the combined text runs
     * past the available width) rather than a second row, per the "no
     * scrolling" LCD constraint (a fixed 480x320 page has no room to grow
     * downward for a feature this small). kiln_cfg_store_get_active_id()/
     * _get_name() are cheap RAM reads (no NVS access), safe on this 1 Hz
     * LVGL-task tick. "(none)" matches KILN_CFG_NO_ACTIVE_ID's own "nothing
     * has ever been applied/saved as the starting point" meaning. */
    /* Owner rule 2026-09-19 (UI_PLAN.md section 6.8 item 6): the suffix is
     * shown only when the board holds two or more saved kiln configs.
     * ui_page_home_kiln_suffix_visible() owns that threshold (host-tested
     * at counts 0, 1, 2, 10); kiln_cfg_store_count() is a plain in_use
     * tally with no kiln_cfg_summary_t buffer at all, so nothing is put on
     * the 8192 B LVGL task stack -- the same stack the 2026-09-04 panic
     * corrupted, and which this callback is already the deepest known
     * dispatch target of. For the same reason the formatting is written
     * out here rather than behind a helper: an extra call frame on this
     * path measured +80 B in check_all_task_stack_budgets.ps1, enough on
     * its own to push lvgl over its 4880 B ceiling. */
    if (ui_page_home_kiln_suffix_visible(kiln_cfg_store_count())) {
        int32_t active_kiln_id = kiln_cfg_store_get_active_id();
        char kiln_name[KILN_CFG_NAME_MAX_LEN + 1];
        size_t used = strlen(status_buf);
        if (active_kiln_id != KILN_CFG_NO_ACTIVE_ID &&
            kiln_cfg_store_get_name(active_kiln_id, kiln_name, sizeof(kiln_name))) {
            snprintf(status_buf + used, sizeof(status_buf) - used, "  Kiln: %s", kiln_name);
        } else {
            snprintf(status_buf + used, sizeof(status_buf) - used, "  Kiln: (none)");
        }
    }
    lv_label_set_text(s_ui_home_status_label, status_buf);

    /* Same plain-C getters dashboard_http.c's GET /api/status and
     * GET /api/profile_exec handlers call -- TODO.md 10.1a's shared-backend
     * rule, not a reimplementation. */
    /* Audit L21: the heavy producer reads below (SPI, queue wait, heap walks)
     * are skipped while Home is not the active screen. The cheap RAM reads
     * above (topbar warning tier, wifi text) stay live. */
    if (lv_obj_get_screen(s_ui_home_status_label) != lv_screen_active()) {
        return;
    }
    dashboard_status_t ds;
    dashboard_get_status(&ds);
    /* ui_home_refresh_cb runs on the LVGL task's 1 Hz timer and is already
     * that task's deepest known dispatch target against the 4880 B
     * measured ceiling within its 8192 B stack (see this file's own header
     * comment above and check_all_task_stack_budgets.py) -- heap-allocate
     * rather than add a 1464-byte profile_exec_status_t stack local here,
     * same pattern as safety_cfg_http.c/dashboard_exec_http.c. */
    profile_exec_status_t *st = heap_caps_malloc(sizeof(*st), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!st) {
        return; /* out of memory -- skip this tick's repaint, next 1 Hz tick tries again */
    }
    profile_executor_get_status(st);

    /* UI_PLAN.md 6.5 -- right-quarter rail, same ds/st snapshot, no new
     * producer call. Kept out-of-line (see ui_home_rail_refresh()'s own
     * header comment in ui_page_home_internal.h) so this callback's own
     * stack frame does not grow -- it is already the lvgl task's deepest
     * known dispatch target against check_all_task_stack_budgets.py's
     * 4880 B ceiling. */
    ui_home_rail_refresh(&ds, st);

    /* UI_PLAN.md 6.1 -- same st snapshot, no new producer call. Out-of-line
     * for the same stack-budget reason as ui_home_rail_refresh() above. */
    ui_home_profile_label_refresh(st);

    /* Progress bar -- see s_ui_home_progress_wrap's own static-declaration comment.
     * dashboard_plan_exec_fields() reports elapsed 0 / total -1 for IDLE, so
     * the "running" gate below matches main_page.html's renderProgress()
     * (RUNNING/PAUSED/FAULTED/DONE only) without a separate state check
     * here. */
    {
        int64_t total_planned_s, remaining_s;
        uint32_t elapsed_s;
        bool remaining_is_estimate;
        dashboard_plan_exec_fields(st, &total_planned_s, &elapsed_s, &remaining_s, &remaining_is_estimate);

        bool bar_running = (st->state == PROFILE_EXEC_RUNNING || st->state == PROFILE_EXEC_PAUSED ||
                            st->state == PROFILE_EXEC_FAULTED || st->state == PROFILE_EXEC_DONE);
        if (!bar_running) {
            lv_obj_add_flag(s_ui_home_progress_wrap, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_ui_home_progress_wrap, LV_OBJ_FLAG_HIDDEN);

            char elapsed_buf[16];
            char label_buf[64];
            ui_home_format_duration(elapsed_s, elapsed_buf, sizeof(elapsed_buf));

            if (total_planned_s < 0) {
                /* No denominator -- elapsed only, no fill at all (matches
                 * renderProgress()'s total_planned_s==null branch). */
                lv_obj_set_width(s_ui_home_progress_fill, 0);
                lv_obj_set_style_bg_color(s_ui_home_progress_fill, UI_THEME_ACCENT_1, 0);
                snprintf(label_buf, sizeof(label_buf), "Elapsed %s", elapsed_buf);
            } else if (remaining_s < 0) {
                /* Unknown remaining (a zero/negative ramp segment) -- full
                 * track in a muted color stands in for the web's animated
                 * indeterminate stripe (see s_ui_home_progress_fill's own comment). */
                lv_obj_set_width(s_ui_home_progress_fill, lv_pct(100));
                lv_obj_set_style_bg_color(s_ui_home_progress_fill, UI_THEME_COLOR_TEXT_SECONDARY, 0);
                snprintf(label_buf, sizeof(label_buf), "Elapsed %s -- time remaining unknown", elapsed_buf);
            } else {
                int32_t pct = total_planned_s > 0
                                  ? (int32_t)((100 * (int64_t)elapsed_s) / total_planned_s)
                                  : 100;
                if (pct < 0) pct = 0;
                if (pct > 100) pct = 100;
                lv_obj_set_width(s_ui_home_progress_fill, lv_pct(pct));
                lv_obj_set_style_bg_color(s_ui_home_progress_fill, UI_THEME_ACCENT_1, 0);
                char remaining_buf[16];
                ui_home_format_duration((uint32_t)remaining_s, remaining_buf, sizeof(remaining_buf));
                snprintf(label_buf, sizeof(label_buf), "Elapsed %s -- %s left%s", elapsed_buf, remaining_buf,
                         remaining_is_estimate ? " (estimate)" : "");
            }
            lv_label_set_text(s_ui_home_progress_label, label_buf);
        }
    }

    /* docs/WEB_AUTH_PLAN.md item 10 -- cancel/expiry logging for the
     * physical credential-reset gesture. See s_ui_home_auth_reset_was_armed_live's
     * own doc comment for why this 1 Hz observation, rather than a true
     * button-press hook, is the mechanism used for "cancel". A transition
     * from armed-and-live to not-live with the gesture STILL reporting
     * armed==true (auth_reset_gesture_cancel() below actually clears it) can
     * only mean the 30 s confirm window elapsed with no confirm pressed --
     * a successful confirm already transitions the state to idle itself
     * (auth_reset_gesture_confirm()'s own contract), so this branch never
     * double-logs a real confirm. */
    {
        uint32_t now_ms = (uint32_t)(hal_time_now_us() / 1000);
        auth_reset_gesture_state_t *gesture = auth_reset_gesture_singleton();
        bool armed_live = auth_reset_gesture_is_armed_and_live(gesture, now_ms);
        if (s_ui_home_auth_reset_was_armed_live && !armed_live && gesture->armed) {
            ESP_LOGE(UI_HOME_TAG,
                     "Auth reset gesture's 30 s confirm window elapsed with no confirm -- "
                     "cancelled/expired, no credential change. Re-arm by repeating all four "
                     "corner taps.");
            auth_reset_gesture_cancel(gesture);
        }
        s_ui_home_auth_reset_was_armed_live = armed_live;
    }

    /* Safety-trip strip. Same gate the removed state-card banner used, kept
     * verbatim on purpose: diag_age_ms < SAFETY_LINK_STALE_MS, because a
     * STALE diag_state == TRIPPED is a silent link, not a live trip, and
     * painting the two identically is exactly the "guard firing vs. dead
     * peer" confusion this codebase has been careful to avoid elsewhere.
     *
     * Hidden (not blanked) when clear: LVGL skips hidden children in flex
     * layout, so this costs zero height whenever nothing is wrong, which is
     * what lets the chart still reach all the way down to the Start button. */
    if (s_ui_home_trip_strip != NULL) {
        /* Same live-trip predicate the Safety page uses (one shared derive,
         * ui_page_safety_logic.h) so the strip and the page cannot disagree. */
        bool safety_tripped = ui_safety_view_derive(ds.diag_ever_received, ds.diag_state,
                                                     ds.diag_age_ms).tripped_live;
        s_ui_home_trip_strip_is_safety = safety_tripped;
        // The BOOT-button OTA-auth bypass banner that used to rank above the
        // safety-trip text here was retired 2026-09-29 along with the
        // AP-password HMAC scheme itself: ROUTE_TIER_ADMIN is the only gate
        // on OTA routes now, and there is nothing left for a bypass window
        // to suspend. See boot_button.h/.c's deletion in the same change.
        if (safety_tripped) {
            char trip_buf[96];
            snprintf(trip_buf, sizeof(trip_buf), "SAFETY TRIP -- %s",
                     safety_trip_words_short(ds.diag_trip_reason));
            lv_label_set_text(s_ui_home_trip_strip, trip_buf);
            lv_obj_remove_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN);
        } else if (ct_leak_alarm_is_active()) {
            /* H9 CT alarm (owner decision 2026-10-04): CT current with every
             * relay commanded off. Ranked just below a live safety trip and
             * above everything informational. Short: 96-char strip, 480x320,
             * no scrolling; the channel and peak are on the web dashboard. */
            lv_label_set_text(s_ui_home_trip_strip, "CT CURRENT WITH ALL RELAYS OFF -- see dashboard");
            lv_obj_remove_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN);
        } else if (auth_reset_gesture_is_armed_and_live(auth_reset_gesture_singleton(),
                                                          (uint32_t)(hal_time_now_us() / 1000))) {
            /* docs/WEB_AUTH_PLAN.md item 10 step 4's "AUTH RESET ARMED --
             * confirm within 30 s" banner. Ranked below the live-safety-trip
             * banner above (the BOOT-button OTA-auth bypass banner that used
             * to also rank above this was retired 2026-09-29, see the
             * comment above the safety-trip branch),
             * above the config-quarantine branches below: this is a live,
             * time-boxed operator action in progress and deserves more
             * visibility than an informational config warning. Reused
             * verbatim -- same widget, same styling, no new LCD color. */
            lv_label_set_text(s_ui_home_trip_strip, "AUTH RESET ARMED -- confirm within 30 s");
            lv_obj_remove_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* Config-load-fault quarantine: the board could not decode its
             * stored zones config (either NEWER than this firmware knows,
             * e.g. after an OTA rollback past a schema bump, or older than
             * its migration chain can consume) and is running/would run on
             * firmware-default PID gains -- profile_executor_run.c refuses
             * to start a firing in this state. Below a live safety trip on
             * purpose (the OTA-bypass banner once ranked here too, retired
             * 2026-09-29 -- see the comment above the safety-trip branch), same
             * ordering as main_page.html's renderZonesConfigLoadFault().
             * Short by necessity: 96-char strip, 480x320, no scrolling. No
             * "fire anyway" affordance here either -- see
             * profile_executor_run.c's comment for why. */
            zones_cfg_load_fault_t fault;
            if (zones_config_get_load_fault(&fault)) {
                char fault_buf[96];
                if (fault.kind == ZONES_CFG_LOAD_FAULT_NEWER) {
                    snprintf(fault_buf, sizeof(fault_buf),
                             "CONFIG QUARANTINED -- v%u newer than fw v%u, reflash",
                             (unsigned)fault.on_disk_version, (unsigned)fault.fw_version);
                } else {
                    snprintf(fault_buf, sizeof(fault_buf),
                             "CONFIG QUARANTINED -- v%u unreadable, reflash matching fw",
                             (unsigned)fault.on_disk_version);
                }
                lv_label_set_text(s_ui_home_trip_strip, fault_buf);
                lv_obj_remove_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN);
            } else if (kiln_cfg_swap_get_boot_fault_kind() != KILN_CFG_SWAP_BOOT_FAULT_NONE) {
                /* M13 fix: an interrupted two-processor kiln-config swap
                 * could not be recovered at boot. For every kind but
                 * ACTIVE_ID_UNSAVED the record is left pending and the
                 * divergence/ceiling gates kiln_cfg_swap.c reuses are what
                 * refuse heat while the two sides disagree; this strip only
                 * says why. ACTIVE_ID_UNSAVED (LOW-1) is display-only: both
                 * processors agree on the applied config, heat is not gated,
                 * and only the saved "which kiln is active" marker failed --
                 * boot retries it -- so it gets softer text that does not
                 * claim anything is interrupted. Ranked above the
                 * migration-persist warning below and below a live safety
                 * trip (the OTA-bypass banner once ranked here too, retired
                 * 2026-09-29). Same 96-char/no-scroll strip; the full reason
                 * is on the web dashboard only -- these literals are the
                 * short forms that fit here. Reads the kind only, not
                 * kiln_cfg_swap_get_boot_fault()'s struct, so no second copy
                 * of its 200-byte reason lands on this stack (ds already
                 * carries one). */
                lv_label_set_text(s_ui_home_trip_strip,
                                   kiln_cfg_swap_get_boot_fault_kind() == KILN_CFG_SWAP_BOOT_FAULT_ACTIVE_ID_UNSAVED
                                       ? "KILN APPLIED -- active kiln not saved, retried at boot"
                                       : "CONFIG SWAP INTERRUPTED -- see dashboard, re-apply config");
                lv_obj_remove_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN);
            } else {
                /* M13 fix (2026-09-16): lowest priority of the branches
                 * here on purpose -- this boot's config IS valid and running;
                 * it is only a warning that a migrated blob's write-back to
                 * flash could not be confirmed, so a LATER boot might hit the
                 * quarantine branch above instead. Same strip, same 96-char/
                 * no-scroll constraint, no new colors -- see
                 * main_page.html's renderZonesConfigMigrationPersistFault()
                 * for the same condition on the web side. */
                zones_cfg_migration_persist_fault_t mfault;
                if (zones_config_get_migration_persist_fault(&mfault)) {
                    char mfault_buf[96];
                    snprintf(mfault_buf, sizeof(mfault_buf),
                             "CONFIG MIGRATION v%u->v%u NOT CONFIRMED SAVED -- re-save zones",
                             (unsigned)mfault.on_disk_version, (unsigned)mfault.fw_version);
                    lv_label_set_text(s_ui_home_trip_strip, mfault_buf);
                    lv_obj_remove_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN);
                } else if (ds.cfg_fs_format_pending) {
                    /* cfg_fs ask-first format refusal (docs/CONFIG_FILESYSTEM.md):
                     * lowest priority. While this shows, cfg is unmounted and
                     * every save is refused (owner decision 2026-10-06), so
                     * the text says so. No action from the LCD (the confirm
                     * is the web UI's, admin-gated, POST /api/cfgfs/
                     * format_confirm); the reason text is on the web
                     * dashboard/Settings page. Same strip, same 96-char/
                     * no-scroll constraint. Hides itself once the format
                     * completes (the flag clears). */
                    lv_label_set_text(s_ui_home_trip_strip,
                                       "SAVES REFUSED: CONFIG FS NEEDS FORMAT CONFIRM -- see web Settings");
                    lv_obj_remove_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN);
                } else {
                    lv_obj_add_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN);
                }
            }
        }
    }

    /* PID_EXPANSION_PLAN.md 7.4's LCD lag notice -- INFORMATIONAL, not a
     * fault (see s_ui_home_lag_notice's own build()-site comment for the styling
     * rationale). Debounce policy: ui_page_home_lag_notice_active()
     * (ui_page_home_graph.h) decides -- commit 1e03448 added
     * profile_exec_zone_status_t::ramp_lag_sustained, already debounced in
     * firmware (EXEC_SUSTAINED_LAG_S == 30s continuous), so THIS file's own
     * ~5s counter is redundant for that field and is not applied to it
     * (stacking both would be ~35s before a real lag reaches the LCD). This
     * firmware build and profile_executor.c ship in the same binary -- there
     * is no "older firmware" skew possible on-device the way main_page.html
     * faces from a stale browser tab -- so have_rich_zone_data is always
     * true here; the false branch exists for host-test parity with the web
     * decision and costs nothing. The tick counter is still advanced off
     * ramp_lock_held every tick regardless, matching the function's own
     * doc comment. profile_exec_status_t's fields are read whether or not a
     * run is active -- all false/0 while IDLE, so this is a no-op then. */
    if (s_ui_home_lag_notice != NULL) {
        /* 2026-09-15 review (review_divergence_rework_c1d2c526_2026-09-15.md,
         * HIGH 2 -- "warning invisible"): a standing (non-ceiling) config
         * divergence between the ESP's expected safety-processor config and
         * the Pico's live one previously had no on-screen surface at all,
         * so autosave could be silently blocked forever with no operator
         * ever seeing why. Reuses this same WARNING-styled widget (not the
         * ALARM-styled trip strip above -- a standing divergence never
         * disables heat) and takes priority over the ramp-lag notice below:
         * a config mismatch is rarer and more consequential than a lagging
         * ramp, and this label (like the trip strip) must not scroll on the
         * 480x320 LCD, so only one message can occupy it per tick. No new
         * httpd/zones-JSON buffers are touched -- this reads the existing
         * in-RAM divergence latch directly. */
        char diverge_reason[CONFIG_DIVERGENCE_REASON_MAX];
        bool standing_diverged = safety_ceiling_sync_is_standing_diverged(diverge_reason, sizeof(diverge_reason));

        /* No recapture poll here -- see this file's header comment. It runs
         * on safety_poll_task now. */
        if (standing_diverged) {
            char notice_buf[160];
            snprintf(notice_buf, sizeof(notice_buf), "Config mismatch (Pico): %.130s", diverge_reason);
            lv_label_set_text(s_ui_home_lag_notice, notice_buf);
            lv_obj_remove_flag(s_ui_home_lag_notice, LV_OBJ_FLAG_HIDDEN);
            s_ui_home_lag_notice_ticks = ui_page_home_lag_notice_tick(st->ramp_lock_held, s_ui_home_lag_notice_ticks);
            goto lag_notice_done;
        }

        s_ui_home_lag_notice_ticks = ui_page_home_lag_notice_tick(st->ramp_lock_held, s_ui_home_lag_notice_ticks);

        bool any_sustained = false;
        uint8_t sustained_mask = 0;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (st->zones[zi].active && st->zones[zi].ramp_lag_sustained) {
                any_sustained = true;
                sustained_mask |= (uint8_t)(1u << zi);
            }
        }

        if (!ui_page_home_lag_notice_active(true, any_sustained, s_ui_home_lag_notice_ticks)) {
            lv_obj_add_flag(s_ui_home_lag_notice, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* Zone names, not just indices -- ui_page_home_lagging_zone_indices()
             * (ui_page_home_graph.c) only hands back the pure index list;
             * looking a name up per index is this file's job (needs
             * zones_config_get_name(), NVS-backed, not host-testable) same
             * split ui_home_show_start_confirm()'s zones_buf loop above already
             * uses for a different list of zone indices. */
            uint8_t idx[MAX31856_CHANNEL_COUNT];
            size_t n = ui_page_home_lagging_zone_indices(sustained_mask, MAX31856_CHANNEL_COUNT, idx,
                                                          MAX31856_CHANNEL_COUNT);
            char zones_buf[96];
            size_t zlen = 0;
            zones_buf[0] = '\0';
            for (size_t i = 0; i < n && zlen < sizeof(zones_buf) - 1; i++) {
                char name[16];
                const char *zname =
                    (zones_config_get_name(idx[i], name, sizeof(name)) && name[0]) ? name : NULL;
                char piece[24];
                if (zname) {
                    snprintf(piece, sizeof(piece), "%s%s", zlen ? ", " : "", zname);
                } else {
                    snprintf(piece, sizeof(piece), "%sZone %u", zlen ? ", " : "", (unsigned)idx[i]);
                }
                size_t piece_len = strlen(piece);
                if (zlen + piece_len < sizeof(zones_buf)) {
                    memcpy(zones_buf + zlen, piece, piece_len + 1);
                    zlen += piece_len;
                }
            }

            char notice_buf[160];
            /* Plain sentence, lower-case lead word -- deliberately not
             * "SAFETY TRIP"-style all-caps: this is normal, expected
             * behaviour (profile_executor.h's own header comment calls the
             * ramp lock "healthy"), not an alert.
             *
             * Exactly one sustained zone: the rate/duration numbers fit and
             * are the whole point of the richer fields (owner ask: "zone 2
             * lagging: 60 C/hr commanded, 22 C/hr achieved, 145 s"). More
             * than one: rates for N zones would not fit this label at any
             * reasonable font size, so this stays to names only, same as
             * the pre-1e03448 message -- LV_LABEL_LONG_DOT (build()-site,
             * below) still truncates with an ellipsis rather than wrap or
             * scroll if the name list itself runs long, per the LCD's
             * no-scroll rule. */
            if (n == 1) {
                const profile_exec_zone_status_t *z = &st->zones[idx[0]];
                snprintf(notice_buf, sizeof(notice_buf),
                         "%s lagging %lus: %ldC/hr cmd vs %ldC/hr actual",
                         zones_buf[0] ? zones_buf : "Zone", (unsigned long)lroundf(z->ramp_lag_held_s),
                         lroundf(z->ramp_lag_commanded_rate_c_per_hr),
                         lroundf(z->ramp_lag_achieved_rate_c_per_hr));
            } else {
                snprintf(notice_buf, sizeof(notice_buf), "Waiting on %s to catch up -- setpoint paused",
                         zones_buf[0] ? zones_buf : "a zone");
            }
            lv_label_set_text(s_ui_home_lag_notice, notice_buf);
            lv_obj_remove_flag(s_ui_home_lag_notice, LV_OBJ_FLAG_HIDDEN);
        }
lag_notice_done:;
    }

    /* Audit L15 visibility rule (pinned by the _Static_asserts in
     * ui_page_home.c): the lag notice yields to the trip strip, and the
     * progress wrap hides while either strip shows, so the rail never shares
     * the column with more than one extra block. */
    if (s_ui_home_trip_strip != NULL && s_ui_home_lag_notice != NULL &&
        !lv_obj_has_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_add_flag(s_ui_home_lag_notice, LV_OBJ_FLAG_HIDDEN);
    }
    if ((s_ui_home_trip_strip != NULL && !lv_obj_has_flag(s_ui_home_trip_strip, LV_OBJ_FLAG_HIDDEN)) ||
        (s_ui_home_lag_notice != NULL && !lv_obj_has_flag(s_ui_home_lag_notice, LV_OBJ_FLAG_HIDDEN))) {
        lv_obj_add_flag(s_ui_home_progress_wrap, LV_OBJ_FLAG_HIDDEN);
    }

    /* Compact home chart -- see this file's header comment ("DESIRED SERIES
     * + PROGRESS BAR RETURN, part 2") for the idle-dot vs. whole-run-timeline
     * split. Same state==IDLE && history_count==0 gate ui_page_history.c
     * uses, so both pages flip from dot to timeline at the exact same
     * instant. profile_feasibility_plan_curve() is also used by the
     * progress-bar block further down -- computed once here and passed down
     * rather than called twice per tick. 2026-08-22: the progress bar itself
     * is gone (see this function's tail comment), but plan_pts/plan_n are
     * still needed for the chart's planned-ahead series and time-axis label,
     * so the call stays -- only its total-seconds return value is now
     * discarded. */
    /* static: LVGL-task-only, keeps the plan curve between ticks so it is
     * rebuilt only when the run's segments change (LCD audit L27) and takes
     * this callback's deepest-stack frame down by the array size. */
    static profile_plan_point_t plan_pts[1 + 2 * PROFILE_MAX_SEGMENTS];
    static size_t s_plan_cache_n = 0;
    static uint32_t s_plan_cache_key = 0;
    static bool s_plan_cache_valid = false;
    static float s_hist_t[UI_PAGE_HOME_CHART_POINTS];
    static float s_hist_c[UI_PAGE_HOME_CHART_POINTS];
    size_t plan_n = 0;
    if (st->state == PROFILE_EXEC_IDLE && profile_executor_get_history_count() == 0) {
        for (uint32_t i = 1; i < UI_PAGE_HOME_CHART_POINTS; i++) {
            s_ui_home_chart_actual_pts[i] = LV_CHART_POINT_NONE;
            s_ui_home_chart_planned_pts[i] = LV_CHART_POINT_NONE;
        }
        /* Representative zone -- same "first configured zone" convention
         * build_zone_row()'s loop and ui_page_history.c's idle-fallback
         * both use; zone_mask is meaningless before a profile has ever
         * started this boot, so there is no mask to read yet. */
        float val = NAN;
        if (s_ui_home_zone_count > 0) {
            for (size_t i = 0; i < ds.channel_count; i++) {
                if (ds.channels[i].channel == 0) {
                    if (ds.channels[i].valid && !ds.channels[i].stale) {
                        val = unit_pref_convert(ds.channels[i].temp_c, ds.temp_unit, UNIT_PREF_KIND_ABSOLUTE);
                    }
                    break;
                }
            }
        }
        /* No planned curve without a running profile -- same rule
         * ui_page_history.c's idle branch documents. */
        s_ui_home_chart_planned_pts[0] = LV_CHART_POINT_NONE;
        /* No time axis in the idle single-dot case -- there is nothing to
         * span yet (the dot never moves, see this file's header comment), so
         * a "0:00-0:00" label would be a confident lie rather than a scale.
         * Hidden here unconditionally; the running branch below is the only
         * place that ever un-hides it. */
        ui_home_chart_set_x_ticks(0.0f, false);
        /* No series drawn either (a single dot, not a line) -- see
         * ui_page_home_legend_visibility()'s own header comment for why
         * has_span=false forces the legend hidden regardless. */
        ui_home_chart_set_legend(false, false, false);
        /* No planned curve, so no "current position along the curve" to mark
         * either -- same honesty rule as the x-label above. */
        lv_obj_add_flag(s_ui_home_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
        if (!isnan(val)) {
            int32_t v = (int32_t)lroundf(val);
            s_ui_home_chart_actual_pts[0] = v;
            /* Freezing floor (see ui_home_freezing_point_disp()'s comment): only
             * raise the lower bound when the real point (v) is itself at or
             * above freezing -- i.e. only the fixed +/-10 padding dipped
             * below the floor, not a genuine sub-zero/fault reading. If v
             * itself is below freezing, axis_lo is left at v-10 unclamped so
             * the excursion stays visible instead of being clamped off the
             * bottom of the plot. */
            int32_t floor_i = (int32_t)lroundf(ui_home_freezing_point_disp(ds.temp_unit));
            int32_t axis_lo = v - 10;
            int32_t axis_hi = v + 10;
            if (axis_lo < floor_i && v >= floor_i) {
                axis_lo = floor_i;
            }
            lv_chart_set_axis_range(s_ui_home_chart, LV_CHART_AXIS_PRIMARY_Y, axis_lo, axis_hi);
            ui_home_chart_set_y_ticks(axis_lo, axis_hi, ds.temp_unit);
        } else {
            s_ui_home_chart_actual_pts[0] = LV_CHART_POINT_NONE;
            /* No reading at all -- the axis range above is untouched (stays
             * whatever it last was), so a Y label here would describe a
             * range that's no longer being drawn. Hide rather than show a
             * stale number. */
            ui_home_chart_hide_y_ticks();
        }
        lv_chart_refresh(s_ui_home_chart);
    } else {
        /* This branch is entered whenever NOT (idle && history_count==0) --
         * that includes RUNNING/PAUSED/DONE/FAULTED, but ALSO plain IDLE with
         * leftover history from a run that already ended (nothing currently
         * active). state_active distinguishes the two: only the former has a
         * real schedule to show ahead of "now", so only it calls
         * profile_feasibility_plan_curve() -- calling it while IDLE would read
         * st->segments/run_start_c left over from whatever last ran and label
         * them as a live plan, which is exactly the "confident wrong number"
         * this task's owner warned against (2026-08-21: "the chart's time
         * scale must say the truth in both idle and running states -- idle
         * has no planned horizon, it's showing recent history"). */
        bool state_active = (st->state != PROFILE_EXEC_IDLE);
        /* 2026-09-01: profile_history_entry_t.actual_c widened from one
         * float to one-per-zone (profile_executor.h, TODO.md section 0/6A.9
         * -- the dashboard's web graph used to lose every non-representative
         * zone's trace on reload, now every zone survives in the firmware
         * ring buffer). This LCD summary chart only ever drew ONE trace
         * (320x480, no room for a 3-line legend without overflow -- see this
         * file's own "LCD pages must fit without scrolling" constraint), so
         * it keeps doing exactly that: the lowest-indexed zone actually in
         * this run's mask, same "representative zone" convention the web
         * dashboard's history.csv used before this fix and ui_page_history.c
         * already documents a few lines up ("first configured zone"). Falls
         * back to zone 0 when the mask is empty (state IDLE-with-leftover-
         * history, where zone_mask is meaningless per that same comment) so
         * this never reads an out-of-range index. */
        uint8_t hist_zone = 0;
        for (uint8_t zi = 0; zi < MAX31856_CHANNEL_COUNT; zi++) {
            if (st->zone_mask & (1u << zi)) { hist_zone = zi; break; }
        }
        /* Reset the active-axis hold exactly once per RUN -- a brand new
         * firing must not inherit the previous firing's widened range.
         * 2026-09-01 defect fix (review of commit 98c3278, reset-one-side
         * class): this used to reset only on the IDLE->active state edge,
         * which is NOT the same as "a new run started". state_active also
         * covers DONE and FAULTED (it's just `state != IDLE`), and
         * profile_executor_start() only refuses RUNNING/PAUSED -- so a new
         * firing started from DONE or FAULTED goes straight to RUNNING with
         * no IDLE tick in between, and the old edge check never fired. A
         * stop/restart that both happen inside this ~1 Hz refresh interval
         * has the same symptom (both the tick before and the tick after see
         * state_active == true, no edge to observe). Both are exactly the
         * "mistaken for the same run just because this refresh happened to
         * run between them" case the old comment here claimed could not
         * happen -- it can, and did.
         *
         * ui_page_home_axis_ratchet_should_reset() (ui_page_home_graph.c) is
         * keyed to run IDENTITY instead: st->total_elapsed_s is documented
         * (profile_executor.h) to be set to 0 exactly once, at
         * profile_executor_run(), and to never decrease again until the next
         * run. A decrease observed between two active ticks -- regardless of
         * whether an IDLE tick was ever sampled in between -- is therefore
         * proof a new run began. See that function's header comment for the
         * full reasoning on why this is the best signal available without
         * modifying profile_executor.c (owned by another task). */
        if (ui_page_home_axis_ratchet_should_reset(state_active, s_ui_home_axis_hold_prev_active,
                                                    st->total_elapsed_s, s_ui_home_axis_hold_prev_elapsed_s)) {
            s_ui_home_axis_hold_have = false;
        }
        s_ui_home_axis_hold_prev_active = state_active;
        if (state_active) {
            s_ui_home_axis_hold_prev_elapsed_s = st->total_elapsed_s;
        }
        size_t count = profile_executor_get_history_count();
        float horizon_s;
        if (state_active) {
            /* segments[]/run_start_c/total_elapsed_s are all meaningful once
             * state != IDLE (profile_executor.h's own field comments) --
             * profile_feasibility_plan_curve() is pure math over that copy,
             * safe to call from this refresh timer every tick. */
            uint32_t plan_key = 2166136261u; /* FNV-1a over the plan's inputs */
            {
                const uint8_t *kb = (const uint8_t *)st->segments;
                size_t kn = (size_t)st->segment_count * sizeof(st->segments[0]);
                for (size_t k = 0; k < kn; k++) { plan_key = (plan_key ^ kb[k]) * 16777619u; }
                const uint8_t *rb = (const uint8_t *)&st->run_start_c;
                for (size_t k = 0; k < sizeof(st->run_start_c); k++) { plan_key = (plan_key ^ rb[k]) * 16777619u; }
                plan_key = (plan_key ^ (uint32_t)st->segment_count) * 16777619u;
            }
            if (!s_plan_cache_valid || plan_key != s_plan_cache_key) {
                s_plan_cache_n = 0;
                (void)profile_feasibility_plan_curve(st->segments, st->segment_count, st->run_start_c,
                                                      plan_pts, sizeof(plan_pts) / sizeof(plan_pts[0]),
                                                      &s_plan_cache_n);
                s_plan_cache_key = plan_key;
                s_plan_cache_valid = true;
            }
            plan_n = s_plan_cache_n;
            /* L26/L28: the axis follows a run that outlasts its plan, and is
             * clamped so a tiny ramp rate cannot overflow the tick math. */
            horizon_s = ui_page_home_active_horizon_s((plan_n > 0) ? plan_pts[plan_n - 1].t : 1.0f,
                                                      (float)st->total_elapsed_s);
        } else {
            /* IDLE with leftover history (count>0 is guaranteed here -- the
             * outer gate that chose this else-branch already ruled out
             * idle-with-zero-history). plan_n stays 0, so ui_home_plan_lookup() below
             * returns NaN for every bucket and the planned series is simply
             * never drawn -- there is nothing planned right now, and drawing
             * one would be a lie. horizon_s instead spans the RECENT HISTORY
             * actually retained (oldest retained sample to the newest), so
             * the x-axis label below can honestly say "recent history", not
             * a run duration that does not exist. */
            plan_n = 0;
            horizon_s = (count > 1) ? (float)(count - 1) * (float)HISTORY_SAMPLE_PERIOD_S : 1.0f;
        }

        unit_pref_t unit = unit_pref_get();
        bool have_range = false;
        float lo = 0.0f, hi = 0.0f;
        /* Count of real (non-NaN) actual points plotted this tick -- feeds
         * ui_page_home_legend_visibility()'s has_actual_multi so a lone
         * run_start_c anchor point (the very first tick of a run, before any
         * history sample exists) does not advertise an "Actual" legend row
         * for a line that has nothing visible to draw (a single point is not
         * a line, and per-point dot markers were removed elsewhere on this
         * chart -- see that function's header comment in
         * ui_page_home_graph.h). */
        size_t actual_point_count = 0;
        /* L24/L27: one executor-lock acquisition for every bucket, matched by
         * each entry's own elapsed_s (not ring index * period). Idle-with-
         * history buckets are measured from the oldest retained sample. */
        for (uint32_t i = 0; i < UI_PAGE_HOME_CHART_POINTS; i++) {
            s_hist_t[i] = (float)i * horizon_s / (float)(UI_PAGE_HOME_CHART_POINTS - 1);
        }
        if (!state_active && count > 0) {
            profile_history_entry_t oldest_e;
            if (profile_executor_get_history(&oldest_e, 0, 1) == 1) {
                for (uint32_t i = 0; i < UI_PAGE_HOME_CHART_POINTS; i++) {
                    s_hist_t[i] += (float)oldest_e.elapsed_s;
                }
            }
        }
        (void)profile_executor_history_sample_actual(
            hist_zone, s_hist_t, UI_PAGE_HOME_CHART_POINTS,
            ui_page_home_history_tolerance_s(horizon_s, UI_PAGE_HOME_CHART_POINTS, (float)HISTORY_SAMPLE_PERIOD_S),
            s_hist_c);
        for (uint32_t i = 0; i < UI_PAGE_HOME_CHART_POINTS; i++) {
            float t_i = (UI_PAGE_HOME_CHART_POINTS > 1)
                            ? (float)i * horizon_s / (float)(UI_PAGE_HOME_CHART_POINTS - 1)
                            : 0.0f;

            float planned_c = ui_home_plan_lookup(plan_pts, plan_n, t_i);
            if (plan_n > 0 && t_i > plan_pts[plan_n - 1].t) {
                planned_c = NAN; /* past the plan's end there is nothing planned (L26) */
            }
            float planned_disp = unit_pref_convert(planned_c, unit, UNIT_PREF_KIND_ABSOLUTE);
            s_ui_home_chart_planned_pts[i] = isnan(planned_disp) ? LV_CHART_POINT_NONE : (int32_t)lroundf(planned_disp);
            if (!isnan(planned_disp)) {
                if (!have_range) { lo = hi = planned_disp; have_range = true; }
                else { if (planned_disp < lo) lo = planned_disp; if (planned_disp > hi) hi = planned_disp; }
            }

            /* Actual stops at "now" -- a bucket time in the future (past
             * st->total_elapsed_s) has no recorded sample yet, and showing
             * one would fabricate data that hasn't happened. This gate only
             * makes sense while state_active (t_i is "seconds since run
             * start" there); the idle-with-history branch's t_i is "seconds
             * since the oldest RETAINED sample" instead (see horizon_s's
             * comment above) -- every bucket in that window already
             * happened, by construction, so there is nothing to gate. */
            bool have_actual = false;
            float actual_c = NAN;
            if (!state_active || t_i <= (float)st->total_elapsed_s + (float)HISTORY_SAMPLE_PERIOD_S / 2.0f) {
                if (count > 0) {
                    /* s_hist_c[] came from one locked, elapsed_s-keyed pass above. */
                    if (!isnan(s_hist_c[i])) {
                        actual_c = s_hist_c[i];
                        have_actual = true;
                    }
                } else if (i == 0) {
                    /* No samples recorded yet this tick, but the run's real
                     * starting temperature is known -- anchor bucket 0 to it
                     * rather than leaving even the start blank. */
                    actual_c = st->run_start_c;
                    have_actual = true;
                }
            }
            if (have_actual) {
                float disp = unit_pref_convert(actual_c, unit, UNIT_PREF_KIND_ABSOLUTE);
                s_ui_home_chart_actual_pts[i] = isnan(disp) ? LV_CHART_POINT_NONE : (int32_t)lroundf(disp);
                if (!isnan(disp)) {
                    actual_point_count++;
                    if (!have_range) { lo = hi = disp; have_range = true; }
                    else { if (disp < lo) lo = disp; if (disp > hi) hi = disp; }
                }
            } else {
                s_ui_home_chart_actual_pts[i] = LV_CHART_POINT_NONE;
            }
        }
        /* 2026-09-01 defect fix (review of commit a50aa64): this branch used
         * to hard-pin the axis to 0..plan_peak, discarding the accumulated
         * lo/hi range entirely -- see ui_page_home_active_y_axis_range()'s
         * header comment in ui_page_home_graph.h for the full history (it
         * superseded a 2026-08-23 "look like the web GUI" request, itself
         * superseded now: newest instruction wins, per this repo's standing
         * rule). lo/hi here are the same accumulated range the fallback
         * branch below already used -- built from BOTH series across the
         * loop above, so the planned curve's peak (anchoring the view to
         * where the firing is going) and any actual overshoot above it (no
         * longer clipped) are both already folded in before this call.
         * have_range must still be true (the same guard the old code lacked
         * -- an empty plan AND no actual sample yet has nothing to range
         * over) or there's nothing to show a Y axis for at all. */
        if (state_active && plan_n > 0 && have_range) {
            int32_t axis_lo, axis_hi;
            ui_page_home_active_y_axis_range(lo, hi, ui_home_freezing_point_disp(unit), s_ui_home_axis_hold_have,
                                              s_ui_home_axis_hold_lo, s_ui_home_axis_hold_hi, &axis_lo, &axis_hi);
            s_ui_home_axis_hold_have = true;
            s_ui_home_axis_hold_lo = axis_lo;
            s_ui_home_axis_hold_hi = axis_hi;
            lv_chart_set_axis_range(s_ui_home_chart, LV_CHART_AXIS_PRIMARY_Y, axis_lo, axis_hi);
            ui_home_chart_set_y_ticks(axis_lo, axis_hi, unit);
        } else if (state_active && plan_n > 0) {
            /* Live plan, but nothing plotted yet this tick (have_range
             * false) -- nothing to honestly range an axis over. */
            ui_home_chart_hide_y_ticks();
        } else if (have_range) {
            /* Padded 10%-of-span range with the same freezing-floor guard as
             * the idle-dot branch above, now factored into
             * ui_page_home_y_axis_range() (ui_page_home_graph.c) so the
             * degenerate lo==hi guard is host-tested rather than only
             * exercised live -- see that function's header comment. */
            int32_t axis_lo, axis_hi;
            ui_page_home_y_axis_range(lo, hi, ui_home_freezing_point_disp(unit), &axis_lo, &axis_hi);
            lv_chart_set_axis_range(s_ui_home_chart, LV_CHART_AXIS_PRIMARY_Y, axis_lo, axis_hi);
            /* Y ticks written from the SAME axis_lo/axis_hi just handed to
             * lv_chart_set_axis_range(), not read back from the chart --
             * that is what keeps them from ever drifting out of sync with a
             * range that changes every tick (see this file's header comment
             * on why lv_scale was rejected in favour of this). `unit` here is
             * the same unit_pref_get() result planned_disp/actual disp were
             * already converted through above, so the suffix can never
             * disagree with the plotted numbers. */
            ui_home_chart_set_y_ticks(axis_lo, axis_hi, unit);
        } else {
            /* No actual and no planned point converted this tick -- nothing
             * to show a range for; leave the previous axis range alone (same
             * as before this change) but don't label it, same honesty rule
             * as the idle branch's "no reading" case above. */
            ui_home_chart_hide_y_ticks();
        }
        /* X (time) ticks -- a real scale, not just a single span string
         * (2026-08-21 owner request: "I want a time scale on the LCD
         * chart"). 2026-08-30 owner request ("the LCD chart should always
         * show the same markers as the web page") and 2026-08-31 ("the times
         * ... should be at the bottom of the graph"): both spanned states
         * below now share ui_home_chart_set_x_ticks() -- main_page.html's
         * drawChartAxis() four-tick shape (tickCount=3: 0, 1/3, 2/3, full
         * span), laid out along the bottom edge -- honest per branch about
         * WHAT it is spanning, matching main_page.html's own choice of what
         * gets an axis at all (see ui_page_home_x_ticks()'s header comment in
         * ui_page_home_graph.h):
         *   - state_active: the chart's horizontal axis is the WHOLE-RUN
         *     PLANNED horizon (0..horizon_s) from profile_feasibility_
         *     plan_curve(), NOT a trailing "last N samples" window (this
         *     file's header comment, part 2) -- "0:00 | <mid> | <end>" is the
         *     actual planned duration being plotted, not a guess. Gated on
         *     plan_n > 0: horizon_s falls back to a hardcoded 1.0f guard a
         *     few lines up specifically to avoid a div-by-zero when the plan
         *     curve came back empty (e.g. a malformed/zero-segment profile)
         *     -- that fallback is a guard, not a real duration, so labelling
         *     it would be exactly the "confident wrong number" this task's
         *     own instructions warn against.
         *   - !state_active (idle, leftover history): there is no planned
         *     run to span -- horizon_s here is the RECENT-HISTORY window
         *     actually being plotted (oldest retained sample to now, see
         *     its own comment above), a genuine span exactly like
         *     main_page.html's drawHistoryChart() draws one over for its own
         *     idle-with-history case (rows.length>0 there too). Gated on
         *     count > 1 (need at least two samples for a non-zero span to be
         *     honest about); a single leftover sample has no span to show a
         *     scale for.
         *   - idle, NO history (handled entirely in the outer `if` above,
         *     not this else-branch): deliberately still gets no axis. A
         *     static per-channel dot is a single instant, not a series, to
         *     put a time scale on -- main_page.html's own equivalent state
         *     (drawIdleDots()) agrees: it only calls drawChartAxis() when
         *     there is an active plan PREVIEW (a profile picked but not yet
         *     started), and this page has no equivalent preview data to draw
         *     one from while IDLE (profile_executor_state_t's segments/
         *     run_start_c are only meaningful once state != IDLE, see
         *     profile_executor.h). Inventing a span here would be exactly the
         *     "confident wrong number" this task warns against, so it stays
         *     hidden -- see that branch's own comment. */
        bool has_span = (state_active && plan_n > 0) || (!state_active && count > 1);
        ui_home_chart_set_x_ticks(horizon_s, has_span);
        /* Same "state_active && plan_n > 0" condition that gates the dashed
         * planned series and the current-position dot below -- the legend's
         * "Plan" row must never claim a series is drawn that isn't.
         * has_actual_multi (>=2 real actual points plotted this tick) is what
         * keeps the "Actual" row honest at the very start of a run -- see
         * ui_page_home_legend_visibility()'s header comment. */
        ui_home_chart_set_legend(has_span, actual_point_count >= 2, state_active && plan_n > 0);
        if (state_active && plan_n > 0) {
            /* Current-position dot -- see its own static declaration comment.
             * Only meaningful here (a live plan with a real horizon to place
             * a position along); positioned on the ACTUAL series when a real
             * sample already exists at that bucket, else on the PLANNED
             * series (covers the first tick or two of a run before any
             * actual sample has been recorded yet) so the dot never just
             * vanishes at the very start of a firing. */
            size_t now_idx =
                ui_page_home_now_bucket_index(horizon_s, (float)st->total_elapsed_s, UI_PAGE_HOME_CHART_POINTS);
            lv_point_t dot_pos;
            bool have_dot_pos = false;
            if (s_ui_home_chart_actual_pts[now_idx] != LV_CHART_POINT_NONE) {
                lv_chart_get_point_pos_by_id(s_ui_home_chart, s_ui_home_chart_actual_series, (uint32_t)now_idx, &dot_pos);
                have_dot_pos = true;
            } else if (s_ui_home_chart_planned_pts[now_idx] != LV_CHART_POINT_NONE) {
                lv_chart_get_point_pos_by_id(s_ui_home_chart, s_ui_home_chart_planned_series, (uint32_t)now_idx, &dot_pos);
                have_dot_pos = true;
            }
            if (have_dot_pos) {
                /* lv_chart_get_point_pos_by_id() returns a position relative
                 * to the chart's own top-left content origin, per its own
                 * doc comment (lv_chart.h) -- s_ui_home_chart_now_dot is a CHILD of
                 * s_ui_home_chart, so lv_obj_set_pos() (parent-relative) is the right
                 * call here, not lv_obj_align() or an absolute coordinate. */
                lv_obj_set_pos(s_ui_home_chart_now_dot, dot_pos.x - 3, dot_pos.y - 3);
                lv_obj_remove_flag(s_ui_home_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
            } else {
                lv_obj_add_flag(s_ui_home_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
            }
        } else if (!state_active && count > 1) {
            /* Idle with leftover history: the bottom tick labels were
             * already written by the shared ui_home_chart_set_x_ticks() call above
             * (0..horizon_s of retained history, same four-tick M:SS shape as
             * the running case). No live plan in this branch, so there is
             * nothing to mark a "current position along the curve" on. */
            lv_obj_add_flag(s_ui_home_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
        } else {
            /* !has_span: idle with no history (a single static dot, not a
             * series -- see the block comment above) or a running state
             * whose plan curve came back empty. ui_home_chart_set_x_ticks() above
             * already hid all four tick labels for this case (has_span was
             * false), so only the now-dot needs hiding here. */
            lv_obj_add_flag(s_ui_home_chart_now_dot, LV_OBJ_FLAG_HIDDEN);
        }
        lv_chart_refresh(s_ui_home_chart);
    }

    /* 2026-08-22 owner request: "remove whatever it is between the graph and
     * the start button" -- the run-state summary card (profile/state text,
     * live safety-trip banner, active kiln-config name) and the progress bar
     * (elapsed/remaining time) that used to live in this space are both
     * GONE, not just hidden -- see this function's/ui_page_home_build()'s
     * removed state_card/progress_row for what used to be here.
     *
     * KNOWN TRADE-OFF, flagged rather than silently dropped: the safety-trip
     * banner this replaces was the one place a live safety trip pre-empted
     * this page's display (ROADMAP.md "safety processor faults should stop
     * firing and the GUI should reflect that, on both the LCD and web page").
     * profile_executor.c's watchdog still independently faults/latches any
     * RUNNING run on a trip -- the firing itself still stops -- but the LCD
     * home page no longer calls that out visually; ui_page_safety.c (Menu ->
     * Safety Processor) is now the only LCD surface that shows it. Confirm
     * this trade-off is intended before relying on the home page alone to
     * notice a trip. */

    /* Merged fire button -- label and color follow the same st->state this
     * function already polled above. Running/Paused reads "Stop" in the
     * danger accent; everything else (Idle/Done/Faulted) reads "Start" in
     * the start-ish accent. */
    if (st->state == PROFILE_EXEC_RUNNING || st->state == PROFILE_EXEC_PAUSED) {
        lv_label_set_text(s_ui_home_fire_btn_label, "Stop");
        lv_obj_set_style_bg_color(s_ui_home_fire_btn, UI_THEME_ACCENT_5, 0);
    } else {
        lv_label_set_text(s_ui_home_fire_btn_label, "Start");
        lv_obj_set_style_bg_color(s_ui_home_fire_btn, UI_THEME_ACCENT_4, 0);
    }

    /* Pause/Resume -- owner request 2026-08-30: "should show on the main
     * page of the lcd like it does on the web page" (app.js's sticky-bar
     * toggle button, next to Stop). Same toggle shape: one button whose
     * label/action flips with state, hidden (zero flex-row width, same
     * discipline as s_ui_home_trip_strip/s_ui_home_progress_wrap above) outside RUNNING/
     * PAUSED -- there is nothing to pause or resume in any other state.
     * No confirmation dialog, matching the web button exactly (only
     * Start/Stop confirm on either surface). */
    if (st->state == PROFILE_EXEC_RUNNING) {
        lv_label_set_text(s_ui_home_pause_btn_label, "Pause");
        lv_obj_remove_flag(s_ui_home_pause_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (st->state == PROFILE_EXEC_PAUSED) {
        lv_label_set_text(s_ui_home_pause_btn_label, "Resume");
        lv_obj_remove_flag(s_ui_home_pause_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui_home_pause_btn, LV_OBJ_FLAG_HIDDEN);
    }

    /* Owner request 2026-09-28 -- Edit visible exactly when there is a
     * firing to edit, same RUNNING/PAUSED condition as Pause/Resume above. */
    if (st->state == PROFILE_EXEC_RUNNING || st->state == PROFILE_EXEC_PAUSED) {
        lv_label_set_text(s_ui_home_edit_btn_label, "Edit");
        lv_obj_remove_flag(s_ui_home_edit_btn, LV_OBJ_FLAG_HIDDEN);
    } else if (st->state != PROFILE_EXEC_FAULTED && ui_home_live_edit_decision_owed()) {
        /* A live-edited working copy is waiting for Discard / Save as /
         * Overwrite (same condition as GET /api/profile/live's
         * pending_decision). The Edit slot is reused, relabeled; its tap is
         * the same PIN-gated callback and routes to the decision page. */
        lv_label_set_text(s_ui_home_edit_btn_label, "Keep?");
        lv_obj_remove_flag(s_ui_home_edit_btn, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_ui_home_edit_btn, LV_OBJ_FLAG_HIDDEN);
    }
    free(st);
}

/* True iff the persisted live-edit record says a decision is owed. The read
 * is an NVS read, so it is cached: re-read only when live_profile_generation()
 * moves (fork/save/clear all bump it), on the first call, and as a slow
 * backstop every UI_HOME_LIVE_EDIT_REREAD_TICKS refreshes (1 Hz). The caller
 * has already excluded RUNNING/PAUSED/FAULTED. */
#define UI_HOME_LIVE_EDIT_REREAD_TICKS 30
bool ui_home_live_edit_decision_owed(void)
{
    static bool s_valid;
    static bool s_pending;
    static uint32_t s_gen;
    static uint8_t s_ticks;
    uint32_t gen = live_profile_generation();
    if (!s_valid || gen != s_gen || ++s_ticks >= UI_HOME_LIVE_EDIT_REREAD_TICKS) {
        live_edit_record_t rec;
        s_pending = live_profile_load_record(&rec) && rec.pending;
        s_gen = gen;
        s_valid = true;
        s_ticks = 0;
    }
    return s_pending;
}

/* UI_PLAN.md 6.1 -- sets the profile-name label left of Start/Pause from the
 * SAME profile_executor_get_status() snapshot ui_home_refresh_cb() already
 * holds. No new producer call.
 *
 * While a run is up the snapshot already carries the name
 * (profile_exec_status_t::profile_name), so nothing is looked up at all.
 * While IDLE the snapshot carries NOTHING usable -- see
 * ui_home_resolve_profile_id()'s comment: get_status() zeroes profile_id for
 * an idle board, so reading it directly would label every idle dashboard
 * with user slot 0's name while Start ran something else entirely. The idle
 * branch therefore goes through that shared resolver, which is the same
 * function ui_home_fire_btn_cb()'s confirmation dialog uses.
 *
 * Owner decision, UI_PLAN.md 6.8 item 2: whenever the snapshot is not IDLE
 * the control is greyed (UI_THEME_COLOR_TEXT_SECONDARY) and its clickable
 * flag is cleared -- changing the profile under a running firing is out of
 * scope, so the affordance is removed rather than left to be tapped and
 * silently ignored. */
void ui_home_profile_label_refresh(const profile_exec_status_t *st)
{
    bool idle = (st->state == PROFILE_EXEC_IDLE);

    if (idle) {
        lv_obj_add_flag(s_ui_home_profile_btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_color(s_ui_home_profile_label, UI_THEME_COLOR_TEXT_PRIMARY, 0);

        uint8_t id = 0;
        char name[PROFILE_NAME_MAX_LEN + 1];
        if (ui_home_resolve_profile_id(st, &id) && ui_home_profile_name_for_id(id, name, sizeof(name))) {
            lv_label_set_text(s_ui_home_profile_label, name);
        } else {
            lv_label_set_text(s_ui_home_profile_label, "--");
        }
        return;
    }

    lv_obj_remove_flag(s_ui_home_profile_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(s_ui_home_profile_label, UI_THEME_COLOR_TEXT_SECONDARY, 0);
    lv_label_set_text(s_ui_home_profile_label, st->profile_name[0] ? st->profile_name : "--");
}

/* UI_PLAN.md 6.5 -- fills the right-quarter rail. Called once per tick from
 * ui_home_refresh_cb() above, given pointers to the SAME ds/st snapshot
 * that callback already fetched -- no new dashboard_get_status()/
 * profile_executor_get_status() call, no new lock. All buffers here are
 * this function's OWN small locals, not ui_home_refresh_cb()'s -- see this
 * function's prototype (ui_page_home_internal.h) for why it is kept
 * out-of-line rather than inlined into that callback's body. */
_Static_assert(UI_PAGE_HOME_RAIL_AUX_CAPTION_COUNT == KILN_IO_RELAY_COUNT,
               "aux caption table must cover every relay");

void ui_home_rail_refresh(const dashboard_status_t *ds, const profile_exec_status_t *st)
{
    for (uint32_t i = 0; i < KILN_IO_RELAY_COUNT; i++) {
        bool on = ui_page_home_rail_pill_on(ds->io_ready && !ds->io_read_failed, ds->relay_on[i]);
        lv_obj_set_style_bg_color(s_ui_home_rail_relay_pill[i],
                                   on ? UI_THEME_ACCENT_4 : UI_THEME_COLOR_TEXT_SECONDARY, 0);
        /* Spare-relay WP-6: aux-bound relays carry an "A<n>" caption (state
         * only, no control). Written only on change so a steady state never
         * invalidates the pill. */
        lv_obj_t *cap_lbl = lv_obj_get_child(s_ui_home_rail_relay_pill[i], 0);
        const char *cap_txt = ui_page_home_rail_aux_caption(ds->aux_enabled_mask, i);
        if (cap_lbl != NULL && strcmp(lv_label_get_text(cap_lbl), cap_txt) != 0) {
            lv_label_set_text(cap_lbl, cap_txt);
        }
    }

    for (uint32_t i = 0; i < MAX31856_CHANNEL_COUNT; i++) {
        if (i >= s_ui_home_zone_count) {
            lv_obj_add_flag(s_ui_home_rail_zone_row[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        lv_obj_remove_flag(s_ui_home_rail_zone_row[i], LV_OBJ_FLAG_HIDDEN);

        char name_buf[20];
        if (!zones_config_get_name((uint8_t)i, name_buf, sizeof(name_buf)) || name_buf[0] == '\0') {
            snprintf(name_buf, sizeof(name_buf), "Zone %u", (unsigned)(i + 1));
        }
        if (ui_page_home_rail_text_changed(lv_label_get_text(s_ui_home_rail_zone_name[i]), name_buf)) {
            lv_label_set_text(s_ui_home_rail_zone_name[i], name_buf);
        }

        bool valid = (i < ds->channel_count) && ds->channels[i].valid;
        char temp_buf[16];
        ui_page_home_rail_format_zone_temp(valid, ds->channels[i].temp_c, temp_buf, sizeof(temp_buf));
        if (ui_page_home_rail_text_changed(lv_label_get_text(s_ui_home_rail_zone_temp[i]), temp_buf)) {
            lv_label_set_text(s_ui_home_rail_zone_temp[i], temp_buf);
        }

        int pct = st->zones[i].active ? ui_page_home_rail_duty_pct(st->zones[i].duty) : 0;
        lv_bar_set_value(s_ui_home_rail_zone_bar[i], pct, LV_ANIM_OFF);
    }

    char watts_buf[16];
    ui_page_home_rail_format_kiln_watts(ds->power_valid, ds->power_w, watts_buf, sizeof(watts_buf));
    if (watts_buf[0] == '\0') {
        lv_obj_add_flag(s_ui_home_rail_watts_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_ui_home_rail_watts_label, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(s_ui_home_rail_watts_label, watts_buf);
    }
}
