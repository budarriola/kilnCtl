// main.c -- SaftyFW entry point. Implements the boot sequence from
// docs/ARCHITECTURE.md section 5 in order; if this file and that section ever
// disagree, the doc is either stale (fix it in the same commit as any
// intentional change here) or this file has a bug.
//
//   1. GPIO6 -> output, driven LOW.        <- FIRST, before anything else
//   2. Watchdog enabled (1 s).
//   3. Read boot_reason; latch it.
//   4. Load config from flash; on CRC failure use safe defaults + set a flag.
//   5. Bring up spi_owner, adc_owner, uart_owner (non-blocking TX).
//   6. Probe the MAX31856. Failure is logged, not fatal.
//   7. Start tasks in priority order.
//   8. Enter GRACE for startup_grace_s (60 s): relay stays de-energized.
//   9. Enter ARMED. K4 may now be energized, iff no guard is tripped.
//
// This phase (TODO.md Phase 2, "Skeleton") implements steps 1-3 and 7 for
// real. Steps 4-6 and 8-9 are TODOs for Phases 3-9 -- see the TODO comments
// at each step below and TODO.md's own phase breakdown; this file does not
// pretend those phases are done.
#include "pico/stdlib.h"

#include "FreeRTOS.h"
#include "task.h"

#include "hardware/gpio.h"
#include "hardware/watchdog.h"

#include "board_pins.h"
#include "boot_reason.h"
#include "startup_diag.h"
#include "config_store.h"
#include "max31856.h"
#include "spi_owner.h"

#include "tasks/console_uart.h"
#include "tasks/current_task.h"
#include "tasks/discrete_task.h"
#include "tasks/link_task.h"
#include "tasks/log_task.h"
#include "tasks/relay_owner.h"
#include "tasks/safety_core.h"
#include "tasks/thermo_task.h"
#include "tasks/uart_owner.h"
#include "tasks/update_task.h"
#include "tasks/watchdog_task.h"

#define SAFTYFW_WATCHDOG_TIMEOUT_MS 1000

