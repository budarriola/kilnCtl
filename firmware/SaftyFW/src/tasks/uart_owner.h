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
// did not have room. Diagnostic only -- consumed by link_task's Frame B
// (SAFETY_CMD_DIAG, tx_frames_dropped, LINK_PROTOCOL.md section 6).
uint32_t uart_owner_get_tx_dropped(void);

// Bytes currently queued in the TX ring, waiting to drain to the UART
// hardware FIFO. A snapshot, not a guarantee -- the IRQ handler is draining
// concurrently, same as every other read in this file -- but adequate for a
// capacity check that only needs "roughly how full", not an exact count.
// Added for log_task's TX-reserve watermark (docs/ARCHITECTURE.md section 1:
// "reserve TX ring capacity for telemetry"; log_task checks this, via
// link_task's wrapper, before handing a LOG frame to uart_owner_send() --
// see link_task_get_tx_ring_fill_fraction()).
size_t uart_owner_get_tx_used(void);

// Total TX ring capacity in bytes (UART_OWNER_TX_RING_SIZE), for a caller to
// turn uart_owner_get_tx_used() into a fraction without duplicating the
// constant. Fixed at compile time; exposed as a function rather than a
// #define so this file stays the one place that constant is spelled.
size_t uart_owner_get_tx_capacity(void);

// 2026-08-23, size-window investigation (coordinator's own lead: the
// DIAG/POWER-never-arrives boundary sits between 34 and 36 RAW bytes, right
// next to the RP2040's 32-byte TX FIFO depth). uart_owner_get_last_send_remainder()
// reports how many bytes of the MOST RECENT uart_owner_send() call were left
// in the ring after the priming write in uart_owner_send() filled the
// hardware FIFO -- i.e. bytes that depend entirely on the TX ISR
// (uart_owner_irq_handler()) actually firing again to ever leave. Read this
// right after link_task.c's own s_last_tx_cmd/s_last_tx_stuffed_len latch
// (same call, same diagnostic generation) to know which command's remainder
// this is. uart_owner_get_tx_bytes_from_isr() is the ISR-side half of the
// same question: if it never increases after a DIAG/POWER send despite a
// nonzero remainder having just been reported, the ISR is not draining that
// frame's tail -- the exact mechanism the coordinator's lead points at.
// Safe to delete once the size-window question is settled.
size_t uart_owner_get_last_send_remainder(void);
uint32_t uart_owner_get_tx_bytes_from_isr(void);
uint32_t uart_owner_get_tx_bytes_to_fifo(void);

// Times uart_owner_send() had to force a fresh TX-interrupt re-arm because
// the priming write got zero bytes into the hardware FIFO (the FIFO was
// already full from an immediately-preceding send) yet still left a nonzero
// remainder for the ISR -- see uart_owner_tx_policy.h for the exact
// condition. This is the root cause the 2026-08-23 DIAG/POWER hunt found:
// simply re-writing the TX-interrupt-enable bit to the same value it
// already held did not reliably produce a fresh interrupt, so the frame sat
// in the ring forever. uart_owner_send() now forces a real re-arm whenever
// this condition is detected; this counter is what keeps the condition
// itself visible now that its former symptom (a frame that never arrives)
// is fixed -- permanent, not diagnostic-only, not slated for deletion.
uint32_t uart_owner_get_tx_self_start_failures(void);

// 2026-08-23, coordinator's ring-pointer-bug hypothesis (round 6): DIAG and
// POWER now arrive at the correct 0.5/s cadence with valid CRCs, but every
// decoded DIAG carries byte-identical, frozen content (uptime_ms stuck at
// one value for 90+ seconds) despite the clock and diag_applied count both
// advancing normally. That is consistent with the ISR replaying the SAME
// ring window every time rather than genuinely new bytes: s_tx_tail stuck
// while s_tx_head keeps climbing, or the two aliasing back to the same
// index. These expose the RAW ring indices (not the derived "how full"
// uart_owner_get_tx_used() already gives) so successive SWD reads can be
// diffed directly against each other, exactly what the coordinator asked to
// compare over time. Also notable: uart_owner_get_tx_self_start_failures()
// now reads a permanent 0 -- the self-start-failure condition this file's
// forced re-arm exists for no longer occurs at all post-narrowing, which
// means priming alone (not the forced re-arm) is now the mechanism getting
// DIAG/POWER's bytes out. uart_owner_get_tx_last_primed_calls() is a
// monotonic per-call counter of the PRIMING loop specifically (distinct
// from s_tx_bytes_to_fifo, a byte count) -- if this is climbing at the same
// rate DIAG/POWER are sent, priming is genuinely running fresh each call
// (ruling out "the priming loop itself is being skipped and something else
// is replaying old FIFO state").
uint32_t uart_owner_get_tx_head(void);
uint32_t uart_owner_get_tx_tail(void);
uint32_t uart_owner_get_tx_last_primed_calls(void);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_TASKS_UART_OWNER_H
