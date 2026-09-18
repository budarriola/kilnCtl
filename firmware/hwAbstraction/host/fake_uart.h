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

/* Rx line-error causes, one per UART event the real ESP backend counts
 * (hal_uart_esp.c's hal_uart_esp_event_task(): UART_FIFO_OVF,
 * UART_BUFFER_FULL, UART_BREAK, UART_PARITY_ERR, UART_FRAME_ERR). Kept as
 * a fake-only enum rather than reusing the IDF's uart_event_type_t so this
 * header stays portable/host-buildable. */
typedef enum {
    FAKE_UART_RX_ERROR_FIFO_OVF,
    FAKE_UART_RX_ERROR_BUFFER_FULL,
    FAKE_UART_RX_ERROR_BREAK,
    FAKE_UART_RX_ERROR_PARITY,
    FAKE_UART_RX_ERROR_FRAME,
} fake_uart_rx_error_t;

/* Injects one occurrence of the named rx line-error cause: increments the
 * counter hal_uart_get_rx_error_count() reports, matching
 * hal_uart_esp.c's event task exactly (same five causes, same policy).
 * FIFO_OVF and BUFFER_FULL additionally discard whatever is currently
 * queued in the rx ring, mirroring the real backend's uart_flush_input()
 * on those two causes only (bytes are already lost/framing is broken by
 * definition); BREAK/PARITY/FRAME are counted only and must never touch
 * queued rx data, since the frame layer above is self-synchronising and
 * CRC-checked -- flushing on every line glitch would delete good frames
 * queued behind a bad one (see hal_uart_esp.c's event task comment).
 * The counter itself is per-slot, plain uint32_t wraparound (no
 * saturation), and is cleared only by hal_uart_restart()'s RX-only
 * contract -- never by reading it -- matching hal_uart_esp.c exactly.
 * Returns HAL_NOT_READY if u is not a live handle, HAL_INVALID_ARG for an
 * unrecognized cause. */
hal_status_t fake_uart_inject_rx_error(hal_uart_t *u, fake_uart_rx_error_t cause);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_FAKE_UART_H */
