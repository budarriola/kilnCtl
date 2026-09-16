#ifndef HEAT_OWNER_ACTIVE_DECIDE_H
#define HEAT_OWNER_ACTIVE_DECIDE_H

#include <stdbool.h>

#include "profile_executor_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 2026-09-15 (Opus adversarial re-review, F6). safety_build_and_send_
 * context() (safety_link_frames.c) is the only compiling caller of this
 * decision -- and, until this fix, also the only place it existed, so the
 * one host-test executable that links safety_link_frames.c
 * (test_safety_link_compile.c) had to fake both heat_enable_is_held() and
 * danger_mode_active() fixed false to satisfy the linker
 * (fake_danger_mode_for_safety_link.c, a986f170), which meant nothing
 * anywhere asserted this decision actually flips true for a PAUSED firing,
 * a held autotune claim, or danger mode -- the exact three non-RUNNING
 * cases N1's Pico-side gate exists to cover.
 *
 * Pulled out as a pure function (state + two held bools + one danger bool
 * -> bool) so it can be host-tested directly, the same way SaftyFW's
 * link_task_tc_type_gate_decide() is -- without linking heat_enable.c,
 * danger_mode.c or profile_executor.c themselves (all out of scope for this
 * pass; profile_executor_state.h's enum is a pure data header with no .c
 * dependency). safety_build_and_send_context() is unchanged in behavior --
 * it now just calls this instead of repeating the condition inline. */
bool heat_owner_active_decide(profile_exec_state_t profile_state, bool profile_claimant_held,
                               bool autotune_claimant_held, bool danger_mode_is_active);

#ifdef __cplusplus
}
#endif

#endif // HEAT_OWNER_ACTIVE_DECIDE_H
