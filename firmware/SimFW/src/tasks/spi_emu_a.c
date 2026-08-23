// spi_emu_a.c -- see spi_emu_a.h. Real body: DESIGN_NOTES.md section 3.2.1's PIO
// SPI slave engine driving 3 max31856_channel_t register images (main-side
// channels behind J6, CS0/CS1/CS2), fed from sim_snapshot_read()'s
// T_tc_reported_c per zone.
//
// BUILD-VERIFIED, NOT HARDWARE-TIMING-VERIFIED -- see
// src/drivers/max31856_spi_slave.pio's file header for the full disclaimer.
#include "spi_emu_a.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "task_priorities.h"
#include "drivers/max31856_pio_engine.h"
#include "sim/max31856_regs.h"
#include "sim/max31856_resp_image.h"
#include "sim/sim_snapshot.h"
#include "sim/tc_fault_state.h"

#define SPI_EMU_A_STACK_WORDS   (configMINIMAL_STACK_SIZE * 2u) // headroom for 3 max31856_channel_t images + the pio_bus_t struct, all task-owned locals
#define SPI_EMU_A_SCAN_DELAY_MS 20u // task-loop cadence, NOT the register update cadence -- see the loop body comment; the actual byte-level SPI response path runs entirely in IRQ context (max31856_pio_engine.c), not this loop

// Bounded retry budget for spi_emu_a_get_reg_image()'s seqlock-style
// optimistic copy (see that function's header comment in spi_emu_a.h) --
// never blocks, just gives up and reports busy after this many attempts.
// 4 is generous: a colliding transaction is astronomically unlikely to
// span multiple consecutive attempts (a full SPI transaction takes
// microseconds at 5 MHz; a 16-byte memcpy takes nanoseconds), so this
// exists purely as a documented, finite bound, not a value tuned against
// observed contention.
#define SPI_EMU_A_REG_IMAGE_MAX_RETRIES 4u

// --- Bus A pin configuration (PROVISIONAL -- no fixture hardware has ever
// been wired; docs/HARDWARE.md section 1 is the authoritative pin map and is
// itself labelled provisional, docs/DESIGN_NOTES.md section 3.6/PLAN.md open question 11.1).
// i2c_owner.c already claims GPIO4/5
// for I2C0 (its own header comment marks that pair provisional too); this
// module picks a DIFFERENT, disjoint block: GPIO6-11, six consecutive pins
// matching DESIGN_NOTES.md 3.6's pin-budget table order (SCLK, MOSI, MISO, CS0, CS1,
// CS2). The consecutive numbering is not just documentation-tidiness -- the
// PIO programs in max31856_spi_slave.pio rely on fixed relative-offset
// arithmetic between SCLK/MOSI/CS0 (see that file's header comment), and
// this layout is exactly what makes the numbers used there (offset 31 for
// SCLK-from-MOSI, offset 29 for SCLK-from-CS0) work out. CONFIRM against a
// real traced J6 pinout before wiring a physical harness -- see PLAN.md
// section 11 open question 1 and section 3.6's own "draft -- verify" note.
#define SPI_EMU_A_SCLK_GPIO 6u
#define SPI_EMU_A_MOSI_GPIO 7u
#define SPI_EMU_A_MISO_GPIO 8u
#define SPI_EMU_A_CS0_GPIO  9u
#define SPI_EMU_A_CS1_GPIO  10u
#define SPI_EMU_A_CS2_GPIO  11u

