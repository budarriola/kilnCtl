#include "dashboard_http_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h> /* strcasecmp -- unit_pref_post_handler's "fahrenheit"/"celsius" match */

#include "esp_flash.h" /* esp_flash_get_size() -- flash_size below, same call as ui_page_diagnostics.c */
#include "esp_heap_caps.h"
#include "esp_image_format.h" /* esp_image_get_metadata() -- flash_used below, address/size now sourced from hal_sysinfo */
#include "esp_log.h"

#include "hal_sysinfo.h" /* hal_sysinfo_get_running_partition()/_get_build_info()/_reset_reason() -- see below */
#include "hal_time.h" /* hal_time_now_us() -- uptime_s below, was esp_timer_get_time() */

#include "autotune_engine.h"
#include "boot_button.h" /* boot_button_ota_bypass_active()/_remaining_ms() -- see the GET /api/status fields below */
#include "lvgl_port.h" /* lvgl_port_touch_is_calibrated() -- see the "touch_calibrated" /api/status field below */
#include "danger_mode.h" /* danger_mode_active() -- profile_exec_start_post_handler()'s mutual-exclusion refusal */
#include "dashboard_json.h" /* json_escape()/append_zone_status_json() -- split out for host-testability, see that header */
#include "heat_interlock.h" /* HEAT_INTERLOCK_REASON_MAX -- see the ERR_UPDATING case below */
#include "http_form.h"
#include "kiln_io_owner.h"
#include "nvs_report.h"
#include "ota_http.h" /* ota_http_heat_blocked_by_update() -- see the ERR_UPDATING case below */
#include "profile_executor.h"
#include "profile_feasibility.h" /* profile_feasibility_plan_curve() -- the duration model, see below */
#include "profiles_http.h" /* profiles_http_get() -- /api/profile_plan, see that handler below */
#include "relay_authority.h"
#include "time_sync.h" /* time_sync_get_status() -- see the time_synced/time_now_epoch/etc fields below */
#include "relay_cycles.h"
#include "run_state.h"
#include "safety_trip_words.h" /* shared cause/remedy/fault-source decode -- see that header's own comment */
#include "sim_backend.h"
#include "uart_task_ids.h"
#include "ramp_assist_cfg.h"
#include "unit_pref.h"
#include "watchdog_cfg.h"
#include "wifi_provision_http.h"
#include "zones_config_accessors.h"

const char *DASH_TAG = "dashboard_http";
/* application/x-www-form-urlencoded, "relay=4&on=1" plus headroom -- same
 * reasoning as wifi_provision_http.c's PROV_BODY_MAX: bounded well above
 * what a legitimate request needs, checked against Content-Length before a
 * single byte is read. */

struct dashboard_board_objects s_dash;

/* json_escape()/append_zone_status_json(): declared in dashboard_json.h,
 * defined in dashboard_json.c -- split out (Opus review, round 3) so they
 * can be host-tested without dragging in lvgl_port.h's LCD/touch driver
 * stack, which this file #includes at file scope and which does not compile
 * on the host MSVC toolchain (ILI9488.h's __attribute__((format(...)))).
 * See dashboard_json.h's own doc comment. Every call site below keeps
 * calling them by the same names as before this split. */

/* hal_reset_reason_t -> short static string, for the diagnostics page's
 * "why did this boot happen" field (UI_PLAN.md section 5). This table's
 * wording is this file's own and must NOT be collapsed with ui_page_
 * diagnostics.c's reset_reason_str() -- see hal_sysinfo.h's top comment.
 * Every hal_reset_reason_t value has a case here, so "unknown" only fires
 * for a future value this file hasn't been updated for (or the reset-
 * reason-not-yet-mapped HAL_RESET_UNKNOWN itself). */
static const char *reset_reason_name(hal_reset_reason_t r)
{
    switch (r) {
    case HAL_RESET_UNKNOWN:    return "unknown";
    case HAL_RESET_POWERON:    return "power-on";
    case HAL_RESET_EXT:        return "external pin";
    case HAL_RESET_SW:         return "software (esp_restart)";
    case HAL_RESET_PANIC:      return "panic/exception";
    case HAL_RESET_INT_WDT:    return "interrupt watchdog";
    case HAL_RESET_TASK_WDT:   return "task watchdog";
    case HAL_RESET_WDT:        return "other watchdog";
    case HAL_RESET_DEEPSLEEP:  return "wake from deep sleep";
    case HAL_RESET_BROWNOUT:   return "brownout";
    case HAL_RESET_SDIO:       return "SDIO";
    default:                   return "unknown";
    }
}

/* TODO.md 10.1a: the data-gathering half of GET /api/status, pulled out into
 * a plain function so the LCD home page (ui_page_home.c) reads the exact
 * same snapshot this handler serializes, rather than a second reimplementation
 * against kiln_io/MAX31856_read_all/etc. See dashboard_http.h for the struct
 * and the "why not nvs_sections too" note. */
