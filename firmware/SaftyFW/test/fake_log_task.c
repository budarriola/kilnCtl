// Host-test fake for log_task_log() (tasks/log_task.h).
//
// 2026-09-15 (Opus adversarial re-review, F3, docs/audits/
// review_tc_type_fixes_a2384dd5_2026-09-15.md): config_store.c's
// reentrancy trip-wire in config_store_only_tc_type_differs() now calls
// log_task_log() instead of abort() when it re-enters (see that function's
// own comment for why: abort() resets the whole RP2040 and drops K4, which
// the owner's hard line forbids). The real log_task.c depends on the
// pico-sdk/FreeRTOS queue machinery this host toolchain does not build, so
// this is a minimal fake -- same pattern as fake_scratch.c/fake_gpio.c
// elsewhere in this host-test suite. Prints to stdout (visible in test
// output, not asserted on by any test today) and always reports "queued"
// (true); nothing here needs the real drop/filter accounting since no
// current host test asserts on log_task's own counters through this path.
#include <stdio.h>

#include "tasks/log_task.h"

bool log_task_log(uint8_t level, const char *tag, const char *msg)
{
    printf("[fake_log_task] level=%u tag=%s msg=%s\n", (unsigned)level, tag ? tag : "(null)",
           msg ? msg : "(null)");
    return true;
}
