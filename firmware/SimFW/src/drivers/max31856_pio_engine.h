// max31856_pio_engine.h -- C glue around the PIO SPI slave programs in
// max31856_spi_slave.pio, implementing docs/SPI_ACCESS_AUDIT.md section 6's
// "Plan B": the response path is PIO + DMA only, with no CPU in it at all.
//
// WHY NOT PLAN A. docs/DESIGN_NOTES.md section 3.2.1 budgeted 1.6 us for the first
// response byte, which SPI_ACCESS_AUDIT.md section 6 item 2 showed to be
// wrong by about 8x: the address byte's VALUE is only known once its last bit
// is latched on SCLK falling edge 8, and in mode 1 the first response bit
// must be on MISO by SCLK rising edge 9 -- half an SCLK period, ~125 ns at
// 4 MHz. An RP2040 interrupt cannot even be entered in that window, and the
// failure mode is not one late byte but a whole burst shifted by a bit
// position, i.e. plausible-looking wrong temperatures. So the ISR-staging
// design is gone.
//
// WHAT REPLACED IT (the full chain, per bus):
//
//   PIO RX SM (one per channel)
//     assembles each MOSI byte directly into a WORD-ALIGNED POINTER INTO THE
//     CHANNEL'S RESPONSE IMAGE (src/sim/max31856_resp_image.h) -- see that
//     header and the .pio shift contract. No arithmetic is left for anyone
//     downstream to do.
//        |  RX-FIFO-not-empty DREQ
//        v
//   DMA "sniff" channel (one per RX SM, transfer_count = 1)
//     copies exactly the FIRST word of each transaction into bus->addr_capture
//     and chains to:
//        |
//        v
//   DMA "load" channel (one per bus, transfer_count = 1)
//     copies addr_capture into the data channel's al3_read_addr_trig, which
//     both sets the read address and starts it. Also raises DMA_IRQ_0 so the
//     CPU can find out, at its leisure, which address the transaction used.
//        |
//        v
//   DMA "data" channel (one per bus)
//     streams response words image[addr], image[addr+1], ... into the shared
//     PIO TX FIFO, paced by TX-FIFO-not-full, with a 512-byte read-address
//     ring that reproduces the part's own 7Fh -> 00h auto-increment wrap.
//        |
//        v
//   PIO TX SM (one per bus) shifts them onto MISO, and tri-states MISO
//     whenever no CS on this bus is low.
//
// The transfer_count of 1 on the sniff channel is the ONLY thing that decides
// which word is the address byte. The PIO program is deliberately stateless
// about it (it preloads the image base on every byte), so a missed CS edge
// cannot mis-frame a transaction.
//
// The CPU's remaining jobs, all of them off the critical path by at least a
// full byte-time:
//   * DMA_IRQ_0 (load channel done): open the transaction in the register
//     model with the address byte recovered from addr_capture, and unmask the
//     RX-FIFO IRQ so the byte stream can be collected. Unmasking here rather
//     than leaving it permanently on is what makes the CPU physically unable
//     to race the sniff channel for the address word.
//   * PIO RX IRQ: apply write-data bytes; for a read, advance the reference
//     register model one byte so its auto-increment address (and hence the
//     ~DRDY release rule) stays in step with what the DMA is actually
//     streaming.
//   * CS rising edge: close the transaction, release ~DRDY if the read
//     touched a releasing register, stop the data channel, flush the TX SM,
//     re-arm the sniff channel.
//
// Ownership: one max31856_pio_bus_t instance per physical bus (PIO0 for the
// ESP-side 3-channel bus, PIO1 for the 1-channel safety bus), owned
// exclusively by spi_emu_a.c / spi_emu_b.c respectively. This driver is also
// the single owner of the DMA channels it claims (via dma_claim_unused_channel,
// disjoint from ct_wave_i2s.c's two -- formerly ct_wave_pwm.c's three,
// deleted 2026-08-23, docs/DESIGN_NOTES.md §3.3) and of the ~DRDY GPIOs.
//
// BUILD-VERIFIED, NOT HARDWARE-TIMING-VERIFIED -- see max31856_spi_slave.pio's
// file header for the full disclaimer. Nothing in this module has been run
// against a real SPI master. The parts that CAN be host-tested (the response
// image's contents and index arithmetic, and the ~DRDY state machine) were
// deliberately factored into src/sim/ so they are.
#ifndef SIMFW_DRIVERS_MAX31856_PIO_ENGINE_H
#define SIMFW_DRIVERS_MAX31856_PIO_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