/* The one-time flash facts /api/status reports. Read through a dedicated
 * helper, and primed at startup by dashboard_http_start(), for a reason
 * beyond tidiness: esp_flash_get_size() and esp_image_get_metadata() disable
 * the cache while they run, which is fatal to any task whose stack lives in
 * PSRAM. dashboard_get_status() is called by rules_task as well as by the
 * httpd handler, so leaving this lazy meant "whichever task arrives first
 * does a flash read" -- a race, not an ordering guarantee, and exactly the
 * thing that would have to be true-by-accident for rules_task's stack to be
 * safe to move off internal DRAM (ROADMAP.md M10). Priming it at startup, on
 * the app_main task, makes that safety a property of the code instead. */
static bool s_flash_facts_read = false;
static bool s_flash_size_known = false;
static uint32_t s_flash_size = 0;
static uint32_t s_flash_partition_size = 0;
static bool s_flash_used_known = false;
static uint32_t s_flash_used = 0;

static void read_flash_facts_once(void)
{
    if (s_flash_facts_read) {
        return;
    }
    /* s_flash_facts_read is set LAST, after every static above is filled in
     * -- Opus review 2026-08-27 caught this set FIRST in an earlier version:
     * a second task entering between that early set and the reads finishing
     * would see it already true and copy still-zero statics, reporting a
     * transient all-unknown flash status for that one response. Worst case
     * now is a handful of redundant reads if two callers really do race in
     * together -- no torn or fabricated values either way. */
    uint32_t flash_size = 0;
    s_flash_size_known = (esp_flash_get_size(NULL, &flash_size) == ESP_OK);
    s_flash_size = flash_size;

    hal_sysinfo_partition_info_t running;
    if (hal_sysinfo_get_running_partition(&running) == HAL_OK) {
        s_flash_partition_size = running.size;
        esp_image_metadata_t metadata = { 0 };
        esp_partition_pos_t part_pos = { .offset = running.address, .size = running.size };
        s_flash_used_known = (esp_image_get_metadata(&part_pos, &metadata) == ESP_OK);
        s_flash_used = metadata.image_len;
    }
    s_flash_facts_read = true;
}

