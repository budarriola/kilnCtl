/* SaftyFW/TODO.md Phase 1's "KilnFW's uart_protocol.c delegating framing/CRC,
 * proven byte-identical" item.
 *
 * uart_protocol.c (App/drivers/espInterfaces/) is switching from two local
 * static wrapper functions -- crc16_ccitt_false()/stuff_and_send() -- that
 * merely called through to kilnlink_crc16_ccitt_false()/kilnlink_stuff(), to
 * calling those CommonFW functions directly at each use site. The wrappers
 * were pure pass-throughs (no algorithm of their own), so this is a
 * mechanical simplification, not a behavior change -- but per this repo's
 * "prove byte-identity before you delete anything" rule for link framing
 * code, this file proves it anyway, the same way
 * firmware/CommonFW/test/test_uart_protocol_delegate.c proved the original
 * migration (from a genuinely independent from-scratch implementation) back
 * when uart_protocol.c still had one.
 *
 * old_crc16_ccitt_false()/old_stuff() below are a verbatim reproduction of
 * what uart_protocol.c's now-deleted wrapper functions computed (which is, in
 * turn, byte-identical to kilnlink_crc16_ccitt_false()/kilnlink_stuff() --
 * they just called them). Comparing against that reproduction, rather than
 * skipping the test because "it's just a wrapper", is what keeps this test
 * meaningful if kilnlink's algorithm ever changes out from under
 * uart_protocol.c's callers without their noticing: a silent behavior change
 * in CommonFW would fail HERE, not just disagree with itself.
 *
 * Built as its own executable by build_host_tests.ps1 (links CommonFW's
 * kilnlink_frame.c/kilnlink_crc.c directly, which the main test executable
 * does not need for anything else). */

#include <stdio.h>
#include <string.h>

#include "kilnlink/kilnlink_frame.h"

/* The RX read-buffer sizing constant, from the REAL header (the stub
 * uart_protocol.h in test/stubs/ is a different file and is not what the
 * firmware compiles). Included here rather than in a new executable because
 * this file is already the one that owns uart_protocol.c's framing sizes. */
#include "../drivers/owners/uart_protocol.h"

static int g_failures = 0;

#define CHECK(cond, msg)                                                    \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);          \
            g_failures++;                                                   \
        }                                                                   \
    } while (0)

/* ---- verbatim reproduction of uart_protocol.c's deleted wrapper bodies --- */

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

/* out must be sized raw_len*2+2 by the caller, same as stuff_and_send()'s old
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

/* ---- comparison ---- */

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

/* The five required cases: empty payload, a payload containing 0x7E, one
 * containing the escape byte 0x7D, one containing both adjacent, and a
 * maximum-length payload (KILNLINK_FRAME_RAW_MAX raw bytes, before
 * stuffing -- the largest frame uart_protocol.c ever builds). */
static void test_required_cases(void)
{
    uint8_t empty[1];
    compare_one(empty, 0, "empty");

    uint8_t with_delim[] = { 0x01, 0x02, OLD_FRAME_DELIM, 0x03, 0x04 };
    compare_one(with_delim, sizeof(with_delim), "contains-0x7E");

    uint8_t with_esc[] = { 0x01, 0x02, OLD_FRAME_ESC, 0x03, 0x04 };
    compare_one(with_esc, sizeof(with_esc), "contains-0x7D");

    uint8_t with_both[] = { 0x01, OLD_FRAME_DELIM, OLD_FRAME_ESC, 0x02,
                             OLD_FRAME_ESC, OLD_FRAME_DELIM, 0x03 };
    compare_one(with_both, sizeof(with_both), "contains-both-adjacent");

    static uint8_t max_payload[KILNLINK_FRAME_RAW_MAX];
    for (size_t i = 0; i < sizeof(max_payload); ++i) {
        /* Deliberately includes delimiter/escape bytes throughout, not just
         * benign fill -- the maximum-length case should also be the worst
         * case for stuffing expansion. */
        max_payload[i] = (uint8_t)(i * 37u + 5u);
    }
    compare_one(max_payload, sizeof(max_payload), "max-length");
}

