// telemetry.h -- SKELETON STUB. Per docs/PLAN.md section 4.1 task map:
// "Periodic state frames to USB (temps, relay states, active faults, sim
// clock)." Owns no peripheral (single-owner-per-peripheral doctrine, PLAN.md
// section 4's opening paragraph) -- it reads the zone snapshot and event
// ring other owners publish (section 4.5) and hands frames to usb_owner; it
// never talks to USB CDC directly. Also drains the event ring to USB as EVT
// frames (section 4.5, "telemetry drains it to USB").
//
// Real body -- the periodic telemetry frame (section 5.3, default 2 Hz) and
// the EVT-frame drain -- is NOT implemented here. This header/.c pair only
// creates the task and idles it.
#ifndef SIMFW_TASKS_TELEMETRY_H
#define SIMFW_TASKS_TELEMETRY_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Creates telemetry at SIMFW_PRIO_TELEMETRY, pinned to
// SIMFW_CORE_ELASTIC_PATH (task_priorities.h). Returns false if task
// creation failed.
bool telemetry_start(void);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_TELEMETRY_H
