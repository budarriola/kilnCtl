// uart_owner.c -- see hal_uart_pico_internal.h. Raw-byte RX/TX rings over
// UART1, IRQ driven both ways, never blocking a caller.
//
// Moved here from firmware/SaftyFW/src/tasks/ by HAL Phase 1a
// (docs/HW_ABSTRACTION_PLAN.md), body byte-identical apart from include
// paths. uart_owner.h renamed to hal_uart_pico_internal.h in the same move
// to avoid colliding with the ESP-side uart_owner.h moving into
// firmware/hwAbstraction/esp/uart/ under the same Phase 1a effort.
#include "hal_uart_pico_internal.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"
#include "hardware/regs/uart.h"

// TEMPORARY (HAL Phase 1b): board_pins.h is a SaftyFW header
// (firmware/SaftyFW/src/board/board_pins.h), not part of hwAbstraction/. Left as
// a same-name include resolved via SaftyFW's own include path (this file
// is compiled into the hwabstraction_pico library, which SaftyFW's
// CMakeLists.txt gives a private include dir on firmware/SaftyFW/src for
// exactly this) until Phase 1b introduces a board-descriptor header inside
// hwAbstraction/pico/ itself (see esp/board_kiln_s3.h's analogous role).
#include "board_pins.h"
#include "uart_owner_tx_policy.h"

#define UART_OWNER_INSTANCE   uart1
#define UART_OWNER_IRQ        UART1_IRQ
// Must stay equal to KilnFW's CONFIG_KILNCTL_SAFETY_BAUD_RATE -- both sides
// hardcode it, there is no negotiation. Keep the bootloader's recovery-mode
// uart_init (bootloader/main.c) equal to it as well.
//
// RAISED from 9600 on 2026-08-25. The 9600 figure was never a property of
// either firmware: it was the TCMT1109 optocouplers, which could not switch
// fast enough for a 8.7 us bit (at 115200 and 57600 not one frame ever
// arrived, 38400 lost about a fifth, 19200 lost about a tenth over minutes).
// Those parts are gone -- the barrier is now one ADuM1201WT digital isolator,
// U6, whose speed grade is orders of magnitude above anything a UART needs
// here. The measured sweep that chose the value below is recorded in
// KilnFW/App/drivers/Kconfig under KILNCTL_SAFETY_BAUD_RATE.
//
// Worth keeping: DC level tests pass in both directions at ANY baud rate,
// because both an optocoupler and a digital isolator carry a static level
// perfectly well. A bench GPIO high/low check across this pair proves the
// wiring and proves nothing whatever about the rate it can carry.
#define UART_OWNER_BAUD_RATE  230400u

// Sized to hold one worst-case stuffed frame outright --
// KILNLINK_FRAME_STUFFED_MAX is 528 bytes (header 8 + payload up to 253 + crc
// 2 = 263 raw, which stuffing can nearly double, plus two delimiters), so
// 1024 clears it with room for the payload bound to grow.
//
// This was 128, sized against "this link's actual traffic" when that meant
// Frame A and FW_VERSION, both comfortably under 70 raw bytes. It stopped
// being true when CONFIG_PAGE arrived: a full page is 4 + 32*7 = 228 raw
// bytes of payload, about 470 stuffed, which is nearly four times the whole
// ring. link_task_send_broadcast_to() builds the entire stuffed frame and
// hands it to uart_owner_send() in one call, and uart_owner_send() is
// all-or-nothing -- if the frame does not fit it drops the whole thing and
// increments s_tx_dropped. So every GET_CONFIG_PAGE reply was discarded
// before a single byte reached the FIFO, always, regardless of timing or
// contention with the 500 ms status broadcast.
//
// The ESP saw that as safety_cfg_store_refetch() timing out forever on a
// link whose telemetry was perfectly healthy -- small frames fit, this one
// never could. s_tx_dropped was counting it the whole time and is carried in
// the DIAG frame (link_task.c's .tx_frames_dropped), but nothing on the ESP
// side raises it as a symptom, so it read as a mystery timeout rather than
// "the far end could not fit the reply".
//
// Cost of the change is 896 bytes of static RAM on a chip with 264 KB.
#define UART_OWNER_TX_RING_SIZE  1024u
// RX has no hard per-frame bound to plan against (arbitrary noise/half-frames
// can arrive) -- sized generously so a burst of garbage does not immediately
// cost a real frame that happens to follow it within one link_task poll.
#define UART_OWNER_RX_RING_SIZE  256u

