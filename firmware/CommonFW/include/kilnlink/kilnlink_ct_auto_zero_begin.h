#ifndef KILNLINK_CT_AUTO_ZERO_BEGIN_H
#define KILNLINK_CT_AUTO_ZERO_BEGIN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ESP -> Pico, SAFETY_CMD_CT_AUTO_ZERO_BEGIN = 0x26 -- docs/LINK_PROTOCOL.md
 * sec 4. CT_COMMISSIONING_PLAN.md step 2: the wire path to current_sense.c's
 * current_sense_recalibrate_zero() mechanism, which existed with nothing
 * calling it. One channel per frame, same "one channel, never all three"
 * convention SET_CT_CAL uses.
 *
 * Fire-and-forget, like SET_CT_CAL/SET_CONFIG: never ACKed on the wire. The
 * Pico does NOT block link_task on this -- current_task.c accumulates the
 * measurement incrementally, one sample per its own normal period, so this
 * request only ARMS that accumulation; the ESP polls progress/result via
 * SAFETY_CMD_GET_CT_AUTO_ZERO (kilnlink_get_ct_auto_zero.h) and its reply
 * SAFETY_CMD_CT_AUTO_ZERO_STATUS (kilnlink_ct_auto_zero_status.h). A
 * synchronous multi-second measurement inside link_task's own dispatch
 * would starve its 30 ms watchdog check-in deadline (watchdog_task.c) and
 * reset the board -- this frame exists specifically so that never happens.
 * Refused (silently, on the Pico side, logged same as SET_CT_CAL) if
 * `channel` is out of range or a measurement is already in progress.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_CT_AUTO_ZERO_BEGIN_CMD 0x26u
#define KILNLINK_CT_AUTO_ZERO_NUM_CHANNELS 3u
#define KILNLINK_CT_AUTO_ZERO_BEGIN_LEN 2u /* cmd(1)+channel(1) */

typedef enum {
    KILNLINK_CT_AUTO_ZERO_BEGIN_OK = 0,
    KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_BUFFER_TOO_SMALL,
    KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_LENGTH_MISMATCH,
    KILNLINK_CT_AUTO_ZERO_BEGIN_ERR_WRONG_CMD,
} kilnlink_ct_auto_zero_begin_status_t;

typedef struct {
    uint8_t channel; /* 0..KILNLINK_CT_AUTO_ZERO_NUM_CHANNELS-1; receiver validates range */
} kilnlink_ct_auto_zero_begin_t;

size_t kilnlink_ct_auto_zero_begin_encode(const kilnlink_ct_auto_zero_begin_t *msg, uint8_t *out,
                                           size_t out_cap,
                                           kilnlink_ct_auto_zero_begin_status_t *status);

kilnlink_ct_auto_zero_begin_status_t kilnlink_ct_auto_zero_begin_decode(
    const uint8_t *payload, size_t len, kilnlink_ct_auto_zero_begin_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_CT_AUTO_ZERO_BEGIN_H */