void dashboard_get_status(dashboard_status_t *out)
{
    memset(out, 0, sizeof(*out));

    out->io_ready = s_dash.io != NULL;
    if (out->io_ready) {
        kiln_io_state_t st;
        memset(&st, 0, sizeof(st));
        /* Routed through kiln_io_owner (not a direct kiln_io_read(s_dash.io,
         * ...) -- see this repo's "bypassed owner module" bug class) so a
         * concurrent relay command from profile_executor.c/autotune_engine.c
         * can never race this read against the owner's own in-flight write.
         * Safe against the flash-worker-style self-deadlock: this handler
         * always runs on the httpd worker task or the LVGL task (see every
         * other dashboard_get_status() call site -- ui_page_home.c,
         * ui_page_temperature.c, ui_page_diagnostics.c), never on
         * kiln_io_owner's own owner_task, so post_and_wait() inside
         * kiln_io_owner_command_read() blocks on a queue/semaphore the
         * owner task itself is free to service, not one it is already
         * holding. It also fails closed on a bounded timeout
         * (KILN_IO_OWNER_WAIT_MS, kiln_io_owner.c's post_and_wait()) rather
         * than blocking forever, so a wedged owner cannot hang this
         * handler either -- err below just comes back ESP_ERR_TIMEOUT, same
         * shape as any other kiln_io_owner_command_*() failure this file
         * already handles. */
        esp_err_t err = kiln_io_owner_command_read(&st);
        for (uint8_t relay = 1; relay <= KILN_IO_RELAY_COUNT; relay++) {
            out->relay_on[relay - 1] = (st.relay_shadow & (1u << (relay - 1))) != 0;
        }
        out->io_read_failed = (err != ESP_OK);
    }

    /* Lifetime contact-cycle count per relay (TODO.md 6A.1). Reported even
     * with no expander attached: it is persisted history, not live hardware
     * state, and a relay's accumulated wear is exactly the thing an operator
     * wants to see when deciding whether to replace one. */
    {
        uint32_t cycles[KILN_IO_RELAY_COUNT];
        relay_cycles_get(cycles);
        memcpy(out->relay_cycles, cycles, sizeof(cycles));
    }

    /* RELAY_LIFE_BUDGET_PLAN.md step 4: budget state for all five counted
     * slots, including the safety relay (RELAY_CYCLES_SAFETY_INDEX), which
     * relay_cycles[] above deliberately excludes. */
    for (uint8_t r = 0; r < RELAY_CYCLES_COUNT; r++) {
        relay_cycles_budget(r, &out->relay_life[r]);
        uint32_t rated_override;
        relay_cycles_get_type(r, &out->relay_life_type[r], &rated_override);
    }
    out->relay_life_tier = relay_cycles_max_budget_tier();

    /* thermo_bus->initialized only means the shared SPI bus came up -- it
     * says nothing about whether any MAX31856 actually answered on it (see
     * MAX31856_bus_init). A board-less bus reads back count == 0 from
     * MAX31856_read_all with no error, so "ready" has to mean "at least one
     * channel responded," not "the bus exists." */
    MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
    size_t count = 0;
    if (sim_backend_enabled()) {
        /* A sim build answers here too, so the dashboard shows the same
         * fabricated kiln the executor is controlling rather than
         * "no thermocouple" next to a running firing. */
        sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
    } else if (s_dash.thermo_bus && s_dash.thermo_bus->initialized) {
        MAX31856_read_all(s_dash.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
    }
    out->thermo_ready = count > 0;
    out->thermo_spi_wedged = MAX31856_bus_spi_wedged(s_dash.thermo_bus);

    /* DISPLAY_ST7796_PLAN.md 9.1: no display-object dependency here --
     * lvgl_port_get_flush_stats() reads lvgl_port.c's own file-static
     * counters, written only by lvgl_port_task's flush callback, so this is
     * safe to call whether or not a display ever came up. */
    lvgl_port_get_flush_stats(&out->flush_last_us, &out->flush_max_us, &out->flush_count);
    out->channel_count = count;
    for (size_t i = 0; i < count; i++) {
        const MAX31856Reading *r = &readings[i];
        bool valid = !r->spi_failed && !isnan(r->tc_temperature_c);
        /* NaN has no valid JSON literal -- report 0 alongside valid:false
         * rather than emit invalid JSON or a string that breaks a naive
         * client-side parseFloat(). Calibration applied here (zone i <->
         * channel i, per zones_http.h) -- see zones_config_apply_cal()'s
         * doc comment for the UART-bridge-side scope gap this leaves. */
        float temp = isnan(r->tc_temperature_c) ? 0.0f : zones_config_apply_cal(r->channel, r->tc_temperature_c);
        float cj = isnan(r->cj_temperature_c) ? 0.0f : r->cj_temperature_c;
        out->channels[i].channel = r->channel;
        out->channels[i].temp_c = temp;
        out->channels[i].cj_c = cj;
        out->channels[i].valid = valid;
        out->channels[i].fault_status = r->fault_status;
        out->channels[i].spi_failed = r->spi_failed;
        /* The driver's r->stale answers "was there a new conversion this
         * poll" -- honest at that layer, but useless as a UI signal, since
         * polling faster than the part converts sets it on a value that is a
         * fraction of a second old. What a user needs to know is whether the
         * NUMBER is too old to trust, which is the age against one shared
         * threshold. See KILN_TEMP_STALE_AGE_MS in MAX31856.h. */
        out->channels[i].age_ms = r->age_ms;
        out->channels[i].stale = (r->age_ms >= KILN_TEMP_STALE_AGE_MS);
    }

    /* safety_ready must equal dashboard_safety_ready() -- see that function's
     * doc comment (dashboard_http.h) for the owner-reported bench bug this
     * fixes: s_dash.safety != NULL only means the driver object was
     * constructed, not that the Pico is actually answering, and the old code
     * reported the former. Defaults to false/ESP_FAIL here so a never-
     * initialized or currently-failing link reads not-ready, never a stale
     * "true" left over from init. */
    esp_err_t safety_status_err = ESP_FAIL;
    bool safety_link_up = false;

    /* ROADMAP.md M6: safety/enclosure temperature straight from the safety
     * link's cache -- see dashboard_http.h's field comment for the wire
     * source and why power_w has no data yet. safety_link_get_status()
     * itself is null-tolerant on a never-initialized link (returns an error,
     * leaving out->safety_temp_c/enclosure_temp_c at the memset(0) above,
     * caught by the isnan() check anyway since a plain 0.0f would otherwise
     * read as a real, very cold reading). */
    if (s_dash.safety) {
        safety_link_status_t sl;
        safety_status_err = safety_link_get_status(s_dash.safety, &sl);
        if (safety_status_err == ESP_OK) {
            safety_link_up = sl.link_up;
            out->safety_temp_c = sl.tc_temp_c;
            out->safety_temp_valid = !isnan(sl.tc_temp_c);
            out->enclosure_temp_c = sl.cj_temp_c;
            out->enclosure_temp_valid = !isnan(sl.cj_temp_c);
            out->safety_tc_is_separate_sensor = safety_tc_is_separate_physical_sensor(&sl);

            /* ROADMAP.md M5/M6, TODO.md 10.10: SAFETY_CMD_POWER (Frame E) now
             * has a real decode path in safety_link.c's safety_apply_power().
             * power_ever_received is false, and power_total_w stays NaN, until
             * a Pico that implements Frame E actually sends one -- same "no
             * hardware yet, honestly null" state as before, just a real path
             * instead of a permanent stub (see this file's history / TODO.md
             * 10.10 for the prior state). */
            if (sl.power_ever_received) {
                out->power_w = sl.power_total_w;
                out->power_valid = !isnan(sl.power_total_w);
            }

            /* dashboard_http.h's ct_current_a comment -- straight passthrough,
             * NaN before the first status frame (safety_link.c's init) or
             * after any single channel's own conversion fails. */
            out->ct_current_a[0] = sl.current_a[0];
            out->ct_current_a[1] = sl.current_a[1];
            out->ct_current_a[2] = sl.current_a[2];

            /* dashboard_http.h's ct_counts comment -- straight passthrough
             * from the POWER (Frame E) cache; false/0 until a V2 frame has
             * arrived (never for a pre-11-protocol Pico). */
            out->ct_counts_valid = sl.power_counts_valid;
            out->ct_counts[0] = sl.power_channel_counts_avg[0];
            out->ct_counts[1] = sl.power_channel_counts_avg[1];
            out->ct_counts[2] = sl.power_channel_counts_avg[2];

            /* K4 -- see dashboard_http.h's field comment. safety_relay_known
             * set here (link answered this poll) rather than left to default
             * true -- Opus review 2026-08-27: these two were the only
             * safety-link-sourced fields on this endpoint NOT using the
             * null-until-known convention every other one does, so a caller
             * that skips checking safety_ready would see a false "de-
             * energized" for a link that has simply never answered, not a
             * confirmed-open relay. */
            out->safety_relay_known = true;
            out->safety_relay_energized = (sl.flags & SAFETY_FLAG_RELAY) != 0u;
            out->safety_heating_enabled = (sl.flags & SAFETY_FLAG_ENABLED) != 0u;
            /* Not from the wire -- this is the ESP's OWN block bitmask, read
             * straight from the link object. See the field comment. */
            out->heat_block_sources = safety_link_get_fault_sources(s_dash.safety);

            /* ROADMAP.md M5: DIAG (Frame B) / TRIP_EVENT (Frame D) now have a
             * decode path -- see safety_link.h's field comments for what
             * each of these means. Straight passthrough, same convention as
             * the temperature/power fields above. */
            out->diag_ever_received = sl.diag_ever_received;
            out->diag_trip_reason = sl.diag_trip_reason;
            out->diag_warn_mask = sl.diag_warn_mask;
            out->diag_trip_mask = sl.diag_trip_mask;
            out->diag_state = sl.diag_state;
            out->diag_age_ms = sl.age_ms;
            out->diag_context_age_100ms = sl.diag_context_age_100ms;
            out->diag_context_frames_ok = sl.diag_context_frames_ok;
            out->diag_context_frames_bad = sl.diag_context_frames_bad;
            out->diag_tx_frames_dropped = sl.diag_tx_frames_dropped;

            out->trip_event_ever_received = sl.trip_event_ever_received;
            out->trip_reason = sl.trip_reason;
            out->trip_event_age_ms = sl.trip_event_age_ms;
            out->trip_safety_tc_c = sl.trip_safety_tc_c;
            out->trip_deciding_threshold = sl.trip_deciding_threshold;
            out->trip_current_a[0] = sl.trip_current_a[0];
            out->trip_current_a[1] = sl.trip_current_a[1];
            out->trip_current_a[2] = sl.trip_current_a[2];
            out->trip_context_age_100ms = sl.trip_context_age_100ms;
            out->trip_fault_sources = sl.trip_fault_sources;
            out->trip_fault_sources_valid = sl.trip_fault_sources_valid;
        }
        out->safety_ready = dashboard_safety_ready(true, safety_status_err, safety_link_up);

        /* TODO.md 9.0's deferred "GUI names both versions" item -- this
         * firmware's own protocol number is always known regardless of link
         * state, the peer's is whatever the last FW_VERSION frame said (or
         * unknown, on this no-Pico bench build). */
        out->self_protocol_version = (uint16_t)UART_PROTOCOL_VERSION;
        (void)safety_link_get_peer_version_status(s_dash.safety, &out->link_version_known,
                                                    &out->link_version_compatible,
                                                    &out->peer_protocol_version,
                                                    &out->peer_min_compatible);

        /* TODO.md owner-report item 5: safety processor's own build identity
         * + config CRC, straight from safety_link.c's FW_VERSION parse.
         * Commit/datetime come back as explicit-length, non-NUL-terminated
         * byte buffers (safety_link.h's own convention) -- copy into this
         * struct's NUL-terminated char[] here, once, so every renderer
         * (JSON below, any future LCD page) gets an ordinary C string
         * instead of re-deriving the length itself. */
        uint8_t commit_buf[64];
        uint8_t commit_len = 0;
        uint8_t datetime_buf[32];
        uint8_t datetime_len = 0;
        (void)safety_link_get_peer_build_status(s_dash.safety, &out->safety_build_known,
                                                 &out->safety_build_dirty, commit_buf, &commit_len,
                                                 datetime_buf, &datetime_len,
                                                 &out->safety_config_version,
                                                 &out->safety_config_crc);
        if (commit_len > sizeof(out->safety_build_commit) - 1u) {
            commit_len = sizeof(out->safety_build_commit) - 1u;
        }
        memcpy(out->safety_build_commit, commit_buf, commit_len);
        out->safety_build_commit[commit_len] = '\0';
        if (datetime_len > sizeof(out->safety_build_datetime) - 1u) {
            datetime_len = sizeof(out->safety_build_datetime) - 1u;
        }
        memcpy(out->safety_build_datetime, datetime_buf, datetime_len);
        out->safety_build_datetime[datetime_len] = '\0';
    } else {
        out->self_protocol_version = (uint16_t)UART_PROTOCOL_VERSION;
    }
    if (!s_dash.safety || safety_status_err != ESP_OK) {
        /* memset(out, 0, ...) above would otherwise leave these reading as a
         * plausible 0.00 A instead of "never arrived" -- same reasoning as
         * safety_temp_c/enclosure_temp_c's own NaN fallback just below. */
        out->ct_current_a[0] = NAN;
        out->ct_current_a[1] = NAN;
        out->ct_current_a[2] = NAN;
        out->ct_counts_valid = false;
        out->ct_counts[0] = 0u;
        out->ct_counts[1] = 0u;
        out->ct_counts[2] = 0u;
    }
    if (!out->safety_temp_valid) {
        out->safety_temp_c = NAN;
    }
    if (!out->enclosure_temp_valid) {
        out->enclosure_temp_c = NAN;
    }
    if (!out->power_valid) {
        out->power_w = NAN;
    }

    /* TODO.md 8.2 "Tie it to the guards, not only the UI": surface the same
     * flag profile_executor.c/autotune_engine.c now refuse on, so the
     * dashboard and pc_tools' Zones panel can say "zone config failed to
     * load" explicitly instead of a kiln that just silently won't fire. */
    out->zones_config_valid = zones_config_is_valid();

    /* UI_PLAN.md section 5's missing field set (see dashboard_http.h's
     * struct comment above these fields for the full rationale). Every read
     * here is cheap and side-effect-free -- esp_app_get_description() reads
     * a const struct baked into the app image, esp_reset_reason()/
     * hal_time_now_us() are simple register/RTC reads, and the
     * heap_caps_get_*() calls are the same O(free-list-length) walk
     * ui_page_diagnostics.c's own refresh_cb() already performs on the same
     * 2-second LCD tick, so doing it again here on a browser's poll cadence
     * is not a new cost profile for this firmware. */
    hal_sysinfo_build_info_t build_info;
    hal_sysinfo_get_build_info(&build_info);
    out->fw_version_known = build_info.valid;
    if (build_info.valid) {
        /* hal_sysinfo_build_info_t::version/date/time are themselves
         * fixed-size, NUL-terminated char arrays -- snprintf still used
         * defensively rather than strcpy, matching this file's json_escape()
         * callers' general "never trust a fixed-size field to already be
         * exactly what its type promises" habit. */
        snprintf(out->fw_version, sizeof(out->fw_version), "%s", build_info.version);
        snprintf(out->fw_build, sizeof(out->fw_build), "%s %s", build_info.date, build_info.time);
    } else {
        out->fw_version[0] = '\0';
        out->fw_build[0] = '\0';
    }

    out->uptime_s = (uint32_t)(hal_time_now_us() / 1000000);
    out->reset_reason = reset_reason_name(hal_sysinfo_reset_reason());

    out->heap_internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    out->heap_internal_largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    out->heap_internal_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    out->heap_internal_total = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);
    /* MALLOC_CAP_SPIRAM reads back as a real 0 (not an error) on a board
     * built without PSRAM enabled -- see dashboard_http.h's field comment
     * and ui_page_diagnostics.c's refresh_cb() for the same call and the
     * same "0 KB is honest, not invented" reasoning. Same for _total below. */
    out->heap_spiram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    out->heap_spiram_largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    out->heap_spiram_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    out->heap_spiram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);

    /* DRAM_PSRAM_PLAN.md Phase 0 (4.1): the one capability not previously
     * broken out. A subset of heap_internal above (both draw from
     * MALLOC_CAP_INTERNAL), reported separately because Phase 1 needs to
     * watch it independently -- see dashboard_http.h's field comment. */
    out->heap_dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    out->heap_dma_largest_free_block = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    out->heap_dma_min_free = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
    out->heap_dma_total = heap_caps_get_total_size(MALLOC_CAP_DMA);

    /* Owner request 2026-08-27: flash usage on the dashboard, same facts
     * ui_page_diagnostics.c's Firmware page already shows on the LCD
     * (build_firmware_statics()) -- total chip size from esp_flash_get_size,
     * and how much of the running app image's own OTA slot is actually used
     * (not the slot's fixed capacity) from esp_image_get_metadata(), the
     * same accessor esp_ota_* uses internally to validate an image. All
     * fixed for the life of this boot (same reasoning ui_page_diagnostics.c's
     * own build_firmware_statics() doc comment gives for reading these once,
     * not on every refresh) -- computed once here rather than on every
     * dashboard_get_status() call (Opus review 2026-08-27: this was walking
     * the image's segment headers via real flash reads from inside the httpd
     * handler on every single /api/status poll). Left at their zero-init
     * default (caught by flash_*_known below) if the one-time read ever
     * fails -- chip-size/partition/image-header reads are all "should never
     * fail on real hardware" but none is worth a fabricated number if one
     * ever does. */
    read_flash_facts_once();
    out->flash_size_known = s_flash_size_known;
    out->flash_size = s_flash_size;
    out->flash_partition_size = s_flash_partition_size;
    out->flash_used_known = s_flash_used_known;
    out->flash_used = s_flash_used;

    /* 2026-08-21: the shared display-unit preference (unit_pref.c) -- see
     * dashboard_http.h's field comment. unit_pref_get() is O(1) RAM-only, so
     * this costs nothing extra on either the HTTP or LCD poll path. */
    out->temp_unit = unit_pref_get();
    out->ramp_assist_enabled = ramp_assist_cfg_enabled();
    /* Local state, always knowable -- deliberately not inside the "did the
     * safety link answer" block above. See the field comment. */
    out->zone_blocked_mask = relay_authority_latched_blocked_mask();

    /* 2026-08-30, PROFILES.md "Scheduled start + candling": time_sync.c's
     * wall-clock status. See dashboard_http.h's field comments for the
     * 0-means-never-synced convention. */
    time_sync_status_t ts;
    time_sync_get_status(&ts);
    out->time_synced = ts.ever_synced;
    out->time_now_epoch = ts.now_epoch;
    out->time_last_sync_epoch = ts.last_sync_epoch;
    strncpy(out->time_tz, ts.tz, sizeof(out->time_tz) - 1);
    out->time_tz[sizeof(out->time_tz) - 1] = '\0';
}


