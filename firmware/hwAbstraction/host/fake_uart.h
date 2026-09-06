/* fake_uart.h -- host fake backend for hal_uart.h (Phase 2).
 *
 * See docs/HW_ABSTRACTION.md "Host fakes (Phase 2 specs)" -- fake_uart
 * (must): real ring fill/drain model -- backpressure and drop-counter
 * behavior testable; send_blocking completes instantly; synchronous drive
 * of the owner path. This is the "PRODUCTION SEAM MISSING" substitution
 * point test_safety_guards.c documents for uart_owner_send.
 *
 * Handle storage: hal_uart_t's opaque storage (HAL_UART_STORAGE_BYTES, 64
 * today) is far smaller than the TX capture / RX ring buffers a useful fake
 * needs, so this backend keeps the real buffers in a small fixed pool of
 * FAKE_UART_MAX_INSTANCES slots and stamps a {magic, slot index} tag into
 * the caller's hal_uart_t storage in hal_uart_init(). A handle whose
 * storage does not carry a live tag (never initialized, already
 * deinitialized, or a stack/heap garbage hal_uart_t) is treated as
 * HAL_NOT_READY by every call that takes one.
 */
#ifndef KILNCTL_FAKE_UART_H
#define KILNCTL_FAKE_UART_H

#include <stddef.h>
#include <stdbool.h>

#include "hal_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

#define FAKE_UART_MAX_INSTANCES   4
#define FAKE_UART_TX_CAPTURE_CAP  2048
#define FAKE_UART_RX_RING_CAP     1024

/* Frees every slot and clears all captured/queued state. Does NOT touch
 * any caller's hal_uart_t storage -- call this only when no hal_uart_t
 * from a previous test case is still considered live. */
void fake_uart_reset_all(void);

bool fake_uart_is_live(const hal_uart_t *u);

/* Bytes captured from BOTH hal_uart_send() and hal_uart_send_blocking(),
 * in call order, for the life of this handle since init (hal_uart_restart
 * must not clear this -- see its RX-only contract). NULL/0 if u is not a
 * live handle. */
const uint8_t *fake_uart_tx_capture(const hal_uart_t *u, size_t *out_len);

/* Queues bytes to be returned in order by hal_uart_recv(). This is the
 * fake's own scripting API, not part of hal_uart.h's contract (which only
 * defines a drop policy for the TX side, via hal_uart_send/get_tx_dropped).
 * Overflow policy here: FIFO, drop-oldest, counted by
 * fake_uart_rx_dropped_count() -- documented so a test asserting on drop
 * behavior knows what to expect. Returns HAL_NOT_READY if u is not a live
 * handle, HAL_INVALID_ARG for a NULL data pointer with nonzero len. */
hal_status_t fake_uart_script_rx(hal_uart_t *u, const uint8_t *data, size_t len);

size_t fake_uart_rx_dropped_count(const hal_uart_t *u);
size_t fake_uart_rx_pending_count(const hal_uart_t *u);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_UART_H */