static uint8_t s_tx_ring[UART_OWNER_TX_RING_SIZE];
static volatile uint32_t s_tx_head = 0; // next free slot to write into (producer: uart_owner_send)
static volatile uint32_t s_tx_tail = 0; // next byte to drain (consumer: the ISR)

static uint8_t s_rx_ring[UART_OWNER_RX_RING_SIZE];
static volatile uint32_t s_rx_head = 0; // next free slot to write into (producer: the ISR)
static volatile uint32_t s_rx_tail = 0; // next byte to drain (consumer: uart_owner_rx_read)

static volatile uint32_t s_tx_dropped = 0;
// Diagnostic (2026-08-23): total bytes actually written to the UART data
// register, split by who wrote them. Queued-but-never-sent is the failure
// this pair exists to make visible -- compare against what link_task says it
// handed over. Removable once the TX path has been trusted for a while.
static volatile uint32_t s_tx_bytes_to_fifo = 0;
static volatile uint32_t s_tx_bytes_from_isr = 0;

// Diagnostic (2026-08-23, size-window investigation): the coordinator's own
// reading of the histogram narrows the DIAG/POWER-never-arrive symptom to a
// size boundary between 34 and 36 raw bytes -- suspiciously close to the
// RP2040 PL011's 32-byte TX FIFO depth, and exactly where uart_owner_send()'s
// same-day priming write operates (see that function's own comment). These
// three latch, for the MOST RECENT uart_owner_send() call only, exactly what
// that call did: how many bytes it asked to send, how many the priming loop
// got directly into the hardware FIFO before uart_is_writable() went false,
// and how many were left queued in the ring for the ISR to pick up. Read
// alongside s_tx_bytes_from_isr (above): if s_tx_last_remainder is nonzero
// after a DIAG/POWER send but s_tx_bytes_from_isr never climbs afterward,
// that is direct proof the ISR is not draining the remainder for THIS call
// -- the exact mechanism the coordinator's lead points at. Cross-reference
// against link_task.c's own s_last_tx_cmd (same diagnostic generation,
// latched at the same call) to know which command this was.
// `volatile`, nothing in firmware reads these back -- SWD only. Safe to
// delete once the size-window question is settled.
static volatile uint32_t s_tx_last_send_len = 0;   // bytes uart_owner_send() was asked to queue
static volatile uint32_t s_tx_last_primed = 0;     // of those, how many the priming loop wrote directly to the HW FIFO this call
static volatile uint32_t s_tx_last_remainder = 0;  // len - primed: bytes left in the ring, dependent on the ISR to ever leave
// Times uart_owner_irq_handler() ran with TX bytes still pending at entry
// (s_tx_tail != s_tx_head) -- distinguishes "the ISR never ran again after
// arming" (this stays flat) from "it ran but uart_is_writable() was false
// the whole time" (this climbs, s_tx_bytes_from_isr does not).
static volatile uint32_t s_tx_isr_pending_entries = 0;

// 2026-08-23, root cause found: the counter above (s_tx_isr_pending_entries)
// and its siblings were what pinned this down. This is the fix's own
// visibility counter -- see uart_owner_tx_policy.h's header comment for the
// full mechanism. Kept permanently (not diagnostic-only, not slated for
// deletion): a send that has to force its own re-arm is rare-but-legitimate
// under real traffic (any two sends close enough together that the second's
// FIFO write finds it still full from the first), and this is the one place
// that condition is visible after the fix -- exactly the discipline
// requested: the symptom is fixed, the CONDITION stays observable rather
// than going dark again.
static volatile uint32_t s_tx_self_start_failures = 0;

