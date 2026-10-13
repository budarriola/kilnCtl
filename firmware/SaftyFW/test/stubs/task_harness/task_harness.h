// task_harness.h -- drives a REAL FreeRTOS-style task body (an infinite
// for(;;) loop) on the host and gets control back again.
//
// How: the stub xTaskCreate() captures the task function. th_run_captured_
// task() calls it under a setjmp; the test's hooks (invoked from inside the
// stubbed blocking calls the loop makes every iteration) script the world for
// the next step and, when the scenario is over, call th_abort(), which
// longjmps out of the loop back into th_run_captured_task(). The task's own
// file-scope state (retry counters, last-checkin ticks, ...) survives across
// calls, but each new call restarts the task body from its top, so a scenario
// that needs N loop iterations runs them all inside ONE call.
#ifndef SAFTYFW_TASK_HARNESS_H
#define SAFTYFW_TASK_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

// Clears tick, captured task function, notification count, gpio pin levels,
// captured IRQ callback, hooks, semaphore-failure flag, call counters.
void th_reset(void);

TaskFunction_t th_captured_task_fn(void);
void           th_run_captured_task(void); // returns after th_abort()
void           th_abort(void);             // longjmp out of the running task loop

// Tick count returned by xTaskGetTickCount().
void       th_set_tick(TickType_t t);
TickType_t th_get_tick(void);
void       th_advance_tick(TickType_t dt);

// Called at the top of every ulTaskNotifyTake() wait (before the pending
// count is read), with the requested wait in ticks. The hook scripts the
// world for this loop iteration: queue SPI responses, set the DRDY pin level,
// call th_fire_drdy_isr(), advance the tick, or th_abort().
typedef void (*th_wait_hook_t)(uint32_t timeout_ticks);
void th_set_wait_hook(th_wait_hook_t hook);

// Called from vTaskDelayUntil() after the wake time has been advanced and the
// tick set to it (watchdog_task_fn's period boundary).
typedef void (*th_delay_hook_t)(void);
void th_set_delay_hook(th_delay_hook_t hook);

// ~DRDY IRQ model.
void     th_gpio_set_level(unsigned pin, bool level);
bool     th_irq_registered(void);
unsigned th_irq_pin(void);
void     th_fire_drdy_isr(void); // invokes the captured callback, as the GPIO IRQ dispatcher would

// Notifications currently pending (what ulTaskNotifyTake() would return).
uint32_t th_pending_notifications(void);

// Makes xSemaphoreTake() time out (returns pdFALSE) while true.
void th_set_sem_take_fails(bool fails);

// Number of vTaskDelayUntil()/ulTaskNotifyTake() calls since th_reset().
uint32_t th_delay_calls(void);
uint32_t th_wait_calls(void);

#endif
