/* Byte-identical proof for ROADMAP.md M2's "KilnFW delegating framing and
 * CRC, proven byte-identical before the old code is deleted" item.
 *
 * This file embeds a verbatim COPY of the pre-migration static functions
 * from firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.c
 * (crc16_ccitt_false + the stuff-into-a-buffer loop from stuff_and_send,
 * renamed with an old_ prefix so they don't collide with kilnlink's own
 * symbols) and runs them side-by-side against kilnlink_crc16_ccitt_false /
 * kilnlink_stuff on:
 *   - every vector in test/vectors/frame_vectors.json's raw_hex/wire_hex
 *     pairs (hand-transcribed below since this is a from-scratch C test with
 *     no JSON parser)
 *   - a deterministic pseudo-random fuzz sweep over lengths 0..RAW_FRAME_MAX
 *     and byte content, including buffers saturated with FRAME_DELIM/FRAME_ESC
 *     bytes to stress the escaping path
 *
 * Both the CRC and the stuffed bytes must match exactly (same length, same
 * content) for every case. This is the "targeted comparison" fallback
 * described in the migration task: the OLD implementation lives inside
 * static functions in an ESP-IDF-only translation unit and can't be linked
 * on the host as-is, so its logic is reproduced verbatim here instead.
 *
 * Build:
 *   cl /nologo /W4 /I ..\include test_uart_protocol_delegate.c ..\src\kilnlink_frame.c ..\src\kilnlink_crc.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "kilnlink/kilnlink_frame.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* ---- verbatim copy of the OLD uart_protocol.c logic (pre-migration) ---- */

#define OLD_FRAME_DELIM   0x7Eu
#define OLD_FRAME_ESC     0x7Du
#define OLD_FRAME_ESC_XOR 0x20u

static uint16_t old_crc16_ccitt_false(const uint8_t *data, size_t len)
{
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)data[i] << 8;
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021) : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

/* Reproduces the stuffing loop inside old stuff_and_send() verbatim (minus
 * the uart_owner_transfer call, which is not part of the framing logic under
 * test). out must be sized raw_len*2+2 by the caller, same as the original's
 * STUFFED_FRAME_MAX-sized stack buffer. */
static size_t old_stuff(const uint8_t *raw, size_t raw_len, uint8_t *out)
{
    size_t o = 0;
    out[o++] = OLD_FRAME_DELIM;
    for (size_t i = 0; i < raw_len; ++i) {
        uint8_t b = raw[i];
        if (b == OLD_FRAME_DELIM || b == OLD_FRAME_ESC) {
            out[o++] = OLD_FRAME_ESC;
            out[o++] = (uint8_t)(b ^ OLD_FRAME_ESC_XOR);
        } else {
            out[o++] = b;
        }
    }
    out[o++] = OLD_FRAME_DELIM;
    return o;
}

/* ---- comparison helpers ---- */

static void compare_one(const uint8_t *raw, size_t raw_len, const char *label)
{
    uint16_t old_crc = old_crc16_ccitt_false(raw, raw_len);
    uint16_t new_crc = kilnlink_crc16_ccitt_false(raw, raw_len);
    if (old_crc != new_crc) {
        printf("CRC MISMATCH (%s): old=0x%04x new=0x%04x len=%zu\n", label, old_crc, new_crc, raw_len);
        g_failures++;
    }

    uint8_t old_out[600];
    uint8_t new_out[600];
    size_t old_len = old_stuff(raw, raw_len, old_out);
    size_t new_len = kilnlink_stuff(raw, raw_len, new_out, sizeof(new_out));

    if (old_len != new_len) {
        printf("STUFF LEN MISMATCH (%s): old=%zu new=%zu\n", label, old_len, new_len);
        g_failures++;
        return;
    }
    if (memcmp(old_out, new_out, old_len) != 0) {
        printf("STUFF BYTES MISMATCH (%s)\n", label);
        g_failures++;
    }
}

