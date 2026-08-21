// main.c -- SimFW entry point. Build/task skeleton only: creates and
// schedules the ten single-owner tasks from docs/DESIGN_NOTES.md section 4.1's task
// map, each currently an idling stub (src/tasks/*.c). No peripheral is
// actually driven yet -- PIO SPI emulation, CT waveform synthesis, I2C/
// expander logic and USB protocol handling are later work, filled into
// these same task bodies without touching this file or any sibling task
// file (single-owner-per-peripheral doctrine, DESIGN_NOTES.md section 4's opening
// paragraph).
//
// Mirrors ../SaftyFW/src/main.c's shape (stdio bring-up, start every task in
// priority order, start the scheduler) but without that project's boot
// sequence (no GPIO6 safety latch, no watchdog, no config-store/MAX31856
// bring-up here -- SimFW is a bench tool, not the thing being protected, and
// none of that machinery exists yet for this project regardless).
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "drivers/ct_wave_pwm.h"

#include "tasks/cmd_task.h"
#include "tasks/fault_sched.h"
#include "tasks/i2c_owner.h"
#include "tasks/log_task.h"
#include "tasks/sim_engine.h"
#include "tasks/spi_emu_a.h"
#include "tasks/spi_emu_b.h"
#include "tasks/telemetry.h"
#include "tasks/usb_owner.h"
#include "tasks/wave_owner.h"

// ---------------------------------------------------------------------------
// DMA channel budget -- docs/HARDWARE.md section 1b.
//
// The RP2040's 12 DMA channels are one global pool with no per-peripheral
// partitioning. Every dma_claim_unused_channel() call site in the fixture
// now routes a failure through drivers/simfw_fatal.h, which halts loudly
// (matching the neighbouring pio_claim_unused_sm(pio, true) panics) instead
// of the silent degrade this budget used to be the only defense against
// (section 1b.5: a fixture that boots, enumerates and answers commands while
// having no CT output or one dead SPI bus -- fixed this pass). That does NOT
// make this compile-time check redundant: catching a 13th channel at build
// time, before anyone is standing at the bench, is still strictly better
// than catching it via a bench-time halt on whichever core claims last.
//
// This file is the only translation unit that sees every DMA claimant's
// count constant at once -- the drivers cannot check it themselves (a driver
// including a task header would invert the layering) and no single owner can
// see the others' numbers, which is precisely the class of mistake section 1b
// exists to prevent. tools/check_single_owner.ps1 re-derives the same sum from
// the same three headers, so the arithmetic is also enforced without a build,
// and that script fails if this assertion is ever deleted.
//
// The formula is section 1b.2's closed form:
//
//     channels = CT zones + sum over buses of (chips_on_bus + 2)
//
// where the +2 per bus is that bus's `dma_load` + `dma_data` (section 1b.1),
// and each bus's chip count is one `dma_sniff[i]` per emulated chip. If a
// third DMA claimant is ever added, add its term here AND to section 1b's
// table AND to check_single_owner.ps1's $dmaBudgetTerms -- all three, in the
// same commit.
#define SIMFW_DMA_CHANNELS_CLAIMED                                            \
    (CT_WAVE_PWM_NUM_CHANNELS + (SPI_EMU_A_CHANNEL_COUNT + 2u) +              \
     (SPI_EMU_B_CHANNEL_COUNT + 2u))

_Static_assert(SIMFW_DMA_CHANNELS_CLAIMED <= NUM_DMA_CHANNELS,
               "SimFW DMA budget exceeded: the CT wave synth and the two PIO "
               "SPI buses together claim more than the RP2040's "
               "NUM_DMA_CHANNELS channels. Every claim now halts loudly via "
               "simfw_fatal() on exhaustion (drivers/simfw_fatal.h) rather "
               "than degrading silently, but that is a bench-time halt -- "
               "catching the overrun here, at compile time, is still better. "
               "Free a channel (see docs/HARDWARE.md section 1b.4, the only "
               "slack that exists) or reduce a channel-count constant.");

// FreeRTOSConfig.h turns on configCHECK_FOR_STACK_OVERFLOW (2) and
// configUSE_MALLOC_FAILED_HOOK (1) unconditionally, matching SaftyFW's
// choice -- cheap insurance, and the kernel requires these two hooks to
// exist at link time regardless. Halting in place rather than resetting, so
// a debugger attached over SWD (this bench fixture's flashing/debug path,
// DESIGN_NOTES.md section 3.1) can see exactly where it happened instead of the
// failure hiding behind a watchdog-looking reboot.
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    (void)pcTaskName;
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

void vApplicationMallocFailedHook(void)
{
    taskDISABLE_INTERRUPTS();
    for (;;) {
    }
}

int main(void)
{
    // Harmless no-op unless a stdio backend is actually selected in
    // CMakeLists.txt (none is, today) -- same call SaftyFW's main() makes
    // at the equivalent point, kept here so bring-up order matches and so
    // it is a one-line change to enable a bench stdio path later.
    stdio_init_all();

    // Start every task, in priority order (highest first), matching
    // docs/DESIGN_NOTES.md section 4.1's table and SaftyFW's own main.c convention.
    // Each _start() function pins its own task's core affinity
    // (task_priorities.h's SIMFW_CORE_RT_PATH / SIMFW_CORE_ELASTIC_PATH) --
    // this file does not set affinity itself, so there is exactly one place
    // per task that can get it wrong, not two.
    //
    // No return value is checked here, but that is no longer "every task
    // body is just an idle loop with nothing that can fail beyond
    // xTaskCreate()" -- it stopped being true the moment wave_owner_start()
    // (ct_wave_pwm_init()) and spi_emu_a/b_start() (max31856_pio_engine_init())
    // started claiming DMA channels from a shared, exhaustible 12-channel
    // pool (docs/HARDWARE.md section 1b). What makes the (void) discards
    // below still defensible is that a DMA claim failure no longer returns
    // false up this chain at all: every dma_claim_unused_channel() call site
    // now halts loudly through drivers/simfw_fatal.h on exhaustion (see that
    // header, and ct_wave_pwm.c/max31856_pio_engine.c's call sites) instead
    // of degrading silently the way it used to. What a _start() function can
    // still fail on -- and what these (void)s still discard -- is a plain
    // FreeRTOS allocation failure (xTaskCreate()/xQueueCreate()/
    // xSemaphoreCreateMutex() under heap pressure), which remains
    // "logged-and-continued, nothing fatal" the way SaftyFW's own main()
    // documents, minus the logging until log_task is real (there is no log
    // sink running yet to report a failure to -- log_task itself is one of
    // the tasks being started). TODO: once log_task/telemetry have real
    // bodies, capture and report these.
    (void)spi_emu_a_start();
    (void)spi_emu_b_start();
    (void)wave_owner_start();
    (void)sim_engine_start();
    (void)usb_owner_start();
    (void)cmd_task_start();
    (void)fault_sched_start();
    (void)i2c_owner_start();
    (void)telemetry_start();
    (void)log_task_start();

    vTaskStartScheduler();

    // vTaskStartScheduler() does not return on success. Reaching here means
    // scheduler start itself failed (e.g. out of heap for the idle/timer
    // tasks).
    for (;;) {
        tight_loop_contents();
    }
}