// 2026-08-23, coordinator's ring-pointer-bug hypothesis (round 6): DIAG/POWER
// arrive at the right cadence with valid CRCs but frozen, byte-identical
// content -- consistent with the ISR replaying the same ring window rather
// than genuinely advancing. This counts how many times uart_owner_send()'s
// PRIMING loop actually ran its body at least once (primed_this_call > 0),
// separate from s_tx_bytes_to_fifo (a byte total, which climbs even if the
// same bytes were somehow re-primed). If this is NOT climbing at the same
// rate DIAG/POWER sends happen, priming itself is being skipped for them --
// if it IS climbing correctly, the fault is downstream of priming.
static volatile uint32_t s_tx_priming_calls = 0;

// 2026-08-23, coordinator's follow-up: the fix above gets bytes OUT of the
// ring (self_start_failures counts right, the ring empties) but the ESP
// still counts zero DIAG/POWER frames -- so either the bytes that leave are
// wrong, or something after uart_owner.c mangles them. These two buffers let
// that be checked directly rather than argued: `expected` is a snapshot of
// the first bytes actually QUEUED for a self-start-failure frame, taken here
// in uart_owner_send() the moment the failure is detected (the frame's true
// first byte MUST be KILNLINK_FRAME_DELIM, 0x7E, if the ring's stuffed
// content is intact); `actual` is what uart_owner_irq_handler() really wrote
// to UARTDR the very next time it drains this frame's bytes. If the two
// differ, the corruption is inside uart_owner.c (the ring or the ISR write
// itself); if they match, the bytes left this file correctly and the fault
// is downstream (the physical link, or the ESP's own deframer). Captured
// once per self-start-failure event (s_tx_trace_pending gates it so a
// LATER, unrelated ISR drain -- e.g. status's own next cycle -- can't
// overwrite `actual` before the coordinator reads it). SWD-only, safe to
// delete once this question is settled.
#define UART_OWNER_TRACE_LEN 8u
static volatile uint8_t  s_tx_trace_expected[UART_OWNER_TRACE_LEN];
static volatile uint8_t  s_tx_trace_actual[UART_OWNER_TRACE_LEN];
static volatile uint32_t s_tx_trace_expected_len = 0;
static volatile uint32_t s_tx_trace_actual_len = 0;
static volatile bool     s_tx_trace_pending = false; // true from the moment a self-start-failure is detected until the ISR has captured `actual`

static inline uint32_t ring_used(uint32_t head, uint32_t tail, uint32_t cap)
{
    return (head - tail) % cap;
}

static inline uint32_t ring_free(uint32_t head, uint32_t tail, uint32_t cap)
{
    // One slot deliberately left unused (head == tail always means "empty",
    // never "full") -- the standard head/tail ring trick, avoids needing a
    // separate count variable that both sides would have to keep in sync.
    return cap - 1u - ring_used(head, tail, cap);
}

