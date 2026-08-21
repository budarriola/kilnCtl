// spi_emu_b.h -- single owner of PIO1 + DMA (docs/PLAN.md section 4.1 task
// map): "1-channel MAX31856 register machine, safety bus." This is bus B
// from section 3.2.1 -- one CS line into the safety Pico's thermocouple bus
// (J7), crossing the digital isolator (section 3.5). Single-owner-per-
// peripheral doctrine, PLAN.md section 4's opening paragraph: no other task
// file may touch PIO1 or its DMA channels.
//
// Real body: owns 1 max31856_channel_t register image (src/sim/
// max31856_regs.h) and the PIO SPI slave engine (src/drivers/
// max31856_pio_engine.h) answering the safety Pico's SPI master against it,
// fed from sim_snapshot_read()'s T_safety_reported_c. See spi_emu_a.h's
// header comment for the shared design notes (the DMA-fed Plan B, task-loop
// cadence, build-vs-hardware-timing verification status) -- not repeated
// here.
#ifndef SIMFW_TASKS_SPI_EMU_B_H
#define SIMFW_TASKS_SPI_EMU_B_H

#include <stdbool.h>
#include <stdint.h>

#include "drivers/max31856_pio_engine.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SPI_EMU_B_CHANNEL_COUNT 1u

// Creates spi_emu_b at SIMFW_PRIO_SPI_EMU_B, pinned to SIMFW_CORE_RT_PATH
// (task_priorities.h). Returns false if task creation failed.
bool spi_emu_b_start(void);

// Read-only instrumentation access (see spi_emu_a_get_stats's comment).
// channel is always 0 on this single-channel bus.
max31856_pio_stats_t spi_emu_b_get_stats(uint8_t channel);

// Coherent register-image getter and "has the master ever configured this
// channel" flag -- see spi_emu_a_get_reg_image()/spi_emu_a_channel_configured()
// in spi_emu_a.h for the full coherency-guarantee rationale, which applies
// identically here (same PIO engine, same max31856_regs.h model, just one
// channel instead of three). channel is always 0 on this single-channel bus.
bool spi_emu_b_get_reg_image(uint8_t channel, uint8_t out_regs[MAX31856_REG_COUNT]);
bool spi_emu_b_channel_configured(uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_SPI_EMU_B_H
