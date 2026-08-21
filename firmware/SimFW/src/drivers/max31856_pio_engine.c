// max31856_pio_engine.c -- see max31856_pio_engine.h for the ownership
// model and docs/PLAN.md section 3.2.1 for the design this implements.
//
// BUILD-VERIFIED, NOT HARDWARE-TIMING-VERIFIED -- see
// max31856_spi_slave.pio's file header for the full disclaimer.
#include "max31856_pio_engine.h"

#include <string.h>

#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "max31856_spi_slave.pio.h"

// --- Bus-lookup table for the two IRQ dispatch functions below -------------
// At most one bus struct per PIO index (0 or 1) -- see the .h file's
// max31856_pio_engine_start_irq() comment. Populated by that function, read
// only from the two irq_handler_pioN functions and the shared GPIO callback,
// both of which only ever run after start_irq() has published the pointer,
// so no lock is needed (single publisher, read-only readers, same core).
static max31856_pio_bus_t *s_bus_for_pio_index[2];

// --- Byte-level channel handling, called from IRQ context ------------------

// Pushes one response byte to the bus's shared TX FIFO on `channel`'s
// behalf, left-justified per max31856_spi_slave.pio's OUT shift-direction
// contract (shift_right=false: OUT emits the current MSB of a 32-bit OSR
// each cycle, so the real byte must sit in bits[31:24] going in).
static void tx_push_byte(max31856_pio_bus_t *bus, uint8_t channel, uint8_t byte)
{
    pio_sm_put(bus->pio, bus->sm_tx, ((uint32_t)byte) << 24);
    bus->stats[channel].bytes_tx++;
    bus->last_tx_channel = channel;
}

// --- Shared TX state-machine stall bookkeeping ------------------------------
// The RP2040 PIO latches a sticky per-SM TXSTALL bit in FDEBUG whenever an
// OUT's autopull found the TX FIFO empty and had to wait. Two helpers so the
// two very different meanings of that bit never get confused:
//
//  * tx_clear_stall() -- "this stall was structural, not a defect". The TX SM
//    unavoidably stalls at the start of EVERY transaction: it enters its
//    byte_loop on the first SCLK edge after CS falls, but the response byte
//    cannot exist until the whole address byte has been received eight clocks
//    later. Counting that as an underrun would make first_byte_late fire once
//    per transaction forever and render the "a run with nonzero underruns is
//    invalid" rule useless.
//  * tx_note_stall() -- "this stall happened AFTER a response byte was
//    already staged", i.e. the ISR genuinely fell behind the master's clock
//    mid-burst. That is the real, countable underrun.
static uint32_t tx_stall_bit(const max31856_pio_bus_t *bus)
{
    return 1u << (PIO_FDEBUG_TXSTALL_LSB + bus->sm_tx);
}

static void tx_clear_stall(max31856_pio_bus_t *bus)
{
    bus->pio->fdebug = tx_stall_bit(bus); // write-1-to-clear
}

static void tx_note_stall(max31856_pio_bus_t *bus, uint8_t channel)
{
    uint32_t bit = tx_stall_bit(bus);
    if ((bus->pio->fdebug & bit) != 0u) {
        bus->pio->fdebug = bit;
        bus->stats[channel].first_byte_late++;
    }
}

// Returns the shared TX state machine to a known, byte-aligned, tri-stated
// idle state. MUST run at every CS deassert.
//
// Why: the ISR necessarily stages one more response byte than the master ever
// clocks out. For an N-register read the master clocks N+1 bytes (address +
// N), and the RX side sees N+1 bytes, so it stages N+1 responses -- but the
// TX SM can only emit one bit per SCLK cycle and the first byte-time is spent
// receiving the address, so at most N bytes ever leave on MISO. Without this
// reset the surplus byte (plus any bits left mid-OSR, plus anything a write
// transaction left stalled) survives into the NEXT CS assertion and leads it,
// shifting every subsequent transaction's MISO stream by a byte and
// accumulating until the 4-deep TX FIFO jams. pio_sm_restart() is what clears
// the OSR/ISR shift counters and the stalled state; clear_fifos() drops the
// surplus bytes; the jmp re-enters the program at its `set y, N` preamble so
// the idle-compare constant is reloaded.
static void tx_reset(max31856_pio_bus_t *bus)
{
    pio_sm_set_enabled(bus->pio, bus->sm_tx, false);
    pio_sm_clear_fifos(bus->pio, bus->sm_tx);
    pio_sm_restart(bus->pio, bus->sm_tx);
    pio_sm_exec(bus->pio, bus->sm_tx, pio_encode_jmp(bus->offset_tx));
    // Tri-state MISO explicitly rather than waiting the ~5 cycles the
    // program's own poll_idle pass would take to do it.
    pio_sm_set_pindirs_with_mask(bus->pio, bus->sm_tx, 0u, 1u << bus->miso_gpio);
    tx_clear_stall(bus);
    pio_sm_set_enabled(bus->pio, bus->sm_tx, true);
}

