// max31856_pio_engine.c -- see max31856_pio_engine.h for the ownership model,
// the Plan B chain diagram, and docs/SPI_ACCESS_AUDIT.md section 6 for why
// the CPU is no longer in the SPI response path at all.
//
// BUILD-VERIFIED, NOT HARDWARE-TIMING-VERIFIED -- see
// max31856_spi_slave.pio's file header for the full disclaimer.
#include "max31856_pio_engine.h"

#include <string.h>

#include "hardware/dma.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

#include "max31856_spi_slave.pio.h"
#include "simfw_fatal.h"

// --- Bus-lookup table for the IRQ dispatch functions below ------------------
// At most one bus struct per PIO index (0 or 1) -- see the .h file's
// max31856_pio_engine_start_irq() comment. Populated by that function, read
// only from the IRQ handlers, all of which only ever run after start_irq()
// has published the pointer, so no lock is needed (single publisher,
// read-only readers, same core).
static max31856_pio_bus_t *s_bus_for_pio_index[2];

// DMA_IRQ_0 has one vector for both buses; irq_set_exclusive_handler() panics
// if the same vector is claimed twice, so the second bus to start must not
// re-register.
static bool s_dma_irq_installed;

// ---------------------------------------------------------------------------
// PLAN B DATA FLOW -- the cycle budget, stated so it can be argued with.
//
// The deadline. In SPI mode 1 the master drives MOSI on SCLK's rising edge
// and this responder samples on the falling edge, so the address byte's value
// exists at falling edge 8. The responder shifts MISO on the rising edge, so
// the DESIGN deadline for the first response byte to be in the TX FIFO is
// rising edge 9: half an SCLK period, 125 ns at 4 MHz / 100 ns at 5 MHz. The
// HARD deadline -- past which the master latches the wrong thing rather than
// merely sampling a bit that settled late -- is the master's own sample point
// at falling edge 9 minus its MISO setup requirement: one full SCLK period,
// 250 ns at 4 MHz / 200 ns at 5 MHz, minus setup.
//
// The path, in RP2040 sysclk cycles (125 MHz default => 8 ns/cycle):
//   RX autopush -> RX-FIFO-not-empty DREQ                        ~1
//   sniff channel: DREQ recognised, arbitrated, read PIO RXF,
//     write bus->addr_capture (SRAM)                             ~7-10
//   chain to load channel (starts the cycle after sniff retires)  ~1-2
//   load channel: read addr_capture (SRAM), write the data
//     channel's al3_read_addr_trig (DMA register block)          ~4-6
//   data channel: triggered, reads image word (SRAM), writes
//     PIO TXF                                                    ~6-8
//                                                        total  ~19-27 cycles
//                                              at 125 MHz  =>  ~150-215 ns
//
// So, honestly: at 125 MHz this design does NOT reliably meet the 125 ns
// design deadline, but it does meet the 250 ns hard deadline at 4 MHz with
// margin, and meets the 200 ns hard deadline at 5 MHz with little. Raising
// sysclk to 200 MHz brings the path to ~95-135 ns and meets the design
// deadline at 4 MHz. Both of those statements rest on RP2040 datasheet
// section 2.5 semantics plus published single-transfer DMA latency figures,
// NOT on a measurement -- there is no fixture hardware. The Saleae capture
// PLAN.md section 10 M-A requires is what settles it, and section 8 of
// SPI_ACCESS_AUDIT.md now has a fourth thing to confirm: that response byte 0
// is aligned to SCLK 9-16 and not 17-24.
//
// The chain deliberately spends ~5 of those cycles on the load hop (sniff
// could write al3_read_addr_trig directly, saving it) so that addr_capture
// exists in SRAM for the CPU to read. The alternative -- reconstructing the
// address from the data channel's read_addr and transfer_count at CS rise --
// is exact but depends on abort semantics this code would then have to be
// right about with no way to test them. Buying ~40 ns of margin with a
// dependency on an untestable erratum-adjacent behaviour is a bad trade for a
// fixture whose entire value is being trustworthy.
// ---------------------------------------------------------------------------

// --- Shared TX state machine ------------------------------------------------

