/* hal_uart_pico.c -- pico-sdk backend for interface/hal_uart.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against the REAL
 * SaftyFW UART owner (firmware/SaftyFW/src/tasks/uart_owner.c/h): UART1 on
 * GPIO4 (TX) / GPIO5 (RX), 230400 baud, IRQ-driven RX ring + IRQ-drained TX
 * ring, never blocking a caller (docs/HW_ABSTRACTION_PLAN.md "hal_uart --
 * two primitives, ESP backend unchanged"). Not wired into any CMakeLists yet
 * -- see firmware/hwAbstraction/test/compile_pico_backends.ps1 for the
 * syntax-only compile check that stands in for that until Phase 1a's real
 * move lands.
 *
 * This file wraps uart_owner.c's PUBLIC functions only (uart_owner_init/
 * send/rx_read/get_tx_dropped/get_tx_used/get_tx_capacity) -- it does not
 * duplicate the ring/IRQ implementation. uart_owner.c stays exactly as it is
 * today (byte-identical body, per Phase 1a's move-only discipline; this file
 * is the Phase 1b adapter sitting in front of it) and keeps owning the real
 * hardware IRQ registration (irq_set_exclusive_handler(UART1_IRQ, ...)),
 * which per docs/HW_ABSTRACTION_PLAN.md's "hal_gpio -- clean-room; no IRQ
 * surface in v1" section and the tree-shape note ("uart_owner_tx_policy.c/h
 * stays with the pico backend as a PRIVATE include") is deliberately NOT
 * exposed through hal_uart.h.
 *
 * INTERFACE MISMATCH notes (docs/HW_ABSTRACTION_PLAN.md asks these to be
 * reported, not silently papered over by widening hal_uart.h):
 *
 * 1. Single fixed instance, not N independent handles. hal_uart_cfg_t
 *    carries port/tx_io/rx_io/baud so a backend can in principle stand up
 *    multiple independent hal_uart_t instances distinguished by port
 *    number. uart_owner.c has no such parameterization: UART1_IRQ,
 *    GPIO4/GPIO5 and 230400 baud are all compile-time constants
 *    (SAFTYFW_PIN_UART1_TX/_RX in board_pins.h, UART_OWNER_BAUD_RATE in
 *    uart_owner.c), and every ring/counter is a file-scope static, not a
 *    per-instance struct -- there is exactly ONE real UART1 link on this
 *    board (docs/ARCHITECTURE.md: UART0 is the separate, raw, HAL-external
 *    console path; see console_uart.c). This backend cannot honor a cfg
 *    describing a second, independent port -- there is only one real link
 *    -- so hal_uart_init() VALIDATES cfg against the owner's own constants
 *    (port against the uart1 index, tx_io/rx_io against board_pins.h's
 *    SAFTYFW_PIN_UART1_TX/_RX, baud against a local literal mirroring
 *    uart_owner.c's private UART_OWNER_BAUD_RATE, since uart_owner.h
 *    exposes no accessor for it) and returns HAL_INVALID_ARG on any
 *    mismatch, rather than silently discarding cfg and letting a caller
 *    believe a different port/pin/baud was honored. hal_uart_init is still
 *    only ever correct to call once, for the one real link, and hal_uart.h
 *    has no ALREADY_INIT-style contract for hal_uart_init the way
 *    hal_spi_bus_init/hal_i2c_bus_init do.
 * 2. hal_uart_get_rx_error_count() has no real backing counter. uart_owner.c
 *    tracks TX-side diagnostics in unusual depth (tx dropped, bytes to
 *    FIFO, bytes from ISR, self-start-failures, raw head/tail, priming
 *    calls -- see uart_owner.h's own extensive 2026-08-23 diagnostic
 *    comments) but has NO RX framing/overrun/break error counter at all;
 *    the RX ring simply overwrites the oldest byte on overflow
 *    (uart_owner_irq_handler()'s RX loop) with no counter incremented for
 *    that event either. This backend returns 0 unconditionally rather than
 *    inventing a counter uart_owner.c does not keep -- a future pass could
 *    add one to uart_owner.c itself (RX ring overflow is arguably the more
 *    interesting event to count), but that is a change to the real owner
 *    module, out of scope for this backend, which per Phase 1a is
 *    byte-identical.
 * 3. hal_uart_restart() has no real implementation to call. Per
 *    docs/HW_ABSTRACTION_PLAN.md's "Reset pairing, verified" section: "The
 *    Pico has no runtime RX reset at all (init-time zero only, uart_owner.c
 *    :255-256)." uart_owner.h/.c export no uart_owner_restart()-equivalent
 *    function whatsoever (unlike the ESP side's uart_owner_restart(), whose
 *    single caller is uart_bridge_system.c). This backend returns
 *    HAL_NOT_SUPPORTED rather than fabricating a reset uart_owner.c was
 *    never given, and never touches TX-side state either way, preserving
 *    hal_uart.h's RX-only contract by simply doing nothing.
 *
 * No mismatch for send/send_blocking/recv/get_tx_dropped: uart_owner_send()
 * is already exactly hal_uart_send's contract (non-blocking,
 * whole-buffer-or-drop, "the caller returns immediately"); send_blocking's
 * documented ring-drained-not-wire-complete semantics
 * (docs/HW_ABSTRACTION_PLAN.md, hal_uart.h's own header comment) are
 * implemented directly against uart_owner_get_tx_used() as specified there;
 * uart_owner_rx_read() is already exactly hal_uart_recv's contract
 * (non-blocking, returns 0..max bytes actually available); and
 * uart_owner_get_tx_dropped() is exactly hal_uart_get_tx_dropped's counter.
 */
