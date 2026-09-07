// dashboard_http -- the Dashboard page's data API: live thermocouple/relay
// status and manual relay control, served over the same HTTP server
// wifi_provision_http.c already owns (see wifi_provision_http_get_server()).
//
// Reads the *same* driver instances the UART bridge reads (kiln_io_t,
// MAX31856BusClass) rather than duplicating ownership of them -- this is a
// second reader/caller alongside the UART bridge, not a second owner.
// Manual relay-ON commands go through relay_authority_on_blocked()
// (App/drivers/owners/relay_authority.c), the same chokepoint the UART bridge uses,
// so a safety fault refuses this path exactly like it refuses the PC link.
//
// Scope (TODO.md section 2): live status/manual control, plus
// start/stop/pause/status for a running profile (GET/POST /api/profile_exec*),
// which reads and drives App/drivers/control/profile_executor.c -- this module owns
// no execution state of its own, same "one reader, one owner" split as
// zones_http.c/profiles_http.c. No temperature graph yet (needs the history
// buffer, TODO.md section 0 -- designed but has no writer yet); that one is
// still deliberately absent rather than stubbed.
#ifndef DASHBOARD_HTTP_H
#define DASHBOARD_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "kiln_io.h"
#include "MAX31856.h"
#include "profile_executor.h"
#include "relay_cycles.h" /* relay_cycles_budget_t/relay_type_t/relay_budget_tier_t -- relay_life below */
#include "safety_link.h"
#include "time_sync_tz.h" /* TIME_SYNC_TZ_MAX_LEN -- see time_tz below */
#include "unit_pref.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure decision for the "safety_ready" bit reported on /api/status and by
 * dashboard_http_get_hw_ready() (readiness_http.c's "hardware present and
 * answering" check reads it too). Owner-reported bench bug: with the UART
 * between the ESP and the RP2040 safety processor physically unplugged, the
 * web UI and LCD both kept reporting the safety link as up. Root cause was
 * that both call sites computed this bit as "does the SafetyLinkClass driver
 * object exist" (a non-NULL pointer, true from the moment safety_link_start()
 * is called and forever after) instead of "is it actually receiving frames" --
 * exactly the "bus exists vs. something answered on it" trap this file's own
 * thermo_ready comment already warns about, just not applied here.
 * safety_link_get_status()'s link_up field IS the real, staleness-gated
 * answer (safety_link.c's safety_link_up_locked(), gated at
 * SAFETY_LINK_STALE_MS = 1500 ms) -- this function is only the null/error
 * handling around calling it, pulled out to a pure, host-testable predicate
 * so the "must equal link_up, never merely non-NULL" contract can't quietly
 * regress at either call site again. */
static inline bool dashboard_safety_ready(bool have_safety_link, esp_err_t status_err, bool link_up)
{
    return have_safety_link && status_err == ESP_OK && link_up;
}

/* One MAX31856 channel's reading, as reported on /api/status's "channels"
 * array -- see dashboard_get_status() below. channel is 0-based (MAX31856.h:
 * "0..2, as used on the wire") and, per this codebase's legacy zone<->channel
 * mapping (zones_config_apply_cal()'s scope note), doubles as the zone_index
 * a caller should match it against until TODO.md 10.8's many-to-one mapping
 * grows a real UI for it. */
typedef struct {
    uint8_t channel;
    float   temp_c;    /* calibration-corrected; meaningless if !valid */
    float   cj_c;
    bool    valid;
    uint8_t fault_status;
    bool    spi_failed;
    /* USER-FACING staleness: true only when age_ms exceeds
     * KILN_TEMP_STALE_AGE_MS (MAX31856.h). This is deliberately NOT the
     * driver's MAX31856Reading::stale, which means "no new conversion since
     * the previous read" and is true for a perfectly good reading taken
     * 200 ms ago. Every UI -- web, LCD, PC tools over UART -- judges
     * staleness by this same rule so they cannot disagree. */
    bool    stale;
    /* Milliseconds since this channel last produced a usable conversion;
     * MAX31856_READING_AGE_UNKNOWN if it never has. */
    uint32_t age_ms;
} dashboard_channel_status_t;

