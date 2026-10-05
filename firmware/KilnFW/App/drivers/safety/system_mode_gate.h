// system_mode_gate -- Phase 6's "is this CLASS of command allowed right now"
// layer, sitting above kiln_io_owner/thermo_owner's race-arbitration locks
// and above readiness_gate.h/ota_interlock.h's existing gates.
// docs/SYSTEM_MODE_GATE_PLAN.md is the design doc this implements; section 5
// there records the owner's 2026-09-25 decisions this table encodes.
//
// Same pure/host-testable layering discipline as ota_interlock.h/
// readiness_gate.h: this file takes a plain-old-data SNAPSHOT (caller-filled,
// no pointers back into profile_executor.h/autotune_engine.h/etc, none of
// which are host-buildable) and answers one pure question -- does this
// snapshot permit this class of action, and if not, why. No I/O, no locks
// taken inside it (SYSTEM_MODE_GATE_PLAN.md 3.4 / CLAUDE.md's "cache a
// snapshot outside the lock" note) -- the caller builds the snapshot from
// each domain's existing cheap read accessor (profile_executor_get_status(),
// autotune_engine_get_status(), etc.) BEFORE taking any lock of its own,
// then calls this with no locks held.
//
// Composition, not replacement (plan section 4): this never replaces
// kiln_io_owner's ownership/safety-fault checks, readiness_gate.h's firing
// checklist, or the OTA/profile mutual interlock -- it is one more row a
// caller consults alongside those, checked ahead of
// readiness_gate_evaluate() rather than instead of it (same position
// App/drivers/http/recovery_start_refusal.h used to occupy, before slice 2
// retired that header in favor of this gate).
//
// Auth vs mode (plan section 3.2): AUTH FIRST, ALWAYS. This gate only ever
// runs for a caller that already cleared route_tier_table.h (or the UART
// bridge's own credential check, or the LCD PIN) -- it never weakens or
// substitutes for an auth check, and route_tier_table.h is untouched by
// this module.
//
// Rollout (plan section 3.6): landed with SYS_ACTION_RAW_RELAY_DEBUG_WRITE
// wired first (kiln_io_owner.c's relay_on_blocked(), the single choke point
// for HTTP's danger-mode relay route, the UART bridge's SET_RELAY/
// SET_RELAY_MASK, and the LCD's manual override -- all three transports at
// once, per the plan's whole point). Slices 2/4/5 (gate-slices-2/4/5 spec,
// 2026-09-25) then wired SYS_ACTION_WRITE_ZONES_CONFIG into every zone/
// config writer: zones_http_post.c, zones_http_pid.c (POST /api/zones/pid --
// revoking its prior deliberate carve-out that let PID-only edits through
// even while a firing was RUNNING/PAUSED), uart_bridge_ext_control.c's
// SET_ZONE_PID/SET_ZONE_MODEL, kiln_cfg_http.c's apply submit,
// backup_import.c's apply, iter_tune_http.c's restore_commissioned, and
// adaptive_tune_http.c's revert handler -- and SYS_ACTION_FACTORY_
// RESET/SYS_ACTION_CFGFS_FORMAT (factory_reset.c's reset_post_handler(),
// the UART-exclusive factory_reset_execute() entry point in
// uart_bridge_system.c, and cfg_fs_format_http.c) the same way. Refusal
// order at every HTTP call site: this gate first, then
// ota_http_check_interlocks(), then http_async_job_busy() where applicable
// (A1) -- reversing that order made this gate's own 409 unreachable while a
// firing was active, a dead-code bug found and fixed during review.
//
// Narrowed, 2026-09-25 (later same day, owner decision): adaptive_tune_http.c's
// enable_post_handler() only gates the enabled=true (turn ON) case now --
// enabled=false (turn OFF) is allowed during a run, since it can only PREVENT
// a future change, never apply one, the same shape as pausing a firing rather
// than a config write. revert_post_handler() is unaffected and still refuses
// unconditionally while a run is active, same as every other wired writer.
//
// Slice 2 (docs/SYSTEM_MODE_GATE_PLAN.md section 3.6), 2026-09-27: wired
// SYS_ACTION_START_PROFILE/SYS_ACTION_START_AUTOTUNE's recovery-mode rule and
// called system_mode_gate_check() for it from profile_executor_run()'s and
// autotune_begin_run_locked()'s existing single choke points, ahead of their
// readiness_gate_evaluate() call -- purely to retire
// App/drivers/http/recovery_start_refusal.h's two HTTP-only call sites
// (dashboard_exec_http.c, dashboard_autotune_http.c), which produced a
// recovery-mode reason string ONLY on HTTP while UART/LCD saw
// readiness_gate.h's own, differently-worded recovery message (plan section
// 2.4's wording drift). Both HTTP call sites now build a snapshot and call
// system_mode_gate_check() instead, so all three transports produce the
// exact same string. They send it in their own JSON 409 envelope
// ({"ok":false,"readiness_item":"recovery_mode","error":...}), NOT via
// system_mode_gate_http_send_refusal()'s plain text -- their frontend
// callers parse the response with r.json(). recovery_start_
// refusal.h and its host test are retired (deleted) as a result -- this was
// their only caller. SYS_ACTION_START_PROFILE/START_AUTOTUNE otherwise still
// return OK unconditionally: every non-recovery start refusal stays owned by
// readiness_gate.h, unchanged by this slice.
// system_mode_gate_check() still returns OK unconditionally for any action
// nobody has wired a rule for yet (SYS_ACTION_OTA_START keeps its own
// existing gate instead -- ota_interlock.c).
#ifndef SYSTEM_MODE_GATE_H
#define SYSTEM_MODE_GATE_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// One entry per CLASS of command (plan section 3.1), not one per route or
// UART subcommand -- SYS_ACTION_RAW_RELAY_DEBUG_WRITE covers HTTP's
// /api/diagnostics/danger/relay, uart_bridge.c's SET_RELAY/SET_RELAY_MASK,
// and the LCD's manual override alike, because kiln_io_owner.c's
// relay_on_blocked() is the one choke point all three already funnel
// through (docs/SYSTEM_MODE_GATE_PLAN.md section 2.5).
typedef enum {
    SYS_ACTION_START_PROFILE = 0,   // wired -- recovery_mode only (slice 2); see rollout note
    SYS_ACTION_START_AUTOTUNE,      // wired -- recovery_mode only (slice 2); see rollout note
    SYS_ACTION_WRITE_ZONES_CONFIG,   // wired -- zones_http_post.c, zones_http_pid.c,
                                      // uart_bridge_ext_control.c, kiln_cfg_http.c,
                                      // backup_import.c, iter_tune_http.c,
                                      // adaptive_tune_http.c's revert handler and its
                                      // enable handler's enabled=true case only -- see
                                      // rollout note's 2026-09-25 narrowing
    SYS_ACTION_RAW_RELAY_DEBUG_WRITE, // wired: kiln_io_owner.c's relay_on_blocked() (relay-ON only;
                                      // also consults restore_in_flight -- 2026-09-28 A4 follow-up B)
    SYS_ACTION_FACTORY_RESET,        // wired: factory_reset.c's reset_post_handler() and
                                      // uart_bridge_system.c's factory_reset_execute()
    SYS_ACTION_CFGFS_FORMAT,         // wired: cfg_fs_format_http.c's format_confirm_post_handler()
    SYS_ACTION_RECOVERY_BOOT,        // wired: ota_http_recovery.c's ota_recovery_boot_post_handler()
                                      // (POST /api/ota/esp/recovery_boot, docs/OTA_SINGLE_SLOT_PLAN.md
                                      // section 4 "Deliberate entry into recovery") -- refused while a
                                      // firing/autotune is active OR any relay is (or may be) energized
    SYS_ACTION_STAGE_WRITE,          // wired: update_http.c's stage upload and stage clear handlers
                                      // (docs/GITHUB_RELEASE_UPDATE_PLAN.md WP4) -- refused outright
                                      // while a firing/autotune is active, PAUSED included
    SYS_ACTION_UPDATE_SETTINGS_WRITE, // wired: update_settings_http.c's POST /api/update/settings
                                      // handler and backup_import.c's update_repo persist step
                                      // (docs/GITHUB_RELEASE_UPDATE_PLAN.md WP9) -- refused outright
                                      // while a firing/autotune is active, PAUSED included
    SYS_ACTION_OTA_START,            // not gated here -- ota_interlock.c stays the owner (plan section 4)
} sys_action_t;

