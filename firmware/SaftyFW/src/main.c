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
#include "hardware/regs/timer.h" // TIMER_DBGPAUSE_BITS, see step 1b below
#include "hardware/timer.h" // timer_hw->dbgpause, see step 1b below
#include "hardware/watchdog.h"

#include "board_pins.h"
#include "boot_reason.h"
#include "hal_scratch.h" // HAL Phase 3 item 1 -- claims for every real scratch owner, see the block in main() below
#include "clear_trip_diag.h" // 2026-08-23 round 4, CLEAR_TRIP crash checkpoints, see its own header comment
#include "watchdog_overdue_diag.h" // 2026-08-23, the CLEAR_TRIP investigation's actual conclusion, see its own header comment
#include "startup_diag.h"
#include "config_store.h"
#include "max31856.h"
/* spi_owner.h's direct include is gone -- HAL Phase 1b routed the SPI0
 * bring-up used below through max31856_bus_init() (max31856.h), which
 * itself goes through interface/hal_spi.h / pico/spi/hal_spi_pico.c over
 * the same spi_owner.c body. */

#include "tasks/console_uart.h"
#include "tasks/current_task.h"
#include "tasks/discrete_task.h"
#include "tasks/link_task.h"
#include "tasks/log_task.h"
#include "tasks/relay_owner.h"
#include "tasks/safety_core.h"
#include "tasks/thermo_task.h"
#include "hal_uart_pico_internal.h"  // HAL Phase 1a: was tasks/uart_owner.h; moved+renamed to firmware/hwAbstraction/pico/uart/hal_uart_pico_internal.h
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

    // DO NOT REMOVE THE WRITE BELOW AS DEAD WEIGHT. It is not decoration --
    // it is the ONLY reason the CLEAR_TRIP-reboots-the-Pico investigation
    // was ever solvable. Before it existed, a stack overflow on this
    // firmware was bit-for-bit indistinguishable from any other watchdog
    // reset: this hook disables interrupts and hangs (below), so
    // watchdog_task never runs again to leave its own diagnostic, and the
    // reboot that follows carries no evidence of what actually happened.
    // Two live suspects (safety_core_task, then log_task) were each
    // eliminated or confirmed ONLY once this write existed to name the
    // real one on the next boot. Removing it returns this firmware to that
    // blind state.
    //
    // 2026-08-23, the CLEAR_TRIP-reboots-the-Pico investigation's actual
    // mechanism: this hook disables interrupts and hangs, deliberately (see
    // the comment above), which means watchdog_task never runs again, the
    // unfed hardware watchdog resets the chip about a second later, and
    // -- until this write existed -- NO diagnostic of any kind survived:
    // scratch[5]'s overdue-checkin latch read magic_ok == false after a
    // reproduced crash, proving watchdog_task_fn() never even reached the
    // branch that would have written it. A stack overflow on this firmware
    // was, until now, indistinguishable from any other watchdog reset.
    //
    // This write closes that gap. It shares watchdog_hw->scratch[5] with
    // watchdog_overdue_diag_codec.c's own overdue-checkin format, tagged
    // with a DIFFERENT magic byte (0xE3 vs that format's 0xD9) so the two
    // never collide -- see watchdog_overflow_diag_t's own doc comment
    // (watchdog_overdue_diag_codec.h) for why sharing one register is
    // correct: a stack overflow's interrupt-disable is exactly what
    // prevents watchdog_task from ever reaching its own write, so the two
    // events are mutually exclusive by construction.
    //
    // Deliberately NOT calling into watchdog_overdue_diag.c/_codec.c's own
    // functions: this hook may be running moments after the very stack
    // such a call would need has overflowed, so nothing beyond a plain MMIO
    // write is safe here -- no snprintf, no strlen, no function call beyond
    // this one register write. pcTaskName[0]/[1] are safe to read
    // regardless: FreeRTOS points pcTaskName at the task's own registered
    // name, a static string literal in .rodata (see every xTaskCreate()
    // call site in this codebase), never the corrupted stack itself. Two
    // bytes is enough to disambiguate every task this firmware currently
    // registers (current_task/discrete_task/link_task/log_task/
    // relay_owner/safety_core/thermo_task/update_task/watchdog_task all
    // differ in their first two characters) --
    // test_two_name_bytes_disambiguate_every_current_task in
    // test_watchdog_overdue_diag_codec.c is the regression guard for that
    // fact holding as tasks are added.
    //
    // The bit layout (magic byte in [31:24], name bytes in [23:16]/[15:8])
    // is kept in exact, hand-maintained sync with
    // watchdog_overflow_diag_decode() (watchdog_overdue_diag_codec.c) --
    // there is deliberately no shared encoder, for the same "cannot safely
    // call into another compilation unit here" reason.
    uint8_t name0 = 0u;
    uint8_t name1 = 0u;
    if (pcTaskName != NULL) {
        name0 = (uint8_t)pcTaskName[0];
        if (name0 != 0u) {
            name1 = (uint8_t)pcTaskName[1];
        }
    }
    // Deliberately a raw watchdog_hw->scratch[] write, NOT
    // hal_scratch_write_u32() -- see this hook's own comment above on why
    // no function call beyond this one register write is safe here. Slot 5
    // tag 0xE3 is still claimed (main(), before the scheduler starts) so
    // hal_scratch's runtime registry records this hook as slot 5's second
    // legitimate co-owner; the claim call and this write are two different
    // moments in two different execution contexts.
    watchdog_hw->scratch[5] = (0xE3u << 24) | ((uint32_t)name0 << 16) | ((uint32_t)name1 << 8);

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

    // --- Step 1b: un-pause the hardware TIMER peripheral for debug halts. --
    // 2026-08-23, the DIAG-content-frozen investigation's actual root cause:
    // TIMER_DBGPAUSE (timer_hw->dbgpause, a register belonging to the TIMER
    // peripheral itself -- NOT watchdog_hw->ctrl's PAUSE_DBG0/DBG1/JTAG bits,
    // which only gate the watchdog's own countdown) resets to 0x7 on this
    // silicon: both DBG0 and DBG1 set, meaning the shared timer counter that
    // to_ms_since_boot(get_absolute_time())/time_us_64() read PAUSES the
    // instant either core is halted by a debugger. Confirmed on the bench:
    // dbgpause read 0x7, TIMERAWL held dead flat (507171us, matching the
    // frozen uptime_ms=507 every DIAG frame reported) across an 8s window,
    // reproduced from a clean flash+run with zero SWD contact afterward --
    // not an artifact of any particular read, an artifact of core 1 coming
    // up (multicore_launch_core1(), inside vTaskStartScheduler() below)
    // while a probe is attached and DBG1 is still at its power-on default.
    // Meanwhile xTaskGetTickCount() stayed healthy throughout, because
    // configTICK_CORE pins the FreeRTOS tick to core 0's own SysTick, a
    // core-local peripheral this register has no effect on -- so the board
    // looked completely alive on every FreeRTOS-tick-paced check while every
    // get_absolute_time()-based timestamp on it silently stopped.
    //
    // This firmware has never configured TIMER_DBGPAUSE (grepped the full
    // git history: never referenced before this line) -- the freeze is
    // RP2040 silicon default behaviour, not a regression, and it predates
    // this fix being visible only because DIAG frames never reached the ESP
    // before 2026-08-23's separate UART self-start-failure fix (uart_owner.c)
    // let anyone actually read a frozen uptime_ms in the first place.
    //
    // THE TRADE-OFF, stated on purpose rather than left implicit: clearing
    // this means get_absolute_time()/time_us_64() keep advancing even while
    // a core is halted mid-debug-session, instead of freezing in step with
    // it. That is a real loss for anyone who wanted debug-correlated timer
    // pausing (e.g. "does this ISR still fire while I'm single-stepping the
    // other core" style debugging). The trade is taken anyway: a debug probe
    // is attached to this board essentially continuously (JTAG/SWD is the
    // only sanctioned flashing path here, per docs/HARDWARE.md), so a timer
    // that silently stops whenever a core halts means every wall-clock
    // timestamp and every clock-based staleness check on a SAFETY processor
    // is unreliable during exactly the situations most likely to have a
    // probe attached -- worth more than debug-correlated pausing, which
    // nothing in this project depends on. safety_core.c's own clock_health.c
    // (added the same pass) is the belt to this fix's suspenders: it detects
    // a stalled get_absolute_time() explicitly and fails the two clock-based
    // staleness checks (context_valid, reboot_grace_active) closed even if
    // this register write is ever lost (a future SDK update, a differently
    // configured probe, RP2350 hardware with its own TICKS block, etc.) --
    // this fix should not be the ONLY thing standing between a stalled clock
    // and a guard that silently passes.
    //
    // Must run before vTaskStartScheduler() (step 8, below) brings up core 1
    // -- the whole point is that DBG1's pause-on-halt is already live the
    // instant core 1 exists to be halted.
    hw_clear_bits(&timer_hw->dbgpause, TIMER_DBGPAUSE_BITS);

    // --- hal_scratch claims for every real watchdog-scratch owner. ---------
    // HAL Phase 3 item 1 (docs/HW_ABSTRACTION_PLAN.md "hal_scratch -- pico
    // watchdog-scratch registry"). Centralized here, once, before the first
    // real access (SAFTYFW_BOOT_STAGE() immediately below writes slot 6),
    // rather than one claim call per accessor module -- this is the single
    // list a future reviewer checks against the slot map in
    // startup_diag.h/hal_scratch.h's own header comments. Slot 4 is never
    // claimed here: hal_scratch_claim() refuses it outright, it belongs to
    // pico-sdk's watchdog_enable() below. Slot 5 gets TWO claims, same slot,
    // distinct tags -- its two legitimate, mutually-exclusive co-owners
    // (watchdog_overdue_diag.c's normal-operation latch and this file's own
    // vApplicationStackOverflowHook() above, which stays a raw MMIO write
    // for the reasons documented on that hook -- claiming it here is pure
    // bookkeeping, unrelated to how it actually writes). Return values are
    // intentionally ignored: a collision here is a build-time-invariant
    // programming error (two modules claiming the same (slot, tag)), not a
    // runtime condition this boot path needs to react to, and the registry
    // is advisory only (see hal_scratch.h's own "REGISTRY / UNIQUENESS"
    // note) -- nothing downstream consults these claims to gate a read or
    // write.
    (void)hal_scratch_claim(0u, "boot_reason", HAL_SCRATCH_TAG_NONE);
    (void)hal_scratch_claim(1u, "boot_reason", HAL_SCRATCH_TAG_NONE);
    (void)hal_scratch_claim(2u, "startup_diag", HAL_SCRATCH_TAG_NONE);
    (void)hal_scratch_claim(3u, "startup_diag", HAL_SCRATCH_TAG_NONE);
    (void)hal_scratch_claim(5u, "watchdog_overdue_diag", 0xD9u);
    (void)hal_scratch_claim(5u, "stack_overflow_hook", 0xE3u);
    (void)hal_scratch_claim(6u, "boot_stage", HAL_SCRATCH_TAG_NONE);
    (void)hal_scratch_claim(7u, "clear_trip_diag", HAL_SCRATCH_TAG_NONE);

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

    // --- Step 3b: read the CLEAR_TRIP crash checkpoint; clear it. -----------
    // 2026-08-23 round 4 of the CLEAR_TRIP-reboots-the-Pico investigation:
    // same shape as step 3 immediately above (read, then clear, before
    // anything else can touch the register), deliberately a SEPARATE call
    // pair rather than folded into boot_reason_read()/boot_reason_clear_trip()
    // -- this diagnostic must never risk the load-bearing trip-reason latch
    // those two functions own. See clear_trip_diag.h's own header comment
    // for the scratch-register budget and why this needed its own module
    // (watchdog_hw->scratch[7] is the one free register; boot_reason.c's
    // own [0]/[1] pair was never available to reuse).
    clear_trip_diag_t clear_trip_diag = clear_trip_diag_read();

    // TODO: surface clear_trip_diag in the DIAG frame (link_task.c,
    // SAFETY_CMD_DIAG) now that DIAG reliably reaches the ESP again
    // (2026-08-23's TIMER_DBGPAUSE fix) -- not done this pass; read and
    // preserved here, same "not yet surfaced" state boot_reason was left in
    // above, SWD-readable via clear_trip_diag_get_cached() in the meantime.
    (void)clear_trip_diag;

    clear_trip_diag_clear();

    // --- Step 3c: read the overdue-checkin latch; clear it. -----------------
    // 2026-08-23, the CLEAR_TRIP-reboots-the-Pico investigation's actual
    // conclusion: same read-then-clear shape as steps 3/3b immediately
    // above. Unlike those two, this scratch word (repurposed
    // SAFTYFW_LAST_CHECKIN_MASK_SCRATCH, watchdog_hw->scratch[5]) is not
    // guaranteed to hold anything from THIS reboot -- it is only ever
    // written when watchdog_task_fn() actually withholds a feed, so most
    // boots will read magic_ok == false here, correctly. See
    // watchdog_overdue_diag.h's own header comment for why repurposing this
    // specific register was safe.
    watchdog_overdue_diag_t watchdog_overdue_diag = watchdog_overdue_diag_read();

    // 2026-08-23, round 2: the sibling event sharing this same register --
    // see watchdog_overflow_diag_t's own doc comment
    // (watchdog_overdue_diag_codec.h) and vApplicationStackOverflowHook()'s
    // own comment above for the full mechanism. Read BEFORE the clear
    // below, same as watchdog_overdue_diag itself -- at most one of the two
    // will ever report magic_ok == true for a given boot.
    watchdog_overflow_diag_t watchdog_overflow_diag = watchdog_overflow_diag_read();

    // TODO: surface watchdog_overdue_diag/watchdog_overflow_diag in the
    // DIAG frame (link_task.c, SAFETY_CMD_DIAG), same not-yet-surfaced
    // state as boot_reason/clear_trip_diag above -- SWD-readable via
    // watchdog_overdue_diag_get_cached()/watchdog_overflow_diag_get_cached()
    // in the meantime.
    (void)watchdog_overdue_diag;
    (void)watchdog_overflow_diag;

    watchdog_overdue_diag_clear(); // clears the one shared register regardless of which format was present, if either

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
    // does call hal_adc_init()/hal_adc_gpio_enable() itself for now, HAL
    // Phase 1b). uart_owner (Phase
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
    // HAL Phase 1b: max31856_bus_init() (max31856.h) now does what
    // spi_owner_init() used to do directly here -- it brings up the same
    // spi_owner.c SPI0 singleton through interface/hal_spi.h's
    // hal_spi_bus_init()/hal_spi_device_attach(), backed by
    // pico/spi/hal_spi_pico.c. Byte-identical timing/retry behavior.
    bool spi_owner_ok = max31856_bus_init();
    SAFTYFW_BOOT_STAGE(SAFTYFW_BOOT_STAGE_SPI_UP);
    // HAL Phase 1b, "close the upward include": uart_owner_init() no longer
    // reads board_pins.h itself -- main.c (which already includes it, for
    // the E-stop/relay pins above) passes SAFTYFW_PIN_UART1_{TX,RX} in
    // explicitly instead.
    const uart_owner_pins_t uart_owner_pins = {
        .tx_pin = SAFTYFW_PIN_UART1_TX,
        .rx_pin = SAFTYFW_PIN_UART1_RX,
    };
    bool uart_owner_ok = uart_owner_init(&uart_owner_pins);
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
    (void)hal_scratch_write_u32(SAFTYFW_STARTUP_DIAG_SCRATCH, start_failures);
    (void)hal_scratch_write_u32(SAFTYFW_STARTUP_DIAG_MAGIC_SCRATCH,
                                 SAFTYFW_STARTUP_DIAG_MAGIC);

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