/* TODO.md 10.1a's shared-backend seam: everything status_get_handler()
 * (dashboard_http.c) serializes onto GET /api/status EXCEPT the
 * "nvs_sections" block (pure boot-time config, not live hardware state --
 * nvs_report_get() is already its own plain getter, no seam needed there).
 * Pure data-in-struct-out, zero httpd_req_t/JSON dependency -- both
 * status_get_handler() and the LCD home page (ui_page_home.c) call this and
 * read the same snapshot instead of two independent implementations of "is
 * the IO board ready" drifting apart. Performs the same live hardware reads
 * status_get_handler() always did (kiln_io_read(), MAX31856_read_all()/
 * sim_backend_read_all()), so it is not free, but it is exactly as expensive
 * as the endpoint always was -- calling it from a UI refresh timer is the
 * same cost profile as an HTTP client polling /api/status. Safe to call even
 * if dashboard_http_start() was never reached (everything reads as
 * not-ready/empty). */
typedef struct {
    bool     io_ready;
    bool     relay_on[KILN_IO_RELAY_COUNT];
    bool     io_read_failed;  /* only meaningful when io_ready */
    uint32_t relay_cycles[KILN_IO_RELAY_COUNT];

    /* RELAY_LIFE_BUDGET.md: budget state for all five counted
     * slots (the four heater relays plus RELAY_CYCLES_SAFETY_INDEX),
     * computed on read via relay_cycles_budget() -- never stored. Kept as
     * its own array rather than widening relay_cycles[] above, which stays
     * KILN_IO_RELAY_COUNT-sized on purpose (relay_cycles_get()'s doc
     * comment: existing callers pass a 4-entry buffer). relay_life_tier is
     * relay_cycles_max_budget_tier(), the value the LCD/web indication
     * actually gates on. */
    relay_cycles_budget_t relay_life[RELAY_CYCLES_COUNT];
    relay_type_t          relay_life_type[RELAY_CYCLES_COUNT];
    relay_budget_tier_t   relay_life_tier;

    bool     thermo_ready;
    size_t   channel_count;   /* <= MAX31856_CHANNEL_COUNT, only this many of
                                * channels[] are populated */
    dashboard_channel_status_t channels[MAX31856_CHANNEL_COUNT];
    /* opus review, commit f3a1600, G2b: straight passthrough of
     * MAX31856_bus_spi_wedged() -- the shared SPI owner (esp_spi_owner.h)
     * that this bus AND the display both transfer through has given up and
     * latched wedged. Previously this state was invisible anywhere but two
     * ESP_LOGE lines; now it is a real field on the same status snapshot
     * every other hardware-health bit here already reports through. True
     * here means every thermocouple channel and the display are effectively
     * dead until spi_owner_deinit()/re-init recovers it (a reboot, today --
     * see esp_spi_owner.h's wedged field comment). Rendered as a prominent
     * banner by main_page.html's renderSpiWedged() (opus review, commit
     * 9fc55d9, M4) -- not just a JSON field nothing reads. */
    bool     thermo_spi_wedged;

    /* DISPLAY_ST7796_PLAN.md 9.1: passthrough of lvgl_port_get_flush_stats()
     * -- makes the "measure first" flush-duration number a `curl .../api/status`
     * away instead of requiring a bench session with a scope. flush_max_us is
     * a running high-water mark since boot (never reset by reading it);
     * flush_count is 0 whenever LVGL has not flushed yet (no display, or
     * screen_idle blanked for the whole session), which distinguishes "no
     * data yet" from "the display never has to redraw" for a reader that
     * only looked once. */
    uint32_t flush_last_us;
    uint32_t flush_max_us;
    uint32_t flush_count;

    bool     safety_ready;
    bool     zones_config_valid;

    /* ROADMAP.md M6 "GUI shows safety temperature, enclosure temperature and
     * power" -- read straight from safety_link_get_status()'s cache
     * (safety_link.h's safety_link_status_t), not re-parsed from a frame
     * here: TODO.md 10.1a's shared-backend rule applies to the safety link
     * exactly like it does to kiln_io/MAX31856 above.
     *
     * safety_temp_c / enclosure_temp_c come from LINK_PROTOCOL.md sec 6 Frame
     * A (SAFETY_CMD_GET_STATUS, the 23-byte layout already parsed by
     * safety_link.c's safety_apply_status()): tc_temp_c (bytes 2..5, the
     * safety-processor's own thermocouple) and cj_temp_c (bytes 6..9, the
     * MAX31856's cold junction -- LINK_PROTOCOL.md sec 7's "Enclosure
     * temperature" row: "this is the electronics enclosure, not the kiln").
     * Both are NaN when TEMP_VALID is clear or nothing has ever arrived --
     * *_valid is exactly !isnan(), computed once here so callers never have
     * to isnan() themselves.
     *
     * power_w comes from LINK_PROTOCOL.md sec 6 Frame E (SAFETY_CMD_POWER,
     * 0x0E, p_total_w) via safety_link.c's safety_apply_power() /
     * safety_drain_inbox() dispatch and firmware/CommonFW's kilnlink_power.h
     * codec (mirrored by hand in safety_link.c -- see uart_task_ids.h's
     * SAFETY_CMD_POWER comment for why). power_valid is true once a Pico
     * that implements Frame E has pushed at least one POWER frame and its
     * p_total_w is a real number (not NaN, e.g. because mains_voltage_v
     * isn't configured or a channel clipped); it stays false -- power_w
     * stays NaN -- for the entire time no such Pico exists, which is the
     * expected state in this environment (ROADMAP.md M0/M5/M6, TODO.md
     * 10.10). */
    bool     safety_temp_valid;
    float    safety_temp_c;
    /* ROADMAP.md "Safety TC display audit, 2026-09-05" -- mirrors
     * safety_tc_is_separate_physical_sensor() (safety_link.h). Consumers of
     * safety_temp_c/safety_temp_valid must treat that reading as belonging
     * to a distinct, independently-faultable sensor (i.e. show it under a
     * "Thermocouple faults"-style display) ONLY when this is true. False
     * covers both "unknown" (pre-V3 Pico) and "confirmed reused from a main
     * zone's probe" -- neither is a separate sensor to warn about here. */
    bool     safety_tc_is_separate_sensor;
    bool     enclosure_temp_valid;
    float    enclosure_temp_c;
    bool     power_valid;
    float    power_w;

    /* 2026-08-27: the safety processor's three raw current-sense channels
     * (safety_link_status_t::current_a, LINK_PROTOCOL.md sec 6 Frame A) --
     * previously read by kiln_call's own safety_get_status text but never
     * exposed on this JSON API at all, which is what left the Thermocouples
     * & Zones page's new per-zone ct_mask (zones_http.h) with no live number
     * to show next to it. Same NaN-means-never-arrived convention as
     * safety_temp_c/enclosure_temp_c above -- safety_link.c never gates
     * these on TEMP_VALID (current sense is independent hardware from the
     * thermocouple), so each channel is individually valid the instant any
     * status frame has ever arrived, NaN before that. Index i = CT channel
     * i+1, matching zone_cfg_t::ct_mask's bit-N-1-is-channel-N convention. */
    float    ct_current_a[3];

    /* Raw 16x-oversampled ADC counts per channel, independent of
     * calibration -- LINK_PROTOCOL.md Frame E's counts_avg field
     * (2026-09-06, safety_link_status_t::power_channel_counts_avg). Unlike
     * ct_current_a[] above, this is never NaN-able (it's a uint16, not a
     * float derived from an uncommissioned k_ct_v_per_a) -- validity is
     * ct_counts_valid instead, false until at least one V2 POWER frame has
     * arrived (never for a Pico that predates KILNLINK_PROTOCOL_VERSION 11).
     * Closes CURRENT_SENSE.md sec 4's "no path from the running board to
     * raw ADC counts" gap -- this is that path's ESP-side terminus. */
    uint16_t ct_counts[3];
    bool     ct_counts_valid;

    /* K4, the safety processor's OWN relay -- straight from safety_link_
     * status_t::flags (SAFETY_FLAG_RELAY/SAFETY_FLAG_ENABLED), so the
     * dashboard can show it the same way as the four ESP-owned relays
     * (relayStatusHtml() in main_page.html) instead of leaving the one
     * relay that actually gates heat invisible next to them.
     *
     * safety_relay_known: false unless this poll's safety_link_get_status()
     * actually succeeded -- null-until-known, same convention diag_state
     * etc. above already use (added 2026-08-27, Opus review: without it
     * memset(out, 0, ...)'s default false read as "confirmed de-energized"
     * for a link that had simply never answered).
     *
     * safety_heating_enabled == SAFETY_FLAG_ENABLED means "SaftyFW's
     * relay_owner state machine is currently ARMED (not tripped)" -- true
     * on any healthy, past-its-grace-period Pico REGARDLESS of whether
     * anyone ever sent SAFETY_CMD_REQUEST_ENABLE. It is NOT "heat was
     * granted"; do not read it as "was my enable request honored" (that bug
     * shipped once already in danger_mode.c's diagnostics-page tile,
     * 2026-08-27 -- see danger_mode.h's doc comments on
     * danger_mode_get_heat_requested()/danger_mode_get_relay_status()). */
    bool     safety_relay_known;
    bool     safety_relay_energized;
    bool     safety_heating_enabled;
    /* The ESP's own heat-block bitmask (safety_link_get_fault_sources(), a
     * bitwise OR of safety_fault_source_t). Reported because until now NO
     * interface -- web, LCD or serial -- could answer "why did nothing turn
     * on?". relay_authority_on_blocked() refuses every relay-ON while any bit
     * here is set, and a profile run in that state reports itself as running,
     * unfaulted, at duty 0. 0 means nothing is blocking heat. */
    uint32_t heat_block_sources;
    /* Bit N set for each zone left latched-blocked by a guard trip
     * (relay_authority_latched_blocked_mask()). Reported for the same reason
     * heat_block_sources is: the latch outlives the firing that set it and
     * refuses heat on that zone until a new firing or an autotune clears it,
     * and until now nothing displayed it anywhere. */
    uint8_t  zone_blocked_mask;

    /* TODO.md 9.0's deferred "GUI names both versions and which one is
     * older" item: this firmware's own KILNLINK_PROTOCOL_VERSION (always
     * known, not link-dependent) plus whatever the Pico last announced via
     * ANNOUNCE_VERSION/FW_VERSION (LINK_PROTOCOL.md sec 4), straight from
     * safety_link_get_peer_version_status() -- same shared-backend rule as
     * safety_temp_c/enclosure_temp_c above. link_version_known is false (and
     * peer_protocol_version/peer_min_compatible are meaningless) until the
     * Pico has pushed at least one FW_VERSION frame; on this build (no Pico
     * attached, ROADMAP.md M0's bench-confirmed dead link) it stays false,
     * which is the honest, designed-for state. link_version_compatible is
     * only meaningful when link_version_known is true. */
    uint16_t self_protocol_version;
    bool     link_version_known;
    bool     link_version_compatible;
    uint16_t peer_protocol_version;
    uint16_t peer_min_compatible;

    /* ROADMAP.md M5's Pico -> ESP telemetry: SAFETY_CMD_DIAG (Frame B) and
     * SAFETY_CMD_TRIP_EVENT (Frame D) now have a decode path in safety_link.c
     * (safety_apply_diag()/safety_apply_trip_event(), dispatched from
     * safety_drain_inbox()) -- straight passthrough of
     * safety_link_status_t's diag_ and trip_ fields, same shared-backend rule
     * as safety_temp_c/power_w above. *_ever_received false (fields below
     * meaningless) until the Pico has actually pushed one, which it cannot
     * on this bench build (ROADMAP.md M0: no live link). trip_reason stays
     * cached indefinitely once received -- "why did it trip" must remain
     * answerable long after the trip cleared, see safety_link.h. */
    bool     diag_ever_received;
    uint8_t  diag_trip_reason;
    uint16_t diag_warn_mask;
    uint16_t diag_trip_mask;
    uint8_t  diag_state;
    /* How old the DIAG frame this diag_state/diag_trip_reason came from is --
     * shares safety_link_status_t's single cached_tick with the overall link
     * age (safety_link.c), so this is that same age, exposed under the diag_
     * name for callers reasoning about diag_state specifically. The GUI's
     * "safety processor tripped" banner (main_page.html/ui_page_home.c) must
     * not act on a stale diag_state == TRIPPED left over from before the
     * link went silent -- that case is already the distinct "link is dead"
     * message, not a live trip -- so it gates on this being fresher than
     * SAFETY_LINK_STALE_MS (safety_link.h), same threshold
     * safety_link_send_clear_trip() already uses to judge the same data. */
    uint16_t diag_age_ms;
    uint8_t  diag_context_age_100ms;
    uint32_t diag_context_frames_ok;
    uint32_t diag_context_frames_bad;
    uint32_t diag_tx_frames_dropped;

    bool     trip_event_ever_received;
    uint8_t  trip_reason;
    uint32_t trip_event_age_ms;
    float    trip_safety_tc_c;
    float    trip_deciding_threshold;
    /* safety_link_status_t's trip_current_a[3] and trip_context_age_100ms,
     * straight passthrough -- both already arrive on Frame D (TRIP_EVENT)
     * and were simply not copied into this struct before. current_a[] is
     * the deciding number for S3 (load stuck on) and S9 (contactor still
     * shows current after being commanded off); context_age_100ms is the
     * deciding number for S6b (link went silent for longer than its
     * timeout) -- see safety_trip_words_cause_numbered()'s call sites for
     * exactly which guard uses which. Both NAN/255 (never/unknown) until a
     * real TRIP_EVENT is received, same convention as the two floats above. */
    float    trip_current_a[3];
    uint8_t  trip_context_age_100ms;
    /* safety_link_status_t's trip_fault_sources, straight passthrough --
     * this board's OWN fault_sources bitmask, snapshotted at the moment the
     * currently-cached trip latched. Meaningful only when trip_reason == 6
     * (S6a, main-controller fault) -- see safety_link.h's field comment for
     * why every other guard's cause lives entirely on SaftyFW's side.
     * Distinct from heat_block_sources above (that one is LIVE, this one is
     * frozen at trip time; a source can assert, cause a trip, and release
     * again, and the two fields will then legitimately disagree). */
    uint32_t trip_fault_sources;
    /* 2026-08-28 audit fix (N3): safety_link_status_t's trip_fault_sources_
     * valid, straight passthrough -- true only when trip_fault_sources above
     * was captured on a trip_seq change THIS BOOT ACTUALLY WITNESSED. False
     * after an ESP reboot with a trip still latched on the Pico (the first
     * resend looks identical to a new trip from here, but the snapshot would
     * be of THIS boot's fault lines, not the real trip instant's). JSON/LCD
     * renderers of trip_fault_sources MUST check this and show "not
     * captured" rather than a plausible-looking wrong value when false. */
    bool     trip_fault_sources_valid;

    /* TODO.md owner-report item 5 (2026-08-21): the safety processor's own
     * build identity and config commissioning state, straight from
     * safety_link_get_peer_build_status() -- same shared-backend rule as
     * every other safety_link.h field above. safety_build_known is false
     * (fields below meaningless) until at least one FW_VERSION frame parsed
     * far enough to reach config_crc -- see safety_link.h's peer_build_known
     * comment for why this is stricter than link_version_known. Strings are
     * copied out with explicit lengths (not null-terminated on the wire). */
    bool     safety_build_known;
    bool     safety_build_dirty;
    char     safety_build_commit[65];   /* peer_build_commit + NUL */
    char     safety_build_datetime[33]; /* peer_build_datetime + NUL */
    uint8_t  safety_config_version;
    uint16_t safety_config_crc;

    /* UI_PLAN.md section 5's one genuinely-missing field set for the web
     * diagnostics page (ui_page_diagnostics.c's ESP-only half, section 8's
     * "Data already on the wire" note): firmware version/build strings,
     * uptime, reset reason, and per-heap-region free/largest-free-block/
     * minimum-ever-free. Always known/populated -- unlike the safety-link
     * fields above, none of this depends on a Pico being attached, so there
     * is no *_valid companion flag to check; a field here is only "wrong" if
     * esp_app_get_description() itself returns NULL (never observed on this
     * target, but see fw_version_known below for the one honest fallback
     * this code still has to make room for). */
    bool     fw_version_known;              /* false only if esp_app_get_description() returned NULL */
    char     fw_version[32];                /* esp_app_desc_t::version, ESP_APP_DESC_VERSION_SIZE-sized */
    char     fw_build[40];                  /* esp_app_desc_t::date + ' ' + time, e.g. "Aug 20 2026 14:03:11"; sized for
                                              * gcc's worst-case format-truncation analysis of two 16-byte fixed
                                              * esp_app_desc_t fields (15 usable chars each) plus separator + NUL */
    uint32_t uptime_s;                      /* hal_time_now_us() / 1e6 -- monotonic since this boot */
    const char *reset_reason;               /* esp_reset_reason() decoded to a short static string */

    /* MALLOC_CAP_INTERNAL (on-chip DRAM) and MALLOC_CAP_SPIRAM (external
     * PSRAM), each: current free bytes, the single largest contiguous free
     * block, and the worst-case (lowest-ever) free-bytes low-water mark since
     * boot. heap_internal_largest_free_block is the number that actually
     * matters on this board: measured 9216 bytes against LVGL's 8192-byte
     * task stack allocation (lvgl_port.c) -- about 1 KB of headroom, and the
     * tightest resource this firmware has. It is the one number worth putting
     * front-and-center on the diagnostics page rather than burying it in a
     * table alongside the SPIRAM figures, which have an order of magnitude
     * more slack (8 MB octal PSRAM, CONFIG_SPIRAM_USE_MALLOC=y). SPIRAM free
     * reads back as a real 0 (not "n/a") on a board built without
     * CONFIG_SPIRAM -- same "no separate validity bit, so 0 is shown as a
     * real 0 KB, not invented as n/a" convention ui_page_diagnostics.c's own
     * refresh_cb() comment already documents for exactly this call. */
    size_t   heap_internal_free;
    size_t   heap_internal_largest_free_block;
    size_t   heap_internal_min_free;
    size_t   heap_internal_total;
    size_t   heap_spiram_free;
    size_t   heap_spiram_largest_free_block;
    size_t   heap_spiram_min_free;
    size_t   heap_spiram_total;

    /* MALLOC_CAP_DMA (DMA-capable internal memory) -- DRAM_PSRAM_PLAN.md
     * Phase 0 (4.1). Added specifically for Phase 1, which lowers
     * SPIRAM_MALLOC_ALWAYSINTERNAL: an explicit MALLOC_CAP_DMA/_INTERNAL
     * request is unaffected by that threshold (it always lands internal),
     * but the *pool* it draws from is the same internal heap the threshold
     * change puts under more pressure from everything else -- this field is
     * how a future soak would notice a DMA allocation starting to starve
     * that it otherwise couldn't see. A strict subset of heap_internal
     * above, not new information about total internal DRAM -- both are
     * carved from MALLOC_CAP_INTERNAL, they just answer "how much of that is
     * usable for DMA" vs. "how much is there at all". Same "0 is a real
     * reading, never invented" convention as heap_spiram above. */
    size_t   heap_dma_free;
    size_t   heap_dma_largest_free_block;
    size_t   heap_dma_min_free;
    size_t   heap_dma_total;

    /* Owner request 2026-08-27: flash usage on the dashboard, mirroring the
     * LCD Firmware page's facts (ui_page_diagnostics.c's build_firmware_
     * statics()). flash_size is the whole chip (esp_flash_get_size());
     * flash_partition_size is the running OTA slot's fixed capacity;
     * flash_used is how many bytes of that slot the running image actually
     * occupies (esp_image_get_metadata(), not the slot's capacity) --
     * "used" means the image, not the partition table's grant. *_known is
     * false (bare 0 elsewhere) only if the underlying esp_flash_get_size()/
     * esp_ota_get_running_partition()/esp_image_get_metadata() call itself
     * failed -- see dashboard_get_status()'s own comment for why that's
     * treated as "should never happen on real hardware" rather than given a
     * fabricated fallback number. */
    bool     flash_size_known;
    uint32_t flash_size;
    uint32_t flash_partition_size;
    bool     flash_used_known;
    uint32_t flash_used;

    /* 2026-08-21, ROADMAP.md "a real shared temperature-unit setting": the
     * device-side source of truth (unit_pref.c), read here so the LCD home
     * page (ui_page_home.c) and GET /api/status's "temp_unit" field can never
     * disagree about which unit is currently selected -- same
     * shared-backend rule TODO.md 10.1a already established for every other
     * field on this struct. DISPLAY-ONLY: nothing else in this struct (every
     * temp_c/cj_c/safety_temp_c/etc. field above) is converted by this value
     * -- those all stay Celsius here, exactly as before this field existed.
     * Each renderer calls unit_pref_convert(..., temp_unit, ...) itself at
     * the point it formats a label, which is the actual "convert at the
     * boundary" this preference's header comment asks for. */
    unit_pref_t temp_unit;

    /* 2026-09-02, forthcoming "ramp assist" feature: the kiln-wide (not
     * per-zone) persisted on/off flag (ramp_assist_cfg.h), reported here so
     * the diagnostics page's toggle -- and any other reader -- shows the
     * board's ACTUAL current state rather than an assumed default. This
     * struct is the flag only; nothing in this codebase yet reads it to
     * change ramp/dwell behaviour -- see ramp_assist_cfg.h's header comment. */
    bool ramp_assist_enabled;

    /* 2026-08-30, PROFILES.md "Scheduled start + candling": time_sync.c's
     * wall-clock status, for display/scheduling-intent only -- see that
     * module's header comment for the hard boundary (never a duration/
     * control source). time_synced false means the board has not completed
     * an SNTP sync since boot; time_now_epoch/time_last_sync_epoch are both
     * 0 in that case, same "0 is honest, not invented" convention as
     * flash_size_known above, since an un-synced RTC's raw epoch is
     * meaningless free-run noise. time_tz is always a valid, non-empty,
     * NUL-terminated string (time_sync_tz_effective()'s guarantee) even
     * when time_synced is false -- the configured TZ is knowable and
     * displayable independent of whether a sync has ever landed. */
    bool   time_synced;
    time_t time_now_epoch;
    time_t time_last_sync_epoch;
    char   time_tz[TIME_SYNC_TZ_MAX_LEN + 1];
} dashboard_status_t;