#include "hal_uart.h"

/* TEMPORARY boundary violation: tasks/uart_owner.h is a SaftyFW header
 * (firmware/SaftyFW/src/tasks/), included here across the hwAbstraction/
 * pico boundary before uart_owner.c has actually moved into this tree. Per
 * docs/HW_ABSTRACTION_PLAN.md's "espInterfaces move set" / Phase 1a, the
 * SaftyFW move set (src/tasks/uart_owner.c/h, uart_owner_tx_policy.c/h)
 * relocates into firmware/hwAbstraction/pico/uart/ itself, at which point
 * this becomes a same-directory include and the violation disappears. Until
 * that move lands, this file wraps uart_owner.c's public API from outside
 * SaftyFW's own tree -- acceptable only as a stopgap; do not add further
 * SaftyFW-header includes to this backend on the strength of this
 * precedent. See docs/HW_ABSTRACTION_PLAN.md Phase 1a for the tracked
 * removal. */
#include "tasks/uart_owner.h"

#include "hal_time.h"
#include "board_pins.h"

/* No per-instance struct: uart_owner.c's state is entirely file-scope
 * statics (one real UART1 link), matching the pico gpio/adc backends'
 * documented under-fill of the opaque hal_uart_t storage. Nothing is stored
 * in u->storage; the pointer is used only to distinguish "was hal_uart_init
 * called" is NOT tracked either -- see INTERFACE MISMATCH note 1 above. */

hal_status_t hal_uart_init(hal_uart_t *u, const hal_uart_cfg_t *cfg) {
    (void)u;
    /* Per INTERFACE MISMATCH 1, uart_owner.c has exactly one real link and
     * every parameter is a compile-time constant -- there is nothing to
     * configure. But a cfg that does not describe THAT link is a caller
     * bug (a second, imagined instance), not something to silently
     * accept: validate cfg against the owner's own compile-time constants
     * rather than discarding it outright. `uart1` and `UART_NUM_1`-style
     * port identity is checked against pico-sdk's `uart1` global (the same
     * object uart_owner.c's private UART_OWNER_INSTANCE macro names);
     * tx_io/rx_io are checked against board_pins.h's
     * SAFTYFW_PIN_UART1_TX/_RX, the same header uart_owner.c itself
     * includes. Baud has no owner-exposed accessor (UART_OWNER_BAUD_RATE is
     * a private #define inside uart_owner.c, not in uart_owner.h) --
     * HAL_UART_PICO_EXPECTED_BAUD below duplicates that literal only
     * because there is no accessor to reference instead; it must be kept
     * equal to uart_owner.c's UART_OWNER_BAUD_RATE by hand until Phase 1a
     * moves uart_owner.c into this tree and the two constants merge into
     * one. */
#define HAL_UART_PICO_EXPECTED_PORT 1 /* uart1, per hal_uart_cfg_t's own doc
                                        * comment: "pico uart0/uart1 index" */
#define HAL_UART_PICO_EXPECTED_BAUD 230400u
    if (cfg != NULL) {
        if (cfg->port != HAL_UART_PICO_EXPECTED_PORT) {
            return HAL_INVALID_ARG;
        }
        if (cfg->tx_io != SAFTYFW_PIN_UART1_TX || cfg->rx_io != SAFTYFW_PIN_UART1_RX) {
            return HAL_INVALID_ARG;
        }
        if (cfg->baud != HAL_UART_PICO_EXPECTED_BAUD) {
            return HAL_INVALID_ARG;
        }
    }
    if (!uart_owner_init()) {
        return HAL_IO;
    }
    return HAL_OK;
}

