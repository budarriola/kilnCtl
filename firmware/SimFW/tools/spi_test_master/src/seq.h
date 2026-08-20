// seq.h -- scripted MAX31856-register-level SPI transaction sequences and
// their in-firmware verification, run against SimFW's PIO slave emulator
// through spi_master.h. See ../README.md for the soak/sweep design this
// backs and firmware/SimFW/docs/PLAN.md section 10 M-A for why every byte
// must be verified on-device rather than left to a logic analyzer.
#ifndef SPI_TEST_MASTER_SEQ_H
#define SPI_TEST_MASTER_SEQ_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t transactions;        // total SPI transactions attempted
    uint32_t bytes_compared;      // total response bytes checked against an expected value
    uint32_t mismatches;          // bytes that did not match
    uint32_t suspected_first_byte_late; // transactions where byte[0] mismatched but every
                                          // later byte in the same transaction matched --
                                          // the master-side signature of PLAN.md 3.2.1's
                                          // "first-byte-late" TX-FIFO-underrun failure mode
                                          // (see ../README.md's troubleshooting table). This
                                          // is a heuristic, not a direct underrun detector --
                                          // only the slave's own PIO-side counters (M-A's
                                          // Saleae capture, or SimFW's own STATS if a link
                                          // exists) can prove an underrun actually happened.
} seq_stats_t;

void seq_stats_reset(seq_stats_t *s);

// --- One-shot scripted tests (human-triggered via SEQ <name> ..., print a
// PASS/FAIL verdict plus the offending bytes on failure). All operate on
// the currently-selected CS (spi_master_select_cs()). ---

// Single-byte read of `addr`; expects `expect` back. Exercises the "single-
// byte reads" case from the task brief.
bool seq_single_read(uint8_t addr, uint8_t expect, seq_stats_t *stats);

// Writes `len` bytes starting at `addr` (auto-increment write), then reads
// them back with a separate auto-increment read transaction and compares.
// This is PLAN.md's "register writes followed by readback verification"
// AND exercises multi-byte auto-increment reads in the same call -- see
// README for why CJHF..CJTO (0x03-0x09) is the register block used for
// this by default (plain R/W, no self-clearing/gated-writability quirks
// that would make the "expected" value ambiguous).
bool seq_write_readback(uint8_t addr, const uint8_t *data, uint8_t len, seq_stats_t *stats);

// Pure multi-byte auto-increment read of `len` bytes starting at `addr`,
// compared against caller-supplied `expect[len]`. The case PLAN.md 3.2.1's
// 1.6us latency budget is really about.
bool seq_autoinc_read(uint8_t addr, const uint8_t *expect, uint8_t len, seq_stats_t *stats);

// Back-to-back transactions with minimal inter-frame gap: runs `count`
// single-byte reads of CR1 (addr 0x01) one immediately after another with
// no delay between spi_master_transact() calls, verifying each against
// `expect_cr1`.
bool seq_back_to_back(uint8_t expect_cr1, uint32_t count, seq_stats_t *stats);

// --- Soak primitive (SOAK <n>): one self-checking iteration, folding
// write-then-auto-increment-readback (multi-byte case) + a single-byte read
// + CS rotation across bus A's 3 channels into one call so a >=10k-
// transaction run produces a hard pass/fail number per the M-A exit
// criterion. `iteration` seeds a deterministic-but-varying test pattern
// (avoids a soak silently passing because every iteration writes the same
// bytes). Rotates spi_master_select_cs() through 0,1,2 (bus A's three CS
// lines) as iteration % 3, which is also this tool's multi-CS/tri-state
// exercise (PLAN.md's "read from one CS while the others are idle, verify
// no contention garbage" -- contention would corrupt the compared bytes and
// show up as a mismatch here, no separate detector needed). Returns true if
// this iteration had zero mismatches. */
// expect_cr1: the value the caller has already established CR1 holds (see
// main.c's SOAK handler, which writes CR1 once before the loop so this
// oracle is correct regardless of prior slave state, rather than assuming
// the part is at its power-on reset default).
bool seq_soak_iteration(uint32_t iteration, uint8_t expect_cr1, seq_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif // SPI_TEST_MASTER_SEQ_H
