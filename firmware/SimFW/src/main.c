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

#include "drivers/ct_wave_i2s.h"
#include "drivers/simfw_fatal.h"

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
// and each bus's chip count is one `dma_sniff[i]` per emulated chip.
// CT_WAVE_I2S_NUM_MODULES (2, one DMA channel per UDA1334A module) replaces
// the retired ct_wave_pwm.c's CT_WAVE_PWM_NUM_CHANNELS (3) here, per
// docs/DESIGN_NOTES.md section 3.3's 2026-08-23 PWM->I2S decision -- the DMA
// count went down even though ct_wave_i2s.c cannot actually run yet
// (docs/HARDWARE.md section 1b.7's PIO1 program-memory blocker is a separate
// budget, not this one). If a third DMA claimant is ever added, add its term
// here AND to section 1b's table AND to check_single_owner.ps1's
// $dmaBudgetTerms -- all three, in the same commit.
#define SIMFW_DMA_CHANNELS_CLAIMED                                            \
    (CT_WAVE_I2S_NUM_MODULES + (SPI_EMU_A_CHANNEL_COUNT + 2u) +              \
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
//
// BOTH HOOKS NOW ROUTE THROUGH simfw_fatal() -- they used to just
// taskDISABLE_INTERRUPTS() and spin, which is INDISTINGUISHABLE AT THE BENCH
// from a healthy-but-quiet fixture: no LED, no USB, nothing. That silence is
// exactly what let telemetry_task's real stack overflow (see this task's
// bench report: telemetry_build_and_send_state_frame() alone measured 10736
// bytes of stack against a 1024-byte task stack, an order of magnitude over)
// go undiagnosable from a dark board with no debug probe attached. A hook
// that never lights the LED turns "the kernel caught a real bug" into "the
// bench operator has no idea whether the fixture is dead or just idle."
void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    simfw_fatal("freertos", "stack overflow in task \"%s\"", pcTaskName ? pcTaskName : "?");
}

void vApplicationMallocFailedHook(void)
{
    simfw_fatal("freertos", "heap allocation failed (configTOTAL_HEAP_SIZE exhausted)");
}

// --- Boot-stage beacon -------------------------------------------------------
// GPIO25 (onboard LED) -- see simfw_fatal.c's header for why this pin has no
// owner until now and why a heartbeat/beacon must yield it the instant
// simfw_fatal() is entered rather than fight for it. This beacon and
// simfw_fatal() never actually contend for the pin in practice: this
// function only ever runs pre-scheduler, in main()'s own straight-line call
// sequence on core 0, and simfw_fatal() halts whichever core called it
// (disabling interrupts, looping forever) rather than returning -- so a
// fatal firing mid-stage simply never returns control to finish that
// stage's blink group, and no two writers are ever mid-sequence on GPIO25 at
// once. This is the "distinct, countable blink pattern per stage" the bench
// operator needs with no debug probe attached (see
// docs/BENCH_RUNBOOK.md's decode table) -- added after a real bench boot
// failure (dark LED, no USB) turned out to be a task stack overflow with
// zero visible signal, see vApplicationStackOverflowHook() above.
//
// Deliberately NOT using pico-sdk's sleep_ms()/busy_wait_ms() distinction
// here beyond picking busy_wait_ms(): no FreeRTOS tick is running yet
// pre-scheduler, so sleep_ms() (which can rely on the SDK's own low-power
// wait) is fine too, but busy_wait_ms() is the same primitive
// simfw_fatal.c's own blink burst already uses -- one busy-wait convention
// for every pre-scheduler LED sequence in this file, not two.
#define SIMFW_BOOT_BEACON_LED_GPIO      25u // same physical LED as simfw_fatal.c's SIMFW_FATAL_LED_GPIO
#define SIMFW_BOOT_BEACON_FLASH_ON_MS   150u
#define SIMFW_BOOT_BEACON_FLASH_OFF_MS  150u
#define SIMFW_BOOT_BEACON_GROUP_GAP_MS  700u // long dark gap between stage groups -- lets a human count flashes reliably

static void simfw_boot_beacon(uint32_t stage_number)
{
    for (uint32_t i = 0; i < stage_number; i++) {
        gpio_put(SIMFW_BOOT_BEACON_LED_GPIO, 1);
        busy_wait_ms(SIMFW_BOOT_BEACON_FLASH_ON_MS);
        gpio_put(SIMFW_BOOT_BEACON_LED_GPIO, 0);
        busy_wait_ms(SIMFW_BOOT_BEACON_FLASH_OFF_MS);
    }
    busy_wait_ms(SIMFW_BOOT_BEACON_GROUP_GAP_MS);
}

