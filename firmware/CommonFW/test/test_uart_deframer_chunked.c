/* Chunked-read deframer test for the ONE piece of framing code that is NOT
 * the shared kilnlink implementation: the hand-rolled byte-stuffing state
 * machine inside uart_protocol_rx_task() in
 * firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.c (~lines 275-330),
 * plus the length/CRC gate in handle_raw_frame() just above it.
 *
 * That state machine reads the UART in 32-byte chunks (uart_read_bytes into
 * a `uint8_t chunk[32]`) and must carry `in_frame`, `escaped`, and `raw_len`
 * across chunk boundaries correctly -- in particular when a 0x7D escape byte
 * lands as the LAST byte of one 32-byte chunk and its escapee is the FIRST
 * byte of the next. This is exactly the scenario a prior by-inspection
 * review flagged as worth testing but did not actually test (see the
 * "frame length mismatch (hdr says 176, got 142 bytes)" field symptom on a
 * large frame, retried byte-identically every time).
 *
 * This file embeds a verbatim COPY of the streaming state machine's body
 * (the two ESP-IDF-only pieces -- uart_read_bytes() and the FreeRTOS task
 * wrapper -- are replaced by feeding pre-chunked byte arrays in, everything
 * else -- the FRAME_DELIM/escaped/raw_len logic -- is byte-for-byte the same
 * as uart_protocol.c's loop body), plus a copy of handle_raw_frame()'s
 * length + CRC gate (using the real kilnlink_crc16_ccitt_false so the CRC
 * check itself is the real one, not a re-implementation).
 *
 * Build:
 *   cl /nologo /W4 /I ..\include test_uart_deframer_chunked.c ..\src\kilnlink_frame.c ..\src\kilnlink_crc.c
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

/* ---- verbatim copy of uart_protocol.c's constants/sizes ---- */
#define FRAME_DELIM   KILNLINK_FRAME_DELIM
#define FRAME_ESC     KILNLINK_FRAME_ESC
#define FRAME_ESC_XOR KILNLINK_FRAME_ESC_XOR
#define HEADER_LEN    KILNLINK_FRAME_HEADER_LEN
#define UART_PROTO_MAX_PAYLOAD 253u
#define RAW_FRAME_MAX (HEADER_LEN + UART_PROTO_MAX_PAYLOAD + 2u)
#define CHUNK_SIZE 32

/* ---- verbatim copy of the deframer state carried across
 * uart_protocol_rx_task()'s reads, and its per-byte loop body ---- */
typedef struct {
    uint8_t raw[RAW_FRAME_MAX];
    size_t raw_len;
    int in_frame;
    int escaped;

    /* test harness: records of every raw frame handed to handle_raw_frame */
    uint8_t last_raw[RAW_FRAME_MAX];
    size_t last_raw_len;
    int frames_seen;
} deframer_t;

static void deframer_init(deframer_t *d)
{
    memset(d, 0, sizeof(*d));
}

/* Stand-in for handle_raw_frame()'s "was this even worth looking at" step:
 * the test doesn't need dedup/inbox/ACK machinery, just a record that a raw
 * frame reached that call, verbatim byte-for-byte. */
static void handle_raw_frame_stub(deframer_t *d, const uint8_t *raw, size_t len)
{
    d->frames_seen++;
    if (len <= sizeof(d->last_raw)) {
        memcpy(d->last_raw, raw, len);
        d->last_raw_len = len;
    }
}

/* Verbatim copy of the for-loop body inside uart_protocol_rx_task(), taking
 * one already-read chunk at a time -- exactly what uart_read_bytes() handed
 * the original loop, split across as many calls as there are chunks. */