void dashboard_get_status(dashboard_status_t *out);

/* TODO.md 10.1a shared-backend seam: the same total_planned_s/elapsed_s/
 * remaining_s/remaining_is_estimate math GET /api/profile_exec serializes,
 * pulled out so ui_page_home.c's LCD progress bar reads identical numbers to
 * main_page.html's web one instead of a second implementation of this
 * switch. See dashboard_http.c's own doc comment on the function body for
 * the per-state rules (IDLE/DONE/FAULTED/RUNNING/PAUSED). out_total_planned_s
 * is -1 (unknown, e.g. IDLE or an unknowable ramp duration), out_remaining_s
 * is -1 (unknown) or 0 (DONE) or >0; out_elapsed_s is always a real count of
 * seconds (0 while IDLE). */
void dashboard_plan_exec_fields(const profile_exec_status_t *st, int64_t *out_total_planned_s,
                                uint32_t *out_elapsed_s, int64_t *out_remaining_s, bool *out_remaining_is_estimate);

/* Outcome of dashboard_set_relay() below -- one variant per distinct refusal
 * reason its HTTP-facing callers' status codes distinguish (diagnostics_http.c's
 * danger_relay_post_handler() today; formerly also dashboard_http.c's
 * relay_post_handler() for POST /api/relay, removed 2026-08-27 with
 * manual_page.html, its only caller -- see this file's removal comment near
 * dashboard_set_relay()): 400 for a missing board or an out-of-range relay,
 * 403 for the two relay_authority.h refusals, 500 for a kiln_io write
 * failure, kept as an
 * enum instead of a bool+err_msg pair (profile_executor_run()'s style)
 * because the caller needs to pick between four *different* HTTP status
 * codes / user-facing messages, not just show one string. */
