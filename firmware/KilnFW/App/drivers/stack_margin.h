/* Task-stack high-water-mark registry -- TODO.md section 13's blocker
 * remover. That investigation found six internal-only task stacks (~20.5KB)
 * that are candidates to shrink or move to PSRAM, but explicitly forbade
 * resizing any of them "from the numbers in this entry alone": this repo has
 * already shipped a stack overflow caused by a size chosen from a comment
 * rather than a measurement (see the reset-one-side/stack-sizing note this
 * module's own commit references). This is the measurement, not the fix --
 * it makes uxTaskGetStackHighWaterMark() readable from the PC side, and
 * changes no stack size.
 *
 * A task registers itself (or is registered by whoever creates it) right
 * after xTaskCreate (or xTaskCreatePinnedToCore, or their WithCaps variants)
 * returns, passing the SAME
 * TaskHandle_t* that call was given as its output-handle argument -- not a
 * copy of the handle's current value. That indirection matters: several
 * call sites here store the handle in a struct field (uart_owner_t::
 * task_handle, uart_protocol_t::rx_task_handle, ...) and set it to NULL if
 * creation ever fails or the task is later torn down, so reading through
 * the pointer at report time (rather than snapshotting the handle once at
 * registration) means a torn-down task correctly reports "not alive"
 * instead of a stale, dangling-handle read.
 *
 * This header is deliberately FreeRTOS-free in its own declarations --
 * `task_handle_slot` is documented as a `TaskHandle_t *` but typed `void *`
 * here so nothing in App/test needs freertos/task.h to exercise the pure
 * math in stack_margin_calc.h. The registry/on-target half below (this
 * file's .c) is not itself host-tested -- there is no I/O-free way to fake
 * uxTaskGetStackHighWaterMark() worth the trouble, and the arithmetic it
 * depends on (word->byte conversion, headroom classification) already is,
 * fully, in stack_margin_calc.h / test_stack_margin.c. */
#ifndef STACK_MARGIN_H
#define STACK_MARGIN_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stack_margin_calc.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-size registry, not a linked list or dynamic allocation: this exists
 * specifically to answer a low-memory question, so it must not itself
 * compete for the heap it is reporting on. Six entries are named in TODO.md
 * section 13 today; 12 leaves headroom for the other long-lived
 * internal-only tasks this investigation's audit also turned up (kiln_io_
 * owner, thermo_owner, spi/i2c owners, profile_executor + its watchdog,
 * wifi_prov_owner, screen_idle) without immediately needing to grow this
 * again -- see TODO.md section 13 for which of those are and are not
 * currently registered.
 *
 * DRAM_PSRAM_PLAN.md Phase 0 (4.2), 2026-09-02: raised 12 -> 28 to cover the
 * previously-uninstrumented tasks that plan's section listed (kiln_io_owner,
 * thermo_owner, screen_idle, spi_owner, i2c_owner, autotune_engine,
 * telemetry_log, link_watchdog, info_uart_bridge, gpio_probe, the LVGL task,
 * boot_button, danger_mode, recovery_exit_reboot, ota_rollback_reboot,
 * ota_pico_rollback -- 16 new registrations) on top of the 12 already
 * registered. Exact occupancy after this change: 28/28, no spare slots left
 * -- the next task added here needs another bump, not a silent overflow (see
 * stack_margin.c's "registry full" log line, which is the failure mode this
 * comment exists to keep from being silent).
 *
 * DRAM_PSRAM_PLAN.md section 7 (cap-raise pass), 2026-09-02: raised 28 -> 40.
 * That 28/28-full state was the actual section 7.3 blocker: kiln_io_owner,
 * thermo_owner, spi_owner, i2c_owner and screen_idle EACH already had a
 * stack_margin_register() call site (added in the Phase 0 pass above), so
 * this was never a missing-registration bug -- every call to register()
 * beyond the 28th silently failed (logged, not fatal) with no slot to put
 * it in, and i2c_owner_init() is shared by two live callers on this board
 * (SX1509 the IO expander, NS2009 the touch controller -- see i2c_owner.c),
 * so its ONE source call site fires TWICE at boot, making the real
 * boot-time registration count 29 against a cap of 28: one guaranteed
 * failure every boot, and which specific task lost the coin flip depended
 * on init order, not on anything about that task. Raising to 40 gives 11
 * spare slots above that 29 -- room for the double i2c_owner registration
 * plus a few more tasks before this needs touching again. Cost: 12 slots *
 * sizeof(stack_margin_entry_t) (28 bytes: char name[20] + TaskHandle_t*
 * (4 bytes on this 32-bit target) + uint32_t, no padding) = 336 bytes of
 * static DRAM -- a deliberate, documented spend on the measurement this
 * whole plan needs before it can spend anything back. */
#define STACK_MARGIN_MAX_TASKS 40u
#define STACK_MARGIN_NAME_MAX  20u

/* Registers one task for reporting. `task_handle_slot` is a `TaskHandle_t *`
 * (see this header's top comment for why it is typed `void *` here); pass
 * the same pointer given to xTaskCreate*'s output-handle argument. Safe to
 * call before that task is actually created, since the slot is read fresh
 * (via the pointer) at report time, not snapshotted here -- registering
 * first and creating the task second, or the reverse, both work.
 *
 * `name` is copied (not retained by reference) and truncated to
 * STACK_MARGIN_NAME_MAX-1 characters if longer; pass the same string
 * literal used as the task's own FreeRTOS name for one obvious mapping
 * between this report and a debugger/`vTaskList()` dump.
 *
 * `configured_stack_bytes` is the exact `usStackDepth` argument passed to
 * that same xTaskCreate* call, in BYTES (matching every xTaskCreate* call
 * in this codebase, which already takes bytes, not words) -- used only to
 * classify the reading (stack_margin_calc.h), never assumed equal to any
 * other task's.
 *
 * Returns false, logging why, without registering anything, if the
 * registry is full or a NULL/empty name or NULL slot pointer was passed --
 * a silently-dropped registration would read on the PC side as "this task
 * doesn't exist," which is worse than a boot-time log line saying so. */
bool stack_margin_register(const char *name, void *task_handle_slot, uint32_t configured_stack_bytes);

/* Number of tasks currently registered (<= STACK_MARGIN_MAX_TASKS). */
size_t stack_margin_count(void);

/* Reads one registered entry's live figures by index
 * (0 <= index < stack_margin_count()). Returns false (nothing written) for
 * an out-of-range index.
 *
 * `alive` is false whenever the registered slot's *TaskHandle_t is NULL
 * (never created, creation failed, or already deleted) -- in that case
 * `hwm_bytes` and `level` are both set to 0, never left uninitialized or
 * carrying a stale previous reading. */
bool stack_margin_read(size_t index, const char **name, uint32_t *configured_stack_bytes,
                        uint32_t *hwm_bytes, stack_margin_level_t *level, bool *alive);

#ifdef __cplusplus
}
#endif

#endif /* STACK_MARGIN_H */
