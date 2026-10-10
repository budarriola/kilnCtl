// Pure byte<->value marshalling helpers for the safety link (ESP32-S3 <->
// RP2040 safety processor), split out of safety_link.c following dashboard_
// json.c's precedent (see that file's own header comment): these functions
// touch nothing but plain buffers/scalars -- no locking, no FreeRTOS, no
// UART/GPIO hardware -- so they can be host-tested directly instead of
// riding along inside safety_link.c's much larger, hardware-dependent
// translation unit.
//
// This was a VERBATIM relocation: every function here is byte-for-byte the
// same code that used to live as a `static` definition in safety_link.c,
// with no behavior change of any kind (same field offsets, same wire
// layout, same protocol/version comparison, same truncation-caps-rather-
// than-rejects contract in safety_parse_fw_version()). safety_link.c keeps
// calling each of these by the same name; only where they are DEFINED
// moved.
#ifndef SAFETY_LINK_FRAME_H
#define SAFETY_LINK_FRAME_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The ESP32-S3 is little-endian and so is every multi-byte field in this
 * protocol, so these are memcpy rather than byte assembly -- but they stay
 * functions so the wire layout is still stated once, in one place. */
float safety_read_f32_le(const uint8_t *bytes);
uint32_t safety_read_u32_le(const uint8_t *bytes);
uint16_t safety_read_u16_le(const uint8_t *bytes);
void safety_put_f32_le(uint8_t *out, float value);
void safety_put_u16_le(uint8_t *out, uint16_t value);
void safety_put_u32_le(uint8_t *out, uint32_t value);

/* Two-sided min_compatible check: the peer's protocol must be >= our floor,
 * and ours must be >= the peer's floor. Pure comparison, no side effects. */
bool safety_link_versions_compatible(uint16_t self_protocol, uint16_t self_min_compatible,
                                      uint16_t peer_protocol, uint16_t peer_min_compatible);

/* kilnlink audit 2026-10-09 M1: true when the Pico's DIAG uptime_ms stepped
 * BACKWARDS from prev_ms to now_ms, i.e. the Pico rebooted. The 8-bit
 * FW_VERSION boot_id alone collides 1 time in 256, so the ESP uses this as a
 * second, independent reboot signal. The one exception is the 32-bit
 * millisecond wrap (~49.7 days): prev within SAFETY_PICO_UPTIME_WRAP_BAND_MS
 * of UINT32_MAX and now within the same band of 0 reads as a wrap, not a
 * reboot. Equal values are not a regression (a duplicated frame). Pure. */
#define SAFETY_PICO_UPTIME_WRAP_BAND_MS 120000u
bool safety_pico_uptime_regressed(uint32_t prev_ms, uint32_t now_ms);

/* Parses as much of a Pico FW_VERSION (0x0B) frame as is present, per
 * LINK_PROTOCOL.md sec 4's "read bytes 1-4 first" floor rule: protocol/
 * min_compatible are read and returned whenever the frame is at least 5
 * bytes, independent of whether the variable-length commit/datetime/boot_id
 * tail parses cleanly. *out_have_boot_id is only set true if boot_id was
 * actually reachable -- a truncated or old-format frame that stops short of
 * it must not report a stale/zero boot_id as real. Returns false only if
 * bytes 1-4 themselves aren't present (frame too short to say anything).
 *
 * out_commit/out_datetime must be at least KILNLINK_ANNOUNCE_MAX_COMMIT_LEN/
 * KILNLINK_ANNOUNCE_MAX_DATETIME_LEN bytes (kilnlink_announce.h) -- a wire
 * commit_len/datetime_len greater than that cap is truncated to fit rather
 * than rejecting the whole frame (2026-08-27 stack-overflow fix: the source
 * read is bounds-checked against the frame length, but commit_len/
 * datetime_len are untrusted wire bytes independent of frame length, so the
 * destination-side cap is enforced separately). The PARSE OFFSET still
 * advances by the full (uncapped) wire length either way, so any fields
 * after a too-long string stay correctly aligned. */
bool safety_parse_fw_version(const uint8_t *p, uint8_t len, uint16_t *out_protocol,
                              uint16_t *out_min_compatible, uint8_t *out_boot_id,
                              bool *out_have_boot_id, bool *out_dirty,
                              uint8_t *out_commit, uint8_t *out_commit_len,
                              uint8_t *out_datetime, uint8_t *out_datetime_len,
                              uint8_t *out_config_version, uint16_t *out_config_crc,
                              bool *out_have_build);

#ifdef __cplusplus
}
#endif

#endif // SAFETY_LINK_FRAME_H