static void deframer_feed_chunk(deframer_t *d, const uint8_t *chunk, int n)
{
    for (int i = 0; i < n; ++i) {
        uint8_t byte = chunk[i];

        if (byte == FRAME_DELIM) {
            if (d->in_frame && d->raw_len > 0) {
                handle_raw_frame_stub(d, d->raw, d->raw_len);
            }
            d->raw_len = 0;
            d->in_frame = 1;
            d->escaped = 0;
            continue;
        }

        if (!d->in_frame) {
            continue; /* discard noise before the first delimiter */
        }

        if (d->escaped) {
            byte = (uint8_t)(byte ^ FRAME_ESC_XOR);
            d->escaped = 0;
        } else if (byte == FRAME_ESC) {
            d->escaped = 1;
            continue;
        }

        if (d->raw_len < sizeof(d->raw)) {
            d->raw[d->raw_len++] = byte;
        } else {
            /* Oversized/corrupt frame: resync on next delimiter. */
            d->in_frame = 0;
        }
    }
}

/* Feeds an entire stuffed wire buffer through the deframer split into
 * CHUNK_SIZE-byte pieces, exactly like uart_read_bytes(chunk, 32, ...)
 * would produce them off a real UART. */
static void deframer_feed_wire(deframer_t *d, const uint8_t *wire, size_t wire_len)
{
    size_t off = 0;
    while (off < wire_len) {
        int n = (int)((wire_len - off) > CHUNK_SIZE ? CHUNK_SIZE : (wire_len - off));
        deframer_feed_chunk(d, wire + off, n);
        off += (size_t)n;
    }
}

/* ---- verbatim copy of handle_raw_frame()'s length+CRC gate ---- */
static int validate_raw_frame(const uint8_t *raw, size_t len)
{
    if (len < HEADER_LEN + 2) {
        return 0;
    }
    uint8_t length = raw[7];
    if (len != HEADER_LEN + length + 2u) {
        printf("  (validate) length mismatch: hdr says %u, got %u bytes\n", length, (unsigned)len);
        return 0;
    }
    uint16_t expected_crc = kilnlink_crc16_ccitt_false(raw, HEADER_LEN + length);
    uint16_t actual_crc = (uint16_t)((raw[HEADER_LEN + length] << 8) | raw[HEADER_LEN + length + 1]);
    if (expected_crc != actual_crc) {
        printf("  (validate) CRC mismatch\n");
        return 0;
    }
    return 1;
}

/* ---- test frame construction: a 186-byte raw frame (8 header + 176
 * payload + 2 CRC), matching the field symptom exactly, whose STUFFED wire
 * form is searched (by trying successive payload perturbations) until an
 * escape pair straddles a 32-byte chunk boundary -- i.e. the 0x7D ESC byte
 * is the last byte of one chunk and its escapee is the first byte of the
 * next. ---- */

#define PAYLOAD_LEN 176u
#define RAW_LEN (HEADER_LEN + PAYLOAD_LEN + 2u)

static void build_raw_frame(uint8_t *raw, uint8_t escape_marker_offset, uint8_t escape_byte)
{
    raw[0] = 0x01; /* DATA */
    raw[1] = 0x00; /* msg_index hi */
    raw[2] = 0x2A; /* msg_index lo */
    raw[3] = 0x03; /* src_device */
    raw[4] = 0x05; /* src_task */
    raw[5] = 0x04; /* dst_device */
    raw[6] = 0x07; /* dst_task */
    raw[7] = (uint8_t)PAYLOAD_LEN;

    for (uint8_t i = 0; i < PAYLOAD_LEN; ++i) {
        raw[HEADER_LEN + i] = (uint8_t)(i * 3u + 7u); /* ordinary, non-escaping filler */
    }
    /* Plant exactly one escapable byte (0x7E or 0x7D) at a chosen payload
     * offset -- this is what we'll slide around to land its stuffed escape
     * pair on a chunk boundary. */
    raw[HEADER_LEN + escape_marker_offset] = escape_byte;

    uint16_t crc = kilnlink_crc16_ccitt_false(raw, HEADER_LEN + PAYLOAD_LEN);
    raw[HEADER_LEN + PAYLOAD_LEN] = (uint8_t)(crc >> 8);
    raw[HEADER_LEN + PAYLOAD_LEN + 1] = (uint8_t)(crc & 0xFF);
}

