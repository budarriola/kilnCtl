// spi_emu_b.c -- see spi_emu_b.h. Real body: DESIGN_NOTES.md section 3.2.1's PIO
// SPI slave engine driving 1 max31856_channel_t register image (safety-side
// channel behind J7, CS0), fed from sim_snapshot_read()'s
// T_safety_reported_c. See spi_emu_a.c's header comment for the design
// notes shared with bus A.
//
// BUILD-VERIFIED, NOT HARDWARE-TIMING-VERIFIED -- see
// src/drivers/max31856_spi_slave.pio's file header for the full disclaimer.
#include "spi_emu_b.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "task_priorities.h"
#include "drivers/max31856_pio_engine.h"
#include "sim/max31856_regs.h"
#include "sim/max31856_resp_image.h"
#include "sim/sim_snapshot.h"
#include "sim/tc_fault_state.h"

// See spi_emu_a.c's identical constant's comment: a -fstack-usage audit
// (2026-08-24) measured this task's worst-case call chain (through
// max31856_pio_engine_init()'s simfw_fatal() failure path) at ~1228 B,
// leaving only ~1.67x margin over the old 2x (2048 B) budget -- under this
// project's 2x floor. 3x (3072 B) restores ~2.5x.
#define SPI_EMU_B_STACK_WORDS   (configMINIMAL_STACK_SIZE * 3u)
#define SPI_EMU_B_SCAN_DELAY_MS 20u // see spi_emu_a.c's identical constant's comment -- task-loop cadence only, not the register update path

// See spi_emu_a.c's identical constant's comment.
#define SPI_EMU_B_REG_IMAGE_MAX_RETRIES 4u

// --- Bus B pin configuration (PROVISIONAL -- see spi_emu_a.c's identical
// disclaimer). Chosen immediately adjacent to bus A's GPIO6-11 block
// (GPIO12-15), disjoint from both that block and i2c_owner.c's GPIO4/5.
// Same consecutive-numbering requirement as bus A applies here (see
// max31856_spi_slave.pio's header comment) -- SCLK=MOSI-1, CS0=SCLK+3.
#define SPI_EMU_B_SCLK_GPIO 12u
#define SPI_EMU_B_MOSI_GPIO 13u
#define SPI_EMU_B_MISO_GPIO 14u
#define SPI_EMU_B_CS0_GPIO  15u
// Safety-side ~DRDY (DESIGN_NOTES.md 3.6's "DRDY + ~FAULT (safety side) | 2 |
// direct GPIO"). GPIO26 = `DRDY_SAFETY` per docs/HARDWARE.md section 1, wired
// straight to J7 pin 4 (`thermoDrdy`) -- no digital isolator in this path
// since 2026-08-23 (DESIGN_NOTES.md section 3.5: the fixture's ground is
// commoned with the DUT's). Its partner `FAULT_SAFETY` is GPIO27, adjacent
// on purpose: these were the only two isolator-crossing direct-GPIO lines
// when the isolator still existed, and section 1 keeps them together (and
// next to the GPIO28 spare) so the safety domain's pins do not interleave
// with GND_Main ones on the header -- a grouping that is still useful for
// readability even with the isolator gone. An earlier revision claimed
// GPIO27 here, which took FAULT_SAFETY's pin and split the pair across what
// was then the isolation boundary -- see spi_emu_a.c's DRDY block.
#define SPI_EMU_B_DRDY_GPIO 26

static TaskHandle_t s_task_handle = NULL;
static max31856_channel_t s_channel;
static max31856_pio_bus_t s_bus;

// Double-banked response image -- see spi_emu_a.c's identical declaration and
// max31856_resp_image.h for why the alignment is load-bearing.
static MAX31856_RESP_IMAGE_ALIGN max31856_resp_image_t s_images[MAX31856_PIO_ENGINE_BANKS];

