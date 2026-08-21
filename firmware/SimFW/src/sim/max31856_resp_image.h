// max31856_resp_image -- the pure, host-testable half of SimFW's "Plan B"
// MAX31856 read responder: a flat, DMA-streamable image of everything the
// emulated part could ever put on MISO, indexed by the raw first byte of a
// transaction.
//
// WHY THIS EXISTS (docs/SPI_ACCESS_AUDIT.md section 6 item 2). The address
// byte's value is only known after its last bit is latched on SCLK falling
// edge 8, and in SPI mode 1 the responder must present the first response bit
// on SCLK rising edge 9 -- about half an SCLK period, ~125 ns at 4 MHz. No
// RP2040 interrupt handler can index a register file inside that window, so
// the CPU is removed from the response path entirely: the PIO RX state
// machine assembles the address byte directly into a *pointer into this
// image*, a DMA control channel drops that pointer into a data channel's
// read-address trigger, and the data channel streams bytes into the PIO TX
// FIFO until CS rises. See src/drivers/max31856_pio_engine.c's "PLAN B DATA
// FLOW" comment for the wiring and the cycle budget.
//
// LAYOUT CONTRACT -- the PIO program, the DMA configuration and this header
// all have to agree, and the agreement is what the three previously-shipped
// bugs in this subsystem were made of, so it is spelled out here and asserted
// by max31856_pio_engine_publish_image() at run time:
//
//   * 256 entries of uint32_t. The index is the raw first byte of the
//     transaction, INCLUDING bit 7 (the read/write bit) -- no masking happens
//     anywhere on the hardware path, so the image simply covers both halves.
//   * Entries 00h..7Fh are the read space; 80h..FFh are a verbatim MIRROR of
//     it. A write transaction indexes the mirror, so MISO carries plausible
//     register data while the master is sending register data. The real part
//     leaves SDO undefined there and every master in this repo discards it
//     (SPI_ACCESS_AUDIT.md section 1), so mirroring is a free, harmless
//     choice that keeps the hardware path branch-free.
//   * Each entry is the response byte LEFT-JUSTIFIED into bits[31:24],
//     matching max31856_spi_slave.pio's OUT shift-left / 8-bit-autopull
//     contract. This is the same `byte << 24` convention the old ISR path
//     used with pio_sm_put().
//   * Addresses 10h..7Fh hold MAX31856_INVALID_ADDR_VALUE (FFh), per the
//     datasheet's "Invalid memory addresses report an FFh value" (page 15).
//   * The two 128-entry halves are each 512 bytes, and an instance must be
//     1024-byte aligned (MAX31856_RESP_IMAGE_ALIGN below). That alignment is
//     load-bearing twice over: the PIO builds the pointer by OR-ing the
//     address byte into the image base's high bits, and the DMA data channel
//     uses a 512-byte read-address ring so a burst that runs off the end of
//     the read space wraps 7Fh -> 00h exactly like the part's own 7-bit
//     address counter does.
//
// FIDELITY NOTE (a real, deliberate consequence of moving the read path off
// the CPU): corruption.bit_error_rate is applied when the image is PUBLISHED,
// not per byte clocked. Every read between two publishes therefore sees the
// same flipped bits, where the reference model
// (max31856_regs_clock_read_byte()) re-rolls per byte. The owner tasks
// republish every scan (20 ms), so "flaky MISO" is still flaky -- at
// per-scan rather than per-byte granularity. corruption.dead_mode, the knob
// that actually matters for fault injection, is exact either way.
//
// Pure C: no pico-sdk, no FreeRTOS, no DMA/PIO types. Everything about this
// file is host-testable and is tested in test/test_max31856_resp_image.c,
// including a byte-for-byte cross-check against the reference register
// machine for every one of the six real transaction shapes.
#ifndef SIMFW_SIM_MAX31856_RESP_IMAGE_H
#define SIMFW_SIM_MAX31856_RESP_IMAGE_H

#include <stdint.h>

#include "max31856_regs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* One half = the part's whole 7-bit address space. */
#define MAX31856_RESP_IMAGE_HALF     0x80u
/* Both halves = every value the raw first byte can take. */
#define MAX31856_RESP_IMAGE_ENTRIES  0x100u
/* Bytes spanned by one half -- the DMA read-ring size, and the reason the
 * whole image must be 1024-byte aligned. */
#define MAX31856_RESP_IMAGE_HALF_BYTES (MAX31856_RESP_IMAGE_HALF * 4u)
#define MAX31856_RESP_IMAGE_BYTES      (MAX31856_RESP_IMAGE_ENTRIES * 4u)
/* Left-justification shift, i.e. the OUT shift-left contract in one number. */
#define MAX31856_RESP_IMAGE_BYTE_SHIFT 24u

/* Alignment every instance MUST be declared with. Spelled portably because
 * this file also compiles into the MSVC host-test binary, where the alignment
 * is meaningless but must still parse. */
#if defined(_MSC_VER)
#define MAX31856_RESP_IMAGE_ALIGN __declspec(align(1024))
#else
#define MAX31856_RESP_IMAGE_ALIGN __attribute__((aligned(1024)))
#endif

typedef struct {
    uint32_t word[MAX31856_RESP_IMAGE_ENTRIES];
} max31856_resp_image_t;

/* Fills every entry with the invalid-address value. Call once per image
 * before the hardware can reach it, so a transaction that arrives before the
 * first publish streams FFh (the datasheet's own answer for an address that
 * reports nothing) rather than uninitialised RAM. */
void max31856_resp_image_init(max31856_resp_image_t *img);

/* Rebuilds the image from `ch`'s live register file, applying the same
 * corruption knobs max31856_regs_clock_read_byte() would. Takes a non-const
 * channel because bit-error injection consumes the channel's RNG state --
 * see the FIDELITY NOTE above.
 *
 * NOT concurrency-safe against a transaction in flight on the same image:
 * the caller must publish into a bank the hardware is not currently pointed
 * at. max31856_pio_engine_refresh_image() implements that double-buffering. */
void max31856_resp_image_publish(max31856_resp_image_t *img, max31856_channel_t *ch);

/* The response byte at `index` -- i.e. what MISO carries for the byte-position
 * the DMA reaches with that image index. Exists so host tests (and any future
 * telemetry) can read the image back in wire terms without duplicating the
 * left-justification convention. */
uint8_t max31856_resp_image_byte(const max31856_resp_image_t *img, uint16_t index);

/* The image index the hardware reaches for the k-th response byte of a
 * transaction whose raw first byte was `addr_byte`. Encodes the DMA read-ring
 * wrap (within one 128-entry half) so a test can predict the exact stream the
 * hardware produces, including the 7Fh -> 00h auto-increment wrap, without
 * any hardware. */
uint16_t max31856_resp_image_index(uint8_t addr_byte, uint16_t byte_offset);

#ifdef __cplusplus
}
#endif

#endif // SIMFW_SIM_MAX31856_RESP_IMAGE_H
