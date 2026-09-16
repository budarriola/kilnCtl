// link_task_tc_type_gate.h -- the pure decision core of
// link_task_heat_is_safe_for_tc_type_change() (link_task.c), factored out
// so it is host-testable (link_task.c itself pulls in FreeRTOS/pico-sdk and
// is not built for the host test executable). Every input is a plain
// scalar the caller has already read from whatever lock/queue/frame it
// actually lives behind -- this file itself touches no lock, no hardware,
// no global state; it takes a snapshot and returns a verdict.
//
// 2026-09-15 Opus re-review N1: see link_task.c's own header comment on
// link_task_heat_is_safe_for_tc_type_change() for the full history and
// rationale (G2's PWM-chop fix, this fix's "the Pico owns that pole"
// rework). This header only documents the shape of the pure decision;
// that comment documents WHY it looks like this.
#ifndef SAFTYFW_TASKS_LINK_TASK_TC_TYPE_GATE_H
#define SAFTYFW_TASKS_LINK_TASK_TC_TYPE_GATE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// "Recent" window for the enable-age check -- see link_task.c's
// LINK_TASK_ENABLE_RECENT_SAFE_WINDOW_MS (this is that same constant,
// moved here so the pure decision function and its host test share one
// definition rather than risking the two drifting apart).
#define LINK_TASK_TC_TYPE_GATE_ENABLE_RECENT_SAFE_WINDOW_MS 2000u

typedef struct {
    // Check 1: safety_core_get_output_status()'s relay_energized -- is K4
    // actually energized right now (Pico's own hardware ground truth).
    bool relay_energized;

    // Check 2: safety_core_ms_since_last_enable_true_request().
    bool     enable_ever_seen;
    uint32_t enable_age_ms;

    // Check 3: the ESP-reported context frame.
    bool     degraded_no_context; // link_task's own DEGRADED_NO_CONTEXT state
    bool     context_ever_received; // s_context_published
    bool     context_lock_available; // could the snapshot be read at all
    bool     context_valid; // decoded frame's own CONTEXT_VALID flag
    uint32_t context_age_ms;
    uint32_t context_max_age_ms; // caller's LINK_TASK_CONTEXT_MAX_AGE_MS
    uint8_t  context_flags; // raw flags byte -- HEAT_REQUESTED/PROFILE_RUNNING/HEAT_OWNER_ACTIVE bits

    // Additional Pico-local heuristics, defense in depth on top of 1-3.
    bool relay_on_continuous;
    bool any_current_present;
} link_task_tc_type_gate_input_t;

// Returns true only if every one of checks 1-3 plus the two additional
// heuristics agrees heat is not being delivered and no heat owner is
// active -- see link_task.c's doc comment on link_task_heat_is_safe_for_
// tc_type_change() for what each field means and why. Pure function: no
// side effects, no globals, safe to call from a host test.
bool link_task_tc_type_gate_decide(const link_task_tc_type_gate_input_t *in,
                                    uint8_t context_flag_heat_requested,
                                    uint8_t context_flag_profile_running,
                                    uint8_t context_flag_heat_owner_active);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LINK_TASK_TC_TYPE_GATE_H
