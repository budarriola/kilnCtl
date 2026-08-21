#ifndef BENCHPROTO_FRAME_H
#define BENCHPROTO_FRAME_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* benchproto -- the wire envelope for a hardened, addressed, request/reply
 * protocol for bench/instrument-fixture firmwares talking to a PC over a
 * serial-shaped link (UART, USB CDC, ...). First consumer: SimFW's USB CDC
 * link to its PC-side `kilnsim` tools (firmware/SimFW/docs/DESIGN_NOTES.md sec
 * 4.4, 5, 5.1, 12). Lifted and generalized from
 * firmware/UnitTestFw/UnitTest/docs/UART_PROTOCOL.md, which described this
 * same shape for a UART link before UnitTestFw was retired -- see
 * firmware/CommonFW/docs/BENCHPROTO.md for the full spec this header
 * implements.
 *
 * benchproto is a *separate* protocol family from `kilnlink`
 * (firmware/CommonFW/include/kilnlink/, the ESP32<->RP2040 safety link).
 * The two happen to share a family resemblance -- SLIP-style byte-stuffed
 * framing, CRC16/CCITT-FALSE, a small fixed header -- because that is a
 * reasonable, well-proven shape for this kind of link and reusing a proven
 * shape is good engineering, not because one is a repackaging of the
 * other's code. Nothing in this file calls into kilnlink_frame.c/.h, and
 * nothing in kilnlink may call into this file: two independent
 * implementations, two independent constant sets, two independent test
 * suites, on purpose. Do not "deduplicate" them into one shared
 * frame-codec -- see BENCHPROTO.md sec 1 for why they are kept apart.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6 (written for kilnlink)
 * apply equally to this library. Every function here is a pure transform
 * on caller-supplied buffers. */

/* One byte holds the wire LENGTH field, so 255 is the hard ceiling; 128 is
 * a deliberately generous, round default for the request/reply payloads
 * SimFW's command groups need (see DESIGN_NOTES.md sec 5.2's representative
 * payloads -- the largest sketched there, FAULT_SCHEDULE, is well under
 * half of this). Raise it (up to 255) the same way KilnFW raised its own
 * copy of this constant from 128 to 253 if a future payload genuinely
 * needs the headroom -- see that commit's reasoning in
 * firmware/KilnFW/App/drivers/espInterfaces/uart_protocol.h for the shape
 * of that decision; it does not apply here today; this file only forwards
 * the *idea* to future readers, not the number. */
#define BENCHPROTO_FRAME_MAX_PAYLOAD 128u

/* type(1) + msg_index(2, BE) + src_device(1) + src_task(1) + dst_device(1)
 * + dst_task(1) + length(1). */
#define BENCHPROTO_FRAME_HEADER_LEN 8u
#define BENCHPROTO_FRAME_CRC_LEN 2u

/* header + max payload + crc, before stuffing. */
#define BENCHPROTO_FRAME_RAW_MAX \
    (BENCHPROTO_FRAME_HEADER_LEN + BENCHPROTO_FRAME_MAX_PAYLOAD + BENCHPROTO_FRAME_CRC_LEN)

/* Stuffing can at most double the bytes, plus two delimiters. */
#define BENCHPROTO_FRAME_STUFFED_MAX (BENCHPROTO_FRAME_RAW_MAX * 2u + 2u)

#define BENCHPROTO_FRAME_DELIM 0x7Eu
#define BENCHPROTO_FRAME_ESC 0x7Du
#define BENCHPROTO_FRAME_ESC_XOR 0x20u

/* DATA/ACK/NACK are the stop-and-wait request/reply exchange
 * (benchproto_link.h drives the state machine around them). BROADCAST is
 * fire-and-forget: no ACK is ever sent or expected for it, and the
 * receiver never dedups it -- the shape SimFW's unsolicited TELEMETRY/EVT
 * frames want (DESIGN_NOTES.md sec 5.3), where the sender must never block on a
 * reply. */
typedef enum {
    BENCHPROTO_MSG_DATA = 0x01,
    BENCHPROTO_MSG_ACK = 0x02,
    BENCHPROTO_MSG_NACK = 0x03, /* destination task not registered ("undeliverable") */
    BENCHPROTO_MSG_BROADCAST = 0x04,
} benchproto_msg_type_t;