#include "sim/max31856_regs.h"
#include "sim/max31856_resp_image.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bus A (ESP side, J6) uses 3; bus B (safety side, J7) uses 1. Sized to the
// larger case so one bus struct shape serves both.
#define MAX31856_PIO_ENGINE_MAX_CHANNELS 3u

// Response images are double-banked per channel: the owner task publishes a
// fresh image into the bank the hardware is NOT pointed at, then hands the
// new base to the PIO in one atomic 32-bit push. The RX state machine adopts
// a new base only while CS is high, so a burst in flight always finishes out
// of the coherent bank it started on -- the same "double-buffer + publish"
// shape src/sim/sim_snapshot.h documents for sim_engine's snapshot, applied
// at the granularity the DMA needs (a base pointer instead of a seq counter,
// because the reader here is a state machine that cannot retry a torn read).
#define MAX31856_PIO_ENGINE_BANKS 2u

// transfer_count loaded into the data channel at the start of every
// transaction. No real master clocks more than 17 bytes in one CS window
// (SPI_ACCESS_AUDIT.md section 1) and the read-address ring makes a longer
// one wrap rather than run away, so this only needs to be comfortably larger
// than any plausible burst; it is re-armed once per transaction from the CS
// rising-edge handler.
#define MAX31856_PIO_ENGINE_DATA_WORDS 256u

// Per-channel instrumentation, DESIGN_NOTES.md 3.2.1's "transactions, bytes, CRC-
// class errors (malformed transactions), write conflicts, first-byte-late
// events (TX FIFO underrun detected by PIO)... counted, never silent."
typedef struct {
    uint32_t transactions;      // address bytes seen (one per CS assertion)
    uint32_t bytes_rx;          // data bytes drained from this channel's RX FIFO (the address byte goes to the sniff DMA, not here)
    uint32_t bytes_tx;          // response bytes the data DMA delivered to the TX FIFO on this channel's behalf
    uint32_t protocol_errors;   // malformed-transaction class: a byte arrived with no channel slot able to account for it, or a transaction was opened with no CS low
    uint32_t write_conflicts;   // a write-data byte arrived while this channel's tracked transaction state says "no write open" (RX/txn-state desync)
    // Genuine response-path underruns, and ONLY those. Under Plan B this is
    // measured from the DMA's own delivered-word count, not from the PIO's
    // sticky FDEBUG.TXSTALL bit, which fixes SPI_ACCESS_AUDIT.md D3 by
    // construction rather than by bookkeeping: the TX state machine still
    // stalls structurally at the start of every transaction (it enters
    // byte_loop on the first SCLK edge after CS falls, while the first
    // response word cannot exist until the address byte has been decoded
    // eight clocks later), and that stall is now simply never consulted.
    //
    // Two things increment this:
    //   * the data channel delivered fewer words than the master clocked
    //     response bytes -- an unambiguous mid-burst starvation;
    //   * the CS rising-edge handler was still re-arming the sniff channel
    //     when the NEXT transaction's address word had already landed in the
    //     RX FIFO, i.e. the responder was late to arm for that transaction.
    //
    // Still true, and worth repeating: lateness of the first response byte
    // *within* the 125 ns window is not observable in-band by any means the
    // fixture has. It shows up as wrong data at the master
    // (tools/spi_test_master's suspected_first_byte_late heuristic) or on a
    // logic-analyzer capture. A run with a nonzero count here is invalid; a
    // run with a zero count is necessary, not sufficient.
    uint32_t first_byte_late;
} max31856_pio_stats_t;

// Everything the caller has to decide, in one struct rather than nine
// positional arguments -- the pin/image/channel relationships below are the
// kind that get silently mis-ordered at a call site.
typedef struct {
    PIO pio;
    uint sclk_gpio;
    uint mosi_gpio;
    uint miso_gpio;
    uint8_t channel_count;                                    // 3 for bus A, 1 for bus B

    // Must list channel_count *consecutive* ascending GPIO numbers (CS0,
    // CS0+1, ...): the RX programs' per-channel jmp_pin wiring and the shared
    // TX program's `in pins, N` idle test both assume it. sclk_gpio must equal
    // mosi_gpio - 1 (mod 32) and cs_gpio[0] must equal sclk_gpio + 3 (mod 32)
    // -- see max31856_spi_slave.pio's header for why. init() checks both.
    uint cs_gpio[MAX31856_PIO_ENGINE_MAX_CHANNELS];

    // ~DRDY output per channel, or -1 for "not wired on this build". Driven
    // open-drain (asserted = actively pulled low, released = input/Hi-Z), per
    // DESIGN_NOTES.md 3.6's "direct GPIO, open-drain emulation".
    int drdy_gpio[MAX31856_PIO_ENGINE_MAX_CHANNELS];

    // Caller-owned register images, not copied.
    max31856_channel_t *channels[MAX31856_PIO_ENGINE_MAX_CHANNELS];

    // Caller-owned response-image banks, not copied. BOTH banks per channel
    // are required and each MUST be declared MAX31856_RESP_IMAGE_ALIGN and
    // pre-initialised with max31856_resp_image_init(); init() rejects a
    // misaligned or missing bank rather than letting the PIO's
    // OR-the-address-into-the-base trick silently produce a wrong pointer.
    max31856_resp_image_t *images[MAX31856_PIO_ENGINE_MAX_CHANNELS][MAX31856_PIO_ENGINE_BANKS];
} max31856_pio_engine_config_t;