typedef enum {
    DASHBOARD_RELAY_OK = 0,
    DASHBOARD_RELAY_ERR_NO_BOARD,     /* s_dash.io is NULL -- no relay board attached */
    DASHBOARD_RELAY_ERR_RANGE,        /* relay_index outside 1..KILN_IO_RELAY_COUNT */
    DASHBOARD_RELAY_ERR_OWNED,        /* relay_authority_manual_blocked_by_owner() */
    DASHBOARD_RELAY_ERR_SAFETY,       /* relay_authority_on_blocked() (only checked for on==true) */
    DASHBOARD_RELAY_ERR_IO_FAIL,      /* kiln_io_set_relay() itself returned non-ESP_OK */
    /* 2026-08-21: mirrors kiln_io_owner.h's new KILN_IO_OWNER_RELAY_ERR_UPDATING
     * -- a manual relay-ON refused because an ESP/Pico firmware update is in
     * progress, not because of any real safety fault. Previously
     * dashboard_set_relay() mapped this case onto DASHBOARD_RELAY_ERR_SAFETY,
     * which read as "something is faulted" to relay_post_handler()'s HTTP
     * response and to ui_page_temperature.c's LCD message, when nothing was.
     * APPENDED (not inserted after ERR_SAFETY) for the same reason
     * kiln_io_owner.h's own new member was appended -- see that header's
     * comment; this enum has the identical "no explicit numeric values, plain
     * positional enum" shape. Checked this enum's other uses before choosing
     * append over insert: only dashboard_http.c's own switch and
     * ui_page_temperature.c's switch read it (both greppable, both updated
     * alongside this), and it is not persisted or sent over the UART wire --
     * uart_bridge.c has its own, separate translation directly off
     * kiln_io_owner_relay_result_t, not off this type. */
    DASHBOARD_RELAY_ERR_UPDATING,
} dashboard_relay_result_t;