// --- Post-scheduler heartbeat -------------------------------------------------
// configUSE_IDLE_HOOK is on (FreeRTOSConfig.h) solely for this: a slow,
// unmistakable "the scheduler is alive and core 0 is idling normally"
// signal, so a healthy fixture's final LED state is never just "dark" (which
// this task's bench report shows is otherwise indistinguishable from a
// silently hung fixture -- see vApplicationStackOverflowHook()'s comment).
// One brief 100 ms flash every ~2 s: short and infrequent enough that it
// cannot be mistaken for a boot-stage group (150/150 ms flashes clustered
// back-to-back, this task's own SIMFW_BOOT_BEACON_* timing) or for
// simfw_fatal()'s 10x 100/100 ms burst-then-solid-on. Runs only on core 0's
// idle task (configUSE_PASSIVE_IDLE_HOOK is 0, configIDLE_AFFINITY is 0) --
// core 1 never calls this. Non-blocking (time_us_64()-gated, not a sleep) so
// it never delays the idle task's own tickless-idle/yield bookkeeping.
// Naturally stops the instant simfw_fatal() disables interrupts and loops
// forever: the idle task simply never runs again to call this, so there is
// no explicit hand-off needed, matching simfw_fatal.c's "yield the pin"
// requirement by construction rather than by an added flag.
#define SIMFW_HEARTBEAT_PERIOD_MS 2000u
#define SIMFW_HEARTBEAT_ON_MS     100u

void vApplicationIdleHook(void)
{
    uint32_t phase = (uint32_t)(time_us_64() / 1000u) % SIMFW_HEARTBEAT_PERIOD_MS;
    gpio_put(SIMFW_BOOT_BEACON_LED_GPIO, phase < SIMFW_HEARTBEAT_ON_MS ? 1 : 0);
}

int main(void)
{
    // Harmless no-op unless a stdio backend is actually selected in
    // CMakeLists.txt (none is, today) -- same call SaftyFW's main() makes
    // at the equivalent point, kept here so bring-up order matches and so
    // it is a one-line change to enable a bench stdio path later.
    stdio_init_all();

    // MUST run before vTaskStartScheduler() and before any core-1 task can
    // possibly fire simfw_fatal() -- see simfw_fatal.h's "CROSS-CORE HALT"
    // comment. Registers the SIO_IRQ_PROC0 handler that lets a core-1 fatal
    // (spi_emu_a/b's DMA/PIO claims, SIMFW_CORE_RT_PATH) also halt core 0,
    // so the fixture cannot keep answering USB/telemetry as if healthy while
    // one bus is silently dead. Core 0 is the only core running at this
    // point in boot (the FreeRTOS RP2040 SMP port launches core 1 lazily,
    // inside vTaskStartScheduler() below), so irq_set_exclusive_handler()/
    // irq_set_enabled() are guaranteed to bind to core 0's own NVIC here.
    simfw_fatal_install_cross_core_halt();

    // Boot-stage beacon init -- see simfw_boot_beacon()'s comment above.
    // gpio_init()/gpio_set_dir() here, ONCE, is deliberately the same pattern
    // simfw_fatal()'s own first two lines use for this pin; harmless to
    // "re-init" if simfw_fatal() ever runs first (it does not touch this
    // path -- both are core-0-only pre-scheduler code, sequential, never
    // concurrent).
    gpio_init(SIMFW_BOOT_BEACON_LED_GPIO);
    gpio_set_dir(SIMFW_BOOT_BEACON_LED_GPIO, GPIO_OUT);

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
    // (ct_wave_i2s_init()) and spi_emu_a/b_start() (max31856_pio_engine_init())
    // started claiming DMA/PIO resources from shared, exhaustible pools
    // (docs/HARDWARE.md section 1b). What makes the (void) discards
    // below still defensible is that a claim failure no longer returns
    // false up this chain at all: every dma_claim_unused_channel() call site
    // now halts loudly through drivers/simfw_fatal.h on exhaustion (see that
    // header, and ct_wave_i2s.c/max31856_pio_engine.c's call sites) instead
    // of degrading silently the way it used to -- ct_wave_i2s_init() is in
    // fact EXPECTED to reach simfw_fatal() on the current build, since PIO1
    // does not have the 8 free program-memory words it needs (section
    // 1b.7); the fixture halts loudly at boot rather than running with a
    // silently-dead CT path. What a _start() function can
    // still fail on -- and what these (void)s still discard -- is a plain
    // FreeRTOS allocation failure (xTaskCreate()/xQueueCreate()/
    // xSemaphoreCreateMutex() under heap pressure), which remains
    // "logged-and-continued, nothing fatal" the way SaftyFW's own main()
    // documents, minus the logging until log_task is real (there is no log
    // sink running yet to report a failure to -- log_task itself is one of
    // the tasks being started). TODO: once log_task/telemetry have real
    // bodies, capture and report these.
    (void)spi_emu_a_start();
    simfw_boot_beacon(1);
    (void)spi_emu_b_start();
    simfw_boot_beacon(2);
    (void)wave_owner_start();
    simfw_boot_beacon(3);
    (void)sim_engine_start();
    simfw_boot_beacon(4);
    (void)usb_owner_start();
    simfw_boot_beacon(5);
    (void)cmd_task_start();
    simfw_boot_beacon(6);
    (void)fault_sched_start();
    simfw_boot_beacon(7);
    (void)i2c_owner_start();
    simfw_boot_beacon(8);
    (void)telemetry_start();
    simfw_boot_beacon(9);
    (void)log_task_start();
    simfw_boot_beacon(10);

    vTaskStartScheduler();

    // vTaskStartScheduler() does not return on success. Reaching here means
    // scheduler start itself failed (e.g. out of heap for the idle/timer
    // tasks).
    for (;;) {
        tight_loop_contents();
    }
}