typedef enum {
    BENCHPROTO_FRAME_OK = 0,
    BENCHPROTO_FRAME_ERR_TOO_SHORT,        /* fewer than HEADER_LEN + CRC_LEN bytes */
    BENCHPROTO_FRAME_ERR_LENGTH_TOO_LONG,  /* header's LENGTH byte exceeds MAX_PAYLOAD */
    BENCHPROTO_FRAME_ERR_LENGTH_MISMATCH,  /* header's LENGTH byte doesn't match buffer size */
    BENCHPROTO_FRAME_ERR_CRC,              /* CRC16 mismatch -- corrupt or truncated frame */
    BENCHPROTO_FRAME_ERR_UNKNOWN_TYPE,     /* type byte isn't one of benchproto_msg_type_t */
    BENCHPROTO_FRAME_ERR_BUFFER_TOO_SMALL, /* caller's output buffer can't hold the result */
    BENCHPROTO_FRAME_ERR_UNTERMINATED_ESC, /* an 0x7D escape byte with nothing following it */
} benchproto_frame_status_t;

/* A decoded (or about-to-be-encoded) frame. On decode, `payload` points
 * into the caller's own `raw` buffer -- no copy, no allocation -- so it is
 * only valid as long as that buffer is. */
typedef struct {
    benchproto_msg_type_t msg_type;
    uint16_t msg_index;
    uint8_t src_device;
    uint8_t src_task;
    uint8_t dst_device;
    uint8_t dst_task;
    uint8_t length;
    const uint8_t *payload;
} benchproto_frame_t;

/* CRC16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection, no final
 * XOR) -- the same well-known variant kilnlink uses, chosen independently
 * here for the same reason: cheap in software, no lookup table required,
 * and a standard, checkable test vector
 * (CRC16_CCITT_FALSE("123456789") == 0x29B1) to catch a transcription
 * error in either implementation. This is benchproto's own
 * implementation; it does not call kilnlink_crc16_ccitt_false(). */
uint16_t benchproto_crc16_ccitt_false(const uint8_t *data, size_t len);

/* Byte-stuff `raw` (SLIP-style: 0x7E and 0x7D become a 0x7D-escaped pair)
 * and wrap it in leading/trailing 0x7E delimiters. `out_cap` must be at
 * least BENCHPROTO_FRAME_STUFFED_MAX to guarantee success for any legal
 * raw frame; returns the number of bytes written to `out`, or 0 if
 * `out_cap` was too small (nothing is written in that case). */
size_t benchproto_stuff(const uint8_t *raw, size_t raw_len, uint8_t *out, size_t out_cap);

/* Reverse of benchproto_stuff for a single frame body. Accepts `in` with
 * or without its surrounding 0x7E delimiters (a live receiver strips them
 * during resync; a one-shot caller decoding a captured/hex-pasted frame
 * usually still has them). Returns the number of bytes written to `out`,
 * or 0 on an unterminated trailing escape or an undersized `out_cap` --
 * check `*status` to tell those apart. `out_cap` must be at least `in_len`
 * (the unstuffed form is never longer than the stuffed one). */
size_t benchproto_unstuff(const uint8_t *in, size_t in_len, uint8_t *out, size_t out_cap,
                          benchproto_frame_status_t *status);

/* Serializes `frame` (header + payload + CRC16, NOT byte-stuffed) into
 * `out_raw`. `out_cap` must be at least
 * BENCHPROTO_FRAME_HEADER_LEN + frame->length + BENCHPROTO_FRAME_CRC_LEN.
 * Returns the number of bytes written, or 0 on
 * BENCHPROTO_FRAME_ERR_LENGTH_TOO_LONG / ERR_BUFFER_TOO_SMALL (check
 * `*status`). Pass the result to benchproto_stuff() to get wire-ready
 * bytes. */
size_t benchproto_frame_encode_raw(const benchproto_frame_t *frame, uint8_t *out_raw, size_t out_cap,
                                   benchproto_frame_status_t *status);

/* Parses an unstuffed raw frame (the output of benchproto_unstuff, or a
 * frame assembled directly by a caller that already stripped
 * delimiters/escapes) into `out`. Validates length and CRC before trusting
 * anything in the header -- a payload is untrusted input arriving over a
 * link to a PC (CommonFW/README.md rule 6, same reasoning). `out->payload`
 * points into `raw`; the caller must keep `raw` alive as long as `out` is
 * used. */
benchproto_frame_status_t benchproto_frame_decode(const uint8_t *raw, size_t raw_len,
                                                    benchproto_frame_t *out);

#ifdef __cplusplus
}
#endif

#endif /* BENCHPROTO_FRAME_H */
