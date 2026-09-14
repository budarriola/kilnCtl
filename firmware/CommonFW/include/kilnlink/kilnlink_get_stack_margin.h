#ifndef KILNLINK_GET_STACK_MARGIN_H
#define KILNLINK_GET_STACK_MARGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_GET_STACK_MARGIN = 0x2B -- docs/LINK_PROTOCOL.md
 * sec 4. One byte, no arguments -- same shape as SAFETY_CMD_GET_CT_CAL
 * (0x22, kilnlink_get_ct_cal.h), a brand-new id per that file's own
 * "Request/reply ids must never be shared" rule, next unallocated after
 * SAFETY_CMD_REBOOT_RESULT (0x2A).
 *
 * Added KILNLINK_PROTOCOL_VERSION 12 -> 13, alongside its reply
 * SAFETY_CMD_STACK_MARGIN (0x2C, kilnlink_stack_margin.h) -- see that
 * bump's own comment in kilnlink_version.h for why this pair needs one
 * (unlike SAFETY_CMD_REBOOT/_RESULT, which did not).
 *
 * Read docs/audits/saftyfw_live_stack_reporting_design_2026-09-11.md before
 * changing anything here: this frame exists to poll a CACHED snapshot a
 * single low-priority round-robin poller task fills in gradually, one task
 * per poll cycle, specifically to avoid repeating the 2026-08-23
 * watchdog-timing regression (a per-checkin uxTaskGetStackHighWaterMark()
 * call on every task dropped the watchdog reset cadence from ~9s to
 * ~1.1s). The ESP is expected to poll this at a SLOW cadence (this data
 * changes on the order of minutes/boots, not the ~1 Hz DIAG frame) -- there
 * is no requirement or benefit to polling it any faster.
 *
 * The Pico answers every copy it sees and never tracks whether its answer
 * arrived, same as GET_CT_CAL/GET_FW_VERSION.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_GET_STACK_MARGIN_CMD 0x2Bu
#define KILNLINK_GET_STACK_MARGIN_LEN 1u /* cmd(1), no fields */

typedef enum {
    KILNLINK_GET_STACK_MARGIN_OK = 0,
    KILNLINK_GET_STACK_MARGIN_ERR_BUFFER_TOO_SMALL,
    KILNLINK_GET_STACK_MARGIN_ERR_LENGTH_MISMATCH,
    KILNLINK_GET_STACK_MARGIN_ERR_WRONG_CMD,
} kilnlink_get_stack_margin_status_t;

typedef struct {
    uint8_t reserved; /* unused; always 0, not part of the wire payload */
} kilnlink_get_stack_margin_t;

/* Serializes `msg` (SAFETY_CMD_GET_STACK_MARGIN payload, byte 0 = 0x2B) into
 * `out`. Always exactly KILNLINK_GET_STACK_MARGIN_LEN (1) byte. `msg` may be
 * NULL. Returns 1, or 0 on KILNLINK_GET_STACK_MARGIN_ERR_BUFFER_TOO_SMALL. */
size_t kilnlink_get_stack_margin_encode(const kilnlink_get_stack_margin_t *msg, uint8_t *out,
                                         size_t out_cap,
                                         kilnlink_get_stack_margin_status_t *status);

/* Parses a GET_STACK_MARGIN payload (as extracted from
 * kilnlink_frame_t::payload) into `out`. `len` must be exactly
 * KILNLINK_GET_STACK_MARGIN_LEN -- this is untrusted input from another
 * processor across an isolated link (CommonFW/README.md rule 6). */
kilnlink_get_stack_margin_status_t kilnlink_get_stack_margin_decode(
    const uint8_t *payload, size_t len, kilnlink_get_stack_margin_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_GET_STACK_MARGIN_H */
