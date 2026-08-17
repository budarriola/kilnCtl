// task_priorities.h -- priorities, core affinity and periods for every SaftyFW
// task, verbatim from docs/ARCHITECTURE.md section 4. One place, so a task file
// never invents its own number and the table can be diffed against the doc.
//
// Higher FreeRTOS priority number = higher priority (standard FreeRTOS
// convention, matches the doc's "Higher number = higher priority").
//
// The core split is the structural half of the no-hang guarantee
// (ARCHITECTURE.md section 4): everything that can trip the relay lives on
// core 1; everything that touches the link lives on core 0. Getting one of
// these constants wrong is a safety-relevant mistake, not a style one -- if
// you are changing a value here, change docs/ARCHITECTURE.md section 4 in the
// same commit (see that file's header: "if they change, this file changes in
// the same commit").
#ifndef SAFTYFW_TASK_PRIORITIES_H
#define SAFTYFW_TASK_PRIORITIES_H

#include "FreeRTOS.h"

// This is the one rule in ARCHITECTURE.md section 8 easiest to violate by
// omission: without configUSE_CORE_AFFINITY, vTaskCoreAffinitySet() compiles,
// links, and silently does nothing -- the core-isolation property in section 4
// evaporates with no build error and no runtime symptom until it matters.
// Caught here, at compile time, in every translation unit that includes this
// header, rather than trusted to a FreeRTOSConfig.h nobody re-reads.
#if !defined(configUSE_CORE_AFFINITY) || (configUSE_CORE_AFFINITY != 1)
#error "configUSE_CORE_AFFINITY must be 1 -- see ARCHITECTURE.md section 8, " \
       "'FreeRTOS SMP configuration': without it vTaskCoreAffinitySet() is a no-op " \
       "and the core-isolation property in section 4 silently evaporates."
#endif

#if !defined(configNUMBER_OF_CORES) || (configNUMBER_OF_CORES != 2)
#error "configNUMBER_OF_CORES must be 2 -- SaftyFW's core split (ARCHITECTURE.md " \
       "section 4) assumes both RP2040 cores are schedulable."
#endif

// Core affinity masks. RP2040 core numbers, not priorities.
#define SAFTYFW_CORE_TRIP_PATH   (1u << 1) // core 1: everything that can trip the relay
#define SAFTYFW_CORE_LINK_PATH   (1u << 0) // core 0: everything that touches the link

// Priorities, highest first. configMAX_PRIORITIES must exceed the highest
// value used here (see FreeRTOSConfig.h).
#define SAFTYFW_PRIO_RELAY_OWNER    7 // event-driven; must always be able to drop the relay
#define SAFTYFW_PRIO_WATCHDOG_TASK  6 // 250 ms
#define SAFTYFW_PRIO_SAFETY_CORE    5 // 100 ms
#define SAFTYFW_PRIO_DISCRETE_TASK  4 // 10 ms
#define SAFTYFW_PRIO_THERMO_TASK    3 // ~DRDY IRQ-driven
#define SAFTYFW_PRIO_CURRENT_TASK   3 // 50 ms
#define SAFTYFW_PRIO_LINK_TASK      2 // event-driven; second-lowest, always droppable under load
#define SAFTYFW_PRIO_LOG_TASK       1 // event-driven; lowest, always droppable

// Periods, in ticks, for the tasks with a fixed period (ARCHITECTURE.md
// section 4). configTICK_RATE_HZ is 1000 (FreeRTOSConfig.h), so pdMS_TO_TICKS
// is exact here.
#define SAFTYFW_PERIOD_WATCHDOG_TASK_MS  250
#define SAFTYFW_PERIOD_SAFETY_CORE_MS    100
#define SAFTYFW_PERIOD_DISCRETE_TASK_MS  10
#define SAFTYFW_PERIOD_CURRENT_TASK_MS   50

#endif // SAFTYFW_TASK_PRIORITIES_H