static void spi_emu_b_task_fn(void *arg)
{
    (void)arg;

    max31856_regs_init(&s_channel, 0xB1u); // fixed, deterministic seed -- see spi_emu_a.c's identical rationale

    // pio1 is the pico-sdk global PIO1 instance handle -- bus B per DESIGN_NOTES.md
    // section 2's connection diagram ("PIO1: SPI slave engine B").
    max31856_pio_engine_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pio = pio1;
    cfg.sclk_gpio = SPI_EMU_B_SCLK_GPIO;
    cfg.mosi_gpio = SPI_EMU_B_MOSI_GPIO;
    cfg.miso_gpio = SPI_EMU_B_MISO_GPIO;
    cfg.channel_count = SPI_EMU_B_CHANNEL_COUNT;
    cfg.cs_gpio[0] = SPI_EMU_B_CS0_GPIO;
    cfg.drdy_gpio[0] = SPI_EMU_B_DRDY_GPIO;
    cfg.channels[0] = &s_channel;
    for (uint8_t b = 0; b < MAX31856_PIO_ENGINE_BANKS; b++) {
        max31856_resp_image_init(&s_images[b]);
        cfg.images[0][b] = &s_images[b];
    }
    max31856_resp_image_publish(&s_images[0], &s_channel);

    if (!max31856_pio_engine_init(&s_bus, &cfg)) {
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000)); // see spi_emu_a.c's identical fallback comment
        }
    }

    max31856_pio_engine_start_irq(&s_bus); // must run on this task's own core (core 1) -- see spi_emu_a.c's identical note

    for (;;) {
        sim_snapshot_t snap;
        if (sim_snapshot_read(&snap) && !max31856_pio_engine_channel_busy(&s_bus, 0)) {
            // Fault wiring: see spi_emu_a.c's identical comment -- same
            // tc_fault_state.h reader call site, TC_FAULT_CHANNEL_SAFETY is
            // this bus's one channel.
            tc_fault_override_t override = { 0 };
            if (tc_fault_state_read(TC_FAULT_CHANNEL_SAFETY, &override)) {
                s_channel.corruption = override.corruption;
            }

            // T_safety_reported_c lives per-zone in sim_snapshot_t (not as a
            // single top-level field) -- sim_snapshot.h's struct comment:
            // "the safety-side blend + its own lag, DESIGN_NOTES.md 4.3". DESIGN_NOTES.md
            // 4.3 states the blend defaults to zone 0 with no separate
            // blend-weight fields published in sim_snapshot_t as of this
            // pass, so zone 0's value is read directly here, matching that
            // default; a configurable blend across zones (DESIGN_NOTES.md 4.3's
            // "safety-TC blend weights") is sim_engine's own computation to
            // do upstream of this snapshot field, not something for this
            // task to re-derive.
            //
            // CJ truth: same 25 degC fixed stand-in as spi_emu_a.c, same
            // reason (sim_snapshot_t has no simulated CJ field yet).
            max31856_regs_advance_conversion(&s_channel, snap.zones[0].T_safety_reported_c, 25.0f);

            // tc_fault_state.h's documented second step -- see spi_emu_a.c's
            // identical comment.
            s_channel.regs[MAX31856_REG_SR] |= (uint8_t)(override.force_sr_bits & (MAX31856_FAULT_OPEN | MAX31856_FAULT_OVUV));

            // See spi_emu_a.c's identical call: this is the only path from
            // this task's register model to MISO, and it also re-derives
            // ~DRDY.
            (void)max31856_pio_engine_refresh_image(&s_bus, 0);

            // State-machine invariant check, inside the same
            // channel_busy()-false guard as the refresh above because
            // re-enabling a state machine mid-byte would corrupt a transfer in
            // flight. Cheap: one PIO CTRL read plus two compares for this
            // one-channel bus. See max31856_pio_engine.h's
            // sm_disabled_repairs comment for why this is a counted repair --
            // the bug it exists for (tx_reset() leaving the shared TX state
            // machine disabled, fixed 2026-08-25) produced no error, no fault
            // bit and no log line, just a master reading a plausible constant
            // 0.0 C forever.
            (void)max31856_pio_engine_check_state_machines(&s_bus);
        }

        vTaskDelay(pdMS_TO_TICKS(SPI_EMU_B_SCAN_DELAY_MS));
    }
}

bool spi_emu_b_start(void)
{
    BaseType_t ok = xTaskCreate(spi_emu_b_task_fn, "spi_emu_b", SPI_EMU_B_STACK_WORDS, NULL,
                                 SIMFW_PRIO_SPI_EMU_B, &s_task_handle);
    if (ok != pdPASS) {
        return false;
    }

    vTaskCoreAffinitySet(s_task_handle, SIMFW_CORE_RT_PATH);
    return true;
}

max31856_pio_stats_t spi_emu_b_get_stats(uint8_t channel)
{
    return max31856_pio_engine_get_stats(&s_bus, channel);
}

bool spi_emu_b_get_reg_image(uint8_t channel, uint8_t out_regs[MAX31856_REG_COUNT])
{
    if (channel >= SPI_EMU_B_CHANNEL_COUNT || out_regs == NULL) {
        return false;
    }

    for (uint8_t attempt = 0; attempt < SPI_EMU_B_REG_IMAGE_MAX_RETRIES; attempt++) {
        if (max31856_pio_engine_channel_busy(&s_bus, channel)) {
            continue;
        }
        uint32_t txns_before = spi_emu_b_get_stats(channel).transactions;

        memcpy(out_regs, s_channel.regs, MAX31856_REG_COUNT);

        if (!max31856_pio_engine_channel_busy(&s_bus, channel) &&
            spi_emu_b_get_stats(channel).transactions == txns_before) {
            return true;
        }
    }
    return false;
}

bool spi_emu_b_channel_configured(uint8_t channel)
{
    if (channel >= SPI_EMU_B_CHANNEL_COUNT) {
        return false;
    }
    return s_channel.master_has_written;
}
