// spi_owner.h -- SPI0 bring-up and the one transfer primitive the MAX31856
// driver uses, per docs/ARCHITECTURE.md section 3's module table ("spi_owner
// | SPI0 (GPIO0/2/3) + CS0 (GPIO1) | ... CS driven as a plain GPIO so a
// register burst stays in one frame").
//
// JUDGEMENT CALL, documented per the task brief: on the main board,
// esp_spi_owner.h is a request-queue *task* because MAX31856.c there shares
// one SPI bus across three thermocouple channels plus the ILI9488 display,
// all serialized through one FIFO-ordered worker so no two drivers'
// transactions can interleave. THERMOCOUPLE.md section 1 is explicit that
// this board's bus has exactly one device and "will never block on another
// driver's transaction" -- there is no second driver to serialize against,
// and ARCHITECTURE.md section 4's task table has no separate spi_owner
// row/priority/period at all, only thermo_task. A request-queue task here
// would be a queue, a worker task, priority/affinity bookkeeping and a
// completion semaphore built to solve a contention problem this board does
// not have. So this is a plain driver module: init once, then a
// mutex-guarded direct-call transfer function that thermo_task (its only
// caller) invokes from its own task context. The mutex exists for the same
// reason the KilnFW channel mutex does -- a future J7 I2C device does not
// change this bus, but a second caller of this module some day should not
// have to relearn that transfers are otherwise unguarded.
#ifndef SAFTYFW_SPI_OWNER_H
#define SAFTYFW_SPI_OWNER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Brings up SPI0 at 4 MHz / mode 1 (CPOL 0, CPHA 1 -- the datasheet's
// "CPHA bit polarity must be set to 1", THERMOCOUPLE.md section 1) on
// SAFTYFW_PIN_SPI0_{SCK,MOSI,MISO}, and configures SAFTYFW_PIN_SPI0_CS0 as a
// plain GPIO output, idling high (a CS left low would let the part latch
// garbage the moment SCLK starts toggling for any other reason). Creates the
// transfer mutex. Safe to call once; returns false (and leaves the mutex
// unset) if the mutex allocation fails.
bool spi_owner_init(void);

// One CS-low/transfer/CS-high transaction: tx and rx are both `len` bytes
// (rx may be NULL if the caller does not need the read-back, matching
// spi_write_read_blocking's own contract -- MOSI still clocks `tx` out
// either way, which is what a register write needs). Blocking; safe to call
// from any task context (guarded by the mutex spi_owner_init() created), but
// there is exactly one caller today (thermo_task) so contention is not
// expected. Returns false if the transfer did not move exactly `len` bytes,
// spi_owner_init() was never called, or the mutex could not be taken.
bool spi_owner_transfer(const uint8_t *tx, uint8_t *rx, size_t len);

#ifdef __cplusplus
}
#endif

#endif // SAFTYFW_SPI_OWNER_H
