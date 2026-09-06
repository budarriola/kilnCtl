/* hal_uart.h -- portable UART interface, two send primitives.
 *
 * See docs/HW_ABSTRACTION_PLAN.md "hal_uart -- two primitives, ESP backend
 * unchanged" for the full derivation. uart_protocol.c's frame_and_send()
 * relies on the send it uses blocking until the bytes are ON THE WIRE
 * (not just handed to a driver) because its ACK timer starts right after
 * the call returns; a fire-and-forget-only interface would start that timer
 * before the last byte left the FIFO. Both primitives exist so each backend
 * uses the shape its protocol layer actually needs.
 *
 * Threading/ownership contract:
 *  - Single-writer: one task per hal_uart_t performs sends; RX draining is
 *    the reader's job via hal_uart_recv, non-blocking, returns 0..max
 *    bytes actually available -- never blocks waiting for more.
 *  - Buffer-copy-in: hal_uart_send/_blocking copy from the caller's buffer
 *    (or hand it to a driver that completes synchronously); the caller may
 *    reuse the buffer as soon as the call returns.
 *  - Drop policy: hal_uart_send is non-blocking, whole-buffer-or-HAL_BUSY
 *    -- it never partially sends. get_tx_dropped() exposes the backend's
 *    drop counter so callers can observe backpressure rather than silently
 *    losing bytes.
 *  - Wire-complete semantics: hal_uart_send_blocking's contract is "returns
 *    when the last byte has left the wire" on the ESP backend
 *    (uart_write_bytes + uart_wait_tx_done, unchanged). The pico backend's
 *    send_blocking is documented as ring-drained-not-wire-complete: it
 *    waits for the software TX ring to empty (get_tx_used()==0), which is
 *    NOT the same guarantee -- the last byte may still be shifting out of
 *    the PL011 (TXFE clears before the byte fully leaves; UARTFR.BUSY is
 *    the correct bit if true wire-completion is ever needed on pico). This
 *    is safe today only because no pico consumer originates an ACK-timed
 *    send (the Pico only answers BROADCAST with BROADCAST,
 *    fire-and-forget). A future pico consumer needing true wire completion
 *    must not assume send_blocking already provides it.
 *  - hal_uart_restart is RX-only by contract (matches uart_owner_restart's
 *    single caller and documented RX-only behavior) -- it must never reset
 *    TX-side state. See UART_PROTOCOL.md "Reset pairing".
 */
#ifndef KILNCTL_HAL_UART_H
#define KILNCTL_HAL_UART_H

#include <stddef.h>
#include <stdint.h>

#include "hal_status.h"

/* HAL_ALIGNAS8 is defined in hal_status.h (included above) so it is shared
 * across hal_uart.h/hal_i2c.h/hal_spi.h instead of copied in each. */

#ifdef __cplusplus
extern "C" {
#endif

/* Reservation per docs/HW_ABSTRACTION_PLAN.md "Opaque handles": 64 B
 * (measured today: uart_owner_t ~32 B on 32-bit). Pico backends have no
 * per-instance struct at all (file-scope statics) and simply under-fill. */
#define HAL_UART_STORAGE_BYTES 64

typedef struct {
    HAL_ALIGNAS8 uint8_t storage[HAL_UART_STORAGE_BYTES];
} hal_uart_t;

typedef struct {
    int port;          /* backend port/instance number -- ESP UART_NUM_*,
                         * pico uart0/uart1 index. Two ports are live on this
                         * board today (main.c's PC link and safety_link.c's
                         * safety link); without this field two hal_uart_t
                         * instances would be indistinguishable to a shared
                         * backend that dispatches on port number. */
    int tx_io;
    int rx_io;
    uint32_t baud;

    /* Owner/event-task sizing, carried through from uart_owner_init()'s real
     * call sites (safety_link.c:416-418, CONFIG_KILNCTL_UART_OWNER_* Kconfig
     * values) so each of the two live ports can be sized/prioritized
     * independently and so the backend's task can be registered for
     * stack-margin reporting with a caller-known stack_depth (see
     * hal_uart_get_task_handle below). 0 in any field means "backend
     * default", and that default equals the real value for this instance
     * class today (queue_len 16, task_priority 5, stack_depth 3072 per
     * KILNCTL_UART_OWNER_STACK_SIZE) -- not an arbitrary backend pick.
     * core_id: HAL_CORE_ANY (hal_status.h) or an explicit core number; pico/
     * host backends ignore this. */
    uint32_t queue_len;
    int task_priority;
    uint32_t stack_depth;
    int core_id;
} hal_uart_cfg_t;

hal_status_t hal_uart_init(hal_uart_t *u, const hal_uart_cfg_t *cfg);

/* Symmetric with hal_uart_init; releases backend resources (ESP:
 * uart_driver_delete). Not called on any hot path today -- added for
 * lifecycle symmetry (bring-up/teardown pairs elsewhere in this interface
 * all have both halves) and for host/test teardown between cases. */
hal_status_t hal_uart_deinit(hal_uart_t *u);

/* Non-blocking, whole-buffer-or-HAL_BUSY. ESP: uart_write_bytes without a
 * wait. */
hal_status_t hal_uart_send(hal_uart_t *u, const uint8_t *data, size_t len);

/* Blocks until the send completes per this backend's wire-complete
 * contract documented above. ESP: today's write + uart_wait_tx_done,
 * unchanged. Pico: send + drain of the software ring (race-free under
 * save_and_disable_interrupts), ring-drained-not-wire-complete. Host: instant. */
hal_status_t hal_uart_send_blocking(hal_uart_t *u, const uint8_t *data,
                                     size_t len, uint32_t timeout_ms);

/* Non-blocking; returns the number of bytes actually copied out, 0..max. */
size_t hal_uart_recv(hal_uart_t *u, uint8_t *out, size_t max);

/* Bounded blocking read: blocks until at least one byte is available or
 * timeout_ms elapses, whichever comes first -- matching the real reader's
 * fallback shape (uart_protocol.c's bounded 1-byte blocking
 * uart_read_bytes() call behind its otherwise-nonblocking poll loop, used
 * exactly when nothing was already buffered). Returns the number of bytes
 * actually copied into buf, 0..cap: 0 means the timeout elapsed with
 * nothing received (the moral equivalent of HAL_TIMEOUT for this
 * size_t-returning sibling of hal_uart_recv). Ends the wait as soon as ANY
 * bytes are available -- never waits to fill cap. 0 if u/buf is NULL or
 * cap == 0. */
size_t hal_uart_recv_blocking(hal_uart_t *u, uint8_t *buf, size_t cap,
                               uint32_t timeout_ms);

uint32_t hal_uart_get_rx_error_count(const hal_uart_t *u);
uint32_t hal_uart_get_tx_dropped(const hal_uart_t *u);

/* RX-only. See contract note above -- must never touch TX-side state. */
hal_status_t hal_uart_restart(hal_uart_t *u);

/* Returns the backend's owner/event task handle (FreeRTOS TaskHandle_t on
 * ESP, cast to void*) for stack-margin registration BY THE CALLER --
 * CLAUDE.md "Register every new task for stack-margin reporting" (the
 * board has bricked into a permanent recovery loop twice from an
 * unregistered task's stack overflow). NULL if u has no task (not
 * initialized, or a backend with no owner task at all, e.g. host/pico
 * today). This function does not itself register anything. */
void *hal_uart_get_task_handle(const hal_uart_t *u);

#ifdef __cplusplus
}
#endif

#endif /* KILNCTL_HAL_UART_H */