/* See dashboard_http.h -- mirrors status_get_handler()'s io_ready/
 * thermo_ready/safety_ready computation exactly (same order, same
 * sim_backend_enabled() branch) so readiness_http.c can never see a
 * different answer than /api/status does for the same three flags. */
void dashboard_http_get_hw_ready(bool *out_io_ready, bool *out_thermo_ready, bool *out_safety_ready)
{
    if (out_io_ready) {
        *out_io_ready = s_dash.io != NULL;
    }
    if (out_safety_ready) {
        /* Same dashboard_safety_ready() contract as dashboard_get_status()
         * above -- see that function's call site, or dashboard_http.h's doc
         * comment, for the owner-reported false-positive this fixes. */
        esp_err_t safety_status_err = ESP_FAIL;
        bool safety_link_up = false;
        if (s_dash.safety) {
            safety_link_status_t sl;
            safety_status_err = safety_link_get_status(s_dash.safety, &sl);
            if (safety_status_err == ESP_OK) {
                safety_link_up = sl.link_up;
            }
        }
        *out_safety_ready = dashboard_safety_ready(s_dash.safety != NULL, safety_status_err, safety_link_up);
    }
    if (out_thermo_ready) {
        MAX31856Reading readings[MAX31856_CHANNEL_COUNT];
        size_t count = 0;
        if (sim_backend_enabled()) {
            sim_backend_read_all(readings, MAX31856_CHANNEL_COUNT, &count);
        } else if (s_dash.thermo_bus && s_dash.thermo_bus->initialized) {
            MAX31856_read_all(s_dash.thermo_bus, readings, MAX31856_CHANNEL_COUNT, &count);
        }
        *out_thermo_ready = count > 0;
    }
}