static unsigned int hex_nibble(char c)
{
    if (c >= '0' && c <= '9') return (unsigned int)(c - '0');
    if (c >= 'a' && c <= 'f') return (unsigned int)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (unsigned int)(c - 'A' + 10);
    return 0;
}

static void hex_to_bytes(const char *hex, uint8_t *out, size_t *out_len)
{
    size_t n = strlen(hex) / 2;
    for (size_t i = 0; i < n; ++i) {
        out[i] = (uint8_t)((hex_nibble(hex[2 * i]) << 4) | hex_nibble(hex[2 * i + 1]));
    }
    *out_len = n;
}

/* raw_hex values transcribed from test/vectors/frame_vectors.json. */
static void test_known_vectors(void)
{
    static const char *raw_hexes[] = {
        "0100050103000304deadbeefe51b",       /* data_with_payload */
        "02000000070107004112",               /* ack_zero_length_payload */
        "04012c0205000505017e027d038eff",     /* payload_needs_escaping */
    };
    for (size_t v = 0; v < sizeof(raw_hexes) / sizeof(raw_hexes[0]); ++v) {
        uint8_t buf[64];
        size_t len;
        hex_to_bytes(raw_hexes[v], buf, &len);
        compare_one(buf, len, raw_hexes[v]);
    }
}

/* Deterministic PRNG (no rand() portability concerns) so results are
 * reproducible across runs/compilers. */
static uint32_t g_rng_state = 0x1234ABCDu;
static uint32_t next_rand(void)
{
    g_rng_state ^= g_rng_state << 13;
    g_rng_state ^= g_rng_state >> 17;
    g_rng_state ^= g_rng_state << 5;
    return g_rng_state;
}

static void format_label(char *label, size_t cap, const char *prefix, size_t len)
{
    /* Manual decimal formatting to avoid sprintf's MSVC /W4 /WX deprecation
     * warning-as-error (C4996) -- this is test scaffolding, not the code
     * under test, so a tiny hand-rolled formatter is fine. */
    char digits[24];
    int nd = 0;
    size_t v = len;
    if (v == 0) {
        digits[nd++] = '0';
    } else {
        while (v > 0 && nd < (int)sizeof(digits)) {
            digits[nd++] = (char)('0' + (v % 10));
            v /= 10;
        }
    }
    size_t p = 0;
    while (prefix[p] != '\0' && p + 1 < cap) {
        label[p] = prefix[p];
        ++p;
    }
    for (int i = nd - 1; i >= 0 && p + 1 < cap; --i) {
        label[p++] = digits[i];
    }
    label[p] = '\0';
}

static void test_fuzz(void)
{
    char label[32];
    for (size_t len = 0; len <= 263; ++len) { /* HEADER_LEN + MAX_PAYLOAD + CRC_LEN */
        uint8_t raw[300];
        for (size_t i = 0; i < len; ++i) {
            raw[i] = (uint8_t)(next_rand() & 0xFF);
        }
        format_label(label, sizeof(label), "fuzz-random-len", len);
        compare_one(raw, len, label);
    }

    /* Buffers saturated with delimiter/escape bytes -- worst case for the
     * stuffing path (every byte escapes). */
    for (size_t len = 0; len <= 263; ++len) {
        uint8_t raw[300];
        for (size_t i = 0; i < len; ++i) {
            raw[i] = (i % 2 == 0) ? OLD_FRAME_DELIM : OLD_FRAME_ESC;
        }
        format_label(label, sizeof(label), "fuzz-delim-esc-len", len);
        compare_one(raw, len, label);
    }
}

int main(void)
{
    test_known_vectors();
    test_fuzz();

    if (g_failures == 0) {
        printf("ALL PASS: old uart_protocol.c CRC/framing == kilnlink_crc16_ccitt_false/kilnlink_stuff, byte-identical\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