// FreeRTOSConfig.h turns on configCHECK_FOR_STACK_OVERFLOW (2) and
// configUSE_MALLOC_FAILED_HOOK (1) unconditionally -- cheap insurance in a
// safety-relevant build, per that file's own comment -- which means the
// kernel requires these two hooks to exist at link time. Halting in place
// (same policy as configASSERT, FreeRTOSConfig.h) rather than resetting:
// resetting would let the watchdog mask a stack overflow or a heap
// exhaustion as an ordinary-looking reboot, which is exactly the kind of
// silent failure ARCHITECTURE.md section 4's boot_reason latching exists to
// avoid for guard trips -- these two deserve the same "stop and let a
// debugger see it" treatment, not fewer eyes on them.
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
    // --- Step 1: GPIO6 low, before anything else touches the board. --------
    // The literal first statements of main(), ahead of even stdio/clock
    // bring-up. GPIO6 powers up as a floating input; a floating gate on Q4 is
    // an undefined coil state (docs/ARCHITECTURE.md section 5). Driving it
    // low first is the same discipline KilnFW applies to kiln_io_init's
    // relay-bits-before-direction ordering, and it is not a formality --
    // every millisecond between power-on and this line is a millisecond K4's
    // state is genuinely unknown.
    gpio_init(SAFTYFW_PIN_RELAY);
    gpio_put(SAFTYFW_PIN_RELAY, 0);      // set the output latch to LOW before...
    gpio_set_dir(SAFTYFW_PIN_RELAY, GPIO_OUT); // ...making the pin an output, so it is never briefly undriven-low/undriven-high

    // Console UART0 (GP16/GP17, debug probe bridge) as early as possible --
    // ahead of even stdio/watchdog/config, deliberately, so a hang or crash
    // anywhere below this line still leaves a boot banner on the wire. This
    // is the M0 liveness proof: a console that comes up before anything that
    // could go wrong has had a chance to. See console_uart.h and
    // docs/HARDWARE.md section 7b.
    console_uart_init();
    console_uart_puts("\r\n=== SaftyFW boot ===\r\n");

    // stdio/clocks: safe to bring up now that GPIO6 is settled. Needed for
    // SAFTYFW_ENABLE_USB_STDIO builds later (TODO.md Phase 2) and harmless
    // otherwise -- stdio_init_all() is a no-op if no stdio backend is
    // selected in CMakeLists.txt.
    stdio_init_all();

    // --- Step 2: hardware watchdog, 1 s. ------------------------------------
    // pause_on_debug = true: hardcoded for now -- there is no release/debug
    // build distinction yet in this CMake project. TODO: add a build option
    // (e.g. SAFTYFW_RELEASE_BUILD) and flip this to false for it.
    // docs/ARCHITECTURE.md section 8 is explicit that a release build which
    // pauses on debug can have its watchdog silently disabled by an attached
    // probe -- "the one component where that matters".
    watchdog_enable(SAFTYFW_WATCHDOG_TIMEOUT_MS, true);
    SAFTYFW_BOOT_STAGE(SAFTYFW_BOOT_STAGE_WATCHDOG_ARMED);

    // --- Step 3: read boot_reason; latch it. --------------------------------
    // watchdog_caused_reboot()/watchdog_enable_caused_reboot() read
    // watchdog_hw->scratch[4], which watchdog_enable() above just wrote --
    // that write is a constant marker (not a per-boot value), so reading
    // these after enabling is equivalent to reading them before; what matters
    // is that watchdog_hw->reason (a separate, HW-latched register) was set
    // before any of this code ran. See src/boot_reason.c's header comment for
    // why scratch[0]/[1] (this firmware's own trip-reason latch) are
    // deliberately NOT scratch[4].
    saftyfw_boot_reason_t boot_reason =
        boot_reason_read(watchdog_caused_reboot(), watchdog_enable_caused_reboot());

    // TODO (Phase 8): report boot_reason (including, if trip_reason_valid,
    // "watchdog reset while tripped on <reason>") in the diagnostic frame --
    // ARCHITECTURE.md section 4: "A safety processor silently
    // watchdog-looping presents as a working system with mysterious
    // intermittent trips -- that bit is how you find it." Until log_task/
    // link_task have real bodies (Phase 7/8) there is nowhere to report it
    // to, so it is read and preserved here but not yet surfaced.
    (void)boot_reason;

    // Once this boot has read and (eventually) reported the latched trip
    // reason, clear it so an unrelated future reset does not re-report a
    // stale one. Safe to do unconditionally: boot_reason above already holds
    // whatever this boot needs.
    boot_reason_clear_trip();

    // --- Step 4: config from flash. ------------------------------------------
    // config_store_boot_load() (Phase 9) reads the config store's flash
    // sector (bootloader/flash_layout.h's SAFTYFW_CONFIG_STORE_FLASH_OFFSET/
    // _SIZE) once, here, before anything below reads config_store_get_tc_type()/
    // config_store_is_calibration_missing() -- see config_store_flash.c's
    // header comment. On a blank/corrupt sector this leaves the cache holding
    // config_store_default() (tc_type = K, calibration_missing = true), same
    // "a missing part must not abort boot" contract as step 6 below.
    // Still TODO (Phase 9, later bullets): S8's threshold and current-sense
    // calibration constants have reserved room in config_store_record_t but
    // nothing reads/writes them yet, and there is no wire command
    // (SAFTY_CMD_SET_CONFIG or similar) to actually set any of this -- see
    // config_store.h's header comment.
    config_store_boot_load();
    SAFTYFW_BOOT_STAGE(SAFTYFW_BOOT_STAGE_CONFIG_LOADED);

    // --- Step 5: spi_owner, adc_owner, uart_owner bring-up. -------------------
    // spi_owner (SPI0 + CS0, Phase 3) is real as of this pass -- see
    // spi_owner.h for why it is a plain init-then-call module rather than a
    // request-queue task (THERMOCOUPLE.md section 1: this bus has exactly
    // one device, so there is no contention to serialize). adc_owner's
    // round-robin init remains TODO (Phase 6 -- current_task_start() below
    // does call adc_init()/adc_gpio_init() itself for now). uart_owner (Phase
    // 7) is real as of this pass: UART1 (GPIO4/5), non-blocking TX ring, IRQ-
    // driven RX ring -- must be brought up before link_task_start() (step 7),
    // which assumes both rings already exist.
    //
    // docs/ARCHITECTURE.md section 5 says uart_owner is the one bring-up
    // whose failure should abort the boot ("Only uart_owner failing to
    // initialise aborts the boot; everything else is logged and stepped
    // over"). uart_owner_init() cannot actually fail on this hardware today
    // (see its header comment), so there is no real abort path to wire yet --
    // its return value is still checked and logged-and-continued for now,
    // matching every other bring-up step here, rather than pretending a
    // fatal path exists before there is a log sink (Phase 8) to report
    // through.
    bool spi_owner_ok = spi_owner_init();
    SAFTYFW_BOOT_STAGE(SAFTYFW_BOOT_STAGE_SPI_UP);
    bool uart_owner_ok = uart_owner_init();
    SAFTYFW_BOOT_STAGE(SAFTYFW_BOOT_STAGE_UART_UP);
    (void)uart_owner_ok; // no log sink yet (Phase 8) to report this to

    // --- Step 6: probe the MAX31856. ------------------------------------------
    // max31856_init() brings up the GPIOs (CS1/FAULT11); max31856_configure()
    // does the real CR0/CR1/MASK write per docs/THERMOCOUPLE.md section 5's
    // table and doubles as the "probe" -- every register write it issues is
    // also a transfer that must succeed for the return value to be true, so
    // a part that is missing or not responding on SPI0 is caught here as a
    // logged, non-fatal failure (docs/ARCHITECTURE.md section 5, step 6:
    // "Failure is logged, not fatal -- S5 handles a blind processor;
    // refusing to boot would leave the E-stop and current guards unserved
    // too"). thermo_task keeps running either way: with no successful
    // configure, every max31856_read() from thermo_task will fail closed
    // (spi_failed = true), which is exactly the honest "sensor invalid"
    // state S5 is built to catch.
    //
    // tc_type: config_store_get_tc_type() (Phase 9) -- reads the cache
    // config_store_boot_load() (step 4, above) already populated, real or
    // safe-default K. This is a real per-installation commissioning read
    // now, not a hard-coded placeholder -- but there is still no
    // commissioning UI/wire command that ever WRITES a non-default tc_type
    // (see config_store.h's header comment), so on every board built so far
    // this still evaluates to K in practice, honestly, via the same safe-
    // default path as a blank sector rather than a special-cased constant.
    bool max31856_ok = spi_owner_ok && max31856_init(SAFTYFW_PIN_SPI0_CS0,
                                                       SAFTYFW_PIN_THERMO_FAULT) &&
                        max31856_configure(config_store_get_tc_type());
    (void)max31856_ok; // no log sink yet (Phase 8) to report this to; the
                        // failure still surfaces at runtime via S5, see above.

    // --- Step 7: start tasks, in priority order (highest first). -------------
    // Matches docs/ARCHITECTURE.md section 4's table top-to-bottom. Each
    // _start() function pins its own task's core affinity
    // (task_priorities.h's SAFTYFW_CORE_TRIP_PATH / SAFTYFW_CORE_LINK_PATH) --
    // this file does not set affinity itself, so there is exactly one place
    // per task that can get it wrong, not two.
    //
    // Every failure here is latched-and-continued rather than fatal, matching
    // firmware/KilnFW/App/main.c's "only two failures abort app_main"
    // convention (here: none do, since there is no PC-equivalent link this
    // phase whose absence would leave every other subsystem unreachable --
    // that is Phase 7's link_task, and even it does not gate anything else).
    // TODO: once log_task has a real sink, also emit these as a logged
    // warning rather than only as the scratch-register latch below.
    // Start results are LATCHED, not discarded. A task that fails to create
    // never sets its watchdog check-in bit; s_checkin_mask can then never
    // equal WATCHDOG_CHECKIN_ALL_MASK, watchdog_task_fn() never feeds, and
    // the board reboots every second with nothing anywhere to say which task
    // was missing -- exactly how the 2026-08-21 update_task regression
    // presented, and how the same symptom presented again on the bench
    // 2026-08-23. Discarding these returns is what made a one-line bug cost
    // two debugging sessions.
    //
    // The latch is a watchdog scratch register rather than a static, because
    // it has to survive the very reset it explains: RAM is re-zeroed every
    // boot, this board has no log sink yet (Phase 8), and GP16/GP17's console
    // header is not fitted (TODO.md 0.5a), so a plain variable would be
    // unreadable in exactly the reboot loop it exists to diagnose. Read it
    // over SWD at SAFTYFW_STARTUP_DIAG_SCRATCH, guarded by the magic word in
    // SAFTYFW_STARTUP_DIAG_MAGIC_SCRATCH.
    SAFTYFW_BOOT_STAGE(SAFTYFW_BOOT_STAGE_THERMO_PROBED);

    uint32_t start_failures = 0u;