/* See dashboard_http.h's doc comment -- the plain-C action function
 * extracted from relay_post_handler() so a non-HTTP caller (ui_page_temperature.c)
 * goes through the same ownership/safety gate and the same kiln_io write,
 * not a reimplementation of either. relay_post_handler() below is now a thin
 * wrapper: parse the HTTP body, call this, translate the result to an HTTP
 * status.
 *
 * 2026-08-19 (TODO.md 10.14 Phase 1): the ownership/safety-fault check and
 * the actual write both moved into kiln_io_owner.c -- this used to
 * reimplement relay_authority_manual_blocked_by_owner()/
 * relay_authority_on_blocked() independently of uart_bridge.c's identical
 * copy, which is exactly the "two copies that can drift" problem that pass
 * closed. This function is now a thin translation from
 * kiln_io_owner_relay_result_t to dashboard_relay_result_t. Also closes a
 * real race: this used to call kiln_io_set_relay() directly, with no
 * coordination against uart_bridge.c's io_bridge_task or
 * profile_executor.c's control loop doing the same. */
dashboard_relay_result_t dashboard_set_relay(uint8_t relay_index, bool on, uint32_t *out_safety_sources)
{
    if (!s_dash.io) {
        return DASHBOARD_RELAY_ERR_NO_BOARD;
    }

    kiln_io_owner_relay_result_t rr = kiln_io_owner_command_set_relay(relay_index, on, out_safety_sources);
    switch (rr) {
    case KILN_IO_OWNER_RELAY_OK:
        return DASHBOARD_RELAY_OK;
    case KILN_IO_OWNER_RELAY_ERR_RANGE:
        return DASHBOARD_RELAY_ERR_RANGE;
    case KILN_IO_OWNER_RELAY_ERR_OWNED:
        ESP_LOGW(DASH_TAG, "dashboard: relay %u refused -- owned by a running profile", (unsigned)relay_index);
        return DASHBOARD_RELAY_ERR_OWNED;
    case KILN_IO_OWNER_RELAY_ERR_SAFETY:
        ESP_LOGW(DASH_TAG, "dashboard: relay %u ON refused -- safety fault sources 0x%02X", (unsigned)relay_index,
                 out_safety_sources ? (unsigned)*out_safety_sources : 0u);
        return DASHBOARD_RELAY_ERR_SAFETY;
    case KILN_IO_OWNER_RELAY_ERR_UPDATING:
        /* 2026-08-21: distinct from ERR_SAFETY above -- see
         * dashboard_http.h's DASHBOARD_RELAY_ERR_UPDATING comment and
         * kiln_io_owner.c's relay_on_blocked() for why this is not a fault.
         * Re-derive the exact reason string relay_on_blocked() already
         * logged once (kiln_io_owner.c does not hand it back through
         * kiln_io_owner_relay_result_t, only the enum value) rather than
         * inventing a second, possibly-drifting message here. */
        {
            char reason[HEAT_INTERLOCK_REASON_MAX];
            if (ota_http_heat_blocked_by_update(reason, sizeof(reason))) {
                ESP_LOGW(DASH_TAG, "dashboard: relay %u ON refused -- %s", (unsigned)relay_index, reason);
            } else {
                /* Should not happen -- kiln_io_owner.c only returns this
                 * result when that same check just returned true -- but the
                 * update could in principle finish between that check and
                 * this one, so fall back to a still-accurate generic reason
                 * rather than printing an empty/stale string. */
                ESP_LOGW(DASH_TAG, "dashboard: relay %u ON refused -- firmware update in progress",
                         (unsigned)relay_index);
            }
        }
        return DASHBOARD_RELAY_ERR_UPDATING;
    case KILN_IO_OWNER_RELAY_ERR_IO_FAIL:
    case KILN_IO_OWNER_RELAY_ERR_TIMEOUT:
    default:
        ESP_LOGW(DASH_TAG, "dashboard: kiln_io_owner_command_set_relay(%u) failed (result %d)",
                 (unsigned)relay_index, (int)rr);
        return DASHBOARD_RELAY_ERR_IO_FAIL;
    }
}