// Returns the shared TX state machine to a known, byte-aligned, tri-stated
// idle state. MUST run at every CS deassert, and MUST run AFTER the data DMA
// channel has been stopped -- otherwise the DMA simply refills the FIFO this
// just cleared.
//
// Why it is still needed under Plan B (SPI_ACCESS_AUDIT.md D2): the data
// channel is paced by TX-FIFO-not-full, so it always runs ahead of the master
// and leaves up to five surplus response words (four in the FIFO, one in the
// OSR) at the end of every transaction. Without this reset that surplus would
// lead the NEXT CS assertion and shift every subsequent transaction's MISO
// stream by a byte, exactly as it did before, only more of it.
// pio_sm_restart() is what clears the OSR shift counter and the stalled
// state; clear_fifos() drops the surplus; the jmp re-enters the program at
// its first instruction -- on bus A that is the `set y, N` preamble, so the
// idle-compare constant is reloaded; on bus B (max31856_spi_tx_b, rewritten
// with `jmp pin` and no compare constant at all) the first instruction IS
// poll_idle, so this jmp lands exactly there with nothing to reload.
static void tx_reset(max31856_pio_bus_t *bus)
{
    pio_sm_set_enabled(bus->pio, bus->sm_tx, false);
    pio_sm_clear_fifos(bus->pio, bus->sm_tx);
    pio_sm_restart(bus->pio, bus->sm_tx);
    pio_sm_exec(bus->pio, bus->sm_tx, pio_encode_jmp(bus->offset_tx));
    // Tri-state MISO explicitly rather than waiting the ~5 cycles the
    // program's own poll_idle pass would take to do it. Three emulated chips
    // share one physical MISO on bus A, so a moment of contention here is a
    // real electrical event, not a cosmetic one (SPI_ACCESS_AUDIT.md D1).
    pio_sm_set_pindirs_with_mask(bus->pio, bus->sm_tx, 0u, 1u << bus->miso_gpio);
    // Discard the sticky TXSTALL bit. Purely hygiene: nothing reads it any
    // more, because under Plan B the TX SM stalls structurally at the start of
    // every single transaction and the bit therefore carries no information.
    // The underrun counter is derived from the data channel's delivered-word
    // count instead -- see max31856_pio_engine.h's first_byte_late comment.
    bus->pio->fdebug = 1u << (PIO_FDEBUG_TXSTALL_LSB + bus->sm_tx);
}

// --- Response-image publication --------------------------------------------

// Hands `base` to `channel`'s RX state machine, which picks it up at its
// `idle` loop (i.e. only while CS is high) via `pull noblock`. One 32-bit
// store, so the handover is atomic with respect to the state machine: it
// either sees the whole new base or the whole old one, never a mixture. That
// is what makes the double-banked image tear-proof -- a burst in flight keeps
// reading the bank it started on, and that bank is never the one being
// rewritten.
static bool publish_base(max31856_pio_bus_t *bus, uint8_t channel, const max31856_resp_image_t *img)
{
    uintptr_t base = (uintptr_t)img;
    // The PIO builds the DMA read pointer by shifting the published value left
    // by 10 and OR-ing the address byte * 4 into the bottom. That is only an
    // OR if the low 10 bits of the base are zero. Checked, not asserted in a
    // comment: an image declared without MAX31856_RESP_IMAGE_ALIGN would
    // otherwise produce a silently wrong pointer and plausible wrong data --
    // the exact failure class this subsystem has already shipped three times.
    if ((base & (MAX31856_RESP_IMAGE_BYTES - 1u)) != 0u) {
        return false;
    }
    pio_sm_put(bus->pio, bus->sm_rx[channel], (uint32_t)(base >> 10));
    return true;
}

// --- ~DRDY ------------------------------------------------------------------

// Open-drain emulation (DESIGN_NOTES.md 3.6): asserted drives the line low, released
// returns it to Hi-Z and lets the DUT's pull-up take it high. Never drives
// high, so a DUT that also drives the net cannot be fought.
static void drdy_sync(max31856_pio_bus_t *bus, uint8_t channel)
{
    int gpio = bus->drdy_gpio[channel];
    if (gpio < 0) {
        return;
    }
    if (max31856_regs_drdy_asserted(bus->channels[channel])) {
        gpio_set_dir((uint)gpio, GPIO_OUT); // output latch is held at 0 from init
    } else {
        gpio_set_dir((uint)gpio, GPIO_IN);
    }
}

// --- Data channel -----------------------------------------------------------

static void data_stop(max31856_pio_bus_t *bus)
{
    uint ch = (uint)bus->dma_data;
    // Clear EN before aborting: the pico-sdk's own guidance for stopping a
    // DREQ-paced channel, and it keeps the abort from racing a transfer that
    // the peripheral is still requesting.
    dma_hw->ch[ch].al1_ctrl = 0u;
    dma_hw->abort = 1u << ch;
    while ((dma_hw->abort & (1u << ch)) != 0u) {
        tight_loop_contents();
    }
}

