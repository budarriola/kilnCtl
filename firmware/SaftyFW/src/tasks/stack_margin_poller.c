// stack_margin_poller.c -- see stack_margin_poller.h for the full design
// rationale and the 2026-08-23 regression this module is built to avoid
// repeating.
#include "stack_margin_poller.h"

#include "FreeRTOS.h"
#include "task.h"

// Per-task configured stack depth, IN WORDS (the value each task's own
// xTaskCreate() call passes -- this port's xTaskCreate() takes WORDS, see
// kilnlink_stack_margin.h's own "UNITS" section). Cross-referenced by hand
// against each task's own *_STACK_WORDS #define (relay_owner.c,
// safety_core.c, discrete_task.c, thermo_task.c, current_task.c,
// link_task.c, log_task.c, update_task.c, watchdog_task.c) rather than
// including each of those headers, several of which are isolation-
// restricted (docs/ARCHITECTURE.md section 2) and none of which export
// their stack-depth constant publicly.
//
// THIS IS A DUPLICATED VALUE, THE SAME HAZARD CLASS AS THE PROJECT'S OWN
// "reset one side of a pair" findings (see project memory
// project_reset_one_side_bug_class.md and CLAUDE.md's own writeup): if a
// task's *_STACK_WORDS changes, this table does NOT change with it
// automatically, and nothing here currently re-derives it mechanically.
// check_saftyfw_task_stack_budgets.py's static ELF-time walk is the
// authoritative source for each task's actual configured depth; this table
// is a manually-kept mirror of it for wire-reporting purposes only, and
// SHOULD be cross-checked against that script's own per-task table
// whenever either changes. Reading a low fraction-used value here is only
// as trustworthy as this table being current.
typedef struct {
    kilnlink_stack_margin_task_id_t task_id;
    const char                     *task_name; // must match the xTaskCreate() name string exactly
    uint16_t                        stack_total_words;
    uint16_t                        high_water_words; // KILNLINK_STACK_MARGIN_UNMEASURED until sampled
} stack_margin_slot_t;

static stack_margin_slot_t s_slots[KILNLINK_STACK_MARGIN_NUM_TASKS] = {
    {KILNLINK_STACK_MARGIN_TASK_RELAY_OWNER,  "relay_owner",   256u,  KILNLINK_STACK_MARGIN_UNMEASURED},
    {KILNLINK_STACK_MARGIN_TASK_SAFETY_CORE,  "safety_core",   1536u, KILNLINK_STACK_MARGIN_UNMEASURED},
    {KILNLINK_STACK_MARGIN_TASK_DISCRETE_TASK,"discrete_task", 1024u, KILNLINK_STACK_MARGIN_UNMEASURED},
    {KILNLINK_STACK_MARGIN_TASK_THERMO_TASK,  "thermo_task",   1024u, KILNLINK_STACK_MARGIN_UNMEASURED},
    {KILNLINK_STACK_MARGIN_TASK_CURRENT_TASK, "current_task",  1536u, KILNLINK_STACK_MARGIN_UNMEASURED},
    {KILNLINK_STACK_MARGIN_TASK_LINK_TASK,    "link_task",     2560u, KILNLINK_STACK_MARGIN_UNMEASURED},
    {KILNLINK_STACK_MARGIN_TASK_LOG_TASK,     "log_task",      512u,  KILNLINK_STACK_MARGIN_UNMEASURED},
    {KILNLINK_STACK_MARGIN_TASK_UPDATE_TASK,  "update_task",   1536u, KILNLINK_STACK_MARGIN_UNMEASURED},
    {KILNLINK_STACK_MARGIN_TASK_WATCHDOG_TASK,"watchdog_task", 256u,  KILNLINK_STACK_MARGIN_UNMEASURED},
};

// Round-robin cursor and completed-round counter. Both only ever touched
// from stack_margin_poller_tick()'s own caller (log_task_fn(), a single
// task) except for the read in stack_margin_poller_snapshot() below --
// aligned uint8_t/uint16_t reads/writes are atomic on this Cortex-M0+
// target, and this data is diagnostic (floor-not-ceiling, see
// kilnlink_stack_margin.h), so a torn snapshot mixing one tick's before/
// after state is an acceptable, self-correcting-next-poll cost, not a
// hazard worth a lock -- the same tradeoff current_task.c's own
// taskENTER_CRITICAL()-free published scalars make for similarly
// diagnostic, self-correcting state, WHERE THE FIELD IS SINGLE-WORD.
static uint8_t s_next_index = 0;
static uint8_t s_rounds_completed = 0;

void stack_margin_poller_tick(void)
{
    stack_margin_slot_t *slot = &s_slots[s_next_index];

    // One task, one call -- see this module's own header comment for why
    // this bound (never more than one uxTaskGetStackHighWaterMark() call
    // per invocation) is the entire point.
    TaskHandle_t handle = xTaskGetHandle(slot->task_name);
    if (handle != NULL) {
        UBaseType_t words = uxTaskGetStackHighWaterMark(handle);
        // Clamp rather than truncate/wrap: every real stack in this build is
        // far below 65534 words, so clamping only ever fires on a logic bug
        // elsewhere, and 0xFFFE (not 0xFFFF, KILNLINK_STACK_MARGIN_UNMEASURED's
        // own sentinel) keeps the two states distinguishable on the wire.
        slot->high_water_words = (words > 0xFFFEu) ? 0xFFFEu : (uint16_t)words;
    }
    // handle == NULL: task not created yet (or renamed) -- leave the slot's
    // current value (UNMEASURED on the very first pass) and simply retry
    // next time this slot's turn comes around, ~4.5s later.

    s_next_index++;
    if (s_next_index >= KILNLINK_STACK_MARGIN_NUM_TASKS) {
        s_next_index = 0;
        if (s_rounds_completed < 255u) {
            s_rounds_completed++;
        }
    }
}

void stack_margin_poller_snapshot(kilnlink_stack_margin_t *out)
{
    if (!out) {
        return;
    }
    out->rounds_completed = s_rounds_completed;
    for (unsigned i = 0; i < KILNLINK_STACK_MARGIN_NUM_TASKS; i++) {
        out->entries[i].task_id = (uint8_t)s_slots[i].task_id;
        out->entries[i].high_water_words = s_slots[i].high_water_words;
        out->entries[i].stack_total_words = s_slots[i].stack_total_words;
    }
}
