// spi_master.h -- thin wrapper around the RP2040's hardware SPI0 peripheral,
// configured as a MASTER, plus software-controlled chip-select GPIOs.
//
// This is the "known-good reference master" half of the M-A bench tool
// (see ../README.md and firmware/SimFW/docs/PLAN.md section 10, milestone
// M-A). It deliberately uses the RP2040's *hardware* SPI block rather than
// PIO: being the master is the easy direction, and hardware SPI is
// trivially trustworthy, which is the entire point of a reference master
// clocking SimFW's PIO *slave* emulator.
#ifndef SPI_TEST_MASTER_SPI_MASTER_H
#define SPI_TEST_MASTER_SPI_MASTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Pin plan (this tool's own GPIOs, independent of and does not need to
// match SimFW's slave-side numbering -- see ../README.md's wiring table for
// how these map onto SimFW bus A / bus B pins on the bench):
//   SPI0 SCK  = GPIO18
//   SPI0 MOSI (TX) = GPIO19
//   SPI0 MISO (RX) = GPIO16
//   CS0..CS3  = GPIO20, GPIO21, GPIO22, GPIO26 (plain GPIO outputs,
//               software-driven -- NOT the SPI peripheral's own CSn
//               function, since we need to select among up to 4 chip
//               selects individually to exercise SimFW bus A's 3-CS/shared-
//               MISO behavior and bus B's single CS from one firmware).
#define SPI_MASTER_SCK_GPIO   18u
#define SPI_MASTER_MOSI_GPIO  19u
#define SPI_MASTER_MISO_GPIO  16u

#define SPI_MASTER_CS_COUNT   4u
extern const uint8_t spi_master_cs_gpio[SPI_MASTER_CS_COUNT];

// Datasheet/DESIGN_NOTES.md 3.2 default: SPI mode 1 (CPOL=0, CPHA=1) -- what the
// real MAX31856 masters (KilnFW/SaftyFW drivers) use, confirmed against
// their actual SPI config and the MAX31856 datasheet's Table 5 (see
// max31856_spi_slave.pio's header for the full derivation). That file's
// RX/TX programs were originally coded backwards (mode 0 edges) and have
// since been corrected to true mode 1. A MODE command can still flip this
// wrapper to mode 0 at runtime as a diagnostic escape hatch for any future
// regression in that area; it is no longer needed to resolve which mode is
// "actually right" -- mode 1 is.
void spi_master_init(void);

// Sets the SPI clock rate. Returns the actual achieved rate (the RP2040
// clock divider is not exact for every requested value); spi_master.c's
// caller should report this back to the operator rather than assume the
// requested rate was hit exactly.
uint32_t spi_master_set_rate_hz(uint32_t hz);
uint32_t spi_master_get_rate_hz(void);

// cpha: 0 or 1 (CPOL is always held at 0 -- neither real master uses
// CPOL=1, and DESIGN_NOTES.md 3.2 never mentions it). Returns false for an
// out-of-range cpha value (no change made).
bool spi_master_set_mode(uint8_t cpha);
uint8_t spi_master_get_mode(void);

// Selects which CS line subsequent transactions assert (0..SPI_MASTER_CS_COUNT-1).
// All CS lines are held idle-high (deasserted) at init and between
// transactions -- only spi_master_transact() below asserts one, and only
// for the duration of that call, matching a real MAX31856 master's framing.
bool spi_master_select_cs(uint8_t cs_index);
uint8_t spi_master_selected_cs(void);

// One full SPI transaction on the currently selected CS: asserts CS, writes
// tx_len bytes from tx (may be NULL if tx_len==0, e.g. a pure read after
// the address byte is included in tx already -- see the caller in
// seq.c, which always folds the address byte into tx[0]), reads back
// exactly tx_len bytes into rx (full-duplex, byte for byte, standard SPI
// semantics -- MISO bytes clocked out during the address byte are
// discarded by the caller, matching the datasheet's "don't care" byte),
// then deasserts CS. Blocking; RP2040 hardware SPI FIFOs handle the
// byte-to-byte pacing, so back-to-back transact() calls with no work
// between them is this tool's "minimal inter-frame gap" mode (see
// ../README.md).
void spi_master_transact(const uint8_t *tx, uint8_t *rx, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif // SPI_TEST_MASTER_SPI_MASTER_H
