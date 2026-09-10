/*
 * FreeRTOSConfig.h -- SaftyFW, RP2040, FreeRTOS-Kernel SMP port.
 *
 * Based on FreeRTOS-Kernel's examples/template_configuration/FreeRTOSConfig.h
 * (the generic reference for every value's meaning) and on the values the
 * RP2040 SMP port itself expects/reads in
 * portable/ThirdParty/GCC/RP2040/include/{portmacro.h,rp2040_config.h} --
 * used as the reference implementation per this task's brief ("base it on
 * FreeRTOS-Kernel's RP2040 SMP demo/example FreeRTOSConfig.h if one ships in
 * the cloned repo"). No RP2040-SMP-specific example FreeRTOSConfig.h ships in
 * this checkout of FreeRTOS-Kernel (only the generic ARMv8-M-flavoured
 * template above), so the SMP-relevant values below (configNUMBER_OF_CORES,
 * configUSE_CORE_AFFINITY, configTICK_CORE, configSMP_SPINLOCK_*) are taken
 * from what portmacro.h / rp2040_config.h actually read, not guessed.
 *
 * docs/ARCHITECTURE.md section 8, "FreeRTOS SMP configuration" is the
 * authority for configNUMBER_OF_CORES / configUSE_CORE_AFFINITY / the 1000 Hz
 * tick. If this file disagrees with that section, the doc wins and this file
 * is wrong -- fix this file, not the doc's memory of it.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ---- SMP (docs/ARCHITECTURE.md section 8) -------------------------------- */
/* Both RP2040 cores usable, and core affinity is a real isolation tool (see
 * task_priorities.h's compile-time assert of these same two values --
 * belt-and-suspenders: wrong here, the assert catches it; wrong in the
 * assert, this is still the one config.h that governs the actual scheduler
 * behaviour). */
#define configNUMBER_OF_CORES                   2
#define configUSE_CORE_AFFINITY                 1
#define configTICK_CORE                         0
#define configRUN_MULTIPLE_PRIORITIES           1 /* higher/lower priority tasks may run on different cores at once -- required for the core split to do anything useful */
#define configTASK_DEFAULT_CORE_AFFINITY        tskNO_AFFINITY
#define configUSE_PASSIVE_IDLE_HOOK             0
#define configIDLE_AFFINITY                     0
#define configUSE_TASK_PREEMPTION_DISABLE       0
#define configTIMER_SERVICE_TASK_CORE_AFFINITY  tskNO_AFFINITY

/* ---- Scheduling ------------------------------------------------------------ */
/* 1000 Hz: the 10 ms discrete_task deadline wants it, and nothing here is
 * power-constrained (ARCHITECTURE.md section 8). */
#define configTICK_RATE_HZ                      1000
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0
/* 8 priority levels (0..7) -- covers SAFTYFW_PRIO_RELAY_OWNER == 7, the
 * highest value task_priorities.h assigns, plus the idle task at 0. */
#define configMAX_PRIORITIES                    8
// 2026-09-09: tried raising this 256->512 to fix a discrete_task.c overflow
// (see that file's own DISCRETE_TASK_STACK_WORDS comment) but reverted --
// every *N-multiplied stack size (link_task/safety_core/current_task's *6,
// update_task's *3, log_task's *2) scales off THIS constant too, so the
// global bump inflated total static stack allocation by ~29KB, not the ~5KB
// intended, and traded the overflow for pvPortMalloc() failing during
// startup (core parked forever in vApplicationMallocFailedHook, confirmed
// via SWD). Fixed per-task instead (current_task.c, discrete_task.c) so
// only the two tasks that actually needed more got it.
#define configMINIMAL_STACK_SIZE                256
#define configMAX_TASK_NAME_LEN                 16
#define configTICK_TYPE_WIDTH_IN_BITS           TICK_TYPE_WIDTH_32_BITS
#define configIDLE_SHOULD_YIELD                 1

