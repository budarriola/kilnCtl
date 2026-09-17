/* route_tier_table.h -- the authoritative OPEN/USER/ADMIN classification of
 * every HTTP route this firmware registers.
 *
 * See docs/WEB_AUTH_PLAN.md section 1 for the design and the rule that
 * decides ambiguous cases: a route is OPEN only if its response cannot be
 * used to change the kiln's behaviour and reveals nothing an onlooker at the
 * kiln cannot already see. Anything that writes is at least USER.
 *
 * This file is data, not wiring: it does not itself gate any request. It is
 * the single source of truth that two things are built against --
 *   - tools/check_route_tier_coverage.ps1 (mechanical, fail-closed: any
 *     `.uri = "..."` registered anywhere under App/drivers with no matching
 *     row here fails the build);
 *   - the enforcement pre-handler (docs/WEB_AUTH_PLAN.md item 5,
 *     kiln_http_register()), which is expected to consult this same table
 *     rather than maintain a second one -- a second, independently
 *     maintained tier table would be exactly the "reset one side of a pair"
 *     bug class CLAUDE.md warns about: two copies of the same fact that can
 *     silently drift apart.
 *
 * KEEP THIS FILE IN SYNC WITH THE REAL ROUTE TABLE. Adding a new
 * httpd_uri_t anywhere under firmware/KilnFW/App/drivers (any .c file) requires an
 * ROUTE_TIER() row here in the SAME change -- the mechanical check exists
 * specifically so a route added without one is caught at check time rather
 * than shipping open by default. A route present here that no longer exists
 * in the tree is harmless (the check only complains about the reverse
 * direction: a real route with no row) but should still be deleted when
 * noticed, so this file does not accumulate stale entries.
 *
 * Route count, and why it is 138 and not the plan's stated 140: the plan's
 * "140 registered routes" figure counts three `.uri = "..."` occurrences in
 * firmware/KilnFW/App/drivers/http/wifi_provision_http.c's own comment block
 * (the recount history above its route table, e.g. "`grep -n '.uri = ""'`")
 * -- those are prose showing the pattern, not real registrations. Comment-
 * stripped, the same way check_uri_handler_cap.ps1 and this table's own
 * coverage check strip comments, the real count as of this writing is 137
 * registered (uri, method) pairs, plus ONE the plan's route tables never
 * named at all: POST /api/dualwrite_window/restore_verified
 * (dualwrite_window_http.c) -- present in code, absent from every table in
 * docs/WEB_AUTH_PLAN.md section 1. It is classified ADMIN below: it mutates
 * persisted dual-write/config-migration state, the same class as its
 * sibling GET /api/dualwrite_window (ADMIN) and the other cfgfs/OTA-adjacent
 * routes in that section.
 *
 * One more correction found while building this table: the plan's OPEN list
 * (section 1) names "GET /api/unit_pref" as a route the dashboard fetches.
 * No such route exists -- dashboard_http.c registers only
 * `POST /api/unit_pref` (unit_pref_post_handler); the temperature-unit
 * value the dashboard actually reads arrives inside GET /api/status's JSON
 * body (dashboard_status_http.c's "temp_unit" field), which is already
 * OPEN. There is nothing to classify for the nonexistent GET variant; the
 * real POST route is classified ADMIN below per the plan's own ambiguous-
 * routes resolution ("OPEN for GET, ADMIN for POST").
 */
#ifndef ROUTE_TIER_TABLE_H
#define ROUTE_TIER_TABLE_H

#include "esp_http_server.h"

typedef enum {
    ROUTE_TIER_OPEN = 0,   /* no credential, ever */
    ROUTE_TIER_USER,       /* `user` or `administrator` */
    ROUTE_TIER_ADMIN,      /* `administrator` only */
} route_tier_t;

typedef struct {
    const char *uri;
    httpd_method_t method;
    route_tier_t tier;
} route_tier_entry_t;

#define ROUTE_TIER(uri_lit, http_method, route_tier) \
    { (uri_lit), (http_method), (route_tier) }

/* One row per httpd_uri_t registered anywhere under App/drivers, keyed by
 * (uri, method) -- the same key the plan's session/enforcement layer uses,
 * since a single uri can carry different tiers per method (e.g.
 * /api/unit_pref, /api/settings/display_power, /api/zones). Grouped by
 * plan section for review, not by file. */
