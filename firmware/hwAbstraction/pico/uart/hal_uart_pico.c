/* hal_uart_pico.c -- pico-sdk backend for interface/hal_uart.h.
 *
 * Phase 1b ("adapt"): implements the Phase-0 interface against the REAL
 * SaftyFW UART owner (firmware/SaftyFW/src/tasks/uart_owner.c/h): UART1 on
 * GPIO4 (TX) / GPIO5 (RX), 230400 baud, IRQ-driven RX ring + IRQ-drained TX
 * ring, never blocking a caller (docs/HW_ABSTRACTION.md "hal_uart --
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
 * which per docs/HW_ABSTRACTION.md's "hal_gpio -- clean-room; no IRQ
 * surface in v1" section and the tree-shape note ("uart_owner_tx_policy.c/h
 * stays with the pico backend as a PRIVATE include") is deliberately NOT
 * exposed through hal_uart.h.
 *
 * INTERFACE MISMATCH notes (docs/HW_ABSTRACTION.md asks these to be
 * reported, not silently papered over by widening hal_uart.h):
 *
 * 1. Single fixed instance, not N independent handles. hal_uart_cfg_t
 *    carries port/tx_io/rx_io/baud so a backend can in principle stand up
 *    multiple independent hal_uart_t instances distinguished by port
 *    number. uart_owner.c has no such parameterization: UART1_IRQ and
 *    230400 baud are compile-time constants (UART_OWNER_BAUD_RATE in
 *    uart_owner.c) and every ring/counter is a file-scope static, not a
 *    per-instance struct -- there is exactly ONE real UART1 link on this
 *    board (docs/ARCHITECTURE.md: UART0 is the separate, raw, HAL-external
 *    console path; see console_uart.c). This backend cannot honor a cfg
 *    describing a second, independent port -- there is only one real link
 *    -- so hal_uart_init() VALIDATES port and baud against the owner's own
 *    constants (port against the uart1 index, baud against a local literal
 *    mirroring uart_owner.c's private UART_OWNER_BAUD_RATE, since
 *    uart_owner.h exposes no accessor for it) and returns HAL_INVALID_ARG on
 *    any mismatch, rather than silently discarding cfg and letting a caller
 *    believe a different port/baud was honored. tx_io/rx_io are NOT
 *    validated against anything here (HAL Phase 1b, "close the upward
 *    include": this backend no longer has its own compile-time notion of
 *    the right pins -- board_pins.h moved out of this file entirely) -- they
 *    are forwarded straight into uart_owner_init() as-is, so a cfg naming
 *    the wrong pins is genuinely honored (and genuinely wrong), not silently
 *    discarded. hal_uart_init is still only ever correct to call once, for
 *    the one real link, and hal_uart.h has no ALREADY_INIT-style contract
 *    for hal_uart_init the way hal_spi_bus_init/hal_i2c_bus_init do.
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
 *    docs/HW_ABSTRACTION.md's "Reset pairing, verified" section: "The
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
 * (docs/HW_ABSTRACTION.md, hal_uart.h's own header comment) are
 * implemented directly against uart_owner_get_tx_used() as specified there;
 * uart_owner_rx_read() is already exactly hal_uart_recv's contract
 * (non-blocking, returns 0..max bytes actually available); and
 * uart_owner_get_tx_dropped() is exactly hal_uart_get_tx_dropped's counter.
 *
 * 4. hal_uart_cfg_t's queue_len/task_priority/stack_depth/core_id fields (added
 *    alongside hal_uart_get_task_handle()) describe an owner/event TASK's
 *    sizing. uart_owner.h's own header comment is explicit that this is NOT
 *    that shape at all: RX is IRQ-driven into a ring link_task polls, and TX
 *    is drained by the SAME UART1 IRQ handler as FIFO space frees up -- there
 *    is no FreeRTOS task here to size, prioritize, or pin a core to (the
 *    "single save_and_disable_interrupts() critical section is sufficient"
 *    reasoning in that header depends on there being no second task or core
 *    involved at all). hal_uart_init() therefore requires all four fields be
 *    0 ("backend default", which for this backend means "N/A, not a task")
 *    and returns HAL_INVALID_ARG for any nonzero value, rather than silently
 *    accepting sizing parameters for a task that will never exist.
 * 5. hal_uart_get_task_handle() has no real handle to return -- see note 4:
 *    this backend has no owner task at all. Returns NULL unconditionally,
 *    which hal_uart.h's own doc comment on this function explicitly allows
 *    ("a backend with no owner task at all, e.g. host/pico today").
 */
#include "hal_uart.h"

/* HAL Phase 1a (docs/HW_ABSTRACTION.md) moved uart_owner.c/h and
 * uart_owner_tx_policy.c/h from firmware/SaftyFW/src/tasks/ into this same
 * directory (uart_owner.h renamed to hal_uart_pico_internal.h to avoid
 * colliding with the ESP-side uart_owner.h moving into
 * firmware/hwAbstraction/esp/uart/ under the same effort), so the former
 * TEMPORARY upward include of a SaftyFW header from outside SaftyFW's own
 * tree is now an ordinary same-directory include. */
#include "hal_uart_pico_internal.h"

#include "hal_time.h"
/* HAL Phase 1b, "close the upward include" (docs/HW_ABSTRACTION.md):
 * this used to #include "board_pins.h" (a SaftyFW header), same as
 * uart_owner.c's former identical note. Pin values now arrive via
 * hal_uart_cfg_t at hal_uart_init() time instead and are forwarded straight
 * to uart_owner_init(), never compared against a compile-time constant this
 * file no longer has. */

