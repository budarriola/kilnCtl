// uart_owner.c -- see uart_owner.h. Raw-byte RX/TX rings over UART1, IRQ
// driven both ways, never blocking a caller.
#include "uart_owner.h"

#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/sync.h"
#include "hardware/uart.h"

#include "board_pins.h"

#define UART_OWNER_INSTANCE   uart1
#define UART_OWNER_IRQ        UART1_IRQ
// Must stay equal to KilnFW's CONFIG_KILNCTL_SAFETY_BAUD_RATE -- both sides
// hardcode it, there is no negotiation. Lowered from 115200 on 2026-08-23:
// the opto-isolated pair (TCMT1109 with R15's 1k pull-up) simply cannot
// switch fast enough for a 8.7 us bit. At 115200 and 57600 not one frame
// ever arrived; 38400 lost about a fifth of them; 19200 looked clean over a
// short window but still lost about a tenth over a longer one; 9600 tracks
// one for one over minutes. See the measurement table in
// KilnFW/App/drivers/Kconfig under KILNCTL_SAFETY_BAUD_RATE.
//
// Worth knowing why this hid for so long: DC level tests pass in both
// directions at any baud rate, because the opto carries a static level
// perfectly well. Bench GPIO high/low checks on this pair had already
// "verified" the wiring.
#define UART_OWNER_BAUD_RATE  9600u

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
    while (uart_is_writable(UART_OWNER_INSTANCE) && s_tx_tail != s_tx_head) {
        uart_get_hw(UART_OWNER_INSTANCE)->dr = s_tx_ring[s_tx_tail];
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

    // Invert the TX pin so the isolator sits IDLE-OFF rather than idle-on.
    //
    // An ordinary UART idles at mark, i.e. high. Driving U3's LED from a pin
    // that idles high means the optocoupler conducts continuously whenever
    // nothing is being sent, which is the whole time: the LED burns current
    // around the clock, ages faster, and -- the part that actually bit us --
    // starts every transmission out of deep saturation, so the first edges
    // come out of a part that has to recover before it can switch cleanly.
    //
    // The RP2040's PL011 has no line-inversion control of its own, but the
    // GPIO block does: GPIO_OVERRIDE_INVERT on the pin's outover flips the
    // peripheral's output on the way to the pad. Idle mark therefore reaches
    // the pad as low, the LED is dark between frames, and the phototransistor
    // rests non-conducting with R15 holding the ESP's input high.
    //
    // This must be kept in step with the ESP: because this end now inverts,
    // safety_link.c applies UART_SIGNAL_TXD_INV only, NOT RXD_INV. Exactly
    // one inversion per direction. The ESP->Pico direction already worked out
    // this way -- its TXD_INV means its pin idles low too, so U2 was already
    // dark at idle; only this direction was wrong.
    gpio_set_outover(SAFTYFW_PIN_UART1_TX, GPIO_OVERRIDE_INVERT);
    // Plain hardware UART, no inversion, no PIO -- the ESP inverts on its
    // side; see this file's header comment.
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
    while (uart_is_writable(UART_OWNER_INSTANCE) && s_tx_tail != s_tx_head) {
        uart_get_hw(UART_OWNER_INSTANCE)->dr = s_tx_ring[s_tx_tail];
        s_tx_tail = (s_tx_tail + 1u) % UART_OWNER_TX_RING_SIZE;
        s_tx_bytes_to_fifo++;
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