/* ---- Synchronisation primitives -------------------------------------------- */
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             1
#define configUSE_COUNTING_SEMAPHORES           1
#define configUSE_QUEUE_SETS                    0
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configQUEUE_REGISTRY_SIZE               0
#define configUSE_APPLICATION_TASK_TAG          0

/* ---- Software timers -------------------------------------------------------
 * Not used by this phase's task skeletons (every periodic task drives its own
 * vTaskDelayUntil loop), but left on: cheap, and Phase 5+ (trip-verify
 * timers, GRACE duration) is a natural fit for one-shot software timers
 * rather than reinventing timing inside safety_core. */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_TASK_STACK_DEPTH            configMINIMAL_STACK_SIZE
#define configTIMER_QUEUE_LENGTH                10

#define configUSE_EVENT_GROUPS                  1
#define configUSE_STREAM_BUFFERS                1

/* ---- Memory allocation -------------------------------------------------------
 * heap_4 (via pico-sdk's FreeRTOS-Kernel-Heap4 target, linked in
 * CMakeLists.txt) -- static allocation is also enabled so a task/queue can be
 * created either way; this phase uses dynamic allocation throughout (matches
 * KilnFW's own App/drivers style), Phase 9's "no dynamic allocation after
 * init" constraint applies to steady-state runtime, not to the one-time task
 * creation calls in main(). */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
// 2026-09-09: raised 32K->40K for headroom after current_task.c's and
// discrete_task.c's per-task stack bumps (see each file's own comment) --
// smaller and more targeted than the reverted global configMINIMAL_STACK_
// SIZE bump above. RP2040 has 264KB SRAM total, so 40K leaves ample margin.
#define configTOTAL_HEAP_SIZE                   (40 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP        0
#define configENABLE_HEAP_PROTECTOR             0
#define configKERNEL_PROVIDED_STATIC_MEMORY     1

/* ---- Hooks / debugging -------------------------------------------------------
 * Stack overflow checking is cheap insurance in a safety-relevant build --
 * always on, not just for debug builds. */
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_IDLE_HOOK                     0
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configRECORD_STACK_HIGH_ADDRESS         1