/* Pins the wire bytes of one known frame against hardcoded expected output,
 * so a future change to CommonFW's kilnlink_crc16_ccitt_false()/kilnlink_
 * stuff() cannot silently move this wire without a host test noticing --
 * comparing two implementations against each other (test_required_cases,
 * above) proves agreement, but proves nothing if both drift together. */
static void test_known_frame_wire_bytes(void)
{
    /* A DATA frame: type=0x01, msg_index=0x0005, src_device=1(HOST),
     * src_task=3, dst_device=0(ESP), dst_task=4, length=3, payload={0xDE,
     * 0xAD, 0xBE}. Same header shape as CommonFW/test/vectors/
     * frame_vectors.json's "data_with_payload" case, transcribed here so this
     * file has no JSON-parsing dependency. */
    static const uint8_t raw[] = {
        0x01, 0x00, 0x05, 0x01, 0x03, 0x00, 0x04, 0x03, 0xDE, 0xAD, 0xBE,
    };
    uint16_t crc = kilnlink_crc16_ccitt_false(raw, sizeof(raw));
    /* CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF) of the 11 raw bytes above,
     * independently computed (not read off kilnlink's own output) and pinned
     * here so a future change to kilnlink_crc16_ccitt_false() is caught. */
    CHECK(crc == 0xA5A3, "known frame CRC pinned to 0xA5A3");

    uint8_t full_raw[sizeof(raw) + 2];
    memcpy(full_raw, raw, sizeof(raw));
    full_raw[sizeof(raw)] = (uint8_t)(crc >> 8);
    full_raw[sizeof(raw) + 1] = (uint8_t)(crc & 0xFF);

    uint8_t stuffed[64];
    size_t stuffed_len = kilnlink_stuff(full_raw, sizeof(full_raw), stuffed, sizeof(stuffed));
    static const uint8_t expected_wire[] = {
        0x7E, 0x01, 0x00, 0x05, 0x01, 0x03, 0x00, 0x04, 0x03, 0xDE, 0xAD, 0xBE,
        0xA5, 0xA3, 0x7E,
    };
    CHECK(stuffed_len == sizeof(expected_wire), "known frame wire length pinned");
    if (stuffed_len == sizeof(expected_wire)) {
        CHECK(memcmp(stuffed, expected_wire, stuffed_len) == 0, "known frame wire bytes pinned");
    }
}

/* The RX buffer must always hold at least one worst-case stuffed frame, and
 * -- since 2026-08-28, at the owner's request -- a whole 2 kB of read
 * headroom above that. Recomputed here from the wire constants rather than
 * copied, so shrinking UART_PROTO_MAX_PAYLOAD's ceiling or changing the
 * header length can never quietly make the buffer too small for one frame.
 *
 * This pins a NUMBER, which is only meaningful next to the invariant that
 * makes the number safe: the read must never wait on this buffer filling
 * (LINK_PROTOCOL.md, "Never wait on a receive buffer filling"). A host test
 * cannot assert the shape of an ESP-IDF read, so that half stays a
 * documented rule plus a comment at the call site -- but if someone shrinks
 * the buffer back toward one frame, they will trip this and read that rule
 * on the way past. */
static void test_rx_chunk_sizing(void)
{
    const unsigned raw_max = KILNLINK_FRAME_HEADER_LEN + 253u + 2u;
    const unsigned stuffed_max = raw_max * 2u + 2u;

    CHECK(stuffed_max == 528u, "worst-case stuffed frame is still 528 bytes");
    CHECK(UART_PROTOCOL_RX_CHUNK_BYTES >= stuffed_max,
          "RX chunk holds at least one worst-case stuffed frame");
    CHECK(UART_PROTOCOL_RX_CHUNK_BYTES == 2048u,
          "RX chunk is the 2 kB the owner asked for (2026-08-28)");
    CHECK(UART_PROTOCOL_RX_CHUNK_BYTES / stuffed_max >= 3u,
          "RX chunk carries several back-to-back frames per wakeup");
}

int main(void)
{
    test_required_cases();
    test_known_frame_wire_bytes();
    test_rx_chunk_sizing();

    if (g_failures == 0) {
        printf("ALL PASS: uart_protocol.c's deleted CRC/framing wrappers == "
               "kilnlink_crc16_ccitt_false/kilnlink_stuff, byte-identical; known frame wire bytes pinned\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