#define SAFTYFW_START_TASK(bit, call)        \
    do {                                     \
        if (!(call)) {                       \
            start_failures |= (1u << (bit)); \
        }                                    \
    } while (0)

    SAFTYFW_START_TASK(WATCHDOG_CHECKIN_RELAY_OWNER, relay_owner_start());
    SAFTYFW_START_TASK(SAFTYFW_START_BIT_WATCHDOG_TASK, watchdog_task_start());
    SAFTYFW_START_TASK(WATCHDOG_CHECKIN_SAFETY_CORE, safety_core_start());
    SAFTYFW_START_TASK(WATCHDOG_CHECKIN_DISCRETE_TASK, discrete_task_start());
    SAFTYFW_START_TASK(WATCHDOG_CHECKIN_THERMO_TASK, thermo_task_start());
    SAFTYFW_START_TASK(WATCHDOG_CHECKIN_CURRENT_TASK, current_task_start());
    SAFTYFW_START_TASK(WATCHDOG_CHECKIN_LINK_TASK, link_task_start());
    SAFTYFW_START_TASK(WATCHDOG_CHECKIN_LOG_TASK, log_task_start());
    // update_task registered a WATCHDOG_CHECKIN_UPDATE_TASK bit (Phase 10)
    // but was never actually started here -- that bit could then never be
    // set, s_checkin_mask could never equal WATCHDOG_CHECKIN_ALL_MASK, and
    // watchdog_task_fn() (watchdog_task.c) would never call watchdog_update(),
    // so the 1 s hardware watchdog fired forever. Root cause of the bench
    // reset loop found 2026-08-21; see ROADMAP.md.
    SAFTYFW_START_TASK(WATCHDOG_CHECKIN_UPDATE_TASK, update_task_start());
