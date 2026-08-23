/*
 * FreeRTOSConfig.h -- SimFW, RP2040, FreeRTOS-Kernel SMP port.
 *
 * Adapted from ../SaftyFW/FreeRTOSConfig.h, which is itself based on
 * FreeRTOS-Kernel's examples/template_configuration/FreeRTOSConfig.h (the
 * generic reference for every value's meaning) and on the values the RP2040
 * SMP port itself expects/reads in
 * portable/ThirdParty/GCC/RP2040/include/{portmacro.h,rp2040_config.h}. No
 * RP2040-SMP-specific example FreeRTOSConfig.h ships in this checkout of
 * FreeRTOS-Kernel, so the SMP-relevant values below (configNUMBER_OF_CORES,
 * configUSE_CORE_AFFINITY, configTICK_CORE, configSMP_SPINLOCK_*) are taken
 * from what portmacro.h / rp2040_config.h actually read, not guessed --
 * same sourcing SaftyFW's copy used.
 *
 * docs/DESIGN_NOTES.md section 4.1 (task map) and section 3.6 (pin budget) are the
 * authority for the SMP configuration below (configNUMBER_OF_CORES,
 * configUSE_CORE_AFFINITY, the 1000 Hz tick) -- if this file and that
 * section disagree, the doc wins and this file is wrong.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* ---- SMP (docs/DESIGN_NOTES.md section 4.1) --------------------------------------
 * Both RP2040 cores usable, and core affinity is how the task map's "core 1
 * = hard-real-time producers, core 0 = everything elastic" split (DESIGN_NOTES.md
 * section 4.1's closing paragraph) actually gets enforced -- see
 * task_priorities.h's compile-time assert of these same two values. */
#define configNUMBER_OF_CORES                   2
#define configUSE_CORE_AFFINITY                 1
#define configTICK_CORE                         0
#define configRUN_MULTIPLE_PRIORITIES           1 /* higher/lower priority tasks may run on different cores at once -- required for the core split to do anything useful */
#define configTASK_DEFAULT_CORE_AFFINITY        tskNO_AFFINITY
#define configUSE_PASSIVE_IDLE_HOOK             0
#define configIDLE_AFFINITY                     0
#define configUSE_TASK_PREEMPTION_DISABLE       0
#define configTIMER_SERVICE_TASK_CORE_AFFINITY  tskNO_AFFINITY

/* ---- Scheduling ------------------------------------------------------------
 * 1000 Hz: matches SaftyFW's choice -- nothing here is power-constrained,
 * and a fine tick keeps the eventual sim_engine 10 Hz tick and the wave/SPI
 * real-time producers' timing comfortably resolvable. */
#define configTICK_RATE_HZ                      1000
#define configUSE_PREEMPTION                    1
#define configUSE_TIME_SLICING                  1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE                 0
/* 8 priority levels (0..7) -- covers SIMFW_PRIO_SPI_EMU_A/_B/WAVE_OWNER == 7,
 * the highest values task_priorities.h assigns, plus the idle task at 0. */
#define configMAX_PRIORITIES                    8
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
 * Not used by this skeleton's task stubs (every periodic stub drives its own
 * vTaskDelay loop), left on for the same reason SaftyFW leaves it on: cheap,
 * and the fault-trigger timing fault_sched will eventually own (DESIGN_NOTES.md
 * section 7.2/7.3) is a natural fit for one-shot software timers. */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1)
#define configTIMER_TASK_STACK_DEPTH            configMINIMAL_STACK_SIZE
#define configTIMER_QUEUE_LENGTH                10

#define configUSE_EVENT_GROUPS                  1
#define configUSE_STREAM_BUFFERS                1

/* ---- Memory allocation -------------------------------------------------------
 * heap_4 (via pico-sdk's FreeRTOS-Kernel-Heap4 target, linked in
 * CMakeLists.txt) -- static allocation also enabled so a task/queue can be
 * created either way. 32 KiB matches SaftyFW's starting number; this
 * skeleton's ten idle task stubs use only configMINIMAL_STACK_SIZE each, so
 * there is ample headroom today -- revisit once real task bodies (USB CDC
 * buffers, PIO/DMA descriptors, the zone snapshot and event ring from
 * DESIGN_NOTES.md section 4.5) land and actually pressure it. */
#define configSUPPORT_STATIC_ALLOCATION         1
#define configSUPPORT_DYNAMIC_ALLOCATION        1
#define configTOTAL_HEAP_SIZE                   (32 * 1024)
#define configAPPLICATION_ALLOCATED_HEAP        0
#define configENABLE_HEAP_PROTECTOR             0
#define configKERNEL_PROVIDED_STATIC_MEMORY     1

/* ---- Hooks / debugging -------------------------------------------------------
 * Stack overflow checking is cheap insurance -- always on, matching
 * SaftyFW's choice, not just a debug-build nicety. */
#define configCHECK_FOR_STACK_OVERFLOW          2
#define configUSE_IDLE_HOOK                     1 /* SimFW's post-scheduler heartbeat LED -- vApplicationIdleHook() in main.c, see its comment; runs on core 0's idle task only (configUSE_PASSIVE_IDLE_HOOK stays 0) */
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configUSE_DAEMON_TASK_STARTUP_HOOK      0
#define configGENERATE_RUN_TIME_STATS           0
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configRECORD_STACK_HIGH_ADDRESS         1

/* configASSERT -- halt in place so a debugger attached over SWD (this bench
 * fixture's flashing/debug path, DESIGN_NOTES.md section 3.1) can inspect exactly
 * where the assertion fired, rather than resetting and masking the bug
 * behind a watchdog-looking reboot. Same policy SaftyFW uses.
 *
 * Uses portDISABLE_INTERRUPTS(), not taskDISABLE_INTERRUPTS(): this file is
 * included by FreeRTOS.h itself before task.h exists, so only port-level
 * macros (from portmacro.h, already in scope by this point) are available
 * here. */
#define configASSERT(x)                                                      \
    if ((x) == 0) {                                                          \
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
 * feature. Same requirement SaftyFW's copy documents. */
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
