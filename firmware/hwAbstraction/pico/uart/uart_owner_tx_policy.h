// uart_owner_tx_policy.h -- pure TX-arm decision logic, split out of
// uart_owner.c the same way watchdog_gate.h is split out of watchdog_task.c
// (see that header's own comment): free of pico-sdk/FreeRTOS includes so it
// is directly host-testable (test/test_uart_owner_tx_policy.c links this .c
// file with no stub layer needed), unlike uart_owner.c itself which needs
// real hardware/uart.h and hardware/sync.h.
//
// 2026-08-23, the DIAG/POWER-went-dark root cause: uart_owner_send()'s
// priming write fills the hardware FIFO directly so the first bytes of a
// frame always leave without waiting on the TX interrupt. When the FIFO is
// ALREADY full at the moment a send starts -- which happens whenever a send
// runs immediately after a previous one that has not finished draining, the
// exact shape of link_task_fn()'s status -> diag -> power sequence every
// single loop iteration where all three are due -- the priming loop writes
// ZERO bytes (uart_is_writable() is false from its very first check), and
// the ENTIRE frame is left dependent on the TX interrupt alone. Simply
// re-writing the same "enabled" value to UARTIMSC in that case did not
// reliably produce a fresh interrupt once the FIFO later drained -- status
// went first every time, primed cleanly, and worked; diag and power went
// second and third every time, found the FIFO still full, primed nothing,
// and were stranded. Not probabilistic (a race would occasionally hit
// status too); structurally deterministic, because program order made
// status always first and diag/power always second/third.
//
// uart_owner_tx_send_is_self_start_failure() is the pure classification of
// that exact condition, factored out so it can be host-tested and so the
// resulting counter (uart_owner_get_tx_self_start_failures(), uart_owner.h)
// stays meaningful after the fix -- this is the sixth silent failure point
// this investigation has found; the fix removes the SYMPTOM (the frame never
// arriving) but this counter keeps the CONDITION (primed nothing, had to
// force a re-arm) visible rather than letting it go dark again.
#ifndef SAFTYFW_TASKS_UART_OWNER_TX_POLICY_H
#define SAFTYFW_TASKS_UART_OWNER_TX_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// True iff this send call queued at least one byte for the ISR to drain
// (remainder > 0) but the priming loop got NONE of them into the hardware
// FIFO itself (primed_this_call == 0) -- the exact condition that stranded
// DIAG and POWER indefinitely: the frame is fully queued (uart_owner_send()
// still returns true, s_tx_dropped stays 0) but nothing about queuing it
// caused a NEW interrupt-worthy transition, so unless something forces one,
// the frame sits in the ring until some unrelated future send happens to
// re-arm at a moment the FIFO is genuinely crossing its threshold.
//
// primed_this_call and remainder are u32 byte counts, never negative by
// construction (remainder = len - primed_this_call, and primed_this_call is
// capped at len by the loop that produces it) -- no signed/unsigned
// footguns to check here, this is a pure two-comparison classifier.
bool uart_owner_tx_send_is_self_start_failure(uint32_t primed_this_call, uint32_t remainder);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_UART_OWNER_TX_POLICY_H