// Every board fact any action's rule needs, and nothing else -- passed by
// value so the decision is pure and the full cross product is host-testable
// (SYSTEM_MODE_GATE_PLAN.md section 3.5). Field meanings mirror
// ota_interlock_snapshot_t / readiness_gate_facts_t's own convention of
// naming the live accessor each field is filled from.
typedef struct {
    bool profile_running;      // profile_executor_get_status() shows RUNNING or PAUSED
    bool autotune_running;     // autotune_engine_get_status()/autotune_engine_is_active()
    bool ota_holds_interlock;  // ota_interlock_check()/ota_http_heat_blocked_by_update() -- reserved,
                                // not consulted by any rule wired in this pass (kiln_io_owner.c
                                // already checks its own OTA interlock separately and first)
    bool recovery_mode;        // boot_guard_is_recovery_mode() -- consulted by SYS_ACTION_START_PROFILE/
                                // SYS_ACTION_START_AUTOTUNE's rule (slice 2); unused by every other rule
    bool restore_in_flight;    // backup_import_restore_in_flight() -- consulted by SYS_ACTION_START_PROFILE/
                                // SYS_ACTION_START_AUTOTUNE's rule (2026-09-28 A4 review follow-up A):
                                // a backup restore's commit pass (tens of seconds, on the http_async_job
                                // task) writes the same profiles/zones state a start reads; refusing a
                                // start while it is set closes the window a restore-in-flight start could
                                // otherwise land in. Also consulted by SYS_ACTION_RAW_RELAY_DEBUG_WRITE
                                // (2026-09-28, A4 review follow-up B): manual/danger-mode relay-ON and the
                                // zone current sweep start (zones_current_sweep_task.c, which checks this
                                // flag directly rather than through this gate -- see its own call site)
                                // were the two heat paths this flag did not yet reach. Unused by every
                                // other rule. Never consulted for a relay-OFF -- this module is never
                                // called on that path.
    bool relays_energized;     // any relay shadow bit set, OR the relay state could not be read (an
                                // unreadable relay is never assumed off) -- consulted only by
                                // SYS_ACTION_RECOVERY_BOOT, unused by every other rule
    bool safety_tripped;       // ARMED-latch trip state -- reserved, unused by this pass's rules
    bool readiness_gate_ready; // !readiness_gate_refuses_start() -- reserved, unused by this pass's rules
} sys_mode_snapshot_t;

// Longest reason string this module writes -- sized like ota_interlock.h's
// OTA_INTERLOCK_REASON_MAX for the same "one more 96-char human-readable
// why field" precedent.
#define SYSTEM_MODE_GATE_REASON_MAX 96

// The decision. Pure: no I/O, no locks, safe to call from any task including
// one holding a lock of its own PROVIDED snap was built before that lock was
// taken (this function itself never blocks). Returns true (refused) or false
// (allowed). When refused and reason/reason_cap are non-NULL, writes a
// human-readable, JSON-safe reason (no '"' or '\\' -- same convention
// readiness_gate.h's messages follow, since dashboard_exec_http.c-style
// callers may embed it directly into a JSON body) -- truncated if reason_cap
// is too small, always NUL-terminated. Leaves reason untouched when not
// refused.
bool system_mode_gate_check(sys_action_t action, const sys_mode_snapshot_t *snap, char *reason,
                             size_t reason_cap);

#ifdef __cplusplus
}
#endif

#endif // SYSTEM_MODE_GATE_H