static const route_tier_entry_t kRouteTierTable[] = {

    /* ---- OPEN -- the always-viewable dashboard (plan section 1, "OPEN") --
     * page shells and static assets needed to load the dashboard before
     * anyone can log in, plus the reads app.js actually issues to render
     * it. */
    ROUTE_TIER("/", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/app.js", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/nav.js", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/theme.css", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/commissioning_shared.js", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/api/status", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/api/profile_exec", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/api/readiness", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/api/history.csv", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/api/profile_plan", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/api/board_temps", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/api/firing_history", HTTP_GET, ROUTE_TIER_OPEN),
    /* NOTE: the plan also lists "GET /api/unit_pref" here -- that route
     * does not exist in the tree (see file header comment). No row for it. */
    ROUTE_TIER("/status", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/scan", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/networks", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/wifi", HTTP_GET, ROUTE_TIER_OPEN),

    /* ---- USER -- start and stop a firing, and nothing else (plan section
     * 1, "USER"). NOTE: /api/profile_exec/stop is also listed here at its
     * plan-assigned tier for table completeness, but per plan section 9 it
     * bypasses authentication unconditionally at the enforcement point --
     * that bypass is item 5's job, not this table's. This table only ever
     * records the tier a route is *nominally* USER/ADMIN under; it does not
     * encode the safety-bypass exception. */
    ROUTE_TIER("/api/profile_exec/start", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile_exec/stop", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile_exec/pause", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile_exec/resume", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile_exec/ack_last_run", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile", HTTP_GET, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profiles", HTTP_GET, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profiles/builtin", HTTP_GET, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile/export", HTTP_GET, ROUTE_TIER_USER),
    ROUTE_TIER("/api/kiln_configs", HTTP_GET, ROUTE_TIER_USER),

    /* ---- ADMIN -- everything else (plan section 1, "ADMIN"). ---- */

    /* Config and zones */
    ROUTE_TIER("/api/zones", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones/pid", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones/current_sweep/start", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones/current_sweep/abort", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones/current_sweep/status", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/settings/tz", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/settings/display_power", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/settings/display_power", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/unit_pref", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/watchdog_cfg", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/watchdog_cfg", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ramp_assist", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ramp_assist", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/sim", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/sim", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/setup/progress", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/setup/progress", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones/ct_channel_map", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones_diag", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/control", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/export", HTTP_GET, ROUTE_TIER_ADMIN),

    /* Profiles as data (not execution) */
    ROUTE_TIER("/api/profile", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/profile/delete", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/profile/import", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/profile/builtin/hide", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/profile/builtin/restore", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/apply", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/clone", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/delete", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/import", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/rename", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/save", HTTP_POST, ROUTE_TIER_ADMIN),

    /* Safety and calibration */
    ROUTE_TIER("/api/safety/clear_trip", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning/bench_preset", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning/ct_auto_zero", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning/ct_cal", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning/relay_type", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/log_level", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/rate_guard/auto", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/rate_guard/auto", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/estop/verify", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/relay_cycles/reset", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/relay_cycles/restore", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/thermo/faults", HTTP_GET, ROUTE_TIER_ADMIN),

    /* Tuning */
    ROUTE_TIER("/api/autotune/start", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/autotune/accept", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/autotune/abort", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/adaptive_tune/enable", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/adaptive_tune/revert", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/autotune", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/autotune/matrix", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/autotune/trace.csv", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/adaptive_tune", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/tuning_recommendations", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/logs/autotune", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/logs/firing", HTTP_GET, ROUTE_TIER_ADMIN),

    /* Danger zone */
    ROUTE_TIER("/api/diagnostics/danger/enable", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/diagnostics/danger/relay", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/diagnostics/danger/start", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/diagnostics/danger/stop", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/diagnostics/danger", HTTP_GET, ROUTE_TIER_ADMIN),

    /* OTA, reset, filesystem -- includes the nine routes that authenticate
     * today via ota_auth/AP-password (plan item 2b): they become ordinary
     * ADMIN routes here. The runtime fallback described in item 2b/11 (auth
     * off -> these nine still require the AP-password challenge, never
     * open) is the enforcement layer's job, not this table's -- this table
     * only records the nominal tier. */
    ROUTE_TIER("/api/ota/esp", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ota/esp/rollback", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ota/esp/recovery_exit", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ota/esp/boot_guard_reset", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ota/pico", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ota/pico/rollback", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/factory_reset", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/cfgfs/format_confirm", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/sw_reset", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/cfgfs/file", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/cfgfs/file", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/backup/import", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/crash_report/ack", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/crash_report/clear", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ota/interlock", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ota/pico/status", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/ota/pico/rollback/status", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/cfgfs", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/cfgfs/format_pending", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/dualwrite_window", HTTP_GET, ROUTE_TIER_ADMIN),
    /* Not in any plan table -- found while cross-checking the plan's route
     * enumeration against the real tree (dualwrite_window_http.c); it
     * mutates the same persisted dual-write/migration state its sibling GET
     * route above reads, so it is classified ADMIN on the same basis. */
    ROUTE_TIER("/api/dualwrite_window/restore_verified", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/partitions", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/backup/export", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/crash_report", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/boot_guard", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/coredump/info", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/coredump/chunk", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/debug/lwip_stats", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/diagnostics/timing", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/saftyfw_stack_margin", HTTP_GET, ROUTE_TIER_ADMIN),
    /* GET /api/ota/challenge stays OPEN per the plan -- it issues a nonce
     * plus the administrator record's salt/iteration count, neither usable
     * without the administrator password itself (plan section 1, the
     * paragraph right after the ADMIN list; item 2b). This is the one
     * OTA-family route that is NOT one of the nine ADMIN routes above. */
    ROUTE_TIER("/api/ota/challenge", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/api/ota/esp/status", HTTP_GET, ROUTE_TIER_OPEN),

    /* Network writes */
    ROUTE_TIER("/provision", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/forget", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/ip_config", HTTP_POST, ROUTE_TIER_ADMIN),

    /* Page shells other than / */
    ROUTE_TIER("/status", HTTP_GET, ROUTE_TIER_OPEN), /* dup key note: this
        is the SAME (uri, method) as the /status row already in OPEN above
        (the Wi-Fi status page has both a page-shell and status role) --
        kept as a single entry, not duplicated, since a table keyed on
        (uri, method) can only hold one tier per key by construction. */
    ROUTE_TIER("/settings", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/settings/backup", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/settings/display", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/settings/zones", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/settings/safety", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/safety", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/safety/commissioning", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/profiles", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/diagnostics", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/readiness", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/setup", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/ota", HTTP_GET, ROUTE_TIER_ADMIN),
};

#define ROUTE_TIER_TABLE_COUNT \
    (sizeof(kRouteTierTable) / sizeof(kRouteTierTable[0]))

#endif /* ROUTE_TIER_TABLE_H */
