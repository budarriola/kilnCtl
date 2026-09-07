#ifndef KILNLINK_CT_AUTO_ZERO_STATUS_H
#define KILNLINK_CT_AUTO_ZERO_STATUS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pico -> ESP, SAFETY_CMD_CT_AUTO_ZERO_STATUS = 0x28 -- docs/LINK_PROTOCOL.md
 * sec 6. Sent in reply to SAFETY_CMD_GET_CT_AUTO_ZERO (0x27). Reports
 * current_task.c's own auto-zero accumulator state (CT_COMMISSIONING_
 * PLAN.md step 2) -- never blocks the Pico, since the accumulation itself
 * happens one sample per current_task's own normal period (see kilnlink_
 * ct_auto_zero_begin.h's header comment for why a blocking measurement on
 * this link is not safe).
 *
 * `state`: 0=IDLE (no BEGIN ever accepted, or a previous result already
 * consumed by a fresh BEGIN), 1=IN_PROGRESS, 2=DONE (zero_counts is the
 * mean of samples_taken raw ADC counts on `channel`; stays DONE, with the
 * same result, until the next BEGIN). `zero_counts`/`samples_taken` are
 * only meaningful when state != IDLE.
 *
 * Freestanding C11, no allocation, no I/O, no globals, every decoder
 * bounds-checked -- CommonFW/README.md rules 1-6. */

#define KILNLINK_CT_AUTO_ZERO_STATUS_CMD 0x28u
#define KILNLINK_CT_AUTO_ZERO_STATUS_LEN 9u /* cmd(1)+state(1)+channel(1)+samples_taken u16(2)+samples_target u16(2)+zero_counts u16(2) */

typedef enum {
    KILNLINK_CT_AUTO_ZERO_STATUS_OK = 0,
    KILNLINK_CT_AUTO_ZERO_STATUS_ERR_BUFFER_TOO_SMALL,
    KILNLINK_CT_AUTO_ZERO_STATUS_ERR_LENGTH_MISMATCH,
    KILNLINK_CT_AUTO_ZERO_STATUS_ERR_WRONG_CMD,
} kilnlink_ct_auto_zero_status_codec_t;

typedef enum {
    KILNLINK_CT_AUTO_ZERO_STATE_IDLE = 0,
    KILNLINK_CT_AUTO_ZERO_STATE_IN_PROGRESS = 1,
    KILNLINK_CT_AUTO_ZERO_STATE_DONE = 2,
} kilnlink_ct_auto_zero_state_t;

typedef struct {
    uint8_t  state;         /* kilnlink_ct_auto_zero_state_t */
    uint8_t  channel;
    uint16_t samples_taken;
    uint16_t samples_target;
    uint16_t zero_counts;   /* mean raw ADC counts, valid when state == DONE */
} kilnlink_ct_auto_zero_status_t;

size_t kilnlink_ct_auto_zero_status_encode(const kilnlink_ct_auto_zero_status_t *msg, uint8_t *out,
                                            size_t out_cap,
                                            kilnlink_ct_auto_zero_status_codec_t *status);

kilnlink_ct_auto_zero_status_codec_t kilnlink_ct_auto_zero_status_decode(
    const uint8_t *payload, size_t len, kilnlink_ct_auto_zero_status_t *out);

#ifdef __cplusplus
}
#endif

#endif /* KILNLINK_CT_AUTO_ZERO_STATUS_H */
