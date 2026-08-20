// spi_test_master -- SimFW bench utility, M-A's "scripted test master."
//
// Runs on a spare Pico wired to SimFW's PIO MAX31856 SPI *slave* emulator
// (bus A and/or bus B, see ../README.md's wiring table). Uses the RP2040's
// hardware SPI peripheral as a known-good reference master (mode 1,
// settable clock, individually-driven CS lines) to clock scripted MAX31856
// register-map transactions at the slave and verify every reply in
// firmware, so a >=10k-transaction soak (docs/PLAN.md section 10, M-A's
// exit criterion) produces a hard pass/fail number without needing a logic
// analyzer to adjudicate every byte.
//
// Control/results link: plain-text line protocol over native USB CDC
// (pico-sdk's stdio_usb, not a custom TinyUSB descriptor set and NOT
// SimFW's own framed binary `benchproto` protocol -- this tool must be
// debuggable with a plain terminal, simplicity is the feature, see the
// README). See handle_line() below for the full command set; `HELP` prints
// it at runtime too.
//
// BUILD-VERIFIED ONLY. No hardware attached in this environment -- nothing
// here has been run against a real Pico or a real SimFW slave. See
// ../README.md's "Status" section.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "pico/stdlib.h"

#include "seq.h"
#include "spi_master.h"

#include "max31856_regs.h"

#define LINE_MAX 128

static seq_stats_t s_stats;

static void print_help(void) {
    printf("spi_test_master command set:\n"
           "  PING                       -> PONG\n"
           "  RATE <hz>                  set SPI clock rate, replies actual Hz achieved\n"
           "  MODE <0|1>                 set CPHA (0 or 1); CPOL always 0. default 1 (SPI mode 1)\n"
           "  CS <0..3>                  select which CS line the next transaction asserts\n"
           "  READ <addr_hex>            single-byte read, replies value in hex\n"
           "  WRITE <addr_hex> <val_hex> single-byte write, replies OK\n"
           "  READN <addr_hex> <n>       multi-byte auto-increment read, replies n hex bytes\n"
           "  SEQ single <addr_hex> <expect_hex>\n"
           "  SEQ wrrd <addr_hex> <hexbytes>      write then auto-increment readback verify\n"
           "  SEQ autoinc <addr_hex> <expect_hexbytes>\n"
           "  SEQ b2b <expect_cr1_hex> <count>    back-to-back CR1 reads, minimal gap\n"
           "  SOAK <count>                run count self-checking iterations (M-A soak)\n"
           "  STATS                      print cumulative transactions/bytes/mismatches/suspected-first-byte-late\n"
           "  RESET                      clear cumulative stats\n"
           "  HELP                       this text\n");
}

static void print_stats(void) {
    printf("STATS transactions=%lu bytes_compared=%lu mismatches=%lu suspected_first_byte_late=%lu rate_hz=%lu mode_cpha=%u cs=%u\n",
           (unsigned long)s_stats.transactions, (unsigned long)s_stats.bytes_compared,
           (unsigned long)s_stats.mismatches, (unsigned long)s_stats.suspected_first_byte_late,
           (unsigned long)spi_master_get_rate_hz(), (unsigned)spi_master_get_mode(),
           (unsigned)spi_master_selected_cs());
}

static bool parse_hex_u32(const char *s, uint32_t *out) {
    if (s == NULL || *s == '\0') return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s) return false;
    *out = (uint32_t)v;
    return true;
}

static bool parse_dec_u32(const char *s, uint32_t *out) {
    if (s == NULL || *s == '\0') return false;
    char *end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s) return false;
    *out = (uint32_t)v;
    return true;
}

// Parses a run of hex-byte pairs ("0A0B0C...") with no separators into
// out[], returns the byte count, or 0 on a malformed string / overflow of
// max_len.
static uint8_t parse_hex_bytes(const char *s, uint8_t *out, uint8_t max_len) {
    size_t slen = strlen(s);
    if (slen == 0 || (slen % 2u) != 0u) return 0;
    uint8_t n = (uint8_t)(slen / 2u);
    if (n > max_len) return 0;
    char byte_str[3] = {0, 0, 0};
    for (uint8_t i = 0; i < n; i++) {
        byte_str[0] = s[i * 2u];
        byte_str[1] = s[i * 2u + 1u];
        char *end = NULL;
        unsigned long v = strtoul(byte_str, &end, 16);
        if (end != &byte_str[2]) return 0;
        out[i] = (uint8_t)v;
    }
    return n;
}