// ~DRDY outputs (DESIGN_NOTES.md 3.6's "DRDY x3 ... direct GPIO, open-drain
// emulation"). Also PROVISIONAL. These numbers come from docs/HARDWARE.md
// section 1's pin table -- DRDY_MAIN_0/1/2 = GPIO2/3/17, wired to J6 pins
// 17/15/13 (`thermoDrdy_0..2`) per that document's section 3.1 -- NOT from a
// fresh derivation here. An earlier revision of this file picked 21/22/26 by
// re-deriving "what is still free" from DESIGN_NOTES.md 3.6 alone, unaware section 1
// had already assigned all eight DRDY/~FAULT lines; that collided head-on
// with FAULT_MAIN_1 (21), FAULT_MAIN_2 (22) and DRDY_SAFETY (26). Section 1
// wins and this file follows it, per section 1's own rule that a driver
// claiming a pin must cite that table or amend it in the same commit.
//
// DRDY_MAIN_2 MOVED off GPIO17 to GPIO0, 2026-08-23: the debug UART turned
// out to already be physically wired to GP16/17 (matching SaftyFW's own
// console_uart.c convention), which this document's earlier GPIO0/1 UART
// assignment did not anticipate. GPIO17 collided head-on with the real
// wiring; GPIO0 was free only because it had been reserved for a UART that
// never actually landed on it. This pin was, and remains, PROVISIONAL --
// docs/HARDWARE.md section 0's own status line confirms no fixture harness
// has been built yet -- so moving it costs nothing physical. See
// docs/HARDWARE.md section 1 for the authoritative table.
//
// The remaining main-side pins section 1 reserves are GPIO19/21/22 for
// FAULT_MAIN_0/1/2, which nothing drives yet. There is no PIO adjacency
// constraint on ~DRDY: max31856_pio_engine_init()'s config_is_sane() checks
// SCLK/MOSI/CS relationships only, and drdy_gpio[] is a plain SIO pin
// toggled between output-low and input-Hi-Z (the open-drain emulation).
#define SPI_EMU_A_DRDY0_GPIO 2
#define SPI_EMU_A_DRDY1_GPIO 3
#define SPI_EMU_A_DRDY2_GPIO 0

static TaskHandle_t s_task_handle = NULL;
static max31856_channel_t s_channels[SPI_EMU_A_CHANNEL_COUNT];
static max31856_pio_bus_t s_bus;

// Double-banked response images -- the bytes the PIO/DMA responder actually
// streams onto MISO with no CPU in the path. Two banks per channel so a
// publish can never tear a burst in flight; see max31856_resp_image.h and
// max31856_pio_engine.h's MAX31856_PIO_ENGINE_BANKS comment. The alignment is
// load-bearing, not decorative: the PIO builds the DMA read pointer by OR-ing
// the address byte into the base's low bits, and
// max31856_pio_engine_init() refuses a misaligned image rather than letting
// that OR quietly produce a wrong pointer.
static MAX31856_RESP_IMAGE_ALIGN max31856_resp_image_t
    s_images[SPI_EMU_A_CHANNEL_COUNT][MAX31856_PIO_ENGINE_BANKS];

// Deterministic per-channel RNG seeds (max31856_regs_init's rng_seed drives
// the noise/bit-error corruption knobs) -- fixed, not derived from anything
// time-based, so a run is reproducible per DESIGN_NOTES.md 4.2's replayability rule.
static const uint32_t s_channel_rng_seeds[SPI_EMU_A_CHANNEL_COUNT] = { 0xA1u, 0xA2u, 0xA3u };

