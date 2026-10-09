#ifndef KILNLINK_FRAME_H
#define KILNLINK_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The wire envelope shared by the PC<->ESP link (App/drivers/uart_protocol.c)
 * and the isolated ESP<->Pico link: 0x7E-delimited, SLIP-style byte-stuffed,
 * an 8-byte header, an optional payload, and a trailing CRC16/CCITT-FALSE.
 * This file is the *only* place that framing/stuffing/CRC exists in the
 * whole system -- see CommonFW/README.md rule 7 ("no CRC or byte-stuffing
 * implementation outside CommonFW") once uart_protocol.c is migrated to call
 * this instead of its own copy.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. Every function here is a
 * pure transform on caller-supplied buffers. */

/* Capped at 253, not a round number: the wire header's LENGTH field is one
 * byte, so 255 is the hard ceiling; 253 keeps DISPLAY_BLIT_CHUNK_PIXELS
 * (pc_tools) an exact pixel count. Mirrors UART_PROTO_MAX_PAYLOAD in
 * App/drivers/owners/uart_protocol.h -- the two must never diverge, a
 * peer built against a different cap will misparse the LENGTH byte. */
#define KILNLINK_FRAME_MAX_PAYLOAD 253u

/* type(1) + msg_index(2) + src_device(1) + src_task(1) + dst_device(1) +
 * dst_task(1) + length(1). */
#define KILNLINK_FRAME_HEADER_LEN 8u
#define KILNLINK_FRAME_CRC_LEN 2u

/* header + max payload + crc, before stuffing. */
#define KILNLINK_FRAME_RAW_MAX \
    (KILNLINK_FRAME_HEADER_LEN + KILNLINK_FRAME_MAX_PAYLOAD + KILNLINK_FRAME_CRC_LEN)

/* Stuffing can at most double the bytes, plus two delimiters. */
#define KILNLINK_FRAME_STUFFED_MAX (KILNLINK_FRAME_RAW_MAX * 2u + 2u)

#define KILNLINK_FRAME_DELIM 0x7Eu
#define KILNLINK_FRAME_ESC 0x7Du
#define KILNLINK_FRAME_ESC_XOR 0x20u

/* uart_proto_msg_type_t (App/drivers/owners/uart_protocol.h), moved
 * here since the envelope and the type byte it carries are the same thing on
 * both links. BROADCAST is the isolated link's only type in practice (see
 * docs/LINK_PROTOCOL.md sec 1) -- DATA/ACK/NACK exist for the PC link's
 * stop-and-wait transport, which the Pico never participates in. */
typedef enum {
    KILNLINK_MSG_DATA = 0x01,
    KILNLINK_MSG_ACK = 0x02,
    KILNLINK_MSG_NACK = 0x03,
    KILNLINK_MSG_BROADCAST = 0x04,
} kilnlink_msg_type_t;

typedef enum {
    KILNLINK_FRAME_OK = 0,
    KILNLINK_FRAME_ERR_TOO_SHORT,        /* fewer than HEADER_LEN + CRC_LEN bytes */
    KILNLINK_FRAME_ERR_LENGTH_TOO_LONG,  /* header's LENGTH byte exceeds MAX_PAYLOAD */
    KILNLINK_FRAME_ERR_LENGTH_MISMATCH,  /* header's LENGTH byte doesn't match buffer size */
    KILNLINK_FRAME_ERR_CRC,              /* CRC16 mismatch -- corrupt or truncated frame */
    KILNLINK_FRAME_ERR_UNKNOWN_TYPE,     /* type byte isn't one of kilnlink_msg_type_t */
    KILNLINK_FRAME_ERR_BUFFER_TOO_SMALL, /* caller's output buffer can't hold the result */
    KILNLINK_FRAME_ERR_UNTERMINATED_ESC, /* an 0x7D escape byte with nothing following it */
} kilnlink_frame_status_t;

/* A decoded (or about-to-be-encoded) frame. On decode, `payload` points into
 * the caller's own `raw` buffer -- no copy, no allocation -- so it is only
 * valid as long as that buffer is. */
typedef struct {
    kilnlink_msg_type_t msg_type;
    uint16_t msg_index;
    uint8_t src_device;
    uint8_t src_task;
    uint8_t dst_device;
    uint8_t dst_task;
    uint8_t length;
    const uint8_t *payload;
} kilnlink_frame_t;

/* CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final XOR) --
 * the same algorithm App/drivers/owners/uart_protocol.c and
 * pc_tools/src/kilnctrl/protocol.py already implement independently. */
uint16_t kilnlink_crc16_ccitt_false(const uint8_t *data, size_t len);

/* Byte-stuff `raw` (SLIP-style: 0x7E and 0x7D become a 0x7D-escaped pair)
 * and wrap it in leading/trailing 0x7E delimiters. `out_cap` must be at
 * least KILNLINK_FRAME_STUFFED_MAX to guarantee success for any legal raw
 * frame; returns the number of bytes written to `out`, or 0 if `out_cap` was
 * too small (nothing is written in that case). */
size_t kilnlink_stuff(const uint8_t *raw, size_t raw_len, uint8_t *out, size_t out_cap);

/* Reverse of kilnlink_stuff for a single frame body. Accepts `in` with or
 * without its surrounding 0x7E delimiters (a live receiver strips them
 * during resync; a one-shot caller decoding a captured/hex-pasted frame
 * usually still has them). Returns the number of bytes written to `out`, or
 * 0 on an unterminated trailing escape or an undersized `out_cap` -- check
 * `*status` to tell those apart. `out_cap` must be at least `in_len` (the
 * unstuffed form is never longer than the stuffed one). */
size_t kilnlink_unstuff(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                        kilnlink_frame_status_t *status);

/* Serializes `frame` (header + payload + CRC16, NOT byte-stuffed) into
 * `out_raw`. `out_cap` must be at least
 * KILNLINK_FRAME_HEADER_LEN + frame->length + KILNLINK_FRAME_CRC_LEN.
 * Returns the number of bytes written, or 0 on
 * KILNLINK_FRAME_ERR_LENGTH_TOO_LONG / ERR_BUFFER_TOO_SMALL (check
 * `*status`). Pass the result to kilnlink_stuff() to get wire-ready bytes. */
size_t kilnlink_frame_encode_raw(const kilnlink_frame_t *frame, uint8_t *out_raw, size_t out_cap,
                                 kilnlink_frame_status_t *status);

/* Parses an unstuffed raw frame (the output of kilnlink_unstuff, or a frame
 * assembled directly by a caller that already stripped delimiters/escapes)
 * into `out`. Validates length and CRC before trusting anything in the
 * header -- a payload is untrusted input from another processor across an
 * isolated link (CommonFW/README.md rule 6). `out->payload` points into
 * `raw`; the caller must keep `raw` alive as long as `out` is used. */
kilnlink_frame_status_t kilnlink_frame_decode(const uint8_t *raw, size_t raw_len,
                                              kilnlink_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_FRAME_H */