static void run_soak(uint32_t count) {
    // Establish a known-good CR1 oracle once, regardless of prior slave
    // state (see seq.h's seq_soak_iteration doc comment).
    uint8_t cr1_val = 0x03u; // reset default; harmless to (re)write even if already this value
    seq_stats_t setup_stats;
    seq_stats_reset(&setup_stats);
    if (!seq_write_readback(MAX31856_REG_CR1, &cr1_val, 1, &setup_stats)) {
        printf("SOAK ABORT could not establish CR1 oracle -- slave not responding correctly\n");
        return;
    }
    s_stats.transactions += setup_stats.transactions;
    s_stats.bytes_compared += setup_stats.bytes_compared;
    s_stats.mismatches += setup_stats.mismatches;

    uint32_t fails = 0;
    uint32_t start_mismatches = s_stats.mismatches;
    for (uint32_t i = 0; i < count; i++) {
        if (!seq_soak_iteration(i, cr1_val, &s_stats)) {
            fails++;
        }
        if ((i % 1000u) == 0u && i > 0u) {
            printf("SOAK progress %lu/%lu mismatches_so_far=%lu\n",
                   (unsigned long)i, (unsigned long)count,
                   (unsigned long)(s_stats.mismatches - start_mismatches));
        }
    }
    printf("SOAK DONE count=%lu failed_iterations=%lu mismatches=%lu suspected_first_byte_late=%lu result=%s\n",
           (unsigned long)count, (unsigned long)fails,
           (unsigned long)(s_stats.mismatches - start_mismatches),
           (unsigned long)s_stats.suspected_first_byte_late,
           (fails == 0u) ? "PASS" : "FAIL");
}