/* Returns the wire offset of the FRAME_ESC byte if some escape pair in
 * `wire` has its ESC as the last byte of a 32-byte chunk (i.e.
 * (esc_offset + 1) % 32 == 0), else -1. */
static int find_boundary_escape(const uint8_t *wire, size_t wire_len)
{
    for (size_t i = 0; i + 1 < wire_len; ++i) {
        if (wire[i] == FRAME_ESC && ((i + 1) % CHUNK_SIZE) == 0) {
            return (int)i;
        }
    }
    return -1;
}

static void test_chunk_boundary_escape(void)
{
    uint8_t raw[RAW_LEN];
    uint8_t wire[RAW_LEN * 2 + 2];
    int boundary_esc_offset = -1;
    uint8_t found_marker_offset = 0;
    uint8_t found_escape_byte = 0;

    /* Search payload offsets and both escapable byte values for a
     * combination whose stuffed ESC byte lands exactly on a chunk boundary.
     * The header is fixed and payload filler bytes never need escaping, so
     * moving a single planted 0x7E/0x7D through the payload sweeps the
     * escape pair through every possible wire offset. */
    for (uint8_t off = 0; off < PAYLOAD_LEN && boundary_esc_offset < 0; ++off) {
        for (int which = 0; which < 2; ++which) {
            uint8_t marker = (which == 0) ? FRAME_DELIM : FRAME_ESC;
            build_raw_frame(raw, off, marker);
            size_t wlen = kilnlink_stuff(raw, RAW_LEN, wire, sizeof(wire));
            CHECK(wlen > 0, "stuff failed while searching for boundary case");
            int esc_off = find_boundary_escape(wire, wlen);
            if (esc_off >= 0) {
                boundary_esc_offset = esc_off;
                found_marker_offset = off;
                found_escape_byte = marker;
                break;
            }
        }
    }
    CHECK(boundary_esc_offset >= 0, "could not construct an escape-at-chunk-boundary case");
    if (boundary_esc_offset < 0) {
        return;
    }
    printf("using payload offset %u (marker 0x%02x) -> ESC lands at wire offset %d "
           "(last byte of chunk %d)\n",
           found_marker_offset, found_escape_byte, boundary_esc_offset,
           boundary_esc_offset / CHUNK_SIZE);

    /* Rebuild the winning frame and its stuffed wire form for the real test. */
    build_raw_frame(raw, found_marker_offset, found_escape_byte);
    size_t wire_len = kilnlink_stuff(raw, RAW_LEN, wire, sizeof(wire));
    CHECK(wire_len > 0, "stuff failed on chosen frame");

    /* --- Positive case: feed the good frame through the deframer chunked
     * exactly as uart_read_bytes(chunk, 32, ...) would, and confirm the
     * reassembled raw frame is byte-identical and passes validation. --- */
    deframer_t d;
    deframer_init(&d);
    deframer_feed_wire(&d, wire, wire_len);

    CHECK(d.frames_seen == 1, "expected exactly one frame delivered to handle_raw_frame");
    CHECK(d.last_raw_len == RAW_LEN, "reassembled frame length mismatch");
    if (d.last_raw_len == RAW_LEN) {
        CHECK(memcmp(d.last_raw, raw, RAW_LEN) == 0, "reassembled frame bytes mismatch");
    }
    CHECK(validate_raw_frame(d.last_raw, d.last_raw_len) == 1,
          "reassembled frame failed length/CRC validation");

    /* --- Negative case: prove the test can actually fail. Corrupt the wire
     * form by dropping one raw payload byte AFTER stuffing (simulating a
     * byte genuinely lost in transit -- e.g. a UART overrun) partway through
     * the frame, well clear of the escape pair under test, and confirm the
     * deframer either reassembles the wrong length or the CRC check catches
     * it. This is the "can the test fail" proof: without this corruption the
     * suite would pass even if deframer_feed_chunk had a bug that happened
     * not to trigger on the good frame. --- */
    uint8_t corrupt_wire[sizeof(wire)];
    memcpy(corrupt_wire, wire, wire_len);
    size_t drop_at = wire_len / 2; /* interior byte, not a delimiter/escape */
    while (corrupt_wire[drop_at] == FRAME_DELIM || corrupt_wire[drop_at] == FRAME_ESC) {
        drop_at++;
    }
    memmove(&corrupt_wire[drop_at], &corrupt_wire[drop_at + 1], wire_len - drop_at - 1);
    size_t corrupt_len = wire_len - 1;

    deframer_t dc;
    deframer_init(&dc);
    deframer_feed_wire(&dc, corrupt_wire, corrupt_len);

    int rejected = 0;
    if (dc.frames_seen != 1) {
        rejected = 1; /* no frame, or resynced into more than one -- caught */
    } else if (dc.last_raw_len != RAW_LEN || memcmp(dc.last_raw, raw, RAW_LEN) != 0) {
        rejected = 1; /* reassembled but wrong -- must fail validation */
        CHECK(validate_raw_frame(dc.last_raw, dc.last_raw_len) == 0,
              "corrupted frame (one byte dropped) was NOT rejected by length/CRC validation "
              "-- test failed to catch a real corruption, as intended by this negative case");
    }
    CHECK(rejected == 1, "dropping one byte from the wire form had no observable effect "
                          "-- corruption was not actually exercised");
    if (rejected && dc.frames_seen == 1) {
        printf("negative case confirmed: one dropped wire byte -> reassembled length %u "
               "(expected %u), validation correctly rejects it\n",
               (unsigned)dc.last_raw_len, (unsigned)RAW_LEN);
    } else if (rejected) {
        printf("negative case confirmed: one dropped wire byte -> %d frame(s) seen "
               "(expected 1)\n", dc.frames_seen);
    }
}

