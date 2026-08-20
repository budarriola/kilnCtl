// task_priorities.h -- priorities, core affinity and periods for every SimFW
// task, verbatim from docs/PLAN.md section 4.1 (task map). One place, so a
// task file never invents its own number and the table can be diffed
// against the doc. Mirrors ../../SaftyFW/src/task_priorities.h's structure
// and discipline.
//
// Higher FreeRTOS priority number = higher priority (standard FreeRTOS
// convention).
//
// The core split is PLAN.md section 4.1's closing paragraph: "Core 1 is
// reserved for the hard-real-time producers (SPI emulation response
// pre-compute, waveform DMA feeding); core 0 does everything elastic." That
// is a real-time-budget split, not a safety-isolation one the way SaftyFW's
// trip-path/link-path split is -- but it is still load-bearing: SIMFW_PRIO_*
// and SIMFW_CORE_*_PATH are the single source of truth for it, so if you are
// changing a value here, change docs/PLAN.md section 4.1 in the same commit.
#ifndef SIMFW_TASK_PRIORITIES_H
#define SIMFW_TASK_PRIORITIES_H

#include "FreeRTOS.h"

// Without configUSE_CORE_AFFINITY, vTaskCoreAffinitySet() compiles, links,
// and silently does nothing -- the core split in PLAN.md section 4.1
// evaporates with no build error and no runtime symptom until it matters.
// Caught here, at compile time, in every translation unit that includes
// this header, rather than trusted to a FreeRTOSConfig.h nobody re-reads.
// Same guard SaftyFW's own task_priorities.h carries.
#if !defined(configUSE_CORE_AFFINITY) || (configUSE_CORE_AFFINITY != 1)
#error "configUSE_CORE_AFFINITY must be 1 -- see PLAN.md section 4.1: " \
       "without it vTaskCoreAffinitySet() is a no-op and the core split " \
       "silently evaporates."
#endif

#if !defined(configNUMBER_OF_CORES) || (configNUMBER_OF_CORES != 2)
#error "configNUMBER_OF_CORES must be 2 -- SimFW's core split (PLAN.md " \
       "section 4.1) assumes both RP2040 cores are schedulable."
#endif

// Core affinity masks. RP2040 core numbers, not priorities.
#define SIMFW_CORE_RT_PATH       (1u << 1) // core 1: hard-real-time producers (SPI emulation, waveform DMA)
#define SIMFW_CORE_ELASTIC_PATH  (1u << 0) // core 0: everything elastic (PLAN.md section 4.1)

// Priorities, highest first, per PLAN.md section 4.1's "Core / Prio"
// columns. configMAX_PRIORITIES must exceed the highest value used here
// (see FreeRTOSConfig.h).
//
// "high" tier (core 1, real-time producers) -- tied at the top, matching
// SaftyFW's own convention of letting the timer task's priority
// (configMAX_PRIORITIES - 1) double as the ceiling for a build's most
// urgent application tasks.
#define SIMFW_PRIO_SPI_EMU_A     7 // PIO0 + DMA, 3-channel MAX31856 register machine (ESP bus)
#define SIMFW_PRIO_SPI_EMU_B     7 // PIO1 + DMA, 1-channel MAX31856 register machine (safety bus)
#define SIMFW_PRIO_WAVE_OWNER    7 // PWM slices + DMA, 60 Hz CT synthesis

// "mid+" tier
#define SIMFW_PRIO_SIM_ENGINE    5 // 10 Hz thermal model tick, publishes the zone snapshot

// "mid" tier (core 0)
#define SIMFW_PRIO_USB_OWNER     4 // USB CDC (TinyUSB): frame RX/TX, CRC, dispatch, telemetry TX
#define SIMFW_PRIO_CMD_TASK      4 // decode/validate commands, route to owners, build replies
#define SIMFW_PRIO_FAULT_SCHED   4 // trigger evaluation each tick; arms/fires scheduled faults
#define SIMFW_PRIO_I2C_OWNER     4 // I2C0: all expander traffic, relay-sense scan, E-stop/DUT-power outputs

// "low" / "lowest" tiers
#define SIMFW_PRIO_TELEMETRY     2 // periodic state frames to USB
#define SIMFW_PRIO_LOG_TASK      1 // deferred logging, drop-counting, never blocks producers

// Periods, in ticks, for tasks with a fixed period per PLAN.md section 4.1 /
// 4.3. configTICK_RATE_HZ is 1000 (FreeRTOSConfig.h), so pdMS_TO_TICKS is
// exact here. Only sim_engine has a doc-stated fixed cadence today; the
// others are event-driven (usb_owner, cmd_task) or will get a real period
// once their bodies land -- no placeholder period is defined for those so a
// stub loop's vTaskDelay() argument is never mistaken for a frozen contract.
#define SIMFW_PERIOD_SIM_ENGINE_MS   100 // 10 Hz, PLAN.md section 4.3

#endif // SIMFW_TASK_PRIORITIES_H