// Re-arms the data channel for the next transaction: control word restored,
// transfer count reloaded, NOT started (the load channel's write to
// al3_read_addr_trig is what starts it, and that write also supplies the read
// address, so there is nothing left for the CPU to get wrong at start time).
static void data_arm(max31856_pio_bus_t *bus)
{
    dma_channel_config c = dma_channel_get_default_config((uint)bus->dma_data);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, true);
    channel_config_set_write_increment(&c, false);
    // 512-byte read ring == 128 entries == one half of the response image.
    // This is what reproduces the part's own 7-bit auto-increment wrap
    // ("the address will loop from 7Fh/FFh to 00h/80h", datasheet page 15)
    // with no code at all, and what keeps a write transaction inside the
    // image's mirror half instead of falling through into the read half.
    channel_config_set_ring(&c, false /* wrap the READ address */, 9);
    channel_config_set_dreq(&c, pio_get_dreq(bus->pio, bus->sm_tx, true));
    dma_channel_set_config((uint)bus->dma_data, &c, false);
    dma_channel_set_write_addr((uint)bus->dma_data, &bus->pio->txf[bus->sm_tx], false);
    dma_channel_set_trans_count((uint)bus->dma_data, MAX31856_PIO_ENGINE_DATA_WORDS, false);
}

// --- Sniff channel ----------------------------------------------------------

// Arms `channel`'s sniff channel to capture exactly one word -- the next word
// this RX state machine pushes, which by construction is the address byte of
// the next transaction. Chains to the load channel on completion.
static void sniff_arm(max31856_pio_bus_t *bus, uint8_t channel)
{
    uint ch = (uint)bus->dma_sniff[channel];
    dma_channel_config c = dma_channel_get_default_config(ch);
    channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
    channel_config_set_read_increment(&c, false);
    channel_config_set_write_increment(&c, false);
    channel_config_set_dreq(&c, pio_get_dreq(bus->pio, bus->sm_rx[channel], false));
    channel_config_set_chain_to(&c, (uint)bus->dma_load);
    dma_channel_configure(ch, &c,
                          (void *)&bus->addr_capture,
                          (const void *)&bus->pio->rxf[bus->sm_rx[channel]],
                          1u,
                          true /* start: it will sit on the DREQ until a word arrives */);
}

// --- Byte-level channel handling, called from IRQ context ------------------

// Recovers the data byte from one RX FIFO word. Every word the RX program
// pushes is `image_base | (byte << 2)` -- see max31856_spi_slave.pio's shift
// contract -- because the program preloads the image base on every byte, not
// just the address byte, so that it has no framing state to get wrong.
static uint8_t rx_word_to_byte(uint32_t word)
{
    return (uint8_t)((word >> 2) & 0xFFu);
}

static void handle_data_byte(max31856_pio_bus_t *bus, uint8_t channel, uint8_t byte)
{
    max31856_channel_t *ch = bus->channels[channel];

    bus->stats[channel].bytes_rx++;
    if (bus->txn_data_bytes[channel] < 0xFFFFu) {
        bus->txn_data_bytes[channel]++;
    }

    if (!bus->txn_open[channel]) {
        // A data byte with no open transaction: the load channel's IRQ has not
        // run yet (it cannot be late by more than a fraction of a byte-time,
        // so this is a genuine anomaly), or CS glitched. Nothing sane to apply
        // it to.
        bus->stats[channel].protocol_errors++;
        return;
    }

    if (bus->txn_is_write[channel]) {
        max31856_regs_clock_write_byte(ch, byte);
        return;
    }

    // Read transaction. The DMA already put the real response byte on the
    // wire; this call exists so the reference register model's auto-increment
    // address advances in step with the hardware's, which is what makes the
    // ~DRDY release rule ("did this read touch LTCB or CJT?") see the same
    // register span the master actually clocked out. The returned byte is
    // discarded on purpose -- it is the model's answer, not the wire's, and
    // under Plan B those differ by at most bit-error re-rolling (see
    // max31856_resp_image.h's FIDELITY NOTE).
    (void)max31856_regs_clock_read_byte(ch);
}

// Drains exactly the words present in `channel`'s RX FIFO at entry. Bounded
// on purpose: reading "until empty" could, in a back-to-back-transaction
// pathology, swallow the NEXT transaction's address word and mis-attribute it
// as this transaction's data.
static void drain_channel_rx(max31856_pio_bus_t *bus, uint8_t channel)
{
    PIO pio = bus->pio;
    uint sm = bus->sm_rx[channel];
    uint level = pio_sm_get_rx_fifo_level(pio, sm);
    for (uint i = 0; i < level; i++) {
        handle_data_byte(bus, channel, rx_word_to_byte(pio_sm_get(pio, sm)));
    }
}

// --- IRQ handlers -----------------------------------------------------------