/* Sanity check independent of the boundary search above: an ordinary small
 * frame (no chunk-boundary tricks) must still round-trip, so a bug that only
 * shows up on the large/escape-heavy case doesn't hide behind a broken small
 * case being silently skipped. */
static void test_small_frame_baseline(void)
{
    uint8_t raw[HEADER_LEN + 3 + 2];
    raw[0] = 0x01;
    raw[1] = 0x00;
    raw[2] = 0x01;
    raw[3] = 0x03;
    raw[4] = 0x05;
    raw[5] = 0x04;
    raw[6] = 0x07;
    raw[7] = 3;
    raw[8] = FRAME_DELIM; /* deliberately escapable content */
    raw[9] = FRAME_ESC;
    raw[10] = 0x42;
    uint16_t crc = kilnlink_crc16_ccitt_false(raw, HEADER_LEN + 3);
    raw[11] = (uint8_t)(crc >> 8);
    raw[12] = (uint8_t)(crc & 0xFF);

    uint8_t wire[64];
    size_t wlen = kilnlink_stuff(raw, sizeof(raw), wire, sizeof(wire));
    CHECK(wlen > 0, "small frame stuff failed");

    deframer_t d;
    deframer_init(&d);
    deframer_feed_wire(&d, wire, wlen);

    CHECK(d.frames_seen == 1, "small frame: expected exactly one frame");
    CHECK(d.last_raw_len == sizeof(raw), "small frame: length mismatch");
    if (d.last_raw_len == sizeof(raw)) {
        CHECK(memcmp(d.last_raw, raw, sizeof(raw)) == 0, "small frame: bytes mismatch");
    }
    CHECK(validate_raw_frame(d.last_raw, d.last_raw_len) == 1, "small frame: failed validation");
}

int main(void)
{
    test_small_frame_baseline();
    test_chunk_boundary_escape();

    if (g_failures == 0) {
        printf("ALL PASS: chunked deframer reassembles a 186-byte frame across 32-byte reads "
               "with an escape pair straddling a chunk boundary, byte-identical; "
               "confirmed the test rejects a corrupted (one wire byte dropped) variant\n");
        return 0;
    }
    printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
