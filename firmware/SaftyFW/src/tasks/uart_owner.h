// uart_owner.h -- owns UART1 (GPIO4 TX / GPIO5 RX), the opto-isolated link to
// the ESP (docs/ARCHITECTURE.md section 3's module table: "RX ring +
// non-blocking TX ring. Drops on full, never blocks").
//
// This is the hardware-owner layer only: raw bytes in, raw bytes out. It
// knows nothing about kilnlink framing, frame types or payload layouts --
// that is link_task's job, same split spi_owner/max31856.c already use.
//
// Plain hardware UART, no inversion, no PIO: the ESP inverts both directions
// itself (uart_set_line_inverse on that side) and each opto-isolator inverts
// once more, so two inversions in series cancel and this side's ordinary
// hardware/uart.h works unmodified. See docs/HARDWARE.md section 1 and
// firmware/KilnFW/App/drivers/safety_link.h's polarity comment -- getting
// this backwards on either side kills the link.
//
// RX is IRQ-driven into a small ring buffer; link_task drains it by polling
// (LINK_TASK_POLL_MS elsewhere) rather than blocking on a queue receive --
// link_task also has to run its own 500 ms TX cadence and check in with
// watchdog_task, so "poll a ring that an IRQ fills" fits its existing loop
// shape better than an RX callback that would need its own synchronisation
// back into link_task's context. TX is also IRQ-driven, drained out of a
// ring buffer by the UART1 IRQ handler as space in the hardware FIFO frees
// up, so uart_owner_send() itself never blocks and never waits on the ISR.
//
// Both rings are single-producer/single-consumer, and (by construction of
// docs/ARCHITECTURE.md section 4's core split) every producer and consumer
// of both rings runs on core 0 -- uart_owner_init() is called from main()
// before the scheduler starts, so the UART1 IRQ is installed on core 0, and
// link_task (the only task that calls uart_owner_send()/uart_owner_rx_read())
// is pinned to SAFTYFW_CORE_LINK_PATH, also core 0. That is what makes a
// short save_and_disable_interrupts()/restore_interrupts() critical section
// (plain pico-sdk hardware/sync.h, not a FreeRTOS one) sufficient here --
// there is no second core ever touching these indices.
#ifndef SAFTYFW_TASKS_UART_OWNER_H
#define SAFTYFW_TASKS_UART_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up UART1 at 115200 8N1 (matching CONFIG_KILNCTL_SAFETY_BAUD_RATE's
// default on the ESP side), configures GPIO4/5 for UART function, and
// installs the shared UART1 IRQ handler for both RX collection and TX
// draining. Must be called once, from main(), before link_task_start() --
// link_task assumes the UART and both rings already exist. Returns false
// only if the underlying pico-sdk calls themselves would not indicate
// failure any other way; in practice uart_init()/gpio_set_function() do not
// fail on this hardware, so this is here for symmetry with the other _init()
// return-value convention (spi_owner_init(), etc.) rather than because a
// realistic failure mode exists today.
bool uart_owner_init(void);

// Non-blocking send: copies `len` bytes into the TX ring and enables the TX
// IRQ so they drain into the hardware FIFO as space frees up. If the ring
// does not have room for the WHOLE frame, nothing is written -- the frame is
// dropped in its entirety (never partially sent), a counter is incremented
// (see uart_owner_get_tx_dropped()), and this returns false immediately.
// Never blocks, never waits on the ISR: this is the mechanism that makes
// LINK_PROTOCOL.md section 2 rule 3 ("if the ring is full, the frame is
// dropped ... caller returns immediately") true in this codebase, not just
// documented.
bool uart_owner_send(const uint8_t *data, size_t len);

// Drains up to `max` bytes already collected by the RX IRQ into `out`.
// Non-blocking: returns immediately with however many bytes were available
// (0 if none). Safe to call from link_task's own poll loop at whatever rate
// it likes -- bytes not yet drained just wait in the ring for the next call,
// up to its fixed capacity (older bytes are silently overwritten if
// link_task falls far enough behind that the RX ring itself fills, which
// only happens if link_task stops running entirely -- watchdog_task will
// catch that before it matters).
size_t uart_owner_rx_read(uint8_t *out, size_t max);

// Total number of frames uart_owner_send() has refused because the TX ring
// did not have room. Diagnostic only -- intended for a future SAFETY_CMD_DIAG
// tx_frames_dropped field (LINK_PROTOCOL.md section 6, Frame B); nothing
// consumes it yet.
uint32_t uart_owner_get_tx_dropped(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_UART_OWNER_H