static void poll_bus_rx(max31856_pio_bus_t *bus)
{
    if (!bus) {
        return;
    }
    for (uint8_t i = 0; i < bus->channel_count; i++) {
        // Only a channel whose CS is actually low can legitimately have bytes
        // for the CPU. Skipping the others is not just an optimisation: an
        // idle channel's RX FIFO belongs to its ARMED SNIFF CHANNEL, which is
        // waiting there for the first word of that channel's next
        // transaction. Draining it here would steal the address word and
        // leave that transaction with no response at all.
        if (bus->cs_low[i]) {
            drain_channel_rx(bus, i);
        }
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

// Which channel on this bus currently has CS low? Exactly one can, which is
// what SPI means; returns -1 if none does (a transaction that ended before
// the load channel's IRQ could be serviced).
static int active_channel(const max31856_pio_bus_t *bus)
{
    for (uint8_t i = 0; i < bus->channel_count; i++) {
        if (gpio_get(bus->cs_gpio[i]) == 0) {
            return (int)i;
        }
    }
    return -1;
}

// Load-channel completion: the response stream is ALREADY running (the same
// write that raised this IRQ set the data channel's read address and started
// it). Everything here is bookkeeping the CPU has a full byte-time or more to
// do.
static void handle_load_done(max31856_pio_bus_t *bus)
{
    uint32_t captured = bus->addr_capture;
    int chan = active_channel(bus);
    if (chan < 0) {
        return;
    }
    uint8_t channel = (uint8_t)chan;

    // The captured word is the DMA read pointer the PIO assembled, i.e.
    // image_base | (addr_byte << 2); recover the raw first byte from it.
    uint8_t addr_byte = (uint8_t)((captured >> 2) & 0xFFu);

    max31856_regs_cs_assert(bus->channels[channel], addr_byte);
    // Belt and braces: a complete address byte having arrived is itself proof
    // that CS is low, so set the flag here too rather than relying solely on
    // the falling-edge IRQ. If that edge were ever missed (a transaction
    // already in flight when this bus came up, say), poll_bus_rx() would
    // otherwise refuse to drain this channel and the RX FIFO would fill until
    // the state machine stalled on autopush.
    bus->cs_low[channel] = true;
    bus->txn_open[channel] = true;
    bus->txn_is_write[channel] = (addr_byte & MAX31856_WRITE_BIT) != 0u;
    bus->txn_data_bytes[channel] = 0u;
    bus->stats[channel].transactions++;

    // Only now unmask the RX-FIFO IRQ. Leaving it permanently on would let
    // this CPU race the sniff channel for the address word, and the loser
    // would be silent: whoever lost would see a byte-shifted stream. Gating
    // it here makes the race impossible rather than improbable.
    pio_set_irq0_source_enabled(bus->pio,
                                 (enum pio_interrupt_source)(pis_sm0_rx_fifo_not_empty + bus->sm_rx[channel]),
                                 true);
}

static void irq_handler_dma(void)
{
    for (int i = 0; i < 2; i++) {
        max31856_pio_bus_t *bus = s_bus_for_pio_index[i];
        if (!bus || bus->dma_load < 0) {
            continue;
        }
        if (dma_channel_get_irq0_status((uint)bus->dma_load)) {
            dma_channel_acknowledge_irq0((uint)bus->dma_load);
            handle_load_done(bus);
        }
    }
}

// Shared GPIO callback (pico-sdk has one callback slot per core, covering
// every GPIO IRQ source) -- handles CS falling AND rising edges for whichever
// bus/channel owns that gpio.
static void gpio_cs_callback(uint gpio, uint32_t events)
{
    for (int pio_idx = 0; pio_idx < 2; pio_idx++) {
        max31856_pio_bus_t *bus = s_bus_for_pio_index[pio_idx];
        if (!bus) {
            continue;
        }
        for (uint8_t ch = 0; ch < bus->channel_count; ch++) {
            if (bus->cs_gpio[ch] != gpio) {
                continue;
            }

            if ((events & GPIO_IRQ_EDGE_FALL) != 0u) {
                // Close the owner task's register-commit window at the
                // PHYSICAL start of the transaction, ~2 us before the address
                // byte is even complete. The address is unknown here and
                // nothing else can be done yet -- deliberately.
                bus->cs_low[ch] = true;
            }

            if ((events & GPIO_IRQ_EDGE_RISE) != 0u) {
                // 1. Stop the CPU byte stream and collect what is left. Order
                //    matters: masking first means the bounded drain below sees
                //    a FIFO nobody else is racing it for.
                pio_set_irq0_source_enabled(bus->pio,
                                             (enum pio_interrupt_source)(pis_sm0_rx_fifo_not_empty + bus->sm_rx[ch]),
                                             false);
                drain_channel_rx(bus, ch);

                // 2. Measure the response stream BEFORE tearing it down. The
                //    data channel counts down from MAX31856_PIO_ENGINE_DATA_WORDS,
                //    so the difference is exactly how many response words it
                //    handed the TX FIFO. Fewer than the master clocked out is
                //    an unambiguous starvation -- and, unlike the sticky
                //    FDEBUG.TXSTALL bit this used to read, it cannot be
                //    confused with the structural start-of-transaction stall
                //    (SPI_ACCESS_AUDIT.md D3).
                uint32_t remaining = dma_hw->ch[(uint)bus->dma_data].transfer_count;
                uint32_t delivered = (remaining <= MAX31856_PIO_ENGINE_DATA_WORDS)
                                          ? (MAX31856_PIO_ENGINE_DATA_WORDS - remaining)
                                          : 0u;
                bus->stats[ch].bytes_tx += delivered;
                if (bus->txn_open[ch] && delivered < bus->txn_data_bytes[ch]) {
                    bus->stats[ch].first_byte_late++;
                }

                // 3. Tear the response path down, data channel first so it
                //    cannot refill the FIFO tx_reset() is about to clear.
                data_stop(bus);
                tx_reset(bus);
                data_arm(bus);

                // 4. Close the transaction in the register model. This is
                //    where ~DRDY is released if the read touched LTCB or (with
                //    the CJ sensor enabled) CJTH/CJTL -- see
                //    max31856_regs.h's max31856_regs_drdy_asserted() comment.
                if (bus->txn_open[ch]) {
                    max31856_regs_cs_deassert(bus->channels[ch]);
                    bus->txn_open[ch] = false;
                    bus->txn_is_write[ch] = false;
                }
                bus->txn_data_bytes[ch] = 0u;
                bus->cs_low[ch] = false;
                drdy_sync(bus, ch);

                // 5. Return the RX state machine to a known byte boundary and
                //    re-supply the image base -- but ONLY if CS really is
                //    still high. If the next transaction has already begun,
                //    restarting the SM would corrupt a byte in flight, and the
                //    SM is already correctly framed anyway (its byte loop is
                //    self-framing: the image base is preloaded on every byte).
                if (gpio_get(gpio) != 0) {
                    pio_sm_set_enabled(bus->pio, bus->sm_rx[ch], false);
                    pio_sm_clear_fifos(bus->pio, bus->sm_rx[ch]);
                    pio_sm_restart(bus->pio, bus->sm_rx[ch]);
                    pio_sm_exec(bus->pio, bus->sm_rx[ch], pio_encode_jmp(bus->offset_rx));
                    (void)publish_base(bus, ch, bus->images[ch][bus->live_bank[ch]]);
                    pio_sm_set_enabled(bus->pio, bus->sm_rx[ch], true);
                }

                // 6. Re-arm the address sniffer. If a word is already waiting
                //    when we get here, the next transaction's address byte
                //    landed while this handler was still running, so its first
                //    response byte is necessarily late -- that is a real,
                //    countable underrun and the only kind of first-byte
                //    lateness the fixture can observe in-band.
                bool already_waiting = !pio_sm_is_rx_fifo_empty(bus->pio, bus->sm_rx[ch]);
                sniff_arm(bus, ch);
                if (already_waiting) {
                    bus->stats[ch].first_byte_late++;
                }
            }
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

static bool config_is_sane(const max31856_pio_engine_config_t *cfg)
{
    if (!cfg || cfg->channel_count == 0u ||
        cfg->channel_count > MAX31856_PIO_ENGINE_MAX_CHANNELS) {
        return false;
    }
    // Pin arithmetic the PIO programs' fixed offsets depend on. Previously
    // only stated in a comment; SPI_ACCESS_AUDIT.md's three defects were all
    // "the comment asserted a contract the C did not deliver", so this one is
    // now checked.
    if (((cfg->sclk_gpio + 1u) & 31u) != (cfg->mosi_gpio & 31u)) {
        return false; // SCLK must be MOSI-1 (mod 32): the RX `wait ... pin 31`
    }
    if (((cfg->sclk_gpio + 3u) & 31u) != (cfg->cs_gpio[0] & 31u)) {
        return false; // CS0 must be SCLK+3 (mod 32): the TX `wait ... pin 29`
    }
    for (uint8_t i = 0; i < cfg->channel_count; i++) {
        if (cfg->cs_gpio[i] != cfg->cs_gpio[0] + i) {
            return false; // the TX program's `in pins, N` reads them as one consecutive group
        }
        if (!cfg->channels[i]) {
            return false;
        }
        for (uint8_t b = 0; b < MAX31856_PIO_ENGINE_BANKS; b++) {
            uintptr_t base = (uintptr_t)cfg->images[i][b];
            if (base == 0u || (base & (MAX31856_RESP_IMAGE_BYTES - 1u)) != 0u) {
                return false; // missing, or not MAX31856_RESP_IMAGE_ALIGN
            }
        }
    }
    return true;
}

bool max31856_pio_engine_init(max31856_pio_bus_t *bus,
                               const max31856_pio_engine_config_t *cfg)
{
    if (!bus || !config_is_sane(cfg)) {
        return false;
    }

    PIO pio = cfg->pio;

    memset(bus, 0, sizeof(*bus));
    bus->pio = pio;
    bus->sclk_gpio = cfg->sclk_gpio;
    bus->mosi_gpio = cfg->mosi_gpio;
    bus->miso_gpio = cfg->miso_gpio;
    bus->channel_count = cfg->channel_count;
    bus->dma_load = -1;
    bus->dma_data = -1;
    for (uint8_t i = 0; i < cfg->channel_count; i++) {
        bus->cs_gpio[i] = cfg->cs_gpio[i];
        bus->drdy_gpio[i] = cfg->drdy_gpio[i];
        bus->channels[i] = cfg->channels[i];
        bus->images[i][0] = cfg->images[i][0];
        bus->images[i][1] = cfg->images[i][1];
        bus->live_bank[i] = 0u;
        bus->dma_sniff[i] = -1;
    }

    // Published here rather than only in start_irq() so there is no window in
    // which this bus's DMA channels are armed but the shared DMA_IRQ_0
    // handler cannot find the bus they belong to -- the OTHER bus may already
    // have enabled that vector. Safe from this point on and no earlier: the
    // memset above is what puts the -1 channel ids in place that make the
    // handler no-op until there is something real to service.
    s_bus_for_pio_index[pio_get_index(pio)] = bus;

    // --- GPIO function/direction setup -------------------------------
    init_input_pin(pio, cfg->sclk_gpio);
    init_input_pin(pio, cfg->mosi_gpio);
    for (uint8_t i = 0; i < cfg->channel_count; i++) {
        init_input_pin(pio, cfg->cs_gpio[i]);
        if (cfg->drdy_gpio[i] >= 0) {
            uint g = (uint)cfg->drdy_gpio[i];
            gpio_init(g);
            gpio_put(g, 0);            // output latch parked low forever...
            gpio_set_dir(g, GPIO_IN);   // ...and the DIRECTION is the open-drain control
        }
    }
    pio_gpio_init(pio, cfg->miso_gpio);
    pio_sm_set_consecutive_pindirs(pio, 0, cfg->miso_gpio, 1, false); // start tri-stated; sm index arg is irrelevant for a pindir-only call on an unclaimed pin

    // --- RX program: one shared load, one SM instance per channel ----
    bus->offset_rx = pio_add_program(pio, &max31856_spi_rx_program);
    for (uint8_t i = 0; i < cfg->channel_count; i++) {
        int sm = pio_claim_unused_sm(pio, true);
        if (sm < 0) {
            return false;
        }
        bus->sm_rx[i] = (uint)sm;

        pio_sm_config c = max31856_spi_rx_program_get_default_config(bus->offset_rx);
        sm_config_set_in_pins(&c, cfg->mosi_gpio);   // IN_BASE = MOSI, see .pio header
        sm_config_set_jmp_pin(&c, cfg->cs_gpio[i]);   // this channel's own CS, absolute gpio
        // TEN, not eight. The RX program assembles a word-aligned pointer into
        // the response image (8 sampled bits + 2 zero bits of word scaling),
        // and autopush must fire on the 10th shifted bit, not the 8th. Get
        // this wrong and every pushed word is the address byte un-scaled --
        // the DMA would then read from image_base + addr instead of
        // image_base + 4*addr and return a byte from the wrong register with
        // no error anywhere. See max31856_spi_slave.pio's shift contract.
        sm_config_set_in_shift(&c, false /* shift left */, true /* autopush */, 10);
        sm_config_set_clkdiv(&c, 1.0f);               // full system clock -- every ns of the ~125 ns first-byte budget matters
        pio_sm_init(pio, bus->sm_rx[i], bus->offset_rx, &c);
        // Seed the image base BEFORE enabling, so the program's very first
        // `pull noblock` finds a real value rather than falling back to X=0.
        //
        // publish_base() can only fail on MAX31856_RESP_IMAGE_ALIGN
        // misalignment, and config_is_sane() above (this function's very
        // first check) already walked every cfg->images[i][b] -- including
        // this exact pointer, cfg->images[i][0], unchanged by the plain
        // assignment `bus->images[i][0] = cfg->images[i][0]` a few lines up
        // -- against the identical alignment test. So this branch is
        // PROVEN UNREACHABLE for any cfg that reached this point: if it ever
        // fires, config_is_sane()'s guarantee has been violated after the
        // fact (e.g. memory corruption of `bus` or `cfg` between the two
        // checks), not a normal runtime condition an unwind path could
        // meaningfully recover from -- there is no well-defined state to
        // unwind BACK TO when the invariant the caller relied on is already
        // false. Routed through simfw_fatal() rather than `return false`,
        // matching the DMA-exhaustion precedent this file already
        // established (docs/HARDWARE.md section 1b.5) for exactly this
        // class of "should be impossible; treat firing as a programming
        // error, not a degrade": a caller silently discarding this `false`
        // (as every dma_claim_unused_channel() site here used to) would
        // leave i-1 RX state machines already claimed AND enabled with no
        // way for anything else to know, on top of the corruption that
        // caused it.
        if (!publish_base(bus, i, bus->images[i][0])) {
            simfw_fatal("max31856_pio_engine",
                        "publish_base failed for channel %u on pio%u "
                        "(image base misaligned despite config_is_sane() "
                        "already validating it -- this should be "
                        "unreachable and indicates memory corruption, not "
                        "a normal runtime failure)",
                        (unsigned)i, (unsigned)pio_get_index(pio));
        }
        pio_sm_set_enabled(pio, bus->sm_rx[i], true);
    }

    // --- TX program: bus-width-specific variant, one shared SM -------
    const pio_program_t *tx_program = (cfg->channel_count == 1u)
        ? &max31856_spi_tx_b_program
        : &max31856_spi_tx_a_program;
    bus->offset_tx = pio_add_program(pio, tx_program);
    int tx_sm = pio_claim_unused_sm(pio, true);
    if (tx_sm < 0) {
        return false;
    }
    bus->sm_tx = (uint)tx_sm;

    pio_sm_config tc = (cfg->channel_count == 1u)
        ? max31856_spi_tx_b_program_get_default_config(bus->offset_tx)
        : max31856_spi_tx_a_program_get_default_config(bus->offset_tx);
    sm_config_set_in_pins(&tc, cfg->cs_gpio[0]);        // IN_BASE = first (of channel_count consecutive) CS gpio
    // JMP_PIN is a SEPARATE config field from IN_BASE, taking an absolute
    // gpio rather than an IN_BASE-relative offset (RP2040 datasheet section
    // 3.5.4). Only tx_b's `jmp pin` instructions consume it -- tx_a has none,
    // so this is a harmless no-op there -- but tx_b has NO other pin
    // configured to test CS with, since its old `in pins, 1` idiom is gone
    // (max31856_spi_slave.pio's tx_b header comment). Get this wrong and
    // tx_b silently branches on the wrong gpio: exactly SPI_ACCESS_AUDIT.md
    // D1's failure shape (plausible wrong MISO / stuck-driven bus), just via
    // a missing config call instead of a wrong shift direction. cs_gpio[0]
    // is correct for both variants: bus B's only CS line, and bus A's CS0
    // (tx_a doesn't use this field, so which of the 3 CS lines is irrelevant
    // for it).
    sm_config_set_jmp_pin(&tc, cfg->cs_gpio[0]);
    sm_config_set_out_pins(&tc, cfg->miso_gpio, 1);      // drives MISO's value
    sm_config_set_set_pins(&tc, cfg->miso_gpio, 1);       // drives MISO's pindir (tri-state control)
    sm_config_set_out_shift(&tc, false, true, 8);          // shift-left, autopull, 8-bit threshold: OUT emits OSR bit 31, so each image word carries its byte in bits[31:24]
    // MANDATORY, not cosmetic (SPI_ACCESS_AUDIT.md D1). Both TX programs test
    // CS with
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
    pio_sm_set_consecutive_pindirs(pio, bus->sm_tx, cfg->miso_gpio, 1, false); // tri-stated at boot, matching the idle state the TX program itself re-derives on its first poll_idle pass
    pio_sm_set_enabled(pio, bus->sm_tx, true);

    // --- DMA: data <- load <- sniff ----------------------------------
    // required = false, checked individually (rather than required = true,
    // or the combined check this replaces) so simfw_fatal() can name which
    // of the two failed and on which PIO bus -- see docs/HARDWARE.md section
    // 1b.5. Before this change, either failure returned false here with
    // dma_data possibly already claimed and leaked, and the caller (the
    // owner task) fell into a 1 s idle loop forever with no report anywhere.
    bus->dma_data = dma_claim_unused_channel(false);
    if (bus->dma_data < 0) {
        simfw_fatal("max31856_pio_engine",
                    "dma_data channel exhausted on pio%u (docs/HARDWARE.md "
                    "section 1b: 11/12 channels already budgeted, 1 spare)",
                    (unsigned)pio_get_index(pio));
    }
    bus->dma_load = dma_claim_unused_channel(false);
    if (bus->dma_load < 0) {
        simfw_fatal("max31856_pio_engine",
                    "dma_load channel exhausted on pio%u (dma_data already "
                    "claimed; docs/HARDWARE.md section 1b: 11/12 channels "
                    "already budgeted, 1 spare)",
                    (unsigned)pio_get_index(pio));
    }
    data_arm(bus);

    {
        // The load channel has no DREQ (the default TREQ is the permanent
        // request), so it runs the instant the sniff channel chains to it.
        // Its single write both delivers the read address and starts the data
        // channel -- that is what al3_read_addr_trig means.
        dma_channel_config c = dma_channel_get_default_config((uint)bus->dma_load);
        channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
        channel_config_set_read_increment(&c, false);
        channel_config_set_write_increment(&c, false);
        dma_channel_configure((uint)bus->dma_load, &c,
                              (void *)&dma_hw->ch[(uint)bus->dma_data].al3_read_addr_trig,
                              (const void *)&bus->addr_capture,
                              1u,
                              false /* started by the sniff channel's chain */);
        dma_channel_set_irq0_enabled((uint)bus->dma_load, true);
    }

    for (uint8_t i = 0; i < cfg->channel_count; i++) {
        bus->dma_sniff[i] = dma_claim_unused_channel(false);
        if (bus->dma_sniff[i] < 0) {
            // The sharper hazard docs/HARDWARE.md section 1b.5 item 3 flags:
            // s_bus_for_pio_index[] is already published and dma_load is
            // already armed with DMA_IRQ_0 enabled at this point, and the
            // shared irq_handler_dma() only skips a bus whose dma_load < 0
            // -- so a `return false` here used to leave a half-initialised
            // bus that the OTHER bus's IRQ install could still service.
            // simfw_fatal() halts synchronously and never returns, so that
            // half-initialised state is never reachable by anything else.
            simfw_fatal("max31856_pio_engine",
                        "dma_sniff[%u] channel exhausted on pio%u, "
                        "channel_count=%u (docs/HARDWARE.md section 1b: "
                        "11/12 channels already budgeted, 1 spare)",
                        (unsigned)i, (unsigned)pio_get_index(pio),
                        (unsigned)cfg->channel_count);
        }
        sniff_arm(bus, i);
    }

    return true;
}

void max31856_pio_engine_start_irq(max31856_pio_bus_t *bus)
{
    if (!bus) {
        return;
    }

    uint pio_idx = pio_get_index(bus->pio);
    s_bus_for_pio_index[pio_idx] = bus;

    // The RX-FIFO-not-empty sources stay MASKED here and are unmasked, per
    // transaction, only once the sniff channel has taken the address word --
    // see handle_load_done(). Registering the vector is all that happens now.
    irq_set_exclusive_handler(pio_idx == 0 ? PIO0_IRQ_0 : PIO1_IRQ_0,
                               pio_idx == 0 ? irq_handler_pio0 : irq_handler_pio1);
    irq_set_enabled(pio_idx == 0 ? PIO0_IRQ_0 : PIO1_IRQ_0, true);

    // DMA_IRQ_0 is shared by both buses. (Historically ct_wave_pwm.c owned
    // DMA_IRQ_1 so the two never contended for a vector; that driver is
    // deleted -- docs/DESIGN_NOTES.md §3.3 -- and its replacement,
    // ct_wave_i2s.c, installs no DMA IRQ handler at all, so DMA_IRQ_1 is
    // simply unclaimed today, docs/HARDWARE.md §1b.6.)
    if (!s_dma_irq_installed) {
        irq_set_exclusive_handler(DMA_IRQ_0, irq_handler_dma);
        irq_set_enabled(DMA_IRQ_0, true);
        s_dma_irq_installed = true;
    }

    // Both CS edges: falling closes the owner task's commit window at the
    // physical start of the transaction, rising does all the teardown.
    for (uint8_t i = 0; i < bus->channel_count; i++) {
        gpio_set_irq_enabled_with_callback(bus->cs_gpio[i],
                                            GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL,
                                            true, gpio_cs_callback);
    }
}

bool max31856_pio_engine_refresh_image(max31856_pio_bus_t *bus, uint8_t channel)
{
    if (!bus || channel >= bus->channel_count) {
        return false;
    }
    if (max31856_pio_engine_channel_busy(bus, channel)) {
        return false;
    }

    uint8_t next = (uint8_t)(bus->live_bank[channel] ^ 1u);
    max31856_resp_image_t *img = bus->images[channel][next];
    max31856_resp_image_publish(img, bus->channels[channel]);
    if (!publish_base(bus, channel, img)) {
        return false;
    }
    bus->live_bank[channel] = next;

    drdy_sync(bus, channel);
    return true;
}

bool max31856_pio_engine_channel_busy(const max31856_pio_bus_t *bus, uint8_t channel)
{
    if (!bus || channel >= bus->channel_count) {
        return false;
    }
    return bus->cs_low[channel] || bus->txn_open[channel];
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