#undef SAFTYFW_START_TASK

    SAFTYFW_BOOT_STAGE(SAFTYFW_BOOT_STAGE_TASKS_STARTED);
    watchdog_hw->scratch[SAFTYFW_STARTUP_DIAG_SCRATCH] = start_failures;
    watchdog_hw->scratch[SAFTYFW_STARTUP_DIAG_MAGIC_SCRATCH] =
        SAFTYFW_STARTUP_DIAG_MAGIC;

    console_uart_puts("SaftyFW: tasks started, entering scheduler\r\n");

    // --- Step 8/9: GRACE -> ARMED. ---------------------------------------------
    // Built (Phase 5, 2026-08-16): relay_owner's own INIT/GRACE/ARMED/
    // TRIPPED state machine (src/tasks/relay_owner.c). No separate call is
    // made from here to "enter grace" -- relay_owner_start() above (step 7)
    // is enough. The GRACE timer starts the instant relay_owner_task()
    // itself begins running, which is effectively the same moment step 7
    // finishes (relay_owner is the highest-priority task in the system and
    // the first one started), so "start tasks" and "enter GRACE" collapse
    // into the same step in practice without main.c having to sequence them
    // itself. See relay_owner.c's header comment for the full reasoning.
    // GRACE -> ARMED then happens automatically, 60 s later, inside that
    // task's own loop -- nothing here drives it.

    SAFTYFW_BOOT_STAGE(SAFTYFW_BOOT_STAGE_SCHEDULER_ENTERED);
    vTaskStartScheduler();

    // vTaskStartScheduler() does not return on success. Reaching here means
    // scheduler start itself failed (e.g. out of heap for the idle/timer
    // tasks) -- GPIO6 is still LOW from step 1 and nothing above ever
    // commanded it otherwise, so the board is left in its safe state even
    // though nothing is running to keep it there deliberately.
    for (;;) {
        tight_loop_contents();
    }
}