// Drains every byte currently sitting in `channel`'s RX FIFO and advances
// that channel's max31856_regs_t transaction state accordingly. Because
// bus A's three RX SMs are individually gated by their own CS line (only
// one is ever actively receiving at a time -- SPI protocol only clocks one
// selected chip), no cross-channel interleaving of the shared TX FIFO can
// happen here; see max31856_pio_engine.h's struct comment.
static void handle_channel_rx_bytes(max31856_pio_bus_t *bus, uint8_t channel)
{
    PIO pio = bus->pio;
    uint sm = bus->sm_rx[channel];
    max31856_channel_t *ch = bus->channels[channel];

    while (!pio_sm_is_rx_fifo_empty(pio, sm)) {
        // RX shift-direction contract (max31856_spi_slave.pio header): the
        // assembled byte lands in the low 8 bits of the 32-bit FIFO word,
        // MSB-first, because ISR is explicitly zeroed at the start of every
        // byte and autopush itself re-zeroes it after each push.
        uint32_t word = pio_sm_get(pio, sm);
        uint8_t byte = (uint8_t)(word & 0xFFu);
        bus->stats[channel].bytes_rx++;

        if (!bus->txn_open[channel]) {
            // First byte of a fresh transaction: the address byte. Opens
            // the transaction in the register model (which -- for a read --
            // takes its own internal coherency snapshot right here, per
            // max31856_regs.h's file-header guarantee) and, for a read,
            // immediately stages the first response byte into the TX FIFO --
            // this IS PLAN.md 3.2.1's Plan A: "the handler indexes the
            // channel's register image and feeds the TX FIFO with the
            // auto-increment stream" triggered by the address byte.
            max31856_regs_cs_assert(ch, byte);
            bus->txn_open[channel] = true;
            bus->txn_is_write[channel] = (byte & MAX31856_WRITE_BIT) != 0u;
            bus->stats[channel].transactions++;

            if (!bus->txn_is_write[channel]) {
                uint8_t resp = max31856_regs_clock_read_byte(ch);
                tx_push_byte(bus, channel, resp);
            }
            // Either way the SM has been stalling since CS fell, for the
            // structural reason tx_clear_stall() documents. Discard that
            // stall so only genuine mid-burst underruns get counted below.
            tx_clear_stall(bus);
            // Write transactions stage nothing -- MISO is don't-care while
            // the master is sending register data; the real chip drives
            // whatever it likes on MISO during a write and every master
            // driver in this repo already ignores it.
            continue;
        }

        // Subsequent byte within an open transaction.
        if (bus->txn_is_write[channel]) {
            max31856_regs_clock_write_byte(ch, byte);
        } else {
            // This RX byte is the master's dummy/don't-care MOSI byte sent
            // while clocking out our response byte -- discard its value,
            // but its ARRIVAL is the cue to stage the *next* response byte
            // one byte-time ahead of when the master will clock it out,
            // exactly mirroring the auto-increment addressing
            // max31856_regs_clock_read_byte() already implements.
            //
            // Checked BEFORE staging: a stall latched since the previous
            // byte means the SM ran dry with the master still clocking --
            // the real underrun.
            tx_note_stall(bus, channel);
            uint8_t resp = max31856_regs_clock_read_byte(ch);
            tx_push_byte(bus, channel, resp);
        }
    }
}

// Shared IRQ body for both buses -- called by the two tiny per-PIO wrapper
// functions below so each can be registered as its own exclusive handler
// (RP2040 IRQ vectors are argument-less C functions; there is no per-call
// user-data slot, hence the small static lookup table instead).
static void poll_bus_rx(max31856_pio_bus_t *bus)
{
    if (!bus) {
        return;
    }
    for (uint8_t i = 0; i < bus->channel_count; i++) {
        handle_channel_rx_bytes(bus, i);
    }
}

static void irq_handler_pio0(void)
{
    poll_bus_rx(s_bus_for_pio_index[0]);
}

static void irq_handler_pio1(void)
{
    poll_bus_rx(s_bus_for_pio_index[1]);
}