static void uart_owner_irq_handler(void)
{
    // RX: drain the hardware FIFO into the ring. If the ring is full, the
    // oldest unread bytes are overwritten (head keeps advancing, tail is
    // dragged forward with it) rather than the ISR blocking or dropping the
    // new byte -- LINK_PROTOCOL.md section 3's "newer context is strictly
    // more useful than older context" applies here too: a link_task that has
    // fallen behind should see the freshest bytes, not get stuck replaying
    // stale ones.
    while (uart_is_readable(UART_OWNER_INSTANCE)) {
        uint8_t c = (uint8_t)uart_getc(UART_OWNER_INSTANCE);
        uint32_t head = s_rx_head;
        uint32_t next = (head + 1u) % UART_OWNER_RX_RING_SIZE;
        if (next == s_rx_tail) {
            // Full: advance tail too, discarding the oldest byte.
            s_rx_tail = (s_rx_tail + 1u) % UART_OWNER_RX_RING_SIZE;
        }
        s_rx_ring[head] = c;
        s_rx_head = next;
    }

    // TX: push as many queued bytes as the hardware FIFO will currently
    // accept. Once the ring is empty, disable the TX interrupt -- it is
    // level-triggered on FIFO-below-threshold, so leaving it enabled with
    // nothing left to send would fire continuously.
    if (s_tx_tail != s_tx_head) {
        s_tx_isr_pending_entries++; // 2026-08-23 diagnostic, see statics above
    }
    while (uart_is_writable(UART_OWNER_INSTANCE) && s_tx_tail != s_tx_head) {
        uint8_t b = s_tx_ring[s_tx_tail];
        // --- DIAGNOSTIC: 2026-08-23, coordinator's byte-integrity check --
        // captures the first UART_OWNER_TRACE_LEN bytes this ISR invocation
        // actually writes to UARTDR while a self-start-failure trace is
        // pending, for direct comparison against s_tx_trace_expected (see
        // that static's own comment in uart_owner_send()). Cleared
        // (s_tx_trace_pending = false) once full so it captures exactly one
        // event at a time and never overwrites itself mid-frame.
        if (s_tx_trace_pending && s_tx_trace_actual_len < UART_OWNER_TRACE_LEN) {
            s_tx_trace_actual[s_tx_trace_actual_len] = b;
            s_tx_trace_actual_len++;
            if (s_tx_trace_actual_len >= UART_OWNER_TRACE_LEN ||
                s_tx_trace_actual_len >= s_tx_trace_expected_len) {
                s_tx_trace_pending = false;
            }
        }
        // --- end diagnostic ---
        uart_get_hw(UART_OWNER_INSTANCE)->dr = b;
        s_tx_tail = (s_tx_tail + 1u) % UART_OWNER_TX_RING_SIZE;
        s_tx_bytes_from_isr++;
    }
    if (s_tx_tail == s_tx_head) {
        uart_set_irq_enables(UART_OWNER_INSTANCE, true, false);
    }
}

bool uart_owner_init(void)
{
    uart_init(UART_OWNER_INSTANCE, UART_OWNER_BAUD_RATE);
    gpio_set_function(SAFTYFW_PIN_UART1_TX, GPIO_FUNC_UART);
    gpio_set_function(SAFTYFW_PIN_UART1_RX, GPIO_FUNC_UART);

    // NO line inversion on this pin -- deliberately, and it must stay that
    // way unless the part on the board changes back.
    //
    // Until 2026-08-25 this line carried gpio_set_outover(GPIO_OVERRIDE_INVERT)
    // so that U3's optocoupler LED sat dark at idle instead of burning current
    // around the clock. That inversion also happened to cancel the opto's own
    // inversion, giving exactly one inversion per direction end to end.
    //
    // The optocouplers (U2/U3, with R7/R12/R15) are gone. The barrier is now
    // one ADuM1201WT digital isolator, U6, and it does NOT invert: a high at
    // its input pin is a high at the matching output pin. There is no LED to
    // keep dark and no inversion to cancel, so inverting here would simply
    // deliver every byte upside down. With the isolator fitted and this
    // override still in place the ESP counted 33 framing errors and zero
    // received frames, and the Pico's own RX pad read low on 2954 of 3000
    // samples -- a permanent break rather than an idle line.
    //
    // Kept in step with the ESP: safety_link.c applies neither TXD_INV nor
    // RXD_INV. Both ends non-inverting, or both inverting -- never one of each.
    // Plain hardware UART, no inversion, no PIO, and none needed on either
    // end now; see the comment above.
    uart_set_hw_flow(UART_OWNER_INSTANCE, false, false);
    uart_set_format(UART_OWNER_INSTANCE, 8, 1, UART_PARITY_NONE);
    uart_set_fifo_enabled(UART_OWNER_INSTANCE, true);

    s_tx_head = 0;
    s_tx_tail = 0;
    s_rx_head = 0;
    s_rx_tail = 0;
    s_tx_dropped = 0;

    irq_set_exclusive_handler(UART_OWNER_IRQ, uart_owner_irq_handler);
    irq_set_enabled(UART_OWNER_IRQ, true);
    // RX interrupt always on; TX interrupt only enabled while there is
    // something queued to send (the ISR itself turns it back off).
    uart_set_irq_enables(UART_OWNER_INSTANCE, true, false);

    return true;
}

