// thermo_task.h -- reads the MAX31856 (Phase 3). Event-driven on ~DRDY
// (GPIO12), a real Pico IRQ per docs/ARCHITECTURE.md section 3 ("unlike the
// main board, this is a direct Pico GPIO, so do not poll it").
#ifndef SAFTYFW_TASKS_THERMO_TASK_H
#define SAFTYFW_TASKS_THERMO_TASK_H

#include <stdbool.h>

#include "snapshots.h"

#ifdef __cplusplus
extern "C" {
#endif

// Registers the ~DRDY (GPIO12) falling-edge IRQ and creates thermo_task at
// SAFTYFW_PRIO_THERMO_TASK, pinned to SAFTYFW_CORE_TRIP_PATH. main.c must
// have already brought up spi_owner and probed/configured the MAX31856
// (docs/ARCHITECTURE.md section 5, steps 5-6) before calling this -- a
// missing/dead part is not fatal (this task just keeps publishing invalid
// snapshots), but the interface has to exist first. Returns false if task
// creation failed.
bool thermo_task_start(void);

// Copies the most recently published snapshot into *out under a mutex (no
// torn reads). Returns false (and leaves *out zeroed/invalid) only if
// thermo_task has not published anything yet, e.g. called before
// thermo_task_start(). Callable from any task -- this is how safety_core
// pulls the reading, matching ARCHITECTURE.md section 2's "safety_core
// pulls, nothing pushes" rule.
bool thermo_task_get_snapshot(thermo_snapshot_t *out);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_THERMO_TASK_H
