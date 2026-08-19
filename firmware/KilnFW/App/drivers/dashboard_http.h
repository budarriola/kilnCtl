// dashboard_http -- the Dashboard page's data API: live thermocouple/relay
// status and manual relay control, served over the same HTTP server
// wifi_provision_http.c already owns (see wifi_provision_http_get_server()).
//
// Reads the *same* driver instances the UART bridge reads (kiln_io_t,
// MAX31856BusClass) rather than duplicating ownership of them -- this is a
// second reader/caller alongside the UART bridge, not a second owner.
// Manual relay-ON commands go through relay_authority_on_blocked()
// (App/drivers/relay_authority.c), the same chokepoint the UART bridge uses,
// so a safety fault refuses this path exactly like it refuses the PC link.
//
// Scope (TODO.md section 2): live status/manual control, plus
// start/stop/pause/status for a running profile (GET/POST /api/profile_exec*),
// which reads and drives App/drivers/profile_executor.c -- this module owns
// no execution state of its own, same "one reader, one owner" split as
// zones_http.c/profiles_http.c. No temperature graph yet (needs the history
// buffer, TODO.md section 0 -- designed but has no writer yet); that one is
// still deliberately absent rather than stubbed.
#ifndef DASHBOARD_HTTP_H
#define DASHBOARD_HTTP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "kiln_io.h"
#include "MAX31856.h"
#include "safety_link.h"

#ifdef __cplusplus
extern "C" {
#endif

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
    bool    stale;
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

    bool     thermo_ready;
    size_t   channel_count;   /* <= MAX31856_CHANNEL_COUNT, only this many of
                                * channels[] are populated */
    dashboard_channel_status_t channels[MAX31856_CHANNEL_COUNT];

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
     * power_w has NO real source yet: LINK_PROTOCOL.md sec 6 Frame E
     * (SAFETY_CMD_POWER, 0x0E) is the wire's power estimate, but neither
     * kilnlink_status.c nor safety_link.c parses it today (safety_link.c's
     * safety_drain_inbox() switch only handles GET_STATUS/FW_VERSION/
     * UPDATE_STATUS) -- see LINK_PROTOCOL.md sec 6/M5's still-open "Pico ->
     * ESP telemetry: ... power" bullet. power_valid is therefore always
     * false today; the field exists so the JSON/LCD shapes are already
     * correct the day Frame E gets wired up, rather than needing a second
     * pass through this struct, the HTTP handler, and the LCD page. */
    bool     safety_temp_valid;
    float    safety_temp_c;
    bool     enclosure_temp_valid;
    float    enclosure_temp_c;
    bool     power_valid;
    float    power_w;
} dashboard_status_t;

void dashboard_get_status(dashboard_status_t *out);

/* Outcome of dashboard_set_relay() below -- one variant per distinct refusal
 * reason relay_post_handler's HTTP status codes already distinguished
 * (400 for a missing board or an out-of-range relay, 403 for the two
 * relay_authority.h refusals, 500 for a kiln_io write failure), kept as an
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
} dashboard_relay_result_t;

/* TODO.md 10.1a's shared-backend seam, extracted from relay_post_handler()
 * (POST /api/relay) the same way dashboard_get_status() was pulled out of
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