static void handle_line(char *line) {
    // Trim trailing CR/LF/whitespace.
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n' || line[len - 1] == ' ')) {
        line[--len] = '\0';
    }
    if (len == 0) return;

    char *save = NULL;
    char *cmd = strtok_r(line, " \t", &save);
    if (cmd == NULL) return;

    if (strcasecmp(cmd, "PING") == 0) {
        printf("PONG\n");
    } else if (strcasecmp(cmd, "HELP") == 0) {
        print_help();
    } else if (strcasecmp(cmd, "STATS") == 0) {
        print_stats();
    } else if (strcasecmp(cmd, "RESET") == 0) {
        seq_stats_reset(&s_stats);
        printf("OK\n");
    } else if (strcasecmp(cmd, "RATE") == 0) {
        char *arg = strtok_r(NULL, " \t", &save);
        uint32_t hz;
        if (!parse_dec_u32(arg, &hz)) {
            printf("ERR RATE needs a decimal Hz value\n");
            return;
        }
        uint32_t actual = spi_master_set_rate_hz(hz);
        printf("OK RATE %lu\n", (unsigned long)actual);
    } else if (strcasecmp(cmd, "MODE") == 0) {
        char *arg = strtok_r(NULL, " \t", &save);
        uint32_t cpha;
        if (!parse_dec_u32(arg, &cpha) || !spi_master_set_mode((uint8_t)cpha)) {
            printf("ERR MODE needs 0 or 1\n");
            return;
        }
        printf("OK MODE %u\n", (unsigned)cpha);
    } else if (strcasecmp(cmd, "CS") == 0) {
        char *arg = strtok_r(NULL, " \t", &save);
        uint32_t cs;
        if (!parse_dec_u32(arg, &cs) || !spi_master_select_cs((uint8_t)cs)) {
            printf("ERR CS needs 0..%u\n", (unsigned)(SPI_MASTER_CS_COUNT - 1u));
            return;
        }
        printf("OK CS %u\n", (unsigned)cs);
    } else if (strcasecmp(cmd, "READ") == 0) {
        char *a = strtok_r(NULL, " \t", &save);
        uint32_t addr;
        if (!parse_hex_u32(a, &addr) || addr >= MAX31856_REG_COUNT) {
            printf("ERR READ needs a valid hex addr\n");
            return;
        }
        uint8_t tx[2] = { (uint8_t)addr, 0 };
        uint8_t rx[2];
        spi_master_transact(tx, rx, 2);
        s_stats.transactions++;
        printf("OK READ %02X %02X\n", (unsigned)addr, rx[1]);
    } else if (strcasecmp(cmd, "WRITE") == 0) {
        char *a = strtok_r(NULL, " \t", &save);
        char *v = strtok_r(NULL, " \t", &save);
        uint32_t addr, val;
        if (!parse_hex_u32(a, &addr) || addr >= MAX31856_REG_COUNT || !parse_hex_u32(v, &val) || val > 0xFFu) {
            printf("ERR WRITE needs valid hex addr and value\n");
            return;
        }
        uint8_t tx[2] = { MAX31856_WRITE_ADDR((uint8_t)addr), (uint8_t)val };
        uint8_t rx[2];
        spi_master_transact(tx, rx, 2);
        s_stats.transactions++;
        printf("OK\n");
    } else if (strcasecmp(cmd, "READN") == 0) {
        char *a = strtok_r(NULL, " \t", &save);
        char *n = strtok_r(NULL, " \t", &save);
        uint32_t addr, count;
        if (!parse_hex_u32(a, &addr) || addr >= MAX31856_REG_COUNT || !parse_dec_u32(n, &count) || count == 0 || count > 16) {
            printf("ERR READN needs valid hex addr and 1..16 count\n");
            return;
        }
        uint8_t tx[1 + 16] = {0};
        uint8_t rx[1 + 16];
        tx[0] = (uint8_t)addr;
        spi_master_transact(tx, rx, (uint32_t)(1 + count));
        s_stats.transactions++;
        printf("OK READN %02X ", (unsigned)addr);
        for (uint32_t i = 0; i < count; i++) printf("%02X", rx[1 + i]);
        printf("\n");
    } else if (strcasecmp(cmd, "SEQ") == 0) {
        char *sub = strtok_r(NULL, " \t", &save);
        if (sub == NULL) {
            printf("ERR SEQ needs a subcommand (single|wrrd|autoinc|b2b)\n");
            return;
        }
        if (strcasecmp(sub, "single") == 0) {
            char *a = strtok_r(NULL, " \t", &save);
            char *e = strtok_r(NULL, " \t", &save);
            uint32_t addr, expect;
            if (!parse_hex_u32(a, &addr) || addr >= MAX31856_REG_COUNT || !parse_hex_u32(e, &expect) || expect > 0xFFu) {
                printf("ERR SEQ single needs valid hex addr and expect\n");
                return;
            }
            bool ok = seq_single_read((uint8_t)addr, (uint8_t)expect, &s_stats);
            printf("SEQ single %s\n", ok ? "PASS" : "FAIL");
        } else if (strcasecmp(sub, "wrrd") == 0) {
            char *a = strtok_r(NULL, " \t", &save);
            char *bytes = strtok_r(NULL, " \t", &save);
            uint32_t addr;
            uint8_t data[16];
            uint8_t n = bytes ? parse_hex_bytes(bytes, data, 16) : 0;
            if (!parse_hex_u32(a, &addr) || addr >= MAX31856_REG_COUNT || n == 0) {
                printf("ERR SEQ wrrd needs valid hex addr and hex byte string\n");
                return;
            }
            bool ok = seq_write_readback((uint8_t)addr, data, n, &s_stats);
            printf("SEQ wrrd %s\n", ok ? "PASS" : "FAIL");
        } else if (strcasecmp(sub, "autoinc") == 0) {
            char *a = strtok_r(NULL, " \t", &save);
            char *bytes = strtok_r(NULL, " \t", &save);
            uint32_t addr;
            uint8_t expect[16];
            uint8_t n = bytes ? parse_hex_bytes(bytes, expect, 16) : 0;
            if (!parse_hex_u32(a, &addr) || addr >= MAX31856_REG_COUNT || n == 0) {
                printf("ERR SEQ autoinc needs valid hex addr and hex expect-byte string\n");
                return;
            }
            bool ok = seq_autoinc_read((uint8_t)addr, expect, n, &s_stats);
            printf("SEQ autoinc %s\n", ok ? "PASS" : "FAIL");
        } else if (strcasecmp(sub, "b2b") == 0) {
            char *e = strtok_r(NULL, " \t", &save);
            char *c = strtok_r(NULL, " \t", &save);
            uint32_t expect, count;
            if (!parse_hex_u32(e, &expect) || expect > 0xFFu || !parse_dec_u32(c, &count) || count == 0) {
                printf("ERR SEQ b2b needs valid hex expect and decimal count\n");
                return;
            }
            bool ok = seq_back_to_back((uint8_t)expect, count, &s_stats);
            printf("SEQ b2b %s\n", ok ? "PASS" : "FAIL");
        } else {
            printf("ERR unknown SEQ subcommand\n");
        }
    } else if (strcasecmp(cmd, "SOAK") == 0) {
        char *c = strtok_r(NULL, " \t", &save);
        uint32_t count;
        if (!parse_dec_u32(c, &count) || count == 0) {
            printf("ERR SOAK needs a decimal iteration count\n");
            return;
        }
        run_soak(count);
    } else {
        printf("ERR unknown command '%s' -- try HELP\n", cmd);
    }
}

int main(void) {
    stdio_init_all();
    spi_master_init();
    seq_stats_reset(&s_stats);

    // Give the USB CDC host side a moment to enumerate before the first
    // banner -- harmless if a terminal is already attached (stdio_usb just
    // buffers/drops until then).
    sleep_ms(1500);
    printf("spi_test_master ready. SPI mode CPOL=0 CPHA=%u, rate=%lu Hz, CS=%u. Type HELP.\n",
           (unsigned)spi_master_get_mode(), (unsigned long)spi_master_get_rate_hz(),
           (unsigned)spi_master_selected_cs());

    char line[LINE_MAX];
    size_t idx = 0;
    while (true) {
        int c = getchar_timeout_us(10000);
        if (c == PICO_ERROR_TIMEOUT) {
            continue;
        }
        if (c == '\n' || c == '\r') {
            if (idx > 0) {
                line[idx] = '\0';
                handle_line(line);
                idx = 0;
            }
            continue;
        }
        if (idx < LINE_MAX - 1) {
            line[idx++] = (char)c;
        }
        // Silently drop overlong lines' excess characters rather than
        // crashing/overflowing -- a malformed command will just fail to
        // parse once terminated.
    }
    return 0;
}