// Shared GPIO callback (pico-sdk has one callback slot per core, covering
// every GPIO IRQ source) -- handles CS *rising* edges (deassert, active-low)
// for whichever bus/channel owns that gpio. Both buses' CS pins are
// registered against this same function; it fans out by scanning the tiny
// bus table, which is cheap (<=4 CS pins total across both real instances).
static void gpio_cs_deassert_callback(uint gpio, uint32_t events)
{
    if ((events & GPIO_IRQ_EDGE_RISE) == 0u) {
        return;
    }
    for (int pio_idx = 0; pio_idx < 2; pio_idx++) {
        max31856_pio_bus_t *bus = s_bus_for_pio_index[pio_idx];
        if (!bus) {
            continue;
        }
        for (uint8_t ch = 0; ch < bus->channel_count; ch++) {
            if (bus->cs_gpio[ch] != gpio) {
                continue;
            }
            // Drain any bytes still sitting in the FIFO before formally
            // closing the transaction, so a burst that finished exactly at
            // CS-rise is not left stranded.
            handle_channel_rx_bytes(bus, ch);
            if (bus->txn_open[ch]) {
                if (!bus->txn_is_write[ch]) {
                    // A stall still latched at the end of a read means the
                    // last response byte(s) went out late.
                    tx_note_stall(bus, ch);
                }
                max31856_regs_cs_deassert(bus->channels[ch]);
                bus->txn_open[ch] = false;
                bus->txn_is_write[ch] = false;
            }
            // Unconditional: even a transaction that never opened (a CS
            // glitch with no complete address byte) must not leave the
            // shared TX SM mid-byte or holding staged data.
            tx_reset(bus);
            return;
        }
    }
}

// --- Init --------------------------------------------------------------

static void init_input_pin(PIO pio, uint gpio)
{
    pio_gpio_init(pio, gpio);
    gpio_set_dir(gpio, GPIO_IN);
    gpio_pull_up(gpio); // idle-high convention for CS; harmless no-op-ish for SCLK/MOSI, which the real master drives
}

bool max31856_pio_engine_init(max31856_pio_bus_t *bus, PIO pio,
                               uint sclk_gpio, uint mosi_gpio, uint miso_gpio,
                               const uint *cs_gpio, uint8_t channel_count,
                               max31856_channel_t *const *channels)
{
    if (!bus || !cs_gpio || !channels || channel_count == 0u ||
        channel_count > MAX31856_PIO_ENGINE_MAX_CHANNELS) {
        return false;
    }

    memset(bus, 0, sizeof(*bus));
    bus->pio = pio;
    bus->sclk_gpio = sclk_gpio;
    bus->mosi_gpio = mosi_gpio;
    bus->miso_gpio = miso_gpio;
    bus->channel_count = channel_count;
    for (uint8_t i = 0; i < channel_count; i++) {
        bus->cs_gpio[i] = cs_gpio[i];
        bus->channels[i] = channels[i];
    }

    // --- GPIO function/direction setup -------------------------------
    init_input_pin(pio, sclk_gpio);
    init_input_pin(pio, mosi_gpio);
    for (uint8_t i = 0; i < channel_count; i++) {
        init_input_pin(pio, cs_gpio[i]);
    }
    pio_gpio_init(pio, miso_gpio);
    pio_sm_set_consecutive_pindirs(pio, 0, miso_gpio, 1, false); // start tri-stated; sm index arg is irrelevant for a pindir-only call on an unclaimed pin

    // --- RX program: one shared load, one SM instance per channel ----
    bus->offset_rx = pio_add_program(pio, &max31856_spi_rx_program);
    for (uint8_t i = 0; i < channel_count; i++) {
        int sm = pio_claim_unused_sm(pio, true);
        if (sm < 0) {
            return false;
        }
        bus->sm_rx[i] = (uint)sm;

        pio_sm_config c = max31856_spi_rx_program_get_default_config(bus->offset_rx);
        sm_config_set_in_pins(&c, mosi_gpio);       // IN_BASE = MOSI, see .pio header
        sm_config_set_jmp_pin(&c, cs_gpio[i]);       // this channel's own CS, absolute gpio
        sm_config_set_in_shift(&c, false, true, 8);   // shift-left, autopush, 8-bit threshold (see .pio header's shift contract)
        sm_config_set_clkdiv(&c, 1.0f);                // full system clock -- tightest response to SCLK edges this design can offer without a divider tuned to a known master clock, which PLAN.md 3.2.1 does not fix at 5 MHz-only
        pio_sm_init(pio, bus->sm_rx[i], bus->offset_rx, &c);
        pio_sm_set_enabled(pio, bus->sm_rx[i], true);
    }

    // --- TX program: bus-width-specific variant, one shared SM -------
    const pio_program_t *tx_program = (channel_count == 1u)
        ? &max31856_spi_tx_b_program
        : &max31856_spi_tx_a_program;
    bus->offset_tx = pio_add_program(pio, tx_program);
    int tx_sm = pio_claim_unused_sm(pio, true);
    if (tx_sm < 0) {
        return false;
    }
    bus->sm_tx = (uint)tx_sm;

    pio_sm_config tc = (channel_count == 1u)
        ? max31856_spi_tx_b_program_get_default_config(bus->offset_tx)
        : max31856_spi_tx_a_program_get_default_config(bus->offset_tx);
    sm_config_set_in_pins(&tc, cs_gpio[0]);            // IN_BASE = first (of channel_count consecutive) CS gpio
    sm_config_set_out_pins(&tc, miso_gpio, 1);          // drives MISO's value
    sm_config_set_set_pins(&tc, miso_gpio, 1);           // drives MISO's pindir (tri-state control)
    sm_config_set_out_shift(&tc, false, true, 8);         // shift-left, autopull, 8-bit threshold (see .pio header's shift contract)
    // MANDATORY, not cosmetic. Both TX programs test CS with
    //     in pins, N ; mov x, isr ; jmp x!=y, active
    // against a SET-loaded constant (7 for bus A's three CS lines, 1 for bus
    // B's one). `in` places the sampled bits at whichever end of the ISR the
    // shift direction says, and pio_get_default_sm_config() -- which
    // *_program_get_default_config() starts from -- leaves IN shifting RIGHT.
    // Shifting right into a zeroed ISR puts the N bits at ISR[31:32-N], so an
    // all-idle bus A reads back 0xE0000000, never 7, and the compare takes
    // the "active" branch unconditionally: MISO would be driven permanently
    // instead of tri-stated, and neither the idle poll nor the per-bit
    // mid-byte CS re-check would ever fire. Shifting LEFT lands the bits in
    // ISR[N-1:0] so the constants mean what the programs say they mean.
    // autopush stays off (the ISR is scratch here, never a byte stream).
    sm_config_set_in_shift(&tc, false, false, 32);
    sm_config_set_clkdiv(&tc, 1.0f);
    pio_sm_init(pio, bus->sm_tx, bus->offset_tx, &tc);
    pio_sm_set_consecutive_pindirs(pio, bus->sm_tx, miso_gpio, 1, false); // tri-stated at boot, matching the idle state the TX program itself will re-derive on its first poll_idle pass
    pio_sm_set_enabled(pio, bus->sm_tx, true);

    return true;
}

