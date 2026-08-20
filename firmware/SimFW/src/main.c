// main.c -- SimFW entry point. Build/task skeleton only: creates and
// schedules the ten single-owner tasks from docs/PLAN.md section 4.1's task
// map, each currently an idling stub (src/tasks/*.c). No peripheral is
// actually driven yet -- PIO SPI emulation, CT waveform synthesis, I2C/
// expander logic and USB protocol handling are later work, filled into
// these same task bodies without touching this file or any sibling task
// file (single-owner-per-peripheral doctrine, PLAN.md section 4's opening
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

// FreeRTOSConfig.h turns on configCHECK_FOR_STACK_OVERFLOW (2) and
// configUSE_MALLOC_FAILED_HOOK (1) unconditionally, matching SaftyFW's
// choice -- cheap insurance, and the kernel requires these two hooks to
// exist at link time regardless. Halting in place rather than resetting, so
// a debugger attached over SWD (this bench fixture's flashing/debug path,
// PLAN.md section 3.1) can see exactly where it happened instead of the
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
    // docs/PLAN.md section 4.1's table and SaftyFW's own main.c convention.
    // Each _start() function pins its own task's core affinity
    // (task_priorities.h's SIMFW_CORE_RT_PATH / SIMFW_CORE_ELASTIC_PATH) --
    // this file does not set affinity itself, so there is exactly one place
    // per task that can get it wrong, not two.
    //
    // No return value is checked yet: there is no log sink running before
    // the scheduler starts to report a failure to (log_task itself is one
    // of the tasks being started), and every task body is currently just an
    // idle loop with nothing that can fail beyond xTaskCreate() itself --
    // same "logged-and-continued, nothing fatal" posture SaftyFW's main()
    // documents for its own bring-up, minus the logging until log_task is
    // real. TODO: once log_task/telemetry have real bodies, capture and
    // report these.
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