/* configASSERT -- halt in place rather than reboot into a watchdog loop that
 * could mask the actual bug; a debugger attached over SWD can then inspect
 * exactly where the assertion fired. Deliberately NOT the same as a guard
 * trip: this is "the firmware itself is in an invalid state", which is a
 * bench/bring-up problem, not a kiln-safety event.
 *
 * Uses portDISABLE_INTERRUPTS(), not taskDISABLE_INTERRUPTS(): this file is
 * included by FreeRTOS.h itself before task.h exists, so only port-level
 * macros (from portmacro.h, already in scope by this point) are available
 * here.
 *
 * 2026-09-09, RP2040 fatal-fault diagnosability pass: this is what actually
 * fired in the thermo_task incident (5b8fc53d) -- configASSERT(pxQueue->
 * uxItemSize == 0) inside FreeRTOS's own xQueueSemaphoreTake(), reached
 * because a stack overflow elsewhere had smashed a heap-resident queue
 * control block WITHOUT reliably tripping the stack canary. Before this
 * change, configASSERT recorded nothing at all -- the reboot that followed
 * was bit-for-bit indistinguishable from a plain watchdog timeout, and the
 * real cause took an SWD session to find. Now it latches into the same
 * watchdog_hw->scratch[5] register vApplicationStackOverflowHook() (main.c)
 * and vApplicationMallocFailedHook() (main.c) already share -- see
 * watchdog_fatal_diag_t's own doc comment
 * (watchdog_overdue_diag_codec.h) for the full mechanism and why one
 * register safely serves all three mutually-exclusive fatal events.
 *
 * Deliberately a raw MMIO write via a constant-folding macro
 * (SAFTYFW_CONFIGASSERT_WORD()), NOT a call into watchdog_overdue_diag_codec.h's
 * own WATCHDOG_FATAL_ASSERT_WORD() macro, and NOT a function call:
 * configASSERT() can fire from an ISR, from inside a FreeRTOS critical
 * section, or -- as in the incident above -- from a context whose stack is
 * already corrupted, so nothing beyond a single store is safe here, same
 * discipline as the other two hooks. It is ALSO not safe to #include
 * "watchdog_overdue_diag_codec.h" from this file even though that header
 * itself has no FreeRTOS/pico-sdk dependency: FreeRTOSConfig.h is pulled in
 * by pico-sdk library sources (e.g. pico_time/time.c, part of
 * hwabstraction_pico) that have no include path to firmware/SaftyFW/src/ --
 * confirmed the hard way, this broke the target build the first time it was
 * tried. So the bit layout below is a SEPARATE, hand-synced copy of
 * WATCHDOG_FATAL_ASSERT_WORD()'s -- exactly the same "no shared encoder
 * across this boundary" discipline vApplicationStackOverflowHook() (main.c)
 * and watchdog_overflow_diag_decode() (watchdog_overdue_diag_codec.c)
 * already use for the 0xE3 format, extended here because configASSERT's own
 * boundary (FreeRTOSConfig.h, visible to every FreeRTOS-adjacent TU) is even
 * wider than a single hook function's. "hardware/watchdog.h" IS safe to
 * include here -- it is pico-sdk's own header for watchdog_hw and is on
 * every such TU's include path already (this file already required it
 * transitively; the target build confirms it compiles from every call
 * site). test_watchdog_overdue_diag_codec.c's
 * test_all_four_scratch5_magic_bytes_are_distinct() and the assert-specific
 * tests check watchdog_overdue_diag_codec.h's copy of this layout; keeping
 * the two in sync is a hand discipline, not a compiler-checked one -- same
 * as the 0xE3 format's existing precedent.
 *
 * PRECISION: __LINE__ (16 bits, truncated) is latched; the file is NOT
 * identified by name (a translation unit may opt into an 8-bit
 * SAFTYFW_ASSERT_FILE_ID by #define-ing it before including FreeRTOS.h --
 * 0 means "not declared", which is what every vendored FreeRTOS kernel
 * source file reads as, since none of them are ours to annotate). A bare
 * line number does not by itself name which of the handful of kernel
 * source files (queue.c/tasks.c/list.c/timers.c/stream_buffer.c/
 * event_groups.c) is implicated -- but combined with the pinned kernel
 * version already vendored in this tree, a line number is enough to grep
 * straight to the exact assert that fired, which is what this incident
 * actually needed. That is considered sufficient localisation given how
 * scarce flash/scratch space is here; see watchdog_fatal_diag_t's own
 * comment (watchdog_overdue_diag_codec.h) for the full precision tradeoff
 * stated across all three formats.
 *
 * WHY A LITERAL ADDRESS, NOT "hardware/watchdog.h"'s watchdog_hw struct:
 * tried that first -- it broke the target build. FreeRTOSConfig.h is
 * pulled in very early by some pico-sdk library TUs (confirmed:
 * pico_time/time.c, part of hwabstraction_pico, via pico.h ->
 * pico/config.h -> FreeRTOS-Kernel's freertos_sdk_config.h ->
 * FreeRTOSConfig.h), BEFORE pico/platform.h has defined __force_inline --
 * "hardware/watchdog.h" transitively pulls in hardware/address_mapped.h,
 * whose __force_inline-tagged functions then fail to parse ("expected ';'
 * before 'static'") in that TU. main.c/watchdog_overdue_diag.c never hit
 * this because they always include "pico/stdlib.h" (which brings in
 * pico/platform.h) first. A literal MMIO address needs no pico-sdk header
 * at all, so it cannot be hit by this ordering problem from any TU.
 * WATCHDOG_BASE (0x40058000) and the SCRATCH5 byte offset (0x20) are from
 * pico-sdk's own hardware/regs/addressmap.h and hardware/regs/watchdog.h
 * (WATCHDOG_SCRATCH5_OFFSET) respectively -- reverify against those two
 * files if this firmware is ever ported to a pico-sdk release that
 * renumbers them; test_all_four_scratch5_magic_bytes_are_distinct() and the
 * assert round-trip tests do not catch an address drift, only a magic-byte
 * collision, since they exercise the codec's bit-packing, not this literal
 * address. */