/* No per-instance struct: uart_owner.c's state is entirely file-scope
 * statics (one real UART1 link), matching the pico gpio/adc backends'
 * documented under-fill of the opaque hal_uart_t storage. Nothing is stored
 * in u->storage; the pointer is used only to distinguish "was hal_uart_init
 * called" is NOT tracked either -- see INTERFACE MISMATCH note 1 above. */

hal_status_t hal_uart_init(hal_uart_t *u, const hal_uart_cfg_t *cfg) {
    (void)u;
    /* Per INTERFACE MISMATCH 1, uart_owner.c has exactly one real link and
     * port/baud are compile-time constants -- there is nothing to configure
     * for those two. But a cfg that does not describe THAT link's port/baud
     * is a caller bug (a second, imagined instance), not something to
     * silently accept: validate those two against the owner's own
     * compile-time constants rather than discarding them outright. `uart1`
     * and `UART_NUM_1`-style port identity is checked against pico-sdk's
     * `uart1` global (the same object uart_owner.c's private
     * UART_OWNER_INSTANCE macro names). Baud has no owner-exposed accessor
     * (UART_OWNER_BAUD_RATE is a private #define inside uart_owner.c, not in
     * uart_owner.h) -- HAL_UART_PICO_EXPECTED_BAUD below duplicates that
     * literal only because there is no accessor to reference instead; it
     * must be kept equal to uart_owner.c's UART_OWNER_BAUD_RATE by hand.
     * tx_io/rx_io are NOT validated (see INTERFACE MISMATCH 1's updated
     * text above) -- a non-NULL cfg is REQUIRED so this file has pins to
     * forward at all; there is no compile-time default left to fall back
     * to now that board_pins.h has moved out of this file. */
#define HAL_UART_PICO_EXPECTED_PORT 1 /* uart1, per hal_uart_cfg_t's own doc
                                        * comment: "pico uart0/uart1 index" */
#define HAL_UART_PICO_EXPECTED_BAUD 230400u
    if (cfg == NULL) {
        return HAL_INVALID_ARG;
    }
    if (cfg->port != HAL_UART_PICO_EXPECTED_PORT) {
        return HAL_INVALID_ARG;
    }
    if (cfg->baud != HAL_UART_PICO_EXPECTED_BAUD) {
        return HAL_INVALID_ARG;
    }
    /* Per INTERFACE MISMATCH 4: this backend has no owner task at all,
     * so queue_len/task_priority/stack_depth must be left at "backend
     * default" (0, per hal_uart_cfg_t's own doc comment) -- a nonzero
     * value describes a task that will never be created. core_id is
     * NOT gated here: hal_uart_cfg_t's own doc comment says "pico/host
     * backends ignore this" (it is not a 0-means-default field, it is a
     * HAL_CORE_ANY-or-explicit-core field the pico backend has no use
     * for at all, same as hal_uart_get_task_handle() having no task to
     * pin). */
    if (cfg->queue_len != 0 || cfg->task_priority != 0 || cfg->stack_depth != 0) {
        return HAL_INVALID_ARG;
    }

    const uart_owner_pins_t pins = {
        .tx_pin = (uint8_t)cfg->tx_io,
        .rx_pin = (uint8_t)cfg->rx_io,
    };
    if (!uart_owner_init(&pins)) {
        return HAL_IO;
    }
    return HAL_OK;
}

hal_status_t hal_uart_deinit(hal_uart_t *u) {
    (void)u;
    /* uart_owner.c exports no teardown -- the real link is brought up once
     * at boot and lives for the process lifetime (docs/HW_ABSTRACTION.md
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
     * and docs/HW_ABSTRACTION.md: waits for uart_owner_get_tx_used()
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

size_t hal_uart_recv_blocking(hal_uart_t *u, uint8_t *buf, size_t cap,
                               uint32_t timeout_ms) {
    if (u == NULL || buf == NULL || cap == 0) {
        return 0;
    }
    /* uart_owner_rx_read() is already non-blocking (INTERFACE section above)
     * with no blocking sibling of its own -- this backend builds the bounded
     * wait on top by polling it, same shape as hal_uart_send_blocking()'s
     * poll-uart_owner_get_tx_used() loop above, and for the same reason: a
     * cheap ring-state read is fine to poll from ordinary task context, and
     * 1 ms is well under both LINK_TASK_POLL_MS (10 ms) and the ~345 ms
     * safety-link reply budget. Ends the wait as soon as ANY bytes are
     * available, per hal_uart_recv_blocking's own contract -- never waits to
     * fill cap. */
    uint64_t deadline_ms = hal_time_now_ms() + (uint64_t)timeout_ms;
    for (;;) {
        size_t got = hal_uart_recv(u, buf, cap);
        if (got > 0) {
            return got;
        }
        if (hal_time_now_ms() >= deadline_ms) {
            return 0;
        }
        hal_time_delay_ms(1);
    }
}

void *hal_uart_get_task_handle(const hal_uart_t *u) {
    (void)u;
    /* See INTERFACE MISMATCH 5 above: uart_owner.c has no FreeRTOS task at
     * all (IRQ-driven RX ring + IRQ-drained TX ring), so there is no handle
     * to return. NULL is an explicitly documented valid answer for this
     * case per hal_uart_get_task_handle's own contract in hal_uart.h. */
    return NULL;
}
