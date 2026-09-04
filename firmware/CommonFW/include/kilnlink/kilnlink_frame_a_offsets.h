#ifndef KILNLINK_FRAME_A_OFFSETS_H
#define KILNLINK_FRAME_A_OFFSETS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_GET_STATUS = 0x01 -- CommonFW/docs/LINK_PROTOCOL.md
 * section 4/6, "Frame A". Single source of truth for this frame's byte
 * offsets, lifted out of three independent hand-written copies per
 * ROADMAP.md M15 "Frame A's field layout is hand-duplicated across
 * firmwares":
 *
 *   1. SaftyFW/src/tasks/link_frame.c's link_frame_pack_status()   (pack)
 *   2. KilnFW/App/drivers/safety_link_frames.c's safety_apply_status()
 *      (parse)
 *   3. SaftyFW/test/test_link_frame_wire.c's mirror_apply_status() (the
 *      drift test for #1/#2, itself formerly a third hand transcription)
 *
 * All three previously carried their own literal `out[N]`/`p[N]` offset
 * table; an offset mismatch between any two of them passes the frame's CRC
 * (the bytes are well-formed, just misinterpreted) and silently misdecodes
 * temperatures/currents/flags. Every site above now indexes through the
 * macros below instead of a literal, so drift is a compile-time impossible
 * state rather than something only a periodic diff check can catch after
 * the fact (App/test/frame_a_offset_drift_check.py still runs, but now as a
 * regression guard against a NEW literal offset table reappearing, not as
 * the primary defense).
 *
 * Header-only, freestanding C11, no allocation, no I/O, no globals --
 * CommonFW/README.md rules 1-6 -- exactly like every other file under this
 * directory. No .c file: these are compile-time constants only, so there is
 * nothing to link, and every consumer already has this include directory on
 * its search path (KilnFW and SaftyFW both already build against `kilnlink`,
 * kilnlink_rollback_result.h being the precedent this header follows).
 *
 * Values are frozen wire contract, not implementation detail -- do not
 * renumber an existing offset to "tidy up" the layout; only ever append a
 * new one, the same discipline LINK_FRAME_STATUS_LEN_V1/_V2/_V3
 * (link_frame.h) already documents for this same frame's overall length. */

#define KILNLINK_FRAME_A_CMD 0x01u

/* V1 (original, bytes 0..22), V2 (V1 + byte 23, tx_dropped_sat), and V3
 * (V2 + bytes 24/25, flags2 + borrowed_zone_index) lengths. See link_frame.h
 * (SaftyFW) / safety_link.h (KilnFW) for the full skew-safety history of
 * each extension -- restated here only as the length constants themselves,
 * which is the part that must be byte-identical across both sides. */
#define KILNLINK_FRAME_A_LEN_V1 23u
#define KILNLINK_FRAME_A_LEN_V2 24u
#define KILNLINK_FRAME_A_LEN_V3 26u

/* Byte offsets into the frame's payload (offset 0 is the command byte
 * itself, KILNLINK_FRAME_A_CMD). */
#define KILNLINK_FRAME_A_OFF_FLAGS               1u  /* 1 byte  -- link/estop/relay/enabled/temp_valid/tc_* bits */
#define KILNLINK_FRAME_A_OFF_TC_TEMP_C            2u  /* 4 bytes -- float32 LE, safety thermocouple reading */
#define KILNLINK_FRAME_A_OFF_CJ_TEMP_C            6u  /* 4 bytes -- float32 LE, cold-junction reading */
#define KILNLINK_FRAME_A_OFF_TC_FAULT            10u  /* 1 byte  -- MAX31856 fault bits, opaque on the wire */
#define KILNLINK_FRAME_A_OFF_AMPS1               11u  /* 4 bytes -- float32 LE, CT channel 1 */
#define KILNLINK_FRAME_A_OFF_AMPS2               15u  /* 4 bytes -- float32 LE, CT channel 2 */
#define KILNLINK_FRAME_A_OFF_AMPS3               19u  /* 4 bytes -- float32 LE, CT channel 3 */
#define KILNLINK_FRAME_A_OFF_TX_DROPPED_SAT      23u  /* 1 byte  -- V2 only */
#define KILNLINK_FRAME_A_OFF_FLAGS2              24u  /* 1 byte  -- V3 only */
#define KILNLINK_FRAME_A_OFF_BORROWED_ZONE_INDEX 25u  /* 1 byte  -- V3 only */

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_FRAME_A_OFFSETS_H */