hal_status_t hal_uart_deinit(hal_uart_t *u) {
    (void)u;
    /* uart_owner.c exports no teardown -- the real link is brought up once
     * at boot and lives for the process lifetime (docs/HW_ABSTRACTION_PLAN.md
     * boot-order section: uart_owner_init() runs from main() before the
     * scheduler starts, with no matching shutdown anywhere in SaftyFW). Not
     * an INTERFACE MISMATCH in the sense of "cannot be expressed" -- hal_uart
     * deinit is documented as "not called on any hot path today ... added
     * for lifecycle symmetry" and host/test teardown, neither of which
     * applies to this real hardware backend -- so this is a documented no-op,
     * not a missing capability that would need a mismatch note above. */
    return HAL_OK;
}

hal_status_t hal_uart_send(hal_uart_t *u, const uint8_t *data, size_t len) {
    (void)u;
    if (!uart_owner_send(data, len)) {
        /* uart_owner_send() returns false either for a bad argument
         * (data == NULL || len == 0) or because the TX ring did not have
         * room for the whole frame (dropped, s_tx_dropped incremented). The
         * two cases are not distinguished by uart_owner.c's return value;
         * HAL_BUSY is used here rather than HAL_INVALID_ARG because the
         * overwhelmingly real case (LINK_PROTOCOL.md section 2 rule 3) is
         * ring-full, and a caller checking hal_uart_get_tx_dropped() already
         * has the authoritative signal for that case regardless of which
         * status this call returns. */
        return HAL_BUSY;
    }
    return HAL_OK;
}

hal_status_t hal_uart_send_blocking(hal_uart_t *u, const uint8_t *data,
                                     size_t len, uint32_t timeout_ms) {
    hal_status_t st = hal_uart_send(u, data, len);
    if (st != HAL_OK) {
        return st;
    }

    /* Ring-drained-not-wire-complete, per hal_uart.h's own header comment
     * and docs/HW_ABSTRACTION_PLAN.md: waits for uart_owner_get_tx_used()
     * to reach 0 (the TX ISR has drained every queued byte into the
     * hardware FIFO), NOT for the PL011 to finish shifting the last byte
     * out onto the wire (that would need UARTFR.BUSY, which uart_owner.c
     * does not check anywhere and no real pico consumer needs -- see the
     * header comment's "no pico consumer originates an ACK-timed send"
     * reasoning: the Pico only ever answers BROADCAST with BROADCAST,
     * fire-and-forget, through link_task.c). uart_owner_get_tx_used() is a
     * plain snapshot read (its own doc comment: "a snapshot, not a
     * guarantee ... adequate"), safe to poll from this non-ISR context. */
    uint64_t deadline_ms = hal_time_now_ms() + (uint64_t)timeout_ms;
    while (uart_owner_get_tx_used() != 0) {
        if (hal_time_now_ms() >= deadline_ms) {
            return HAL_TIMEOUT;
        }
        /* Yield instead of busy-spinning: uart_owner_get_tx_used() is a
         * cheap snapshot read, but polling it in a tight loop still burns
         * a full CPU core doing nothing while the TX IRQ drains the ring in
         * the background. 1 ms is well under LINK_TASK_POLL_MS (10 ms) and
         * the ~345 ms safety-link reply budget, so this does not threaten
         * either. */
        hal_time_delay_ms(1);
    }
    return HAL_OK;
}

size_t hal_uart_recv(hal_uart_t *u, uint8_t *out, size_t max) {
    (void)u;
    return uart_owner_rx_read(out, max);
}

uint32_t hal_uart_get_rx_error_count(const hal_uart_t *u) {
    (void)u;
    /* No real counter exists to report -- see INTERFACE MISMATCH 2 above. */
    return 0;
}

uint32_t hal_uart_get_tx_dropped(const hal_uart_t *u) {
    (void)u;
    return uart_owner_get_tx_dropped();
}

hal_status_t hal_uart_restart(hal_uart_t *u) {
    (void)u;
    /* No real reset to call -- see INTERFACE MISMATCH 3 above. This never
     * touches TX-side state (it touches nothing at all), preserving the
     * RX-only contract by construction. */
    return HAL_NOT_SUPPORTED;
}
