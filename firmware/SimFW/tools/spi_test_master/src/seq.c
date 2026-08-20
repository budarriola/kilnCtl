#include "seq.h"

#include <stdio.h>
#include <string.h>

#include "spi_master.h"

// Register addresses/write-bit convention come from SimFW's own slave
// model, not a re-guess at the datasheet (task brief: "so the master's
// expectations match the slave's model"). Read-only reference -- this
// project's CMakeLists.txt adds SimFW/src to the include path for exactly
// this header and links nothing else from SimFW.
#include "max31856_regs.h"

void seq_stats_reset(seq_stats_t *s) {
    memset(s, 0, sizeof(*s));
}

// Clocks one full transaction: tx[0] is the address byte (with write bit
// already applied by the caller via MAX31856_WRITE_ADDR / left as a plain
// read address), tx[1..len-1] are write-data bytes (ignored for a read --
// pass zeros). rx[0] is the address byte's don't-care echo; rx[1..len-1]
// are the response bytes callers care about for a read.
static void do_transact(uint8_t addr_byte, const uint8_t *wdata, uint8_t wlen,
                         uint8_t *rx_out /* len wlen+1 */) {
    uint8_t tx[1 + 32];
    uint8_t rx[1 + 32];
    tx[0] = addr_byte;
    if (wdata != NULL) {
        memcpy(&tx[1], wdata, wlen);
    } else {
        memset(&tx[1], 0, wlen);
    }
    spi_master_transact(tx, rx, (uint32_t)(1 + wlen));
    memcpy(rx_out, rx, (size_t)(1 + wlen));
}

static void record_compare(seq_stats_t *stats, const uint8_t *got, const uint8_t *expect,
                            uint8_t len) {
    stats->transactions++;
    bool first_mismatch = (len > 0) && (got[0] != expect[0]);
    bool rest_ok = true;
    for (uint8_t i = 0; i < len; i++) {
        stats->bytes_compared++;
        if (got[i] != expect[i]) {
            stats->mismatches++;
            if (i > 0) {
                rest_ok = false;
            }
        }
    }
    if (first_mismatch && rest_ok && len > 1) {
        stats->suspected_first_byte_late++;
    }
}

bool seq_single_read(uint8_t addr, uint8_t expect, seq_stats_t *stats) {
    uint8_t rx[2];
    do_transact(addr, NULL, 1, rx);
    uint8_t got = rx[1];
    record_compare(stats, &got, &expect, 1);
    bool ok = (got == expect);
    if (!ok) {
        printf("FAIL single_read addr=%02Xh expect=%02Xh got=%02Xh\n", addr, expect, got);
    }
    return ok;
}

bool seq_write_readback(uint8_t addr, const uint8_t *data, uint8_t len, seq_stats_t *stats) {
    if (len == 0 || len > 16) {
        printf("FAIL write_readback bad len=%u\n", len);
        return false;
    }
    uint8_t wrx[1 + 16];
    do_transact(MAX31856_WRITE_ADDR(addr), data, len, wrx);
    stats->transactions++; // write side counted as its own transaction; no byte compare on a write

    uint8_t rrx[1 + 16];
    do_transact(addr, NULL, len, rrx);
    const uint8_t *got = &rrx[1];
    record_compare(stats, got, data, len);

    bool ok = (memcmp(got, data, len) == 0);
    if (!ok) {
        printf("FAIL write_readback addr=%02Xh len=%u wrote=", addr, len);
        for (uint8_t i = 0; i < len; i++) printf("%02X", data[i]);
        printf(" read=");
        for (uint8_t i = 0; i < len; i++) printf("%02X", got[i]);
        printf("\n");
    }
    return ok;
}

bool seq_autoinc_read(uint8_t addr, const uint8_t *expect, uint8_t len, seq_stats_t *stats) {
    if (len == 0 || len > 16) {
        printf("FAIL autoinc_read bad len=%u\n", len);
        return false;
    }
    uint8_t rx[1 + 16];
    do_transact(addr, NULL, len, rx);
    const uint8_t *got = &rx[1];
    record_compare(stats, got, expect, len);
    bool ok = (memcmp(got, expect, len) == 0);
    if (!ok) {
        printf("FAIL autoinc_read addr=%02Xh len=%u expect=", addr, len);
        for (uint8_t i = 0; i < len; i++) printf("%02X", expect[i]);
        printf(" got=");
        for (uint8_t i = 0; i < len; i++) printf("%02X", got[i]);
        printf("\n");
    }
    return ok;
}

bool seq_back_to_back(uint8_t expect_cr1, uint32_t count, seq_stats_t *stats) {
    bool all_ok = true;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t rx[2];
        do_transact(MAX31856_REG_CR1, NULL, 1, rx);
        uint8_t got = rx[1];
        record_compare(stats, &got, &expect_cr1, 1);
        if (got != expect_cr1) {
            all_ok = false;
        }
        // No delay inserted between iterations -- CS deasserts and
        // reasserts back-to-back inside spi_master_transact(), which is
        // this tool's "minimal inter-frame gap" case.
    }
    return all_ok;
}

// Soak's write-verify block: CJHF..CJTO (0x03..0x09, 7 bytes) is a
// contiguous run of plain R/W registers per max31856_regs.h's table --
// none of CR0's self-clearing bits, none of CJTH/CJTL's CJ-disable-gated
// writability, so "what we wrote" is unambiguously "what must read back."
#define SOAK_BLOCK_ADDR MAX31856_REG_CJHF
#define SOAK_BLOCK_LEN  7u

bool seq_soak_iteration(uint32_t iteration, uint8_t expect_cr1, seq_stats_t *stats) {
    // Rotate CS 0,1,2 across bus A's three emulated channels -- exercises
    // the shared-MISO tri-state path (task brief: "read from one CS while
    // the others are idle, verify no contention garbage"). If wired to bus
    // B instead (single CS), spi_master_select_cs() only has CS0 to land
    // on regardless of this rotation, which is harmless.
    spi_master_select_cs((uint8_t)(iteration % 3u));

    // Deterministic-but-varying pattern so a soak can't pass by accident of
    // every iteration writing the same bytes back to themselves.
    uint8_t pattern[SOAK_BLOCK_LEN];
    for (uint8_t i = 0; i < SOAK_BLOCK_LEN; i++) {
        pattern[i] = (uint8_t)((iteration * 37u + i * 11u + 5u) & 0xFFu);
    }

    bool ok = seq_write_readback(SOAK_BLOCK_ADDR, pattern, SOAK_BLOCK_LEN, stats);

    // Single-byte read case, folded into the same iteration: CR1 is
    // untouched by the write-readback block above, so whatever the caller
    // established it holds (expect_cr1) is a stable oracle for every
    // iteration without needing a slave reset between iterations.
    bool ok2 = seq_single_read(MAX31856_REG_CR1, expect_cr1, stats);

    return ok && ok2;
}