bool uart_owner_send(const uint8_t *data, size_t len)
{
    if (data == NULL || len == 0) {
        return false;
    }

    uint32_t save = save_and_disable_interrupts();

    uint32_t head = s_tx_head;
    uint32_t tail = s_tx_tail;
    if (ring_free(head, tail, UART_OWNER_TX_RING_SIZE) < len) {
        // Not enough room for the WHOLE frame -- drop it entirely rather
        // than writing a truncated frame the far end could misparse as a
        // shorter, differently-shaped one. LINK_PROTOCOL.md section 2 rule
        // 3.
        s_tx_dropped++;
        restore_interrupts(save);
        return false;
    }

    for (size_t i = 0; i < len; i++) {
        s_tx_ring[head] = data[i];
        head = (head + 1u) % UART_OWNER_TX_RING_SIZE;
    }
    s_tx_head = head;

    // Prime the hardware FIFO here rather than waiting for the TX interrupt
    // to do all of it.
    //
    // The PL011's TX interrupt is raised when the FIFO level passes DOWN
    // THROUGH the trigger level -- it is a transition, not a standing "there
    // is room" condition. Arming the interrupt while the FIFO is already
    // empty therefore does not necessarily produce one: with nothing in the
    // FIFO there is no level left to fall through, so the handler that would
    // have pushed the first bytes may never run, and whatever is sitting in
    // the ring stays there until some later send happens to re-arm it at a
    // moment when the transition does occur.
    //
    // Frames up to the 32-byte FIFO depth were unaffected, which is why this
    // went unnoticed for so long: the status broadcast is 35 bytes stuffed
    // and effectively always got out. The first frame big enough to need
    // several refills -- the 188-byte config-page reply -- did not. Observed
    // on the bench 2026-08-23: the RP2040 queued all 188 bytes and reported
    // the send accepted, while the ESP assembled only 140-142 of the 186 raw
    // bytes before the next frame's delimiter closed it, every time.
    //
    // Writing directly here means the first bytes always leave, and the FIFO
    // is left full enough that draining it genuinely does cross the trigger
    // level and keep the handler firing for the rest.
    //
    // 2026-08-23, size-window follow-up: the claim two paragraphs up ("the
    // 188-byte config-page reply... did not [get out]... this fixes it") was
    // never actually re-confirmed against a real on-wire capture after this
    // priming write was added -- link_task.c's own s_last_page_* diagnostic
    // block (link_task_send_config_page(), added the SAME day to chase a
    // *different* symptom) stayed all-zero for the rest of that
    // investigation, because GET_CONFIG_PAGE stopped reaching this function
    // at all for an unrelated dispatch-side reason. So this fix shipped on
    // the strength of the FIRST bench observation (truncation) plus a
    // plausible mechanism, not a confirmed-fixed second observation. DIAG
    // (36 raw / ~38 stuffed) and POWER (65 raw / ~67 stuffed) are both
    // BROADCAST frames through this exact function, same as CONFIG_PAGE, so
    // if the remainder below is genuinely never picked up by the ISR for
    // some size- or timing-dependent reason this write alone doesn't cover,
    // it would look exactly like what is currently observed: accepted here,
    // never on the wire. primed/remainder below make that directly
    // observable instead of inferred.
    uint32_t primed_this_call = 0;
    while (uart_is_writable(UART_OWNER_INSTANCE) && s_tx_tail != s_tx_head) {
        uart_get_hw(UART_OWNER_INSTANCE)->dr = s_tx_ring[s_tx_tail];
        s_tx_tail = (s_tx_tail + 1u) % UART_OWNER_TX_RING_SIZE;
        s_tx_bytes_to_fifo++;
        primed_this_call++;
    }
    if (primed_this_call > 0) {
        s_tx_priming_calls++; // 2026-08-23 diagnostic, see static's own comment
    }

    // --- DIAGNOSTIC: 2026-08-23 size-window investigation, see statics
    // declared above uart_owner_irq_handler() -- pure recording, no
    // control-flow effect.
    s_tx_last_send_len = (uint32_t)len;
    s_tx_last_primed = primed_this_call;
    uint32_t remainder = (uint32_t)len - primed_this_call;
    s_tx_last_remainder = remainder;
    // --- end diagnostic ---

    // 2026-08-23, ROOT CAUSE FIX (the DIAG/POWER-went-dark hunt): if this
    // call primed NOTHING (the FIFO was already full when it started, e.g.
    // link_task_fn()'s status -> diag -> power sequence running back-to-back
    // in the same loop iteration -- status always primes first and leaves
    // the FIFO full, so diag and power always find it full and never get a
    // byte in edgewise) yet left a nonzero remainder, simply re-writing
    // uart_set_irq_enables() with the SAME "enabled" value it already had
    // does not reliably produce a fresh interrupt once the FIFO later drains
    // -- that was the actual mechanism: the frame sat in the ring forever,
    // accepted, undropped, uncorrupted, and simply never transmitted. See
    // uart_owner_tx_policy.h for the pure (host-tested) classification this
    // branches on, and the narrowed-fix comment inside the `if` below for
    // exactly what forcing the transition now does (revised once already --
    // this does NOT busy-wait for FIFO space either way, both mechanisms are
    // plain register writes).
    //
    // 2026-08-23 FOLLOW-UP: forcing the transition got bytes out of the ring
    // (self_start_failures counts correctly, the ring empties) but did NOT
    // make the frames arrive at the ESP -- so either the ORIGINAL shape of
    // this fix (uart_set_irq_enables() called twice) was corrupting
    // something via a side effect, or the fault is entirely downstream of
    // this file. Narrowed below to rule out the former as far as possible;
    // s_tx_trace_expected/s_tx_trace_actual (see their own comment) are what
    // will show whether it was.
    bool self_start_failure = uart_owner_tx_send_is_self_start_failure(primed_this_call, remainder);
    if (self_start_failure) {
        s_tx_self_start_failures++;

        // --- DIAGNOSTIC: 2026-08-23, coordinator's byte-integrity check --
        // snapshot the first bytes actually sitting in the ring starting at
        // s_tx_tail (unchanged by this call, since primed_this_call == 0
        // means the tail never moved) -- this is exactly what the ISR must
        // write first. Only armed if nothing else is already pending a
        // capture, so a burst (diag immediately followed by power, both
        // self-start failures in the same loop iteration) does not let
        // power's capture silently overwrite diag's before the ISR ever
        // gets to diag's bytes.
        if (!s_tx_trace_pending) {
            uint32_t n = remainder < UART_OWNER_TRACE_LEN ? remainder : UART_OWNER_TRACE_LEN;
            uint32_t t = s_tx_tail;
            for (uint32_t i = 0; i < n; i++) {
                s_tx_trace_expected[i] = s_tx_ring[t];
                t = (t + 1u) % UART_OWNER_TX_RING_SIZE;
            }
            s_tx_trace_expected_len = n;
            s_tx_trace_actual_len = 0;
            s_tx_trace_pending = true;
        }
        // --- end diagnostic ---

        // 2026-08-23, ROOT CAUSE FIX, narrowed per coordinator's follow-up:
        // this used to call the SDK's uart_set_irq_enables() twice (disable,
        // then re-enable) to force the interrupt-enable bit to make a real
        // transition -- see uart_owner_tx_policy.h for why a transition is
        // needed at all. That helper (pico-sdk hardware/uart.h,
        // uart_set_irqs_enabled()) does more than write IMSC.TXIM though: it
        // reconstructs the ENTIRE imsc register from scratch (RXIM/RTIM
        // included, rewritten to the same value they already held) and,
        // whenever tx_needs_data is true, ALSO rewrites UARTIFLS's
        // TXIFLSEL bits via hw_write_masked. None of that is needed to
        // produce the one transition this fix actually depends on, and
        // touching more than necessary is exactly the kind of side effect
        // the coordinator asked to rule out. hw_clear_bits()/hw_set_bits()
        // (hardware/address_mapped.h) toggle ONLY the TXIM bit, atomically,
        // via the RP2040's hardware SET/CLR alias registers -- no RXIM/RTIM
        // rewrite, no IFLS touch here at all (the unconditional call two
        // lines below still does its own single IFLS rewrite exactly as it
        // always has, unchanged).
        hw_clear_bits(&uart_get_hw(UART_OWNER_INSTANCE)->imsc, UART_UARTIMSC_TXIM_BITS);
        hw_set_bits(&uart_get_hw(UART_OWNER_INSTANCE)->imsc, UART_UARTIMSC_TXIM_BITS);
    }

    // Arm the drain interrupt only if anything is actually left; arming it
    // with an empty ring is what the handler's own tail-end check undoes
    // anyway.
    uart_set_irq_enables(UART_OWNER_INSTANCE, true, s_tx_tail != s_tx_head);

    restore_interrupts(save);
    return true;
}

