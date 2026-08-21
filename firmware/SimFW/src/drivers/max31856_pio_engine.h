// max31856_pio_engine.h -- C glue around the PIO SPI slave programs in
// max31856_spi_slave.pio, implementing docs/PLAN.md section 3.2.1's "Plan A
// -- ISR staging": the address byte lands in a channel's RX FIFO, a core-1
// interrupt handler (max31856_pio_engine_poll_irq(), see below) indexes that
// channel's max31856_channel_t register image (src/sim/max31856_regs.h) and
// feeds the shared TX FIFO with the auto-increment response stream.
//
// Ownership: one max31856_pio_bus_t instance per physical bus (PIO0 for the
// ESP-side 3-channel bus, PIO1 for the 1-channel safety bus), owned
// exclusively by spi_emu_a.c / spi_emu_b.c respectively -- this header has
// no static state of its own beyond the tiny bus-lookup table the ISR
// dispatcher needs (see the .c file), so nothing here fights the
// single-owner-per-peripheral doctrine (docs/PLAN.md section 4).
//
// BUILD-VERIFIED, NOT HARDWARE-TIMING-VERIFIED -- see max31856_spi_slave.pio's
// file header for the full disclaimer. Nothing in this module has been run
// against a real SPI master.
#ifndef SIMFW_DRIVERS_MAX31856_PIO_ENGINE_H
#define SIMFW_DRIVERS_MAX31856_PIO_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

#include "hardware/pio.h"

#include "sim/max31856_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

// Bus A (ESP side, J6) uses 3; bus B (safety side, J7) uses 1. Sized to the
// larger case so one bus struct shape serves both.
#define MAX31856_PIO_ENGINE_MAX_CHANNELS 3u

// Per-channel instrumentation, PLAN.md 3.2.1's "transactions, bytes, CRC-
// class errors (malformed transactions), write conflicts, first-byte-late
// events (TX FIFO underrun detected by PIO)... counted, never silent."
typedef struct {
    uint32_t transactions;      // address bytes seen (one per CS assertion)
    uint32_t bytes_rx;          // total bytes drained from this channel's RX FIFO
    uint32_t bytes_tx;          // total bytes pushed to the (possibly shared) TX FIFO on this channel's behalf
    uint32_t protocol_errors;   // malformed-transaction class: a byte arrived with no channel slot able to account for it (should never happen; see .c)
    uint32_t write_conflicts;   // a write-data byte arrived while this channel's tracked transaction state says "no write open" (RX/txn-state desync)
    // TX FIFO underrun observed for this channel AFTER a response byte had
    // already been staged -- i.e. the ISR genuinely fell behind the master's
    // clock mid-burst. The structural stall every transaction begins with
    // (the TX SM enters its byte_loop on the first SCLK edge after CS falls,
    // but the first response byte cannot exist until the address byte has
    // finished arriving eight clocks later) is discarded, not counted -- see
    // tx_clear_stall()/tx_note_stall() in the .c file. Counting it would make
    // this fire once per transaction forever.
    //
    // NOTE the name is now narrower than it reads: lateness of the FIRST
    // response byte specifically is NOT observable in-band, because that
    // stall is indistinguishable from the structural one. It shows up as
    // wrong data at the master (tools/spi_test_master's
    // suspected_first_byte_late heuristic) or on a logic-analyzer capture.
    uint32_t first_byte_late;
} max31856_pio_stats_t;

// One physical bus (one PIO block: RX SM(s) + one shared TX SM).
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

    max31856_channel_t *channels[MAX31856_PIO_ENGINE_MAX_CHANNELS]; // caller-owned register images, not copied

    // Per-channel open-transaction bookkeeping. Written ONLY from
    // max31856_pio_engine_poll_irq() (core-1 IRQ context per PLAN.md
    // 3.2.1's Plan A) -- spi_emu_a/b's own task-loop code must treat these
    // as read-only via max31856_pio_engine_channel_busy() below, never write
    // them directly (single-writer-within-the-module rule, mirroring the
    // repo's snapshot doctrine at a smaller scale).
    volatile bool txn_open[MAX31856_PIO_ENGINE_MAX_CHANNELS];
    volatile bool txn_is_write[MAX31856_PIO_ENGINE_MAX_CHANNELS];

    max31856_pio_stats_t stats[MAX31856_PIO_ENGINE_MAX_CHANNELS];

    // Underrun-detection bookkeeping for the shared TX SM (see .c's
    // handle_channel_rx_bytes()): which channel most recently pushed to the
    // TX FIFO, so a detected stall can be attributed to the right counter.
    uint8_t last_tx_channel;
} max31856_pio_bus_t;