static void spi_emu_a_task_fn(void *arg)
{
    (void)arg;

    static const uint s_cs_gpio[SPI_EMU_A_CHANNEL_COUNT] = { SPI_EMU_A_CS0_GPIO, SPI_EMU_A_CS1_GPIO, SPI_EMU_A_CS2_GPIO };
    static const int s_drdy_gpio[SPI_EMU_A_CHANNEL_COUNT] = { SPI_EMU_A_DRDY0_GPIO, SPI_EMU_A_DRDY1_GPIO, SPI_EMU_A_DRDY2_GPIO };

    // pio0 is the pico-sdk global PIO0 instance handle -- bus A per DESIGN_NOTES.md
    // section 2's connection diagram ("PIO0: SPI slave engine A").
    max31856_pio_engine_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pio = pio0;
    cfg.sclk_gpio = SPI_EMU_A_SCLK_GPIO;
    cfg.mosi_gpio = SPI_EMU_A_MOSI_GPIO;
    cfg.miso_gpio = SPI_EMU_A_MISO_GPIO;
    cfg.channel_count = SPI_EMU_A_CHANNEL_COUNT;

    for (uint8_t i = 0; i < SPI_EMU_A_CHANNEL_COUNT; i++) {
        max31856_regs_init(&s_channels[i], s_channel_rng_seeds[i]);
        for (uint8_t b = 0; b < MAX31856_PIO_ENGINE_BANKS; b++) {
            // FFh everywhere until the first publish -- the datasheet's own
            // answer for an address that reports nothing, and far better than
            // whatever uninitialised RAM would put on the wire if a master
            // clocked this bus before the first scan.
            max31856_resp_image_init(&s_images[i][b]);
            cfg.images[i][b] = &s_images[i][b];
        }
        max31856_resp_image_publish(&s_images[i][0], &s_channels[i]);
        cfg.cs_gpio[i] = s_cs_gpio[i];
        cfg.drdy_gpio[i] = s_drdy_gpio[i];
        cfg.channels[i] = &s_channels[i];
    }

    if (!max31856_pio_engine_init(&s_bus, &cfg)) {
        // Nothing sane to do if the PIO block cannot supply 4 state machines
        // for a bus that owns it exclusively (3 RX + 1 shared TX) -- this
        // should only happen if PIO0 was already partially claimed by a
        // bug elsewhere, which the single-owner-per-peripheral doctrine
        // (DESIGN_NOTES.md section 4) says should never occur. Idle rather than
        // spin/crash so the rest of the system (other tasks) stays alive
        // for USB/telemetry to at least report the failure once that path
        // exists.
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    // Must be called from core 1 (this task's own core, per
    // SIMFW_CORE_RT_PATH below) -- irq_set_exclusive_handler binds to
    // whichever core calls it, and DESIGN_NOTES.md 3.2.1's Plan A requires the
    // address-byte-triggered IRQ to run on core 1.
    max31856_pio_engine_start_irq(&s_bus);

    for (;;) {
        // Task-loop half of DESIGN_NOTES.md 3.2.1's split: NOT the byte-level SPI
        // response path (that is entirely IRQ-driven in
        // max31856_pio_engine.c/the PIO programs) -- this loop only pulls
        // fresh truth from sim_snapshot_read() and pushes it into each
        // channel's register model at roughly the datasheet's ~100 ms
        // nominal conversion cadence (DESIGN_NOTES.md 3.2), independent of
        // sim_engine's own 10 Hz tick and of this loop's own 20 ms scan
        // period (the scan runs faster than the update cadence so a channel
        // that was mid-transaction on one pass gets a fresh chance to
        // update ~20 ms later rather than waiting a further 100 ms).
        sim_snapshot_t snap;
        if (sim_snapshot_read(&snap)) {
            for (uint8_t i = 0; i < SPI_EMU_A_CHANNEL_COUNT && i < snap.zone_count; i++) {
                if (max31856_pio_engine_channel_busy(&s_bus, i)) {
                    // A transaction is open on this channel right now --
                    // DESIGN_NOTES.md 3.2.1's coherency rule: register-image commits
                    // happen "only between transactions, never while that
                    // channel's CS is low." Skip this channel for this pass;
                    // the next 20 ms scan will catch it, well inside the
                    // ~100 ms conversion cadence budget.
                    continue;
                }

                // Fault wiring: tc_fault_state.h landed (fault_sched is the
                // sole writer; this task-loop context, not IRQ context, is
                // the documented reader call site per that header's
                // ownership note). MAIN_0/1/2 index-match this loop's
                // channel index i by construction (tc_fault_state.h's own
                // enum comment).
                tc_fault_override_t override = { 0 }; // no-fault default if the read below fails (out-of-range channel, never expected here)
                if (tc_fault_state_read((tc_fault_channel_t)i, &override)) {
                    s_channels[i].corruption = override.corruption;
                }

                // The main-side TC reads T_tc_reported_c (sim_snapshot.h's
                // "TC lag already applied by sim_engine" signal); CJ truth
                // is not modeled by sim_engine yet (no field for it in
                // sim_snapshot_t as of this pass), so 25 degC (a plausible
                // room-temperature cold junction) is used as a fixed stand-
                // in -- max31856_regs_advance_conversion()'s CJ path still
                // runs for real (offset writes, threshold comparisons), it
                // just tracks a constant rather than a simulated ambient
                // drift. Flagging this rather than inventing a fake "true
                // CJ" field on a struct this task does not own.
                max31856_regs_advance_conversion(&s_channels[i], snap.zones[i].T_tc_reported_c, 25.0f);

                // tc_fault_state.h's documented second step: OPEN/OVUV are
                // not threshold-derived, so advance_conversion() never sets
                // them -- OR the fault-scheduled override in explicitly,
                // masked to just those two bits per that header's own
                // defensive-masking note.
                s_channels[i].regs[MAX31856_REG_SR] |= (uint8_t)(override.force_sr_bits & (MAX31856_FAULT_OPEN | MAX31856_FAULT_OVUV));

                // Publish the freshly-converted registers into the bank the
                // hardware is NOT reading, hand the new base over, and
                // re-derive ~DRDY. This is the ONLY path by which anything
                // this task computes reaches MISO -- under Plan B the DMA
                // never touches s_channels[] -- so a conversion that is not
                // followed by a refresh is a conversion the DUT never sees.
                // Called unconditionally every scan rather than only after a
                // register change, so corruption.bit_error_rate keeps
                // re-rolling; see max31856_resp_image.h's FIDELITY NOTE.
                (void)max31856_pio_engine_refresh_image(&s_bus, i);
            }
        }

        vTaskDelay(pdMS_TO_TICKS(SPI_EMU_A_SCAN_DELAY_MS));
    }
}

bool spi_emu_a_start(void)
{
    BaseType_t ok = xTaskCreate(spi_emu_a_task_fn, "spi_emu_a", SPI_EMU_A_STACK_WORDS, NULL,
                                 SIMFW_PRIO_SPI_EMU_A, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_RT_PATH);
    return true;
}

max31856_pio_stats_t spi_emu_a_get_stats(uint8_t channel)
{
    return max31856_pio_engine_get_stats(&s_bus, channel);
}

bool spi_emu_a_get_reg_image(uint8_t channel, uint8_t out_regs[MAX31856_REG_COUNT])
{
    if (channel >= SPI_EMU_A_CHANNEL_COUNT || out_regs == NULL) {
        return false;
    }

    for (uint8_t attempt = 0; attempt < SPI_EMU_A_REG_IMAGE_MAX_RETRIES; attempt++) {
        if (max31856_pio_engine_channel_busy(&s_bus, channel)) {
            continue; // CS already low -- no point copying, try again
        }
        uint32_t txns_before = spi_emu_a_get_stats(channel).transactions;

        memcpy(out_regs, s_channels[channel].regs, MAX31856_REG_COUNT);

        // If the channel is still not busy AND no new transaction was
        // observed to have started (and possibly already finished) while
        // the copy above ran, nothing could have mutated regs[] mid-copy --
        // see the coherency-guarantee comment in spi_emu_a.h.
        if (!max31856_pio_engine_channel_busy(&s_bus, channel) &&
            spi_emu_a_get_stats(channel).transactions == txns_before) {
            return true;
        }
    }
    return false;
}

bool spi_emu_a_channel_configured(uint8_t channel)
{
    if (channel >= SPI_EMU_A_CHANNEL_COUNT) {
        return false;
    }
    return s_channels[channel].master_has_written;
}