size_t uart_owner_rx_read(uint8_t *out, size_t max)
{
    if (out == NULL || max == 0) {
        return 0;
    }

    uint32_t save = save_and_disable_interrupts();

    uint32_t head = s_rx_head;
    uint32_t tail = s_rx_tail;
    size_t available = ring_used(head, tail, UART_OWNER_RX_RING_SIZE);
    size_t n = (available < max) ? available : max;

    for (size_t i = 0; i < n; i++) {
        out[i] = s_rx_ring[tail];
        tail = (tail + 1u) % UART_OWNER_RX_RING_SIZE;
    }
    s_rx_tail = tail;

    restore_interrupts(save);
    return n;
}

uint32_t uart_owner_get_tx_dropped(void)
{
    return s_tx_dropped;
}

size_t uart_owner_get_tx_used(void)
{
    uint32_t save = save_and_disable_interrupts();
    uint32_t head = s_tx_head;
    uint32_t tail = s_tx_tail;
    restore_interrupts(save);
    return ring_used(head, tail, UART_OWNER_TX_RING_SIZE);
}

size_t uart_owner_get_tx_capacity(void)
{
    return UART_OWNER_TX_RING_SIZE;
}

size_t uart_owner_get_last_send_remainder(void)
{
    return (size_t)s_tx_last_remainder;
}