// One physical bus (one PIO block: RX SM(s) + one shared TX SM + 2..4 DMA
// channels).
typedef struct {
    PIO pio;
    uint offset_rx;              // program offset shared by every RX SM instance on this bus
    uint offset_tx;               // program offset for the (bus-specific) TX program
    uint sm_rx[MAX31856_PIO_ENGINE_MAX_CHANNELS];
    uint sm_tx;
    uint8_t channel_count;         // 3 for bus A, 1 for bus B

    uint sclk_gpio;
    uint mosi_gpio;
    uint miso_gpio;
    uint cs_gpio[MAX31856_PIO_ENGINE_MAX_CHANNELS];
    int drdy_gpio[MAX31856_PIO_ENGINE_MAX_CHANNELS];

    max31856_channel_t *channels[MAX31856_PIO_ENGINE_MAX_CHANNELS];
    max31856_resp_image_t *images[MAX31856_PIO_ENGINE_MAX_CHANNELS][MAX31856_PIO_ENGINE_BANKS];
    uint8_t live_bank[MAX31856_PIO_ENGINE_MAX_CHANNELS];   // bank the hardware is currently pointed at

    // DMA channels (see the file header's chain diagram). -1 until claimed.
    int dma_sniff[MAX31856_PIO_ENGINE_MAX_CHANNELS];
    int dma_load;
    int dma_data;

    /* Robustness counter for the "a state machine got left disabled" bug
     * class, incremented by max31856_pio_engine_check_state_machines().
     *
     * This exists because that class of defect is completely silent from the
     * inside. On 2026-08-25 tx_reset() disabled the TX state machine and
     * returned without re-enabling it, which killed MISO permanently on the
     * first CS-rising edge of the first transaction. Nothing in the fixture
     * noticed or reported anything: the response image stayed correct, the
     * shadow truth stayed correct, the register model kept working, and the
     * only externally visible effect was that the master read a plausible
     * constant (exactly 0.0 C) instead of a temperature. A fixture whose whole
     * job is to be trustworthy silicon must not be able to fail that quietly.
     *
     * Deliberately a COUNTER AND A REPAIR, not an assert or a fatal: this
     * module runs a safety processor's only thermocouple on the bench, and
     * halting the fixture mid-run is worse than continuing with a repaired
     * state machine and a non-zero count. But the count is the point -- a
     * repair that left no trace would just be a slower version of the silent
     * failure it replaces. Non-zero here always means a real bug in this
     * file's own enable/disable pairing, never a hardware or wiring problem,
     * so it should be zero forever and any other value is a regression to
     * chase rather than tolerate. */
    uint32_t sm_disabled_repairs;

    // Written by the sniff DMA channel, read by the CPU in the load channel's
    // completion IRQ. One per bus, not per channel: only one CS on a bus is
    // ever low at a time (that is what SPI means), so there is only ever one
    // transaction in flight per bus to capture an address for.
    volatile uint32_t addr_capture;

    // Per-channel transaction bookkeeping. cs_low is set on the CS falling
    // edge (before the address is known) purely so the owner task's
    // max31856_pio_engine_channel_busy() gate closes at the physical start of
    // a transaction rather than ~2 us into it. txn_open means the register
    // model has an open transaction. Written ONLY from IRQ context; the
    // task-loop side must treat them as read-only via channel_busy().
    volatile bool cs_low[MAX31856_PIO_ENGINE_MAX_CHANNELS];
    volatile bool txn_open[MAX31856_PIO_ENGINE_MAX_CHANNELS];
    volatile bool txn_is_write[MAX31856_PIO_ENGINE_MAX_CHANNELS];
    volatile uint16_t txn_data_bytes[MAX31856_PIO_ENGINE_MAX_CHANNELS]; // data bytes seen after the address byte, i.e. response bytes the master clocked

    max31856_pio_stats_t stats[MAX31856_PIO_ENGINE_MAX_CHANNELS];
} max31856_pio_bus_t;