#include <stdint.h>

#ifndef SAFTYFW_ASSERT_FILE_ID
#define SAFTYFW_ASSERT_FILE_ID 0u
#endif

/* MUST be kept byte-for-byte identical to watchdog_overdue_diag_codec.h's
 * WATCHDOG_FATAL_ASSERT_WORD() (magic 0xA5, file_id at [23:16], line at
 * [15:0]) -- see this block's own comment for why the two cannot share one
 * definition. */
#define SAFTYFW_CONFIGASSERT_WORD(file_id, line)                             \
    (0xA5000000u | (((uint32_t)(file_id) & 0xFFu) << 16) |                   \
     ((uint32_t)(line) & 0xFFFFu))

/* RP2040 WATCHDOG_BASE + WATCHDOG_SCRATCH5_OFFSET, see this block's own
 * comment for the derivation and why this is a literal address rather than
 * watchdog_hw->scratch[5]. */
#define SAFTYFW_WATCHDOG_SCRATCH5_ADDR ((volatile uint32_t *)(0x40058000u + 0x20u))

#define configASSERT(x)                                                      \
    if ((x) == 0) {                                                          \
        *SAFTYFW_WATCHDOG_SCRATCH5_ADDR =                                    \
            SAFTYFW_CONFIGASSERT_WORD(SAFTYFW_ASSERT_FILE_ID, __LINE__);     \
        portDISABLE_INTERRUPTS();                                            \
        for (;;) {                                                           \
        }                                                                    \
    }

/* ---- Co-routines (unused) --------------------------------------------------- */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* ---- API inclusion ----------------------------------------------------------- */
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configUSE_MINI_LIST_ITEM                1
#define configSTACK_DEPTH_TYPE                  uint32_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t
#define configHEAP_CLEAR_MEMORY_ON_FREE         1
#define configUSE_NEWLIB_REENTRANT              0
#define configUSE_POSIX_ERRNO                   0

#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTaskGetIdleTaskHandle           0
#define INCLUDE_eTaskGetState                    1
/* Required 1, not merely convenient: the RP2040 SMP port's own
 * vPortLockInternalSpinUnlockWithNotify() (port.c) calls
 * xEventGroupSetBitsFromISR(), whose prototype in event_groups.h is itself
 * guarded on INCLUDE_xTimerPendFunctionCall == 1 -- leaving this at the
 * template's default 0 fails the port's own build, not just an application
 * feature. */
#define INCLUDE_xTimerPendFunctionCall           1
#define INCLUDE_xTaskAbortDelay                  0
#define INCLUDE_xTaskGetHandle                   1
#define INCLUDE_xTaskResumeFromISR               1

/* ---- Cortex-M0+ interrupt priorities -----------------------------------------
 * RP2040 is Cortex-M0+: 2 priority bits, so valid values are 0/64/128/192.
 * Lowest priority (192) for both, matching every other Cortex-M0+ FreeRTOS
 * port's convention -- these interrupts must never preempt anything running
 * inside a FreeRTOS critical section. */
#define configKERNEL_INTERRUPT_PRIORITY         255
#define configMAX_SYSCALL_INTERRUPT_PRIORITY    (192) // equivalent to 3 << 6 on this port
#define configMAX_API_CALL_INTERRUPT_PRIORITY   configMAX_SYSCALL_INTERRUPT_PRIORITY

/* configSMP_SPINLOCK_0/1 and configTICK_CORE are read by
 * portable/ThirdParty/GCC/RP2040/include/rp2040_config.h, which supplies its
 * own defaults (PICO_SPINLOCK_ID_OS1/OS2) if this file leaves them undefined
 * -- left undefined here deliberately, so that file's defaults (matched to
 * what pico-sdk itself reserves for RTOS use) are the single source of
 * truth rather than a copy that can drift. */

#endif /* FREERTOS_CONFIG_H */
