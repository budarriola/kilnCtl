// spi_emu_b.c -- see spi_emu_b.h. Real body: PLAN.md section 3.2.1's PIO
// SPI slave engine driving 1 max31856_channel_t register image (safety-side
// channel behind J7, CS0), fed from sim_snapshot_read()'s
// T_safety_reported_c. See spi_emu_a.c's header comment for the design
// notes shared with bus A.
//
// BUILD-VERIFIED, NOT HARDWARE-TIMING-VERIFIED -- see
// src/drivers/max31856_spi_slave.pio's file header for the full disclaimer.
#include "spi_emu_b.h"

#include "FreeRTOS.h"
#include "task.h"

#include "task_priorities.h"
#include "drivers/max31856_pio_engine.h"
#include "sim/max31856_regs.h"
#include "sim/sim_snapshot.h"
#include "sim/tc_fault_state.h"

#define SPI_EMU_B_STACK_WORDS   (configMINIMAL_STACK_SIZE * 2u)
#define SPI_EMU_B_SCAN_DELAY_MS 20u // see spi_emu_a.c's identical constant's comment -- task-loop cadence only, not the register update path

// --- Bus B pin configuration (PROVISIONAL -- see spi_emu_a.c's identical
// disclaimer). Chosen immediately adjacent to bus A's GPIO6-11 block
// (GPIO12-15), disjoint from both that block and i2c_owner.c's GPIO4/5.
// Same consecutive-numbering requirement as bus A applies here (see
// max31856_spi_slave.pio's header comment) -- SCLK=MOSI-1, CS0=SCLK+3.
#define SPI_EMU_B_SCLK_GPIO 12u
#define SPI_EMU_B_MOSI_GPIO 13u
#define SPI_EMU_B_MISO_GPIO 14u
#define SPI_EMU_B_CS0_GPIO  15u

static TaskHandle_t s_task_handle = NULL;
static max31856_channel_t s_channel;
static max31856_pio_bus_t s_bus;

static void spi_emu_b_task_fn(void *arg)
{
    (void)arg;

    max31856_regs_init(&s_channel, 0xB1u); // fixed, deterministic seed -- see spi_emu_a.c's identical rationale

    const uint cs_gpio[SPI_EMU_B_CHANNEL_COUNT] = { SPI_EMU_B_CS0_GPIO };
    max31856_channel_t *const channel_ptrs[SPI_EMU_B_CHANNEL_COUNT] = { &s_channel };

    // pio1 is the pico-sdk global PIO1 instance handle -- bus B per PLAN.md
    // section 2's connection diagram ("PIO1: SPI slave engine B").
    if (!max31856_pio_engine_init(&s_bus, pio1, SPI_EMU_B_SCLK_GPIO, SPI_EMU_B_MOSI_GPIO, SPI_EMU_B_MISO_GPIO,
                                    cs_gpio, SPI_EMU_B_CHANNEL_COUNT, channel_ptrs)) {
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
            // "the safety-side blend + its own lag, PLAN.md 4.3". PLAN.md
            // 4.3 states the blend defaults to zone 0 with no separate
            // blend-weight fields published in sim_snapshot_t as of this
            // pass, so zone 0's value is read directly here, matching that
            // default; a configurable blend across zones (PLAN.md 4.3's
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
