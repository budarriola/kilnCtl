// log_task.h -- drains the log ring into the link (Phase 8). Lowest priority
// in the system, always droppable (docs/ARCHITECTURE.md section 1: "A log
// frame must never be able to displace a telemetry frame").
#ifndef SAFTYFW_TASKS_LOG_TASK_H
#define SAFTYFW_TASKS_LOG_TASK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates log_task at SAFTYFW_PRIO_LOG_TASK, pinned to
// SAFTYFW_CORE_LINK_PATH (it drains into link_task's TX ring, so it belongs
// on the same core). Returns false if task creation failed.
bool log_task_start(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_LOG_TASK_H
