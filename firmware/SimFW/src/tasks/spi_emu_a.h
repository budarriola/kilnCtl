// spi_emu_a.h -- single owner of PIO0 + DMA (docs/DESIGN_NOTES.md section 4.1 task
// map): "3-channel MAX31856 register machine, ESP bus." This is bus A from
// section 3.2.1 -- three CS lines (CS0/CS1/CS2) sharing SCLK/MOSI/MISO into
// the ESP32-S3's thermocouple bus (J6). Single-owner-per-peripheral
// doctrine, DESIGN_NOTES.md section 4's opening paragraph: no other task file may
// touch PIO0 or its DMA channels.
//
// Real body: owns 3 max31856_channel_t register images (src/sim/
// max31856_regs.h) and the PIO SPI slave engine that answers a real SPI
// master's reads/writes against them (src/drivers/max31856_pio_engine.h),
// per docs/SPI_ACCESS_AUDIT.md section 6's DMA-fed "Plan B" (which replaced
// DESIGN_NOTES.md 3.2.1's "Plan A -- ISR staging" once the first-byte budget turned
// out to be ~125 ns rather than 1.6 us). The task's
// own loop periodically reads sim_snapshot_read() and feeds each channel's
// simulated temperature into the register model at roughly the datasheet's
// ~100 ms nominal conversion cadence (DESIGN_NOTES.md 3.2) -- independent of
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
// telemetry/cmd_task pass -- not wired up by this pass, DESIGN_NOTES.md 3.2.1:
// "underruns are counted, never silent"). channel is 0..2 (CS0..CS2).
max31856_pio_stats_t spi_emu_a_get_stats(uint8_t channel);

// Coherent register-image getter (closes the DESIGN_NOTES.md 5.2/3.2.1 gap: this
// header previously exported no accessor for a channel's live
// max31856_channel_t.regs[], only instrumentation counters).
//
// COHERENCY GUARANTEE AND WHY THIS APPROACH: DESIGN_NOTES.md 3.2.1 requires that
// "a multi-byte LTCB read is always internally consistent" and that the
// register image is never sampled mid-transaction. The two options PLAN.md
// lists are (a) a busy/retry indication keyed off CS, or (b) a PIO-engine-
// maintained between-transactions-committed shadow. This picks (a): the
// PIO engine already tracks per-channel open-transaction state
// (max31856_pio_engine_channel_busy(), max31856_pio_engine.h) as the
// single source of truth for "is CS low right now", and write
// transactions mutate ch->regs[] directly from IRQ context while CS is
// low (max31856_pio_engine.c's handle_data_byte() calls
// max31856_regs_clock_write_byte() straight through) -- so "busy" is
// already exactly the condition this getter must refuse to sample across.
// Option (b) would mean a *second* copy of every register plus a commit
// step the ISR must remember to run on every CS-rise, duplicating state
// the engine already has; option (a) needs no new state at all.
//
// This is called from cmd_task (a different task, and normally a
// different core -- spi_emu_a's IRQs are pinned to core 1) than the one
// mutating regs[], so a single busy check
// immediately before the copy is not quite enough: CS could assert
// between the check and the memcpy. The implementation therefore also
// re-checks busy *and* the channel's transaction counter
// (max31856_pio_stats_t.transactions, which increments once per CS
// assertion) immediately after the copy; if either changed, the copy may
// have torn and is discarded. This is a bounded, non-blocking retry loop
// (a seqlock-style optimistic read, the same pattern the zone snapshot
// uses elsewhere in this repo, DESIGN_NOTES.md 4.5) -- it never spins waiting on
// the ISR and never introduces a lock the IRQ/PIO path could stall on
// (DESIGN_NOTES.md 4.5's "nothing may block spi_emu_*" is about spi_emu_a/b's own
// hot path, which this getter runs nowhere near).
//
// Returns true and fills out_regs[MAX31856_REG_COUNT] with a coherent
// snapshot of the channel's 16-byte register image if one was obtained
// within the retry budget. Returns false (out_regs left untouched) if the
// channel was observed busy on every attempt -- callers (cmd_task's
// TC_GET_REGS/TC_GET_MASTER_CONFIG handlers) must report this truthfully
// as reg_image_valid=false / busy, never guess at a possibly-torn image.
bool spi_emu_a_get_reg_image(uint8_t channel, uint8_t out_regs[MAX31856_REG_COUNT]);

// True once the master has ever written any byte to this channel (any
// register address, including a write attempt at a read-only one) --
// distinguishes "the DUT configured this channel wrong" from "the DUT
// never configured this channel at all" (DESIGN_NOTES.md 5.2's
// TC_GET_MASTER_CONFIG). Backed by max31856_channel_t.master_has_written
// (src/sim/max31856_regs.h), a monotonic single-byte flag that needs no
// busy/retry protection the way the 16-byte reg image does -- see that
// field's own comment.
bool spi_emu_a_channel_configured(uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_TASKS_SPI_EMU_A_H
