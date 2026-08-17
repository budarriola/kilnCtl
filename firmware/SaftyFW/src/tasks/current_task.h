// current_task.h -- samples ADC0/1/2 (Phase 6). Genuinely shared hardware:
// one SAR ADC round-robined across three current-sense channels
// (docs/ARCHITECTURE.md section 3, adc_owner). The actual sampling/
// conversion math lives in src/current_sense.{c,h}; this task owns the
// hardware, the timing, and publishing mutex-guarded copies of its output.
#ifndef SAFTYFW_TASKS_CURRENT_TASK_H
#define SAFTYFW_TASKS_CURRENT_TASK_H

#include <stdbool.h>

#include "current_sense.h"
#include "snapshots.h"

#ifdef __cplusplus
extern "C" {
#endif

// Creates current_task at SAFTYFW_PRIO_CURRENT_TASK, pinned to
// SAFTYFW_CORE_TRIP_PATH. Returns false if task creation failed.
bool current_task_start(void);

// Copies out the most recent published current_snapshot_t. Safe to call
// from any task. Nothing calls this yet (Phase 7's S3/S4/S9 are the first
// real consumer) -- it exists so this phase's output is actually reachable
// once that lands.
void current_task_get_snapshot(current_snapshot_t *out);

// Copies out the most recent published power-estimate quantities
// (docs/CURRENT_SENSE.md section 3b). Safe to call from any task. Intended
// consumer is Phase 8's SAFETY_CMD_POWER telemetry frame, not a guard --
// see current_sense.h's header comment for why this is a separate struct.
void current_task_get_power(current_sense_power_t *out);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_CURRENT_TASK_H