uint32_t uart_owner_get_tx_bytes_from_isr(void)
{
    return s_tx_bytes_from_isr;
}

uint32_t uart_owner_get_tx_bytes_to_fifo(void)
{
    return s_tx_bytes_to_fifo;
}

uint32_t uart_owner_get_tx_self_start_failures(void)
{
    return s_tx_self_start_failures;
}

uint32_t uart_owner_get_tx_head(void)
{
    // Snapshot only -- the producer (uart_owner_send()) can be updating this
    // concurrently from link_task's perspective across two separate SWD
    // reads, same "not a guarantee, adequate for a coarse comparison"
    // caveat as uart_owner_get_tx_used()'s own doc comment.
    return s_tx_head;
}

uint32_t uart_owner_get_tx_tail(void)
{
    // Snapshot only, same caveat as uart_owner_get_tx_head() -- the ISR
    // drains this concurrently. Read alongside uart_owner_get_tx_head() a
    // few seconds apart: if tail is not advancing while head climbs, or the
    // two settle into a small, unchanging gap that repeats exactly every
    // DIAG/POWER cycle, that is the ring-pointer bug the coordinator is
    // checking for.
    return s_tx_tail;
}

uint32_t uart_owner_get_tx_last_primed_calls(void)
{
    return s_tx_priming_calls;
}
