// log_task.h -- SKELETON STUB. Per docs/PLAN.md section 4.1 task map:
// "Deferred logging, drop-counting, never blocks producers." Lowest
// priority in the system, always droppable -- mirrors
// ../../SaftyFW/src/tasks/log_task.h's doctrine: a log frame must never be
// able to displace a telemetry frame, and no producer task ever blocks
// waiting for this one.
//
// Real body -- the log queue, the drop counter, and handing entries to
// usb_owner -- is NOT implemented here. This header/.c pair only creates
// the task and idles it.
#ifndef SIMFW_TASKS_LOG_TASK_H
#define SIMFW_TASKS_LOG_TASK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates log_task at SIMFW_PRIO_LOG_TASK, pinned to
// SIMFW_CORE_ELASTIC_PATH (task_priorities.h). Returns false if task
// creation failed.
bool log_task_start(void);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_LOG_TASK_H
