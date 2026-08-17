// discrete_task.h -- debounces the E-stop (GPIO9, S7) and mainFault (GPIO10,
// S6a) discretes. docs/ARCHITECTURE.md section 4: 10 ms period, so a 50 ms
// E-stop debounce is 5 samples and a 200 ms mainFault debounce is 20.
#ifndef SAFTYFW_TASKS_DISCRETE_TASK_H
#define SAFTYFW_TASKS_DISCRETE_TASK_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates discrete_task at SAFTYFW_PRIO_DISCRETE_TASK, pinned to
// SAFTYFW_CORE_TRIP_PATH. Returns false if task creation failed.
bool discrete_task_start(void);

// Debounced reads for safety_core (Phase 4). Both active-low at the pin
// (docs/HARDWARE.md); these return the logical, already-inverted sense:
// true == E-stop pressed / mainFault asserted. Safe to call from any task --
// backed by a single volatile read of a value only discrete_task writes.
bool discrete_task_estop_pressed(void);
bool discrete_task_main_fault(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_DISCRETE_TASK_H