void max31856_pio_engine_start_irq(max31856_pio_bus_t *bus)
{
    if (!bus) {
        return;
    }

    uint pio_idx = pio_get_index(bus->pio);
    s_bus_for_pio_index[pio_idx] = bus;

    // RX-FIFO-not-empty IRQ, one enable per RX SM, routed to this PIO
    // block's IRQ0 line.
    for (uint8_t i = 0; i < bus->channel_count; i++) {
        pio_set_irq0_source_enabled(bus->pio, (enum pio_interrupt_source)(pis_sm0_rx_fifo_not_empty + bus->sm_rx[i]), true);
    }

    irq_set_exclusive_handler(pio_idx == 0 ? PIO0_IRQ_0 : PIO1_IRQ_0,
                               pio_idx == 0 ? irq_handler_pio0 : irq_handler_pio1);
    irq_set_enabled(pio_idx == 0 ? PIO0_IRQ_0 : PIO1_IRQ_0, true);

    // CS-rising-edge (deassert) detection -- PLAN.md 3.2.1: "CS deassert is
    // detected by a second tiny SM or a GPIO IRQ" -- GPIO IRQ chosen here,
    // see max31856_pio_engine.h's struct comment on why a PIO SM was not
    // used for this (keeps the 4-SM-per-block budget for RX+TX instead).
    for (uint8_t i = 0; i < bus->channel_count; i++) {
        gpio_set_irq_enabled_with_callback(bus->cs_gpio[i], GPIO_IRQ_EDGE_RISE, true, gpio_cs_deassert_callback);
    }
}

bool max31856_pio_engine_channel_busy(const max31856_pio_bus_t *bus, uint8_t channel)
{
    if (!bus || channel >= bus->channel_count) {
        return false;
    }
    return bus->txn_open[channel];
}

max31856_pio_stats_t max31856_pio_engine_get_stats(const max31856_pio_bus_t *bus, uint8_t channel)
{
    max31856_pio_stats_t empty;
    memset(&empty, 0, sizeof(empty));
    if (!bus || channel >= bus->channel_count) {
        return empty;
    }
    return bus->stats[channel];
}
