// spi_emu_a.h -- single owner of PIO0 + DMA (docs/PLAN.md section 4.1 task
// map): "3-channel MAX31856 register machine, ESP bus." This is bus A from
// section 3.2.1 -- three CS lines (CS0/CS1/CS2) sharing SCLK/MOSI/MISO into
// the ESP32-S3's thermocouple bus (J6). Single-owner-per-peripheral
// doctrine, PLAN.md section 4's opening paragraph: no other task file may
// touch PIO0 or its DMA channels.
//
// Real body: owns 3 max31856_channel_t register images (src/sim/
// max31856_regs.h) and the PIO SPI slave engine that answers a real SPI
// master's reads/writes against them (src/drivers/max31856_pio_engine.h),
// per docs/PLAN.md section 3.2.1's "Plan A -- ISR staging" design. The task's
// own loop periodically reads sim_snapshot_read() and feeds each channel's
// simulated temperature into the register model at roughly the datasheet's
// ~100 ms nominal conversion cadence (PLAN.md 3.2) -- independent of
// sim_engine's own 10 Hz tick rate, per this pass's task instructions -- and
// also reads src/sim/tc_fault_state.h (fault_sched's per-channel corruption-
// knob overrides) each pass, applying it to the channel before advancing the
// conversion.
//
// BUILD-VERIFIED, NOT HARDWARE-TIMING-VERIFIED -- see
// src/drivers/max31856_spi_slave.pio's file header for the full disclaimer;
// the SPI-slave timing this task drives has not been proven against a real
// master in this environment.
#ifndef SIMFW_TASKS_SPI_EMU_A_H
#define SIMFW_TASKS_SPI_EMU_A_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/max31856_pio_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_EMU_A_CHANNEL_COUNT 3u

// Creates spi_emu_a at SIMFW_PRIO_SPI_EMU_A, pinned to SIMFW_CORE_RT_PATH
// (task_priorities.h) -- core 1, the hard-real-time producers' core.
// Returns false if task creation failed.
bool spi_emu_a_start(void);

// Read-only instrumentation access for any other task (e.g. a future
// telemetry/cmd_task pass -- not wired up by this pass, PLAN.md 3.2.1:
// "underruns are counted, never silent"). channel is 0..2 (CS0..CS2).
max31856_pio_stats_t spi_emu_a_get_stats(uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_SPI_EMU_A_H
