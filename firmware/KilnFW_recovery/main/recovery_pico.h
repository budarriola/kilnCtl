// recovery_pico.h -- the recovery image's RP2040 safety-processor update relay.
//
// The browser uploads the Pico slot image to POST /api/recovery/pico/upload;
// the handler receives it into a PSRAM buffer, validates it (size, vectors,
// CRC32) and hands it to a dedicated relay task that speaks the kilnlink
// UPDATE_* protocol to the Pico over UART1 (TX GPIO5, RX GPIO4, 230400 baud).
// The httpd task never touches the UART, so GET /api/recovery/pico/status
// keeps answering for the whole (minutes-long) transfer, and a stalled Pico can
// never block ESP recovery, the other routes or Exit Recovery.
//
// The image is buffered whole, not streamed to the Pico live, on purpose: the
// single httpd task cannot serve the status poll while it is inside a body
// receive, the Pico's flash erase dominates the transfer time anyway, and the
// CRC gate (recompute over the PSRAM copy, refuse on mismatch) needs the whole
// image before the first erase. See docs/RECOVERY_IMAGE_PLAN.md W4.
#ifndef RECOVERY_PICO_H
#define RECOVERY_PICO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RECOVERY_PICO_IDLE = 0,
    RECOVERY_PICO_RECEIVING, // body arriving over HTTP (buffer reserved)
    RECOVERY_PICO_DISCOVER,
    RECOVERY_PICO_BEGIN,
    RECOVERY_PICO_ERASING,
    RECOVERY_PICO_SENDING,
    RECOVERY_PICO_RETRANSMIT,
    RECOVERY_PICO_FINISHING,
    RECOVERY_PICO_DONE,
    RECOVERY_PICO_FAILED,
    RECOVERY_PICO_ABORTED,
} recovery_pico_phase_t;

const char *recovery_pico_phase_name(recovery_pico_phase_t p);

// True when this board has PSRAM the upload buffer can use.
bool recovery_pico_psram_available(void);

// Reserves the PSRAM buffer for a body of `len` bytes and marks the relay busy.
// Returns the buffer, or NULL with *why set to a static refusal text and
// *http_status to the status to send (413 too big, 503 busy/no PSRAM/low heap).
uint8_t *recovery_pico_reserve(size_t len, int *http_status, const char **why);
// Frees a reservation whose upload never reached recovery_pico_start().
void recovery_pico_release(void);
// Starts the relay task on the reserved buffer (already validated). `image_slot`
// is the slot the image is linked for; `operator_slot` the operator's asserted
// target (RPP_SLOT_A/B) or RPP_SLOT_UNKNOWN for auto. Frees the reservation and
// returns false (with *why) if the task cannot start.
bool recovery_pico_start(size_t len, uint32_t crc32, int image_slot, int operator_slot,
                         const char **why);
// Asks a running transfer to stop (it sends ABORT to the Pico). Safe any time.
void recovery_pico_abort(void);

// Builds the status JSON (also counts as the client's "still here" poll) into
// an internal PSRAM buffer and returns it (*len = bytes, excluding the NUL), or
// NULL if it cannot be built. The pointer is valid until the next call; only
// the httpd task calls this, so the caller needs no buffer of its own.
const char *recovery_pico_status_json(int *len);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_PICO_H
