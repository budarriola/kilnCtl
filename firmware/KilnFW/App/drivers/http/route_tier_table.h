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
    /* Reachable with NO credential and regardless of lockout/session state,
     * same as OPEN, but distinct from OPEN because it is not "public read"
     * -- it is "this route only ever REDUCES heat/risk, so authentication
     * must never be able to make it harder to reach than an unauthenticated
     * board" (plan section 9). Kept as its own tier, not folded into OPEN,
     * so route_tier_table.h stays the single legible record of *why* a
     * route needs no session: OPEN means "safe to reveal/costs nothing",
     * this means "safe (indeed necessary) to always allow because it can
     * only make the kiln safer". http_auth_check() and
     * kiln_http_prehandler() both key off THIS enum value rather than a
     * URI string match, so there remains exactly one place a route's
     * always-reachable status is decided. */
    ROUTE_TIER_SAFETY_REDUCE,
    /* Exactly one route belongs to this tier: POST /api/auth/bootstrap_password
     * (WEB_AUTH_PLAN.md items 10/11's administrator-bootstrap state,
     * web_auth_admin_bootstrap_needed() in net/web_auth_session.h). Distinct
     * from ROUTE_TIER_ADMIN on purpose: this route must be reachable with NO
     * session at all (none can exist pre-bootstrap) but ONLY while bootstrap
     * is needed -- the inverse gating shape of every other tier here, which
     * all gate on role, never on this kind of one-shot system state. Kept as
     * its own tier rather than folded into ROUTE_TIER_ADMIN or ROUTE_TIER_OPEN
     * so http_auth_check() (the one place that interprets it) has a single
     * named case to key off, not a URI string match. */
    ROUTE_TIER_ADMIN_BOOTSTRAP,
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
    /* WEB_AUTH_PLAN.md section 6: the login page and the credential-check
     * route it submits to. Both OPEN for the same reason
     * GET /api/ota/challenge is OPEN -- a caller with no session yet must
     * still be able to reach the login form and submit credentials; the
     * real gate is web_auth_store_verify_password() inside the handler
     * itself, not this classification. */
    ROUTE_TIER("/login", HTTP_GET, ROUTE_TIER_OPEN),
    ROUTE_TIER("/api/auth/login", HTTP_POST, ROUTE_TIER_OPEN),
    /* WEB_AUTH_PLAN.md section 8: the web-GUI inactivity lock's status poll.
     * OPEN, not USER -- deliberately excluded from
     * http_auth_decision_counts_as_activity()'s activity set (see that
     * function's own comment): a poll that extends the very session it
     * reports on would defeat the lock. POST .../extend, the explicit
     * "stay unlocked" action, is the real USER-tier row below, grouped with
     * the rest of that tier so its activity-touch behaviour is the ordinary
     * one every other USER route already gets. */
    ROUTE_TIER("/api/auth/session", HTTP_GET, ROUTE_TIER_OPEN),
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
     * 1, "USER"). NOTE: /api/profile_exec/stop is classified
     * ROUTE_TIER_SAFETY_REDUCE, not ROUTE_TIER_USER -- plan section 9
     * requires it be reachable with no session and regardless of lockout
     * state (a locked-out owner watching a kiln climb must still be able to
     * stop it). This is decided HERE, in the one table both the mechanical
     * coverage check and the enforcement pre-handler consult, rather than
     * as a URI string match inside the enforcement function -- see
     * ROUTE_TIER_SAFETY_REDUCE's own doc comment above. */
    /* WEB_AUTH_PLAN.md section 8: the explicit "stay unlocked" action. USER
     * tier so the shared pre-handler's own activity-touch (see
     * http_auth_decision_counts_as_activity()) extends the session on this
     * route exactly the same way it does for every other ordinary
     * authenticated request -- no special-case touch code lives in the
     * handler itself. */
    ROUTE_TIER("/api/auth/session/extend", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile_exec/start", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile_exec/stop", HTTP_POST, ROUTE_TIER_SAFETY_REDUCE),
    ROUTE_TIER("/api/profile_exec/pause", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile_exec/resume", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile_exec/ack_last_run", HTTP_POST, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile", HTTP_GET, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profiles", HTTP_GET, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profiles/builtin", HTTP_GET, ROUTE_TIER_USER),
    /* Reads which profiles the operator marked favorite. Same tier as the
     * profile listings it annotates -- it reports marks on those same
     * profiles and nothing else. */
    ROUTE_TIER("/api/profiles/favorites", HTTP_GET, ROUTE_TIER_USER),
    ROUTE_TIER("/api/profile/export", HTTP_GET, ROUTE_TIER_USER),
    ROUTE_TIER("/api/kiln_configs", HTTP_GET, ROUTE_TIER_USER),

    /* ---- ADMIN_BOOTSTRAP -- the one-route exception (plan items 10/11):
     * reachable with no session, gated instead on
     * web_auth_admin_bootstrap_needed() at the enforcement point. ---- */
    ROUTE_TIER("/api/auth/bootstrap_password", HTTP_POST, ROUTE_TIER_ADMIN_BOOTSTRAP),

    /* ---- ADMIN -- everything else (plan section 1, "ADMIN"). ---- */

    /* Config and zones */
    ROUTE_TIER("/api/zones", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones/pid", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/zones/current_sweep/start", HTTP_POST, ROUTE_TIER_ADMIN),
    /* SAFETY_REDUCE, not ADMIN: aborts the current-sweep task
     * (zones_current_sweep_abort(), zones_current_sweep_task.c), which drives
     * relays to measure per-zone current -- an expired session must not be
     * able to keep that running. Same principle as /api/profile_exec/stop
     * above. */
    ROUTE_TIER("/api/zones/current_sweep/abort", HTTP_POST, ROUTE_TIER_SAFETY_REDUCE),
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
    /* Writes a persisted favorite mark. A write, so at least USER; ADMIN to
     * match its sibling persisted-preference writes above (builtin hide/
     * restore), which likewise only change what the UI shows. */
    ROUTE_TIER("/api/profile/favorite", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/apply", HTTP_POST, ROUTE_TIER_ADMIN),
    /* Read-only progress/outcome of the apply the ADMIN route above starts
     * (the apply is asynchronous since item 5's worker landed, so its result
     * needs a route of its own). USER, matching GET /api/kiln_configs
     * itself: it reports a state enum, the target id and the board's own
     * refusal text, discloses no configuration content, and cannot start,
     * alter or cancel a swap. */
    ROUTE_TIER("/api/kiln_configs/apply_status", HTTP_GET, ROUTE_TIER_USER),
    ROUTE_TIER("/api/kiln_configs/clone", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/delete", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/import", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/rename", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/kiln_configs/save", HTTP_POST, ROUTE_TIER_ADMIN),
    /* The one way out of a quarantined store (kiln_cfg_store.c's
     * set_quarantine()) -- discards whatever could not be read and starts a
     * fresh, empty store. Same tier as the other mutating kiln_configs
     * routes above. */
    ROUTE_TIER("/api/kiln_configs/quarantine_clear", HTTP_POST, ROUTE_TIER_ADMIN),

    /* Safety and calibration */
    ROUTE_TIER("/api/safety/clear_trip", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning/bench_preset", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning/ct_auto_zero", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning/ct_cal", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/safety/commissioning/ct_trim", HTTP_POST, ROUTE_TIER_ADMIN),
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
    /* SAFETY_REDUCE, not ADMIN: aborts a running autotune
     * (autotune_engine_abort() -> abort_locked() -> force_relays_off(),
     * autotune_engine_guard.c), which drives relays for the relay-step test.
     * Same principle as /api/profile_exec/stop above. */
    ROUTE_TIER("/api/autotune/abort", HTTP_POST, ROUTE_TIER_SAFETY_REDUCE),
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
    /* SAFETY_REDUCE, not ADMIN: exits the danger-mode relay window
     * (danger_mode_stop() -> kiln_io_owner_command_all_relays_off() plus
     * releasing heat-enable, danger_mode.c), which is a window explicitly
     * armed to drive relays outside the normal safety-gated path. Same
     * principle as /api/profile_exec/stop above. */
    ROUTE_TIER("/api/diagnostics/danger/stop", HTTP_POST, ROUTE_TIER_SAFETY_REDUCE),
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
    /* NOTE (2026-09-17 audit finding 7): GET /status is intentionally NOT
     * re-listed here. It already has a ROUTE_TIER_OPEN row above (the Wi-Fi
     * status page has both a page-shell and status role, one (uri, method)
     * key). This file is a flat C array, not a keyed map -- nothing
     * prevented a second literal entry for the same key from actually being
     * present here for a time, and the two readers of this table
     * (http_auth_lookup_tier(), which takes the first match, and
     * check_route_tier_coverage.ps1's Get-TieredKeys, which used to take the
     * last) disagreed about which row would win the moment a future edit
     * ever gave the two different tiers. Both rows happened to be OPEN, so
     * the disagreement was inert -- see the audit for the failure scenario.
     * Fixed by deleting this duplicate (the surviving row above wins under
     * both readers) and by check_route_tier_coverage.ps1 now refusing to
     * build a tier map at all when it finds a repeated (method, uri) key. */
    ROUTE_TIER("/settings", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/settings/backup", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/settings/display", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/settings/zones", HTTP_GET, ROUTE_TIER_ADMIN),
    /* Kiln-config selector + management page, split out of main_page.html
     * 2026-09-18 (kiln_cfg_http.c). Same tier as every other settings page
     * shell. */
    ROUTE_TIER("/settings/kiln_configs", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/settings/safety", HTTP_GET, ROUTE_TIER_ADMIN),
    /* WEB_AUTH_PLAN.md section 6: the admin password/settings page
     * (security_http.c) and the two API routes it calls -- GET returns only
     * boolean/timeout status (never a hash/salt/PIN value), POST is the one
     * dispatch route every Save button on that page submits to. ADMIN, same
     * as every other /settings page shell and /api/settings writer. */
    ROUTE_TIER("/settings/security", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/auth/config", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/auth/security", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/safety", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/safety/commissioning", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/profiles", HTTP_GET, ROUTE_TIER_ADMIN),
    /* docs/LIVE_PROFILE_EDIT_PLAN.md pass 2 (section 10) -- all five routes
     * ADMIN, matching /profiles: this edits a firing already running on this
     * kiln, right now, so it is at least as sensitive as profile creation. */
    ROUTE_TIER("/live_profile", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/profile/live", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/profile/live/fork", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/profile/live", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/api/profile/live/decide", HTTP_POST, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/diagnostics", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/readiness", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/setup", HTTP_GET, ROUTE_TIER_ADMIN),
    ROUTE_TIER("/ota", HTTP_GET, ROUTE_TIER_ADMIN),
};

#define ROUTE_TIER_TABLE_COUNT \
    (sizeof(kRouteTierTable) / sizeof(kRouteTierTable[0]))

#endif /* ROUTE_TIER_TABLE_H */
