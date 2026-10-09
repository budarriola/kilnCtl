// test_uart_owner_tx_policy.c -- host tests for the pure TX self-start-
// failure classifier (firmware/hwAbstraction/pico/uart/uart_owner_tx_policy.c/.h,
// moved from src/tasks/ by HAL Phase 1a), the 2026-08-23
// root cause of the DIAG/POWER-went-dark hunt: a send whose priming write
// got zero bytes into the hardware FIFO (because the FIFO was already full
// from an immediately-preceding send) left its ENTIRE frame dependent on the
// TX interrupt alone, and simply re-writing the same "enabled" value to the
// interrupt mask did not reliably produce a fresh interrupt -- the frame sat
// in the ring forever, accepted, undropped, never transmitted.
//
// uart_owner_tx_policy.c has no pico-sdk/FreeRTOS dependency, so unlike
// uart_owner.c itself it is directly host-testable here, same pattern
// test_watchdog_gate.c already established for watchdog_gate.c.
#include "test_common.h"
#include "../../hwAbstraction/pico/uart/uart_owner_tx_policy.h"

// The exact failure condition this whole investigation chased: nothing
// primed, but bytes are still queued and depend on the ISR alone.
static void test_zero_primed_nonzero_remainder_is_failure(void)
{
    TEST_CHECK(uart_owner_tx_send_is_self_start_failure(0u, 38u),
               "primed==0, remainder==38 (DIAG's exact shape when the FIFO was already full) "
               "must classify as a self-start failure");
    TEST_CHECK(uart_owner_tx_send_is_self_start_failure(0u, 67u),
               "primed==0, remainder==67 (POWER's exact shape) must classify as a self-start "
               "failure too");
}

// The common, healthy case (status: 33 primed, 3 remainder) must NOT be
// flagged -- some bytes went straight into the FIFO, so the FIFO's own
// ongoing drain is what will naturally cross the interrupt threshold; no
// forced re-arm is needed or wanted here.
static void test_nonzero_primed_is_never_a_failure_even_with_remainder(void)
{
    TEST_CHECK(!uart_owner_tx_send_is_self_start_failure(33u, 3u),
               "33 primed / 3 remainder (status's own healthy shape) must NOT classify as a "
               "self-start failure");
    TEST_CHECK(!uart_owner_tx_send_is_self_start_failure(1u, 1u),
               "even a single primed byte means the FIFO's own drain will eventually cross the "
               "threshold -- must not classify as a self-start failure");
}

// A frame that fit entirely within the priming write (remainder == 0) is
// not a failure regardless of how much was primed -- there is nothing left
// for the ISR to be responsible for at all.
static void test_zero_remainder_is_never_a_failure(void)
{
    TEST_CHECK(!uart_owner_tx_send_is_self_start_failure(0u, 0u),
               "primed==0, remainder==0 can only mean len==0 (nothing was ever queued) -- not a "
               "failure, there is nothing dependent on the ISR");
    TEST_CHECK(!uart_owner_tx_send_is_self_start_failure(24u, 0u),
               "the whole frame was primed directly into the FIFO -- not a failure, nothing is "
               "left for the ISR to drain");
}

void run_test_uart_owner_tx_policy(void)
{
    TEST_SECTION("uart_owner_tx_send_is_self_start_failure -- the DIAG/POWER-went-dark root "
                  "cause classifier");
    test_zero_primed_nonzero_remainder_is_failure();
    test_nonzero_primed_is_never_a_failure_even_with_remainder();
    test_zero_remainder_is_never_a_failure();
}
