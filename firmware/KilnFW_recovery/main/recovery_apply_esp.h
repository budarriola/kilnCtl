// recovery_apply_esp.h -- ESP-IDF wiring of recovery_apply.c: the `stage` and
// `app` partitions, a PSA SHA-256, esp_image_verify and the boot-partition
// switch, run from a dedicated task so the httpd task answers immediately and
// the web page polls GET /api/recovery/apply_status.
//
// Nothing here writes the `recovery` partition (owner decision D6: recovery is
// updated by JTAG only) and nothing needs a credential (the recovery image is
// unauthenticated; its only access control is its SoftAP passphrase).
#ifndef RECOVERY_APPLY_ESP_H
#define RECOVERY_APPLY_ESP_H

#include <stdbool.h>

#include "esp_err.h"
#include "esp_partition.h"
#include "recovery_apply.h"
#include "stage_header.h"

#ifdef __cplusplus
extern "C" {
#endif

// Starts the apply task. `app` is the ota_0 partition the image is copied into;
// `pre_boot` (may be NULL) runs once, after the copy verified and just before
// the boot partition is switched, and returns 0 on success -- the caller clears
// the boot_guard counter there, same order as /api/recovery/exit. On success the
// task restarts the chip itself when the apply finished.
// ESP_ERR_INVALID_STATE: an apply is already running (or finished and the
// restart is pending). ESP_ERR_NOT_FOUND: no `stage` partition.
// ESP_ERR_NO_MEM: task creation failed. Nothing is touched on any error return.
esp_err_t recovery_apply_esp_start(const esp_partition_t *app, int (*pre_boot)(void));

// True while an apply is running or its restart is pending: every other
// mutating route refuses with 409 meanwhile.
bool recovery_apply_busy(void);

// Snapshot of the last apply's progress (phase IDLE if none ran this boot).
void recovery_apply_esp_status(recovery_apply_progress_t *out);

// Decodes the stage header from flash. STAGE_HDR_OK fills *out; STAGE_HDR_BLANK
// means nothing is staged; any other value is a damaged header; STAGE_HDR_BAD_ARG
// is returned when there is no `stage` partition or the read failed.
stage_hdr_status_t recovery_apply_esp_stage_info(stage_header_t *out);

#ifdef __cplusplus
}
#endif

#endif // RECOVERY_APPLY_ESP_H