/* TODO.md 10.1a's shared-backend seam, extracted from the old POST /api/relay
 * handler (removed 2026-08-27 -- see this file's dashboard_relay_result_t
 * comment) the same way dashboard_get_status() was pulled out of
 * status_get_handler() -- so ui_page_temperature.c's per-zone manual relay
 * toggle goes through the exact same ownership/safety-fault gating the web
 * dashboard's manual override does (relay_authority_manual_blocked_by_owner(),
 * relay_authority_on_blocked() for on==true only -- turning OFF is never
 * gated, see relay_authority.h) and the exact same write (kiln_io_set_relay()),
 * not a second copy of either check.
 *
 * relay_index is 1-based (kiln_io_set_relay's convention, matches
 * zone_cfg_t::relay_mask's bit-N-1-is-relay-N numbering). out_safety_sources,
 * if non-NULL, is set to the fault-source bitmask backing a
 * DASHBOARD_RELAY_ERR_SAFETY result (for logging/reporting) and left
 * untouched otherwise. Safe to call even if dashboard_http_start() was never
 * reached (reads as DASHBOARD_RELAY_ERR_NO_BOARD). */
dashboard_relay_result_t dashboard_set_relay(uint8_t relay_index, bool on, uint32_t *out_safety_sources);

/* Registers the dashboard's routes on the server wifi_provision_http.c
 * already started. Any pointer may be NULL (board not attached/not up) --
 * GET /api/status reports that honestly rather than faking data, matching
 * app_main's existing "partial hardware is still worth reporting"
 * convention for boot_fault_sources. Call after the I2C/SPI/thermo
 * bring-up block in app_main, once it's known what actually came up. */
esp_err_t dashboard_http_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                               SafetyLinkClass *safety_or_null);

/* Read-only accessor for readiness_http.c (TODO.md 8.3): the same three
 * hardware-answering flags /api/status reports as io_ready/thermo_ready/
 * safety_ready, without readiness_http.c needing its own copy of the
 * io/thermo/safety pointers or the "did anything actually answer" read
 * logic -- one owner, one point of truth, same discipline as
 * zones_config_get_*() being the only way profiles_http.c touches zone
 * config. thermo_ready performs the same live read status_get_handler()
 * does (a board-less bus reads back count==0 with no error), so calling
 * this is not free, but it is called only when the readiness page is
 * requested, not on any hot path. Safe to call even if
 * dashboard_http_start() was never reached (all three read as false). */
void dashboard_http_get_hw_ready(bool *out_io_ready, bool *out_thermo_ready, bool *out_safety_ready);

#ifdef __cplusplus
}
#endif

#endif // DASHBOARD_HTTP_H