/* relay_safety_reason() and relay_post_handler() (POST /api/relay) removed
 * 2026-08-27: the endpoint's only caller was manual_page.html
 * (/settings/manual), removed the same day at the owner's request -- "the
 * danger zone in the diagnostics page covers it fine" -- now that the
 * kiln's heating elements are actually wired to this board. Confirmed by
 * grep across the whole repo (firmware/, tools/, docs/, mykicadMcp/) that
 * nothing else -- not the LCD, not PcTools, not either MCP server -- ever
 * called POST /api/relay; diagnostics_http.c's Danger Zone posts to the
 * separate /api/diagnostics/danger/relay instead. dashboard_set_relay()
 * above (the actual ownership/safety gate + kiln_io write) is UNTOUCHED and
 * stays exactly as shared as before: diagnostics_http.c's
 * danger_relay_post_handler() and ui_page_temperature.c's LCD relay control
 * both still call it directly, so removing the one dead HTTP door does not
 * touch the gate itself or either surviving caller. */

esp_err_t dashboard_http_start(kiln_io_t *io_or_null, MAX31856BusClass *thermo_bus_or_null,
                               SafetyLinkClass *safety_or_null)
{
    s_dash.io = io_or_null;
    s_dash.thermo_bus = thermo_bus_or_null;
    s_dash.safety = safety_or_null;

    httpd_handle_t server = wifi_provision_http_get_server();
    if (!server) {
        ESP_LOGE(DASH_TAG, "no HTTP server -- wifi_provision_http_start() must run first");
        return ESP_ERR_INVALID_STATE;
    }

    static const httpd_uri_t status_uri = {
        .uri = "/api/status", .method = HTTP_GET, .handler = dashboard_status_get_handler,
    };
    static const httpd_uri_t exec_status_uri = {
        .uri = "/api/profile_exec", .method = HTTP_GET, .handler = profile_exec_status_get_handler,
    };
    static const httpd_uri_t profile_plan_uri = {
        .uri = "/api/profile_plan", .method = HTTP_GET, .handler = profile_plan_get_handler,
    };
    static const httpd_uri_t exec_start_uri = {
        .uri = "/api/profile_exec/start", .method = HTTP_POST, .handler = profile_exec_start_post_handler,
    };
    static const httpd_uri_t exec_stop_uri = {
        .uri = "/api/profile_exec/stop", .method = HTTP_POST, .handler = profile_exec_stop_post_handler,
    };
    static const httpd_uri_t exec_pause_uri = {
        .uri = "/api/profile_exec/pause", .method = HTTP_POST, .handler = profile_exec_pause_post_handler,
    };
    static const httpd_uri_t exec_resume_uri = {
        .uri = "/api/profile_exec/resume", .method = HTTP_POST, .handler = profile_exec_resume_post_handler,
    };
    static const httpd_uri_t exec_ack_last_run_uri = {
        .uri = "/api/profile_exec/ack_last_run", .method = HTTP_POST,
        .handler = profile_exec_ack_last_run_post_handler,
    };
    static const httpd_uri_t control_status_uri = {
        .uri = "/api/control", .method = HTTP_GET, .handler = control_status_get_handler,
    };
    static const httpd_uri_t firing_history_uri = {
        .uri = "/api/firing_history", .method = HTTP_GET, .handler = firing_history_get_handler,
    };
    static const httpd_uri_t safety_clear_trip_uri = {
        .uri = "/api/safety/clear_trip", .method = HTTP_POST, .handler = safety_clear_trip_post_handler,
    };
    static const httpd_uri_t safety_log_level_uri = {
        .uri = "/api/safety/log_level", .method = HTTP_POST, .handler = safety_log_level_post_handler,
    };
    static const httpd_uri_t history_csv_uri = {
        .uri = "/api/history.csv", .method = HTTP_GET, .handler = history_csv_get_handler,
    };
    static const httpd_uri_t autotune_status_uri = {
        .uri = "/api/autotune", .method = HTTP_GET, .handler = autotune_status_get_handler,
    };
    static const httpd_uri_t autotune_matrix_uri = {
        .uri = "/api/autotune/matrix", .method = HTTP_GET, .handler = autotune_matrix_get_handler,
    };
    static const httpd_uri_t autotune_start_uri = {
        .uri = "/api/autotune/start", .method = HTTP_POST, .handler = autotune_start_post_handler,
    };
    static const httpd_uri_t autotune_abort_uri = {
        .uri = "/api/autotune/abort", .method = HTTP_POST, .handler = autotune_abort_post_handler,
    };
    static const httpd_uri_t autotune_accept_uri = {
        .uri = "/api/autotune/accept", .method = HTTP_POST, .handler = autotune_accept_post_handler,
    };
    static const httpd_uri_t autotune_trace_uri = {
        .uri = "/api/autotune/trace.csv", .method = HTTP_GET, .handler = autotune_trace_csv_get_handler,
    };
    static const httpd_uri_t unit_pref_uri = {
        .uri = "/api/unit_pref", .method = HTTP_POST, .handler = unit_pref_post_handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/status) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/profile_exec) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &profile_plan_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/profile_plan) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_start_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/profile_exec/start) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_stop_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/profile_exec/stop) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_pause_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/profile_exec/pause) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_resume_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/profile_exec/resume) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &exec_ack_last_run_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/profile_exec/ack_last_run) failed: %s",
                 esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &safety_clear_trip_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/safety/clear_trip) failed: %s", esp_err_to_name(err));
    }
    err = httpd_register_uri_handler(server, &safety_log_level_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/safety/log_level) failed: %s", esp_err_to_name(err));
    }
    err = httpd_register_uri_handler(server, &control_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/control) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &firing_history_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/firing_history) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &history_csv_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/history.csv) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_status_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/autotune) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_matrix_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/autotune/matrix) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_start_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/autotune/start) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_abort_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/autotune/abort) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_accept_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/autotune/accept) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &autotune_trace_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/autotune/trace.csv) failed: %s", esp_err_to_name(err));
        return err;
    }
    err = httpd_register_uri_handler(server, &unit_pref_uri);
    if (err != ESP_OK) {
        ESP_LOGE(DASH_TAG, "httpd_register_uri_handler(/api/unit_pref) failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Prime the flash facts here, on the app_main task, so no later caller
     * -- least of all rules_task, whose stack is a candidate to move to
     * PSRAM -- can be the one that performs a cache-disabling flash read.
     * See read_flash_facts_once()'s own comment. */
    read_flash_facts_once();

    ESP_LOGI(DASH_TAG, "dashboard API up (io_ready=%d, thermo_ready=%d, safety_ready=%d)", s_dash.io != NULL,
             s_dash.thermo_bus != NULL && s_dash.thermo_bus->initialized, s_dash.safety != NULL);
    return ESP_OK;
}
