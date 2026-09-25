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
// caller consults alongside those, same as recovery_start_refusal.h sits
// alongside readiness_gate_refuses_start() rather than instead of it.
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
// 2026-09-25) then wired SYS_ACTION_WRITE_ZONES_CONFIG (zones_http_post.c,
// uart_bridge_ext_control.c's SET_ZONE_PID/MODEL, kiln_cfg_http.c's apply
// submit, backup_import.c's apply) and SYS_ACTION_FACTORY_RESET/
// SYS_ACTION_CFGFS_FORMAT (factory_reset.c, cfg_fs_format_http.c) the same
// way. system_mode_gate_check() still returns OK unconditionally for any
// action nobody has wired a rule for yet (SYS_ACTION_START_PROFILE/
// SYS_ACTION_START_AUTOTUNE/SYS_ACTION_OTA_START keep their own existing
// gates instead -- readiness_gate.h and ota_interlock.c respectively).
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
    SYS_ACTION_START_PROFILE = 0,
    SYS_ACTION_START_AUTOTUNE,
    SYS_ACTION_WRITE_ZONES_CONFIG,   // wired -- see callers listed in this file's rollout note
    SYS_ACTION_RAW_RELAY_DEBUG_WRITE, // wired: kiln_io_owner.c's relay_on_blocked()
    SYS_ACTION_FACTORY_RESET,        // wired: factory_reset.c's reset_post_handler()
    SYS_ACTION_CFGFS_FORMAT,         // wired: cfg_fs_format_http.c's format_confirm_post_handler()
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
    bool recovery_mode;        // boot_guard_is_recovery_mode() -- reserved, unused by this pass's rules
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