// Claims a PIO block's SMs and this bus's DMA channels, loads the RX program
// (shared) and the correct TX program variant for channel_count (3 -> tx_a,
// 1 -> tx_b), configures every GPIO listed, publishes each channel's bank-0
// image, and starts everything running (parked at "wait for CS" / "poll idle"
// / "armed for the next address word" -- no master activity is required for
// this call to succeed).
//
// Returns false, having claimed nothing it cannot release cleanly, if the
// config is inconsistent (channel count, pin arithmetic, missing or
// misaligned image bank).
//
// Resource exhaustion does NOT return false -- it does not return at all.
// PIO state-machine, PIO program-memory and DMA-channel exhaustion each halt
// the whole fixture through simfw_fatal() (drivers/simfw_fatal.h), naming the
// specific resource, rather than degrading into a silently-dead SPI path.
// That is deliberate: this init runs from spi_emu_a/b, which are pinned to
// core 1, and simfw_fatal()'s cross-core halt is what stops core 0 continuing
// to serve USB and telemetry as though the fixture were healthy.
//
// This paragraph used to claim false was returned for the exhaustion cases
// too. It was not: the state-machine claims passed required = true to
// pico-sdk, which panics internally and never returns, so the guard below it
// was unreachable and the panic bypassed the cross-core halt. Both claims now
// pass required = false and route through simfw_fatal() explicitly -- see
// their call sites in max31856_pio_engine.c.
bool max31856_pio_engine_init(max31856_pio_bus_t *bus,
                               const max31856_pio_engine_config_t *cfg);

// Enables this bus's PIO0/1_IRQ_0 vector, the shared DMA_IRQ_0 vector, and
// the shared GPIO callback (CS both edges on every CS gpio this bus owns),
// binding them to the CORE THE CALLER IS RUNNING ON via
// irq_set_exclusive_handler's normal core-affinity semantics. Must be called
// from spi_emu_a_task_fn/spi_emu_b_task_fn after those tasks are pinned to
// SIMFW_CORE_RT_PATH (core 1), not from whichever core runs static init.
//
// At most one bus may be active per PIO index (0 or 1) at a time.
void max31856_pio_engine_start_irq(max31856_pio_bus_t *bus);

// Rebuilds `channel`'s response image from its live register file and hands
// the new bank to the hardware. Call from the owner task after
// max31856_regs_advance_conversion() (and, cheaply, every scan, so
// corruption.bit_error_rate keeps re-rolling -- see max31856_resp_image.h's
// FIDELITY NOTE). Also re-derives the ~DRDY output level for the channel.
//
// Returns false and publishes nothing if the channel currently has CS low:
// the double-bank scheme only stays airtight if the retired bank is not
// rewritten while a burst that started on it could still be reading it, and
// "not busy" plus the owner tasks' 20 ms scan period against a 34 us
// worst-case transaction is what guarantees that.
bool max31856_pio_engine_refresh_image(max31856_pio_bus_t *bus, uint8_t channel);

// True if `channel` currently has CS low or an open transaction in the
// register model. The task-loop side must check this before calling
// max31856_regs_advance_conversion() or refresh_image() on that channel.
bool max31856_pio_engine_channel_busy(const max31856_pio_bus_t *bus, uint8_t channel);

/* Verifies the invariant that every state machine this bus owns -- one RX per
 * channel plus the single shared TX -- is actually enabled, and re-enables any
 * that is not, counting each repair in bus->sm_disabled_repairs.
 *
 * Call it from the owning task's periodic loop, NOT from an interrupt: it
 * reads PIO CTRL and may call pio_sm_set_enabled(), and it is deliberately
 * cheap enough (one register read plus a compare per bus) to run at the
 * task-loop cadence.
 *
 * MUST NOT be called while a transaction is in flight. Re-enabling a state
 * machine mid-byte would corrupt the byte in flight, which is the same reason
 * the CS-rise handler guards its own RX restart on CS actually being high.
 * Gate the call on max31856_pio_engine_channel_busy() being false for every
 * channel, exactly as the image-refresh path already does.
 *
 * Returns true if the invariant already held (nothing repaired). See
 * sm_disabled_repairs' own comment for why this is a counted repair rather
 * than an assert or a fatal. */
bool max31856_pio_engine_check_state_machines(max31856_pio_bus_t *bus);

max31856_pio_stats_t max31856_pio_engine_get_stats(const max31856_pio_bus_t *bus, uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_DRIVERS_MAX31856_PIO_ENGINE_H