// Claims a PIO block's SMs, loads the RX program (shared) and the correct
// TX program variant for channel_count (3 -> tx_a, 1 -> tx_b), configures
// every GPIO listed, and starts all state machines running (parked at their
// respective "wait for CS" / "poll idle" entry points -- no master activity
// is required for this call to succeed). Returns false if the PIO block
// cannot supply enough free state machines/program space (should not happen
// on system boot, since nothing else on this bus's PIO block is claimed yet,
// but the caller -- e.g. a future host/loopback test harness -- should still
// check it).
//
// cs_gpio must list channel_count *consecutive* ascending GPIO numbers
// (CS0, CS0+1, CS0+2, ...) -- both the RX program's per-channel jmp_pin
// wiring and, more importantly, the shared TX program's `in pins, N` idle
// test assume this. sclk_gpio must equal mosi_gpio - 1 (mod 32) and
// cs_gpio[0] must equal sclk_gpio + 3 (mod 32) -- see
// max31856_spi_slave.pio's header comment for why. spi_emu_a.c / spi_emu_b.c
// pick GPIO numbers satisfying this by construction; this function does not
// re-derive or re-validate the arithmetic itself.
bool max31856_pio_engine_init(max31856_pio_bus_t *bus, PIO pio,
                               uint sclk_gpio, uint mosi_gpio, uint miso_gpio,
                               const uint *cs_gpio, uint8_t channel_count,
                               max31856_channel_t *const *channels);

// Enables this bus's PIO0/1_IRQ_0 vector (RX-FIFO-not-empty on every RX SM)
// and the shared GPIO callback (CS rising edge on every CS gpio this bus
// owns) and binds both to the CORE THE CALLER IS RUNNING ON via
// irq_set_exclusive_handler's normal core-affinity semantics -- PLAN.md
// 3.2.1's "address byte triggers a PIO RX IRQ pinned to core 1" means this
// must be called from spi_emu_a_task_fn/spi_emu_b_task_fn after those tasks
// have already been pinned to SIMFW_CORE_RT_PATH (core 1), not from
// whichever core happens to run static init.
//
// At most one bus may be active per PIO index (0 or 1) at a time -- this
// matches the real system (one spi_emu_a instance on PIO0, one spi_emu_b
// instance on PIO1) and is enforced by a small static lookup table in the
// .c file, not by hardware.
void max31856_pio_engine_start_irq(max31856_pio_bus_t *bus);

// True if `channel` currently has an open SPI transaction (CS low). The
// task-loop side (spi_emu_a_task_fn/spi_emu_b_task_fn) must check this
// before calling max31856_regs_advance_conversion() on that channel and
// skip the update for this tick if busy -- max31856_regs.h's coherency
// guarantee already protects an in-flight READ (it snapshots at CS-assert),
// but a WRITE transaction touches ch->regs directly via
// max31856_regs_clock_write_byte() and could race a same-core ISR
// preemption mid-write if advance_conversion() were allowed to run
// concurrently (PLAN.md 3.2.1: register-image commits happen "only between
// transactions, never while that channel's CS is low"). Skipping one ~ms
// tick is invisible against the ~100 ms nominal conversion cadence.
bool max31856_pio_engine_channel_busy(const max31856_pio_bus_t *bus, uint8_t channel);

max31856_pio_stats_t max31856_pio_engine_get_stats(const max31856_pio_bus_t *bus, uint8_t channel);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_DRIVERS_MAX31856_PIO_ENGINE_H
