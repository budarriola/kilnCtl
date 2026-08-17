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
#define UART_OWNER_BAUD_RATE  115200u

// Sized well past one worst-case stuffed Frame A/FW_VERSION frame (header 8 +
// payload up to ~60 + crc 2 = ~70 raw bytes; stuffing can at most double that
// plus two delimiters -- KILNLINK_FRAME_STUFFED_MAX in kilnlink_frame.h gives
// the general bound). 128 bytes covers this link's actual traffic with
// margin, without pretending to be a general-purpose byte pipe.
#define UART_OWNER_TX_RING_SIZE  128u
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

    // Make sure the drain interrupt is armed -- cheap to call unconditionally
    // even if it was already enabled.
    uart_set_irq_enables(UART_OWNER_INSTANCE, true, true);

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
